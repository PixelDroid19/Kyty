#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Log.h"
#include "Kyty/Core/VirtualMemory.h"

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

UT_END();
