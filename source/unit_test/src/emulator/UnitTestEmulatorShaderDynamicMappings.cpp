#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Log.h"
#include "Kyty/Core/VirtualMemory.h"
#include "../../../emulator/src/Graphics/ShaderStorageAnalysis.h"

#include <cstdlib>

UT_BEGIN(EmulatorShaderDynamicMappings);

using namespace Libs::Graphics;

[[noreturn]] static void RunSharedMappingsProbe()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	constexpr uint32_t kLoadCount = 65;
	constexpr uint32_t kFirstPc   = 8;
	constexpr uint32_t kPairSize  = 16;

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	for (uint32_t index = 0; index < kLoadCount; ++index)
	{
		const uint32_t pc = kFirstPc + index * kPairSize;
		ShaderInstruction sload {};
		sload.pc                 = pc;
		sload.type               = ShaderInstructionType::SLoadDwordx4;
		sload.format             = ShaderInstructionFormat::Sdst4SbaseSoffset;
		sload.dst                = {.type = ShaderOperandType::Sgpr, .register_id = 4, .size = 4};
		sload.src[0]             = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 2};
		sload.src[1].type        = ShaderOperandType::IntegerInlineConstant;
		sload.src[1].constant.u  = 160u;
		sload.src_num            = 2;
		code.GetInstructions().Add(sload);

		ShaderInstruction consumer {};
		consumer.pc                 = pc + 8u;
		consumer.type               = ShaderInstructionType::SBufferLoadDwordx4;
		consumer.format             = ShaderInstructionFormat::Sdst4SvSoffset;
		consumer.dst                = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 4};
		consumer.src[0]             = {.type = ShaderOperandType::Sgpr, .register_id = 4, .size = 4};
		consumer.src[1].type        = ShaderOperandType::IntegerInlineConstant;
		consumer.src[1].constant.u  = 0;
		consumer.src_num            = 2;
		code.GetInstructions().Add(consumer);
	}
	ShaderInstruction end {};
	end.pc   = kFirstPc + kLoadCount * kPairSize;
	end.type = ShaderInstructionType::SEndpgm;
	code.GetInstructions().Add(end);

	// Every load addresses the same valid EUD descriptor.
	alignas(16) uint32_t eud[64] = {};
	eud[40]                      = 0x00100000u;
	eud[41]                      = 4u << 16u;
	eud[42]                      = 16u;
	const uint64_t eud_ptr =
	    Core::VirtualMemory::Alloc(0, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite);
	if (eud_ptr == 0 || !Core::VirtualMemory::CopyToGuest(eud_ptr, eud, sizeof(eud))) { std::_Exit(2); }

	HW::UserSgprInfo user_sgpr {};
	for (int index = 0; index < 16; ++index)
	{
		user_sgpr.type[index] = HW::UserSgprType::Region;
	}
	user_sgpr.value[0] = static_cast<uint32_t>(eud_ptr);
	user_sgpr.value[1] = static_cast<uint32_t>(eud_ptr >> 32u);

	uint16_t direct_offsets[6];
	for (auto& offset: direct_offsets) { offset = 0xffffu; }
	direct_offsets[5] = 0;

	ShaderUserData user_data {};
	user_data.direct_resource_offset = direct_offsets;
	user_data.direct_resource_count  = 6;
	user_data.eud_size_dw            = 48;

	ShaderParsedUsage   usage {};
	ShaderBindResources bind {};
	ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 16, &code);

	if (!Core::VirtualMemory::Free(eud_ptr)) { std::_Exit(3); }
	if (bind.storage_buffers.buffers_num != 1 ||
	    bind.dynamic_sloads.records.Size() != kLoadCount)
	{
		std::_Exit(4);
	}
	for (uint32_t index = 0; index < kLoadCount; ++index)
	{
		const uint32_t pc = kFirstPc + index * kPairSize;
		const auto& record = bind.dynamic_sloads.records.At(index);
		if (record.kind != ShaderDynamicSLoadResourceKind::StorageBuffer || record.resource_index != 0 ||
		    record.destination_register != 4 || record.instruction_pc != pc || record.last_consumer_pc != pc + 8u)
		{
			std::_Exit(5);
		}
	}

	ShaderBindResources copied_bind = bind;
	copied_bind.dynamic_sloads.records[0].instruction_pc = 0xdeadbeefu;
	if (copied_bind.dynamic_sloads.records.At(0).instruction_pc != 0xdeadbeefu ||
	    bind.dynamic_sloads.records.At(0).instruction_pc != kFirstPc)
	{
		std::_Exit(6);
	}

	std::_Exit(0);
}

TEST(EmulatorShaderDynamicMappings, Preserves65LoadsThatShareOneStorageResource)
{
	ASSERT_EXIT(RunSharedMappingsProbe(), ::testing::ExitedWithCode(0), "");
}

static ShaderInstruction SplitTextureHalfLoad(uint32_t pc, int destination, uint32_t byte_offset)
{
	ShaderInstruction sload {};
	sload.pc                = pc;
	sload.type              = ShaderInstructionType::SLoadDwordx4;
	sload.format            = ShaderInstructionFormat::Sdst4SbaseSoffset;
	sload.dst               = {.type = ShaderOperandType::Sgpr, .register_id = destination, .size = 4};
	sload.src[0]            = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 2};
	sload.src[1].type       = ShaderOperandType::IntegerInlineConstant;
	sload.src[1].constant.u = byte_offset;
	sload.src_num           = 2;
	return sload;
}

// An image consumer whose T# s[8:15] is loaded from the EUD as two halves:
// low s[8:11] from dword 16 and high s[12:15] from dword 20. With a clobber,
// an S_MOV rewrites s9 between the loads and the consumer.
[[noreturn]] static void RunSplitTextureProbe(bool high_first, bool clobber, bool use_memo)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	const auto low  = SplitTextureHalfLoad(high_first ? 8u : 0u, 8, 64u);
	const auto high = SplitTextureHalfLoad(high_first ? 0u : 8u, 12, 80u);
	code.GetInstructions().Add(high_first ? high : low);
	code.GetInstructions().Add(high_first ? low : high);
	if (clobber)
	{
		ShaderInstruction mov {};
		mov.pc                = 16;
		mov.type              = ShaderInstructionType::SMovB32;
		mov.format            = ShaderInstructionFormat::SVdstSVsrc0;
		mov.dst               = {.type = ShaderOperandType::Sgpr, .register_id = 9, .size = 1};
		mov.src[0].type       = ShaderOperandType::IntegerInlineConstant;
		mov.src[0].constant.u = 0;
		mov.src_num           = 1;
		code.GetInstructions().Add(mov);
	}
	ShaderInstruction load {};
	load.pc             = 24;
	load.type           = ShaderInstructionType::ImageLoad;
	load.format         = ShaderInstructionFormat::Vdata4Vaddr3StDmaskF;
	load.dst            = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 4};
	load.src[0]         = {.type = ShaderOperandType::Vgpr, .register_id = 4, .size = 3};
	load.src[1]         = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 8};
	load.src_num        = 2;
	load.mimg_dimension = 1;
	load.mimg_dmask     = 15;
	code.GetInstructions().Add(load);
	ShaderInstruction end {};
	end.pc   = 32;
	end.type = ShaderInstructionType::SEndpgm;
	code.GetInstructions().Add(end);

	alignas(16) uint32_t eud[64] = {};
	for (uint32_t word = 0; word < 8; ++word)
	{
		eud[16 + word] = 0x1000u + word;
	}
	eud[19] = 9u << 28u; // 2D T# type in dword 3.
	const uint64_t eud_ptr =
	    Core::VirtualMemory::Alloc(0, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite);
	if (eud_ptr == 0 || !Core::VirtualMemory::CopyToGuest(eud_ptr, eud, sizeof(eud))) { std::_Exit(2); }

	HW::UserSgprInfo user_sgpr {};
	for (int index = 0; index < 16; ++index)
	{
		user_sgpr.type[index] = HW::UserSgprType::Region;
	}
	user_sgpr.value[0] = static_cast<uint32_t>(eud_ptr);
	user_sgpr.value[1] = static_cast<uint32_t>(eud_ptr >> 32u);

	uint16_t direct_offsets[6];
	for (auto& offset: direct_offsets) { offset = 0xffffu; }
	direct_offsets[5] = 0;

	ShaderUserData user_data {};
	user_data.direct_resource_offset = direct_offsets;
	user_data.direct_resource_count  = 6;
	user_data.eud_size_dw            = 48;

	// The memo is shared by draws of one program: the second parse reads its cached candidates.
	const ShaderCodeResourceMemo memo;
	ShaderParsedUsage            usage {};
	ShaderBindResources          bind {};
	for (int parse = 0; parse < (use_memo ? 2 : 1); ++parse)
	{
		usage = {};
		bind  = {};
		ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 16, &code, 0, false, nullptr, nullptr, nullptr,
		                  use_memo ? &memo : nullptr);
	}
	if (!Core::VirtualMemory::Free(eud_ptr)) { std::_Exit(3); }

	uint32_t texture_records = 0;
	for (const auto& record: bind.dynamic_sloads.records)
	{
		texture_records += record.kind == ShaderDynamicSLoadResourceKind::Texture ? 1u : 0u;
	}
	if (clobber)
	{
		std::_Exit(bind.textures2D.textures_num == 0 && texture_records == 0 ? 0 : 4);
	}
	if (bind.textures2D.textures_num != 1 || texture_records != 2 || !bind.textures2D.desc[0].dynamic_sload ||
	    bind.textures2D.desc[0].start_register != 8)
	{
		std::_Exit(5);
	}
	for (int field = 0; field < 8; ++field)
	{
		if (bind.textures2D.desc[0].texture.fields[field] != eud[16 + field]) { std::_Exit(6); }
	}
	for (const auto& record: bind.dynamic_sloads.records)
	{
		const bool low_record = record.instruction_pc == low.pc && record.offset_dw == 16 && record.resource_field_offset == 0;
		const bool high_record = record.instruction_pc == high.pc && record.offset_dw == 20 && record.resource_field_offset == 4;
		if (record.kind == ShaderDynamicSLoadResourceKind::Texture && !low_record && !high_record) { std::_Exit(7); }
	}
	std::_Exit(0);
}

TEST(EmulatorShaderDynamicMappings, ResolvesSplitEudTextureInEitherLoadOrder)
{
	ASSERT_EXIT(RunSplitTextureProbe(false, false, false), ::testing::ExitedWithCode(0), "");
	ASSERT_EXIT(RunSplitTextureProbe(true, false, false), ::testing::ExitedWithCode(0), "");
	ASSERT_EXIT(RunSplitTextureProbe(false, false, true), ::testing::ExitedWithCode(0), "");
	ASSERT_EXIT(RunSplitTextureProbe(true, false, true), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderDynamicMappings, RejectsSplitEudTextureRewrittenBeforeConsumer)
{
	ASSERT_EXIT(RunSplitTextureProbe(false, true, false), ::testing::ExitedWithCode(0), "");
	ASSERT_EXIT(RunSplitTextureProbe(false, true, true), ::testing::ExitedWithCode(0), "");
}

UT_END();
