#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

UT_BEGIN(EmulatorComputeWaveScalar);
using namespace Libs::Graphics;

static ShaderCode ParseScalarShift()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	const uint32_t words[] = {0x8f6b860eu, 0xbf810000u};
	ShaderCode     code;
	code.SetType(ShaderType::Compute);
	ShaderParse(words, &code);
	return code;
}

static ShaderInstruction BinaryMask()
{
	ShaderInstruction instruction;
	instruction.type            = ShaderInstructionType::SAndB64;
	instruction.format          = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
	instruction.src_num         = 2;
	instruction.dst.type        = ShaderOperandType::Sgpr;
	instruction.dst.register_id = 4;
	instruction.src[0].type     = ShaderOperandType::ExecLo;
	instruction.src[1].type     = ShaderOperandType::VccLo;
	instruction.dst.size = instruction.src[0].size = instruction.src[1].size = 2;
	return instruction;
}

TEST(EmulatorComputeWaveScalar, AdmitsOnlyExactMaskPairs)
{
	for (const auto type: {ShaderInstructionType::SAndB64, ShaderInstructionType::SOrB64, ShaderInstructionType::SXorB64})
	{
		auto instruction = BinaryMask();
		instruction.type = type;
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::ScalarMask);
		for (int variant = 0; variant < 8; ++variant)
		{
			auto invalid = instruction;
			switch (variant)
			{
				case 0: invalid.dst.register_id = 103; break;
				case 1: invalid.src[0].size = 1; break;
				case 2: invalid.src[1].type = ShaderOperandType::LiteralConstant; break;
				case 3: invalid.src[2] = invalid.src[0]; break;
				case 4: invalid.dst2 = invalid.dst; break;
				case 5: invalid.vop3_omod = 1; break;
				case 6: invalid.ds_encoding_control = 1; break;
				case 7: invalid.src[0].negate = true; break;
			}
			EXPECT_EQ(ShaderClassifyComputeWaveInstruction(invalid), ShaderComputeWaveInstructionKind::Unsupported);
		}
	}
}

TEST(EmulatorComputeWaveScalar, SaveExecRejectsSpecialDestinationsAndOutOfRangeImmediates)
{
	auto instruction        = BinaryMask();
	instruction.type        = ShaderInstructionType::SAndSaveexecB64;
	instruction.format      = ShaderInstructionFormat::Sdst2Ssrc02;
	instruction.src_num     = 1;
	instruction.src[1]      = {};
	instruction.src[0].type = ShaderOperandType::IntegerInlineConstant;
	for (int value: {-17, -16, 0, 64, 65})
	{
		instruction.src[0].constant.i = value;
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), value >= -16 && value <= 64
		                                                                 ? ShaderComputeWaveInstructionKind::ScalarMask
		                                                                 : ShaderComputeWaveInstructionKind::Unsupported);
	}
	instruction.src[0].constant.i = 0;
	for (const auto type: {ShaderOperandType::ExecLo, ShaderOperandType::VccLo})
	{
		instruction.dst.type        = type;
		instruction.dst.register_id = 0;
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::Unsupported);
	}
}

TEST(EmulatorComputeWaveScalar, AdmitsOnlyExactVccHiScalarShiftTuple)
{
	const auto code = ParseScalarShift();
	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::SLshlB32);
	EXPECT_EQ(instruction.format, ShaderInstructionFormat::SVdstSVsrc0SVsrc1);
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::ScalarShift);

	auto other_operands                = instruction;
	other_operands.src[0].register_id  = 13;
	other_operands.src[1].constant.i   = 31;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(other_operands), ShaderComputeWaveInstructionKind::ScalarShift);
	other_operands.src[1].constant.i = 0;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(other_operands), ShaderComputeWaveInstructionKind::ScalarShift);

	for (int variant = 0; variant < 17; ++variant)
	{
		auto invalid = instruction;
		switch (variant)
		{
			case 0: invalid.dst.type = ShaderOperandType::VccLo; break;
			case 1: invalid.dst.register_id = 1; break;
			case 2: invalid.dst.size = 2; break;
			case 3: invalid.src[0].type = ShaderOperandType::Vgpr; break;
			case 4: invalid.src[0].size = 2; break;
			case 5: invalid.src[0].register_id = 104; break;
			case 6: invalid.src[1].constant.i = -1; break;
			case 7: invalid.src[1].constant.i = 32; break;
			case 8: invalid.src[1].size = 1; break;
			case 9: invalid.src[0].negate = true; break;
			case 10: invalid.vop3_omod = 1; break;
			case 11: invalid.ds_offset = 1; break;
			case 12: invalid.ds_encoding_control = 1; break;
			case 13: invalid.dst2 = invalid.dst; break;
			case 14: invalid.src[2] = invalid.src[0]; break;
			case 15: invalid.vop3_op_sel = 1; break;
			case 16: invalid.vop_sdwa = true; break;
		}
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(invalid), ShaderComputeWaveInstructionKind::Unsupported);
	}

	auto extra_source = instruction;
	extra_source.src[3] = extra_source.src[1];
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(extra_source), ShaderComputeWaveInstructionKind::Unsupported);
	auto extra_ds_registers = instruction;
	extra_ds_registers.ds_encoding_registers = 1;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(extra_ds_registers), ShaderComputeWaveInstructionKind::Unsupported);
}

UT_END();
