#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include <cstring>
#include <vector>

UT_BEGIN(EmulatorGraphicsPackets);

using namespace Libs::Graphics;

TEST(EmulatorGraphicsPackets, EncodesContiguousShRegisters)
{
	const ShaderRegister registers[] = {{0x20cu, 0x11111111u}, {0x20du, 0x22222222u}};
	uint32_t             command[4]  = {};

	ASSERT_EQ(Gen5::GraphicsGetShRegistersPacketSize(registers, 2), 4u);
	ASSERT_EQ(Gen5::GraphicsEncodeShRegisters(command, 4, registers, 2), 4u);
	EXPECT_EQ(command[0], KYTY_PM4(4, Pm4::IT_SET_SH_REG, 0));
	EXPECT_EQ(command[1], 0x20cu);
	EXPECT_EQ(command[2], 0x11111111u);
	EXPECT_EQ(command[3], 0x22222222u);
}

TEST(EmulatorGraphicsPackets, GroupsSortedShRegisters)
{
	const ShaderRegister registers[] = {{0x20fu, 0x33333333u}, {0x20cu, 0x11111111u}, {0x20du, 0x22222222u}};
	uint32_t             command[7]  = {};

	ASSERT_EQ(Gen5::GraphicsEncodeShRegisters(command, 7, registers, 3), 7u);
	EXPECT_EQ(command[0], KYTY_PM4(4, Pm4::IT_SET_SH_REG, 0));
	EXPECT_EQ(command[1], 0x20cu);
	EXPECT_EQ(command[2], 0x11111111u);
	EXPECT_EQ(command[3], 0x22222222u);
	EXPECT_EQ(command[4], KYTY_PM4(3, Pm4::IT_SET_SH_REG, 0));
	EXPECT_EQ(command[5], 0x20fu);
	EXPECT_EQ(command[6], 0x33333333u);
}

TEST(EmulatorGraphicsPackets, EncodesDispatch)
{
	uint32_t command[5] = {};

	ASSERT_EQ(Gen5::GraphicsEncodeDispatch(command, 5, 2, 3, 4, 0), 5u);
	EXPECT_EQ(command[0], KYTY_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0));
	EXPECT_EQ(command[1], 2u);
	EXPECT_EQ(command[2], 3u);
	EXPECT_EQ(command[3], 4u);
	EXPECT_EQ(command[4], 0x41u);
}

TEST(EmulatorGraphicsPackets, TreatsZeroPm4DwordAsSinglePadding)
{
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0u), 1u);
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0x01fe0000u), 2u);
}

TEST(EmulatorGraphicsPackets, ParsesGen5LshlAddU32)
{
	const uint32_t shader[] = {0xd7460003u, 0x040a0300u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::VLshlAddU32);
	EXPECT_EQ(instruction.format, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2);
	EXPECT_EQ(instruction.dst.type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.dst.register_id, 3);
	EXPECT_EQ(instruction.src[0].register_id, 0);
	EXPECT_EQ(instruction.src[1].register_id, 1);
	EXPECT_EQ(instruction.src[2].register_id, 2);
}

// RDNA2 VOP3 v_add3_u32 (op 0x36D): dst = src0 + src1 + src2.
TEST(EmulatorGraphicsPackets, ParsesGen5Add3U32)
{
	// Same layout as ParsesGen5LshlAddU32 with opcode field set to 0x36D.
	const uint32_t shader[] = {0xd76d0003u, 0x040a0300u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::VAdd3U32);
	EXPECT_EQ(instruction.format, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2);
	EXPECT_EQ(instruction.dst.register_id, 3);
	EXPECT_EQ(instruction.src[0].register_id, 0);
	EXPECT_EQ(instruction.src[1].register_id, 1);
	EXPECT_EQ(instruction.src[2].register_id, 2);
}

// VOP2 SDWA with SRC0_NEG (bit 20 of SDWA control). Captured post-Play path
// hits EXIT_NOT_IMPLEMENTED(src0_neg != 0) until negate is wired like VOP3.
TEST(EmulatorGraphicsPackets, ParsesVop2SdwaSrc0Negate)
{
	// v_add_f32 v0, |−v2|, v1 with SDWA: src0=SDWA(249), opcode=3.
	const uint32_t word0 = (0x03u << 25u) | (0u << 17u) | (1u << 9u) | 249u;
	const uint32_t word1 = 2u | (6u << 8u) | (6u << 16u) | (1u << 20u) | (0u << 23u) | (6u << 24u);
	const uint32_t shader[] = {word0, word1, 0xbf800000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::VAddF32);
	EXPECT_EQ(instruction.src[0].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.src[0].register_id, 2);
	EXPECT_TRUE(instruction.src[0].negate);
	EXPECT_FALSE(instruction.src[0].absolute);
	EXPECT_EQ(instruction.src[1].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.src[1].register_id, 1);
	EXPECT_FALSE(instruction.src[1].negate);
}

// VOP2 SDWA OMOD uses the same output modifier contract as VOP3: 1=x2,
// 2=x4, 3=x0.5. Keep it in the parsed dst so each backend instruction can
// either apply it or reject it explicitly.
TEST(EmulatorGraphicsPackets, ParsesVop2SdwaOmod)
{
	// v_mul_f32 v0, v2, v1 with SDWA OMOD=2 (x4).
	const uint32_t word0 = (0x08u << 25u) | (0u << 17u) | (1u << 9u) | 249u;
	const uint32_t word1 = 2u | (6u << 8u) | (2u << 14u) | (6u << 16u) | (0u << 23u) | (6u << 24u);
	const uint32_t shader[] = {word0, word1, 0xbf800000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::VMulF32);
	EXPECT_EQ(instruction.dst.type, ShaderOperandType::Vgpr);
	EXPECT_FLOAT_EQ(instruction.dst.multiplier, 4.0f);

	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;
	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
	EXPECT_NE(source.FindIndex("%m200_0 = OpFMul %float %m197_0 %float_4_000000"), Core::STRING8_INVALID_INDEX);
}

TEST(EmulatorGraphicsPackets, ParsesVopcSdwaAbsolute)
{
	// v_cmp_gt_f32 |v2|, v1 with SDWA: VOP2 opcode 0x3e dispatches VOPC.
	const uint32_t word0 = (0x3eu << 25u) | (0x04u << 17u) | (1u << 9u) | 249u;
	const uint32_t word1 = 2u | (6u << 16u) | (1u << 21u) | (0u << 23u) | (6u << 24u);
	const uint32_t shader[] = {word0, word1, 0xbf800000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::VCmpGtF32);
	EXPECT_TRUE(instruction.src[0].absolute);

	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;
	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
	EXPECT_NE(source.FindIndex("OpExtInst %float %GLSL_std_450 FAbs"), Core::STRING8_INVALID_INDEX);
}

// Captured post-Play Gen5 VOP2 word 0x00000009 at pc=0x64:
// OP=0, vdst=v0, vsrc1=v0, src0=s9 → v_cndmask_b32 v0, s9, v0 (legacy OP encoding).
TEST(EmulatorGraphicsPackets, ParsesGen5Vop2Op0AsCndmask)
{
	const uint32_t shader[] = {0x00000009u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::VCndmaskB32);
	EXPECT_EQ(instruction.format, ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2);
	EXPECT_EQ(instruction.dst.type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.dst.register_id, 0);
	EXPECT_EQ(instruction.src[0].type, ShaderOperandType::Sgpr);
	EXPECT_EQ(instruction.src[0].register_id, 9);
	EXPECT_EQ(instruction.src[1].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.src[1].register_id, 0);
	EXPECT_EQ(instruction.src[2].type, ShaderOperandType::VccLo);
	EXPECT_EQ(instruction.src_num, 3);
}

// GFX10/RDNA VOP2 opcode 0x25 is v_add_u32/v_add_nc_u32: no carry/VCC dst.
TEST(EmulatorGraphicsPackets, ParsesGen5Vop2Opcode25AsAddNoCarry)
{
	const uint32_t word0 = (0x25u << 25u) | (0u << 17u) | (1u << 9u) | (2u + 256u);
	const uint32_t shader[] = {word0, 0xbf800000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::VAddI32);
	EXPECT_EQ(instruction.format, ShaderInstructionFormat::SVdstSVsrc0SVsrc1);
	EXPECT_EQ(instruction.dst2.type, ShaderOperandType::Unknown);
	EXPECT_EQ(instruction.src[0].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.src[0].register_id, 2);
	EXPECT_EQ(instruction.src[1].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.src[1].register_id, 1);

	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;
	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
	EXPECT_NE(source.FindIndex("OpIAdd %uint"), Core::STRING8_INVALID_INDEX);
}

TEST(EmulatorGraphicsPackets, ParsesBufferStoreFormatXyzw)
{
	const uint32_t shader[] = {0xe01c2000u, 0x80010004u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::BufferStoreFormatXyzw);
	EXPECT_EQ(instruction.format, ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen);
	EXPECT_EQ(instruction.dst.type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.dst.register_id, 0);
	EXPECT_EQ(instruction.dst.size, 4);
	EXPECT_EQ(instruction.src[0].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.src[0].register_id, 4);
	EXPECT_EQ(instruction.src[1].type, ShaderOperandType::Sgpr);
	EXPECT_EQ(instruction.src[1].register_id, 4);
	EXPECT_EQ(instruction.src[1].size, 4);
	EXPECT_EQ(instruction.src[2].type, ShaderOperandType::IntegerInlineConstant);
	EXPECT_EQ(instruction.src[2].constant.u, 0u);
}

// MUBUF buffer_load_dwordx4 with 12-bit offset folded into a LiteralConstant
// soffset. SPIR-V must OpStore the offset into %temp_int_* as %int_N (not
// %uint_N), and FindConstants must register the Int twin for that literal.
TEST(EmulatorGraphicsPackets, MubufLiteralSoffsetUsesIntConstant)
{
	// op=0x0E buffer_load_dwordx4, idxen=1, offset=56, soffset=inline 0 (128),
	// srsrc=2 → s[8:11], vdata=v30, vaddr=v43; s_nop then s_endpgm.
	const uint32_t word0 = (0x38u << 26u) | (0x0Eu << 18u) | (1u << 13u) | 56u;
	const uint32_t word1 = (128u << 24u) | (2u << 16u) | (30u << 8u) | 43u;
	const uint32_t shader[] = {word0, word1, 0xbf800000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_GE(code.GetInstructions().Size(), 1u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::BufferLoadDwordx4);
	EXPECT_EQ(instruction.src[2].type, ShaderOperandType::LiteralConstant);
	EXPECT_EQ(instruction.src[2].constant.u, 56u);

	ShaderComputeInputInfo input {};
	input.threads_num[0]                         = 1;
	input.threads_num[1]                         = 1;
	input.threads_num[2]                         = 1;
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 8;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadOnly;
	input.bind.push_constant_size                = 16;

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);

	EXPECT_NE(source.FindIndex("%int_56 = OpConstant %int 56"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %temp_int_2 %int_56"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("OpStore %temp_int_2 %uint_56"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("unknown_int_constant"), Core::STRING8_INVALID_INDEX);
}

TEST(EmulatorGraphicsPackets, ClassifiesDirectBufferStoreAsReadWrite)
{
	const uint32_t shader[] = {0xe01c2000u, 0x80000004u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	EXPECT_EQ(ShaderGetDirectStorageUsage(code, 0), ShaderStorageUsage::ReadWrite);
	EXPECT_EQ(ShaderGetDirectStorageUsage(code, 4), ShaderStorageUsage::Unknown);
}

TEST(EmulatorGraphicsPackets, ReusesFourComponentBufferFunctionType)
{
	const uint32_t shader[] = {0xd7460003u, 0x040a0300u, 0xd7460003u, 0x040a0300u, 0xe01c2000u, 0x80000004u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ShaderComputeInputInfo input {};
	input.threads_num[0]                         = 1;
	input.threads_num[1]                         = 1;
	input.threads_num[2]                         = 1;
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 0;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadWrite;
	input.bind.push_constant_size                = 16;
	const auto source                            = SpirvGenerateSource(code, nullptr, nullptr, &input);

	EXPECT_NE(source.FindIndex("%function_buffer_load_store_float4 = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%function_buffer_load_float4 = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%function_buffer_store_float4 = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%function_tbuffer_load_store_format_xyzw = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%function_tbuffer_load_format_xyzw = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%function_tbuffer_store_format_xyzw = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpSGreaterThanEqual %bool %tbuf_s_f_xyzw_11 %int_75"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpSLessThanEqual %bool %tbuf_s_f_xyzw_11 %int_77"), Core::STRING8_INVALID_INDEX);
}

TEST(EmulatorGraphicsPackets, StructuresBackwardSBranchAsLoopHeader)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);

	ShaderInstruction nop;
	nop.pc                = 0;
	nop.type              = ShaderInstructionType::SInstPrefetch;
	nop.format            = ShaderInstructionFormat::Imm;
	nop.src_num           = 1;
	nop.src[0].type       = ShaderOperandType::LiteralConstant;
	nop.src[0].constant.u = 0;

	ShaderInstruction header;
	header.pc                = 4;
	header.type              = ShaderInstructionType::SInstPrefetch;
	header.format            = ShaderInstructionFormat::Imm;
	header.src_num           = 1;
	header.src[0].type       = ShaderOperandType::LiteralConstant;
	header.src[0].constant.u = 0;

	ShaderInstruction branch;
	branch.pc                = 8;
	branch.type              = ShaderInstructionType::SBranch;
	branch.format            = ShaderInstructionFormat::Label;
	branch.src_num           = 1;
	branch.src[0].type       = ShaderOperandType::LiteralConstant;
	branch.src[0].constant.i = -8;

	ShaderInstruction end;
	end.pc     = 12;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;

	code.GetInstructions().Add(nop);
	code.GetInstructions().Add(header);
	code.GetInstructions().Add(branch);
	code.GetInstructions().Add(end);
	code.GetLabels().Add(ShaderLabel(branch));

	ShaderComputeInputInfo input {};
	input.threads_num[0] = 1;
	input.threads_num[1] = 1;
	input.threads_num[2] = 1;

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);

	EXPECT_NE(source.FindIndex("OpBranch %label_0004_0008"), Core::STRING8_INVALID_INDEX);
	// OpLoopMerge must be immediately followed by a branch into the body block.
	const auto merge_idx = source.FindIndex("OpLoopMerge %loop_merge_0008 %loop_continue_0008 None");
	EXPECT_NE(merge_idx, Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpBranch %loop_body_0008", merge_idx), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%loop_body_0008 = OpLabel", merge_idx), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpBranch %loop_continue_0008"), Core::STRING8_INVALID_INDEX);
	const auto merge_label_idx = source.FindIndex("%loop_merge_0008 = OpLabel");
	EXPECT_NE(merge_label_idx, Core::STRING8_INVALID_INDEX);
	// Merge of an unconditional backward branch is unreachable; it must end
	// with a terminator before any subsequent guest block label.
	EXPECT_NE(source.FindIndex("OpUnreachable", merge_label_idx), Core::STRING8_INVALID_INDEX);
}

TEST(EmulatorGraphicsPackets, UsesForwardSBranchBeforeTargetAsSelectionMerge)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);

	auto make_nop = [](uint32_t pc)
	{
		ShaderInstruction inst;
		inst.pc                = pc;
		inst.type              = ShaderInstructionType::SInstPrefetch;
		inst.format            = ShaderInstructionFormat::Imm;
		inst.src_num           = 1;
		inst.src[0].type       = ShaderOperandType::LiteralConstant;
		inst.src[0].constant.u = 0;
		return inst;
	};

	auto make_branch = [](uint32_t pc, ShaderInstructionType type, int32_t imm)
	{
		ShaderInstruction inst;
		inst.pc                = pc;
		inst.type              = type;
		inst.format            = ShaderInstructionFormat::Label;
		inst.src_num           = 1;
		inst.src[0].type       = ShaderOperandType::LiteralConstant;
		inst.src[0].constant.i = imm;
		return inst;
	};

	auto outer      = make_branch(0x00, ShaderInstructionType::SCbranchVccz, 0x1c);   // -> 0x20
	auto inner      = make_branch(0x08, ShaderInstructionType::SCbranchExecz, 0x0c); // -> 0x18
	auto then_exit  = make_branch(0x10, ShaderInstructionType::SBranch, 0x1c);       // -> 0x30
	auto else_exit  = make_branch(0x1c, ShaderInstructionType::SBranch, 0x10);       // -> 0x30
	ShaderInstruction end;
	end.pc     = 0x30;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;

	code.GetInstructions().Add(outer);
	code.GetInstructions().Add(make_nop(0x04));
	code.GetInstructions().Add(inner);
	code.GetInstructions().Add(make_nop(0x0c));
	code.GetInstructions().Add(then_exit);
	code.GetInstructions().Add(make_nop(0x18));
	code.GetInstructions().Add(else_exit);
	code.GetInstructions().Add(make_nop(0x20));
	code.GetInstructions().Add(make_nop(0x24));
	code.GetInstructions().Add(end);
	code.GetLabels().Add(ShaderLabel(outer));
	code.GetLabels().Add(ShaderLabel(inner));
	code.GetLabels().Add(ShaderLabel(then_exit));
	code.GetLabels().Add(ShaderLabel(else_exit));

	ShaderComputeInputInfo input {};
	input.threads_num[0] = 1;
	input.threads_num[1] = 1;
	input.threads_num[2] = 1;

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);

	EXPECT_NE(source.FindIndex("OpSelectionMerge %label_0030_001c None"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("OpSelectionMerge %label_0020_0000 None"), Core::STRING8_INVALID_INDEX);
}

TEST(EmulatorGraphicsPackets, ClassifiesGen5FourComponent32BitBufferFormats)
{
	EXPECT_FALSE(ShaderIsGen5FourComponent32BitBufferFormat(74));
	EXPECT_TRUE(ShaderIsGen5FourComponent32BitBufferFormat(75));
	EXPECT_TRUE(ShaderIsGen5FourComponent32BitBufferFormat(76));
	EXPECT_TRUE(ShaderIsGen5FourComponent32BitBufferFormat(77));
	EXPECT_FALSE(ShaderIsGen5FourComponent32BitBufferFormat(78));
}

TEST(EmulatorGraphicsPackets, AllowsRegionScalarsOnlyInsideSrtRange)
{
	ShaderUserData no_srt {};

	EXPECT_TRUE(ShaderCanBindDirectSgpr(&no_srt, 4, HW::UserSgprType::Region));

	ShaderUserData user_data {};
	user_data.srt_size_dw = 8;

	EXPECT_TRUE(ShaderCanBindDirectSgpr(&user_data, 4, HW::UserSgprType::Region));
	EXPECT_FALSE(ShaderCanBindDirectSgpr(&user_data, 8, HW::UserSgprType::Region));
	EXPECT_TRUE(ShaderCanBindDirectSgpr(&user_data, 8, HW::UserSgprType::Unknown));
}

TEST(EmulatorGraphicsPackets, AcceptsFullUserSgprWriteWindow)
{
	// Observed frontiers:
	//  - PS SET_SH_REG reg_num == 16 at slot 0 (full classic window)
	//  - PS SET_SH_REG reg_num == 30 at slot 0 (cmd_id 0xc01e7600 Gen5 MSB path)
	EXPECT_EQ(HW::UserSgprInfo::SGPRS_MAX, 32);
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(0, 16));
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(0, 30));
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(0, 32));
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(0, 1));
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(15, 1));
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(31, 1));
	EXPECT_FALSE(HW::UserSgprInfo::WriteRangeValid(0, 0));
	EXPECT_FALSE(HW::UserSgprInfo::WriteRangeValid(0, 33));
	EXPECT_FALSE(HW::UserSgprInfo::WriteRangeValid(1, 32));
	EXPECT_FALSE(HW::UserSgprInfo::WriteRangeValid(32, 1));
	// Register indices: PS_0=0xC, PS_31=0x2B (contiguous 32 dwords).
	EXPECT_EQ(Pm4::SPI_SHADER_USER_DATA_PS_31, Pm4::SPI_SHADER_USER_DATA_PS_0 + 31u);
}

TEST(EmulatorGraphicsPackets, ReportsType3Pm4PacketSizeInDwords)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	// sceAgcGetPacketSize (Lkf86B98qPc): type-3 header → dword length.
	const uint32_t wait_flip = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE);
	EXPECT_EQ(wait_flip, 0xC0051018u);
	EXPECT_EQ(Gen5::GraphicsGetDataPacketSizeDw(&wait_flip), 7u);

	const uint32_t release_mem = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);
	EXPECT_EQ(release_mem, 0xC0051060u);
	EXPECT_EQ(Gen5::GraphicsGetDataPacketSizeDw(&release_mem), 7u);

	const uint32_t wait_mem64 = KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64);
	EXPECT_EQ(wait_mem64, 0xC0071058u);
	EXPECT_EQ(Gen5::GraphicsGetDataPacketSizeDw(&wait_mem64), 9u);

	EXPECT_EQ(Gen5::GraphicsGetDataPacketSizeDw(nullptr), 0u);
}

TEST(EmulatorGraphicsPackets, PatchesReleaseMemEndOfPipeAddress)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t command[7] = {};
	command[0]           = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);

	EXPECT_EQ(Gen5::GraphicsAgcQueueEndOfPipeActionPatchAddress(command, 0x123456789abcdef0ull), 0);
	EXPECT_EQ(command[3], 0x9abcdef0u);
	EXPECT_EQ(command[4], 0x12345678u);

	command[0] = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64);
	EXPECT_NE(Gen5::GraphicsAgcQueueEndOfPipeActionPatchAddress(command, 1), 0);
	EXPECT_EQ(Gen5::GraphicsAgcQueueEndOfPipeActionPatchAddress(nullptr, 1), Libs::LibKernel::KERNEL_ERROR_EINVAL);
}

// Observed post-Play: guest encodes WaitMem/ReleaseMem with a placeholder then
// patches the address through GetDataPacketPayloadAddress. WaitMem stores the
// 64-bit address in the first body dwords (cmd+1); ReleaseMem stores it after
// action/gcr (cmd+3). Default consumers keep the historical cmd+2 payload.
TEST(EmulatorGraphicsPackets, ResolvesDataPacketPayloadAddressByOpcode)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t  wait_mem[9]    = {};
	uint32_t  release_mem[7] = {};
	uint32_t  write_data[6]  = {};
	uint32_t* payload        = nullptr;

	wait_mem[0] = KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64);
	ASSERT_EQ(Gen5::GraphicsGetDataPacketPayloadAddress(&payload, wait_mem, 1), 0);
	EXPECT_EQ(payload, wait_mem + 1);

	wait_mem[0] = KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_WAIT_MEM_32);
	ASSERT_EQ(Gen5::GraphicsGetDataPacketPayloadAddress(&payload, wait_mem, 0), 0);
	EXPECT_EQ(payload, wait_mem + 1);

	release_mem[0] = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);
	ASSERT_EQ(Gen5::GraphicsGetDataPacketPayloadAddress(&payload, release_mem, 1), 0);
	EXPECT_EQ(payload, release_mem + 3);

	// WriteData keeps the historical payload base at cmd+2 (address field).
	write_data[0] = KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_WRITE_DATA);
	ASSERT_EQ(Gen5::GraphicsGetDataPacketPayloadAddress(&payload, write_data, 1), 0);
	EXPECT_EQ(payload, write_data + 2);
}

// Post-Play load path: WriteData with cache_policy=2 packs into the same
// control layout as cache_policy=0 (dst | policy<<8 | increment<<16 | confirm<<24).
TEST(EmulatorGraphicsPackets, EncodesWriteDataCachePolicy2ControlWord)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	uint32_t             data[2]     = {0x11111111u, 0x22222222u};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	uint32_t* cmd = Gen5::GraphicsDcbWriteData(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 4, 2, 0x124e80f40ull, data, 2, 0, 1);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_WRITE_DATA));
	EXPECT_EQ(cmd[1], 0x01000204u);
	EXPECT_EQ(cmd[2], 0x24e80f40u);
	EXPECT_EQ(cmd[3], 0x00000001u);
	EXPECT_EQ(cmd[4], 0x11111111u);
	EXPECT_EQ(cmd[5], 0x22222222u);
}

// DrawIndexAuto accepts modifier 0x80000000 (Astro post-compute) as well as
// the historical 0x40000000 default.
TEST(EmulatorGraphicsPackets, EncodesDrawIndexAutoModifier80000000)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	uint32_t* cmd = Gen5::GraphicsDcbDrawIndexAuto(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 1u, 0x80000000ull);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(3, Pm4::IT_DRAW_INDEX_AUTO, 0u));
	EXPECT_EQ(cmd[1], 1u);
	EXPECT_EQ(cmd[2], 0x2u);
}

// Gen5 type-2 pad (NID qj7QZpgr9Uw): single 0x80000000 dword.
TEST(EmulatorGraphicsPackets, EncodesCbType2Pad)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[8] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 8;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 8;

	uint32_t* cmd = Gen5::GraphicsCbType2Pad(reinterpret_cast<Gen5::CommandBuffer*>(&cb));
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], 0x80000000u);
	EXPECT_EQ(cb.cursor_up, storage + 1);
}

// sceAgcDcbSetBaseIndirectArgs: IT_SET_BASE with 8-byte-aligned address.
TEST(EmulatorGraphicsPackets, EncodesDcbSetBaseIndirectArgs)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	uint32_t* cmd =
	    Gen5::GraphicsDcbSetBaseIndirectArgs(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 1u, 0x00000005074063c0ull);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(4, Pm4::IT_SET_BASE, 0u) | (1u << 1u));
	EXPECT_EQ(cmd[1], 1u);
	EXPECT_EQ(cmd[2], 0x074063c0u);
	EXPECT_EQ(cmd[3], 0x00000005u);
}

// sceAgcDcbDispatchIndirect: IT_DISPATCH_INDIRECT with modifier mask.
TEST(EmulatorGraphicsPackets, EncodesDcbDispatchIndirect)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[8] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 8;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 8;

	uint32_t* cmd = Gen5::GraphicsDcbDispatchIndirect(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0u, 1u);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(3, Pm4::IT_DISPATCH_INDIRECT, 0u));
	EXPECT_EQ(cmd[1], 0u);
	EXPECT_EQ(cmd[2], (1u & 0xa038u) | 0x41u);
}

// sceAgcDcbDrawIndexIndirect with modifier 0x80000000 (Astro after DrawIndexAuto).
TEST(EmulatorGraphicsPackets, EncodesDcbDrawIndexIndirectModifier80000000)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	uint32_t* cmd =
	    Gen5::GraphicsDcbDrawIndexIndirect(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0u, 0x80000000ull);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(5, Pm4::IT_DRAW_INDEX_INDIRECT, 0u));
	EXPECT_EQ(cmd[1], 0u);
	// Default patch offsets when modifier low bits are clear: 0x280 / 0x280.
	EXPECT_EQ(cmd[2], 0x280u);
	EXPECT_EQ(cmd[3], 0x280u);
	EXPECT_EQ(cmd[4], 2u);
}

// Placeholder address 0 is reserved then patched before submit (Astro path).
TEST(EmulatorGraphicsPackets, EncodesWriteDataZeroAddressThenPatches)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	uint32_t             data[1]     = {0xdeadbeefu};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	uint32_t* cmd = Gen5::GraphicsDcbWriteData(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 1, 0, 0, data, 1, 0, 0);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(5, Pm4::IT_NOP, Pm4::R_WRITE_DATA));
	EXPECT_EQ(cmd[2], 0u);
	EXPECT_EQ(cmd[3], 0u);
	EXPECT_EQ(cmd[4], 0xdeadbeefu);

	EXPECT_EQ(Gen5::GraphicsWriteDataPatchSetAddressOrOffset(cmd, 0x0000000123456780ull), 0);
	EXPECT_EQ(cmd[2], 0x23456780u);
	EXPECT_EQ(cmd[3], 0x00000001u);
}

// The host CP normalizes guest waits into R_WAIT_MEM_64. A size=0 guest wait
// must keep its 32-bit predicate when its reference/mask arguments carry
// upper bits that the guest packet width does not consume.
TEST(EmulatorGraphicsPackets, Encodes32BitWaitWithInactiveUpperPredicate)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	uint32_t* cmd = Gen5::GraphicsDcbWaitRegMem(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0, 3, 0, 0,
	                                             reinterpret_cast<const volatile void*>(0x00000001268815d0ull),
	                                             0xfeedface00000001ull, 0xffffffffffffffffull, 400);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64));
	EXPECT_EQ(cmd[1], 0x268815d0u);
	EXPECT_EQ(cmd[2], 0x00000001u);
	EXPECT_EQ(cmd[3], 0xffffffffu);
	EXPECT_EQ(cmd[4], 0u);
	EXPECT_EQ(cmd[5], 1u);
	EXPECT_EQ(cmd[6], 0u);
	EXPECT_EQ(cmd[7], 3u);
	EXPECT_EQ(cmd[8], 10u);
}

// WaitRegMem cache_policy is not packed into R_WAIT_MEM_64; stream (1),
// bypass-style (2), and observed policy 3 must still encode the same poll packet.
TEST(EmulatorGraphicsPackets, EncodesWaitRegMemCachePolicy1And2SamePacket)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	auto encode = [](uint8_t policy) {
		uint32_t             storage[16] = {};
		AlignasCommandBuffer cb {};
		cb.bottom      = storage;
		cb.top         = storage + 16;
		cb.cursor_up   = storage;
		cb.cursor_down = storage + 16;
		uint32_t* cmd  = Gen5::GraphicsDcbWaitRegMem(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0, 3, 0, policy,
		                                             reinterpret_cast<const volatile void*>(0x00000001268815d0ull),
		                                             0x0000000000000001ull, 0x00000000ffffffffull, 400);
		EXPECT_NE(cmd, nullptr);
		EXPECT_EQ(cmd[0], KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64));
		EXPECT_EQ(cmd[1], 0x268815d0u);
		EXPECT_EQ(cmd[5], 1u);
		EXPECT_EQ(cmd[7], 3u);
		return cmd[8];
	};

	EXPECT_EQ(encode(1), 10u);
	EXPECT_EQ(encode(2), 10u);
	EXPECT_EQ(encode(3), 10u);
}

// Post-Play: WaitMem address stays 0; preceding ReleaseMem is EopPatched.
// Resolve Wait to the Release address when packets are contiguous.
TEST(EmulatorGraphicsPackets, ResolvesNullWaitMemAddressFromPrecedingRelease)
{
	uint32_t stream[16] = {};
	// ReleaseMem with patched label address 0x00000001268815d0.
	stream[0] = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);
	stream[1] = 0x14u;
	stream[2] = 0x00010000u; // data_sel=1
	stream[3] = 0x268815d0u;
	stream[4] = 0x00000001u;
	stream[5] = 1u;
	stream[6] = 0u;
	// WaitMem64 with null address (body starts at stream[8]).
	stream[7]  = KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64);
	stream[8]  = 0u;
	stream[9]  = 0u;
	stream[10] = 0xffffffffu;
	stream[11] = 0u;
	stream[12] = 1u;
	stream[13] = 0u;
	stream[14] = 3u;
	stream[15] = 10u;

	uint64_t* resolved = Gen5::GraphicsResolveWaitMemAddressFromPrecedingRelease(stream + 8);
	ASSERT_NE(resolved, nullptr);
	EXPECT_EQ(reinterpret_cast<uint64_t>(resolved), 0x00000001268815d0ull);

	// Non-contiguous / wrong previous packet → no invent.
	EXPECT_EQ(Gen5::GraphicsResolveWaitMemAddressFromPrecedingRelease(stream + 1), nullptr);
	stream[3] = 0;
	stream[4] = 0;
	EXPECT_EQ(Gen5::GraphicsResolveWaitMemAddressFromPrecedingRelease(stream + 8), nullptr);
}

// Submit-time fence publish: ReleaseMem data_sel=1 must store the 32-bit
// immediate so WaitRegMem can observe it even when the ring worker is blocked.
TEST(EmulatorGraphicsPackets, PublishesReleaseMemDataSel1FenceOnScan)
{
	uint32_t fence = 0;
	uint32_t stream[8] = {};
	stream[0] = KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);
	stream[1] = 0x14u;                           // action
	stream[2] = 0x00010000u;                     // gcr=0 | data_sel=1
	stream[3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fence));
	stream[4] = static_cast<uint32_t>(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&fence)) >> 32u);
	stream[5] = 1u; // data_lo
	stream[6] = 0u;
	stream[7] = 0u;

	EXPECT_EQ(GraphicsPm4PublishFenceProducers(stream, 8), 1u);
	EXPECT_EQ(fence, 1u);

	// data_sel=0 is barrier-only: no store.
	stream[2] = 0u;
	fence     = 0;
	EXPECT_EQ(GraphicsPm4PublishFenceProducers(stream, 8), 0u);
	EXPECT_EQ(fence, 0u);
}

// WriteData memory destinations also publish fences at submit time.
TEST(EmulatorGraphicsPackets, PublishesWriteDataFenceOnScan)
{
	uint32_t words[2] = {0, 0};
	uint32_t stream[6] = {};
	stream[0] = KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_WRITE_DATA);
	stream[1] = 0x01000004u; // dst=4, write_confirm=1
	stream[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(words));
	stream[3] = static_cast<uint32_t>(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(words)) >> 32u);
	stream[4] = 0x11111111u;
	stream[5] = 0x22222222u;

	EXPECT_EQ(GraphicsPm4PublishFenceProducers(stream, 6), 1u);
	EXPECT_EQ(words[0], 0x11111111u);
	EXPECT_EQ(words[1], 0x22222222u);
}

// Encoder accepts data_sel=1 (32-bit immediate). Packet layout stores data_sel
// in bits 23:16 of dword 2 and the immediate in dwords 5..6.
TEST(EmulatorGraphicsPackets, EncodesReleaseMemDataSel1Immediate32)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	// Observed post-Play: action=0x14, gcr=0, data_sel=1, data=1.
	const auto* label = reinterpret_cast<const volatile Gen5::Label*>(static_cast<uintptr_t>(0x100));
	uint32_t*   cmd =
	    Gen5::GraphicsCbReleaseMem(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x14, 0, 1, 0, label, 1, 1, 0, 1, 0, 0);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM));
	EXPECT_EQ(cmd[1], 0x14u);
	EXPECT_EQ(cmd[2], 0x00010000u); // gcr=0 | data_sel=1 << 16
	EXPECT_EQ(cmd[3], 0x100u);
	EXPECT_EQ(cmd[4], 0u);
	EXPECT_EQ(cmd[5], 1u);
	EXPECT_EQ(cmd[6], 0u);
	EXPECT_EQ(cmd[7], 0u);
}

// data_sel 0: barrier/flush only (no label write). Matches CP custom path.
TEST(EmulatorGraphicsPackets, EncodesReleaseMemDataSel0Barrier)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	const auto* label = reinterpret_cast<const volatile Gen5::Label*>(static_cast<uintptr_t>(0x100));
	uint32_t*   cmd =
	    Gen5::GraphicsCbReleaseMem(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x28, 0, 1, 0, label, 0, 0, 0, 0, 0, 0);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM));
	EXPECT_EQ(cmd[1], 0x28u);
	EXPECT_EQ(cmd[2], 0x00000000u); // gcr=0 | data_sel=0 << 16
	EXPECT_EQ(cmd[3], 0x100u);
	EXPECT_EQ(cmd[4], 0u);
	EXPECT_EQ(cmd[5], 0u);
	EXPECT_EQ(cmd[6], 0u);
	EXPECT_EQ(cmd[7], 0u);
}

// Immediate ReleaseMem does not encode GDS; gds_size 0 (unused) matches gds_size 1.
TEST(EmulatorGraphicsPackets, EncodesReleaseMemDataSel1WithUnusedGdsSize0)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	const auto* label = reinterpret_cast<const volatile Gen5::Label*>(static_cast<uintptr_t>(0x100));
	uint32_t*   cmd =
	    Gen5::GraphicsCbReleaseMem(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x14, 0, 1, 0, label, 1, 1, 0, 0, 0, 0);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM));
	EXPECT_EQ(cmd[2], 0x00010000u);
	EXPECT_EQ(cmd[5], 1u);
	EXPECT_EQ(cmd[7], 0u);
}

// Clock-counter form with non-zero interrupt selector (Gen5 after Resident Load).
TEST(EmulatorGraphicsPackets, EncodesReleaseMemDataSel3WithInterrupt)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	const auto* label = reinterpret_cast<const volatile Gen5::Label*>(static_cast<uintptr_t>(0x200));
	uint32_t*   cmd =
	    Gen5::GraphicsCbReleaseMem(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x28, 0, 1, 0, label, 3, 0, 0, 1, 1, 0x11u);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM));
	EXPECT_EQ(cmd[1], 0x28u);
	EXPECT_EQ(cmd[2], 0x01030000u); // interrupt=1 << 24 | data_sel=3 << 16
	EXPECT_EQ(cmd[3], 0x200u);
	EXPECT_EQ(cmd[7], 0x11u);
}

TEST(EmulatorGraphicsPackets, EncodesReleaseMemDataSel1WithNullDestination)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	uint32_t* cmd = Gen5::GraphicsCbReleaseMem(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x14, 0, 1, 0, nullptr, 1, 0, 0, 0, 0, 0);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_RELEASE_MEM));
	EXPECT_EQ(cmd[1], 0x14u);
	EXPECT_EQ(cmd[2], 0x00010000u);
	EXPECT_EQ(cmd[3], 0u);
	EXPECT_EQ(cmd[4], 0u);
	EXPECT_EQ(cmd[5], 0u);
	EXPECT_EQ(cmd[6], 0u);
	EXPECT_EQ(cmd[7], 0u);
}

TEST(EmulatorGraphicsPackets, SizesGen5RotatedXRenderTargets)
{
	TileSizeAlign size {};
	TileGetRenderTargetSize(161, 109, 161, 0x1b, 4, &size);

	EXPECT_EQ(size.size, 131072u);
	EXPECT_EQ(size.align, 65536u);

	TileGetRenderTargetSize(1920, 1080, 1920, 0x1b, 4, &size);
	EXPECT_EQ(size.size, 8847360u);
}

// Observed post-Play sample texture: format 56, 157x102, tile 27 (SW_64KB_R_X).
// Size must match the render-target 64 KiB block geometry, not a linear span.
TEST(EmulatorGraphicsPackets, SizesGen5RotatedXSampleTextures)
{
	TileSizeAlign size {};
	TileGetTextureSize2(56, 157, 102, 157, 1, 27, &size, nullptr, nullptr);

	EXPECT_EQ(size.size, 131072u);
	EXPECT_EQ(size.align, 65536u);

	// Pitch 0 means "use width" for block pitch — same total as explicit width.
	TileSizeAlign size_zero_pitch {};
	TileGetTextureSize2(56, 157, 102, 0, 1, 27, &size_zero_pitch, nullptr, nullptr);
	EXPECT_EQ(size_zero_pitch.size, size.size);
	EXPECT_EQ(size_zero_pitch.align, size.align);
}

// Captured first fail after GPU chain: format 56, 800x320, tile 27 → 0x150000.
TEST(EmulatorGraphicsPackets, SizesGen5RotatedXTexture800x320)
{
	TileSizeAlign size {};
	TileGetTextureSize2(56, 800, 320, 800, 1, 27, &size, nullptr, nullptr);
	EXPECT_EQ(size.size, 0x150000u);
	EXPECT_EQ(size.align, 65536u);
}

// Gen5 sampled format 133 maps to BC1: 4x4 texel blocks, 8 bytes per block.
// The tile-27 allocation is computed in block elements, not source texels.
TEST(EmulatorGraphicsPackets, SizesGen5RotatedXSampledBc1Texture)
{
	TileSizeAlign size {};
	TilePaddedSize padded {};
	TileGetTextureSize2(133, 3840, 2160, 3840, 1, 27, &size, nullptr, &padded);

	// block_width=960, block_height=540, 8-BPE tile blocks are 128x64:
	// ceil(960/128)=8, ceil(540/64)=9, size=72*64 KiB.
	EXPECT_EQ(size.size, 72u * 65536u);
	EXPECT_EQ(size.align, 65536u);
	EXPECT_EQ(padded.width, 512u * 8u);
	EXPECT_EQ(padded.height, 256u * 9u);
}

// Gen5 kRenderTarget (tile 27) within-block addressing for 4 BPE must be a
// bijection over a 128x128 block and keep every texel inside the 64 KiB block.
// Golden low offsets match the guest RT bit equations (not Mesa ADDRLIB).
TEST(EmulatorGraphicsPackets, Sw64kRx4bppWithinBlockIsBijective)
{
	constexpr uint32_t k_block = 128u;
	bool               seen[65536] {};
	uint32_t           unique = 0;
	for (uint32_t y = 0; y < k_block; y++)
	{
		for (uint32_t x = 0; x < k_block; x++)
		{
			const uint64_t off = TileGetSw64kRxOffset(x, y, k_block, 4);
			ASSERT_LT(off, 65536u);
			ASSERT_EQ(off % 4u, 0u);
			ASSERT_FALSE(seen[off]);
			seen[off] = true;
			unique++;
		}
	}
	EXPECT_EQ(unique, k_block * k_block);
	EXPECT_EQ(TileGetSw64kRxOffset(0, 0, k_block, 4), 0u);
	EXPECT_EQ(TileGetSw64kRxOffset(1, 0, k_block, 4), 4u);
	EXPECT_EQ(TileGetSw64kRxOffset(0, 1, k_block, 4), 0x8u);
	EXPECT_EQ(TileGetSw64kRxOffset(1, 1, k_block, 4), 0xcu);
}

// kStandard64KB (tile 9) 32bpp uses a different within-block interleave.
TEST(EmulatorGraphicsPackets, Standard64KB32WithinBlockIsBijective)
{
	constexpr uint32_t k_block = 128u;
	bool               seen[65536] {};
	uint32_t           unique = 0;
	for (uint32_t y = 0; y < k_block; y++)
	{
		for (uint32_t x = 0; x < k_block; x++)
		{
			const uint64_t off = TileGetStandard64KB32Offset(x, y, k_block);
			ASSERT_LT(off, 65536u);
			ASSERT_EQ(off % 4u, 0u);
			ASSERT_FALSE(seen[off]);
			seen[off] = true;
			unique++;
		}
	}
	EXPECT_EQ(unique, k_block * k_block);
	EXPECT_EQ(TileGetStandard64KB32Offset(0, 0, k_block), 0u);
	EXPECT_EQ(TileGetStandard64KB32Offset(1, 0, k_block), 4u);
	EXPECT_EQ(TileGetStandard64KB32Offset(0, 1, k_block), 0x10u);
	// Distinct from kRenderTarget: (0,1) is 0x10 here, 0x8 for tile 27.
	EXPECT_NE(TileGetStandard64KB32Offset(0, 1, k_block), TileGetSw64kRxOffset(0, 1, k_block, 4));
}

TEST(EmulatorGraphicsPackets, Sw64kRx8bppWithinBlockIsBijective)
{
	constexpr uint32_t k_block_w = 128u;
	constexpr uint32_t k_block_h = 64u;
	bool               seen[65536] {};
	uint32_t           unique = 0;
	for (uint32_t y = 0; y < k_block_h; y++)
	{
		for (uint32_t x = 0; x < k_block_w; x++)
		{
			const uint64_t off = TileGetSw64kRxOffset(x, y, k_block_w, 8);
			ASSERT_LT(off, 65536u);
			ASSERT_EQ(off % 8u, 0u);
			ASSERT_FALSE(seen[off]);
			seen[off] = true;
			unique++;
		}
	}
	EXPECT_EQ(unique, k_block_w * k_block_h);
	EXPECT_EQ(TileGetSw64kRxOffset(0, 0, k_block_w, 8), 0u);
	EXPECT_EQ(TileGetSw64kRxOffset(1, 0, k_block_w, 8), 8u);
	EXPECT_EQ(TileGetSw64kRxOffset(0, 1, k_block_w, 8), 0x10u);
	EXPECT_EQ(TileGetSw64kRxOffset(1, 1, k_block_w, 8), 0x18u);
	EXPECT_EQ(TileGetSw64kRxOffset(0, 64, k_block_w, 8), 65536u);
}

// Detile round-trip: tile a gradient with the inverse of the offset function
// and recover the original linear image.
TEST(EmulatorGraphicsPackets, Sw64kRx4bppDetileRoundTrip)
{
	constexpr uint32_t k_w     = 160u;
	constexpr uint32_t k_h     = 96u;
	constexpr uint32_t k_bpp   = 4u;
	TileSizeAlign      size {};
	TileGetRenderTargetSize(k_w, k_h, k_w, 0x1b, k_bpp, &size);
	ASSERT_GT(size.size, 0u);

	std::vector<uint8_t> linear(static_cast<size_t>(k_w) * k_h * k_bpp);
	std::vector<uint8_t> tiled(size.size, 0);
	std::vector<uint8_t> out(linear.size(), 0xAAu);

	for (uint32_t y = 0; y < k_h; y++)
	{
		for (uint32_t x = 0; x < k_w; x++)
		{
			const uint32_t v   = (x * 3u + y * 5u) & 0xffu;
			const size_t   idx = (static_cast<size_t>(y) * k_w + x) * k_bpp;
			linear[idx + 0]    = static_cast<uint8_t>(v);
			linear[idx + 1]    = static_cast<uint8_t>(v ^ 0x11u);
			linear[idx + 2]    = static_cast<uint8_t>(v ^ 0x22u);
			linear[idx + 3]    = 0xffu;
			const uint64_t off = TileGetSw64kRxOffset(x, y, k_w, k_bpp);
			ASSERT_LT(off + k_bpp, static_cast<uint64_t>(size.size) + 1u);
			std::memcpy(tiled.data() + off, linear.data() + idx, k_bpp);
		}
	}

	TileConvertSw64kRxToLinear(out.data(), tiled.data(), k_w, k_h, k_w, k_bpp);
	EXPECT_EQ(out, linear);

	// Multi-block: second block along X starts at 64 KiB for 128-wide blocks.
	EXPECT_EQ(TileGetSw64kRxOffset(128, 0, k_w, k_bpp), 65536u);
}

// Functional detile: RGBA8 ramp R=x across two 64 KiB macro columns (pitch=256).
TEST(EmulatorGraphicsPackets, Sw64kRx4bppDetileRampPreservesX)
{
	constexpr uint32_t k_w   = 256u;
	constexpr uint32_t k_h   = 64u;
	constexpr uint32_t k_bpp = 4u;
	TileSizeAlign      size {};
	TileGetRenderTargetSize(k_w, k_h, k_w, 0x1b, k_bpp, &size);
	ASSERT_GT(size.size, 0u);

	std::vector<uint8_t> linear(static_cast<size_t>(k_w) * k_h * k_bpp);
	std::vector<uint8_t> tiled(size.size, 0);
	std::vector<uint8_t> out(linear.size(), 0xAAu);

	for (uint32_t y = 0; y < k_h; y++)
	{
		for (uint32_t x = 0; x < k_w; x++)
		{
			const size_t i     = (static_cast<size_t>(y) * k_w + x) * k_bpp;
			linear[i + 0]      = static_cast<uint8_t>(x & 0xffu);
			linear[i + 1]      = 0;
			linear[i + 2]      = 0;
			linear[i + 3]      = 0xff;
			const uint64_t off = TileGetSw64kRxOffset(x, y, k_w, k_bpp);
			ASSERT_LT(off + k_bpp, tiled.size() + 1);
			std::memcpy(tiled.data() + off, linear.data() + i, k_bpp);
		}
	}

	TileConvertSw64kRxToLinear(out.data(), tiled.data(), k_w, k_h, k_w, k_bpp);
	for (uint32_t y = 0; y < k_h; y++)
	{
		for (uint32_t x = 0; x < k_w; x++)
		{
			const size_t i = (static_cast<size_t>(y) * k_w + x) * k_bpp;
			EXPECT_EQ(out[i + 0], static_cast<uint8_t>(x & 0xffu)) << "x=" << x << " y=" << y;
		}
	}
}

// Macro pitch: pitch_elems=256 with width=192 still advances one 64 KiB block per
// 128 rows of y (blocks_x = ceil(256/128) = 2).
TEST(EmulatorGraphicsPackets, Sw64kRx4bppMacroPitchAdvancesBlock)
{
	EXPECT_EQ(TileGetSw64kRxOffset(0, 128, 256, 4), 65536u * 2u);
	EXPECT_EQ(TileGetSw64kRxOffset(0, 128, 192, 4), 65536u * 2u);
	EXPECT_EQ(TileGetSw64kRxOffset(0, 128, 128, 4), 65536u);
}

// Captured world GBuffer class: 642x362 tile 27 RGBA8 (fmt 56) size must match
// RT size so FindRenderTexture exact alias can hit.
TEST(EmulatorGraphicsPackets, SizesGen5RotatedXGbuffer642x362Rgba8MatchesSample56)
{
	TileSizeAlign rt {};
	TileGetRenderTargetSize(642, 362, 642, 0x1b, 4, &rt);
	TileSizeAlign tex {};
	TileGetTextureSize2(56, 642, 362, 642, 1, 27, &tex, nullptr, nullptr);
	EXPECT_EQ(rt.size, tex.size);
	EXPECT_EQ(rt.align, tex.align);
	EXPECT_GT(rt.size, 0u);
}

// Post-Play DCB fragment after CB/DB meta EVENT_WRITE: a run of Type0
// single-register writes (10 dwords) then WaitFlipDone (0xC0051018). Walking
// with Pm4NonType3PacketDwords must land on the Type3 header.
TEST(EmulatorGraphicsPackets, WalksGen5Type0RunBeforeWaitFlipDone)
{
	const uint32_t stream[] = {
	    0x00000001u, 0x00000007u, 0x00000007u, 0x00000000u, 0x00000080u, 0x00000080u, 0x00000001u, 0x00000001u,
	    0x01fe0000u, 0x00000000u, 0xc0051018u, 0x00000000u, 0x00000003u, 0x00000000u, 0x00000000u, 0x00000000u,
	    0x00000000u,
	};

	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0xc0051018u), 0u); // Type3
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0x80000000u), 1u); // Type2
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0x00000001u), 2u); // Type0
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0x01fe0000u), 2u); // Type0 (single body)

	uint32_t off = 0;
	while (off < 10u)
	{
		const uint32_t step = Pm4::Pm4NonType3PacketDwords(stream[off]);
		ASSERT_EQ(step, 2u);
		ASSERT_LE(off + step, static_cast<uint32_t>(sizeof(stream) / sizeof(stream[0])));
		off += step;
	}
	EXPECT_EQ(off, 10u);
	EXPECT_EQ(stream[off], 0xc0051018u);
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(stream[off]), 0u);
}

// Gen5 streams can interleave Type1 2-dword units after EVENT_WRITE
// with Type0 before WaitFlipDone. COUNT in the Type1 header is not the body size.
TEST(EmulatorGraphicsPackets, WalksGen5Type1PairsBeforeWaitFlipDone)
{
	const uint32_t stream[] = {
	    0xc0004600u, 0x0000002cu, 0x7d0703e0u, 0x00007fccu, 0x00000003u, 0x00000000u, 0x7d070440u, 0x00007fccu,
	    0x00000003u, 0x00000000u, 0x7d070440u, 0x00007fccu, 0xc0051018u, 0x00000000u, 0x00000000u, 0x00000000u,
	    0x00000000u, 0x00000000u, 0x00000000u,
	};

	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0x7d0703e0u), 2u);
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0x7d070440u), 2u);
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0x00000003u), 2u);

	// Skip leading EVENT_WRITE (Type3): header + 1 body.
	uint32_t off = 2;
	while (off < 12u)
	{
		const uint32_t step = Pm4::Pm4NonType3PacketDwords(stream[off]);
		ASSERT_EQ(step, 2u);
		off += step;
	}
	EXPECT_EQ(off, 12u);
	EXPECT_EQ(stream[off], 0xc0051018u);
}

// Some Gen5 2-dword units have type bits 11 but
// an impossible COUNT (e.g. 0xf84d2e90). Walking must treat them like Type0/1
// pairs so the stream lands on WaitFlipDone.
TEST(EmulatorGraphicsPackets, WalksGen5OversizedType3PairsBeforeWaitFlipDone)
{
	const uint32_t stream[] = {
	    0xc0004600u, 0x0000002eu, 0xc0004600u, 0x0000002cu, 0xf84d2e90u, 0x00007f9bu, 0x0bbb68c0u, 0x00000003u,
	    0xf84d3300u, 0x00007f9bu, 0x15dd6a84u, 0x00007ff8u, 0xf84d3360u, 0x00007f9bu, 0xc0051018u, 0x00000000u,
	    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	};

	const uint32_t total = static_cast<uint32_t>(sizeof(stream) / sizeof(stream[0]));
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0xf84d2e90u, total - 4u), 2u);
	EXPECT_EQ(Pm4::Pm4NonType3PacketDwords(0xc0051018u, 7u), 0u);

	// Skip two EVENT_WRITE packets (header + body each).
	uint32_t off = 4;
	while (off < 14u)
	{
		const uint32_t step = Pm4::Pm4NonType3PacketDwords(stream[off], total - off);
		ASSERT_EQ(step, 2u);
		off += step;
	}
	EXPECT_EQ(off, 14u);
	EXPECT_EQ(stream[off], 0xc0051018u);
}

// PKT3_ONE_REG_WRITE (0x57) encodes register selection in the
// header, so the normal PKT3 COUNT field is not a payload length. Treat it as
// header + one value dword so the following WaitFlipDone packet stays aligned.
TEST(EmulatorGraphicsPackets, WalksGen5OneRegWriteBeforeWaitFlipDone)
{
	const uint32_t stream[] = {
	    0xc0004600u, 0x0000002cu, 0x00000000u, 0x00000000u, 0xc15857a0u, 0x00000001u, 0x00000000u, 0x00000000u,
	    0x0000001eu, 0x00000000u, 0x00000001u, 0x00000000u, 0xc0051018u, 0x00000000u, 0x00000000u, 0x00000000u,
	    0x00000000u, 0x00000000u, 0x00000000u,
	};

	EXPECT_EQ(Pm4::Pm4SpecialType3PacketDwords(0xc15857a0u), 2u);
	EXPECT_EQ(Pm4::Pm4SpecialType3PacketDwords(0xc0051018u), 0u);

	uint32_t off = 2; // after EVENT_WRITE
	while (off < 12u)
	{
		const uint32_t special = Pm4::Pm4SpecialType3PacketDwords(stream[off]);
		const uint32_t step    = (special != 0u) ? special : Pm4::Pm4NonType3PacketDwords(stream[off]);
		ASSERT_NE(step, 0u);
		off += step;
	}
	EXPECT_EQ(off, 12u);
	EXPECT_EQ(stream[off], 0xc0051018u);
}

TEST(EmulatorGraphicsPackets, RecognizesOpaqueType3PairOnlyBeforeWaitFlipDone)
{
	const uint32_t stream[] = {
	    0xc3b50072u, 0xa6b5a527u, 0x00000014u, 0x00000000u, 0xc0051018u, 0x00000000u,
	    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	};
	const uint32_t no_wait[] = {0xc3b50072u, 0xa6b5a527u, 0x00000014u, 0x00000000u};

	EXPECT_TRUE(Pm4::Pm4Gen5OpaquePairPrecedesWaitFlipDone(stream, static_cast<uint32_t>(std::size(stream))));
	EXPECT_FALSE(Pm4::Pm4Gen5OpaquePairPrecedesWaitFlipDone(no_wait, static_cast<uint32_t>(std::size(no_wait))));
	EXPECT_FALSE(Pm4::Pm4Gen5OpaquePairPrecedesWaitFlipDone(nullptr, 0u));
}

TEST(EmulatorGraphicsPackets, AllocatesCommandBufferDwords)
{
	// Layout matches Gen5::CommandBuffer in Graphics.cpp (non-virtual methods).
	// NID LtTouSCZjHM is sceAgcCbNop: (CommandBuffer*, num_dw) → dword* with type-3 NOP.
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	uint32_t             storage[64] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 64;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 64;

	EXPECT_EQ(Gen5::GraphicsCbAllocateDwords(nullptr, 10), nullptr);
	EXPECT_EQ(Gen5::GraphicsCbAllocateDwords(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0), nullptr);

	uint32_t* first = Gen5::GraphicsCbAllocateDwords(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 10);
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(first, storage);
	EXPECT_EQ(cb.cursor_up, storage + 10);
	EXPECT_EQ(first[0], KYTY_PM4(10, Pm4::IT_NOP, Pm4::R_ZERO));

	uint32_t* second = Gen5::GraphicsCbAllocateDwords(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 4);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(second, storage + 10);
	EXPECT_EQ(cb.cursor_up, storage + 14);
	EXPECT_EQ(second[0], KYTY_PM4(4, Pm4::IT_NOP, Pm4::R_ZERO));
}

// Gen5 DCB resetQueue: 12-bit op mask, IT_CLEAR_STATE + low 4 bits of state.
TEST(EmulatorGraphicsPackets, EncodesDcbResetQueueAsClearState)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	// Historical only-accepted value 0x3ff still must encode CLEAR_STATE.
	uint32_t* cmd = Gen5::GraphicsDcbResetQueue(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x3ffu, 0u);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(2, Pm4::IT_CLEAR_STATE, 0u));
	EXPECT_EQ(cmd[1], 0u);

	// Astro-style non-0x3ff op with non-zero state selector.
	uint32_t* cmd2 = Gen5::GraphicsDcbResetQueue(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x001u, 0x5u);
	ASSERT_NE(cmd2, nullptr);
	EXPECT_EQ(cmd2[0], KYTY_PM4(2, Pm4::IT_CLEAR_STATE, 0u));
	EXPECT_EQ(cmd2[1], 0x5u);

	// State is masked to 4 bits in the body dword.
	uint32_t* cmd3 = Gen5::GraphicsDcbResetQueue(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0x0ffu, 0x1fu);
	ASSERT_NE(cmd3, nullptr);
	EXPECT_EQ(cmd3[1], 0xfu);
}

// GraphicsDcbAcquireMem encodes size_bytes == -1 as size_lo == 0 (full range)
// while still writing base >> 8. Observed post-Play full-target barrier:
// cache_action 0x06007fc0, base_lo=1, size_lo=0 (guest base 0x100, size -1).
TEST(EmulatorGraphicsPackets, EncodesFullRangeAcquireMemWithNonZeroBase)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[64] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 64;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 64;

	const auto* base = reinterpret_cast<const volatile void*>(static_cast<uintptr_t>(0x100));
	uint32_t*   cmd  = Gen5::GraphicsDcbAcquireMem(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0, 0x06007fc0u, 0x280u, base,
	                                               static_cast<uint64_t>(-1), 400);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_ACQUIRE_MEM));
	EXPECT_EQ(cmd[1], 0x06007fc0u);
	EXPECT_EQ(cmd[2], 0u); // size_lo: full-range sentinel
	EXPECT_EQ(cmd[3], 0u);
	EXPECT_EQ(cmd[4], 1u); // base_lo: 0x100 >> 8
	EXPECT_EQ(cmd[5], 0u);
	EXPECT_EQ(cmd[6], 10u); // poll_cycles / 40
	EXPECT_EQ(cmd[7], 0x280u);
}

// GFX linear surfaces pad rows to 256 bytes. For RGBA8 (format 56) that is a
// 64-texel pitch alignment — width alone is wrong for non-pow2 sizes.
TEST(EmulatorGraphicsPackets, AlignsGen5LinearTexturePitchTo256ByteRows)
{
	// 348-wide RGBA8 logo: 348*4 = 1392 -> next 256-byte boundary is 1536 bytes = 384 texels.
	EXPECT_EQ(ShaderGen5LinearTexturePitch(348, 56), 384u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(1, 56), 64u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(64, 56), 64u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(65, 56), 128u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(1280, 56), 1280u);
	// Format 14 = UFMT_8_8_UNORM (2 Bpp): 256/2 = 128-texel row alignment.
	EXPECT_EQ(ShaderGen5TextureBytesPerElement(14), 2u);
	EXPECT_EQ(ShaderGen5TextureBytesPerElement(56), 4u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(1, 14), 128u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(128, 14), 128u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(129, 14), 256u);
	// Captured loading atlas: 2048x4096 RG8 linear (format 14, tile 0).
	EXPECT_EQ(ShaderGen5LinearTexturePitch(2048, 14), 2048u);
}

// Captured post-Play sample texture: format 14 (RG8), 2048x4096, linear tile 0.
// Size must be pitch*height*2 with 256-byte row alignment (pitch == width here).
TEST(EmulatorGraphicsPackets, SizesGen5LinearRg8Texture2048x4096)
{
	TileSizeAlign size {};
	const uint32_t width  = 2048;
	const uint32_t height = 4096;
	const uint32_t pitch  = ShaderGen5LinearTexturePitch(width, 14);
	TileGetTextureSize2(14, width, height, pitch, 1, 0, &size, nullptr, nullptr);
	EXPECT_EQ(pitch, 2048u);
	EXPECT_EQ(size.size, static_cast<uint64_t>(pitch) * height * 2u);
	EXPECT_NE(size.size, 0u);
}

// Captured post-Play intermediate RT: 642x362 (attrib2 0x281/0x169), tile 0x1b,
// COLOR_16_16_16_16 (format 0xc) → 8 Bpp, SW_64KB_R_X block grid 128x64.
// Matching sample texture format 71 (UFMT_16_16_16_16_FLOAT) must size equal so
// FindRenderTexture can alias the RT.
TEST(EmulatorGraphicsPackets, SizesGen5RotatedXRenderTargetRgba16Float)
{
	TileSizeAlign size {};
	const uint32_t width  = 0x281u + 1u;
	const uint32_t height = 0x169u + 1u;
	EXPECT_EQ(width, 642u);
	EXPECT_EQ(height, 362u);
	TileGetRenderTargetSize(width, height, width, 0x1bu, 8u, &size);
	// blocks_x = ceil(642/128)=6, blocks_y = ceil(362/64)=6, size = 6*6*65536
	EXPECT_EQ(size.size, 6u * 6u * 65536u);
	EXPECT_EQ(size.align, 65536u);
	EXPECT_EQ(ShaderGen5TextureBytesPerElement(71), 8u);
	TileSizeAlign tex {};
	TileGetTextureSize2(71, width, height, width, 1, 27, &tex, nullptr, nullptr);
	EXPECT_EQ(tex.size, size.size);
	EXPECT_EQ(tex.align, size.align);
}

// CB_COLOR0_INFO.ROUND_MODE is bit 18; captured post-Play RTs set it.
// Decoder uses KYTY_PM4_GET — keep the shift/mask contract locked.
TEST(EmulatorGraphicsPackets, DecodesCbColorInfoRoundModeBit)
{
	const uint32_t round_on  = 1u << Pm4::CB_COLOR0_INFO_ROUND_MODE_SHIFT;
	const uint32_t round_off = 0u;
	EXPECT_EQ(Pm4::CB_COLOR0_INFO_ROUND_MODE_SHIFT, 18u);
	EXPECT_EQ(Pm4::CB_COLOR0_INFO_ROUND_MODE_MASK, 1u);
	EXPECT_EQ(KYTY_PM4_GET(round_on, CB_COLOR0_INFO, ROUND_MODE), 1u);
	EXPECT_EQ(KYTY_PM4_GET(round_off, CB_COLOR0_INFO, ROUND_MODE), 0u);
	// Adjacent blend_bypass (bit 16) must not alias into ROUND_MODE.
	const uint32_t blend_bypass = 1u << Pm4::CB_COLOR0_INFO_BLEND_BYPASS_SHIFT;
	EXPECT_EQ(KYTY_PM4_GET(blend_bypass, CB_COLOR0_INFO, ROUND_MODE), 0u);
}

// image_sample_lz (MIMG op 0x27) with dmask 0xf — observed after PlayGo/Resident.
TEST(EmulatorGraphicsPackets, ParsesImageSampleLzDmaskF)
{
	const uint32_t word0 = (0x3cu << 26u) | (0x27u << 18u) | (0xfu << 8u);
	const uint32_t shader[] = {word0, 0u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::ImageSampleLz);
	EXPECT_EQ(code.GetInstructions().At(0).format, ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF);
	EXPECT_EQ(code.GetInstructions().At(0).dst.size, 4);
}

// image_sample (MIMG op 0x20) with single-channel dmasks — captured post-Play
// with dmask 0x4 then 0x2 at sequential PCs.
TEST(EmulatorGraphicsPackets, ParsesImageSampleSingleChannelDmasks)
{
	// MIMG encoding: bits[31:26]=0x3c, opcode bits[24:18], dmask bits[11:8].
	const uint32_t enc = 0x3cu << 26u;
	const uint32_t dmask4 = enc | (0x20u << 18u) | (0x4u << 8u);
	const uint32_t dmask2 = enc | (0x20u << 18u) | (0x2u << 8u);
	const uint32_t word1  = 0u;
	const uint32_t shader[] = {dmask4, word1, dmask2, word1, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::ImageSample);
	EXPECT_EQ(code.GetInstructions().At(0).format, ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4);
	EXPECT_EQ(code.GetInstructions().At(0).dst.size, 1);
	EXPECT_EQ(code.GetInstructions().At(1).type, ShaderInstructionType::ImageSample);
	EXPECT_EQ(code.GetInstructions().At(1).format, ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2);
	EXPECT_EQ(code.GetInstructions().At(1).dst.size, 1);
}

// Captured after NGS2 voice fix: image_sample dmask 0xb (R+G+A) at PC 0x40.
TEST(EmulatorGraphicsPackets, ParsesImageSampleDmaskB)
{
	const uint32_t word0 = (0x3cu << 26u) | (0x20u << 18u) | (0xbu << 8u);
	const uint32_t shader[] = {word0, 0u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::ImageSample);
	EXPECT_EQ(code.GetInstructions().At(0).format, ShaderInstructionFormat::Vdata3Vaddr3StSsDmaskB);
	EXPECT_EQ(code.GetInstructions().At(0).dst.size, 3);
}

// image_sample dmask 0xa materializes the G and A channels.
TEST(EmulatorGraphicsPackets, ParsesAndMaterializesImageSampleDmaskA)
{
	const uint32_t word0 = (0x3cu << 26u) | (0x20u << 18u) | (0xau << 8u);
	const uint32_t shader[] = {word0, 0u, 0xbf800000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::ImageSample);
	EXPECT_EQ(code.GetInstructions().At(0).format, ShaderInstructionFormat::Vdata2Vaddr3StSsDmaskA);
	EXPECT_EQ(code.GetInstructions().At(0).dst.size, 2);

	ShaderPixelInputInfo input {};
	input.bind.push_constant_size                = 48;
	input.bind.textures2D.textures_num           = 1;
	input.bind.textures2D.textures2d_sampled_num = 1;
	input.bind.textures2D.desc[0].start_register = 8;
	input.bind.samplers.samplers_num             = 1;
	input.bind.samplers.start_register[0]        = 20;
	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);

	EXPECT_NE(source.FindIndex("OpAccessChain %_ptr_Function_float %temp_v4float %uint_1"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpAccessChain %_ptr_Function_float %temp_v4float %uint_3"), Core::STRING8_INVALID_INDEX);
}

// Captured Gen5 MIMG-NSA image_sample instructions. The third dword supplies
// ADDR1, so these coordinates are v2/v5 and v5/v4 rather than contiguous
// v2/v3 and v5/v6. The following instruction must start after all three dwords.
TEST(EmulatorGraphicsPackets, ParsesImageSampleNonSequentialAddresses)
{
	const uint32_t shader[] = {
	    0xf080040au, 0x00a20602u, 0x00000005u,
	    0xf0800f0au, 0x00800005u, 0x00000004u,
	    0xbf810000u,
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	const auto& first = code.GetInstructions().At(0);
	EXPECT_EQ(first.pc, 0u);
	EXPECT_EQ(first.type, ShaderInstructionType::ImageSample);
	EXPECT_EQ(first.mimg_address_num, 5);
	EXPECT_EQ(first.mimg_address[0].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(first.mimg_address[0].register_id, 2);
	EXPECT_EQ(first.mimg_address[1].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(first.mimg_address[1].register_id, 5);

	const auto& second = code.GetInstructions().At(1);
	EXPECT_EQ(second.pc, 12u);
	EXPECT_EQ(second.type, ShaderInstructionType::ImageSample);
	EXPECT_EQ(second.mimg_address_num, 5);
	EXPECT_EQ(second.mimg_address[0].register_id, 5);
	EXPECT_EQ(second.mimg_address[1].register_id, 4);
	EXPECT_EQ(code.GetInstructions().At(2).pc, 24u);
}

TEST(EmulatorGraphicsPackets, MaterializesImageSampleNonSequentialCoordinates)
{
	const uint32_t shader[] = {
	    0xbf800000u,
	    0xf080040au, 0x00a20602u, 0x00000005u,
	    0xbf810000u,
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ShaderPixelInputInfo input {};
	input.bind.push_constant_size                   = 48;
	input.bind.textures2D.textures_num              = 1;
	input.bind.textures2D.textures2d_sampled_num    = 1;
	input.bind.textures2D.desc[0].start_register    = 8;
	input.bind.samplers.samplers_num                = 1;
	input.bind.samplers.start_register[0]           = 20;
	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);

	EXPECT_NE(source.FindIndex("OpLoad %float %v2"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpLoad %float %v5"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("OpLoad %float %v3"), Core::STRING8_INVALID_INDEX);
}

TEST(EmulatorGraphicsPackets, MaterializesPixelDirectSgprPushConstant)
{
	const uint32_t shader[] = {0xbf800000u, 0xbf800000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ShaderPixelInputInfo input {};
	input.bind.direct_sgprs.sgprs_num         = 1;
	input.bind.direct_sgprs.start_register[0] = 4;
	input.bind.push_constant_size             = 16;

	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);

	EXPECT_NE(source.FindIndex("%s4 = OpVariable %_ptr_Function_uint Function"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s4"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpAccessChain %_ptr_PushConstant_uint %vsharp"), Core::STRING8_INVALID_INDEX);
}

// Gen5 SMEM opcode 0x3: s_load_dwordx8 s[4:11], s[0:1], 0 — captured at PC 0x18.
TEST(EmulatorGraphicsPackets, ParsesSmembSLoadDwordx8)
{
	// Same envelope as MaterializesExtendedSLoadDwordAndX2 x2 word, opcode 1→3.
	const uint32_t shader[] = {0xf40c0100u, 0xfa000000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& inst = code.GetInstructions().At(0);
	EXPECT_EQ(inst.type, ShaderInstructionType::SLoadDwordx8);
	EXPECT_EQ(inst.format, ShaderInstructionFormat::Sdst8SbaseSoffset);
	EXPECT_EQ(inst.dst.size, 8);
	EXPECT_EQ(inst.src[0].size, 2);
}

// Gen5 SMEM: SLoadDwordx2 s[4:5], s[0:1], 0; SLoadDword s6, s[0:1], 8; s_endpgm
// s[0:1] is the extended user-data pointer; offsets 0 and 8 map into push-constant vsharp.
TEST(EmulatorGraphicsPackets, MaterializesExtendedSLoadDwordAndX2)
{
	const uint32_t shader[] = {0xf4040100u, 0xfa000000u, 0xf4000180u, 0xfa000008u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::SLoadDwordx2);
	EXPECT_EQ(code.GetInstructions().At(1).type, ShaderInstructionType::SLoadDword);

	ShaderComputeInputInfo input {};
	input.threads_num[0]                         = 1;
	input.threads_num[1]                         = 1;
	input.threads_num[2]                         = 1;
	input.bind.extended.used                     = true;
	input.bind.extended.start_register           = 0;
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 16;
	input.bind.storage_buffers.extended[0]       = true;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::Constant;
	input.bind.push_constant_size                = 16;

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);

	// Destinations must be real stores, not comment-only no-ops.
	EXPECT_NE(source.FindIndex("OpStore %s4"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s5"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s6"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpAccessChain %_ptr_PushConstant_uint %vsharp"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpLoad %uint"), Core::STRING8_INVALID_INDEX);
}

// Gen5 SMEM from fetch_attrib_reg (+gs_prolog shift): loads attribute-table dwords into SGPRs.
TEST(EmulatorGraphicsPackets, MaterializesFetchAttribSLoadDwordAndX2)
{
	// SLoadDwordx2 s[48:49], s[14:15], 0; SLoadDword s50, s[14:15], 8; s_endpgm
	const uint32_t shader[] = {0xf4040c07u, 0xfa000000u, 0xf4000c87u, 0xfa000008u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::SLoadDwordx2);
	EXPECT_EQ(code.GetInstructions().At(0).src[0].register_id, 14);
	EXPECT_EQ(code.GetInstructions().At(1).type, ShaderInstructionType::SLoadDword);
	EXPECT_EQ(code.GetInstructions().At(1).dst.register_id, 50);

	ShaderVertexInputInfo input {};
	input.fetch_embedded        = true;
	input.gs_prolog             = true;
	input.fetch_attrib_reg      = 6; // physical s14 with +8 shift
	input.fetch_buffer_reg      = 4; // physical s12
	// Values >= 256 so SPIR-V emits hex constants (see Spirv::AddConstant).
	input.fetch_attrib_data[0]  = 0xa1b2c301u;
	input.fetch_attrib_data[1]  = 0xa1b2c302u;
	input.fetch_attrib_data[2]  = 0xa1b2c303u;
	input.fetch_attrib_data_num = 3;

	const auto source = SpirvGenerateSource(code, &input, nullptr, nullptr);

	EXPECT_NE(source.FindIndex("OpStore %s48"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s49"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s50"), Core::STRING8_INVALID_INDEX);
	// Sanitized table values must appear as SPIR-V uint constants used by the stores.
	EXPECT_NE(source.FindIndex("0xa1b2c301"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("0xa1b2c302"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("0xa1b2c303"), Core::STRING8_INVALID_INDEX);
}

// Gen5 SMEM: s_buffer_load_dwordx8 s[32:39], s[8:11], 0; s_nop; s_endpgm
// Encoding matches the SMRD opcode table (0x0B); recompiler already supports Sdst8SvSoffset.
TEST(EmulatorGraphicsPackets, ParsesGen5SBufferLoadDwordx8)
{
	// word0: enc=0x3d, op=0x0B, sdst=32, sbase=4 → s[8:11]; word1: null soffset, imm 0
	// s_nop (sopp) then s_endpgm so SEndpgm recompiler's index>=2 path is satisfied.
	const uint32_t shader[] = {0xf42c0804u, 0xfa000000u, 0xbf800000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	ShaderParse(shader, &code);

	ASSERT_GE(code.GetInstructions().Size(), 2u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::SBufferLoadDwordx8);
	EXPECT_EQ(code.GetInstructions().At(0).format, ShaderInstructionFormat::Sdst8SvSoffset);
	EXPECT_EQ(code.GetInstructions().At(0).dst.register_id, 32);
	EXPECT_EQ(code.GetInstructions().At(0).dst.size, 8);
	EXPECT_EQ(code.GetInstructions().At(0).src[0].register_id, 8);
	EXPECT_EQ(code.GetInstructions().At(0).src[0].size, 4);

	ShaderVertexInputInfo input {};
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 8;
	input.bind.storage_buffers.extended[0]       = false;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::Constant;
	input.bind.storage_buffers.binding_index     = 0;
	input.bind.push_constant_size                = 16;
	input.bind.descriptor_set_slot               = 0;

	const auto source = SpirvGenerateSource(code, &input, nullptr, nullptr);

	// Helper and call site must exist; stores happen inside the helper via pointer params.
	EXPECT_NE(source.FindIndex("%sbuffer_load_dword_8"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpFunctionCall %void %sbuffer_load_dword_8"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%s32"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%s39"), Core::STRING8_INVALID_INDEX);
}

TEST(EmulatorGraphicsPackets, RejectsInvalidPacketInputs)
{
	const ShaderRegister outside_sh_space[] = {{Pm4::SH_NUM, 1u}};
	uint32_t             command[5]         = {};

	EXPECT_EQ(Gen5::GraphicsGetShRegistersPacketSize(nullptr, 1), 0u);
	EXPECT_EQ(Gen5::GraphicsGetShRegistersPacketSize(outside_sh_space, 1), 0u);
	EXPECT_EQ(Gen5::GraphicsEncodeShRegisters(command, 1, outside_sh_space, 1), 0u);
	EXPECT_EQ(Gen5::GraphicsEncodeDispatch(command, 4, 1, 1, 1, 0), 0u);
}

TEST(EmulatorGraphicsPackets, SizesResourceRegistrationMemory)
{
	size_t small = 0;
	size_t large = 0;

	EXPECT_LT(Gen5Driver::GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(nullptr, 32, 4), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(&small, 32, 4), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(&large, 64, 8), 0);
	EXPECT_GT(small, 0u);
	EXPECT_GT(large, small);
	EXPECT_EQ(small % 64u, 0u);

	std::unique_ptr<uint8_t[]> memory(new uint8_t[small]);
	EXPECT_LT(Gen5Driver::GraphicsDriverInitResourceRegistration(nullptr, small, 4), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverInitResourceRegistration(memory.get(), 0, 4), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverInitResourceRegistration(memory.get(), small, 4), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverRegisterDefaultOwner(0), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterDefaultOwner(1), 0);
	uint32_t default_owner = 0;
	EXPECT_LT(Gen5Driver::GraphicsDriverGetDefaultOwner(nullptr), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverGetDefaultOwner(&default_owner), 0);
	EXPECT_GT(default_owner, 0u);
	uint32_t max_name = 0;
	EXPECT_LT(Gen5Driver::GraphicsDriverGetResourceRegistrationMaxNameLength(nullptr), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverGetResourceRegistrationMaxNameLength(&max_name), 0);
	EXPECT_GT(max_name, 0u);
	uint32_t owner = 0;
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterOwner(nullptr, "runtime"), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterOwner(&owner, nullptr), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverRegisterOwner(&owner, "runtime"), 0);
	EXPECT_GT(owner, 0u);
	uint8_t  resource_memory[64] = {};
	uint32_t resource            = 0;
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterResource(nullptr, owner, resource_memory, sizeof(resource_memory), "buffer", 1, 0), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterResource(&resource, owner, nullptr, sizeof(resource_memory), "buffer", 1, 0), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverRegisterResource(&resource, owner, resource_memory, sizeof(resource_memory), "buffer", 1, 0), 0);
	EXPECT_GT(resource, 0u);
	EXPECT_LT(Gen5Driver::GraphicsDriverUnregisterResource(0), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverUnregisterResource(resource), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverUnregisterResource(resource), 0);
}

TEST(EmulatorGraphicsPackets, MapsPixelInputsFromVertexOutputSuperset)
{
	ShaderSemantic outputs[2] {};
	outputs[0].semantic         = 15;
	outputs[0].hardware_mapping = 5;
	outputs[1].semantic         = 16;
	outputs[1].hardware_mapping = 7;

	ShaderSemantic inputs[1] {};
	inputs[0].semantic       = 15;
	inputs[0].is_flat_shaded = 1;

	ShaderRegister regs[32] {};
	ASSERT_TRUE(Gen5::GraphicsBuildInterpolantMapping(regs, outputs, 2, inputs, 1));
	EXPECT_EQ(regs[0].offset, Pm4::SPI_PS_INPUT_CNTL_0);
	EXPECT_EQ(regs[0].value, 5u | 0x400u);
	EXPECT_EQ(regs[1].offset, Pm4::SPI_PS_INPUT_CNTL_0 + 1);
	EXPECT_EQ(regs[1].value, 0u);

	inputs[0].semantic = 17;
	inputs[0].default_value = 0;
	ASSERT_TRUE(Gen5::GraphicsBuildInterpolantMapping(regs, outputs, 2, inputs, 1));
	EXPECT_EQ(regs[0].value, 0x20u);

	inputs[0].default_value = 3;
	ASSERT_TRUE(Gen5::GraphicsBuildInterpolantMapping(regs, outputs, 2, inputs, 1));
	EXPECT_EQ(regs[0].value, 0x320u);

	ASSERT_TRUE(Gen5::GraphicsBuildInterpolantMapping(regs, outputs, 2, nullptr, 0));
	EXPECT_EQ(regs[0].value, 5u);
	EXPECT_EQ(regs[1].value, 7u);
}


// Captured: ShaderUserData eud_size_dw=12, srt_size_dw=0, user_sgpr_num=30
// without a type-5 pointer. Descriptors fit in the SGPR window.
TEST(EmulatorGraphicsPackets, EudWithoutSrtUsesUserSgprWindow)
{
	EXPECT_GE(HW::UserSgprInfo::SGPRS_MAX, 30);
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(0, 30));
	// Embedded policy: when srt==0, eud!=0, and no type-5 pointer, eud_size
	// must be <= user_sgpr_num.
	constexpr uint16_t eud = 12;
	constexpr int      n   = 30;
	EXPECT_LT(static_cast<int>(eud), n + 1);
	EXPECT_EQ(0, 0); // srt_size_dw == 0
}

// Type-5 guest EUD tables may exceed metadata eud_size_dw. Astro: api=40,
// dwords=4, eud_size=24 → need=28 still allowed under the hard 256 cap.
TEST(EmulatorGraphicsPackets, Gen5EudSpanAllowsModestMetadataOverrun)
{
	EXPECT_TRUE(ShaderGen5EudSpanAllowed(16, 4, 24));  // fully inside metadata
	EXPECT_TRUE(ShaderGen5EudSpanAllowed(40, 4, 24));  // need 28 > 24, still ok
	EXPECT_TRUE(ShaderGen5EudSpanAllowed(16 + 24 - 4, 4, 24)); // last in-bound
	EXPECT_FALSE(ShaderGen5EudSpanAllowed(16, 257, 24)); // past hard cap
}

// Captured EXP target 0x03: MRT3 compressed (half2), done may be 0.
TEST(EmulatorGraphicsPackets, ExpTarget0x03IsMrt3Compr)
{
	EXPECT_NE(static_cast<uint64_t>(ShaderInstructionFormat::Mrt3Vsrc0Vsrc1ComprVm),
	          static_cast<uint64_t>(ShaderInstructionFormat::Unknown));
	EXPECT_NE(static_cast<uint64_t>(ShaderInstructionFormat::Mrt3Vsrc0Vsrc1ComprVm),
	          static_cast<uint64_t>(ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone));
	// GCN/GFX10 color MRT targets are 0x00 + N.
	EXPECT_EQ(0x00 + 3, 0x03);
}

// Captured s_buffer_load_dwordx4 with SGPR soffset + imm 0x10.
TEST(EmulatorGraphicsPackets, SmemImmOffsetFieldDefaultsZero)
{
	ShaderInstruction inst {};
	EXPECT_EQ(inst.smem_imm_offset, 0);
	inst.smem_imm_offset = 0x10;
	EXPECT_EQ(inst.smem_imm_offset, 0x10);
}

// Captured VOP1 opcode 0x8 is v_cvt_i32_f32 (and VOP3 0x188).
TEST(EmulatorGraphicsPackets, Vop1Opcode8IsVCvtI32F32)
{
	EXPECT_NE(static_cast<int>(ShaderInstructionType::VCvtI32F32), static_cast<int>(ShaderInstructionType::Unknown));
	EXPECT_NE(static_cast<int>(ShaderInstructionType::VCvtI32F32), static_cast<int>(ShaderInstructionType::VCvtU32F32));
}

// Captured CB_SHADER_MASK 0xffff (RT0+RT1 all channels). Each RT nibble is
// either 0 (disabled) or 0xf (RGBA); partial channel enables are unsupported.
TEST(EmulatorGraphicsPackets, CbShaderMaskFullChannelNibbles)
{
	auto nibble_ok = [](uint32_t mask) {
		for (uint32_t rt = 0; rt < 8u; rt++)
		{
			const uint32_t n = (mask >> (rt * 4u)) & 0xfu;
			if (n != 0u && n != 0xfu)
			{
				return false;
			}
		}
		return true;
	};
	EXPECT_TRUE(nibble_ok(0x0000000fu));
	EXPECT_TRUE(nibble_ok(0x0000ffffu));
	EXPECT_TRUE(nibble_ok(0x00000000u));
	EXPECT_FALSE(nibble_ok(0x00000001u)); // partial RT0
	EXPECT_FALSE(nibble_ok(0x000000f1u));
}

// Captured first fail after EUD bind: EXP target 0x25 is Param5.
TEST(EmulatorGraphicsPackets, ExpTarget0x25IsParam5)
{
	EXPECT_NE(static_cast<uint64_t>(ShaderInstructionFormat::Param5Vsrc0Vsrc1Vsrc2Vsrc3),
	          static_cast<uint64_t>(ShaderInstructionFormat::Unknown));
	EXPECT_NE(static_cast<uint64_t>(ShaderInstructionFormat::Param5Vsrc0Vsrc1Vsrc2Vsrc3),
	          static_cast<uint64_t>(ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3));
	// GCN/GFX10: parameter exports occupy targets 0x20 + N.
	EXPECT_EQ(0x20 + 5, 0x25);
}

// VOP1 SDWA: src0 encoding 249 + control dword. Captured after Param6 as
// "unknown operand: 249" until VOP1 shares VOP2's SDWA decode for src0.
TEST(EmulatorGraphicsPackets, ParsesVop1SdwaSrc0)
{
	// v_mov_b32 v0, -v2 with SDWA: opcode 1 (mov), src0=249.
	// VOP1: bits[24:17]=vdst, bits[16:9]=op, bits[8:0]=src0; encoding via VOP2 trampoline 0x3f.
	const uint32_t word0 = (0x3fu << 25u) | (0u << 17u) | (0x01u << 9u) | 249u;
	const uint32_t word1 = 2u | (6u << 8u) | (6u << 16u) | (1u << 20u); // src vgpr2, DWORD, src0_neg
	const uint32_t shader[] = {word0, word1, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& inst = code.GetInstructions().At(0);
	EXPECT_EQ(inst.type, ShaderInstructionType::VMovB32);
	EXPECT_EQ(inst.src[0].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(inst.src[0].register_id, 2);
	EXPECT_TRUE(inst.src[0].negate);
	EXPECT_EQ(inst.src[0].swizzle, 6u);
}

// SOP1 s_not_b64 (op 0x08): bitwise not of a 64-bit SGPR pair.
TEST(EmulatorGraphicsPackets, ParsesGen5SNotB64)
{
	// SOP1 encoding: [31:23]=0b101111101, [22:16]=sdst, [15:8]=op, [7:0]=ssrc0
	// s_not_b64 s[0:1], s[2:3] → sdst=0, op=0x08, ssrc0=2
	const uint32_t word0 = (0x17Du << 23u) | (0u << 16u) | (0x08u << 8u) | 2u;
	const uint32_t shader[] = {word0, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& inst = code.GetInstructions().At(0);
	EXPECT_EQ(inst.type, ShaderInstructionType::SNotB64);
	EXPECT_EQ(inst.format, ShaderInstructionFormat::Sdst2Ssrc02);
	EXPECT_EQ(inst.dst.size, 2);
	EXPECT_EQ(inst.src[0].size, 2);
	EXPECT_EQ(inst.dst.register_id, 0);
	EXPECT_EQ(inst.src[0].register_id, 2);
}

// VOP1 SDWA with src0_sel=BYTE_0 (0). Observed Gen5 path after PlayGo/Resident Load.
TEST(EmulatorGraphicsPackets, ParsesVop1SdwaSrc0Byte0)
{
	// v_mov_b32 v0, v2.b0 with SDWA: opcode 1, src0_sel=0 (BYTE_0), dst_sel=DWORD.
	const uint32_t word0 = (0x3fu << 25u) | (0u << 17u) | (0x01u << 9u) | 249u;
	const uint32_t word1 = 2u | (6u << 8u) | (0u << 16u); // vgpr2, dst DWORD, src0 BYTE_0
	const uint32_t shader[] = {word0, word1, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& inst = code.GetInstructions().At(0);
	EXPECT_EQ(inst.type, ShaderInstructionType::VMovB32);
	EXPECT_EQ(inst.src[0].register_id, 2);
	EXPECT_EQ(inst.src[0].swizzle, 0u);
}

// Compressed MRT export must read the uint packed-half shadow written by
// VCvtPkrtz, not a float load+bitcast of the same VGPR (precision/path mismatch).
TEST(EmulatorGraphicsPackets, CompressedMrtExportReadsPackedHalfFromUintShadow)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	auto vgpr = [](int reg)
	{
		ShaderOperand op {};
		op.type        = ShaderOperandType::Vgpr;
		op.register_id = reg;
		op.size        = 1;
		return op;
	};

	ShaderCode code;
	code.SetType(ShaderType::Pixel);

	ShaderInstruction pack_rg;
	pack_rg.pc      = 0;
	pack_rg.type    = ShaderInstructionType::VCvtPkrtzF16F32;
	pack_rg.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	pack_rg.dst     = vgpr(4);
	pack_rg.src[0]  = vgpr(2);
	pack_rg.src[1]  = vgpr(3);
	pack_rg.src_num = 2;

	ShaderInstruction pack_ba;
	pack_ba.pc      = 4;
	pack_ba.type    = ShaderInstructionType::VCvtPkrtzF16F32;
	pack_ba.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	pack_ba.dst     = vgpr(5);
	pack_ba.src[0]  = vgpr(6);
	pack_ba.src[1]  = vgpr(7);
	pack_ba.src_num = 2;

	ShaderInstruction export_mrt;
	export_mrt.pc      = 8;
	export_mrt.type    = ShaderInstructionType::Exp;
	export_mrt.format  = ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone;
	export_mrt.src[0]  = vgpr(4);
	export_mrt.src[1]  = vgpr(5);
	export_mrt.src_num = 2;

	code.GetInstructions().Add(pack_rg);
	code.GetInstructions().Add(pack_ba);
	code.GetInstructions().Add(export_mrt);

	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;

	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);

	// VCvtPkrtzF16F32 implements the ISA's binary16 round-toward-zero
	// conversion in integer bits. GLSL PackHalf2x16 rounds to nearest-even and
	// therefore cannot represent this instruction's contract.
	EXPECT_EQ(source.FindIndex("PackHalf2x16"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("tpk0_bits_0"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("tpk1_bits_0"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("tpk0_subnormal_0"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("tpk0_max_finite_0"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("UnpackHalf2x16"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%t1_2 = OpLoad %float %v4"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%t6_2 = OpLoad %float %v5"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpLoad %uint %v4_packed_half"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpLoad %uint %v5_packed_half"), Core::STRING8_INVALID_INDEX);
}

// Compressed MRT export must branch on EXEC and OpKill when inactive so dead
// lanes do not write color.
TEST(EmulatorGraphicsPackets, CompressedMrtExportIsGuardedByExecMask)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	auto vgpr = [](int reg)
	{
		ShaderOperand op {};
		op.type        = ShaderOperandType::Vgpr;
		op.register_id = reg;
		op.size        = 1;
		return op;
	};

	ShaderCode code;
	code.SetType(ShaderType::Pixel);

	ShaderInstruction export_mrt;
	export_mrt.pc      = 0;
	export_mrt.type    = ShaderInstructionType::Exp;
	export_mrt.format  = ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone;
	export_mrt.src[0]  = vgpr(0);
	export_mrt.src[1]  = vgpr(1);
	export_mrt.src_num = 2;

	code.GetInstructions().Add(export_mrt);

	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;

	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);

	EXPECT_NE(source.FindIndex("OpBranchConditional %exp_exec_b_0 %exp_store_0 %exp_kill_0"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%exp_kill_0 = OpLabel"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpKill"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%exp_store_0 = OpLabel"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %outColor"), Core::STRING8_INVALID_INDEX);
}

// Captured dual-strict first fail: EXP target 0x26 done=0 compr=0 vm=0 en=0xf
// at VS PC 0x264. Same ParamN path as 0x20+N; real ShaderParse entry (not a re-impl).
TEST(EmulatorGraphicsPackets, ParsesExpTarget0x26AsParam6)
{
	// EXP encoding: bits[31:26]=0x3e, target bits[9:4], en bits[3:0].
	const uint32_t word0 = (0x3eu << 26u) | (0x26u << 4u) | 0xfu;
	const uint32_t word1 = 0x03020100u; // v0..v3
	const uint32_t shader[] = {word0, word1, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& inst = code.GetInstructions().At(0);
	EXPECT_EQ(inst.type, ShaderInstructionType::Exp);
	EXPECT_EQ(inst.format, ShaderInstructionFormat::Param6Vsrc0Vsrc1Vsrc2Vsrc3);
	EXPECT_EQ(inst.src_num, 4);
	EXPECT_EQ(0x20 + 6, 0x26);
}

// Captured post-detile PS: user_sgpr_num=30, eud=12, type5@0x1c, samplers at
// 0x18 (direct), 0x20 and 0x24 (EUD). EUD virtual base is user_sgpr_num
// rounded up to a multiple of 4 (30 → 32); api index uses the extended
// start-16 convention so eud[0] is addressed as 16.
TEST(EmulatorGraphicsPackets, Gen5EudOverflowSharpOffsetMapping)
{
	constexpr int user_sgpr_num = 30;
	constexpr int eud_base      = (user_sgpr_num + 3) & ~3;
	EXPECT_EQ(eud_base, 32);

	// S0@0x18 (4 dwords) fits in the SGPR window.
	EXPECT_LE(0x18 + 4, user_sgpr_num);
	// S1@0x20 and S2@0x24 overflow into EUD.
	EXPECT_GT(0x20 + 4, user_sgpr_num);
	EXPECT_GT(0x24 + 4, user_sgpr_num);

	EXPECT_EQ(16 + (0x20 - eud_base), 16); // eud[0]
	EXPECT_EQ(16 + (0x24 - eud_base), 20); // eud[4]
	// eud_size=12 covers both 4-dword samplers (indices 0..7).
	EXPECT_LE((0x24 - eud_base) + 4, 12);
}


TEST(EmulatorGraphicsPackets, Gen5SingleComponent32BitBufferFormat)
{
	EXPECT_TRUE(ShaderIsGen5SingleComponent32BitBufferFormat(20));
	EXPECT_FALSE(ShaderIsGen5SingleComponent32BitBufferFormat(75));
	EXPECT_TRUE(ShaderIsGen5FourComponent32BitBufferFormat(75));
	EXPECT_EQ(DstSel(4, 0, 0, 1), 0x204u);
}

TEST(EmulatorGraphicsPackets, Gen5RenderTextureAbgrSampleView)
{
	EXPECT_EQ(DstSel(7, 6, 5, 4), 0x977u);
	EXPECT_LT(VulkanImage::VIEW_ABGR, VulkanImage::VIEW_MAX);
	EXPECT_NE(VulkanImage::VIEW_ABGR, VulkanImage::VIEW_DEFAULT);
	EXPECT_NE(VulkanImage::VIEW_ABGR, VulkanImage::VIEW_BGRA);
}

// sceAgcDcbStallCommandBufferParser: fixed EVENT_WRITE with CS partial flush (0x07).
TEST(EmulatorGraphicsPackets, EncodesDcbStallCommandBufferParser)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[8] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 8;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 8;

	uint32_t* cmd = Gen5::GraphicsDcbStallCommandBufferParser(reinterpret_cast<Gen5::CommandBuffer*>(&cb));
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(2, Pm4::IT_EVENT_WRITE, 0));
	EXPECT_EQ(cmd[1], 0x07u);
}

// 3.20 catalog: WaitRegMemPatchAddress / DmaDataPatch use fixed field offsets.
TEST(EmulatorGraphicsPackets, WaitRegMemAndDmaDataPatchFieldOffsets)
{
	using namespace Kyty::Libs::Graphics;
	// Custom R_WAIT_MEM_64: address at +4 bytes.
	const uint32_t wait64 = KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64);
	EXPECT_EQ(GraphicsWaitRegMemAddressByteOffset(wait64), 4u);
	const uint32_t wait32 = KYTY_PM4(6, Pm4::IT_NOP, Pm4::R_WAIT_MEM_32);
	EXPECT_EQ(GraphicsWaitRegMemAddressByteOffset(wait32), 4u);
	// Hardware IT_WAIT_REG_MEM: address at +8 bytes.
	const uint32_t it_wait = KYTY_PM4(7, Pm4::IT_WAIT_REG_MEM, 0);
	EXPECT_EQ(GraphicsWaitRegMemAddressByteOffset(it_wait), 8u);
	EXPECT_EQ(GraphicsWaitRegMemAddressByteOffset(0u), 0u);

	const uint32_t dma = KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_DMA_DATA);
	EXPECT_TRUE(GraphicsIsCustomDmaDataPacket(dma));
	EXPECT_FALSE(GraphicsIsCustomDmaDataPacket(wait64));
}

TEST(EmulatorGraphicsPackets, PatchesDmaDataDestinationAndWaitRegMemAddress)
{
	using namespace Kyty::Libs::Graphics;
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t dma[8] = {};
	dma[0]          = KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_DMA_DATA);
	dma[3]          = 64u;
	EXPECT_EQ(Gen5::GraphicsAgcDmaDataPatchSetDstAddressOrOffset(dma, 0x0000000123456789ull), 0);
	EXPECT_EQ(dma[4], 0x23456789u);
	EXPECT_EQ(dma[5], 0x00000001u);
	EXPECT_EQ(Gen5::GraphicsAgcDmaDataPatchSetDstAddressOrOffset(nullptr, 0), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	uint32_t not_dma[2] = {KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0};
	EXPECT_EQ(Gen5::GraphicsAgcDmaDataPatchSetDstAddressOrOffset(not_dma, 1), Libs::LibKernel::KERNEL_ERROR_EINVAL);

	uint32_t wait[9] = {};
	wait[0]          = KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64);
	EXPECT_EQ(Gen5::GraphicsAgcWaitRegMemPatchAddress(wait, 0x00000001abcdef00ull), 0);
	EXPECT_EQ(wait[1], 0xabcdef00u);
	EXPECT_EQ(wait[2], 0x00000001u);
	EXPECT_EQ(Gen5::GraphicsAgcWaitRegMemPatchAddress(not_dma, 1), Libs::LibKernel::KERNEL_ERROR_EINVAL);
}

TEST(EmulatorGraphicsPackets, EncodesCbNopAsFullType3Packet)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {0xffffffffu};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	uint32_t* cmd = Gen5::GraphicsCbNop(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 4);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(4, Pm4::IT_NOP, Pm4::R_ZERO));
	EXPECT_EQ(cmd[1], 0u);
	EXPECT_EQ(cmd[2], 0u);
	EXPECT_EQ(cmd[3], 0u);
	EXPECT_EQ(Gen5::GraphicsCbNop(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 1), nullptr);
	EXPECT_EQ(Gen5::GraphicsGetDataPacketSizeDw(cmd), 4u);
}

TEST(EmulatorGraphicsPackets, DecodesCbNopBodyWithoutSideEffects)
{
	EXPECT_EQ(Pm4::Pm4Type3NopBodyDwords(KYTY_PM4(10, Pm4::IT_NOP, Pm4::R_ZERO)), 9u);
	EXPECT_EQ(Pm4::Pm4Type3NopBodyDwords(KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_RELEASE_MEM)), 0u);
	EXPECT_EQ(Pm4::Pm4Type3NopBodyDwords(0u), 0u);
}

// sceAgcDcbDmaData / sceAgcAcbDmaData: encode IT_NOP + R_DMA_DATA.
TEST(EmulatorGraphicsPackets, EncodesDcbDmaDataCustomPacket)
{
	struct AlignasCommandBuffer
	{
		uint32_t* bottom      = nullptr;
		uint32_t* top         = nullptr;
		uint32_t* cursor_up   = nullptr;
		uint32_t* cursor_down = nullptr;
		void*     callback    = nullptr;
		void*     user_data   = nullptr;
		uint32_t  reserved_dw = 0;
		uint32_t  pad         = 0;
	};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t             storage[16] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 16;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 16;

	const uint64_t dst = 0x0000000120000000ull;
	const uint64_t src = 0x0000000130000000ull;
	uint32_t*      cmd =
	    Gen5::GraphicsDcbDmaData(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 4, 0, 0, dst, 0, 0, src, 64, 0, 0, 0);
	ASSERT_NE(cmd, nullptr);
	EXPECT_EQ(cmd[0], KYTY_PM4(8, Pm4::IT_NOP, Pm4::R_DMA_DATA));
	EXPECT_EQ(cmd[1], 0x00000004u);
	EXPECT_EQ(cmd[2], 0u);
	EXPECT_EQ(cmd[3], 64u);
	EXPECT_EQ(cmd[4], 0x20000000u);
	EXPECT_EQ(cmd[5], 0x00000001u);
	EXPECT_EQ(cmd[6], 0x30000000u);
	EXPECT_EQ(cmd[7], 0x00000001u);

	// Invalid byte_count must not allocate.
	EXPECT_EQ(Gen5::GraphicsDcbDmaData(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 4, 0, 0, dst, 0, 0, src, 3, 0, 0, 0),
	          nullptr);
}

UT_END();
