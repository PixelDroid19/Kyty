#include "Emulator/Graphics/Graphics.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SUBSYSTEM_INIT(Graphics)
{
	auto width  = Config::GetScreenWidth();
	auto height = Config::GetScreenHeight();

	WindowInit(width, height);
	VideoOut::VideoOutInit(width, height);
	GraphicsRenderInit();
	GraphicsRunInit();
	GpuMemoryInit();
	LabelInit();
	TileInit();
	IndexBufferInit();
	ShaderInit();
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Graphics) {}

KYTY_SUBSYSTEM_DESTROY(Graphics) {}

void GraphicsDbgDumpDcb(const char* type, uint32_t num_dw, uint32_t* cmd_buffer)
{
	EXIT_IF(type == nullptr);

	static std::atomic_int id = 0;

	if (Config::CommandBufferDumpEnabled() && num_dw > 0 && cmd_buffer != nullptr)
	{
		Core::File f;
		String     file_name = Config::GetCommandBufferDumpFolder().FixDirectorySlash() +
		                   String::FromPrintf("%04d_%04d_buffer_%s.log", GraphicsRunGetFrameNum(), id++, type);
		Core::File::CreateDirectories(file_name.DirectoryWithoutFilename());
		f.Create(file_name);
		if (f.IsInvalid())
		{
			printf(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, file_name.C_Str());
			return;
		}
		Pm4::DumpPm4PacketStream(&f, cmd_buffer, 0, num_dw);
		f.Close();
	}
}

uint32_t GraphicsPm4PublishFenceProducers(const uint32_t* data, uint32_t num_dw)
{
	if (data == nullptr || num_dw == 0)
	{
		return 0;
	}

	uint32_t published = 0;
	uint32_t i         = 0;
	while (i < num_dw)
	{
		const uint32_t header   = data[i];
		const uint32_t pkt_type = header >> 30u;
		if (pkt_type != 3u)
		{
			if (pkt_type == 2u || header == 0u)
			{
				i += 1;
				continue;
			}
			if (pkt_type == 0u || pkt_type == 1u)
			{
				// Type0/Type1 single-body align unit (matches CommandProcessor::Run).
				i += (i + 1u < num_dw) ? 2u : 1u;
				continue;
			}
			break;
		}

		const uint32_t total_dw = KYTY_PM4_LEN(header);
		if (total_dw < 2u || i + total_dw > num_dw)
		{
			break;
		}

		const uint32_t op = (header >> 8u) & 0xffu;
		const uint32_t r  = KYTY_PM4_R(header);
		const uint32_t* body = data + i + 1u;
		const uint32_t  body_dw = total_dw - 1u;

		// Custom Gen5 ReleaseMem: IT_NOP + R_RELEASE_MEM, 7 or 8 dwords total.
		if (op == Pm4::IT_NOP && r == Pm4::R_RELEASE_MEM && body_dw >= 6u)
		{
			const uint32_t data_sel = (body[1] >> 16u) & 0xffu;
			auto*          dst =
			    reinterpret_cast<void*>(body[2] | (static_cast<uint64_t>(body[3]) << 32u));
			const uint64_t value = body[4] | (static_cast<uint64_t>(body[5]) << 32u);
			if (dst != nullptr)
			{
				if (data_sel == 1u)
				{
					*static_cast<uint32_t*>(dst) = static_cast<uint32_t>(value);
					published++;
				} else if (data_sel == 2u)
				{
					*static_cast<uint64_t*>(dst) = value;
					published++;
				}
			}
		}
		// Custom Gen5 WriteData: IT_NOP + R_WRITE_DATA.
		else if (op == Pm4::IT_NOP && r == Pm4::R_WRITE_DATA && body_dw >= 3u)
		{
			const uint32_t write_control = body[0];
			const uint32_t dst_sel       = write_control & 0xffu;
			auto*          dst =
			    reinterpret_cast<uint32_t*>(body[1] | (static_cast<uint64_t>(body[2]) << 32u));
			// Memory destinations observed on Gen5 builders: 1, 2, 4, 5.
			if (dst != nullptr && (dst_sel == 1u || dst_sel == 2u || dst_sel == 4u || dst_sel == 5u))
			{
				const uint32_t increment = (write_control >> 16u) & 0xffu;
				const uint32_t src_dwords = body_dw - 3u;
				for (uint32_t n = 0; n < src_dwords; n++)
				{
					const uint32_t out_index = (increment == 0u) ? n : 0u;
					dst[out_index]           = body[3u + n];
				}
				if (src_dwords > 0u)
				{
					published++;
				}
			}
		}
		// Standard IT_WRITE_DATA (non-custom).
		else if (op == Pm4::IT_WRITE_DATA && body_dw >= 3u)
		{
			auto*          dst =
			    reinterpret_cast<uint32_t*>(body[1] | (static_cast<uint64_t>(body[2]) << 32u));
			if (dst != nullptr)
			{
				const uint32_t src_dwords = body_dw - 3u;
				for (uint32_t n = 0; n < src_dwords; n++)
				{
					dst[n] = body[3u + n];
				}
				if (src_dwords > 0u)
				{
					published++;
				}
			}
		}

		i += total_dw;
	}

	return published;
}

namespace Gen4 {

LIB_NAME("GraphicsDriver", "GraphicsDriver");

int KYTY_SYSV_ABI GraphicsSetVsShader(uint32_t* cmd, uint64_t size, const uint32_t* vs_regs, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < sizeof(HW::VsStageRegisters) / 4 + 2);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);
	printf("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

	printf("\t m_spiShaderPgmLoVs    = %08" PRIx32 "\n", vs_regs[0]);
	printf("\t m_spiShaderPgmHiVs    = %08" PRIx32 "\n", vs_regs[1]);
	printf("\t m_spiShaderPgmRsrc1Vs = %08" PRIx32 "\n", vs_regs[2]);
	printf("\t m_spiShaderPgmRsrc2Vs = %08" PRIx32 "\n", vs_regs[3]);
	printf("\t m_spiVsOutConfig      = %08" PRIx32 "\n", vs_regs[4]);
	printf("\t m_spiShaderPosFormat  = %08" PRIx32 "\n", vs_regs[5]);
	printf("\t m_paClVsOutCntl       = %08" PRIx32 "\n", vs_regs[6]);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_VS);
	cmd[1] = shader_modifier;
	memcpy(&cmd[2], vs_regs, static_cast<size_t>(7) * 4);

	return OK;
}

int KYTY_SYSV_ABI GraphicsUpdateVsShader(uint32_t* cmd, uint64_t size, const uint32_t* vs_regs, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < sizeof(HW::VsStageRegisters) / 4 + 2);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);
	printf("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

	printf("\t m_spiShaderPgmLoVs    = %08" PRIx32 "\n", vs_regs[0]);
	printf("\t m_spiShaderPgmHiVs    = %08" PRIx32 "\n", vs_regs[1]);
	printf("\t m_spiShaderPgmRsrc1Vs = %08" PRIx32 "\n", vs_regs[2]);
	printf("\t m_spiShaderPgmRsrc2Vs = %08" PRIx32 "\n", vs_regs[3]);
	printf("\t m_spiVsOutConfig      = %08" PRIx32 "\n", vs_regs[4]);
	printf("\t m_spiShaderPosFormat  = %08" PRIx32 "\n", vs_regs[5]);
	printf("\t m_paClVsOutCntl       = %08" PRIx32 "\n", vs_regs[6]);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_VS_UPDATE);
	cmd[1] = shader_modifier;
	memcpy(&cmd[2], vs_regs, static_cast<size_t>(7) * 4);

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetEmbeddedVsShader(uint32_t* cmd, uint64_t size, uint32_t id, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 3);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);
	printf("\t id              = %" PRIu32 "\n", id);
	printf("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_VS_EMBEDDED);
	cmd[1] = shader_modifier;
	cmd[2] = id;

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetPsShader(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	if (ps_regs == nullptr)
	{
		EXIT_NOT_IMPLEMENTED(size < 1 + 1);

		printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
		printf("\t size            = %" PRIu64 "\n", size);
		printf("\t embedded_id     = %d\n", 0);

		cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS_EMBEDDED);
		cmd[1] = 0;
	} else
	{
		EXIT_NOT_IMPLEMENTED(size < 12 + 1);

		printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
		printf("\t size            = %" PRIu64 "\n", size);

		printf("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
		printf("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
		printf("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
		printf("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
		printf("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
		printf("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
		printf("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
		printf("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
		printf("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
		printf("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
		printf("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
		printf("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

		cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS);
		memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetPsShader350(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	if (ps_regs == nullptr)
	{
		EXIT_NOT_IMPLEMENTED(size < 1 + 1);

		printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
		printf("\t size            = %" PRIu64 "\n", size);
		printf("\t embedded_id     = %d\n", 0);

		cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS_EMBEDDED);
		cmd[1] = 0;
	} else
	{
		EXIT_NOT_IMPLEMENTED(size < 12 + 1);

		printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
		printf("\t size            = %" PRIu64 "\n", size);

		printf("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
		printf("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
		printf("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
		printf("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
		printf("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
		printf("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
		printf("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
		printf("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
		printf("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
		printf("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
		printf("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
		printf("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

		cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS);
		memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);
	}

	// printf("ok\n");

	return OK;
}

int KYTY_SYSV_ABI GraphicsUpdatePsShader(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(ps_regs == nullptr);
	EXIT_NOT_IMPLEMENTED(size < 12 + 1);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);

	printf("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
	printf("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
	printf("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
	printf("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
	printf("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
	printf("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
	printf("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
	printf("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
	printf("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
	printf("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
	printf("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
	printf("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS_UPDATE);
	memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);

	return OK;
}

int KYTY_SYSV_ABI GraphicsUpdatePsShader350(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(ps_regs == nullptr);
	EXIT_NOT_IMPLEMENTED(size < 12 + 1);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);

	printf("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
	printf("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
	printf("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
	printf("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
	printf("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
	printf("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
	printf("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
	printf("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
	printf("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
	printf("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
	printf("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
	printf("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS_UPDATE);
	memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetCsShaderWithModifier(uint32_t* cmd, uint64_t size, const uint32_t* cs_regs, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 7 + 2);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);
	printf("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

	printf("\t m_computePgmLo      = %08" PRIx32 "\n", cs_regs[0]);
	printf("\t m_computePgmHi      = %08" PRIx32 "\n", cs_regs[1]);
	printf("\t m_computePgmRsrc1   = %08" PRIx32 "\n", cs_regs[2]);
	printf("\t m_computePgmRsrc2   = %08" PRIx32 "\n", cs_regs[3]);
	printf("\t m_computeNumThreadX = %08" PRIx32 "\n", cs_regs[4]);
	printf("\t m_computeNumThreadY = %08" PRIx32 "\n", cs_regs[5]);
	printf("\t m_computeNumThreadZ = %08" PRIx32 "\n", cs_regs[6]);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_CS);
	cmd[1] = shader_modifier;
	memcpy(&cmd[2], cs_regs, static_cast<size_t>(7) * 4);

	return OK;
}

int KYTY_SYSV_ABI GraphicsDrawIndex(uint32_t* cmd, uint64_t size, uint32_t index_count, const void* index_addr, uint32_t flags,
                                    uint32_t type)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 6);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);
	printf("\t index_count = %" PRIu32 "\n", index_count);
	printf("\t index_addr  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
	printf("\t flags       = %08" PRIx32 "\n", flags);
	printf("\t type        = %" PRIu32 "\n", type);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_DRAW_INDEX);
	cmd[1] = index_count;
	cmd[2] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(index_addr) & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((reinterpret_cast<uint64_t>(index_addr) >> 32u) & 0xffffffffu);
	cmd[4] = flags;
	cmd[5] = type;

	return OK;
}

int KYTY_SYSV_ABI GraphicsDrawIndexAuto(uint32_t* cmd, uint64_t size, uint32_t index_count, uint32_t flags)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 3);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);
	printf("\t index_count = %" PRIu32 "\n", index_count);
	printf("\t flags       = %08" PRIx32 "\n", flags);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_DRAW_INDEX_AUTO);
	cmd[1] = index_count;
	cmd[2] = flags;

	return OK;
}

int KYTY_SYSV_ABI GraphicsInsertWaitFlipDone(uint32_t* cmd, uint64_t size, uint32_t video_out_handle, uint32_t display_buffer_index)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 3);

	printf("\t cmd_buffer           = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size                 = %" PRIu64 "\n", size);
	printf("\t video_out_handle     = %" PRIu32 "\n", video_out_handle);
	printf("\t display_buffer_index = %" PRIu32 "\n", display_buffer_index);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE);
	cmd[1] = video_out_handle;
	cmd[2] = display_buffer_index;

	return OK;
}

int KYTY_SYSV_ABI GraphicsDispatchDirect(uint32_t* cmd, uint64_t size, uint32_t thread_group_x, uint32_t thread_group_y,
                                         uint32_t thread_group_z, uint32_t mode)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 5);

	printf("\t cmd_buffer     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size           = %" PRIu64 "\n", size);
	printf("\t thread_group_x = %" PRIu32 "\n", thread_group_x);
	printf("\t thread_group_y = %" PRIu32 "\n", thread_group_y);
	printf("\t thread_group_z = %" PRIu32 "\n", thread_group_z);
	printf("\t mode           = %" PRIu32 "\n", mode);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_DISPATCH_DIRECT);
	cmd[1] = thread_group_x;
	cmd[2] = thread_group_y;
	cmd[3] = thread_group_z;
	cmd[4] = mode;

	return OK;
}

uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState175(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState200(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState350(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t KYTY_SYSV_ABI GraphicsDispatchInitDefaultHardwareState(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DISPATCH_RESET);

	return 2;
}

int KYTY_SYSV_ABI GraphicsSubmitCommandBuffers(uint32_t count, void* dcb_gpu_addrs[], const uint32_t* dcb_sizes_in_bytes,
                                               void* ccb_gpu_addrs[], const uint32_t* ccb_sizes_in_bytes)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(count != 1);

	auto* dcb      = (dcb_gpu_addrs == nullptr ? nullptr : static_cast<uint32_t*>(dcb_gpu_addrs[0]));
	auto  dcb_size = (dcb_sizes_in_bytes == nullptr ? 0 : dcb_sizes_in_bytes[0] / 4);
	auto* ccb      = (ccb_gpu_addrs == nullptr ? nullptr : static_cast<uint32_t*>(ccb_gpu_addrs[0]));
	auto  ccb_size = (ccb_sizes_in_bytes == nullptr ? 0 : ccb_sizes_in_bytes[0] / 4);

	GraphicsDbgDumpDcb("d", dcb_size, dcb);
	GraphicsDbgDumpDcb("c", ccb_size, ccb);

	GraphicsRunSubmit(dcb, dcb_size, ccb, ccb_size);

	return OK;
}

int KYTY_SYSV_ABI GraphicsSubmitAndFlipCommandBuffers(uint32_t count, void* dcb_gpu_addrs[], const uint32_t* dcb_sizes_in_bytes,
                                                      void* ccb_gpu_addrs[], const uint32_t* ccb_sizes_in_bytes, int handle, int index,
                                                      int flip_mode, int64_t flip_arg)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(count != 1);

	auto* dcb      = (dcb_gpu_addrs == nullptr ? nullptr : static_cast<uint32_t*>(dcb_gpu_addrs[0]));
	auto  dcb_size = (dcb_sizes_in_bytes == nullptr ? 0 : dcb_sizes_in_bytes[0] / 4);
	auto* ccb      = (ccb_gpu_addrs == nullptr ? nullptr : static_cast<uint32_t*>(ccb_gpu_addrs[0]));
	auto  ccb_size = (ccb_sizes_in_bytes == nullptr ? 0 : ccb_sizes_in_bytes[0] / 4);

	GraphicsDbgDumpDcb("d", dcb_size, dcb);
	GraphicsDbgDumpDcb("c", ccb_size, ccb);

	printf("\t handle    = %" PRId32 "\n", handle);
	printf("\t index     = %" PRId32 "\n", index);
	printf("\t flip_mode = %" PRId32 "\n", flip_mode);
	printf("\t flip_arg  = %" PRId64 "\n", flip_arg);

	GraphicsRunSubmitAndFlip(dcb, dcb_size, ccb, ccb_size, handle, index, flip_mode, flip_arg);

	return OK;
}

int KYTY_SYSV_ABI GraphicsSubmitDone()
{
	PRINT_NAME();

	GraphicsRunDone();

	return OK;
}

int KYTY_SYSV_ABI GraphicsAreSubmitsAllowed()
{
	return GraphicsRunAreSubmitsAllowed() ? 1 : 0;
}

void KYTY_SYSV_ABI GraphicsFlushMemory()
{
	PRINT_NAME();

	GpuMemoryFlushAll(WindowGetGraphicContext());
}

int KYTY_SYSV_ABI GraphicsAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata)
{
	PRINT_NAME();

	if (eq == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EBADF;
	}

	return GraphicsRenderAddEqEvent(eq, id, udata);
}

int KYTY_SYSV_ABI GraphicsDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id)
{
	PRINT_NAME();

	if (eq == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EBADF;
	}

	return GraphicsRenderDeleteEqEvent(eq, id);
}

uint32_t KYTY_SYSV_ABI GraphicsMapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw,
                                               uint32_t* read_ptr_addr)
{
	PRINT_NAME();

	printf("\t pipe_id       = %" PRIu32 "\n", pipe_id);
	printf("\t queue_id      = %" PRIu32 "\n", queue_id);
	printf("\t ring_addr     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(ring_addr));
	printf("\t ring_size_dw  = %" PRIu32 "\n", ring_size_dw);
	printf("\t read_ptr_addr = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(read_ptr_addr));

	uint32_t id = GraphicsRunMapComputeQueue(pipe_id, queue_id, ring_addr, ring_size_dw, read_ptr_addr);

	printf("\t queue         = %" PRIu32 "\n", id);

	return id;
}

void KYTY_SYSV_ABI GraphicsUnmapComputeQueue(uint32_t id)
{
	PRINT_NAME();

	printf("\t id = %" PRIu32 "\n", id);

	GraphicsRunUnmapComputeQueue(id);
}

int KYTY_SYSV_ABI GraphicsComputeWaitOnAddress(uint32_t* cmd, uint64_t size, uint32_t* gpu_addr, uint32_t mask, uint32_t func, uint32_t ref)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 6);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);
	printf("\t gpu_addr    = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(gpu_addr));
	printf("\t mask        = %08" PRIx32 "\n", mask);
	printf("\t func        = %" PRIu32 "\n", func);
	printf("\t ref         = %08" PRIx32 "\n", ref);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_WAIT_MEM_32);
	cmd[1] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(gpu_addr) & 0xffffffffu);
	cmd[2] = static_cast<uint32_t>((reinterpret_cast<uint64_t>(gpu_addr) >> 32u) & 0xffffffffu);
	cmd[3] = mask;
	cmd[4] = func;
	cmd[5] = ref;

	return OK;
}

void KYTY_SYSV_ABI GraphicsDingDong(uint32_t ring_id, uint32_t offset_dw)
{
	PRINT_NAME();

	printf("\t ring_id   = %" PRIu32 "\n", ring_id);
	printf("\t offset_dw = %" PRIu32 "\n", offset_dw);

	GraphicsRunDingDong(ring_id, offset_dw);
}

int KYTY_SYSV_ABI GraphicsInsertPushMarker(uint32_t* cmd, uint64_t size, const char* str)
{
	PRINT_NAME();

	auto len = strlen(str) + 1;

	EXIT_NOT_IMPLEMENTED(size * 4 < len + 1);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);
	printf("\t str         = %s\n", str);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PUSH_MARKER);

	memcpy(cmd + 1, str, len);

	return OK;
}

int KYTY_SYSV_ABI GraphicsInsertPopMarker(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_POP_MARKER);

	return OK;
}

uint64_t KYTY_SYSV_ABI GraphicsGetGpuCoreClockFrequency()
{
	return LibKernel::KernelGetTscFrequency();
}

int KYTY_SYSV_ABI GraphicsIsUserPaEnabled()
{
	return 0;
}

void* KYTY_SYSV_ABI GraphicsGetTheTessellationFactorRingBufferBaseAddress()
{
	PRINT_NAME();

	auto addr = Core::VirtualMemory::AllocAligned(0, 0x20000, Core::VirtualMemory::Mode::ReadWrite, 256);
	Core::VirtualMemory::Free(addr);
	bool again = Core::VirtualMemory::AllocFixed(addr, 0x20000, Core::VirtualMemory::Mode::ReadWrite);
	EXIT_NOT_IMPLEMENTED(!again);
	Core::VirtualMemory::Free(addr);

	printf("\t addr = %016" PRIx64 "\n", addr);

	return reinterpret_cast<void*>(addr);
}

int KYTY_SYSV_ABI GraphicsRegisterOwner(uint32_t* owner_handle, const char* name)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(owner_handle == nullptr);
	EXIT_NOT_IMPLEMENTED(name == nullptr);

	printf("\t RegisterOwner: %s\n", name);

	GpuMemoryRegisterOwner(owner_handle, name);

	printf("\t handler: %" PRIu32 "\n", *owner_handle);

	return OK;
}

int KYTY_SYSV_ABI GraphicsRegisterResource(uint32_t* resource_handle, uint32_t owner_handle, const void* memory, size_t size,
                                           const char* name, uint32_t type, uint64_t user_data)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(resource_handle == nullptr);
	EXIT_NOT_IMPLEMENTED(memory == nullptr);
	EXIT_NOT_IMPLEMENTED(name == nullptr);

	printf("\t RegisterResource: %s\n", name);
	printf("\t owner_handle:     %" PRIu32 "\n", owner_handle);
	printf("\t addr:             %016" PRIx64 "\n", reinterpret_cast<uint64_t>(memory));
	printf("\t size:             %" PRIu64 "\n", size);
	printf("\t type:             %" PRIu32 "\n", type);
	printf("\t user_data:        %" PRIu64 "\n", user_data);

	uint32_t rhandle = 0;

	GpuMemoryRegisterResource(&rhandle, owner_handle, memory, size, name, type, user_data);

	printf("\t handler: %" PRIu32 "\n", rhandle);

	if (resource_handle != nullptr)
	{
		*resource_handle = rhandle;
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsUnregisterAllResourcesForOwner(uint32_t owner_handle)
{
	PRINT_NAME();

	printf("\t owner_handle:     %" PRIu32 "\n", owner_handle);

	GpuMemoryUnregisterAllResourcesForOwner(owner_handle);

	return OK;
}

int KYTY_SYSV_ABI GraphicsUnregisterOwnerAndResources(uint32_t owner_handle)
{
	PRINT_NAME();

	printf("\t owner_handle:     %" PRIu32 "\n", owner_handle);

	GpuMemoryUnregisterOwnerAndResources(owner_handle);

	return OK;
}

int KYTY_SYSV_ABI GraphicsUnregisterResource(uint32_t resource_handle)
{
	PRINT_NAME();

	printf("\t resource_handle:     %" PRIu32 "\n", resource_handle);

	GpuMemoryUnregisterResource(resource_handle);

	return OK;
}

} // namespace Gen4

namespace Gen5 {

LIB_NAME("Graphics5", "Graphics5");

struct RegisterDefaultInfo
{
	uint32_t       type;
	ShaderRegister reg[16];
};

struct RegisterDefaults
{
	ShaderRegister** tbl0       = nullptr;
	ShaderRegister** tbl1       = nullptr;
	ShaderRegister** tbl2       = nullptr;
	ShaderRegister** tbl3       = nullptr;
	uint64_t         unknown[2] = {};
	uint32_t*        types      = nullptr;
	uint32_t         count      = 0;
};

struct CommandBuffer
{
	using Callback = KYTY_SYSV_ABI bool (*)(CommandBuffer*, uint32_t, void*);

	uint32_t* bottom;
	uint32_t* top;
	uint32_t* cursor_up;
	uint32_t* cursor_down;
	Callback  callback;
	void*     user_data;
	uint32_t  reserved_dw;

	void DbgDump() const
	{
		printf("\t bottom      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(bottom));
		printf("\t top         = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(top));
		printf("\t cursor_up   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cursor_up));
		printf("\t cursor_down = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cursor_down));
		printf("\t callback    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(callback));
		printf("\t user_data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_data));
		printf("\t reserved_dw = %" PRIu32 "\n", reserved_dw);
	}

	[[nodiscard]] KYTY_SYSV_ABI uint32_t GetAvailableSizeDW() const
	{
		auto available = static_cast<uint32_t>(cursor_down - cursor_up);
		return std::max(available, reserved_dw) - reserved_dw;
	}

	KYTY_SYSV_ABI bool ReserveDW(uint32_t num_dw)
	{
		uint32_t remaining = GetAvailableSizeDW();
		if (num_dw > remaining)
		{
			bool result = callback(this, num_dw + reserved_dw, user_data);
			if (!result)
			{
				return false;
			}
			EXIT_NOT_IMPLEMENTED(!(GetAvailableSizeDW() >= num_dw));
		}
		return true;
	}

	KYTY_SYSV_ABI uint32_t* AllocateDW(uint32_t size_dw)
	{
		if (size_dw == 0 || !ReserveDW(size_dw))
		{
			return nullptr;
		}
		auto* ret_ptr = cursor_up;
		cursor_up += size_dw;
		return ret_ptr;
	}
};

struct Label
{
	volatile uint64_t m_value;
	uint64_t          m_reserved[3];
};

static bool copy_and_sort_sh_registers(const ShaderRegister* regs, uint32_t num_regs, Vector<ShaderRegister>* sorted)
{
	if (regs == nullptr || sorted == nullptr || num_regs == 0 || num_regs > 4096)
	{
		return false;
	}

	for (uint32_t i = 0; i < num_regs; i++)
	{
		if (regs[i].offset >= Pm4::SH_NUM)
		{
			return false;
		}
		sorted->Add(regs[i]);
	}

	sorted->Sort([](const ShaderRegister& left, const ShaderRegister& right) { return left.offset < right.offset; });
	return true;
}

uint32_t GraphicsGetShRegistersPacketSize(const ShaderRegister* regs, uint32_t num_regs)
{
	Vector<ShaderRegister> sorted;
	if (!copy_and_sort_sh_registers(regs, num_regs, &sorted))
	{
		return 0;
	}

	uint32_t groups = 1;
	for (uint32_t i = 1; i < sorted.Size(); i++)
	{
		if (sorted[i].offset != sorted[i - 1].offset + 1)
		{
			groups++;
		}
	}
	return num_regs + groups * 2;
}

uint32_t GraphicsEncodeShRegisters(uint32_t* cmd, uint32_t capacity_dw, const ShaderRegister* regs, uint32_t num_regs)
{
	Vector<ShaderRegister> sorted;
	if (!copy_and_sort_sh_registers(regs, num_regs, &sorted))
	{
		return 0;
	}

	uint32_t required_dw = num_regs + 2;
	for (uint32_t i = 1; i < sorted.Size(); i++)
	{
		if (sorted[i].offset != sorted[i - 1].offset + 1)
		{
			required_dw += 2;
		}
	}
	if (cmd == nullptr || capacity_dw < required_dw)
	{
		return 0;
	}

	uint32_t output = 0;
	uint32_t begin  = 0;
	while (begin < sorted.Size())
	{
		uint32_t end = begin + 1;
		while (end < sorted.Size() && sorted[end].offset == sorted[end - 1].offset + 1)
		{
			end++;
		}

		const uint32_t value_count = end - begin;
		cmd[output++]              = KYTY_PM4(value_count + 2, Pm4::IT_SET_SH_REG, 0);
		cmd[output++]              = sorted[begin].offset;
		for (uint32_t i = begin; i < end; i++)
		{
			cmd[output++] = sorted[i].value;
		}
		begin = end;
	}

	return output;
}

uint32_t GraphicsEncodeDispatch(uint32_t* cmd, uint32_t capacity_dw, uint32_t group_x, uint32_t group_y, uint32_t group_z,
                                uint32_t modifier)
{
	if (cmd == nullptr || capacity_dw < 5)
	{
		return 0;
	}

	cmd[0] = KYTY_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0);
	cmd[1] = group_x;
	cmd[2] = group_y;
	cmd[3] = group_z;
	cmd[4] = (modifier & 0xA038u) | 0x41u;
	return 5;
}

static RegisterDefaultInfo g_cx_reg_info1[] = {
    /* 0 */ {0xE24F806D, {{Pm4::CB_COLOR_CONTROL, 0x00cc0010}}},
    /* 1 */ {0xF6C28182, {{Pm4::CB_DCC_CONTROL, 0x00000000}}},
    /* 2 */ {0x6F6E55A5, {{Pm4::CB_RMI_GL2_CACHE_CONTROL, 0x00000000}}},
    /* 3 */ {0x0BC65DA4, {{Pm4::CB_SHADER_MASK, 0x00000000}}},
    /* 4 */ {0x9E5AD592, {{Pm4::CB_TARGET_MASK, 0x00000000}}},
    /* 5 */ {0xBB513B98, {{Pm4::DB_ALPHA_TO_MASK, 0x0000aa00}}},
    /* 6 */ {0xAB64B23B, {{Pm4::DB_COUNT_CONTROL, 0x00000000}}},
    /* 7 */ {0x53C39964, {{Pm4::DB_DEPTH_CONTROL, 0x00000000}}},
    /* 8 */ {0x01396B11, {{Pm4::DB_EQAA, 0x00000000}}},
    /* 9 */ {0x7D42019A, {{Pm4::DB_RENDER_CONTROL, 0x00000000}}},
    /* 10 */ {0x3548F523, {{Pm4::PS_SHADER_SAMPLE_EXCLUSION_MASK, 0x00000000}}},
    /* 11 */ {0xF43AD28A, {{Pm4::DB_RMI_L2_CACHE_CONTROL, 0x00000000}}},
    /* 12 */ {0x6DE4C312, {{Pm4::DB_SHADER_CONTROL, 0x00000000}}},
    /* 13 */ {0x00A77AE0, {{Pm4::DB_SRESULTS_COMPARE_STATE0, 0x00000000}}},
    /* 14 */ {0x00A779B7, {{Pm4::DB_SRESULTS_COMPARE_STATE1, 0x00000000}}},
    /* 15 */ {0x5100100C, {{Pm4::DB_STENCILREFMASK, 0x00000000}}},
    /* 16 */ {0x59958BBA, {{Pm4::DB_STENCILREFMASK_BF, 0x00000000}}},
    /* 17 */ {0x0C06F17C, {{Pm4::DB_STENCIL_CONTROL, 0x00000000}}},
    /* 18 */ {0x6F104B72, {{Pm4::GE_MAX_OUTPUT_PER_SUBGROUP, 0x00000000}}},
    /* 19 */ {0x25C70D9C, {{Pm4::PA_CL_CLIP_CNTL, 0x00000000}}},
    /* 20 */ {0x3881201E, {{Pm4::PA_CL_OBJPRIM_ID_CNTL, 0x00000000}}},
    /* 21 */ {0x09AFDDAF, {{Pm4::PA_CL_VTE_CNTL, 0x0000043f}}},
    /* 22 */ {0x367D63CF, {{Pm4::PA_SC_AA_CONFIG, 0x00000000}}},
    /* 23 */ {0x43707DB8, {{Pm4::PA_SC_CLIPRECT_RULE, 0x0000ffff}}},
    /* 24 */ {0xF6AE26BA, {{Pm4::PA_SC_CONSERVATIVE_RASTERIZATION_CNTL, 0x00000000}}},
    /* 25 */ {0x1B917652, {{Pm4::PA_SC_FSR_ENABLE, 0x00000000}}},
    /* 26 */ {0x94B1E4F7, {{Pm4::PA_SC_HORIZ_GRID, 0x00000000}}},
    /* 27 */ {0xE3661B6C, {{Pm4::PA_SC_LEFT_VERT_GRID, 0x00000000}}},
    /* 28 */ {0x1EB8D73A, {{Pm4::PA_SC_MODE_CNTL_0, 0x00000002}}},
    /* 29 */ {0x15051FA3, {{Pm4::PA_SC_MODE_CNTL_1, 0x00000000}}},
    /* 30 */ {0x9C51A7F1, {{Pm4::PA_SC_RIGHT_VERT_GRID, 0x00000000}}},
    /* 31 */ {0xA20EFC70, {{Pm4::PA_SC_WINDOW_OFFSET, 0x00000000}}},
    /* 32 */ {0x0EC09F6E, {{Pm4::PA_STATE_STEREO_X, 0x00000000}}},
    /* 33 */ {0x34A7D6D3, {{Pm4::PA_STEREO_CNTL, 0x00000000}}},
    /* 34 */ {0xCE831B94, {{Pm4::PA_SU_HARDWARE_SCREEN_OFFSET, 0x00000000}}},
    /* 35 */ {0x5CC72A74, {{Pm4::PA_SU_LINE_CNTL, 0x00000008}}},
    /* 36 */ {0x3B77713C, {{Pm4::PA_SU_POINT_MINMAX, 0xffff0000}}},
    /* 37 */ {0x40F64410, {{Pm4::PA_SU_POINT_SIZE, 0x00080008}}},
    /* 38 */ {0x69441268, {{Pm4::PA_SU_POLY_OFFSET_CLAMP, 0x00000000}}},
    /* 39 */ {0x2E418B83, {{Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL, 0x000001e9}}},
    /* 40 */ {0xA00D0C8D, {{Pm4::PA_SU_SC_MODE_CNTL, 0x00000240}}},
    /* 41 */ {0xB1289FB3, {{Pm4::PA_SU_SMALL_PRIM_FILTER_CNTL, 0x00000001}}},
    /* 42 */ {0x144832FB, {{Pm4::PA_SU_VTX_CNTL, 0x0000002d}}},
    /* 43 */ {0x9890D9FA, {{Pm4::SPI_TMPRING_SIZE, 0x00000000}}},
    /* 44 */ {0x9016FAF1, {{Pm4::VGT_DRAW_PAYLOAD_CNTL, 0x00000000}}},
    /* 45 */ {0x4B73CE27, {{Pm4::VGT_GS_MAX_VERT_OUT, 0x00000400}}},
    /* 46 */ {0x5F5A3E7B, {{Pm4::VGT_GS_OUT_PRIM_TYPE, 0x00000002}}},
    /* 47 */ {0xD4AF3A51, {{Pm4::VGT_LS_HS_CONFIG, 0x00000000}}},
    /* 48 */ {0x6CF4F543, {{Pm4::VGT_PRIMITIVEID_RESET, 0xffffffff}}},
    /* 49 */ {0x5FB86CCB, {{Pm4::VGT_PRIMITIVEID_EN, 0x00000000}}},
    /* 50 */ {0xEDEFA188, {{Pm4::VGT_REUSE_OFF, 0x00000000}}},
    /* 51 */ {0xD0DE9EE6, {{Pm4::VGT_SHADER_STAGES_EN, 0x00000000}}},
    /* 52 */ {0xC5831803, {{Pm4::VGT_TESS_DISTRIBUTION, 0x88101000}}},
    /* 53 */ {0x8E6DE84B, {{Pm4::VGT_TF_PARAM, 0x00000000}}},
    /* 54 */
    {0xD0771662,
     {
         {Pm4::PA_SC_CENTROID_PRIORITY_0, 0x00000000},
         {Pm4::PA_SC_CENTROID_PRIORITY_1, 0x00000000},
     }},
    /* 55 */ {0x569F7444, {{Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, 0x00000000}}},
    /* 56 */
    {0x5C6637CD,
     {
         {Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 0xffffffff},
         {Pm4::PA_SC_AA_MASK_X0Y1_X1Y1, 0xffffffff},
     }},
    /* 57 */
    {0xCAE3E690,
     {
         {Pm4::PA_SC_BINNER_CNTL_0, 0x00000002},
         {Pm4::PA_SC_BINNER_CNTL_1, 0x03ff0080},
     }},
    /* 58 */
    {0x43FBD769,
     {
         {Pm4::CB_BLEND_RED, 0x00000000},
         {Pm4::CB_BLEND_BLUE, 0x00000000},
         {Pm4::CB_BLEND_GREEN, 0x00000000},
         {Pm4::CB_BLEND_ALPHA, 0x00000000},
     }},
    /* 59 */ {0xEF550356, {{Pm4::CB_BLEND0_CONTROL, 0x20010001}}},
    /* 60 */
    {0x8F52E279,
     {
         {Pm4::TA_BC_BASE_ADDR, 0x00000000},
         {Pm4::TA_BC_BASE_ADDR_HI, 0x00000000},
     }},
    /* 61 */
    {0x1F2D8149,
     {
         {Pm4::PA_SC_CLIPRECT_0_TL, 0x00000000},
         {Pm4::PA_SC_CLIPRECT_0_BR, 0x20002000},
     }},
    /* 62 */ {0x853D0614, {{Pm4::CX_NOP, 0x00000000}}},
    /* 63 */
    {0x4413C6F9,
     {
         {Pm4::DB_DEPTH_BOUNDS_MIN, 0x00000000},
         {Pm4::DB_DEPTH_BOUNDS_MAX, 0x00000000},
     }},
    /* 64 */
    {0x67096014,
     {
         {Pm4::DB_Z_INFO, 0x80000000},
         {Pm4::DB_STENCIL_INFO, 0x20000000},
         {Pm4::DB_Z_READ_BASE, 0x00000000},
         {Pm4::DB_STENCIL_READ_BASE, 0x00000000},
         {Pm4::DB_Z_WRITE_BASE, 0x00000000},
         {Pm4::DB_STENCIL_WRITE_BASE, 0x00000000},
         {Pm4::DB_Z_READ_BASE_HI, 0x00000000},
         {Pm4::DB_STENCIL_READ_BASE_HI, 0x00000000},
         {Pm4::DB_Z_WRITE_BASE_HI, 0x00000000},
         {Pm4::DB_STENCIL_WRITE_BASE_HI, 0x00000000},
         {Pm4::DB_HTILE_DATA_BASE_HI, 0x00000000},
         {Pm4::DB_DEPTH_VIEW, 0x00000000},
         {Pm4::DB_HTILE_DATA_BASE, 0x00000000},
         {Pm4::DB_DEPTH_SIZE_XY, 0x00000000},
         {Pm4::DB_DEPTH_CLEAR, 0x00000000},
         {Pm4::DB_STENCIL_CLEAR, 0x00000000},
     }},
    /* 65 */
    {0x88F5E915,
     {
         {Pm4::PA_SC_FOV_WINDOW_LR, 0xff00ff00},
         {Pm4::PA_SC_FOV_WINDOW_TB, 0x00000000},
     }},
    /* 66 */
    {0x033F1EFF,
     {
         {Pm4::FSR_RECURSIONS0, 0x00000000},
         {Pm4::FSR_RECURSIONS1, 0x00000000},
     }},
    /* 67 */
    {0x918106BB,
     {
         {Pm4::PA_SC_GENERIC_SCISSOR_TL, 0x80000000},
         {Pm4::PA_SC_GENERIC_SCISSOR_BR, 0x40004000},
     }},
    /* 68 */
    {0x95F0E7AC,
     {
         {Pm4::PA_CL_GB_VERT_CLIP_ADJ, 0x4e7e0000},
         {Pm4::PA_CL_GB_VERT_DISC_ADJ, 0x4e7e0000},
         {Pm4::PA_CL_GB_HORZ_CLIP_ADJ, 0x4e7e0000},
         {Pm4::PA_CL_GB_HORZ_DISC_ADJ, 0x4e7e0000},
     }},
    /* 69 */
    {0xB48CBAB2,
     {
         {Pm4::PA_SU_POLY_OFFSET_BACK_SCALE, 0x00000000},
         {Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET, 0x00000000},
     }},
    /* 70 */
    {0x05BB3BC6,
     {
         {Pm4::PA_SU_POLY_OFFSET_FRONT_SCALE, 0x00000000},
         {Pm4::PA_SU_POLY_OFFSET_FRONT_OFFSET, 0x00000000},
     }},
    /* 71 */
    {0x94FABA07,
     {
         {Pm4::DB_RENDER_OVERRIDE, 0x00000000},
         {Pm4::DB_RENDER_OVERRIDE2, 0x00000000},
     }},
    /* 72 */
    {0x38E92C91,
     {
         {Pm4::CB_COLOR0_BASE, 0x00000000},
         {Pm4::CB_COLOR0_VIEW, 0x00000000},
         {Pm4::CB_COLOR0_INFO, 0x00000000},
         {Pm4::CB_COLOR0_ATTRIB, 0x00000000},
         {Pm4::CB_COLOR0_DCC_CONTROL, 0x00000048},
         {Pm4::CB_COLOR0_CMASK, 0x00000000},
         {Pm4::CB_COLOR0_FMASK, 0x00000000},
         {Pm4::CB_COLOR0_CLEAR_WORD0, 0x00000000},
         {Pm4::CB_COLOR0_CLEAR_WORD1, 0x00000000},
         {Pm4::CB_COLOR0_DCC_BASE, 0x00000000},
         {Pm4::CB_COLOR0_BASE_EXT, 0x00000000},
         {Pm4::CB_COLOR0_CMASK_BASE_EXT, 0x00000000},
         {Pm4::CB_COLOR0_FMASK_BASE_EXT, 0x00000000},
         {Pm4::CB_COLOR0_DCC_BASE_EXT, 0x00000000},
         {Pm4::CB_COLOR0_ATTRIB2, 0x00000000},
         {Pm4::CB_COLOR0_ATTRIB3, 0x0006c000},
     }},
    /* 73 */
    {0x0B177B43,
     {
         {Pm4::PA_SC_SCREEN_SCISSOR_TL, 0x00000000},
         {Pm4::PA_SC_SCREEN_SCISSOR_BR, 0x40004000},
     }},
    /* 74 */ {0x48531062, {{Pm4::SPI_PS_INPUT_CNTL_0, 0x00000000}}},
    /* 75 */
    {0xAAA964B9,
     {
         {Pm4::PA_CL_UCP_0_X, 0x00000000},
         {Pm4::PA_CL_UCP_0_Y, 0x00000000},
         {Pm4::PA_CL_UCP_0_Z, 0x00000000},
         {Pm4::PA_CL_UCP_0_W, 0x00000000},
     }},
    /* 76 */
    {0x7690AF6F,
     {
         {Pm4::PA_CL_VPORT_XSCALE, 0x4e7e0000},
         {Pm4::PA_CL_VPORT_YSCALE, 0x4e7e0000},
         {Pm4::PA_CL_VPORT_ZSCALE, 0x4e7e0000},
         {Pm4::PA_CL_VPORT_XOFFSET, 0x00000000},
         {Pm4::PA_CL_VPORT_YOFFSET, 0x00000000},
         {Pm4::PA_CL_VPORT_ZOFFSET, 0x00000000},
         {Pm4::PA_SC_VPORT_SCISSOR_0_TL, 0x80000000},
         {Pm4::PA_SC_VPORT_SCISSOR_0_BR, 0x40004000},
         {Pm4::PA_SC_VPORT_ZMIN_0, 0x00000000},
         {Pm4::PA_SC_VPORT_ZMAX_0, 0x00000000},
     }},
    /* 77 */
    {0x078D7060,
     {
         {Pm4::PA_SC_WINDOW_SCISSOR_TL, 0x80000000},
         {Pm4::PA_SC_WINDOW_SCISSOR_BR, 0x40004000},
     }},

};

static RegisterDefaultInfo g_sh_reg_info1[] = {
    /* 0 */ {0x5D6E3EC7, {{Pm4::COMPUTE_PGM_RSRC1, 0x00000000}}},
    /* 1 */ {0x57E7079A, {{Pm4::COMPUTE_PGM_RSRC2, 0x00000000}}},
    /* 2 */ {0x7467FAFD, {{Pm4::COMPUTE_PGM_RSRC3, 0x00000000}}},
    /* 3 */ {0x9E826B50, {{Pm4::COMPUTE_RESOURCE_LIMITS, 0x00000000}}},
    /* 4 */ {0xDC484F18, {{Pm4::COMPUTE_TMPRING_SIZE, 0x00000000}}},
    /* 5 */ {0x5DA8BCA3, {{Pm4::SPI_SHADER_PGM_RSRC1_GS, 0x00000000}}},
    /* 6 */ {0x5CA726D8, {{Pm4::SPI_SHADER_PGM_RSRC1_HS, 0x00000000}}},
    /* 7 */ {0x5DD28360, {{Pm4::SPI_SHADER_PGM_RSRC1_PS, 0x00000000}}},
    /* 8 */ {0x57EFA0BE, {{Pm4::SPI_SHADER_PGM_RSRC2_GS, 0x00000000}}},
    /* 9 */ {0x502363D5, {{Pm4::SPI_SHADER_PGM_RSRC2_HS, 0x00000000}}},
    /* 10 */ {0x506D14BD, {{Pm4::SPI_SHADER_PGM_RSRC2_PS, 0x00000000}}},
    /* 11 */ {0xB2609506, {{Pm4::COMPUTE_USER_ACCUM_0, 0x00000000}}},
    /* 12 */
    {0x9E5CFB8A,
     {
         {Pm4::SPI_SHADER_PGM_RSRC3_HS, 0x00000000},
         {Pm4::SPI_SHADER_PGM_RSRC3_GS, 0x00000000},
         {Pm4::SPI_SHADER_PGM_RSRC3_PS, 0x00000000},
     }},
    /* 13 */
    {0xC918DF3E,
     {
         {Pm4::COMPUTE_PGM_LO, 0x00000000},
         {Pm4::COMPUTE_PGM_HI, 0x00000000},
     }},
    /* 14 */
    {0xC9751C9C,
     {
         {Pm4::SPI_SHADER_PGM_LO_ES, 0x00000000},
         {Pm4::SPI_SHADER_PGM_HI_ES, 0x00000000},
     }},
    /* 15 */
    {0xC97EF77A,
     {
         {Pm4::SPI_SHADER_PGM_LO_GS, 0x00000000},
         {Pm4::SPI_SHADER_PGM_HI_GS, 0x00000000},
     }},
    /* 16 */
    {0xC927C6B9,
     {
         {Pm4::SPI_SHADER_PGM_LO_HS, 0x00000000},
         {Pm4::SPI_SHADER_PGM_HI_HS, 0x00000000},
     }},
    /* 17 */
    {0xC92A1EC5,
     {
         {Pm4::SPI_SHADER_PGM_LO_LS, 0x00000000},
         {Pm4::SPI_SHADER_PGM_HI_LS, 0x00000000},
     }},
    /* 18 */
    {0xC9E01B31,
     {
         {Pm4::SPI_SHADER_PGM_LO_PS, 0x00000000},
         {Pm4::SPI_SHADER_PGM_HI_PS, 0x00000000},
     }},
    /* 19 */ {0x50685F29, {{Pm4::SH_NOP, 0x00000000}}},
    /* 20 */ {0xB26219CA, {{Pm4::SPI_SHADER_USER_ACCUM_ESGS_0, 0x00000000}}},
    /* 21 */ {0xB25B6CF9, {{Pm4::SPI_SHADER_USER_ACCUM_LSHS_0, 0x00000000}}},
    /* 22 */ {0xB2F86101, {{Pm4::SPI_SHADER_USER_ACCUM_PS_0, 0x00000000}}},
    /* 23 */
    {0x07E3B155,
     {
         {Pm4::SPI_SHADER_USER_DATA_ADDR_LO_GS, 0x00000000},
         {Pm4::SPI_SHADER_USER_DATA_ADDR_HI_GS, 0x00000000},
     }},
    /* 24 */
    {0x07E383C6,
     {
         {Pm4::SPI_SHADER_USER_DATA_ADDR_LO_HS, 0x00000000},
         {Pm4::SPI_SHADER_USER_DATA_ADDR_HI_HS, 0x00000000},
     }},
    /* 25 */ {0xBDA98653, {{Pm4::COMPUTE_USER_DATA_0, 0x00000000}}},
    /* 26 */ {0xBDBD1D0F, {{Pm4::SPI_SHADER_USER_DATA_GS_0, 0x00000000}}},
    /* 27 */ {0xBD946FD4, {{Pm4::SPI_SHADER_USER_DATA_HS_0, 0x00000000}}},
    /* 28 */ {0xBDF02A4C, {{Pm4::SPI_SHADER_USER_DATA_PS_0, 0x00000000}}},
};

static RegisterDefaultInfo g_uc_reg_info1[] = {
    /* 0 */ {0x19E93E85, {{Pm4::GDS_OA_ADDRESS, 0x00000000}}},
    /* 1 */ {0x3B5C2AF3, {{Pm4::GDS_OA_CNTL, 0x00000000}}},
    /* 2 */ {0x47974A35, {{Pm4::GDS_OA_COUNTER, 0x00000000}}},
    /* 3 */ {0x105971C2, {{Pm4::GE_CNTL, 0x00000000}}},
    /* 4 */ {0x7D137765, {{Pm4::GE_INDX_OFFSET, 0x00000000}}},
    /* 5 */ {0xD187FEBC, {{Pm4::GE_MULTI_PRIM_IB_RESET_EN, 0x00000000}}},
    /* 6 */ {0x12F854AC, {{Pm4::GE_STEREO_CNTL, 0x00000000}}},
    /* 7 */ {0x40D49AD1, {{Pm4::GE_USER_VGPR_EN, 0x00000000}}},
    /* 8 */ {0x8C0923DA, {{Pm4::FSR_EXTEND_SUBPIXEL_ROUNDING, 0x00000000}}},
    /* 9 */ {0xBB8DF494, {{Pm4::TEXTURE_GRADIENT_CONTROL, 0x00000000}}},
    /* 10 */ {0xF6D8A76E, {{Pm4::TEXTURE_GRADIENT_FACTORS, 0x40000040}}},
    /* 11 */ {0x7620F1E9, {{Pm4::VGT_OBJECT_ID, 0x00000000}}},
    /* 12 */ {0x9EBFAB10, {{Pm4::VGT_PRIMITIVE_TYPE, 0x00000000}}},
    /* 13 */
    {0x98A09D0E,
     {
         {Pm4::TA_CS_BC_BASE_ADDR, 0x00000000},
         {Pm4::TA_CS_BC_BASE_ADDR_HI, 0x00000000},
     }},
    /* 14 */
    {0x195D37D2,
     {
         {Pm4::FSR_ALPHA_VALUE0, 0x00000000},
         {Pm4::FSR_ALPHA_VALUE1, 0x00000000},
     }},
    /* 15 */
    {0xF9EC4F85,
     {
         {Pm4::FSR_CONTROL_POINT0, 0x00000000},
         {Pm4::FSR_CONTROL_POINT1, 0x00000000},
         {Pm4::FSR_CONTROL_POINT2, 0x00000000},
         {Pm4::FSR_CONTROL_POINT3, 0x00000000},
     }},
    /* 16 */
    {0x4626B750,
     {
         {Pm4::FSR_WINDOW0, 0x00000000},
         {Pm4::FSR_WINDOW1, 0x00000000},
     }},
    /* 17 */ {0x4CC673A0, {{Pm4::MEMORY_MAPPING_MASK, 0x00000000}}},
    /* 18 */ {0xDE5B3431, {{Pm4::UC_NOP, 0x00000000}}},
    /* 19 */ {0x036AC8A6, {{Pm4::GE_USER_VGPR1, 0x00000000}}}};

static RegisterDefaultInfo g_cx_reg_info2[] = {
    /* 0 */ {0x8FB4EDB5, {{Pm4::DB_DFSM_CONTROL, 0x00000000}}},
    /* 1 */ {0xB994AD29, {{Pm4::DB_HTILE_SURFACE, 0x00000000}}},
    /* 2 */ {0xD427322F, {{Pm4::PA_SC_NGG_MODE_CNTL, 0x00000000}}},
    /* 3 */ {0xF58FEA31, {{Pm4::SPI_INTERP_CONTROL_0, 0x00000000}}},
};

static RegisterDefaultInfo g_sh_reg_info2[] = {
    /* 0 */ {0x6AC156EF, {{Pm4::COMPUTE_DESTINATION_EN_SE0, 0x00000000}}},
    /* 1 */ {0x6AC15610, {{Pm4::COMPUTE_DESTINATION_EN_SE1, 0x00000000}}},
    /* 2 */ {0x6AC15009, {{Pm4::COMPUTE_DESTINATION_EN_SE2, 0x00000000}}},
    /* 3 */ {0x6AC153BA, {{Pm4::COMPUTE_DESTINATION_EN_SE3, 0x00000000}}},
    /* 4 */ {0xBE7DCD73, {{Pm4::COMPUTE_DISPATCH_TUNNEL, 0x00000000}}},
    /* 5 */ {0x0C4B1438, {{Pm4::COMPUTE_SHADER_CHKSUM, 0x00000000}}},
    /* 6 */ {0xDB00D71A, {{Pm4::COMPUTE_START_X, 0x00000000}}},
    /* 7 */ {0xDB00D249, {{Pm4::COMPUTE_START_Y, 0x00000000}}},
    /* 8 */ {0xDB00EC60, {{Pm4::COMPUTE_START_Z, 0x00000000}}},
    /* 9 */ {0x0C4D6FE4, {{Pm4::SPI_SHADER_PGM_CHKSUM_GS, 0x00000000}}},
    /* 10 */ {0x0C4A80EF, {{Pm4::SPI_SHADER_PGM_CHKSUM_HS, 0x00000000}}},
    /* 11 */ {0x0DD283E7, {{Pm4::SPI_SHADER_PGM_CHKSUM_PS, 0x00000000}}},
    /* 12 */ {0xC620E68C, {{Pm4::SPI_SHADER_PGM_RSRC4_GS, 0x00000000}}},
    /* 13 */ {0xC67EFACF, {{Pm4::SPI_SHADER_PGM_RSRC4_HS, 0x00000000}}},
    /* 14 */ {0xD9E6D9F7, {{Pm4::SPI_SHADER_PGM_RSRC4_PS, 0x00000000}}},
};

static RegisterDefaultInfo g_uc_reg_info2[] = {
    /* 0 */ {0x31F34B9F, {{Pm4::VGT_HS_OFFCHIP_PARAM, 0x00000000}}},
    /* 1 */ {0xAC0F9E76, {{Pm4::UC_NOP, 0x00000000}}},
    /* 2 */ {0x929FD95D, {{Pm4::VGT_TF_MEMORY_BASE, 0x00000000}}},
};

#define KYTY_ID(id, tbl)   ((id)*4 + (tbl))
#define KYTY_INDEX_CX1(id) g_cx_reg_info1[id].type, KYTY_ID(id, 0), 0
#define KYTY_INDEX_SH1(id) g_sh_reg_info1[id].type, KYTY_ID(id, 1), 0
#define KYTY_INDEX_UC1(id) g_uc_reg_info1[id].type, KYTY_ID(id, 2), 0
#define KYTY_INDEX_CX2(id) g_cx_reg_info2[id].type, KYTY_ID(id, 0), 0
#define KYTY_INDEX_SH2(id) g_sh_reg_info2[id].type, KYTY_ID(id, 1), 0
#define KYTY_INDEX_UC2(id) g_uc_reg_info2[id].type, KYTY_ID(id, 2), 0
#define KYTY_REG_CX1(id)   &g_cx_reg_info1[id].reg[0]
#define KYTY_REG_SH1(id)   &g_sh_reg_info1[id].reg[0]
#define KYTY_REG_UC1(id)   &g_uc_reg_info1[id].reg[0]
#define KYTY_REG_CX2(id)   &g_cx_reg_info2[id].reg[0]
#define KYTY_REG_SH2(id)   &g_sh_reg_info2[id].reg[0]
#define KYTY_REG_UC2(id)   &g_uc_reg_info2[id].reg[0]

static ShaderRegister* g_tbl_cx1[] = {
    KYTY_REG_CX1(0),  KYTY_REG_CX1(1),  KYTY_REG_CX1(2),  KYTY_REG_CX1(3),  KYTY_REG_CX1(4),  KYTY_REG_CX1(5),  KYTY_REG_CX1(6),
    KYTY_REG_CX1(7),  KYTY_REG_CX1(8),  KYTY_REG_CX1(9),  KYTY_REG_CX1(10), KYTY_REG_CX1(11), KYTY_REG_CX1(12), KYTY_REG_CX1(13),
    KYTY_REG_CX1(14), KYTY_REG_CX1(15), KYTY_REG_CX1(16), KYTY_REG_CX1(17), KYTY_REG_CX1(18), KYTY_REG_CX1(19), KYTY_REG_CX1(20),
    KYTY_REG_CX1(21), KYTY_REG_CX1(22), KYTY_REG_CX1(23), KYTY_REG_CX1(24), KYTY_REG_CX1(25), KYTY_REG_CX1(26), KYTY_REG_CX1(27),
    KYTY_REG_CX1(28), KYTY_REG_CX1(29), KYTY_REG_CX1(30), KYTY_REG_CX1(31), KYTY_REG_CX1(32), KYTY_REG_CX1(33), KYTY_REG_CX1(34),
    KYTY_REG_CX1(35), KYTY_REG_CX1(36), KYTY_REG_CX1(37), KYTY_REG_CX1(38), KYTY_REG_CX1(39), KYTY_REG_CX1(40), KYTY_REG_CX1(41),
    KYTY_REG_CX1(42), KYTY_REG_CX1(43), KYTY_REG_CX1(44), KYTY_REG_CX1(45), KYTY_REG_CX1(46), KYTY_REG_CX1(47), KYTY_REG_CX1(48),
    KYTY_REG_CX1(49), KYTY_REG_CX1(50), KYTY_REG_CX1(51), KYTY_REG_CX1(52), KYTY_REG_CX1(53), KYTY_REG_CX1(54), KYTY_REG_CX1(55),
    KYTY_REG_CX1(56), KYTY_REG_CX1(57), KYTY_REG_CX1(58), KYTY_REG_CX1(59), KYTY_REG_CX1(60), KYTY_REG_CX1(61), KYTY_REG_CX1(62),
    KYTY_REG_CX1(63), KYTY_REG_CX1(64), KYTY_REG_CX1(65), KYTY_REG_CX1(66), KYTY_REG_CX1(67), KYTY_REG_CX1(68), KYTY_REG_CX1(69),
    KYTY_REG_CX1(70), KYTY_REG_CX1(71), KYTY_REG_CX1(72), KYTY_REG_CX1(73), KYTY_REG_CX1(74), KYTY_REG_CX1(75), KYTY_REG_CX1(76),
    KYTY_REG_CX1(77)};

static ShaderRegister* g_tbl_sh1[]    = {KYTY_REG_SH1(0),  KYTY_REG_SH1(1),  KYTY_REG_SH1(2),  KYTY_REG_SH1(3),  KYTY_REG_SH1(4),
                                         KYTY_REG_SH1(5),  KYTY_REG_SH1(6),  KYTY_REG_SH1(7),  KYTY_REG_SH1(8),  KYTY_REG_SH1(9),
                                         KYTY_REG_SH1(10), KYTY_REG_SH1(11), KYTY_REG_SH1(12), KYTY_REG_SH1(13), KYTY_REG_SH1(14),
                                         KYTY_REG_SH1(15), KYTY_REG_SH1(16), KYTY_REG_SH1(17), KYTY_REG_SH1(18), KYTY_REG_SH1(19),
                                         KYTY_REG_SH1(20), KYTY_REG_SH1(21), KYTY_REG_SH1(22), KYTY_REG_SH1(23), KYTY_REG_SH1(24),
                                         KYTY_REG_SH1(25), KYTY_REG_SH1(26), KYTY_REG_SH1(27), KYTY_REG_SH1(28)};
static ShaderRegister* g_tbl_uc1[]    = {KYTY_REG_UC1(0),  KYTY_REG_UC1(1),  KYTY_REG_UC1(2),  KYTY_REG_UC1(3),  KYTY_REG_UC1(4),
                                         KYTY_REG_UC1(5),  KYTY_REG_UC1(6),  KYTY_REG_UC1(7),  KYTY_REG_UC1(8),  KYTY_REG_UC1(9),
                                         KYTY_REG_UC1(10), KYTY_REG_UC1(11), KYTY_REG_UC1(12), KYTY_REG_UC1(13), KYTY_REG_UC1(14),
                                         KYTY_REG_UC1(15), KYTY_REG_UC1(16), KYTY_REG_UC1(17), KYTY_REG_UC1(18), KYTY_REG_UC1(19)};
static uint32_t        g_tbl_index1[] = {
           KYTY_INDEX_CX1(0),  KYTY_INDEX_CX1(1),  KYTY_INDEX_CX1(2),  KYTY_INDEX_CX1(3),  KYTY_INDEX_CX1(4),  KYTY_INDEX_CX1(5),
           KYTY_INDEX_CX1(6),  KYTY_INDEX_CX1(7),  KYTY_INDEX_CX1(8),  KYTY_INDEX_CX1(9),  KYTY_INDEX_CX1(10), KYTY_INDEX_CX1(11),
           KYTY_INDEX_CX1(12), KYTY_INDEX_CX1(13), KYTY_INDEX_CX1(14), KYTY_INDEX_CX1(15), KYTY_INDEX_CX1(16), KYTY_INDEX_CX1(17),
           KYTY_INDEX_CX1(18), KYTY_INDEX_CX1(19), KYTY_INDEX_CX1(20), KYTY_INDEX_CX1(21), KYTY_INDEX_CX1(22), KYTY_INDEX_CX1(23),
           KYTY_INDEX_CX1(24), KYTY_INDEX_CX1(25), KYTY_INDEX_CX1(26), KYTY_INDEX_CX1(27), KYTY_INDEX_CX1(28), KYTY_INDEX_CX1(29),
           KYTY_INDEX_CX1(30), KYTY_INDEX_CX1(31), KYTY_INDEX_CX1(32), KYTY_INDEX_CX1(33), KYTY_INDEX_CX1(34), KYTY_INDEX_CX1(35),
           KYTY_INDEX_CX1(36), KYTY_INDEX_CX1(37), KYTY_INDEX_CX1(38), KYTY_INDEX_CX1(39), KYTY_INDEX_CX1(40), KYTY_INDEX_CX1(41),
           KYTY_INDEX_CX1(42), KYTY_INDEX_CX1(43), KYTY_INDEX_CX1(44), KYTY_INDEX_CX1(45), KYTY_INDEX_CX1(46), KYTY_INDEX_CX1(47),
           KYTY_INDEX_CX1(48), KYTY_INDEX_CX1(49), KYTY_INDEX_CX1(50), KYTY_INDEX_CX1(51), KYTY_INDEX_CX1(52), KYTY_INDEX_CX1(53),
           KYTY_INDEX_CX1(54), KYTY_INDEX_CX1(55), KYTY_INDEX_CX1(56), KYTY_INDEX_CX1(57), KYTY_INDEX_CX1(58), KYTY_INDEX_CX1(59),
           KYTY_INDEX_CX1(60), KYTY_INDEX_CX1(61), KYTY_INDEX_CX1(62), KYTY_INDEX_CX1(63), KYTY_INDEX_CX1(64), KYTY_INDEX_CX1(65),
           KYTY_INDEX_CX1(66), KYTY_INDEX_CX1(67), KYTY_INDEX_CX1(68), KYTY_INDEX_CX1(69), KYTY_INDEX_CX1(70), KYTY_INDEX_CX1(71),
           KYTY_INDEX_CX1(72), KYTY_INDEX_CX1(73), KYTY_INDEX_CX1(74), KYTY_INDEX_CX1(75), KYTY_INDEX_CX1(76), KYTY_INDEX_CX1(77),
           KYTY_INDEX_SH1(0),  KYTY_INDEX_SH1(1),  KYTY_INDEX_SH1(2),  KYTY_INDEX_SH1(3),  KYTY_INDEX_SH1(4),  KYTY_INDEX_SH1(5),
           KYTY_INDEX_SH1(6),  KYTY_INDEX_SH1(7),  KYTY_INDEX_SH1(8),  KYTY_INDEX_SH1(9),  KYTY_INDEX_SH1(10), KYTY_INDEX_SH1(11),
           KYTY_INDEX_SH1(12), KYTY_INDEX_SH1(13), KYTY_INDEX_SH1(14), KYTY_INDEX_SH1(15), KYTY_INDEX_SH1(16), KYTY_INDEX_SH1(17),
           KYTY_INDEX_SH1(18), KYTY_INDEX_SH1(19), KYTY_INDEX_SH1(20), KYTY_INDEX_SH1(21), KYTY_INDEX_SH1(22), KYTY_INDEX_SH1(23),
           KYTY_INDEX_SH1(24), KYTY_INDEX_SH1(25), KYTY_INDEX_SH1(26), KYTY_INDEX_SH1(27), KYTY_INDEX_SH1(28), KYTY_INDEX_UC1(0),
           KYTY_INDEX_UC1(1),  KYTY_INDEX_UC1(2),  KYTY_INDEX_UC1(3),  KYTY_INDEX_UC1(4),  KYTY_INDEX_UC1(5),  KYTY_INDEX_UC1(6),
           KYTY_INDEX_UC1(7),  KYTY_INDEX_UC1(8),  KYTY_INDEX_UC1(9),  KYTY_INDEX_UC1(10), KYTY_INDEX_UC1(11), KYTY_INDEX_UC1(12),
           KYTY_INDEX_UC1(13), KYTY_INDEX_UC1(14), KYTY_INDEX_UC1(15), KYTY_INDEX_UC1(16), KYTY_INDEX_UC1(17), KYTY_INDEX_UC1(18),
           KYTY_INDEX_UC1(19)};

static ShaderRegister* g_tbl_cx2[]    = {KYTY_REG_CX2(0), KYTY_REG_CX2(1), KYTY_REG_CX2(2), KYTY_REG_CX2(3)};
static ShaderRegister* g_tbl_sh2[]    = {KYTY_REG_SH2(0),  KYTY_REG_SH2(1),  KYTY_REG_SH2(2),  KYTY_REG_SH2(3),  KYTY_REG_SH2(4),
                                         KYTY_REG_SH2(5),  KYTY_REG_SH2(6),  KYTY_REG_SH2(7),  KYTY_REG_SH2(8),  KYTY_REG_SH2(9),
                                         KYTY_REG_SH2(10), KYTY_REG_SH2(11), KYTY_REG_SH2(12), KYTY_REG_SH2(13), KYTY_REG_SH2(14)};
static ShaderRegister* g_tbl_uc2[]    = {KYTY_REG_UC2(0), KYTY_REG_UC2(1), KYTY_REG_UC2(2)};
static uint32_t        g_tbl_index2[] = {KYTY_INDEX_CX2(0),  KYTY_INDEX_CX2(1),  KYTY_INDEX_CX2(2),  KYTY_INDEX_CX2(3),  KYTY_INDEX_SH2(0),
                                         KYTY_INDEX_SH2(1),  KYTY_INDEX_SH2(2),  KYTY_INDEX_SH2(3),  KYTY_INDEX_SH2(4),  KYTY_INDEX_SH2(5),
                                         KYTY_INDEX_SH2(6),  KYTY_INDEX_SH2(7),  KYTY_INDEX_SH2(8),  KYTY_INDEX_SH2(9),  KYTY_INDEX_SH2(10),
                                         KYTY_INDEX_SH2(11), KYTY_INDEX_SH2(12), KYTY_INDEX_SH2(13), KYTY_INDEX_SH2(14), KYTY_INDEX_UC2(0),
                                         KYTY_INDEX_UC2(1),  KYTY_INDEX_UC2(2)};

static RegisterDefaults g_reg_defaults1 = { // @suppress("Invalid arguments")
    g_tbl_cx1, g_tbl_sh1, g_tbl_uc1, nullptr, {0, 0}, g_tbl_index1, sizeof(g_tbl_index1) / 12};
static RegisterDefaults g_reg_defaults2 = { // @suppress("Invalid arguments")
    g_tbl_cx2, g_tbl_sh2, g_tbl_uc2, nullptr, {0, 0}, g_tbl_index2, sizeof(g_tbl_index2) / 12};

int KYTY_SYSV_ABI GraphicsInit(uint32_t* state, uint32_t ver)
{
	PRINT_NAME();

	printf("\t state = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(state));
	printf("\t ver   = %u\n", ver);

	// Null state is accepted (return OK) so titles that probe early do not hard
	// abort; non-null must receive version + feature-flag words.
	if (state == nullptr)
	{
		return OK;
	}
	// Gen5 tables were authored for AGC ver 8. Other versions currently reuse
	// those register defaults while their version-specific tables are modeled.
	if (ver != 8)
	{
		printf("\t WARNING: AGC ver %u != 8, using ver-8 register defaults\n", ver);
	}
	EXIT_IF(!GraphicsInitWriteGuestState(state, ver));

	return OK;
}

void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2(uint32_t ver)
{
	PRINT_NAME();

	if (ver != 8) { printf("\t WARNING: AGC ver %u != 8\n", ver); }
	EXIT_NOT_IMPLEMENTED(offsetof(RegisterDefaults, count) != 0x38);

	return &g_reg_defaults1;
}

void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2Internal(uint32_t ver)
{
	PRINT_NAME();

	if (ver != 8) { printf("\t WARNING: AGC ver %u != 8\n", ver); }
	EXIT_NOT_IMPLEMENTED(offsetof(RegisterDefaults, count) != 0x38);

	return &g_reg_defaults2;
}

static void dbg_dump_shader(const Shader* h)
{
	printf("\t file_header  = 0x%08" PRIx32 "\n", h->file_header);
	printf("\t version      = 0x%08" PRIx32 "\n", h->version);
	printf("\t user_data    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->user_data));
	if (h->user_data != nullptr)
	{
		printf("\t\t direct_resource_count    = 0x%04" PRIx16 "\n", h->user_data->direct_resource_count);
		printf("\t\t direct_resource_offset   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->user_data->direct_resource_offset));
		for (int i = 0; i < static_cast<int>(h->user_data->direct_resource_count); i++)
		{
			printf("\t\t\t offset[%02d] = %04" PRIx16 "\n", i, h->user_data->direct_resource_offset[i]);
		}
		for (int imm = 0; imm < 4; imm++)
		{
			printf("\t\t sharp_resource_count  [%d] = 0x%04" PRIx16 "\n", imm, h->user_data->sharp_resource_count[imm]);
			printf("\t\t sharp_resource_offset [%d] = 0x%016" PRIx64 "\n", imm,
			       reinterpret_cast<uint64_t>(h->user_data->sharp_resource_offset[imm]));
			for (int i = 0; i < static_cast<int>(h->user_data->sharp_resource_count[imm]); i++)
			{
				printf("\t\t\t offset_dw[%d] = %04" PRIx16 ", size = %" PRIu16 "\n", i,
				       h->user_data->sharp_resource_offset[imm][i].offset_dw, h->user_data->sharp_resource_offset[imm][i].size);
			}
		}
		printf("\t\t eud_size_dw    = 0x%04" PRIx16 "\n", h->user_data->eud_size_dw);
		printf("\t\t srt_size_dw    = 0x%04" PRIx16 "\n", h->user_data->srt_size_dw);
	}
	printf("\t code             = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->code));
	printf("\t num_cx_registers = 0x%02" PRIx8 "\n", h->num_cx_registers);
	printf("\t cx_registers     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->cx_registers));
	for (int i = 0; i < static_cast<int>(h->num_cx_registers); i++)
	{
		printf("\t\t cx[%d]: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", i, h->cx_registers[i].offset, h->cx_registers[i].value);
	}
	printf("\t num_sh_registers = 0x%02" PRIx8 "\n", h->num_sh_registers);
	printf("\t sh_registers     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->sh_registers));
	for (int i = 0; i < static_cast<int>(h->num_sh_registers); i++)
	{
		printf("\t\t sh[%d]: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", i, h->sh_registers[i].offset, h->sh_registers[i].value);
	}
	printf("\t specials                          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->specials));
	printf("\t\t ge_cntl:              offset = %08" PRIx32 ", value = %08" PRIx32 "\n", h->specials->ge_cntl.offset,
	       h->specials->ge_cntl.value);
	printf("\t\t vgt_shader_stages_en: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", h->specials->vgt_shader_stages_en.offset,
	       h->specials->vgt_shader_stages_en.value);
	printf("\t\t vgt_gs_out_prim_type: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", h->specials->vgt_gs_out_prim_type.offset,
	       h->specials->vgt_gs_out_prim_type.value);
	printf("\t\t ge_user_vgpr_en:      offset = %08" PRIx32 ", value = %08" PRIx32 "\n", h->specials->ge_user_vgpr_en.offset,
	       h->specials->ge_user_vgpr_en.value);
	printf("\t\t dispatch_modifier = %08" PRIx32 "\n", h->specials->dispatch_modifier);
	printf("\t\t user_data_range: start = %08" PRIx32 ", end = %08" PRIx32 "\n", h->specials->user_data_range.start,
	       h->specials->user_data_range.end);
	printf("\t\t draw_modifier: enbl_start_vertex_offset   = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_start_vertex_offset);
	printf("\t\t draw_modifier: enbl_start_index_offset    = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_start_index_offset);
	printf("\t\t draw_modifier: enbl_start_instance_offset = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_start_instance_offset);
	printf("\t\t draw_modifier: enbl_draw_index            = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_draw_index);
	printf("\t\t draw_modifier: enbl_user_vgprs            = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_user_vgprs);
	printf("\t\t draw_modifier: render_target_slice_offset = %08" PRIx32 "\n", h->specials->draw_modifier.render_target_slice_offset);
	printf("\t\t draw_modifier: fuse_draws                 = %08" PRIx32 "\n", h->specials->draw_modifier.fuse_draws);
	printf("\t\t draw_modifier: compiler_flags             = %08" PRIx32 "\n", h->specials->draw_modifier.compiler_flags);
	printf("\t\t draw_modifier: is_default                 = %08" PRIx32 "\n", h->specials->draw_modifier.is_default);
	printf("\t\t draw_modifier: reserved                   = %08" PRIx32 "\n", h->specials->draw_modifier.reserved);
	printf("\t num_input_semantics               = 0x%08" PRIx32 "\n", h->num_input_semantics);
	printf("\t input_semantics                   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->input_semantics));
	for (int i = 0; i < static_cast<int>(h->num_input_semantics); i++)
	{
		printf("\t\t input_semantics[%d]: semantic         = %08" PRIx32 "\n", i, h->input_semantics[i].semantic);
		printf("\t\t input_semantics[%d]: hardware_mapping = %08" PRIx32 "\n", i, h->input_semantics[i].hardware_mapping);
		printf("\t\t input_semantics[%d]: size_in_elements = %08" PRIx32 "\n", i, h->input_semantics[i].size_in_elements);
		printf("\t\t input_semantics[%d]: is_f16           = %08" PRIx32 "\n", i, h->input_semantics[i].is_f16);
		printf("\t\t input_semantics[%d]: is_flat_shaded   = %08" PRIx32 "\n", i, h->input_semantics[i].is_flat_shaded);
		printf("\t\t input_semantics[%d]: is_linear        = %08" PRIx32 "\n", i, h->input_semantics[i].is_linear);
		printf("\t\t input_semantics[%d]: is_custom        = %08" PRIx32 "\n", i, h->input_semantics[i].is_custom);
		printf("\t\t input_semantics[%d]: static_vb_index  = %08" PRIx32 "\n", i, h->input_semantics[i].static_vb_index);
		printf("\t\t input_semantics[%d]: static_attribute = %08" PRIx32 "\n", i, h->input_semantics[i].static_attribute);
		printf("\t\t input_semantics[%d]: reserved         = %08" PRIx32 "\n", i, h->input_semantics[i].reserved);
		printf("\t\t input_semantics[%d]: default_value    = %08" PRIx32 "\n", i, h->input_semantics[i].default_value);
		printf("\t\t input_semantics[%d]: default_value_hi = %08" PRIx32 "\n", i, h->input_semantics[i].default_value_hi);
	}
	printf("\t num_output_semantics              = 0x%04" PRIx16 "\n", h->num_output_semantics);
	printf("\t output_semantics                  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->output_semantics));
	for (int i = 0; i < static_cast<int>(h->num_output_semantics); i++)
	{
		printf("\t\t output_semantics[%d]: semantic         = %08" PRIx32 "\n", i, h->output_semantics[i].semantic);
		printf("\t\t output_semantics[%d]: hardware_mapping = %08" PRIx32 "\n", i, h->output_semantics[i].hardware_mapping);
		printf("\t\t output_semantics[%d]: size_in_elements = %08" PRIx32 "\n", i, h->output_semantics[i].size_in_elements);
		printf("\t\t output_semantics[%d]: is_f16           = %08" PRIx32 "\n", i, h->output_semantics[i].is_f16);
		printf("\t\t output_semantics[%d]: is_flat_shaded   = %08" PRIx32 "\n", i, h->output_semantics[i].is_flat_shaded);
		printf("\t\t output_semantics[%d]: is_linear        = %08" PRIx32 "\n", i, h->output_semantics[i].is_linear);
		printf("\t\t output_semantics[%d]: is_custom        = %08" PRIx32 "\n", i, h->output_semantics[i].is_custom);
		printf("\t\t output_semantics[%d]: static_vb_index  = %08" PRIx32 "\n", i, h->output_semantics[i].static_vb_index);
		printf("\t\t output_semantics[%d]: static_attribute = %08" PRIx32 "\n", i, h->output_semantics[i].static_attribute);
		printf("\t\t output_semantics[%d]: reserved         = %08" PRIx32 "\n", i, h->output_semantics[i].reserved);
		printf("\t\t output_semantics[%d]: default_value    = %08" PRIx32 "\n", i, h->output_semantics[i].default_value);
		printf("\t\t output_semantics[%d]: default_value_hi = %08" PRIx32 "\n", i, h->output_semantics[i].default_value_hi);
	}
	printf("\t header_size                       = 0x%08" PRIx32 "\n", h->header_size);
	printf("\t shader_size                       = 0x%08" PRIx32 "\n", h->shader_size);
	printf("\t embedded_constant_buffer_size_dqw = 0x%08" PRIx32 "\n", h->embedded_constant_buffer_size_dqw);
	printf("\t target                            = 0x%08" PRIx32 "\n", h->target);
	printf("\t scratch_size_dw_per_thread        = 0x%04" PRIx16 "\n", h->scratch_size_dw_per_thread);
	printf("\t special_sizes_bytes               = 0x%04" PRIx16 "\n", h->special_sizes_bytes);
	printf("\t type                              = 0x%02" PRIx8 "\n", h->type);
}

int KYTY_SYSV_ABI GraphicsCreateShader(Shader** dst, void* header, const volatile void* code)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(dst == nullptr);
	EXIT_NOT_IMPLEMENTED(header == nullptr);
	EXIT_NOT_IMPLEMENTED(code == nullptr);

	printf("\t header = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(header));
	printf("\t code   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(code));

	auto* h = static_cast<Shader*>(header);

	auto update_addr = [](auto& m)
	{
		if (m != nullptr)
		{
			m = reinterpret_cast<typename std::remove_reference<decltype(m)>::type>(reinterpret_cast<uintptr_t>(m) +
			                                                                        reinterpret_cast<uintptr_t>(&m));
		}
	};

	update_addr(h->cx_registers);
	update_addr(h->sh_registers);
	update_addr(h->user_data);
	update_addr(h->specials);
	update_addr(h->input_semantics);
	update_addr(h->output_semantics);
	update_addr(h->user_data->direct_resource_offset);
	update_addr(h->user_data->sharp_resource_offset[0]);
	update_addr(h->user_data->sharp_resource_offset[1]);
	update_addr(h->user_data->sharp_resource_offset[2]);
	update_addr(h->user_data->sharp_resource_offset[3]);

	h->code = code;

	EXIT_NOT_IMPLEMENTED(h->file_header != 0x34333231);
	EXIT_NOT_IMPLEMENTED(h->version != 0x00000018);

	auto base = reinterpret_cast<uint64_t>(code);

	printf("\t base   = 0x%016" PRIx64 "\n", base);

	ShaderMappedData map;
	map.user_data           = h->user_data;
	map.input_semantics     = h->input_semantics;
	map.num_input_semantics = h->num_input_semantics;

	ShaderMapUserData(base, map);

	EXIT_NOT_IMPLEMENTED((base & 0xFFFF0000000000FFull) != 0);

	// Gen5 shader binary types (Prospero::ShaderBinaryType). Front halves and FS
	// carry no program address registers; GS/HS use LO_ES/LO_LS (merged) or
	// LO_GS/LO_HS (back halves). Search the SH list rather than assuming [0]/[1].
	constexpr uint8_t kShaderBinaryCs      = 0;
	constexpr uint8_t kShaderBinaryPs      = 1;
	constexpr uint8_t kShaderBinaryGs      = 2;
	constexpr uint8_t kShaderBinaryHs      = 3;
	constexpr uint8_t kShaderBinaryGsFront = 4;
	constexpr uint8_t kShaderBinaryHsFront = 5;
	constexpr uint8_t kShaderBinaryGsBack  = 6;
	constexpr uint8_t kShaderBinaryHsBack  = 7;
	constexpr uint8_t kShaderBinaryFs      = 8;

	uint32_t lo_offset = 0;
	bool     needs_pgm = true;
	switch (h->type)
	{
		case kShaderBinaryCs: lo_offset = Pm4::COMPUTE_PGM_LO; break;
		case kShaderBinaryPs: lo_offset = Pm4::SPI_SHADER_PGM_LO_PS; break;
		case kShaderBinaryGs: lo_offset = Pm4::SPI_SHADER_PGM_LO_ES; break;
		case kShaderBinaryHs: lo_offset = Pm4::SPI_SHADER_PGM_LO_LS; break;
		case kShaderBinaryGsBack: lo_offset = Pm4::SPI_SHADER_PGM_LO_GS; break;
		case kShaderBinaryHsBack: lo_offset = Pm4::SPI_SHADER_PGM_LO_HS; break;
		case kShaderBinaryGsFront:
		case kShaderBinaryHsFront:
		case kShaderBinaryFs: needs_pgm = false; break;
		default:
			printf("\t SHADER DIAG: unknown type=%u num_sh_registers=%u\n", h->type, h->num_sh_registers);
			EXIT("invalid shader\n");
	}

	if (needs_pgm)
	{
		EXIT_NOT_IMPLEMENTED(h->sh_registers == nullptr || h->num_sh_registers == 0);

		bool patched = false;
		for (uint32_t lo_index = 0; lo_index < h->num_sh_registers; lo_index++)
		{
			if (h->sh_registers[lo_index].offset != lo_offset)
			{
				continue;
			}
			const uint32_t hi_index  = lo_index + 1u;
			const uint32_t hi_offset = lo_offset + 1u;
			EXIT_NOT_IMPLEMENTED(hi_index >= h->num_sh_registers || h->sh_registers[hi_index].offset != hi_offset);

			// Header LO/HI hold a relative code offset; absolute = base + offset.
			const uint64_t shader_offset =
			    (static_cast<uint64_t>(h->sh_registers[lo_index].value) << 8u) |
			    ((static_cast<uint64_t>(h->sh_registers[hi_index].value) & 0xffu) << 40u);
			const uint64_t addr = base + shader_offset;

			h->sh_registers[lo_index].value = static_cast<uint32_t>((addr >> 8u) & 0xffffffffu);
			h->sh_registers[hi_index].value =
			    (h->sh_registers[hi_index].value & 0xffffff00u) | static_cast<uint32_t>((addr >> 40u) & 0xffu);
			patched = true;
			break;
		}
		if (!patched)
		{
			printf("\t SHADER DIAG: type=%u num_sh_registers=%u missing PGM_LO=0x%x\n", h->type, h->num_sh_registers,
			       lo_offset);
			for (uint32_t i = 0; i < h->num_sh_registers && i < 8; i++)
			{
				printf("\t   sh_reg[%u] offset=0x%x value=0x%x\n", i, h->sh_registers[i].offset, h->sh_registers[i].value);
			}
			EXIT("invalid shader\n");
		}
	}

	*dst = h;

	dbg_dump_shader(h);

	return OK;
}

static constexpr int kGraphics5ErrorInvalidShaderHalves = static_cast<int>(0x8a6c0008u);

static ShaderRegister* find_shader_register(ShaderRegister* regs, uint32_t num_regs, uint32_t offset, uint32_t occurrence = 0)
{
	if (regs == nullptr)
	{
		return nullptr;
	}
	for (uint32_t i = 0; i < num_regs; i++)
	{
		if (regs[i].offset != offset)
		{
			continue;
		}
		if (occurrence == 0)
		{
			return regs + i;
		}
		occurrence--;
	}
	return nullptr;
}

static void patch_shader_register_address(ShaderRegister* regs, uint32_t num_regs, uint32_t lo_offset, uint64_t address)
{
	auto* lo = find_shader_register(regs, num_regs, lo_offset);
	if (lo == nullptr)
	{
		return;
	}
	auto* hi = (lo + 1 < regs + num_regs && (lo + 1)->offset == lo_offset + 1u) ? lo + 1 : nullptr;
	if (hi == nullptr)
	{
		return;
	}
	lo->value = static_cast<uint32_t>((address >> 8u) & 0xffffffffu);
	hi->value = (hi->value & 0xffffff00u) | static_cast<uint32_t>((address >> 40u) & 0xffu);
}

int KYTY_SYSV_ABI GraphicsUnknownGetFusedShaderSize(SizeAlign* dst, const Shader* front, const Shader* back)
{
	PRINT_NAME();

	printf("\t dst   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst));
	printf("\t front = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(front));
	printf("\t back  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(back));

	EXIT_NOT_IMPLEMENTED(dst == nullptr);
	EXIT_NOT_IMPLEMENTED(front == nullptr);
	EXIT_NOT_IMPLEMENTED(back == nullptr);

	constexpr uint8_t kGsFront = 4;
	constexpr uint8_t kHsFront = 5;
	constexpr uint8_t kGsBack  = 6;
	constexpr uint8_t kHsBack  = 7;

	if (!((front->type == kGsFront && back->type == kGsBack) || (front->type == kHsFront && back->type == kHsBack)))
	{
		return kGraphics5ErrorInvalidShaderHalves;
	}

	dst->m_size  = static_cast<uint64_t>(back->num_sh_registers) * sizeof(ShaderRegister);
	dst->m_align = 4;
	return OK;
}

int KYTY_SYSV_ABI GraphicsUnknownFuseShaderHalves(Shader* fused_result, const Shader* front, const Shader* back, void* scratch_mem)
{
	PRINT_NAME();

	printf("\t fused_result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(fused_result));
	printf("\t front        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(front));
	printf("\t back         = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(back));
	printf("\t scratch_mem  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(scratch_mem));

	EXIT_NOT_IMPLEMENTED(fused_result == nullptr);
	EXIT_NOT_IMPLEMENTED(front == nullptr);
	EXIT_NOT_IMPLEMENTED(back == nullptr);

	constexpr uint8_t kGs      = 2;
	constexpr uint8_t kHs      = 3;
	constexpr uint8_t kGsFront = 4;
	constexpr uint8_t kHsFront = 5;
	constexpr uint8_t kGsBack  = 6;
	constexpr uint8_t kHsBack  = 7;

	if (!((front->type == kGsFront && back->type == kGsBack) || (front->type == kHsFront && back->type == kHsBack)))
	{
		return kGraphics5ErrorInvalidShaderHalves;
	}

	*fused_result      = *back;
	fused_result->type = static_cast<uint8_t>(front->type == kGsFront ? kGs : kHs);

	if (front->specials != nullptr && back->specials != nullptr)
	{
		const auto front_stages = front->specials->vgt_shader_stages_en.value;
		const auto back_stages  = back->specials->vgt_shader_stages_en.value;
		const auto mismatch_bit = (front->type == kGsFront ? (1u << 22u) : (1u << 21u));
		if (((front_stages ^ back_stages) & mismatch_bit) != 0)
		{
			return kGraphics5ErrorInvalidShaderHalves;
		}
	}

	if (scratch_mem != nullptr && back->sh_registers != nullptr && back->num_sh_registers != 0)
	{
		auto* sh_registers = static_cast<ShaderRegister*>(scratch_mem);
		std::memcpy(sh_registers, back->sh_registers, static_cast<size_t>(back->num_sh_registers) * sizeof(ShaderRegister));
		fused_result->sh_registers = sh_registers;
	}

	auto*      fused_regs      = fused_result->sh_registers;
	const auto fused_reg_count = static_cast<uint32_t>(fused_result->num_sh_registers);
	const auto front_reg_count = static_cast<uint32_t>(front->num_sh_registers);

	if (front->type == kGsFront)
	{
		for (uint32_t occurrence = 0; occurrence < 2; occurrence++)
		{
			auto*       dst = find_shader_register(fused_regs, fused_reg_count, Pm4::SPI_SHADER_PGM_CHKSUM_GS, occurrence);
			const auto* src = find_shader_register(front->sh_registers, front_reg_count, Pm4::SPI_SHADER_PGM_CHKSUM_GS, occurrence);
			if (dst != nullptr && src != nullptr)
			{
				dst->value = src->value;
			}
		}
		patch_shader_register_address(fused_regs, fused_reg_count, Pm4::SPI_SHADER_PGM_LO_ES, reinterpret_cast<uint64_t>(front->code));
	} else
	{
		patch_shader_register_address(fused_regs, fused_reg_count, Pm4::SPI_SHADER_PGM_LO_LS, reinterpret_cast<uint64_t>(front->code));
	}

	fused_result->user_data = nullptr;
	return OK;
}

int KYTY_SYSV_ABI GraphicsSetCxRegIndirectPatchSetAddress(uint32_t* cmd, const volatile ShaderRegister* regs)
{
	PRINT_NAME();

	printf("\t cmd  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t regs = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);
	EXIT_NOT_IMPLEMENTED(regs == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	cmd[2] = vaddr & 0xffffffffu;
	cmd[3] = (vaddr >> 32u) & 0xffffffffu;

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetShRegIndirectPatchSetAddress(uint32_t* cmd, const volatile ShaderRegister* regs)
{
	PRINT_NAME();

	printf("\t cmd  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t regs = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);
	EXIT_NOT_IMPLEMENTED(regs == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	cmd[2] = vaddr & 0xffffffffu;
	cmd[3] = (vaddr >> 32u) & 0xffffffffu;

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetUcRegIndirectPatchSetAddress(uint32_t* cmd, const volatile ShaderRegister* regs)
{
	PRINT_NAME();

	printf("\t cmd  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t regs = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);
	EXIT_NOT_IMPLEMENTED(regs == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	cmd[2] = vaddr & 0xffffffffu;
	cmd[3] = (vaddr >> 32u) & 0xffffffffu;

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetCxRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs)
{
	PRINT_NAME();

	printf("\t cmd      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t num_regs = %" PRIu32 "\n", num_regs);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[1] += num_regs;

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetShRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs)
{
	PRINT_NAME();

	printf("\t cmd      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t num_regs = %" PRIu32 "\n", num_regs);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[1] += num_regs;

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetUcRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs)
{
	PRINT_NAME();

	printf("\t cmd      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t num_regs = %" PRIu32 "\n", num_regs);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[1] += num_regs;

	return OK;
}

int KYTY_SYSV_ABI GraphicsCreatePrimState(ShaderRegister* cx_regs, ShaderRegister* uc_regs, const Shader* hs, const Shader* gs,
                                          uint32_t prim_type)
{
	PRINT_NAME();

	printf("\t cx_regs   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cx_regs));
	printf("\t uc_regs   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(uc_regs));
	printf("\t hs        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(hs));
	printf("\t gs        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(gs));
	printf("\t prim_type = %" PRIu32 "\n", prim_type);

	EXIT_NOT_IMPLEMENTED(hs != nullptr);
	EXIT_NOT_IMPLEMENTED(gs == nullptr);
	EXIT_NOT_IMPLEMENTED(cx_regs == nullptr);
	EXIT_NOT_IMPLEMENTED(uc_regs == nullptr);

	EXIT_NOT_IMPLEMENTED(gs->type != 2);
	EXIT_NOT_IMPLEMENTED(gs->specials->vgt_shader_stages_en.offset != Pm4::VGT_SHADER_STAGES_EN);
	EXIT_NOT_IMPLEMENTED(gs->specials->vgt_gs_out_prim_type.offset != Pm4::VGT_GS_OUT_PRIM_TYPE);
	EXIT_NOT_IMPLEMENTED(gs->specials->ge_cntl.offset != Pm4::GE_CNTL);
	EXIT_NOT_IMPLEMENTED(gs->specials->ge_user_vgpr_en.offset != Pm4::GE_USER_VGPR_EN);

	cx_regs[0] = gs->specials->vgt_shader_stages_en;
	cx_regs[1] = gs->specials->vgt_gs_out_prim_type;

	uc_regs[0]        = gs->specials->ge_cntl;
	uc_regs[1]        = gs->specials->ge_user_vgpr_en;
	uc_regs[2].offset = Pm4::VGT_PRIMITIVE_TYPE;
	uc_regs[2].value  = prim_type;

	return OK;
}

// Pack ShaderSemantic bitfields into a 32-bit word matching the hardware layout.
static uint32_t ShaderSemanticWord(const ShaderSemantic& semantic)
{
	return ((semantic.semantic & 0xffu) << 0u) | ((semantic.hardware_mapping & 0xffu) << 8u) |
	       ((semantic.size_in_elements & 0xfu) << 16u) | ((semantic.is_f16 & 0x3u) << 20u) |
	       ((semantic.is_flat_shaded & 0x1u) << 22u) | ((semantic.is_linear & 0x1u) << 23u) |
	       ((semantic.is_custom & 0x1u) << 24u) | ((semantic.static_vb_index & 0x1u) << 25u) |
	       ((semantic.static_attribute & 0x1u) << 26u) | ((semantic.reserved & 0x1u) << 27u) |
	       ((semantic.default_value & 0x3u) << 28u) | ((semantic.default_value_hi & 0x3u) << 30u);
}

static uint32_t ApplyInterpolantDefaultValue(uint32_t value, uint32_t ps_word)
{
	value &= ~0x00000300u;
	value |= ((ps_word >> 28u) & 0x3u) << 8u;
	return value;
}

static uint32_t ApplyInterpolantDefaultValueHi(uint32_t value, uint32_t ps_word)
{
	value &= ~0x00600000u;
	value |= ((ps_word >> 30u) & 0x3u) << 21u;
	return value;
}

static uint32_t CreateInterpolantMappingValue(uint32_t value, uint32_t ps_word, uint32_t gs_word)
{
	const uint32_t flat_shade =
	    ((ps_word & 0x00400000u) != 0 || (ps_word & 0x01000000u) != 0 ? 0x00000400u : 0u);

	value &= ~0x0000001fu;
	value |= (gs_word >> 8u) & 0x1fu;
	value &= ~0x00000400u;
	value |= flat_shade;

	return ApplyInterpolantDefaultValue(value, ps_word);
}

static uint32_t CreateInterpolantDefaultValue(uint32_t value, uint32_t ps_word)
{
	value &= ~0x0000001fu;
	value &= ~0x00000400u;
	return ApplyInterpolantDefaultValue(value, ps_word);
}

static uint32_t CreateInterpolantF16Value(uint32_t ps_word, const ShaderSemantic* gs_semantic)
{
	uint32_t value = (ps_word << 4u) & 0x03000000u;

	if (gs_semantic == nullptr)
	{
		value |= 0x00180020u;
	}
	else
	{
		const uint32_t common_word = ps_word & ShaderSemanticWord(*gs_semantic);

		value &= 0xfff7ffdfu;
		value |= (common_word >> 15u) & 0x20u;
		value ^= 0x00080020u;
		value &= ~0x00100000u;
		value |= (~common_word >> 1u) & 0x00100000u;
	}

	return ApplyInterpolantDefaultValueHi(value, ps_word);
}

static uint32_t CreateInterpolantNonF16Value(uint32_t ps_word, const ShaderSemantic* gs_semantic)
{
	uint32_t value = 0;
	// OFFSET 0x20: hardware default when custom PS input or unmatched GS export.
	if ((ps_word & 0x01000000u) != 0 || gs_semantic == nullptr)
	{
		value |= 0x20u;
	}
	return value;
}

static const ShaderSemantic* FindInterpolantOutputSemantic(const Shader* gs, uint32_t semantic)
{
	if (gs == nullptr || gs->output_semantics == nullptr)
	{
		return nullptr;
	}
	for (uint16_t i = 0; i < gs->num_output_semantics; i++)
	{
		if (gs->output_semantics[i].semantic == semantic)
		{
			return &gs->output_semantics[i];
		}
	}
	return nullptr;
}

static void SetInterpolantRegister(ShaderRegister* regs, uint32_t index, uint32_t value)
{
	regs[index].offset = Pm4::SPI_PS_INPUT_CNTL_0 + index;
	regs[index].value  = value;
}

static void SetIdentityInterpolantRegisters(ShaderRegister* regs, uint32_t first_index)
{
	for (uint32_t i = first_index; i < 32u; i++)
	{
		SetInterpolantRegister(regs, i, i);
	}
}

// Legacy helper used by deterministic tests: map PS inputs from a VS/GS export
// superset (flat-shade bit, default OFFSET 0x20, or identity outputs when PS
// has no inputs). Does not cover f16/custom packs — CreateInterpolantMapping does.
bool GraphicsBuildInterpolantMapping(ShaderRegister* regs, const ShaderSemantic* outputs, uint32_t output_count,
                                     const ShaderSemantic* inputs, uint32_t input_count)
{
	if (regs == nullptr || output_count > 32 || input_count > 32 || (output_count != 0 && outputs == nullptr) ||
	    (input_count != 0 && inputs == nullptr))
	{
		return false;
	}

	for (uint32_t i = 0; i < 32; i++)
	{
		regs[i].offset = Pm4::SPI_PS_INPUT_CNTL_0 + i;
		regs[i].value  = 0;
	}
	if (inputs == nullptr && input_count == 0)
	{
		for (uint32_t output_index = 0; output_index < output_count; output_index++)
		{
			const auto& output = outputs[output_index];
			if (output.hardware_mapping >= 32)
			{
				return false;
			}
			regs[output_index].value = output.hardware_mapping;
		}
		return true;
	}

	for (uint32_t input_index = 0; input_index < input_count; input_index++)
	{
		const auto& input  = inputs[input_index];
		const auto  ps_word = ShaderSemanticWord(input);
		const ShaderSemantic* output = nullptr;
		for (uint32_t output_index = 0; output_index < output_count; output_index++)
		{
			if (outputs[output_index].semantic == input.semantic)
			{
				output = &outputs[output_index];
				break;
			}
		}

		uint32_t value = ((ps_word & 0x00300000u) != 0 ? CreateInterpolantF16Value(ps_word, output)
		                                               : CreateInterpolantNonF16Value(ps_word, output));
		value = (output == nullptr ? CreateInterpolantDefaultValue(value, ps_word)
		                           : CreateInterpolantMappingValue(value, ps_word, ShaderSemanticWord(*output)));
		regs[input_index].value = value;
	}
	return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int KYTY_SYSV_ABI GraphicsCreateInterpolantMapping(ShaderRegister* regs, const Shader* gs, const Shader* ps)
{
	PRINT_NAME();

	printf("\t regs = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
	printf("\t gs   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(gs));
	printf("\t ps   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(ps));

	EXIT_NOT_IMPLEMENTED(regs == nullptr);
	EXIT_NOT_IMPLEMENTED(ps != nullptr && ps->num_input_semantics != 0 && ps->input_semantics == nullptr);

	// No PS inputs: identity SPI_PS_INPUT_CNTL slots (common clear / full-screen paths).
	if (ps == nullptr || ps->num_input_semantics == 0)
	{
		SetIdentityInterpolantRegisters(regs, 0);
		return OK;
	}

	EXIT_NOT_IMPLEMENTED(gs == nullptr);
	EXIT_NOT_IMPLEMENTED(gs->num_output_semantics != 0 && gs->output_semantics == nullptr);
	EXIT_NOT_IMPLEMENTED(sizeof(ShaderSemantic) != 4);

	for (uint32_t ps_index = 0; ps_index < ps->num_input_semantics; ps_index++)
	{
		const auto& ps_semantic = ps->input_semantics[ps_index];
		const auto* gs_semantic = FindInterpolantOutputSemantic(gs, ps_semantic.semantic);
		const auto  ps_word     = ShaderSemanticWord(ps_semantic);

		auto value =
		    ((ps_word & 0x00300000u) != 0 ? CreateInterpolantF16Value(ps_word, gs_semantic)
		                                  : CreateInterpolantNonF16Value(ps_word, gs_semantic));
		value = (gs_semantic == nullptr
		             ? CreateInterpolantDefaultValue(value, ps_word)
		             : CreateInterpolantMappingValue(value, ps_word, ShaderSemanticWord(*gs_semantic)));

		SetInterpolantRegister(regs, ps_index, value);
	}

	SetIdentityInterpolantRegisters(regs, ps->num_input_semantics);
	return OK;
}

int KYTY_SYSV_ABI GraphicsGetDataPacketPayloadAddress(uint32_t** addr, uint32_t* cmd, int type)
{
	PRINT_NAME();

	printf("\t addr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(addr));
	printf("\t cmd  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t type = %d\n", type);

	EXIT_NOT_IMPLEMENTED(addr == nullptr);
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);
	// type 1: payload at cmd+2 (existing). type 0 observed on the post-logo
	// path with the same relative payload offset.
	EXIT_NOT_IMPLEMENTED(type != 0 && type != 1);

	const uint32_t header = cmd[0];
	const uint32_t r      = KYTY_PM4_R(header);
	printf("\t header = 0x%08" PRIx32 " r = 0x%02" PRIx32 "\n", header, r);

	// WaitMem stores the 64-bit address in the first body dwords (cmd+1).
	// ReleaseMem stores action/gcr then address at cmd+3 (matches EopPatch).
	// Default remains cmd+2 for WriteData / SET_SH_REG-style consumers.
	if (r == Pm4::R_WAIT_MEM_64 || r == Pm4::R_WAIT_MEM_32)
	{
		*addr = cmd + 1;
	}
	else if (r == Pm4::R_RELEASE_MEM)
	{
		*addr = cmd + 3;
	}
	else
	{
		*addr = cmd + 2;
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsSuspendPoint()
{
	PRINT_NAME();

	GraphicsRunDone();

	return OK;
}

uint64_t* GraphicsResolveWaitMemAddressFromPrecedingRelease(const uint32_t* wait_body)
{
	if (wait_body == nullptr)
	{
		return nullptr;
	}

	// wait_body[0] is the first body dword after the WaitMem64 header.
	// Contiguous post-Play fence layout (dwords):
	//   [rel_hdr][a][gcr|ds][addr_lo][addr_hi][data_lo][data_hi][wait_hdr][addr...]
	// so the ReleaseMem header sits 8 dwords before wait_body[0].
	const uint32_t* release = wait_body - 8;
	if (release[0] != KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_RELEASE_MEM))
	{
		return nullptr;
	}

	const uint64_t addr = release[3] | (static_cast<uint64_t>(release[4]) << 32u);
	if (addr == 0)
	{
		return nullptr;
	}

	return reinterpret_cast<uint64_t*>(addr);
}

int KYTY_SYSV_ABI GraphicsAgcQueueEndOfPipeActionPatchAddress(uint32_t* cmd, uint64_t address)
{
	PRINT_NAME();

	if (cmd == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const uint32_t header = cmd[0];
	if ((header >> 30u) != 3u || ((header >> 8u) & 0xffu) != Pm4::IT_NOP || KYTY_PM4_R(header) != Pm4::R_RELEASE_MEM ||
	    KYTY_PM4_LEN(header) < 7u)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	// ReleaseMem stores the 64-bit destination address in payload dwords 1..2.
	cmd[3] = static_cast<uint32_t>(address & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>(address >> 32u);
	return OK;
}

// sceAgcCbNop (NID LtTouSCZjHM). SysV: rdi=CommandBuffer*, rsi=num_dw.
// Encodes a full type-3 NOP of length num_dw (not a bare cursor bump).
uint32_t* KYTY_SYSV_ABI GraphicsCbNop(CommandBuffer* buf, uint32_t num_dw)
{
	PRINT_NAME();

	printf("\t buf    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buf));
	printf("\t num_dw = %" PRIu32 "\n", num_dw);

	if (buf == nullptr || num_dw < 2u || num_dw > 0x4001u)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(num_dw);
	if (cmd == nullptr)
	{
		return nullptr;
	}
	cmd[0] = KYTY_PM4(num_dw, Pm4::IT_NOP, Pm4::R_ZERO);
	for (uint32_t i = 1; i < num_dw; i++)
	{
		cmd[i] = 0;
	}
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbAllocateDwords(CommandBuffer* buf, uint32_t num_dw)
{
	// Historical alias: guests resolve LtTouSCZjHM as sceAgcCbNop.
	return GraphicsCbNop(buf, num_dw);
}

uint32_t KYTY_SYSV_ABI GraphicsCbNopGetSize(uint32_t size_in_dwords)
{
	return 4u * size_in_dwords;
}

uint32_t KYTY_SYSV_ABI GraphicsCbDispatchGetSize()
{
	// GraphicsCbDispatch allocates a fixed 5-dword packet.
	return 20u;
}

uint32_t KYTY_SYSV_ABI GraphicsCbSetShRegisterRangeDirectGetSize(uint32_t num_values)
{
	// Header + offset + values: (2 + num_values) dwords.
	return 4u * num_values + 8u;
}

uint64_t KYTY_SYSV_ABI GraphicsGetIsTrinityMode()
{
	// Non-Pro Prospero reports 0. Do not invent Pro/Trinity features.
	return 0;
}

int KYTY_SYSV_ABI GraphicsDebugRaiseException(uint32_t exception_id)
{
	PRINT_NAME();
	printf("\t exception_id = 0x%08" PRIx32 "\n", exception_id);
	return OK;
}

int KYTY_SYSV_ABI GraphicsWriteDataPatchSetAddressOrOffset(uint32_t* cmd, uint64_t address_or_offset)
{
	PRINT_NAME();
	printf("\t cmd = 0x%016" PRIx64 " addr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), address_or_offset);

	if (cmd == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const auto op  = (cmd[0] >> 8u) & 0xffu;
	const auto reg = KYTY_PM4_R(cmd[0]);
	// Accept both hardware IT_WRITE_DATA and the Gen5 custom R_WRITE_DATA NOP
	// envelope used by GraphicsDcbWriteData.
	if (op != Pm4::IT_WRITE_DATA && !(op == Pm4::IT_NOP && reg == Pm4::R_WRITE_DATA))
	{
		return static_cast<int>(0x8a6c000cu);
	}

	cmd[2] = static_cast<uint32_t>(address_or_offset & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((address_or_offset >> 32u) & 0xffffffffu);
	return OK;
}

// sceAgcGetPacketSize (NID Lkf86B98qPc): type-3 header → dword length.
uint32_t KYTY_SYSV_ABI GraphicsGetDataPacketSizeDw(const uint32_t* cmd)
{
	PRINT_NAME();

	printf("\t cmd = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));

	if (cmd == nullptr)
	{
		return 0;
	}

	const uint32_t header = cmd[0];
	printf("\t header = 0x%08" PRIx32 "\n", header);

	if ((header >> 30u) != 3u)
	{
		return 0;
	}

	const uint32_t size_dw = KYTY_PM4_LEN(header);
	printf("\t size_dw = %" PRIu32 "\n", size_dw);
	return size_dw;
}

// sceAgcDmaDataPatchSetDstAddressOrOffset (NID IxYiarKlXxM).
// R_DMA_DATA layout: +0 header, +16/+20 destination address lo/hi.
int KYTY_SYSV_ABI GraphicsAgcDmaDataPatchSetDstAddressOrOffset(uint32_t* cmd, uint64_t destination_address)
{
	PRINT_NAME();
	printf("\t cmd = 0x%016" PRIx64 " dst = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), destination_address);

	if (cmd == nullptr || !GraphicsIsCustomDmaDataPacket(cmd[0]))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	cmd[4] = static_cast<uint32_t>(destination_address & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>((destination_address >> 32u) & 0xffffffffu);
	return OK;
}

// sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate (NID cdDRpqcFGbU).
int KYTY_SYSV_ABI GraphicsAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate(uint32_t* cmd, uint64_t source_value)
{
	PRINT_NAME();
	printf("\t cmd = 0x%016" PRIx64 " src = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), source_value);

	if (cmd == nullptr || !GraphicsIsCustomDmaDataPacket(cmd[0]))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	cmd[6] = static_cast<uint32_t>(source_value & 0xffffffffu);
	cmd[7] = static_cast<uint32_t>((source_value >> 32u) & 0xffffffffu);
	return OK;
}

// sceAgcWaitRegMemPatchAddress (NID 3KDcnM3lrcU).
// IT_WAIT_REG_MEM: address at +8; custom R_WAIT_MEM_*: address at +4.
int KYTY_SYSV_ABI GraphicsAgcWaitRegMemPatchAddress(uint32_t* cmd, uint64_t address)
{
	PRINT_NAME();
	printf("\t cmd = 0x%016" PRIx64 " addr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), address);

	if (cmd == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	const uint32_t byte_off = GraphicsWaitRegMemAddressByteOffset(cmd[0]);
	if (byte_off == 0 || (byte_off % 4u) != 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	const uint32_t dw = byte_off / 4u;
	cmd[dw]     = static_cast<uint32_t>(address & 0xffffffffu);
	cmd[dw + 1] = static_cast<uint32_t>((address >> 32u) & 0xffffffffu);
	return OK;
}





int KYTY_SYSV_ABI GraphicsAgcDriverUnknownKRzWekV120()
{
	// Called immediately before the first indexed draw on observed Gen5 boots.
	// Real semantics unknown; return success so the draw path continues.
	PRINT_NAME();
	return OK;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbSetShRegisterRangeDirect(CommandBuffer* buf, uint32_t offset, const uint32_t* values, uint32_t num_values)
{
	PRINT_NAME();

	printf("\t buf        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buf));
	printf("\t offset     = %" PRIx32 "\n", offset);
	printf("\t values     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(values));
	printf("\t num_values = %" PRIu32 "\n", num_values);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(num_values == 0);
	EXIT_NOT_IMPLEMENTED(offset == 0);
	EXIT_NOT_IMPLEMENTED(offset > 0x3ffu);

	buf->DbgDump();

	auto* marker = buf->AllocateDW(2);
	marker[0]    = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO);
	marker[1]    = 0x6875000d;

	auto* cmd = buf->AllocateDW(num_values + 2);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(num_values + 2, Pm4::IT_SET_SH_REG, 0u);
	cmd[1] = offset;

	if (values == nullptr)
	{
		memset(cmd + 2, 0, static_cast<size_t>(num_values) * 4);
	} else
	{
		memcpy(cmd + 2, values, static_cast<size_t>(num_values) * 4);
	}

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbSetShRegistersDirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs)
{
	PRINT_NAME();

	printf("\t buf      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buf));
	printf("\t regs     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
	printf("\t num_regs = %" PRIu32 "\n", num_regs);

	if (buf == nullptr || regs == nullptr || num_regs == 0 || num_regs > 4096)
	{
		return nullptr;
	}

	Vector<ShaderRegister> copied;
	for (uint32_t i = 0; i < num_regs; i++)
	{
		copied.Add(ShaderRegister {regs[i].offset, regs[i].value});
	}

	const uint32_t size_dw = GraphicsGetShRegistersPacketSize(copied.GetData(), copied.Size());
	if (size_dw == 0)
	{
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(size_dw);
	if (cmd == nullptr || GraphicsEncodeShRegisters(cmd, size_dw, copied.GetData(), copied.Size()) != size_dw)
	{
		return nullptr;
	}

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbDispatch(CommandBuffer* buf, uint32_t group_x, uint32_t group_y, uint32_t group_z, uint32_t modifier)
{
	PRINT_NAME();

	if (buf == nullptr)
	{
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(5);
	if (cmd == nullptr || GraphicsEncodeDispatch(cmd, 5, group_x, group_y, group_z, modifier) != 5)
	{
		return nullptr;
	}

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbReleaseMem(CommandBuffer* buf, uint8_t action, uint16_t gcr_cntl, uint8_t dst, uint8_t cache_policy,
                                             const volatile Label* address, uint8_t data_sel, uint64_t data, uint16_t gds_offset,
                                             uint16_t gds_size, uint8_t interrupt, uint32_t interrupt_ctx_id)
{
	PRINT_NAME();

	printf("\t action           = 0x%02" PRIx8 "\n", action);
	printf("\t gcr_cntl         = 0x%04" PRIx16 "\n", gcr_cntl);
	printf("\t dst              = %" PRIu8 "\n", dst);
	printf("\t cache_policy     = 0x%02" PRIx8 "\n", cache_policy);
	printf("\t address          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	printf("\t data_sel         = 0x%02" PRIx8 "\n", data_sel);
	printf("\t data             = 0x%016" PRIx64 "\n", data);
	printf("\t gds_offset       = %" PRIu16 "\n", gds_offset);
	printf("\t gds_size         = %" PRIu16 "\n", gds_size);
	printf("\t interrupt        = 0x%02" PRIx8 "\n", interrupt);
	printf("\t interrupt_ctx_id = %" PRIu32 "\n", interrupt_ctx_id);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	// dst: 0 = memory, 1 = TC_L2 (Gen5 AGC).
	EXIT_NOT_IMPLEMENTED(dst > 1);
	// data_sel: 0 = no destination write (barrier/flush only), 1 = 32-bit
	// immediate, 2 = 64-bit immediate, 3 = GPU clock counter. Packet layout
	// stores data in DW5/DW6 for write forms; data_sel 0 uses the same custom
	// envelope. CP custom R_RELEASE_MEM already accepts 0..3.
	EXIT_NOT_IMPLEMENTED(data_sel != 0 && data_sel != 1 && data_sel != 2 && data_sel != 3);
	EXIT_NOT_IMPLEMENTED(gds_offset != 0);
	// Non-GDS forms do not encode GDS fields. Guests pass gds_size 0 (unused),
	// 1 (legacy default), or 2 (observed Gen5).
	EXIT_NOT_IMPLEMENTED(gds_size > 2);
	// interrupt selector is a small enum (0 = none). Non-zero values are
	// packed into the control dword; the CP may still treat clock/immediate
	// writes as non-interrupting label publishes.
	EXIT_NOT_IMPLEMENTED(interrupt > 3);
	EXIT_NOT_IMPLEMENTED((interrupt_ctx_id & ~0x07ffffffu) != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(8);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);
	cmd[1] = action | (static_cast<uint32_t>(cache_policy) << 8u);
	cmd[2] = gcr_cntl | (static_cast<uint32_t>(data_sel) << 16u) | (static_cast<uint32_t>(interrupt) << 24u);
	cmd[3] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(address) & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>((reinterpret_cast<uint64_t>(address) >> 32u) & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>(data & 0xffffffffu);
	cmd[6] = static_cast<uint32_t>((data >> 32u) & 0xffffffffu);
	cmd[7] = interrupt_ctx_id & 0x07ffffffu;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbResetQueue(CommandBuffer* buf, uint32_t op, uint32_t state)
{
	PRINT_NAME();

	printf("\t op    = 0x%08" PRIx32 "\n", op);
	printf("\t state = 0x%08" PRIx32 "\n", state);

	// Gen5 sce::Agc::DrawCommandBuffer::resetQueue: 12-bit op mask and a small
	// state selector. Emit IT_CLEAR_STATE with the low 4 bits of state.
	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED((op & ~0xfffu) != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(2);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(2, Pm4::IT_CLEAR_STATE, 0u);
	cmd[1] = state & 0xfu;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbWaitUntilSafeForRendering(CommandBuffer* buf, uint32_t video_out_handle, uint32_t display_buffer_index)
{
	PRINT_NAME();

	printf("\t video_out_handle     = %" PRIu32 "\n", video_out_handle);
	printf("\t display_buffer_index = %" PRIu32 "\n", display_buffer_index);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(7);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE);
	cmd[1] = video_out_handle;
	cmd[2] = display_buffer_index;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetShRegisterDirect(CommandBuffer* buf, ShaderRegister reg)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(3, Pm4::IT_SET_SH_REG, 0u);
	cmd[1] = reg.offset;
	cmd[2] = reg.value;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetCxRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs)
{
	PRINT_NAME();

	printf("\t regs     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
	printf("\t num_regs = %" PRIu32 "\n", num_regs);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(4);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	cmd[0] = KYTY_PM4(4, Pm4::IT_NOP, Pm4::R_CX_REGS_INDIRECT);
	cmd[1] = num_regs;
	cmd[2] = vaddr & 0xffffffffu;
	cmd[3] = (vaddr >> 32u) & 0xffffffffu;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetShRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs)
{
	PRINT_NAME();

	printf("\t regs     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
	printf("\t num_regs = %" PRIu32 "\n", num_regs);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(4);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	cmd[0] = KYTY_PM4(4, Pm4::IT_NOP, Pm4::R_SH_REGS_INDIRECT);
	cmd[1] = num_regs;
	cmd[2] = vaddr & 0xffffffffu;
	cmd[3] = (vaddr >> 32u) & 0xffffffffu;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetUcRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs)
{
	PRINT_NAME();

	printf("\t regs     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
	printf("\t num_regs = %" PRIu32 "\n", num_regs);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(4);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto vaddr = reinterpret_cast<uint64_t>(regs);

	cmd[0] = KYTY_PM4(4, Pm4::IT_NOP, Pm4::R_UC_REGS_INDIRECT);
	cmd[1] = num_regs;
	cmd[2] = vaddr & 0xffffffffu;
	cmd[3] = (vaddr >> 32u) & 0xffffffffu;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexSize(CommandBuffer* buf, uint8_t index_size, uint8_t cache_policy)
{
	PRINT_NAME();

	printf("\t index_size   = 0x%" PRIx8 "\n", index_size);
	printf("\t cache_policy = 0x%" PRIx8 "\n", cache_policy);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(cache_policy != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(2);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(2, Pm4::IT_INDEX_TYPE, 0u);
	cmd[1] = index_size;

	return cmd;
}

static uint32_t decode_draw_index_initiator(uint64_t modifier)
{
	if ((modifier & (1ull << 32u)) != 0)
	{
		return 0;
	}

	return (static_cast<uint32_t>(modifier) >> 3u) & 0x20u;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexAuto(CommandBuffer* buf, uint32_t index_count, uint64_t modifier)
{
	PRINT_NAME();

	printf("\t index_count = 0x%" PRIx32 "\n", index_count);
	printf("\t modifier    = 0x%016" PRIx64 "\n", modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	// Observed Gen5 modifiers: 0x40000000 (default) and 0x80000000 (Astro
	// post-compute path). Other bits remain unsupported until evidenced.
	EXIT_NOT_IMPLEMENTED(modifier != 0x40000000ull && modifier != 0x80000000ull);

	buf->DbgDump();

	// IT_DRAW_INDEX_AUTO consumes the decoded draw initiator, not the AGC
	// modifier itself. Both observed Gen5 modifiers currently decode to the
	// standard auto-draw initiator value 2.
	auto* cmd = buf->AllocateDW(3);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(3, Pm4::IT_DRAW_INDEX_AUTO, 0u);
	cmd[1] = index_count;
	cmd[2] = decode_draw_index_initiator(modifier) | 0x2u;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexAutoWithBase(CommandBuffer* buf, uint32_t base_vertex, uint32_t index_count,
                                                         uint64_t modifier)
{
	PRINT_NAME();

	printf("\t base_vertex = 0x%" PRIx32 "\n", base_vertex);
	printf("\t index_count = 0x%" PRIx32 "\n", index_count);
	printf("\t modifier    = 0x%016" PRIx64 "\n", modifier);

	// Captured Dreaming Sarah: base_vertex is always 0. Non-zero base needs a
	// separate PM4 encoding before it can be accepted.
	EXIT_NOT_IMPLEMENTED(base_vertex != 0);

	return GraphicsDcbDrawIndexAuto(buf, index_count, modifier);
}

// sceAgcDcbDrawIndexOffset — NID B+aG9DUnTKA.
// Packet layout (5 DW): header, index_count, index_offset, index_count, flags&0xE0000001.
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexOffset(CommandBuffer* buf, uint32_t index_offset, uint32_t index_count, uint32_t flags)
{
	PRINT_NAME();

	printf("\t index_offset = 0x%" PRIx32 "\n", index_offset);
	printf("\t index_count  = 0x%" PRIx32 "\n", index_count);
	printf("\t flags        = 0x%" PRIx32 "\n", flags);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(5);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(5, Pm4::IT_DRAW_INDEX_OFFSET_2, 0u);
	cmd[1] = index_count;
	cmd[2] = index_offset;
	cmd[3] = index_count;
	cmd[4] = flags & 0xE0000001u;

	return cmd;
}

// AGC indexed draw: sceAgcDcbDrawIndex(dcb, index_count, index_addr, modifier).
// Emits the same R_DRAW_INDEX custom PM4 packet (header 0xC008100C, 9 DW) that the
// command-processor's cp_op_draw_index handler consumes.
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndex(CommandBuffer* buf, uint32_t index_count, const void* index_addr, uint64_t modifier)
{
	PRINT_NAME();

	printf("\t index_count = 0x%" PRIx32 "\n", index_count);
	printf("\t index_addr  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
	printf("\t modifier    = 0x%016" PRIx64 "\n", modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	// Header 0xC008100C == KYTY_PM4(10, IT_NOP, R_DRAW_INDEX): 10 DW total.
	auto* cmd = buf->AllocateDW(10);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto addr = reinterpret_cast<uint64_t>(index_addr);

	cmd[0] = KYTY_PM4(10, Pm4::IT_NOP, Pm4::R_DRAW_INDEX);
	cmd[1] = index_count;
	cmd[2] = static_cast<uint32_t>(addr & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((addr >> 32u) & 0xffffffffu);
	cmd[4] = 0; // flags
	cmd[5] = 1; // type (indexed)
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbEventWrite(CommandBuffer* buf, uint8_t event_type, const volatile void* address)
{
	PRINT_NAME();

	printf("\t event_type = 0x%02" PRIx8 "\n", event_type);
	printf("\t address    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(address != nullptr);
	EXIT_NOT_IMPLEMENTED(event_type > 0x3fu);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(2);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	uint32_t event_index = 0;

	cmd[0] = KYTY_PM4(2, Pm4::IT_EVENT_WRITE, 0u);
	cmd[1] = (event_index << 8u) | event_type;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbStallCommandBufferParser(CommandBuffer* buf)
{
	// GNM/AGC stallCommandBufferParser: fixed EVENT_WRITE CS partial flush (0x07).
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	buf->DbgDump();
	auto* cmd = buf->AllocateDW(2);
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);
	constexpr uint32_t kCsPartialFlush = 0x07u;
	cmd[0]                             = KYTY_PM4(2, Pm4::IT_EVENT_WRITE, 0u);
	cmd[1]                             = kCsPartialFlush;
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDmaData(CommandBuffer* buf, uint8_t destination, uint8_t destination_cache_policy, uint8_t source,
                                           uint64_t destination_address, uint8_t source_cache_policy, uint8_t control4,
                                           uint64_t source_address, uint32_t byte_count, uint8_t control7, uint8_t control8,
                                           uint8_t control9)
{
	// sceAgcDcbDmaData / sceAgcAcbDmaData custom R_DMA_DATA packet layout.
	PRINT_NAME();
	printf("\t destination              = 0x%02" PRIx8 "\n", destination);
	printf("\t destination_cache_policy = 0x%02" PRIx8 "\n", destination_cache_policy);
	printf("\t source                   = 0x%02" PRIx8 "\n", source);
	printf("\t destination_address      = 0x%016" PRIx64 "\n", destination_address);
	printf("\t source_cache_policy      = 0x%02" PRIx8 "\n", source_cache_policy);
	printf("\t source_address           = 0x%016" PRIx64 "\n", source_address);
	printf("\t byte_count               = %" PRIu32 "\n", byte_count);

	if (buf == nullptr || byte_count == 0 || (byte_count & 3u) != 0)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(8);
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_DMA_DATA);
	cmd[1] = static_cast<uint32_t>(destination) | (static_cast<uint32_t>(destination_cache_policy) << 8u) |
	         (static_cast<uint32_t>(source) << 16u) | (static_cast<uint32_t>(source_cache_policy) << 24u);
	cmd[2] = static_cast<uint32_t>(control4) | (static_cast<uint32_t>(control7) << 8u) | (static_cast<uint32_t>(control8) << 16u) |
	         (static_cast<uint32_t>(control9) << 24u);
	cmd[3] = byte_count;
	cmd[4] = static_cast<uint32_t>(destination_address & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>((destination_address >> 32u) & 0xffffffffu);
	cmd[6] = static_cast<uint32_t>(source_address & 0xffffffffu);
	cmd[7] = static_cast<uint32_t>((source_address >> 32u) & 0xffffffffu);
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbAcquireMem(CommandBuffer* buf, uint8_t engine, uint32_t cb_db_op, uint32_t gcr_cntl,
                                              const volatile void* base, uint64_t size_bytes, uint32_t poll_cycles)
{
	PRINT_NAME();

	printf("\t engine      = 0x%02" PRIx8 "\n", engine);
	printf("\t cb_db_op    = 0x%08" PRIx32 "\n", cb_db_op);
	printf("\t gcr_cntl    = 0x%08" PRIx32 "\n", gcr_cntl);
	printf("\t base        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(base));
	printf("\t size_bytes  = 0x%016" PRIx64 "\n", size_bytes);
	printf("\t poll_cycles = %" PRIu32 "\n", poll_cycles);

	bool no_size = (static_cast<int64_t>(size_bytes) == -1);
	auto vaddr   = reinterpret_cast<uint64_t>(base);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	// AGC ver 10 issues ACQUIRE_MEM (GCR cache ops) with finer-than-256B granularity;
	// the PM4 encoding is size>>8 / addr>>8, so sub-256B bits are dropped. That is
	// acceptable for our coherency model — warn instead of aborting.
	if (!no_size && (size_bytes & 0xffu) != 0)
	{
		printf("\t WARNING: ACQUIRE_MEM size 0x%" PRIx64 " not 256B-aligned\n", size_bytes);
	}
	EXIT_NOT_IMPLEMENTED(!no_size && (size_bytes >> 40u) != 0);
	if ((vaddr & 0xffu) != 0)
	{
		printf("\t WARNING: ACQUIRE_MEM base 0x%" PRIx64 " not 256B-aligned\n", vaddr);
	}
	EXIT_NOT_IMPLEMENTED((vaddr >> 40u) != 0);
	EXIT_NOT_IMPLEMENTED(engine > 1);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(8);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_ACQUIRE_MEM);
	cmd[1] = (static_cast<uint32_t>(engine) << 31u) | cb_db_op;
	cmd[2] = (no_size ? 0 : size_bytes >> 8u);
	cmd[3] = 0;
	cmd[4] = vaddr >> 8u;
	cmd[5] = 0;
	cmd[6] = poll_cycles / 40;
	cmd[7] = gcr_cntl;

	return cmd;
}

// Gen5 NID qj7QZpgr9Uw: append a single Type-2 PM4 pad dword (0x80000000).
// Observed after compute/context setup; CP treats Type-2 as header-only filler.
uint32_t* KYTY_SYSV_ABI GraphicsCbType2Pad(CommandBuffer* buf)
{
	PRINT_NAME();

	if (buf == nullptr)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(1);
	if (cmd == nullptr)
	{
		return nullptr;
	}
	cmd[0] = 0x80000000u;
	return cmd;
}

// sceAgcDcbSetBaseIndirectArgs: IT_SET_BASE for indirect argument buffers.
uint32_t* KYTY_SYSV_ABI GraphicsDcbSetBaseIndirectArgs(CommandBuffer* buf, uint32_t base_index, uint64_t address)
{
	PRINT_NAME();

	printf("\t base_index = %" PRIu32 "\n", base_index);
	printf("\t address    = 0x%016" PRIx64 "\n", address);

	if (buf == nullptr)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(4);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	cmd[0] = KYTY_PM4(4, Pm4::IT_SET_BASE, 0u) | ((base_index & 0x1u) << 1u);
	cmd[1] = 1u;
	cmd[2] = static_cast<uint32_t>(address & ~7ull);
	cmd[3] = static_cast<uint32_t>(address >> 32u);
	return cmd;
}

// sceAgcDcbDispatchIndirect: IT_DISPATCH_INDIRECT from SetBaseIndirect args.
uint32_t* KYTY_SYSV_ABI GraphicsDcbDispatchIndirect(CommandBuffer* buf, uint32_t data_offset, uint32_t modifier)
{
	PRINT_NAME();

	printf("\t data_offset = %" PRIu32 "\n", data_offset);
	printf("\t modifier    = 0x%08" PRIx32 "\n", modifier);

	if (buf == nullptr)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(3);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	cmd[0] = KYTY_PM4(3, Pm4::IT_DISPATCH_INDIRECT, 0u);
	cmd[1] = data_offset;
	cmd[2] = (modifier & 0xa038u) | 0x41u;
	return cmd;
}

static uint32_t extract_modifier_bits(uint32_t modifier, uint32_t start, uint32_t count)
{
	return (modifier >> start) & ((1u << count) - 1u);
}

static uint32_t indirect_modifier_sgpr_base(uint32_t modifier)
{
	const auto stage = modifier >> 29u;
	return ((stage == 3u || stage == 5u) ? 0x80u : 0u) + 0x8cu;
}

static uint64_t decode_indirect_modifier_patch_offsets(uint64_t modifier, bool indexed)
{
	const auto low       = static_cast<uint32_t>(modifier);
	const auto sgpr_base = indirect_modifier_sgpr_base(low);

	uint64_t base_vtx_loc = 0x280u;
	if ((low & 0x1u) != 0)
	{
		base_vtx_loc = sgpr_base + extract_modifier_bits(low, 9u, 5u);
	}

	uint64_t start_inst_loc = 0x280u;
	if ((low & 0x4u) != 0)
	{
		start_inst_loc = sgpr_base + extract_modifier_bits(low, 19u, 5u);
	}

	if (indexed && (low & 0x2u) != 0)
	{
		base_vtx_loc |= static_cast<uint64_t>(sgpr_base + extract_modifier_bits(low, 14u, 5u)) << 16u;
		base_vtx_loc |= 1ull << 59u;
	}

	return base_vtx_loc | (start_inst_loc << 32u);
}

static uint32_t decode_indirect_draw_initiator(uint64_t modifier)
{
	const auto low       = static_cast<uint32_t>(modifier);
	uint32_t   initiator = 2u;

	if ((modifier & (1ull << 32u)) == 0)
	{
		initiator = ((low >> 3u) & 0x20u) | 2u;
	}

	return initiator;
}

// sceAgcDcbDrawIndexIndirect: IT_DRAW_INDEX_INDIRECT from SetBaseIndirect args.
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexIndirect(CommandBuffer* buf, uint32_t data_offset_in_bytes, uint64_t modifier)
{
	PRINT_NAME();

	printf("\t data_offset = 0x%" PRIx32 "\n", data_offset_in_bytes);
	printf("\t modifier    = 0x%016" PRIx64 "\n", modifier);

	if (buf == nullptr)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(5);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	const auto patch_offsets = decode_indirect_modifier_patch_offsets(modifier, true);

	cmd[0] = KYTY_PM4(5, Pm4::IT_DRAW_INDEX_INDIRECT, 0u);
	cmd[1] = data_offset_in_bytes;
	cmd[2] = static_cast<uint32_t>(patch_offsets);
	cmd[3] = static_cast<uint32_t>(patch_offsets >> 32u);
	cmd[4] = decode_indirect_draw_initiator(modifier);
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy, uint64_t address_or_offset,
                                             const void* data, uint32_t num_dwords, uint8_t increment, uint8_t write_confirm)
{
	PRINT_NAME();

	printf("\t dst               = 0x%02" PRIx8 "\n", dst);
	printf("\t cache_policy      = 0x%02" PRIx8 "\n", cache_policy);
	printf("\t address_or_offset = 0x%016" PRIx64 "\n", address_or_offset);
	printf("\t data              = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	printf("\t num_dwords        = %" PRIu32 "\n", num_dwords);
	printf("\t increment         = 0x%02" PRIx8 "\n", increment);
	printf("\t write_confirm     = 0x%02" PRIx8 "\n", write_confirm);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED((4 + num_dwords - 2u) > 0x3fffu);
	EXIT_NOT_IMPLEMENTED(data == nullptr);
	// address_or_offset may be 0: Gen5 reserves the packet then patches the
	// destination with GraphicsWriteDataPatchSetAddressOrOffset before submit.

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(4 + num_dwords);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(4 + num_dwords, Pm4::IT_NOP, Pm4::R_WRITE_DATA);
	cmd[1] = dst | (static_cast<uint32_t>(cache_policy) << 8u) | (static_cast<uint32_t>(increment) << 16u) |
	         (static_cast<uint32_t>(write_confirm) << 24u);
	cmd[2] = address_or_offset & 0xffffffffu;
	cmd[3] = (address_or_offset >> 32u) & 0xffffffffu;

	memcpy(cmd + 4, data, static_cast<size_t>(num_dwords) * 4);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbWaitRegMem(CommandBuffer* buf, uint8_t size, uint8_t compare_function, uint8_t op, uint8_t cache_policy,
                                              const volatile void* address, uint64_t reference, uint64_t mask, uint32_t poll_cycles)
{
	PRINT_NAME();

	printf("\t size             = 0x%02" PRIx8 "\n", size);
	printf("\t compare_function = 0x%02" PRIx8 "\n", compare_function);
	printf("\t op               = 0x%02" PRIx8 "\n", op);
	printf("\t cache_policy     = 0x%02" PRIx8 "\n", cache_policy);
	printf("\t address          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	printf("\t reference        = 0x%016" PRIx64 "\n", reference);
	printf("\t mask             = 0x%016" PRIx64 "\n", mask);
	printf("\t poll_cycles      = %" PRIu32 "\n", poll_cycles);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	// size 0 = 32-bit memory wait (observed after ReleaseMem data_sel=1),
	// size 1 = 64-bit wait (existing path). Encode 32-bit waits with zeroed
	// high halves in the same R_WAIT_MEM_64 packet layout the CP already runs.
	EXIT_NOT_IMPLEMENTED(size != 0 && size != 1);
	// op 0/1: observed memory-wait forms (op=0 size=0 cmp=3 ref=1 mask=~0 after
	// ReleaseMem data_sel=1; op=1 was the prior path). Packet encoding is shared.
	EXIT_NOT_IMPLEMENTED(op != 0 && op != 1);
	// cache_policy is a builder-side GPU-cache hint. The normalized R_WAIT_MEM_64
	// packet has no policy field; host CP polls guest memory the same regardless.
	// Observed values: 0/1/2 (WriteData-era) and 0x03 on early Gen5 WaitRegMem.
	EXIT_NOT_IMPLEMENTED(cache_policy > 3);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(9);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	// The host CP has one normalized 64-bit packet form. A guest size=0 wait
	// still has 32-bit comparison semantics, so its upper mask/reference
	// dwords must not participate in that normalized comparison.
	const uint64_t effective_mask      = (size == 0 ? (mask & 0xffffffffull) : mask);
	const uint64_t effective_reference = (size == 0 ? (reference & 0xffffffffull) : reference);
	cmd[0] = KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64);
	cmd[1] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(address) & 0xffffffffu);
	cmd[2] = static_cast<uint32_t>((reinterpret_cast<uint64_t>(address) >> 32u) & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>(effective_mask & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>((effective_mask >> 32u) & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>(effective_reference & 0xffffffffu);
	cmd[6] = static_cast<uint32_t>((effective_reference >> 32u) & 0xffffffffu);
	cmd[7] = compare_function;
	cmd[8] = poll_cycles / 40;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbWaitRegMem(CommandBuffer* buf, uint8_t size, uint8_t compare_function, uint8_t cache_policy,
                                              const volatile void* address, uint64_t reference, uint64_t mask, uint32_t poll_cycles)
{
	// Gen5 ACB WaitRegMem omits the DCB `op` argument; encode as op=0.
	return GraphicsDcbWaitRegMem(buf, size, compare_function, 0, cache_policy, address, reference, mask, poll_cycles);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbEventWrite(CommandBuffer* buf, uint8_t event_type, const volatile void* address)
{
	return GraphicsDcbEventWrite(buf, event_type, address);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbWriteData(CommandBuffer* buf, uint8_t dst, uint8_t cache_policy, uint64_t address_or_offset,
                                             const void* data, uint32_t num_dwords, uint8_t increment, uint8_t write_confirm)
{
	return GraphicsDcbWriteData(buf, dst, cache_policy, address_or_offset, data, num_dwords, increment, write_confirm);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbAcquireMem(CommandBuffer* buf, uint32_t gcr_cntl, const volatile void* base, uint64_t size_bytes,
                                              uint32_t poll_cycles)
{
	// ACB form fixes engine=1 (ME) and cb_db_op=0.
	return GraphicsDcbAcquireMem(buf, 1, 0, gcr_cntl, base, size_bytes, poll_cycles);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbResetQueue(CommandBuffer* buf, uint32_t op)
{
	PRINT_NAME();
	printf("\t op = 0x%08" PRIx32 "\n", op);

	if (buf == nullptr || (op & ~0x1c2u) != 0)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(2);
	if (cmd == nullptr)
	{
		return nullptr;
	}
	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DISPATCH_RESET);
	cmd[1] = 0;
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbCopyData(CommandBuffer* buf, uint8_t dst, uint8_t dst_cache_policy, uint64_t dst_address, uint8_t src,
                                            uint8_t src_cache_policy, uint64_t src_address_or_immediate, uint8_t item_size,
                                            uint8_t write_confirm)
{
	PRINT_NAME();
	printf("\t dst=0x%02" PRIx8 " src=0x%02" PRIx8 " dst_addr=0x%016" PRIx64 " src=0x%016" PRIx64 " item=%u conf=%u\n", dst, src,
	       dst_address, src_address_or_immediate, item_size, write_confirm);

	if (buf == nullptr)
	{
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(6);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	cmd[0] = KYTY_PM4(6, Pm4::IT_COPY_DATA, 0u);
	cmd[1] = ((static_cast<uint32_t>(src) >> 1u) & 0xfu) | (((static_cast<uint32_t>(dst) >> 1u) & 0xfu) << 8u) |
	         ((static_cast<uint32_t>(src_cache_policy) & 0x3u) << 13u) | ((static_cast<uint32_t>(item_size) & 0x1u) << 16u) |
	         ((static_cast<uint32_t>(write_confirm) & 0x1u) << 20u) | ((static_cast<uint32_t>(dst_cache_policy) & 0x3u) << 25u) |
	         ((static_cast<uint32_t>(src) & 0x1u) << 30u);
	cmd[2] = static_cast<uint32_t>(src_address_or_immediate & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((src_address_or_immediate >> 32u) & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>(dst_address & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>((dst_address >> 32u) & 0xffffffffu);
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbCopyData(CommandBuffer* buf, uint8_t dst, uint8_t dst_cache_policy, uint64_t dst_address, uint8_t src,
                                            uint8_t src_cache_policy, uint64_t src_address_or_immediate, uint8_t item_size,
                                            uint8_t write_confirm)
{
	// ACB memory-src encoding uses src==5 for a shifted DCB form.
	const auto dcb_src = (src == 5 ? static_cast<uint8_t>(5u << 1u) : src);
	return GraphicsDcbCopyData(buf, dst, dst_cache_policy, dst_address, dcb_src, src_cache_policy, src_address_or_immediate, item_size,
	                           write_confirm);
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbPushMarker(CommandBuffer* buf, const char* str, uint32_t /*color*/)
{
	if (buf == nullptr)
	{
		return nullptr;
	}
	if (str == nullptr)
	{
		str = "";
	}

	const auto len            = strlen(str) + 1;
	const auto payload_dwords = static_cast<uint32_t>((len + 3) / 4);
	const auto size           = 1u + (payload_dwords == 0 ? 1u : payload_dwords);
	auto*      cmd            = buf->AllocateDW(size);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PUSH_MARKER);
	memset(cmd + 1, 0, static_cast<size_t>(size - 1) * sizeof(uint32_t));
	memcpy(cmd + 1, str, len);
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbPopMarker(CommandBuffer* buf)
{
	if (buf == nullptr)
	{
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(2);
	if (cmd == nullptr)
	{
		return nullptr;
	}
	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_POP_MARKER);
	cmd[1] = 0;
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color)
{
	return GraphicsDcbPushMarker(buf, str, color);
}

uint32_t* KYTY_SYSV_ABI GraphicsAcbPopMarker(CommandBuffer* buf)
{
	return GraphicsDcbPopMarker(buf);
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexBuffer(CommandBuffer* buf, uint64_t index_addr)
{
	PRINT_NAME();
	printf("\t index_addr = 0x%016" PRIx64 "\n", index_addr);

	if (buf == nullptr || (index_addr & 1u) != 0)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(3);
	if (cmd == nullptr)
	{
		return nullptr;
	}
	cmd[0] = KYTY_PM4(3, Pm4::IT_INDEX_BASE, 0u);
	cmd[1] = static_cast<uint32_t>(index_addr & 0xffffffffu);
	cmd[2] = static_cast<uint32_t>((index_addr >> 32u) & 0xffffffffu);
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetIndexCount(CommandBuffer* buf, uint32_t index_count)
{
	PRINT_NAME();
	printf("\t index_count = 0x%" PRIx32 "\n", index_count);

	if (buf == nullptr)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(2);
	if (cmd == nullptr)
	{
		return nullptr;
	}
	cmd[0] = KYTY_PM4(2, Pm4::IT_INDEX_BUFFER_SIZE, 0u);
	cmd[1] = index_count;
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetNumInstances(CommandBuffer* buf, uint32_t num_instances)
{
	PRINT_NAME();
	printf("\t num_instances = 0x%" PRIx32 "\n", num_instances);

	if (buf == nullptr)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(2);
	if (cmd == nullptr)
	{
		return nullptr;
	}
	cmd[0] = KYTY_PM4(2, Pm4::IT_NUM_INSTANCES, 0u);
	cmd[1] = num_instances;
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbGetLodStats(CommandBuffer* buf, uint8_t cache_policy, const volatile void* buffer,
                                               uint32_t buffer_size_in_bytes, uint32_t reset_count, uint8_t force_reset,
                                               uint8_t report_and_reset, uint32_t reporting_interval_in_100k_clocks)
{
	PRINT_NAME();
	printf("\t buffer = 0x%016" PRIx64 " size = %" PRIu32 "\n", reinterpret_cast<uint64_t>(buffer), buffer_size_in_bytes);

	if (buf == nullptr)
	{
		return nullptr;
	}

	buf->DbgDump();
	auto* cmd = buf->AllocateDW(5);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	const auto buffer_addr = reinterpret_cast<uint64_t>(buffer);
	cmd[0]                 = KYTY_PM4(5, Pm4::IT_GET_LOD_STATS, 0u);
	cmd[1]                 = buffer_size_in_bytes;
	cmd[2]                 = static_cast<uint32_t>(buffer_addr & 0xffffffc0u);
	cmd[3]                 = static_cast<uint32_t>((buffer_addr >> 32u) & 0xffffffffu);
	cmd[4]                 = ((static_cast<uint32_t>(cache_policy) & 0x3u) << 28u) |
	         ((static_cast<uint32_t>(report_and_reset) & 0x1u) << 19u) | ((static_cast<uint32_t>(force_reset) & 0x1u) << 18u) |
	         ((reset_count & 0xffu) << 10u) | ((reporting_interval_in_100k_clocks & 0xffu) << 2u);
	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetFlip(CommandBuffer* buf, uint32_t video_out_handle, int32_t display_buffer_index, uint32_t flip_mode,
                                           int64_t flip_arg)
{
	PRINT_NAME();

	printf("\t video_out_handle     = %" PRIu32 "\n", video_out_handle);
	printf("\t display_buffer_index = %" PRId32 "\n", display_buffer_index);
	printf("\t flip_mode            = %" PRIu32 "\n", flip_mode);
	printf("\t flip_arg             = %" PRId64 "\n", flip_arg);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(6);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_FLIP);
	cmd[1] = video_out_handle;
	cmd[2] = display_buffer_index;
	cmd[3] = flip_mode;
	cmd[4] = static_cast<uint32_t>(static_cast<uint64_t>(flip_arg) & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>((static_cast<uint64_t>(flip_arg) >> 32u) & 0xffffffffu);

	return cmd;
}

} // namespace Gen5

namespace Gen5Driver {

LIB_NAME("Graphics5Driver", "Graphics5Driver");

static Core::Mutex     g_resource_registration_mutex;
static Vector<uint32_t> g_registered_resources;

struct Packet
{
	uint32_t* addr;
	uint32_t  dw_num;
	uint8_t   pad[4];
};

int KYTY_SYSV_ABI GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(size_t* size, uint32_t max_resources,
                                                                                 uint32_t max_owners)
{
	if (size == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	constexpr size_t header_bytes       = 256;
	constexpr size_t resource_bytes     = 64;
	constexpr size_t owner_bytes        = 64;
	constexpr size_t required_alignment = 64;
	constexpr size_t max_size = std::numeric_limits<size_t>::max();
	auto checked_add_scaled = [](size_t current, uint32_t count, size_t stride, size_t* result) {
		if (count != 0 && stride > (std::numeric_limits<size_t>::max() - current) / static_cast<size_t>(count))
		{
			return false;
		}
		*result = current + static_cast<size_t>(count) * stride;
		return true;
	};

	size_t unaligned = header_bytes;
	if (!checked_add_scaled(unaligned, max_resources, resource_bytes, &unaligned) ||
	    !checked_add_scaled(unaligned, max_owners, owner_bytes, &unaligned) || unaligned > max_size - (required_alignment - 1))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	*size = (unaligned + required_alignment - 1) & ~(required_alignment - 1);
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverInitResourceRegistration(void* memory, size_t size, uint32_t max_owners)
{
	if (memory == nullptr || size == 0 || max_owners == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	static std::atomic<void*>    registration_memory {nullptr};
	static std::atomic<size_t>   registration_size {0};
	static std::atomic<uint32_t> registration_max_owners {0};
	registration_memory.store(memory, std::memory_order_release);
	registration_size.store(size, std::memory_order_release);
	registration_max_owners.store(max_owners, std::memory_order_release);
	return OK;
}

// Handle reserved for the driver's default resource owner. Named owners are
// allocated above it so their handles never collide with the default.
static constexpr uint32_t GRAPHICS_DEFAULT_OWNER = 1;

int KYTY_SYSV_ABI GraphicsDriverRegisterDefaultOwner(uint32_t options)
{
	if (options != 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	static std::atomic<bool> default_owner_registered {false};
	default_owner_registered.store(true, std::memory_order_release);
	return OK;
}

// sce::Agc::ResourceRegistration::getDefaultOwner(unsigned int*): returns the
// handle of the driver's default owner through the output pointer.
int KYTY_SYSV_ABI GraphicsDriverGetDefaultOwner(uint32_t* owner)
{
	if (owner == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	*owner = GRAPHICS_DEFAULT_OWNER;
	return OK;
}

// Maximum length, including the terminator, accepted for a registered resource
// name. Names passed to resource registration are bounded by this value.
static constexpr uint32_t GRAPHICS_RESOURCE_NAME_MAX = 256;

// sce::Agc::ResourceRegistration::getMaxNameLength(unsigned int*).
int KYTY_SYSV_ABI GraphicsDriverGetResourceRegistrationMaxNameLength(uint32_t* max_length)
{
	if (max_length == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	*max_length = GRAPHICS_RESOURCE_NAME_MAX;
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverRegisterOwner(uint32_t* owner, const char* name)
{
	if (owner == nullptr || name == nullptr || name[0] == '\0')
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	static std::atomic<uint32_t> next_owner {GRAPHICS_DEFAULT_OWNER + 1};
	*owner = next_owner.fetch_add(1, std::memory_order_relaxed);
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverRegisterResource(uint32_t* resource, uint32_t owner, const void* base, uint64_t size, const char* name,
                                                 uint32_t /*type*/, uint64_t /*user_data*/)
{
	if (resource == nullptr || owner == 0 || base == nullptr || size == 0 || name == nullptr || name[0] == '\0')
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	static std::atomic<uint32_t> next_resource {1};
	*resource = next_resource.fetch_add(1, std::memory_order_relaxed);
	{
		Core::LockGuard lock(g_resource_registration_mutex);
		g_registered_resources.Add(*resource);
	}
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverUnregisterResource(uint32_t resource)
{
	if (resource == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	Core::LockGuard lock(g_resource_registration_mutex);
	const auto      index = g_registered_resources.Find(resource);
	if (!g_registered_resources.IndexValid(index))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	g_registered_resources.RemoveAt(index);
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSubmitDcb(const Packet* packet)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(packet == nullptr);
	EXIT_NOT_IMPLEMENTED(packet->pad[0] != 0);

	printf("\t addr   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(packet->addr));
	printf("\t dw_num = 0x%08" PRIx32 "\n", packet->dw_num);

	GraphicsDbgDumpDcb("d", packet->dw_num, packet->addr);

	GraphicsRunSubmit(packet->addr, packet->dw_num, nullptr, 0);

	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSubmitAcb(uint32_t queue, const Packet* packet)
{
	PRINT_NAME();
	printf("\t queue  = 0x%08" PRIx32 "\n", queue);
	printf("\t packet = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(packet));

	if (packet == nullptr)
	{
		return OK;
	}

	printf("\t acb    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(packet->addr));
	printf("\t dw_num = 0x%08" PRIx32 "\n", packet->dw_num);

	// Queue-indexed compute submit is not fully modeled yet. Execute the ACB
	// through the existing command processor so WaitRegMem/ReleaseMem packets
	// still complete guest labels rather than stalling on an empty GPU path.
	(void)queue;
	if (packet->addr != nullptr && packet->dw_num != 0)
	{
		GraphicsDbgDumpDcb("a", packet->dw_num, packet->addr);
		GraphicsRunSubmit(packet->addr, packet->dw_num, nullptr, 0);
	}
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata)
{
	PRINT_NAME();
	if (eq == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EBADF;
	}
	return GraphicsRenderAddEqEvent(eq, id, udata);
}

} // namespace Gen5Driver

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
