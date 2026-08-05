#include "Emulator/Graphics/Graphics.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/GuestMemory.h"
#include "Emulator/GpuMemoryFault.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/RenderResolutionCoordinator.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/GuestTextureLayout.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/GpuMappingLifecycle.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/TimePort.h"
#include "Emulator/Hle/Registration.h"
#include "Emulator/Log.h"
#include "Emulator/PresentationStats.h"
#include "Emulator/VideoFrameMemory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

using ::Kyty::Kernel::OK;

namespace {

struct GpuMappingReleaseTransaction
{
	uint64_t                         vaddr           = 0;
	uint64_t                         size            = 0;
	Kernel::Memory::KernelGpuMappingCompletion completion = nullptr;
	void*                            completion_data = nullptr;
};

void GraphicsRegisterGpuMappingRange(void* context, uint64_t vaddr, uint64_t size)
{
	(void)context;
	GpuMemorySetAllocatedRange(vaddr, size);
}

bool GraphicsCompleteGpuMappingRelease(void* data)
{
	EXIT_IF(data == nullptr);

	auto* transaction = static_cast<GpuMappingReleaseTransaction*>(data);
	return VideoOut::VideoOutRunBufferUnmapTransaction(
	    transaction->vaddr, transaction->size,
	    [](void* action_data)
	    {
		    EXIT_IF(action_data == nullptr);
		    auto* transaction = static_cast<GpuMappingReleaseTransaction*>(action_data);
		    GpuMemoryFreeMappedRangeQuiesced(WindowGetGraphicContext(), transaction->vaddr, transaction->size);
		    return transaction->completion(transaction->completion_data);
	    },
	    transaction);
}

bool GraphicsReleaseGpuMappingRange(void* context, uint64_t vaddr, uint64_t size,
	                                Kernel::Memory::KernelGpuMappingCompletion completion, void* completion_data)
{
	(void)context;
	if (vaddr == 0 || size == 0 || vaddr > std::numeric_limits<uint64_t>::max() - size || completion == nullptr)
	{
		return false;
	}

	GpuMappingReleaseTransaction transaction {vaddr, size, completion, completion_data};
	return GraphicsRunWithQuiescedSubmissions(GraphicsCompleteGpuMappingRelease, &transaction);
}

bool GraphicsQueryPresentationStats(void* context, Kyty::Emulator::PresentationStats::Snapshot* out)
{
	(void)context;
	return WindowGetPresentStats(out);
}

bool GraphicsQueryMappedRange(uint64_t address, uint64_t size, Kyty::Emulator::GuestMemory::MappedRange* out)
{
	if (out == nullptr)
	{
		return false;
	}

	Kernel::Memory::KernelMappedRange mapped {};
	if (!Kernel::Memory::KernelQueryMappedRange(address, size, &mapped))
	{
		return false;
	}

	switch (mapped.kind)
	{
		case Kernel::Memory::KernelMappedRangeKind::None: out->kind = Kyty::Emulator::GuestMemory::MappedRangeKind::None; break;
		case Kernel::Memory::KernelMappedRangeKind::Physical: out->kind = Kyty::Emulator::GuestMemory::MappedRangeKind::Physical; break;
		case Kernel::Memory::KernelMappedRangeKind::Flexible: out->kind = Kyty::Emulator::GuestMemory::MappedRangeKind::Flexible; break;
	}
	out->base = mapped.base;
	out->size = mapped.size;
	return true;
}

int GraphicsQueryProtection(void* address, void** start, void** end, int* protection)
{
	return Kernel::Memory::KernelQueryMemoryProtection(address, start, end, protection);
}

} // namespace

void GraphicsSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	auto width  = Config::GetScreenWidth();
	auto height = Config::GetScreenHeight();
	const auto mode = Config::GetRenderResolutionMode() == Config::RenderResolutionMode::Native ? RenderResolutionMode::Native
	                                                                                            : RenderResolutionMode::Fixed;
	const auto resolution_status = RenderResolutionInitialize(
	    mode, {Config::GetRenderResolutionWidth(), Config::GetRenderResolutionHeight()});
	EXIT_IF(resolution_status != ResolutionPolicyStatus::Success);

	WindowInit(width, height);
	VideoOut::VideoOutInit(width, height);
	GraphicsRenderInit();
	GraphicsRunInit();
	GpuMemoryInit();
	const Kyty::Emulator::GpuMemoryFault::Callbacks gpu_memory_fault_callbacks {
	    GpuMemoryCheckAccessViolation,
	    GpuDirtyPageTrackerNotifyFaultHandlerInstalled,
	};
	EXIT_IF(!Kyty::Emulator::GpuMemoryFault::GetPort().Install(gpu_memory_fault_callbacks));
	const Kernel::Memory::GpuMappingLifecycleCallbacks gpu_mapping_lifecycle_callbacks {
	    nullptr,
	    GraphicsRegisterGpuMappingRange,
	    GraphicsReleaseGpuMappingRange,
	};
	EXIT_IF(!Kernel::Memory::GetGpuMappingLifecyclePort().Install(gpu_mapping_lifecycle_callbacks));
	const Kyty::Emulator::GuestMemory::Callbacks guest_memory_callbacks {GraphicsQueryMappedRange, GraphicsQueryProtection};
	EXIT_IF(!Kyty::Emulator::GuestMemory::GetPort().Install(guest_memory_callbacks));
	const Kyty::Emulator::VideoFrameMemory::Callbacks video_frame_memory_callbacks {
	    GuestTextureLayoutRegisterLinear,
	    GuestTextureLayoutUnregister,
	    [](uint64_t base, uint64_t size) { (void)GpuMemoryNotifyHostWrite(base, size); }};
	EXIT_IF(!Kyty::Emulator::VideoFrameMemory::InstallCallbacks(video_frame_memory_callbacks));
	const Kyty::Emulator::PresentationStats::Callbacks presentation_stats_callbacks {nullptr, GraphicsQueryPresentationStats};
	EXIT_IF(!Kyty::Emulator::PresentationStats::GetPort().Install(presentation_stats_callbacks));
	// These adapters are process-lifetime functions. Keep them installed so
	// Audio can unregister frames during teardown without depending on a
	// graphics-shutdown ordering edge.
	LabelInit();
	TileInit();
	IndexBufferInit();
	ShaderInit();
}

void GraphicsSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void GraphicsSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

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
			KYTY_LOG_WARN(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, file_name.C_Str());
			return;
		}
		Pm4::DumpPm4PacketStream(&f, cmd_buffer, 0, num_dw);
		f.Close();
	}
}

namespace Gen4 {

LIB_NAME("GraphicsDriver", "GraphicsDriver");

int KYTY_SYSV_ABI GraphicsSetVsShader(uint32_t* cmd, uint64_t size, const uint32_t* vs_regs, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < sizeof(HW::VsStageRegisters) / 4 + 2);

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

			KYTY_LOG_DEBUG("\t m_spiShaderPgmLoVs    = %08" PRIx32 "\n", vs_regs[0]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmHiVs    = %08" PRIx32 "\n", vs_regs[1]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc1Vs = %08" PRIx32 "\n", vs_regs[2]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc2Vs = %08" PRIx32 "\n", vs_regs[3]);
			KYTY_LOG_DEBUG("\t m_spiVsOutConfig      = %08" PRIx32 "\n", vs_regs[4]);
			KYTY_LOG_DEBUG("\t m_spiShaderPosFormat  = %08" PRIx32 "\n", vs_regs[5]);
			KYTY_LOG_DEBUG("\t m_paClVsOutCntl       = %08" PRIx32 "\n", vs_regs[6]);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_VS);
	cmd[1] = shader_modifier;
	memcpy(&cmd[2], vs_regs, static_cast<size_t>(7) * 4);

	return OK;
}

int KYTY_SYSV_ABI GraphicsUpdateVsShader(uint32_t* cmd, uint64_t size, const uint32_t* vs_regs, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < sizeof(HW::VsStageRegisters) / 4 + 2);

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

			KYTY_LOG_DEBUG("\t m_spiShaderPgmLoVs    = %08" PRIx32 "\n", vs_regs[0]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmHiVs    = %08" PRIx32 "\n", vs_regs[1]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc1Vs = %08" PRIx32 "\n", vs_regs[2]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc2Vs = %08" PRIx32 "\n", vs_regs[3]);
			KYTY_LOG_DEBUG("\t m_spiVsOutConfig      = %08" PRIx32 "\n", vs_regs[4]);
			KYTY_LOG_DEBUG("\t m_spiShaderPosFormat  = %08" PRIx32 "\n", vs_regs[5]);
			KYTY_LOG_DEBUG("\t m_paClVsOutCntl       = %08" PRIx32 "\n", vs_regs[6]);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_VS_UPDATE);
	cmd[1] = shader_modifier;
	memcpy(&cmd[2], vs_regs, static_cast<size_t>(7) * 4);

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetEmbeddedVsShader(uint32_t* cmd, uint64_t size, uint32_t id, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 3);

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t id              = %" PRIu32 "\n", id);
			KYTY_LOG_DEBUG("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

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

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t embedded_id     = %d\n", 0);

		cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS_EMBEDDED);
		cmd[1] = 0;
	} else
	{
		EXIT_NOT_IMPLEMENTED(size < 12 + 1);

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);

			KYTY_LOG_DEBUG("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
			KYTY_LOG_DEBUG("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
			KYTY_LOG_DEBUG("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
			KYTY_LOG_DEBUG("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
			KYTY_LOG_DEBUG("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
			KYTY_LOG_DEBUG("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
			KYTY_LOG_DEBUG("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
			KYTY_LOG_DEBUG("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
			KYTY_LOG_DEBUG("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

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

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t embedded_id     = %d\n", 0);

		cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS_EMBEDDED);
		cmd[1] = 0;
	} else
	{
		EXIT_NOT_IMPLEMENTED(size < 12 + 1);

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);

			KYTY_LOG_DEBUG("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
			KYTY_LOG_DEBUG("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
			KYTY_LOG_DEBUG("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
			KYTY_LOG_DEBUG("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
			KYTY_LOG_DEBUG("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
			KYTY_LOG_DEBUG("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
			KYTY_LOG_DEBUG("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
			KYTY_LOG_DEBUG("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
			KYTY_LOG_DEBUG("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

		cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS);
		memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);
	}

	// KYTY_LOG_DEBUG("ok\n");

	return OK;
}

int KYTY_SYSV_ABI GraphicsUpdatePsShader(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(ps_regs == nullptr);
	EXIT_NOT_IMPLEMENTED(size < 12 + 1);

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);

			KYTY_LOG_DEBUG("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
			KYTY_LOG_DEBUG("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
			KYTY_LOG_DEBUG("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
			KYTY_LOG_DEBUG("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
			KYTY_LOG_DEBUG("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
			KYTY_LOG_DEBUG("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
			KYTY_LOG_DEBUG("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
			KYTY_LOG_DEBUG("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
			KYTY_LOG_DEBUG("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS_UPDATE);
	memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);

	return OK;
}

int KYTY_SYSV_ABI GraphicsUpdatePsShader350(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(ps_regs == nullptr);
	EXIT_NOT_IMPLEMENTED(size < 12 + 1);

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);

			KYTY_LOG_DEBUG("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
			KYTY_LOG_DEBUG("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
			KYTY_LOG_DEBUG("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
			KYTY_LOG_DEBUG("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
			KYTY_LOG_DEBUG("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
			KYTY_LOG_DEBUG("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
			KYTY_LOG_DEBUG("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
			KYTY_LOG_DEBUG("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
			KYTY_LOG_DEBUG("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
			KYTY_LOG_DEBUG("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PS_UPDATE);
	memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetCsShaderWithModifier(uint32_t* cmd, uint64_t size, const uint32_t* cs_regs, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 7 + 2);

			KYTY_LOG_DEBUG("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size            = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

			KYTY_LOG_DEBUG("\t m_computePgmLo      = %08" PRIx32 "\n", cs_regs[0]);
			KYTY_LOG_DEBUG("\t m_computePgmHi      = %08" PRIx32 "\n", cs_regs[1]);
			KYTY_LOG_DEBUG("\t m_computePgmRsrc1   = %08" PRIx32 "\n", cs_regs[2]);
			KYTY_LOG_DEBUG("\t m_computePgmRsrc2   = %08" PRIx32 "\n", cs_regs[3]);
			KYTY_LOG_DEBUG("\t m_computeNumThreadX = %08" PRIx32 "\n", cs_regs[4]);
			KYTY_LOG_DEBUG("\t m_computeNumThreadY = %08" PRIx32 "\n", cs_regs[5]);
			KYTY_LOG_DEBUG("\t m_computeNumThreadZ = %08" PRIx32 "\n", cs_regs[6]);

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

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t index_count = %" PRIu32 "\n", index_count);
			KYTY_LOG_DEBUG("\t index_addr  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
			KYTY_LOG_DEBUG("\t flags       = %08" PRIx32 "\n", flags);
			KYTY_LOG_DEBUG("\t type        = %" PRIu32 "\n", type);

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

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t index_count = %" PRIu32 "\n", index_count);
			KYTY_LOG_DEBUG("\t flags       = %08" PRIx32 "\n", flags);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_DRAW_INDEX_AUTO);
	cmd[1] = index_count;
	cmd[2] = flags;

	return OK;
}

int KYTY_SYSV_ABI GraphicsDrawIndexOffset(uint32_t* cmd, uint64_t size, uint32_t index_offset, uint32_t index_count, uint32_t flags)
{
	PRINT_NAME();

	if (cmd == nullptr || size != 9)
	{
		return -1;
	}

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t index_offset = %" PRIu32 "\n", index_offset);
			KYTY_LOG_DEBUG("\t index_count = %" PRIu32 "\n", index_count);
			KYTY_LOG_DEBUG("\t flags       = %08" PRIx32 "\n", flags);

	cmd[0] = KYTY_PM4(5, Pm4::IT_DRAW_INDEX_OFFSET_2, 0u) | (flags & 1u);
	cmd[1] = index_count;
	cmd[2] = index_offset;
	cmd[3] = index_count;
	cmd[4] = Config::IsNeo() ? (flags & 0xE0000000u) : 0u;
	cmd[5] = KYTY_PM4(4, Pm4::IT_NOP, 0u);
	cmd[6] = 0u;
	cmd[7] = 0u;
	cmd[8] = 0u;

	return OK;
}

int KYTY_SYSV_ABI GraphicsInsertWaitFlipDone(uint32_t* cmd, uint64_t size, uint32_t video_out_handle, uint32_t display_buffer_index)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 3);

			KYTY_LOG_DEBUG("\t cmd_buffer           = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size                 = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t video_out_handle     = %" PRIu32 "\n", video_out_handle);
			KYTY_LOG_DEBUG("\t display_buffer_index = %" PRIu32 "\n", display_buffer_index);

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

			KYTY_LOG_DEBUG("\t cmd_buffer     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size           = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t thread_group_x = %" PRIu32 "\n", thread_group_x);
			KYTY_LOG_DEBUG("\t thread_group_y = %" PRIu32 "\n", thread_group_y);
			KYTY_LOG_DEBUG("\t thread_group_z = %" PRIu32 "\n", thread_group_z);
			KYTY_LOG_DEBUG("\t mode           = %" PRIu32 "\n", mode);

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

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState175(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState200(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t KYTY_SYSV_ABI GraphicsDrawInitDefaultHardwareState350(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t KYTY_SYSV_ABI GraphicsDispatchInitDefaultHardwareState(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);

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

	GraphicsRunSubmit(dcb, dcb_size, ccb, ccb_size, GraphicsSubmissionCompletion::None);

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

			KYTY_LOG_DEBUG("\t handle    = %" PRId32 "\n", handle);
			KYTY_LOG_DEBUG("\t index     = %" PRId32 "\n", index);
			KYTY_LOG_DEBUG("\t flip_mode = %" PRId32 "\n", flip_mode);
			KYTY_LOG_DEBUG("\t flip_arg  = %" PRId64 "\n", flip_arg);

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

int KYTY_SYSV_ABI GraphicsAddEqEvent(::Kyty::Kernel::EventQueue::KernelEqueue eq, int id, void* udata)
{
	PRINT_NAME();

	if (eq == nullptr)
	{
		return Kernel::KERNEL_ERROR_EBADF;
	}

	return GraphicsRenderAddEqEvent(eq, id, udata);
}

int KYTY_SYSV_ABI GraphicsDeleteEqEvent(::Kyty::Kernel::EventQueue::KernelEqueue eq, int id)
{
	PRINT_NAME();

	if (eq == nullptr)
	{
		return Kernel::KERNEL_ERROR_EBADF;
	}

	return GraphicsRenderDeleteEqEvent(eq, id);
}

uint32_t KYTY_SYSV_ABI GraphicsMapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw,
                                               uint32_t* read_ptr_addr)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t pipe_id       = %" PRIu32 "\n", pipe_id);
			KYTY_LOG_DEBUG("\t queue_id      = %" PRIu32 "\n", queue_id);
			KYTY_LOG_DEBUG("\t ring_addr     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(ring_addr));
			KYTY_LOG_DEBUG("\t ring_size_dw  = %" PRIu32 "\n", ring_size_dw);
			KYTY_LOG_DEBUG("\t read_ptr_addr = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(read_ptr_addr));

	uint32_t id = GraphicsRunMapComputeQueue(pipe_id, queue_id, ring_addr, ring_size_dw, read_ptr_addr);

			KYTY_LOG_DEBUG("\t queue         = %" PRIu32 "\n", id);

	return id;
}

void KYTY_SYSV_ABI GraphicsUnmapComputeQueue(uint32_t id)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t id = %" PRIu32 "\n", id);

	GraphicsRunUnmapComputeQueue(id);
}

int KYTY_SYSV_ABI GraphicsComputeWaitOnAddress(uint32_t* cmd, uint64_t size, uint32_t* gpu_addr, uint32_t mask, uint32_t func, uint32_t ref)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 6);

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t gpu_addr    = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(gpu_addr));
			KYTY_LOG_DEBUG("\t mask        = %08" PRIx32 "\n", mask);
			KYTY_LOG_DEBUG("\t func        = %" PRIu32 "\n", func);
			KYTY_LOG_DEBUG("\t ref         = %08" PRIx32 "\n", ref);

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

			KYTY_LOG_DEBUG("\t ring_id   = %" PRIu32 "\n", ring_id);
			KYTY_LOG_DEBUG("\t offset_dw = %" PRIu32 "\n", offset_dw);

	GraphicsRunDingDong(ring_id, offset_dw);
}

int KYTY_SYSV_ABI GraphicsInsertPushMarker(uint32_t* cmd, uint64_t size, const char* str)
{
	PRINT_NAME();

	auto len = strlen(str) + 1;

	EXIT_NOT_IMPLEMENTED(size * 4 < len + 1);

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t str         = %s\n", str);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_PUSH_MARKER);

	memcpy(cmd + 1, str, len);

	return OK;
}

int KYTY_SYSV_ABI GraphicsInsertPopMarker(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

			KYTY_LOG_DEBUG("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t size        = %" PRIu64 "\n", size);

	cmd[0] = KYTY_PM4(size, Pm4::IT_NOP, Pm4::R_POP_MARKER);

	return OK;
}

uint64_t KYTY_SYSV_ABI GraphicsGetGpuCoreClockFrequency()
{
	return Kernel::TimePort::GetFrequency();
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

			KYTY_LOG_DEBUG("\t addr = %016" PRIx64 "\n", addr);

	return reinterpret_cast<void*>(addr);
}

int KYTY_SYSV_ABI GraphicsRegisterOwner(uint32_t* owner_handle, const char* name)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(owner_handle == nullptr);
	EXIT_NOT_IMPLEMENTED(name == nullptr);

			KYTY_LOG_DEBUG("\t RegisterOwner: %s\n", name);

	GpuMemoryRegisterOwner(owner_handle, name);

			KYTY_LOG_DEBUG("\t handler: %" PRIu32 "\n", *owner_handle);

	return OK;
}

int KYTY_SYSV_ABI GraphicsRegisterResource(uint32_t* resource_handle, uint32_t owner_handle, const void* memory, size_t size,
                                           const char* name, uint32_t type, uint64_t user_data)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(resource_handle == nullptr);
	EXIT_NOT_IMPLEMENTED(memory == nullptr);
	EXIT_NOT_IMPLEMENTED(name == nullptr);

			KYTY_LOG_DEBUG("\t RegisterResource: %s\n", name);
			KYTY_LOG_DEBUG("\t owner_handle:     %" PRIu32 "\n", owner_handle);
			KYTY_LOG_DEBUG("\t addr:             %016" PRIx64 "\n", reinterpret_cast<uint64_t>(memory));
			KYTY_LOG_DEBUG("\t size:             %" PRIu64 "\n", size);
			KYTY_LOG_DEBUG("\t type:             %" PRIu32 "\n", type);
			KYTY_LOG_DEBUG("\t user_data:        %" PRIu64 "\n", user_data);

	uint32_t rhandle = 0;

	GpuMemoryRegisterResource(&rhandle, owner_handle, memory, size, name, type, user_data);

			KYTY_LOG_DEBUG("\t handler: %" PRIu32 "\n", rhandle);

	if (resource_handle != nullptr)
	{
		*resource_handle = rhandle;
	}

	return OK;
}

int KYTY_SYSV_ABI GraphicsUnregisterAllResourcesForOwner(uint32_t owner_handle)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t owner_handle:     %" PRIu32 "\n", owner_handle);

	GpuMemoryUnregisterAllResourcesForOwner(owner_handle);

	return OK;
}

int KYTY_SYSV_ABI GraphicsUnregisterOwnerAndResources(uint32_t owner_handle)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t owner_handle:     %" PRIu32 "\n", owner_handle);

	GpuMemoryUnregisterOwnerAndResources(owner_handle);

	return OK;
}

int KYTY_SYSV_ABI GraphicsUnregisterResource(uint32_t resource_handle)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t resource_handle:     %" PRIu32 "\n", resource_handle);

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
			KYTY_LOG_DEBUG("\t bottom      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(bottom));
			KYTY_LOG_DEBUG("\t top         = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(top));
			KYTY_LOG_DEBUG("\t cursor_up   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cursor_up));
			KYTY_LOG_DEBUG("\t cursor_down = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cursor_down));
			KYTY_LOG_DEBUG("\t callback    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(callback));
			KYTY_LOG_DEBUG("\t user_data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_data));
			KYTY_LOG_DEBUG("\t reserved_dw = %" PRIu32 "\n", reserved_dw);
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
    /* 4 */ {0x9E5AD592, {{Pm4::CB_TARGET_MASK, 0x0000000f}}},
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

namespace {

constexpr int      GRAPHICS5_DRIVER_ERROR_INVALID_VALUE    = static_cast<int>(0x8a6c0033u);
constexpr int      GRAPHICS5_DRIVER_ERROR_INVALID_ARGUMENT = static_cast<int>(0x8a6c0035u);
constexpr uint32_t WORKLOAD_STREAM_RECORD_SIZE             = 32u;
constexpr uint32_t WORKLOAD_ACTIVE_PACKET_SIZE_DW          = 18u;
constexpr uint32_t WORKLOAD_COMPLETE_PACKET_SIZE_DW        = 12u;
constexpr uint32_t WORKLOAD_STREAM_MIN_ID                  = 1u;
constexpr uint32_t WORKLOAD_STREAM_MAX_ID                  = 31u;
constexpr uint32_t WORKLOAD_ID_MAX                         = 63u;
constexpr uint32_t WORKLOAD_ACTIVE_COUNT_MAX               = 63u;

std::mutex g_workload_stream_mutex;
uint32_t   g_workload_stream_mask = 0;
uint8_t    g_workload_streams[WORKLOAD_STREAM_MAX_ID + 1u][WORKLOAD_STREAM_RECORD_SIZE] = {};

constexpr uint32_t kDescriptorTableEntries = 32;

template <typename T> T ReadDescriptorField(const uint8_t* data, size_t offset)
{
	T value {};
	std::memcpy(&value, data + offset, sizeof(value));
	return value;
}

uint32_t ExtractDescriptorBits(uint32_t value, uint32_t first_bit, uint32_t bit_count)
{
	return (value >> first_bit) & ((1u << bit_count) - 1u);
}

uint32_t ReplaceDescriptorTwoBitField(uint32_t value, uint32_t field, uint32_t shift)
{
	const uint32_t mask = 0x3u << shift;
	return (value & ~mask) | ((field & 0x3u) << shift);
}

uint32_t ApplyDescriptorMask(uint32_t flags, uint32_t source_value, uint32_t mask_value)
{
	flags &= 0xffffffe0u;
	flags |= ExtractDescriptorBits(mask_value, 8u, 5u);
	flags &= 0xfffffbffu;
	flags |= ((source_value & 0x400000u) != 0u ? 0x400u : ((source_value >> 14u) & 0x400u));
	return flags;
}

void WriteDefaultDescriptorEntries(uint64_t* output, uint32_t first_entry)
{
	for (uint32_t i = first_entry; i < kDescriptorTableEntries; ++i)
	{
		output[i] = (static_cast<uint64_t>(i) << 32u) | (0x10000000u + i);
	}
}

uint32_t BuildDescriptorFlags(uint32_t source_value, bool has_mask, uint32_t mask_value)
{
	const uint32_t mode = (source_value >> 20u) & 0x3u;
	uint32_t       flags = 0;

	if (mode == 0u)
	{
		flags = (((source_value >> 24u) & 0x1u) | (has_mask ? 0u : 1u)) << 5u;
		flags = ReplaceDescriptorTwoBitField(flags, source_value >> 28u, 8u);
	}
	else
	{
		flags = ((source_value << 4u) & 0x03000000u) + 0x80000u;
		if (mode == 2u)
		{
			flags &= 0xffefffdfu;
			flags |= has_mask ? ((~(mask_value & source_value) >> 16u) & 0x20u) : 0x20u;
			flags = ReplaceDescriptorTwoBitField(flags, source_value >> 30u, 8u);
			flags = ReplaceDescriptorTwoBitField(flags, source_value >> 30u, 21u);
		}
		else if (has_mask)
		{
			const uint32_t masked = mask_value & source_value;
			flags &= 0xffffffdfu;
			flags |= (masked >> 15u) & 0x20u;
			flags ^= 0x20u;
			flags &= 0xffefffffu;
			flags |= (~masked >> 1u) & 0x100000u;
			flags = ReplaceDescriptorTwoBitField(flags, source_value >> 30u, 8u);
			flags = ReplaceDescriptorTwoBitField(flags, source_value >> 30u, 21u);
		}
		else
		{
			flags |= 0x100020u;
			flags = ReplaceDescriptorTwoBitField(flags, source_value >> 28u, 8u);
			flags = ReplaceDescriptorTwoBitField(flags, source_value >> 30u, 21u);
		}
	}

	return has_mask ? ApplyDescriptorMask(flags, source_value, mask_value) : (flags & 0xfffffbe0u);
}

} // namespace

int KYTY_SYSV_ABI GraphicsBuildDescriptorTable(uint64_t output_address, uint64_t descriptor_address, uint64_t source_address,
	                                           uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	PRINT_NAME();
	(void)arg3;
	(void)arg4;
	(void)arg5;

	auto* output = reinterpret_cast<uint64_t*>(output_address);
	if (output == nullptr)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	if (source_address == 0u)
	{
		WriteDefaultDescriptorEntries(output, 0);
		return OK;
	}

	const auto* source   = reinterpret_cast<const uint8_t*>(source_address);
	const uint32_t count = ReadDescriptorField<uint32_t>(source, 0x50u);
	if (count == 0u)
	{
		WriteDefaultDescriptorEntries(output, 0);
		return OK;
	}
	if (descriptor_address == 0u || count > kDescriptorTableEntries)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	const auto* descriptor = reinterpret_cast<const uint8_t*>(descriptor_address);
	const uint16_t mask_count = ReadDescriptorField<uint16_t>(descriptor, 0x56u);
	const auto* masks = reinterpret_cast<const uint32_t*>(ReadDescriptorField<uint64_t>(descriptor, 0x38u));
	const auto* source_entries = reinterpret_cast<const uint32_t*>(ReadDescriptorField<uint64_t>(source, 0x30u));
	if (source_entries == nullptr || (mask_count != 0u && masks == nullptr))
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t source_value = source_entries[i];
		uint32_t       matching_mask = mask_count;
		for (uint32_t j = 0; j < mask_count; ++j)
		{
			if (static_cast<uint8_t>(masks[j]) == static_cast<uint8_t>(source_value))
			{
				matching_mask = j;
				break;
			}
		}

		const bool has_mask = matching_mask < mask_count;
		const uint32_t mask_value = has_mask ? masks[matching_mask] : 0u;
		const uint32_t flags = BuildDescriptorFlags(source_value, has_mask, mask_value);
		output[i] = (static_cast<uint64_t>(flags) << 32u) | (0x10000000u + i);
	}

	WriteDefaultDescriptorEntries(output, count);
	return OK;
}

int KYTY_SYSV_ABI GraphicsInit(uint32_t* state, uint32_t ver)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t state = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(state));
			KYTY_LOG_DEBUG("\t ver   = %u\n", ver);

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
			KYTY_LOG_WARN("\t WARNING: AGC ver %u != 8, using ver-8 register defaults\n", ver);
	}
	EXIT_IF(!GraphicsInitWriteGuestState(state, ver));

	return OK;
}

void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2(uint32_t ver)
{
	PRINT_NAME();

	if (ver != 8) { KYTY_LOG_WARN("\t WARNING: AGC ver %u != 8\n", ver); }
	EXIT_NOT_IMPLEMENTED(offsetof(RegisterDefaults, count) != 0x38);

	return &g_reg_defaults1;
}

void* KYTY_SYSV_ABI GraphicsGetRegisterDefaults2Internal(uint32_t ver)
{
	PRINT_NAME();

	if (ver != 8) { KYTY_LOG_WARN("\t WARNING: AGC ver %u != 8\n", ver); }
	EXIT_NOT_IMPLEMENTED(offsetof(RegisterDefaults, count) != 0x38);

	return &g_reg_defaults2;
}

static void dbg_dump_shader(const Shader* h)
{
			KYTY_LOG_DEBUG("\t file_header  = 0x%08" PRIx32 "\n", h->file_header);
			KYTY_LOG_DEBUG("\t version      = 0x%08" PRIx32 "\n", h->version);
			KYTY_LOG_DEBUG("\t user_data    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->user_data));
	if (h->user_data != nullptr)
	{
			KYTY_LOG_DEBUG("\t\t direct_resource_count    = 0x%04" PRIx16 "\n", h->user_data->direct_resource_count);
			KYTY_LOG_DEBUG("\t\t direct_resource_offset   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->user_data->direct_resource_offset));
		for (int i = 0; i < static_cast<int>(h->user_data->direct_resource_count); i++)
		{
			KYTY_LOG_DEBUG("\t\t\t offset[%02d] = %04" PRIx16 "\n", i, h->user_data->direct_resource_offset[i]);
		}
		for (int imm = 0; imm < 4; imm++)
		{
			KYTY_LOG_DEBUG("\t\t sharp_resource_count  [%d] = 0x%04" PRIx16 "\n", imm, h->user_data->sharp_resource_count[imm]);
			KYTY_LOG_DEBUG("\t\t sharp_resource_offset [%d] = 0x%016" PRIx64 "\n", imm,
			       reinterpret_cast<uint64_t>(h->user_data->sharp_resource_offset[imm]));
			for (int i = 0; i < static_cast<int>(h->user_data->sharp_resource_count[imm]); i++)
			{
			KYTY_LOG_DEBUG("\t\t\t offset_dw[%d] = %04" PRIx16 ", size = %" PRIu16 "\n", i,
				       h->user_data->sharp_resource_offset[imm][i].offset_dw, h->user_data->sharp_resource_offset[imm][i].size);
			}
		}
			KYTY_LOG_DEBUG("\t\t eud_size_dw    = 0x%04" PRIx16 "\n", h->user_data->eud_size_dw);
			KYTY_LOG_DEBUG("\t\t srt_size_dw    = 0x%04" PRIx16 "\n", h->user_data->srt_size_dw);
	}
			KYTY_LOG_DEBUG("\t code             = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->code));
			KYTY_LOG_DEBUG("\t num_cx_registers = 0x%02" PRIx8 "\n", h->num_cx_registers);
			KYTY_LOG_DEBUG("\t cx_registers     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->cx_registers));
	for (int i = 0; i < static_cast<int>(h->num_cx_registers); i++)
	{
			KYTY_LOG_DEBUG("\t\t cx[%d]: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", i, h->cx_registers[i].offset, h->cx_registers[i].value);
	}
			KYTY_LOG_DEBUG("\t num_sh_registers = 0x%02" PRIx8 "\n", h->num_sh_registers);
			KYTY_LOG_DEBUG("\t sh_registers     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->sh_registers));
	for (int i = 0; i < static_cast<int>(h->num_sh_registers); i++)
	{
			KYTY_LOG_DEBUG("\t\t sh[%d]: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", i, h->sh_registers[i].offset, h->sh_registers[i].value);
	}
			KYTY_LOG_DEBUG("\t specials                          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->specials));
			KYTY_LOG_DEBUG("\t\t ge_cntl:              offset = %08" PRIx32 ", value = %08" PRIx32 "\n", h->specials->ge_cntl.offset,
	       h->specials->ge_cntl.value);
			KYTY_LOG_DEBUG("\t\t vgt_shader_stages_en: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", h->specials->vgt_shader_stages_en.offset,
	       h->specials->vgt_shader_stages_en.value);
			KYTY_LOG_DEBUG("\t\t vgt_gs_out_prim_type: offset = %08" PRIx32 ", value = %08" PRIx32 "\n", h->specials->vgt_gs_out_prim_type.offset,
	       h->specials->vgt_gs_out_prim_type.value);
			KYTY_LOG_DEBUG("\t\t ge_user_vgpr_en:      offset = %08" PRIx32 ", value = %08" PRIx32 "\n", h->specials->ge_user_vgpr_en.offset,
	       h->specials->ge_user_vgpr_en.value);
			KYTY_LOG_DEBUG("\t\t dispatch_modifier = %08" PRIx32 "\n", h->specials->dispatch_modifier);
			KYTY_LOG_DEBUG("\t\t user_data_range: start = %08" PRIx32 ", end = %08" PRIx32 "\n", h->specials->user_data_range.start,
	       h->specials->user_data_range.end);
			KYTY_LOG_DEBUG("\t\t draw_modifier: enbl_start_vertex_offset   = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_start_vertex_offset);
			KYTY_LOG_DEBUG("\t\t draw_modifier: enbl_start_index_offset    = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_start_index_offset);
			KYTY_LOG_DEBUG("\t\t draw_modifier: enbl_start_instance_offset = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_start_instance_offset);
			KYTY_LOG_DEBUG("\t\t draw_modifier: enbl_draw_index            = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_draw_index);
			KYTY_LOG_DEBUG("\t\t draw_modifier: enbl_user_vgprs            = %08" PRIx32 "\n", h->specials->draw_modifier.enbl_user_vgprs);
			KYTY_LOG_DEBUG("\t\t draw_modifier: render_target_slice_offset = %08" PRIx32 "\n", h->specials->draw_modifier.render_target_slice_offset);
			KYTY_LOG_DEBUG("\t\t draw_modifier: fuse_draws                 = %08" PRIx32 "\n", h->specials->draw_modifier.fuse_draws);
			KYTY_LOG_DEBUG("\t\t draw_modifier: compiler_flags             = %08" PRIx32 "\n", h->specials->draw_modifier.compiler_flags);
			KYTY_LOG_DEBUG("\t\t draw_modifier: is_default                 = %08" PRIx32 "\n", h->specials->draw_modifier.is_default);
			KYTY_LOG_DEBUG("\t\t draw_modifier: reserved                   = %08" PRIx32 "\n", h->specials->draw_modifier.reserved);
			KYTY_LOG_DEBUG("\t num_input_semantics               = 0x%08" PRIx32 "\n", h->num_input_semantics);
			KYTY_LOG_DEBUG("\t input_semantics                   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->input_semantics));
	for (int i = 0; i < static_cast<int>(h->num_input_semantics); i++)
	{
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: semantic         = %08" PRIx32 "\n", i, h->input_semantics[i].semantic);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: hardware_mapping = %08" PRIx32 "\n", i, h->input_semantics[i].hardware_mapping);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: size_in_elements = %08" PRIx32 "\n", i, h->input_semantics[i].size_in_elements);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: is_f16           = %08" PRIx32 "\n", i, h->input_semantics[i].is_f16);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: is_flat_shaded   = %08" PRIx32 "\n", i, h->input_semantics[i].is_flat_shaded);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: is_linear        = %08" PRIx32 "\n", i, h->input_semantics[i].is_linear);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: is_custom        = %08" PRIx32 "\n", i, h->input_semantics[i].is_custom);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: static_vb_index  = %08" PRIx32 "\n", i, h->input_semantics[i].static_vb_index);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: static_attribute = %08" PRIx32 "\n", i, h->input_semantics[i].static_attribute);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: reserved         = %08" PRIx32 "\n", i, h->input_semantics[i].reserved);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: default_value    = %08" PRIx32 "\n", i, h->input_semantics[i].default_value);
			KYTY_LOG_DEBUG("\t\t input_semantics[%d]: default_value_hi = %08" PRIx32 "\n", i, h->input_semantics[i].default_value_hi);
	}
			KYTY_LOG_DEBUG("\t num_output_semantics              = 0x%04" PRIx16 "\n", h->num_output_semantics);
			KYTY_LOG_DEBUG("\t output_semantics                  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(h->output_semantics));
	for (int i = 0; i < static_cast<int>(h->num_output_semantics); i++)
	{
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: semantic         = %08" PRIx32 "\n", i, h->output_semantics[i].semantic);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: hardware_mapping = %08" PRIx32 "\n", i, h->output_semantics[i].hardware_mapping);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: size_in_elements = %08" PRIx32 "\n", i, h->output_semantics[i].size_in_elements);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: is_f16           = %08" PRIx32 "\n", i, h->output_semantics[i].is_f16);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: is_flat_shaded   = %08" PRIx32 "\n", i, h->output_semantics[i].is_flat_shaded);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: is_linear        = %08" PRIx32 "\n", i, h->output_semantics[i].is_linear);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: is_custom        = %08" PRIx32 "\n", i, h->output_semantics[i].is_custom);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: static_vb_index  = %08" PRIx32 "\n", i, h->output_semantics[i].static_vb_index);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: static_attribute = %08" PRIx32 "\n", i, h->output_semantics[i].static_attribute);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: reserved         = %08" PRIx32 "\n", i, h->output_semantics[i].reserved);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: default_value    = %08" PRIx32 "\n", i, h->output_semantics[i].default_value);
			KYTY_LOG_DEBUG("\t\t output_semantics[%d]: default_value_hi = %08" PRIx32 "\n", i, h->output_semantics[i].default_value_hi);
	}
			KYTY_LOG_DEBUG("\t header_size                       = 0x%08" PRIx32 "\n", h->header_size);
			KYTY_LOG_DEBUG("\t shader_size                       = 0x%08" PRIx32 "\n", h->shader_size);
			KYTY_LOG_DEBUG("\t embedded_constant_buffer_size_dqw = 0x%08" PRIx32 "\n", h->embedded_constant_buffer_size_dqw);
			KYTY_LOG_DEBUG("\t target                            = 0x%08" PRIx32 "\n", h->target);
			KYTY_LOG_DEBUG("\t scratch_size_dw_per_thread        = 0x%04" PRIx16 "\n", h->scratch_size_dw_per_thread);
			KYTY_LOG_DEBUG("\t special_sizes_bytes               = 0x%04" PRIx16 "\n", h->special_sizes_bytes);
			KYTY_LOG_DEBUG("\t type                              = 0x%02" PRIx8 "\n", h->type);
}

int KYTY_SYSV_ABI GraphicsCreateShader(Shader** dst, void* header, const volatile void* code)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(dst == nullptr);
	EXIT_NOT_IMPLEMENTED(header == nullptr);
	EXIT_NOT_IMPLEMENTED(code == nullptr);

			KYTY_LOG_DEBUG("\t header = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(header));
			KYTY_LOG_DEBUG("\t code   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(code));

	auto* h = static_cast<Shader*>(header);

	auto update_addr = [](auto& m) -> bool
	{
		const auto raw = reinterpret_cast<uintptr_t>(m);
		if (raw == 0 || raw >= 0x100000000ull)
		{
			return false;
		}
		m = reinterpret_cast<typename std::remove_reference<decltype(m)>::type>(raw + reinterpret_cast<uintptr_t>(&m));
		return true;
	};

	update_addr(h->cx_registers);
	update_addr(h->sh_registers);
	update_addr(h->user_data);
	update_addr(h->specials);
	update_addr(h->input_semantics);
	update_addr(h->output_semantics);
	if (h->user_data != nullptr)
	{
		update_addr(h->user_data->direct_resource_offset);
		update_addr(h->user_data->sharp_resource_offset[0]);
		update_addr(h->user_data->sharp_resource_offset[1]);
		update_addr(h->user_data->sharp_resource_offset[2]);
		update_addr(h->user_data->sharp_resource_offset[3]);
	}

	h->code = code;

	EXIT_NOT_IMPLEMENTED(h->file_header != 0x34333231);
	EXIT_NOT_IMPLEMENTED(h->version != 0x00000018);

	auto base = reinterpret_cast<uint64_t>(code);

			KYTY_LOG_DEBUG("\t base   = 0x%016" PRIx64 "\n", base);

	ShaderMappedData map;
	map.user_data           = h->user_data;
	map.input_semantics     = h->input_semantics;
	map.num_input_semantics = h->num_input_semantics;
	map.code_size_bytes     = h->shader_size;

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
			KYTY_LOG_DEBUG("\t SHADER DIAG: unknown type=%u num_sh_registers=%u\n", h->type, h->num_sh_registers);
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

			// PGM_LO/HI name the effective entry point, which may be inside the
			// supplied code allocation. Resource metadata belongs to that address,
			// not only to the allocation base.
			ShaderMapUserData(addr, map);

			h->sh_registers[lo_index].value = static_cast<uint32_t>((addr >> 8u) & 0xffffffffu);
			h->sh_registers[hi_index].value =
			    (h->sh_registers[hi_index].value & 0xffffff00u) | static_cast<uint32_t>((addr >> 40u) & 0xffu);
			patched = true;
			break;
		}
		if (!patched)
		{
			KYTY_LOG_WARN("\t SHADER DIAG: type=%u num_sh_registers=%u missing PGM_LO=0x%x\n", h->type, h->num_sh_registers,
			       lo_offset);
			for (uint32_t i = 0; i < h->num_sh_registers && i < 8; i++)
			{
			KYTY_LOG_DEBUG("\t   sh_reg[%u] offset=0x%x value=0x%x\n", i, h->sh_registers[i].offset, h->sh_registers[i].value);
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

			KYTY_LOG_DEBUG("\t dst   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst));
			KYTY_LOG_DEBUG("\t front = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(front));
			KYTY_LOG_DEBUG("\t back  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(back));

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

			KYTY_LOG_DEBUG("\t fused_result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(fused_result));
			KYTY_LOG_DEBUG("\t front        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(front));
			KYTY_LOG_DEBUG("\t back         = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(back));
			KYTY_LOG_DEBUG("\t scratch_mem  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(scratch_mem));

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

	// Front half ends with s_setpc into the separately allocated back half.
	// Record that relationship so ES-as-VS recompilation can linearize both.
	if (front->code != nullptr && back->code != nullptr)
	{
		ShaderRegisterContinuation(reinterpret_cast<uint64_t>(front->code), reinterpret_cast<uint64_t>(back->code));
	}

	fused_result->user_data = nullptr;
	return OK;
}

int KYTY_SYSV_ABI GraphicsSetCxRegIndirectPatchSetAddress(uint32_t* cmd, const volatile ShaderRegister* regs)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t cmd  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t regs = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));

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

			KYTY_LOG_DEBUG("\t cmd  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t regs = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));

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

			KYTY_LOG_DEBUG("\t cmd  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t regs = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));

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

			KYTY_LOG_DEBUG("\t cmd      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t num_regs = %" PRIu32 "\n", num_regs);

	if (cmd == nullptr)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	if (num_regs > UINT32_MAX - cmd[1])
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}
	cmd[1] += num_regs;

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetShRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t cmd      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t num_regs = %" PRIu32 "\n", num_regs);

	if (cmd == nullptr)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	if (num_regs > UINT32_MAX - cmd[1])
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}
	cmd[1] += num_regs;

	return OK;
}

int KYTY_SYSV_ABI GraphicsSetUcRegIndirectPatchAddRegisters(uint32_t* cmd, uint32_t num_regs)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t cmd      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t num_regs = %" PRIu32 "\n", num_regs);

	if (cmd == nullptr)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	if (num_regs > UINT32_MAX - cmd[1])
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}
	cmd[1] += num_regs;

	return OK;
}

int KYTY_SYSV_ABI GraphicsCreatePrimState(ShaderRegister* cx_regs, ShaderRegister* uc_regs, const Shader* hs, const Shader* gs,
                                          uint32_t prim_type)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t cx_regs   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cx_regs));
			KYTY_LOG_DEBUG("\t uc_regs   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(uc_regs));
			KYTY_LOG_DEBUG("\t hs        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(hs));
			KYTY_LOG_DEBUG("\t gs        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(gs));
			KYTY_LOG_DEBUG("\t prim_type = %" PRIu32 "\n", prim_type);

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
	regs[index].offset = Pm4::CX_PS_SHADER_USAGE_BASE + index;
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
		regs[i].offset = Pm4::CX_PS_SHADER_USAGE_BASE + i;
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

static bool IsReadableGuestRange(const void* address, size_t bytes)
{
	if (address == nullptr || bytes == 0)
	{
		return false;
	}

	void* start = nullptr;
	void* end   = nullptr;
	if (Kyty::Emulator::GuestMemory::GetPort().QueryProtection(const_cast<void*>(address), &start, &end, nullptr) != OK)
	{
		return false;
	}

	const auto begin = reinterpret_cast<uintptr_t>(address);
	const auto last  = begin + bytes - 1u;
	return last >= begin && reinterpret_cast<uintptr_t>(start) <= begin && last <= reinterpret_cast<uintptr_t>(end);
}

static bool IsCreateInterpolantMappingShape(const void* producer, const void* pixel)
{
	if (!IsReadableGuestRange(producer, sizeof(Shader)) || !IsReadableGuestRange(pixel, sizeof(Shader)))
	{
		return false;
	}

	const auto* producer_shader = static_cast<const Shader*>(producer);
	const auto* pixel_shader    = static_cast<const Shader*>(pixel);
	return producer_shader->type == 2u && pixel_shader->type == 1u;
}

int KYTY_SYSV_ABI GraphicsBuildDescriptorTableOrInterpolantMapping(uint64_t output_address, uint64_t descriptor_address,
                                                                   uint64_t source_address, uint64_t arg3, uint64_t arg4,
                                                                   uint64_t arg5)
{
	if (IsCreateInterpolantMappingShape(reinterpret_cast<const void*>(descriptor_address), reinterpret_cast<const void*>(source_address)))
	{
		return GraphicsCreateInterpolantMapping(reinterpret_cast<ShaderRegister*>(output_address),
		                                        reinterpret_cast<const Shader*>(descriptor_address),
		                                        reinterpret_cast<const Shader*>(source_address));
	}

	return GraphicsBuildDescriptorTable(output_address, descriptor_address, source_address, arg3, arg4, arg5);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int KYTY_SYSV_ABI GraphicsCreateInterpolantMapping(ShaderRegister* regs, const Shader* gs, const Shader* ps)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t regs = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
			KYTY_LOG_DEBUG("\t gs   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(gs));
			KYTY_LOG_DEBUG("\t ps   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(ps));

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

			KYTY_LOG_DEBUG("\t addr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(addr));
			KYTY_LOG_DEBUG("\t cmd  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
			KYTY_LOG_DEBUG("\t type = %d\n", type);

	EXIT_NOT_IMPLEMENTED(addr == nullptr);
	EXIT_NOT_IMPLEMENTED(cmd == nullptr);
	// type 1: payload at cmd+2 (existing). type 0 observed on the post-logo
	// path with the same relative payload offset.
	EXIT_NOT_IMPLEMENTED(type != 0 && type != 1);

	const uint32_t header = cmd[0];
	const uint32_t r      = KYTY_PM4_R(header);
			KYTY_LOG_DEBUG("\t header = 0x%08" PRIx32 " r = 0x%02" PRIx32 "\n", header, r);

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

	// sceAgcSuspendPoint is a cooperative driver checkpoint, not a graphics
	// shutdown request. Unity calls it during normal frame progression; tearing
	// down the run loop here permanently stops presentation after boot frames.
	return OK;
}

uint64_t* GraphicsResolveWaitMemAddressFromPrecedingRelease(const uint32_t* wait_body, const uint32_t* stream_begin,
                                                           const uint32_t* stream_end)
{
	if (wait_body == nullptr || stream_begin == nullptr || stream_end == nullptr)
	{
		return nullptr;
	}

	const uintptr_t begin = reinterpret_cast<uintptr_t>(stream_begin);
	const uintptr_t end   = reinterpret_cast<uintptr_t>(stream_end);
	const uintptr_t wait  = reinterpret_cast<uintptr_t>(wait_body);
	if (begin >= end || wait < begin + sizeof(uint32_t) || wait + 8u * sizeof(uint32_t) > end)
	{
		return nullptr;
	}
	const auto* wait_header = reinterpret_cast<const uint32_t*>(wait - sizeof(uint32_t));
	if (*wait_header != KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64))
	{
		return nullptr;
	}

	// Custom ReleaseMem packets have either seven or eight dwords in the stream.
	// The WaitMem body starts after its header, so try both complete layouts while
	// keeping every candidate inside the validated command range.
	for (const uint32_t release_dwords: {8u, 9u})
	{
		if (wait < begin + static_cast<uintptr_t>(release_dwords) * sizeof(uint32_t))
		{
			continue;
		}
		const auto* release = reinterpret_cast<const uint32_t*>(wait - static_cast<uintptr_t>(release_dwords) * sizeof(uint32_t));
		const uint32_t expected_header = KYTY_PM4(release_dwords - 1u, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);
		if (release[0] != expected_header)
		{
			continue;
		}

		const uint64_t addr = release[3] | (static_cast<uint64_t>(release[4]) << 32u);
		// Address 1 is the no-destination marker used by cache/event-only
		// ReleaseMem packets. Never expose it as a host pointer for WaitMem.
		if (addr == 0 || addr == 1)
		{
			return nullptr;
		}
		return reinterpret_cast<uint64_t*>(addr);
	}

	return nullptr;
}

int KYTY_SYSV_ABI GraphicsAgcQueueEndOfPipeActionPatchAddress(uint32_t* cmd, uint64_t address)
{
	PRINT_NAME();

	if (cmd == nullptr)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	const uint32_t header = cmd[0];
	if ((header >> 30u) != 3u || ((header >> 8u) & 0xffu) != Pm4::IT_NOP || KYTY_PM4_R(header) != Pm4::R_RELEASE_MEM ||
	    KYTY_PM4_LEN(header) < 7u)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
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

			KYTY_LOG_DEBUG("\t buf    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buf));
			KYTY_LOG_DEBUG("\t num_dw = %" PRIu32 "\n", num_dw);

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

uint32_t* KYTY_SYSV_ABI GraphicsCbEmitDefaultIndexedUconfig(CommandBuffer* buf, uint32_t control0, uint32_t control1)
{
	PRINT_NAME();

	if (buf == nullptr || control0 != 0u || control1 != 0u)
	{
		return nullptr;
	}

	auto* cmd = buf->AllocateDW(3);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	// SET_UCONFIG_REG_INDEX selects the indexed UCONFIG bootstrap register.
	cmd[0] = KYTY_PM4(3, Pm4::IT_SET_UCONFIG_REG_INDEX, 0u);
	cmd[1] = Pm4::VGT_INDEX_TYPE_INDEXED_UCONFIG_SELECTOR;
	cmd[2] = Pm4::VGT_INDEX_TYPE_DEFAULT_VALUE;
	return cmd;
}

bool GraphicsDecodeIndexedUconfigVgtIndexType(uint32_t header, const uint32_t* body, uint32_t available_dwords,
                                              uint32_t* index_type)
{
	if (body == nullptr || index_type == nullptr || available_dwords < 2u ||
	    header != KYTY_PM4(3, Pm4::IT_SET_UCONFIG_REG_INDEX, 0u) || body[0] != Pm4::VGT_INDEX_TYPE_INDEXED_UCONFIG_SELECTOR)
	{
		return false;
	}

	// VGT_INDEX_TYPE shares the low two-bit index-size field with IT_INDEX_TYPE.
	*index_type = body[1] & Pm4::VGT_INDEX_TYPE_SIZE_MASK;
	return true;
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
			KYTY_LOG_DEBUG("\t exception_id = 0x%08" PRIx32 "\n", exception_id);
	return OK;
}

int KYTY_SYSV_ABI GraphicsWriteDataPatchSetAddressOrOffset(uint32_t* cmd, uint64_t address_or_offset)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " addr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), address_or_offset);

	if (cmd == nullptr)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
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

			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));

	if (cmd == nullptr)
	{
		return 0;
	}

	const uint32_t header = cmd[0];
			KYTY_LOG_DEBUG("\t header = 0x%08" PRIx32 "\n", header);

	if ((header >> 30u) != 3u)
	{
		return 0;
	}

	const uint32_t size_dw = KYTY_PM4_LEN(header);
			KYTY_LOG_DEBUG("\t size_dw = %" PRIu32 "\n", size_dw);
	return size_dw;
}

// sceAgcDmaDataPatchSetDstAddressOrOffset (NID IxYiarKlXxM).
// R_DMA_DATA layout: +0 header, +16/+20 destination address lo/hi.
int KYTY_SYSV_ABI GraphicsAgcDmaDataPatchSetDstAddressOrOffset(uint32_t* cmd, uint64_t destination_address)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " dst = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), destination_address);

	if (cmd == nullptr || !GraphicsIsCustomDmaDataPacket(cmd[0]))
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}
	cmd[4] = static_cast<uint32_t>(destination_address & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>((destination_address >> 32u) & 0xffffffffu);
	return OK;
}

// sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate (NID cdDRpqcFGbU).
int KYTY_SYSV_ABI GraphicsAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate(uint32_t* cmd, uint64_t source_value)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " src = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), source_value);

	if (cmd == nullptr || !GraphicsIsCustomDmaDataPacket(cmd[0]))
	{
		return Kernel::KERNEL_ERROR_EINVAL;
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
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " addr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), address);

	if (cmd == nullptr)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}
	const uint32_t byte_off = GraphicsWaitRegMemAddressByteOffset(cmd[0]);
	if (byte_off == 0 || (byte_off % 4u) != 0)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}
	const uint32_t dw = byte_off / 4u;
	switch (GraphicsGetWaitRegMemForm(cmd[0]))
	{
		case GraphicsWaitRegMemForm::CustomWaitMem32:
			cmd[dw]     = static_cast<uint32_t>(address) & ~0x3u;
			cmd[dw + 1] = static_cast<uint32_t>(address >> 32u) & 0x3ffffu;
			break;
		case GraphicsWaitRegMemForm::CustomWaitMem64:
			cmd[dw]     = static_cast<uint32_t>(address) & ~0x7u;
			cmd[dw + 1] = static_cast<uint32_t>(address >> 32u) & 0x3ffffu;
			break;
		default:
			cmd[dw]     = static_cast<uint32_t>(address);
			cmd[dw + 1] = static_cast<uint32_t>(address >> 32u);
			break;
	}
	return OK;
}

// sceAgcWaitRegMemPatchCompareFunction (NID n485EBnIWmk).
// IT_WAIT_REG_MEM: compare at +4; R_WAIT_MEM_32: +20; R_WAIT_MEM_64: +28.
int KYTY_SYSV_ABI GraphicsAgcWaitRegMemPatchCompareFunction(uint32_t* cmd, uint32_t compare_function)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " compare_function = %" PRIu32 "\n", reinterpret_cast<uint64_t>(cmd), compare_function);

	if (cmd == nullptr || compare_function > 7u)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	uint32_t byte_off = 0;
	switch (GraphicsGetWaitRegMemForm(cmd[0]))
	{
		case GraphicsWaitRegMemForm::ItWaitRegMem: byte_off = 4u; break;
		case GraphicsWaitRegMemForm::CustomWaitMem32: byte_off = 20u; break;
		case GraphicsWaitRegMemForm::CustomWaitMem64: byte_off = 28u; break;
		default: return Kernel::KERNEL_ERROR_EINVAL;
	}

	const uint32_t dw = byte_off / 4u;
	cmd[dw]           = GraphicsPatchUInt32Bits(cmd[dw], 0x7u, compare_function);
	return OK;
}

// sceAgcWaitRegMemPatchReference (NID 7nOoijNPvEU).
int KYTY_SYSV_ABI GraphicsAgcWaitRegMemPatchReference(uint32_t* cmd, uint64_t reference)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " reference = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), reference);

	if (cmd == nullptr)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	switch (GraphicsGetWaitRegMemForm(cmd[0]))
	{
		case GraphicsWaitRegMemForm::ItWaitRegMem:
			cmd[4] = static_cast<uint32_t>(reference);
			return OK;
		case GraphicsWaitRegMemForm::CustomWaitMem32:
			cmd[5] = static_cast<uint32_t>(reference);
			return OK;
		case GraphicsWaitRegMemForm::CustomWaitMem64:
			cmd[5] = static_cast<uint32_t>(reference & 0xffffffffu);
			cmd[6] = static_cast<uint32_t>((reference >> 32u) & 0xffffffffu);
			return OK;
		default: return Kernel::KERNEL_ERROR_EINVAL;
	}
}

// sceAgcWaitRegMemPatchMask (NID hXAnLgDHCoI).
int KYTY_SYSV_ABI GraphicsAgcWaitRegMemPatchMask(uint32_t* cmd, uint64_t mask)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " mask = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), mask);

	if (cmd == nullptr)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	switch (GraphicsGetWaitRegMemForm(cmd[0]))
	{
		case GraphicsWaitRegMemForm::ItWaitRegMem:
			cmd[5] = static_cast<uint32_t>(mask);
			return OK;
		case GraphicsWaitRegMemForm::CustomWaitMem32:
			cmd[3] = static_cast<uint32_t>(mask);
			return OK;
		case GraphicsWaitRegMemForm::CustomWaitMem64:
			cmd[3] = static_cast<uint32_t>(mask & 0xffffffffu);
			cmd[4] = static_cast<uint32_t>((mask >> 32u) & 0xffffffffu);
			return OK;
		default: return Kernel::KERNEL_ERROR_EINVAL;
	}
}

// sceAgcQueueEndOfPipeActionPatchGcrCntl (NID J8YCgfKAMQs).
int KYTY_SYSV_ABI GraphicsAgcQueueEndOfPipeActionPatchGcrCntl(uint32_t* cmd, uint32_t gcr_cntl)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " gcr_cntl = 0x%04" PRIx32 "\n", reinterpret_cast<uint64_t>(cmd), gcr_cntl & 0xffffu);

	if (cmd == nullptr || !GraphicsIsAgcReleaseMemPacket(cmd[0]))
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	cmd[2] = GraphicsPatchUInt32Bits(cmd[2], 0x0000ffffu, gcr_cntl);
	return OK;
}

// sceAgcQueueEndOfPipeActionPatchData (NID MlEw1feXcjg).
int KYTY_SYSV_ABI GraphicsAgcQueueEndOfPipeActionPatchData(uint32_t* cmd, uint64_t data)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " data = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), data);

	if (cmd == nullptr || !GraphicsIsAgcReleaseMemPacket(cmd[0]))
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	cmd[5] = static_cast<uint32_t>(data & 0xffffffffu);
	cmd[6] = static_cast<uint32_t>((data >> 32u) & 0xffffffffu);
	return OK;
}

// sceAgcQueueEndOfPipeActionPatchType (NID T9fjQIINoeE).
int KYTY_SYSV_ABI GraphicsAgcQueueEndOfPipeActionPatchType(uint32_t* cmd, uint32_t data_selection)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " data_selection = %" PRIu32 "\n", reinterpret_cast<uint64_t>(cmd), data_selection);

	if (cmd == nullptr || data_selection > 3u || !GraphicsIsAgcReleaseMemPacket(cmd[0]))
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	cmd[2] = GraphicsPatchUInt32Bits(cmd[2], 0x00ff0000u, data_selection << 16u);
	return OK;
}

static bool GraphicsResolveWriteDataPatchArgs(uint64_t first, uint64_t second, uint32_t** cmd_out, uint32_t* value_out)
{
	auto* first_cmd  = reinterpret_cast<uint32_t*>(first);
	auto* second_cmd = reinterpret_cast<uint32_t*>(second);
	if (first_cmd != nullptr && GraphicsIsWriteDataPacket(first_cmd[0]))
	{
		*cmd_out   = first_cmd;
		*value_out = static_cast<uint32_t>(second);
		return true;
	}
	if (second_cmd != nullptr && GraphicsIsWriteDataPacket(second_cmd[0]))
	{
		*cmd_out   = second_cmd;
		*value_out = static_cast<uint32_t>(first);
		return true;
	}
	return false;
}

static int GraphicsWriteDataPatchControlByte(uint32_t* cmd, uint32_t value, uint32_t byte_index)
{
	if (cmd == nullptr || !GraphicsIsWriteDataPacket(cmd[0]) || byte_index > 3u)
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}

	const uint32_t shift = byte_index * 8u;
	cmd[1]               = GraphicsPatchUInt32Bits(cmd[1], 0xffu << shift, (value & 0xffu) << shift);
	return OK;
}

// sceAgcWriteDataPatchSetCachePolicy (NID eAy8eGNsCuU).
int KYTY_SYSV_ABI GraphicsWriteDataPatchSetCachePolicy(uint32_t* cmd, uintptr_t arg1)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " arg1 = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd),
	       static_cast<uint64_t>(arg1));

	uint32_t* packet = nullptr;
	uint32_t  value  = 0;
	if (!GraphicsResolveWriteDataPatchArgs(reinterpret_cast<uint64_t>(cmd), static_cast<uint64_t>(arg1), &packet, &value))
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}
	return GraphicsWriteDataPatchControlByte(packet, value, 1u);
}

// sceAgcWriteDataPatchSetDst (NID tmy-+rBpspY).
int KYTY_SYSV_ABI GraphicsWriteDataPatchSetDst(uint32_t* cmd, uintptr_t arg1)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " arg1 = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd),
	       static_cast<uint64_t>(arg1));

	uint32_t* packet = nullptr;
	uint32_t  value  = 0;
	if (!GraphicsResolveWriteDataPatchArgs(reinterpret_cast<uint64_t>(cmd), static_cast<uint64_t>(arg1), &packet, &value))
	{
		return Kernel::KERNEL_ERROR_EINVAL;
	}
	return GraphicsWriteDataPatchControlByte(packet, value, 0u);
}

// sceAgcDcbStallCommandBufferParserGetSize (NID +u6dKSLWM2o).
uint32_t KYTY_SYSV_ABI GraphicsDcbStallCommandBufferParserGetSize()
{
	return 2u * sizeof(uint32_t);
}

// sceAgcDcbDmaDataGetSize (NID 2ccJz9LQI+w).
uint32_t KYTY_SYSV_ABI GraphicsDcbDmaDataGetSize()
{
	return 8u * sizeof(uint32_t);
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

			KYTY_LOG_DEBUG("\t buf        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buf));
			KYTY_LOG_DEBUG("\t offset     = %" PRIx32 "\n", offset);
			KYTY_LOG_DEBUG("\t values     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(values));
			KYTY_LOG_DEBUG("\t num_values = %" PRIu32 "\n", num_values);

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

			KYTY_LOG_DEBUG("\t buf      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buf));
			KYTY_LOG_DEBUG("\t regs     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
			KYTY_LOG_DEBUG("\t num_regs = %" PRIu32 "\n", num_regs);

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

	auto* cmd = buf->AllocateDW(6);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	cmd[0] = KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_DISPATCH_DIRECT);
	cmd[1] = group_x;
	cmd[2] = group_y;
	cmd[3] = group_z;
	cmd[4] = modifier;
	cmd[5] = 0;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsCbReleaseMem(CommandBuffer* buf, uint8_t action, uint16_t gcr_cntl, uint8_t dst, uint8_t cache_policy,
                                             const volatile Label* address, uint8_t data_sel, uint64_t data, uint16_t gds_offset,
                                             uint16_t gds_size, uint8_t interrupt, uint32_t interrupt_ctx_id)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t action           = 0x%02" PRIx8 "\n", action);
			KYTY_LOG_DEBUG("\t gcr_cntl         = 0x%04" PRIx16 "\n", gcr_cntl);
			KYTY_LOG_DEBUG("\t dst              = %" PRIu8 "\n", dst);
			KYTY_LOG_DEBUG("\t cache_policy     = 0x%02" PRIx8 "\n", cache_policy);
			KYTY_LOG_DEBUG("\t address          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
			KYTY_LOG_DEBUG("\t data_sel         = 0x%02" PRIx8 "\n", data_sel);
			KYTY_LOG_DEBUG("\t data             = 0x%016" PRIx64 "\n", data);
			KYTY_LOG_DEBUG("\t gds_offset       = %" PRIu16 "\n", gds_offset);
			KYTY_LOG_DEBUG("\t gds_size         = %" PRIu16 "\n", gds_size);
			KYTY_LOG_DEBUG("\t interrupt        = 0x%02" PRIx8 "\n", interrupt);
			KYTY_LOG_DEBUG("\t interrupt_ctx_id = %" PRIu32 "\n", interrupt_ctx_id);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	// dst: 0 = memory, 1 = TC_L2 (Gen5 AGC).
	EXIT_NOT_IMPLEMENTED(dst > 1);
	// data_sel: 0 = no destination write (barrier/flush only), 1 = 32-bit
	// immediate, 2 = 64-bit immediate, 3 = GPU clock counter. Packet layout
	// stores data in DW5/DW6 for write forms; data_sel 0 uses the same custom
	// envelope. CP custom R_RELEASE_MEM already accepts 0..3.
	EXIT_NOT_IMPLEMENTED(data_sel != 0 && data_sel != 1 && data_sel != 2 && data_sel != 3);
	EXIT_NOT_IMPLEMENTED(gds_offset != 0);
	// Non-GDS forms do not encode GDS fields. Guests may pass gds_size 0
	// (unused), 1 (default command buffer value), or 2 (Gen5 command buffer
	// value).
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

			KYTY_LOG_DEBUG("\t op    = 0x%08" PRIx32 "\n", op);
			KYTY_LOG_DEBUG("\t state = 0x%08" PRIx32 "\n", state);

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

			KYTY_LOG_DEBUG("\t video_out_handle     = %" PRIu32 "\n", video_out_handle);
			KYTY_LOG_DEBUG("\t display_buffer_index = %" PRIu32 "\n", display_buffer_index);

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

int KYTY_SYSV_ABI GraphicsDriverRegisterWorkloadStream(uint32_t stream_id, const void* stream)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t stream_id = %" PRIu32 "\n", stream_id);
			KYTY_LOG_DEBUG("\t stream    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(stream));

	if (stream_id < WORKLOAD_STREAM_MIN_ID || stream_id > WORKLOAD_STREAM_MAX_ID)
	{
		return GRAPHICS5_DRIVER_ERROR_INVALID_VALUE;
	}

	if (stream == nullptr)
	{
		return GRAPHICS5_DRIVER_ERROR_INVALID_ARGUMENT;
	}

	std::lock_guard lock(g_workload_stream_mutex);

	const uint32_t stream_bit = 1u << stream_id;
	if ((g_workload_stream_mask & stream_bit) != 0)
	{
		return GRAPHICS5_DRIVER_ERROR_INVALID_VALUE;
	}

	std::memset(g_workload_streams[stream_id], 0, WORKLOAD_STREAM_RECORD_SIZE);
	std::memcpy(g_workload_streams[stream_id], stream, WORKLOAD_STREAM_RECORD_SIZE);
	g_workload_stream_mask |= stream_bit;

	return OK;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetWorkloadsActive(CommandBuffer* buf, uint32_t stream_id, const uint32_t* workload_ids,
                                                      uint32_t workload_count)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t stream_id      = %" PRIu32 "\n", stream_id);
			KYTY_LOG_DEBUG("\t workload_ids   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(workload_ids));
			KYTY_LOG_DEBUG("\t workload_count = %" PRIu32 "\n", workload_count);

	if (buf == nullptr || workload_ids == nullptr || workload_count == 0 || workload_count > WORKLOAD_ACTIVE_COUNT_MAX ||
	    stream_id < WORKLOAD_STREAM_MIN_ID || stream_id > WORKLOAD_STREAM_MAX_ID)
	{
		return nullptr;
	}

	uint64_t workload_mask = 0;
	for (uint32_t i = 0; i < workload_count; i++)
	{
		const uint32_t workload_id = workload_ids[i];
		if (workload_id > WORKLOAD_ID_MAX)
		{
			return nullptr;
		}

		const uint64_t workload_bit = 1ull << workload_id;
		if ((workload_mask & workload_bit) != 0)
		{
			return nullptr;
		}
		workload_mask |= workload_bit;
	}

	{
		std::lock_guard lock(g_workload_stream_mutex);
		if ((g_workload_stream_mask & (1u << stream_id)) == 0)
		{
			return nullptr;
		}
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(WORKLOAD_ACTIVE_PACKET_SIZE_DW);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	cmd[0] = KYTY_PM4(WORKLOAD_ACTIVE_PACKET_SIZE_DW, Pm4::IT_NOP, Pm4::R_ZERO);
	std::memset(cmd + 1, 0, static_cast<size_t>(WORKLOAD_ACTIVE_PACKET_SIZE_DW - 1u) * sizeof(uint32_t));
	cmd[1] = stream_id;
	cmd[2] = static_cast<uint32_t>(workload_mask & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((workload_mask >> 32u) & 0xffffffffu);

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetWorkloadComplete(CommandBuffer* buf, uint32_t stream_id, uint32_t workload_id)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t stream_id   = %" PRIu32 "\n", stream_id);
			KYTY_LOG_DEBUG("\t workload_id = %" PRIu32 "\n", workload_id);

	if (buf == nullptr || stream_id < WORKLOAD_STREAM_MIN_ID || stream_id > WORKLOAD_STREAM_MAX_ID || workload_id > WORKLOAD_ID_MAX)
	{
		return nullptr;
	}

	{
		std::lock_guard lock(g_workload_stream_mutex);
		if ((g_workload_stream_mask & (1u << stream_id)) == 0)
		{
			return nullptr;
		}
	}

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(WORKLOAD_COMPLETE_PACKET_SIZE_DW);
	if (cmd == nullptr)
	{
		return nullptr;
	}

	const uint64_t workload_clear_mask = ~(1ull << workload_id);

	cmd[0] = KYTY_PM4(WORKLOAD_COMPLETE_PACKET_SIZE_DW, Pm4::IT_NOP, Pm4::R_ZERO);
	std::memset(cmd + 1, 0, static_cast<size_t>(WORKLOAD_COMPLETE_PACKET_SIZE_DW - 1u) * sizeof(uint32_t));
	cmd[1] = stream_id;
	cmd[2] = workload_id;
	cmd[3] = static_cast<uint32_t>(workload_clear_mask & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>((workload_clear_mask >> 32u) & 0xffffffffu);

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

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetCxRegisterDirect(CommandBuffer* buf, ShaderRegister reg)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0u);
	cmd[1] = reg.offset;
	cmd[2] = reg.value;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetUcRegisterDirect(CommandBuffer* buf, ShaderRegister reg)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(3);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(3, Pm4::IT_SET_UCONFIG_REG, 0u);
	cmd[1] = reg.offset;
	cmd[2] = reg.value;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbSetCxRegistersIndirect(CommandBuffer* buf, const volatile ShaderRegister* regs, uint32_t num_regs)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t regs     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
			KYTY_LOG_DEBUG("\t num_regs = %" PRIu32 "\n", num_regs);

	if (buf == nullptr)
	{
		return nullptr;
	}

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

			KYTY_LOG_DEBUG("\t regs     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
			KYTY_LOG_DEBUG("\t num_regs = %" PRIu32 "\n", num_regs);

	if (buf == nullptr)
	{
		return nullptr;
	}

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

			KYTY_LOG_DEBUG("\t regs     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(regs));
			KYTY_LOG_DEBUG("\t num_regs = %" PRIu32 "\n", num_regs);

	if (buf == nullptr)
	{
		return nullptr;
	}

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

			KYTY_LOG_DEBUG("\t index_size   = 0x%" PRIx8 "\n", index_size);
			KYTY_LOG_DEBUG("\t cache_policy = 0x%" PRIx8 "\n", cache_policy);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(cache_policy != 0);

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(2);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(2, Pm4::IT_INDEX_TYPE, 0u);
	cmd[1] = index_size;

	return cmd;
}

static bool draw_index_auto_modifier_supported(uint64_t modifier)
{
	// ShaderDrawModifier defines the low 32 bits plus is_default at bit 32.
	// Bits 33..63 are reserved and cannot be represented by the draw packet.
	constexpr uint64_t defined_bits = (1ull << 33u) - 1ull;
	return (modifier & ~defined_bits) == 0;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexAuto(CommandBuffer* buf, uint32_t index_count, uint64_t modifier)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t index_count = 0x%" PRIx32 "\n", index_count);
			KYTY_LOG_DEBUG("\t modifier    = 0x%016" PRIx64 "\n", modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(!draw_index_auto_modifier_supported(modifier));

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(7);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	cmd[0] = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_DRAW_INDEX_AUTO);
	cmd[1] = index_count;
	cmd[2] = static_cast<uint32_t>(modifier & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>(modifier >> 32u);
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexAutoWithBase(CommandBuffer* buf, uint32_t base_vertex, uint32_t index_count,
                                                         uint64_t modifier)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t base_vertex = 0x%" PRIx32 "\n", base_vertex);
			KYTY_LOG_DEBUG("\t index_count = 0x%" PRIx32 "\n", index_count);
			KYTY_LOG_DEBUG("\t modifier    = 0x%016" PRIx64 "\n", modifier);

	// Not registered under B+aG9DUnTKA (that NID is sceAgcDcbDrawIndexOffset).
	// Keep helper for a future evidenced Auto-with-base NID; base_vertex==0 only.
	EXIT_NOT_IMPLEMENTED(base_vertex != 0);

	return GraphicsDcbDrawIndexAuto(buf, index_count, modifier);
}

// sceAgcDcbDrawIndexOffset — NID B+aG9DUnTKA.
// Packet layout (5 DW): header, index_count, index_offset, index_count, flags&0xE0000001.
uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndexOffset(CommandBuffer* buf, uint32_t index_offset, uint32_t index_count, uint32_t flags)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t index_offset = 0x%" PRIx32 "\n", index_offset);
			KYTY_LOG_DEBUG("\t index_count  = 0x%" PRIx32 "\n", index_count);
			KYTY_LOG_DEBUG("\t flags        = 0x%" PRIx32 "\n", flags);

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

uint32_t* KYTY_SYSV_ABI GraphicsDcbDrawIndex(CommandBuffer* buf, uint32_t index_count, const void* index_addr, uint64_t modifier)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t index_count = 0x%" PRIx32 "\n", index_count);
			KYTY_LOG_DEBUG("\t index_addr  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
			KYTY_LOG_DEBUG("\t modifier    = 0x%016" PRIx64 "\n", modifier);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	EXIT_NOT_IMPLEMENTED(!draw_index_auto_modifier_supported(modifier));

	buf->DbgDump();

	auto* cmd = buf->AllocateDW(7);

	EXIT_NOT_IMPLEMENTED(cmd == nullptr);

	auto addr = reinterpret_cast<uint64_t>(index_addr);

	cmd[0] = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_DRAW_INDEX);
	cmd[1] = index_count;
	cmd[2] = static_cast<uint32_t>(addr & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((addr >> 32u) & 0xffffffffu);
	cmd[4] = static_cast<uint32_t>(modifier & 0xffffffffu);
	cmd[5] = static_cast<uint32_t>(modifier >> 32u);
	cmd[6] = 0;

	return cmd;
}

uint32_t* KYTY_SYSV_ABI GraphicsDcbEventWrite(CommandBuffer* buf, uint8_t event_type, const volatile void* address)
{
	PRINT_NAME();

			KYTY_LOG_DEBUG("\t event_type = 0x%02" PRIx8 "\n", event_type);
			KYTY_LOG_DEBUG("\t address    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));

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
			KYTY_LOG_DEBUG("\t destination              = 0x%02" PRIx8 "\n", destination);
			KYTY_LOG_DEBUG("\t destination_cache_policy = 0x%02" PRIx8 "\n", destination_cache_policy);
			KYTY_LOG_DEBUG("\t source                   = 0x%02" PRIx8 "\n", source);
			KYTY_LOG_DEBUG("\t destination_address      = 0x%016" PRIx64 "\n", destination_address);
			KYTY_LOG_DEBUG("\t source_cache_policy      = 0x%02" PRIx8 "\n", source_cache_policy);
			KYTY_LOG_DEBUG("\t source_address           = 0x%016" PRIx64 "\n", source_address);
			KYTY_LOG_DEBUG("\t byte_count               = %" PRIu32 "\n", byte_count);

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

			KYTY_LOG_DEBUG("\t engine      = 0x%02" PRIx8 "\n", engine);
			KYTY_LOG_DEBUG("\t cb_db_op    = 0x%08" PRIx32 "\n", cb_db_op);
			KYTY_LOG_DEBUG("\t gcr_cntl    = 0x%08" PRIx32 "\n", gcr_cntl);
			KYTY_LOG_DEBUG("\t base        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(base));
			KYTY_LOG_DEBUG("\t size_bytes  = 0x%016" PRIx64 "\n", size_bytes);
			KYTY_LOG_DEBUG("\t poll_cycles = %" PRIu32 "\n", poll_cycles);

	bool no_size = (static_cast<int64_t>(size_bytes) == -1);
	auto vaddr   = reinterpret_cast<uint64_t>(base);

	EXIT_NOT_IMPLEMENTED(buf == nullptr);
	// AGC ver 10 issues ACQUIRE_MEM (GCR cache ops) with finer-than-256B granularity;
	// the PM4 encoding is size>>8 / addr>>8, so sub-256B bits are dropped. That is
	// acceptable for our coherency model — warn instead of aborting.
	if (!no_size && (size_bytes & 0xffu) != 0)
	{
			KYTY_LOG_DEBUG("\t WARNING: ACQUIRE_MEM size 0x%" PRIx64 " not 256B-aligned\n", size_bytes);
	}
	EXIT_NOT_IMPLEMENTED(!no_size && (size_bytes >> 40u) != 0);
	if ((vaddr & 0xffu) != 0)
	{
			KYTY_LOG_DEBUG("\t WARNING: ACQUIRE_MEM base 0x%" PRIx64 " not 256B-aligned\n", vaddr);
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

			KYTY_LOG_DEBUG("\t base_index = %" PRIu32 "\n", base_index);
			KYTY_LOG_DEBUG("\t address    = 0x%016" PRIx64 "\n", address);

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

			KYTY_LOG_DEBUG("\t data_offset = %" PRIu32 "\n", data_offset);
			KYTY_LOG_DEBUG("\t modifier    = 0x%08" PRIx32 "\n", modifier);

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

			KYTY_LOG_DEBUG("\t data_offset = 0x%" PRIx32 "\n", data_offset_in_bytes);
			KYTY_LOG_DEBUG("\t modifier    = 0x%016" PRIx64 "\n", modifier);

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

			KYTY_LOG_DEBUG("\t dst               = 0x%02" PRIx8 "\n", dst);
			KYTY_LOG_DEBUG("\t cache_policy      = 0x%02" PRIx8 "\n", cache_policy);
			KYTY_LOG_DEBUG("\t address_or_offset = 0x%016" PRIx64 "\n", address_or_offset);
			KYTY_LOG_DEBUG("\t data              = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
			KYTY_LOG_DEBUG("\t num_dwords        = %" PRIu32 "\n", num_dwords);
			KYTY_LOG_DEBUG("\t increment         = 0x%02" PRIx8 "\n", increment);
			KYTY_LOG_DEBUG("\t write_confirm     = 0x%02" PRIx8 "\n", write_confirm);

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

			KYTY_LOG_DEBUG("\t size             = 0x%02" PRIx8 "\n", size);
			KYTY_LOG_DEBUG("\t compare_function = 0x%02" PRIx8 "\n", compare_function);
			KYTY_LOG_DEBUG("\t op               = 0x%02" PRIx8 "\n", op);
			KYTY_LOG_DEBUG("\t cache_policy     = 0x%02" PRIx8 "\n", cache_policy);
			KYTY_LOG_DEBUG("\t address          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
			KYTY_LOG_DEBUG("\t reference        = 0x%016" PRIx64 "\n", reference);
			KYTY_LOG_DEBUG("\t mask             = 0x%016" PRIx64 "\n", mask);
			KYTY_LOG_DEBUG("\t poll_cycles      = %" PRIu32 "\n", poll_cycles);

	if (buf == nullptr || size > 1 || compare_function > 7 || op > 4 || cache_policy > 3)
	{
		return nullptr;
	}

	buf->DbgDump();

	const uint32_t packet_dwords = (size == 0 ? 7u : 9u);
	auto*          cmd           = buf->AllocateDW(packet_dwords);

	if (cmd == nullptr)
	{
		return nullptr;
	}

	const uint64_t address_value = reinterpret_cast<uint64_t>(address);
	const uint32_t poll          = std::min(poll_cycles >> 4u, 0xffffu);

	cmd[0] = KYTY_PM4(packet_dwords, Pm4::IT_NOP, size == 0 ? Pm4::R_WAIT_MEM_32 : Pm4::R_WAIT_MEM_64);
	cmd[1] = static_cast<uint32_t>(address_value) & (size == 0 ? ~0x3u : ~0x7u);
	cmd[2] = static_cast<uint32_t>(address_value >> 32u) & 0x3ffffu;
	cmd[3] = static_cast<uint32_t>(mask);

	if (size == 0)
	{
		cmd[4] = static_cast<uint32_t>(reference);
		cmd[5] = 0x10u | (static_cast<uint32_t>(compare_function) & 0x7u) |
		         ((static_cast<uint32_t>(op) & 0x3u) << 8u) | ((static_cast<uint32_t>(op) & 0xcu) << 4u) |
		         ((static_cast<uint32_t>(cache_policy) & 0x3u) << 25u);
		cmd[6] = poll;
	} else
	{
		cmd[4] = static_cast<uint32_t>(mask >> 32u);
		cmd[5] = static_cast<uint32_t>(reference);
		cmd[6] = static_cast<uint32_t>(reference >> 32u);
		cmd[7] = 0x10u | (static_cast<uint32_t>(compare_function) & 0x7u) |
		         ((static_cast<uint32_t>(op) & 0x1u) << 8u) | ((static_cast<uint32_t>(op) & 0x6u) << 5u) |
		         ((static_cast<uint32_t>(cache_policy) & 0x3u) << 25u);
		cmd[8] = poll;
	}

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
			KYTY_LOG_DEBUG("\t op = 0x%08" PRIx32 "\n", op);

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
			KYTY_LOG_DEBUG("\t dst=0x%02" PRIx8 " src=0x%02" PRIx8 " dst_addr=0x%016" PRIx64 " src=0x%016" PRIx64 " item=%u conf=%u\n", dst, src,
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
			KYTY_LOG_DEBUG("\t index_addr = 0x%016" PRIx64 "\n", index_addr);

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
			KYTY_LOG_DEBUG("\t index_count = 0x%" PRIx32 "\n", index_count);

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
			KYTY_LOG_DEBUG("\t num_instances = 0x%" PRIx32 "\n", num_instances);

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
			KYTY_LOG_DEBUG("\t buffer = 0x%016" PRIx64 " size = %" PRIu32 "\n", reinterpret_cast<uint64_t>(buffer), buffer_size_in_bytes);

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

			KYTY_LOG_DEBUG("\t video_out_handle     = %" PRIu32 "\n", video_out_handle);
			KYTY_LOG_DEBUG("\t display_buffer_index = %" PRId32 "\n", display_buffer_index);
			KYTY_LOG_DEBUG("\t flip_mode            = %" PRIu32 "\n", flip_mode);
			KYTY_LOG_DEBUG("\t flip_arg             = %" PRId64 "\n", flip_arg);

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

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
