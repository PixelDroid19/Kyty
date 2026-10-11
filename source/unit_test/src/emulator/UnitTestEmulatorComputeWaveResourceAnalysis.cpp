#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderComputeWaveResourceAnalysis.h"

UT_BEGIN(EmulatorComputeWaveResourceAnalysis);

using namespace Libs::Graphics;

static ShaderInstruction MappedLoad()
{
	ShaderInstruction instruction {};
	instruction.pc                 = 4u;
	instruction.type               = ShaderInstructionType::SLoadDwordx4;
	instruction.format             = ShaderInstructionFormat::Sdst4SbaseSoffset;
	instruction.dst                = {.type = ShaderOperandType::Sgpr, .register_id = 16, .size = 4};
	instruction.src[0]             = {.type = ShaderOperandType::Sgpr, .register_id = 12, .size = 2};
	instruction.src[1].type        = ShaderOperandType::IntegerInlineConstant;
	instruction.src[1].constant.u  = 0x50u;
	instruction.src_num            = 2;
	instruction.smem_flags         = 0u;
	return instruction;
}

static ShaderBindResources MappedBinding()
{
	ShaderBindResources bind {};
	bind.extended.used                     = true;
	bind.extended.slot                     = 5;
	bind.extended.start_register           = 12;
	bind.extended.eud_user_sgpr_num        = 16;
	bind.extended.eud_size_dw              = 48u;
	bind.extended.eud_offset_base          = 32;
	bind.extended.data.UpdateAddress(0x1000u);
	bind.storage_buffers.buffers_num       = 1;
	bind.storage_buffers.dynamic_sload[0] = true;
	bind.storage_buffers.sources[0]       = ShaderStorageBindingSource::DynamicScalarLoad;
	ShaderDynamicSLoadMapping mapping {};
	mapping.kind                 = ShaderDynamicSLoadResourceKind::StorageBuffer;
	mapping.resource_index       = 0;
	mapping.destination_register = 16;
	mapping.instruction_pc       = 4u;
	mapping.offset_dw            = 20;
	mapping.dword_count           = 4;
	mapping.last_consumer_pc      = 8u;
	bind.dynamic_sloads.records.Add(mapping);
	return bind;
}

TEST(EmulatorComputeWaveResourceAnalysis, AdmitsExactMappedEudStorageLoad)
{
	const auto instruction = MappedLoad();
	const auto bind        = MappedBinding();
	EXPECT_TRUE(ShaderPairedEudStorageLoadSupported(instruction, bind));
	auto high_instruction = instruction;
	auto high_bind = bind;
	high_instruction.src[1].constant.u = 264u * 4u;
	high_bind.extended.eud_size_dw = 268u;
	high_bind.dynamic_sloads.records[0].offset_dw = 264;
	EXPECT_TRUE(ShaderPairedEudStorageLoadSupported(high_instruction, high_bind));
}

TEST(EmulatorComputeWaveResourceAnalysis, RejectsNonExactOrIncompleteMappings)
{
	const auto instruction = MappedLoad();
	const auto bind        = MappedBinding();
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, {}));

	auto changed_instruction = instruction;
	changed_instruction.type = ShaderInstructionType::SLoadDwordx8;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.dst.register_id = 17;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.dst.register_id = 104;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.dst.negate = true;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.src[1].size = 4;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	ShaderOperand* unused_operands[] = {&changed_instruction.dst2, &changed_instruction.src[2], &changed_instruction.src[3]};
	for (auto* operand: unused_operands)
	{
		changed_instruction = instruction;
		operand->type       = ShaderOperandType::Sgpr;
		EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	}
	changed_instruction = instruction;
	changed_instruction.vop3_op_sel = 1u;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.vop3_omod = 1u;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.vop_sdwa = true;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.ds_offset = 1u;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.ds_encoding_control = 1u;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.ds_encoding_registers = 1u;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.src[0].register_id++;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.src[1].constant.i = -4;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.src[1].constant.u++;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.smem_imm_offset = 4;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));
	changed_instruction = instruction;
	changed_instruction.smem_flags = 0xffu;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, bind));

	auto changed_bind = bind;
	changed_bind.dynamic_sloads.records[0].last_consumer_pc = instruction.pc;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.dynamic_sloads.records[0].offset_dw++;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.dynamic_sloads.records[0].resource_index = 1;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.storage_buffers.dynamic_sload[0] = false;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.storage_buffers.sources[0] = ShaderStorageBindingSource::DirectResource;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.dynamic_sloads.records[0].resource_field_offset = 1;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.extended.data.fields[0] = 0u;
	changed_bind.extended.data.fields[1] = 0u;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.extended.eud_size_dw = 0u;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.extended.slot = 1;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.extended.start_register = 32;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_bind = bind;
	changed_bind.extended.eud_offset_base = 16;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(instruction, changed_bind));
	changed_instruction = instruction;
	changed_instruction.src[1].constant.u = (SHADER_GEN5_EUD_MAX_DWORDS - 3u) * 4u;
	changed_bind = bind;
	changed_bind.dynamic_sloads.records[0].offset_dw = SHADER_GEN5_EUD_MAX_DWORDS - 3;
	changed_bind.extended.eud_size_dw = 1u;
	EXPECT_FALSE(ShaderPairedEudStorageLoadSupported(changed_instruction, changed_bind));
}

UT_END();
