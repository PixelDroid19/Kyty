#include "Kyty/UnitTest.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

UT_BEGIN(EmulatorComputeWaveAlu);

using namespace Libs::Graphics;

// A different lowering strategy cannot legalize malformed registers or controls
// that neither lowering models. Valid generic tuples have separate controls.
TEST(EmulatorComputeWaveAlu, BankedAluClaimsOnlyPlainSingleDestinationIntegerTuples)
{
	ShaderInstruction instruction;
	instruction.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	instruction.src_num = 2;
	instruction.dst.type = instruction.src[0].type = instruction.src[1].type = ShaderOperandType::Vgpr;
	instruction.dst.size = instruction.src[0].size = instruction.src[1].size = 1;
	instruction.dst.register_id = instruction.src[0].register_id = 255;
	for (const auto type: {ShaderInstructionType::VAndB32, ShaderInstructionType::VOrB32, ShaderInstructionType::VXorB32,
	                       ShaderInstructionType::VAddI32, ShaderInstructionType::VSubI32})
	{
		instruction.type = type;
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::BankedAlu);
		auto modified = instruction;
		modified.dst2.type = ShaderOperandType::VccLo;
		modified.dst2.size = 2;
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
		modified = instruction;
		modified.vop_sdwa = true;
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
		modified = instruction;
		modified.vop3_op_sel = 1;
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
		modified = instruction;
		modified.src[1].dpp = true;
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
		modified = instruction;
		modified.dst.register_id = 256;
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	}
}

TEST(EmulatorComputeWaveAlu, AdmitsCndmaskMaskSelectTuples)
{
	// v_cndmask_b32 v3, 0x4797e880, v16, s[16:17] — the tuple observed in the
	// workload shader: 32-bit select per lane against a packed mask pair.
	ShaderInstruction instruction;
	instruction.type              = ShaderInstructionType::VCndmaskB32;
	instruction.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
	instruction.src_num           = 3;
	instruction.dst.type          = ShaderOperandType::Vgpr;
	instruction.dst.size          = 1;
	instruction.dst.register_id   = 3;
	instruction.src[0].type       = ShaderOperandType::LiteralConstant;
	instruction.src[0].constant.u = 0x4797e880u;
	instruction.src[1].type       = ShaderOperandType::Vgpr;
	instruction.src[1].size       = 1;
	instruction.src[1].register_id = 16;
	instruction.src[2].type       = ShaderOperandType::Sgpr;
	instruction.src[2].size       = 2;
	instruction.src[2].register_id = 16;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::BankedAlu);

	// VCC and EXEC mask pairs select identically.
	auto modified          = instruction;
	modified.src[2].type   = ShaderOperandType::VccLo;
	modified.src[2].register_id = 0;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::BankedAlu);
	modified.src[2].type = ShaderOperandType::ExecLo;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::BankedAlu);

	// One-word SGPR and inline-constant data sources stay inside the contract.
	modified             = instruction;
	modified.src[0].type = ShaderOperandType::Sgpr;
	modified.src[0].size = 1;
	modified.src[0].register_id = 4;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::BankedAlu);
	modified.src[0].type        = ShaderOperandType::IntegerInlineConstant;
	modified.src[0].size        = 0;
	modified.src[0].register_id = 0;
	modified.src[0].constant.i  = 0;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::BankedAlu);
}

TEST(EmulatorComputeWaveAlu, RejectsNonContractCndmaskTuples)
{
	ShaderInstruction instruction;
	instruction.type              = ShaderInstructionType::VCndmaskB32;
	instruction.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
	instruction.src_num           = 3;
	instruction.dst.type          = ShaderOperandType::Vgpr;
	instruction.dst.size          = 1;
	instruction.dst.register_id   = 3;
	instruction.src[0].type       = ShaderOperandType::Vgpr;
	instruction.src[0].size       = 1;
	instruction.src[0].register_id = 1;
	instruction.src[1].type       = ShaderOperandType::Vgpr;
	instruction.src[1].size       = 1;
	instruction.src[1].register_id = 2;
	instruction.src[2].type       = ShaderOperandType::VccLo;
	instruction.src[2].size       = 2;
	instruction.src[2].register_id = 0;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::BankedAlu);

	// The select operand must be a two-word mask pair.
	auto modified          = instruction;
	modified.src[2].type   = ShaderOperandType::Vgpr;
	modified.src[2].size   = 1;
	modified.src[2].register_id = 4;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	modified          = instruction;
	modified.src[2].size = 1;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	modified             = instruction;
	modified.src[2].type = ShaderOperandType::LiteralConstant;
	modified.src[2].size = 0;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	modified                  = instruction;
	modified.src[2].type      = ShaderOperandType::Sgpr;
	modified.src[2].register_id = 127;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);

	// Mask words are not 32-bit data sources.
	modified             = instruction;
	modified.src[0].type = ShaderOperandType::ExecLo;
	modified.src[0].size = 2;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);

	// Encoding modifiers, a second destination, and a fourth source stay rejected.
	modified = instruction;
	modified.vop_sdwa = true;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	modified             = instruction;
	modified.vop3_op_sel = 1;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	modified             = instruction;
	modified.vop3_omod   = 1;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	// Float input modifiers are lowered in ISA order, neg(abs(x)), by the banked path.
	modified                 = instruction;
	modified.src[0].negate   = true;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::BankedAlu);
	modified                 = instruction;
	modified.src[1].absolute = true;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::BankedAlu);
	modified          = instruction;
	modified.dst2.type = ShaderOperandType::VccLo;
	modified.dst2.size = 2;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	modified             = instruction;
	modified.src_num     = 4;
	modified.src[3].type = ShaderOperandType::Vgpr;
	modified.src[3].size = 1;
	modified.src[3].register_id = 5;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	modified            = instruction;
	modified.dst.size   = 2;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
	modified            = instruction;
	modified.dst.type   = ShaderOperandType::Sgpr;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::Unsupported);
}

UT_END();
