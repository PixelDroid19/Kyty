#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <cstddef>
#include <utility>
#include <vector>

UT_BEGIN(EmulatorFragmentNeutralRegion);

using namespace Libs::Graphics;

using Program = std::vector<ShaderInstruction>;

static ShaderOperand Operand(ShaderOperandType type, int reg, int size)
{
	ShaderOperand operand {};
	operand.type        = type;
	operand.register_id = reg;
	operand.size        = size;
	return operand;
}

static ShaderOperand Pair(int reg)
{
	return Operand(ShaderOperandType::Sgpr, reg, 2);
}

static ShaderOperand Mask()
{
	return Pair(36);
}

static ShaderOperand Exec()
{
	return Operand(ShaderOperandType::ExecLo, 0, 2);
}

static ShaderOperand Saved()
{
	return Operand(ShaderOperandType::VccLo, 0, 2);
}

static ShaderInstruction Scalar(ShaderInstructionType type, ShaderOperand destination, ShaderOperand source)
{
	ShaderInstruction instruction {};
	instruction.type    = type;
	instruction.format  = ShaderInstructionFormat::Sdst2Ssrc02;
	instruction.dst     = destination;
	instruction.src[0]  = source;
	instruction.src_num = 1;
	return instruction;
}

static ShaderInstruction Copy()
{
	return Scalar(ShaderInstructionType::SMovB64, Mask(), Exec());
}

static ShaderInstruction Saveexec(ShaderOperand saved = Saved())
{
	return Scalar(ShaderInstructionType::SOrn2SaveexecB64, saved, Mask());
}

static ShaderInstruction Restore(ShaderOperand saved = Saved())
{
	return Scalar(ShaderInstructionType::SMovB64, Exec(), saved);
}

static ShaderInstruction Derive(ShaderOperand condition, ShaderOperand left, ShaderOperand right)
{
	auto derive    = Scalar(ShaderInstructionType::SAndB64, condition, left);
	derive.format  = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
	derive.src[1]  = right;
	derive.src_num = 2;
	return derive;
}

static ShaderInstruction Initializer(ShaderOperand condition)
{
	ShaderInstruction initializer {};
	initializer.type    = ShaderInstructionType::VCndmaskB32;
	initializer.format  = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
	initializer.dst     = Operand(ShaderOperandType::Vgpr, 10, 1);
	initializer.src[0]  = Operand(ShaderOperandType::LiteralConstant, 0, 0);
	initializer.src[1]  = Operand(ShaderOperandType::Vgpr, 9, 1);
	initializer.src[2]  = condition;
	initializer.src_num = 3;
	return initializer;
}

static ShaderInstruction Reduction(bool fetch_inactive = false, uint16_t control = 0x111u)
{
	ShaderInstruction reduction {};
	reduction.type                      = ShaderInstructionType::VOrB32;
	reduction.format                    = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	reduction.dst                       = Operand(ShaderOperandType::Vgpr, 10, 1);
	reduction.src[0]                    = reduction.dst;
	reduction.src[0].dpp                = true;
	reduction.src[0].dpp_ctrl           = control;
	reduction.src[0].dpp_row_mask       = 15u;
	reduction.src[0].dpp_bank_mask      = 15u;
	reduction.src[0].dpp_fetch_inactive = fetch_inactive;
	reduction.src[1]                    = reduction.dst;
	reduction.src_num                   = 2;
	return reduction;
}

static ShaderInstruction Bitwise()
{
	ShaderInstruction bitwise {};
	bitwise.type    = ShaderInstructionType::VOrB32;
	bitwise.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	bitwise.dst     = Operand(ShaderOperandType::Vgpr, 11, 1);
	bitwise.src[0]  = Operand(ShaderOperandType::Vgpr, 10, 1);
	bitwise.src[1]  = bitwise.src[0];
	bitwise.src_num = 2;
	return bitwise;
}

static ShaderInstruction Readlane(int vgpr)
{
	ShaderInstruction readlane {};
	readlane.type              = ShaderInstructionType::VReadlaneB32;
	readlane.format            = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	readlane.dst               = Operand(ShaderOperandType::Sgpr, 0, 1);
	readlane.src[0]            = Operand(ShaderOperandType::Vgpr, vgpr, 1);
	readlane.src[1]            = Operand(ShaderOperandType::LiteralConstant, 0, 0);
	readlane.src[1].constant.u = 31;
	readlane.src_num           = 2;
	return readlane;
}

static ShaderInstruction Permute(uint8_t op_sel)
{
	ShaderInstruction permute {};
	permute.type        = ShaderInstructionType::VPermlanex16B32;
	permute.format      = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
	permute.dst         = Operand(ShaderOperandType::Vgpr, 9, 1);
	permute.src[0]      = Operand(ShaderOperandType::Vgpr, 10, 1);
	permute.src[1]      = Operand(ShaderOperandType::IntegerInlineConstant, 0, 0);
	permute.src[2]      = permute.src[1];
	permute.src_num     = 3;
	permute.vop3_op_sel = op_sel;
	return permute;
}

static ShaderCode Build(const Program& program, const std::vector<ShaderLabel>& labels = {})
{
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	for (auto instruction: program)
	{
		instruction.pc = code.GetInstructions().Size() * 4u;
		code.GetInstructions().Add(instruction);
	}
	for (const auto& label: labels)
	{
		code.GetLabels().Add(label);
	}
	return code;
}

static Program Region()
{
	return {Copy(), Saveexec(), Initializer(Mask()), Reduction(), Restore()};
}

static Program Insert(Program program, size_t index, const ShaderInstruction& instruction)
{
	program.insert(program.begin() + static_cast<std::ptrdiff_t>(index), instruction);
	return program;
}

static bool Admitted(const Program& program, uint32_t index = 1, const std::vector<ShaderLabel>& labels = {})
{
	return ShaderFragmentNeutralRegionSupported(Build(program, labels), index);
}

static bool Proven(const Program& program, const std::vector<ShaderLabel>& labels = {})
{
	return ShaderAnalyzeFragmentPartialWaveReads(Build(program, labels)).supported;
}

TEST(EmulatorFragmentNeutralRegion, AdmitsGuestInitializationAndRetainedDppDestinations)
{
	EXPECT_TRUE(Admitted(Region()));
	EXPECT_FALSE(Admitted(Region(), 0));
	EXPECT_FALSE(Admitted(Region(), UINT32_MAX));
	EXPECT_TRUE(Admitted(Region(), 1, {ShaderLabel(0, 100)}));
}

TEST(EmulatorFragmentNeutralRegion, AdmitsConditionDerivedFromTheCapturedMask)
{
	const Program program {Copy(), Derive(Pair(16), Mask(), Pair(30)), Saveexec(), Initializer(Pair(16)), Reduction(), Restore()};
	EXPECT_TRUE(Admitted(program, 2));
	EXPECT_TRUE(Admitted({Copy(), Derive(Pair(16), Pair(30), Mask()), Saveexec(), Initializer(Pair(16)), Reduction(), Restore()}, 2));
	EXPECT_FALSE(Admitted({Copy(), Derive(Pair(16), Pair(30), Pair(32)), Saveexec(), Initializer(Pair(16)), Reduction(), Restore()}, 2));
	EXPECT_FALSE(Admitted(
	    {Copy(), Scalar(ShaderInstructionType::SMovB64, Pair(16), Pair(30)), Saveexec(), Initializer(Pair(16)), Reduction(), Restore()},
	    2));
}

TEST(EmulatorFragmentNeutralRegion, RejectsUnknownMaskAndClobberedSave)
{
	auto program      = Region();
	program[0].src[0] = Pair(20);
	EXPECT_FALSE(Admitted(program));
	program        = Region();
	program[1].dst = program[1].src[0];
	EXPECT_FALSE(Admitted(program));
	program           = Region();
	program[2].src[2] = Saved();
	EXPECT_FALSE(Admitted(program));
}

TEST(EmulatorFragmentNeutralRegion, RejectsClobberedAndNeverCapturedMasks)
{
	EXPECT_FALSE(Admitted(Insert(Region(), 1, Scalar(ShaderInstructionType::SMovB64, Mask(), Pair(50))), 2));
	auto half   = Scalar(ShaderInstructionType::SMovB32, Operand(ShaderOperandType::Sgpr, 37, 1), Operand(ShaderOperandType::Sgpr, 50, 1));
	half.format = ShaderInstructionFormat::SVdstSVsrc0;
	EXPECT_FALSE(Admitted(Insert(Region(), 1, half), 2));
	EXPECT_FALSE(
	    Admitted({Scalar(ShaderInstructionType::SMovB64, Exec(), Mask()), Saveexec(), Initializer(Mask()), Reduction(), Restore()}));
	EXPECT_FALSE(Admitted({Saveexec(), Initializer(Mask()), Reduction(), Restore()}, 0));
}

TEST(EmulatorFragmentNeutralRegion, RejectsExecWrittenBetweenCopyAndEntry)
{
	EXPECT_FALSE(Admitted(Insert(Region(), 1, Scalar(ShaderInstructionType::SMovB64, Exec(), Pair(50))), 2));
	EXPECT_FALSE(Admitted(Insert(Region(), 1, Scalar(ShaderInstructionType::SAndSaveexecB64, Pair(52), Pair(50))), 2));
}

TEST(EmulatorFragmentNeutralRegion, RejectsAliasBetweenSaveAndCondition)
{
	const auto condition = Pair(16);
	EXPECT_FALSE(Admitted(
	    {Copy(), Derive(condition, Mask(), Pair(30)), Saveexec(condition), Initializer(condition), Reduction(), Restore(condition)}, 2));
}

TEST(EmulatorFragmentNeutralRegion, RejectsBypassedInitializerAndUndefinedDppDestination)
{
	EXPECT_FALSE(Admitted(Region(), 1, {ShaderLabel(4, 100)}));
	EXPECT_FALSE(Admitted(Region(), 1, {ShaderLabel(8, 100)}));
	EXPECT_FALSE(Admitted(Region(), 1, {ShaderLabel(12, 100)}));
	EXPECT_FALSE(Admitted(Region(), 1, {ShaderLabel(16, 100)}));
	auto program               = Region();
	program[3].dst.register_id = 11;
	EXPECT_FALSE(Admitted(program));
	program                       = Region();
	program[3].src[1].register_id = 9;
	EXPECT_FALSE(Admitted(program));
}

TEST(EmulatorFragmentNeutralRegion, RejectsInvertedConditionAndNonZeroNeutral)
{
	auto program = Region();
	std::swap(program[2].src[0], program[2].src[1]);
	EXPECT_FALSE(Admitted(program));
	program                      = Region();
	program[2].src[0].constant.u = 1;
	EXPECT_FALSE(Admitted(program));
	program                = Region();
	program[2].src[0].type = ShaderOperandType::FloatInlineConstant;
	EXPECT_FALSE(Admitted(program));
}

TEST(EmulatorFragmentNeutralRegion, RejectsMissingRestoreAndSideEffects)
{
	auto program      = Region();
	program[4].src[0] = Pair(30);
	EXPECT_FALSE(Admitted(program));
	program[4].src[0] = Operand(ShaderOperandType::VccLo, 0, 1);
	EXPECT_FALSE(Admitted(program));
	program = Region();
	program.pop_back();
	EXPECT_FALSE(Admitted(program));
	program         = Region();
	program[3].type = ShaderInstructionType::ImageStore;
	EXPECT_FALSE(Admitted(program));
	program[3].type = ShaderInstructionType::SCbranchScc0;
	EXPECT_FALSE(Admitted(program));
	EXPECT_FALSE(Admitted(Insert(Region(), 4, Scalar(ShaderInstructionType::SMovB64, Pair(40), Pair(42)))));
}

TEST(EmulatorFragmentNeutralRegion, AdmitsPlainBitwiseInstructionsAndRejectsPartialWrites)
{
	EXPECT_TRUE(Admitted(Insert(Region(), 4, Bitwise())));
	for (int field = 0; field < 4; ++field)
	{
		auto bitwise = Bitwise();
		switch (field)
		{
			case 0: bitwise.vop_sdwa = true; break;
			case 1: bitwise.vop_sdwa_ctrl = 0x06060600u; break;
			case 2: bitwise.vop3_op_sel = 1; break;
			default: bitwise.vop3_omod = 1; break;
		}
		EXPECT_FALSE(Admitted(Insert(Region(), 4, bitwise)));
	}
}

TEST(EmulatorFragmentNeutralRegion, RejectsMalformedRegistersAndModifiers)
{
	for (int field = 0; field < 15; ++field)
	{
		auto program = Region();
		switch (field)
		{
			case 0: program[2].dst.register_id = 300; break;
			case 1: program[2].dst.register_id = -1; break;
			case 2: program[2].dst.type = ShaderOperandType::Sgpr; break;
			case 3: program[2].dst.size = 2; break;
			case 4: program[2].src[1].register_id = 400; break;
			case 5: program[2].vop_sdwa = true; break;
			case 6: program[3].vop_sdwa = true; break;
			case 7: program[3].vop3_op_sel = 1; break;
			case 8: program[3].vop3_omod = 1; break;
			case 9: program[3].src_num = 9; break;
			case 10: program[3].src_num = -1; break;
			case 11: program[3].src[1].dpp = true; break;
			case 12: program[2].vop3_omod = 1; break;
			case 13: program[2].vop_sdwa_ctrl = 0x06060600u; break;
			default: program[3].dst.register_id = 300; break;
		}
		EXPECT_FALSE(Admitted(program));
	}
}

TEST(EmulatorFragmentNeutralRegion, ProvesPartialWaveReadsOnlyThroughRegions)
{
	EXPECT_TRUE(Proven(Insert(Region(), 5, Readlane(10))));
	const auto lone = ShaderAnalyzeFragmentPartialWaveReads(Build({Readlane(10)}));
	EXPECT_FALSE(lone.supported);
	EXPECT_EQ(lone.unsupported_pc, 0u);
	EXPECT_FALSE(Proven(Insert(Region(), 5, Readlane(9))));
	EXPECT_FALSE(Proven(Insert(Region(), 5, Readlane(300))));
}

TEST(EmulatorFragmentNeutralRegion, RejectsReadsReachableWithoutTheRegion)
{
	EXPECT_FALSE(Proven(Insert(Region(), 5, Readlane(10)), {ShaderLabel(20, 100)}));
	auto program                  = Region();
	program[3].src[1].register_id = 11;
	EXPECT_FALSE(Proven(Insert(program, 5, Readlane(10))));
	EXPECT_TRUE(Proven(Insert(Insert(Region(), 5, Scalar(ShaderInstructionType::SMovB64, Pair(40), Pair(42))), 6, Readlane(10))));
}

TEST(EmulatorFragmentNeutralRegion, FetchedInactiveRowReadsNeedProofButQuadReadsDoNot)
{
	EXPECT_FALSE(Proven({Reduction(true, 0x111u)}));
	EXPECT_TRUE(Proven({Reduction(false, 0x111u)}));
	EXPECT_TRUE(Proven({Reduction(true, 0xb1u)}));
	EXPECT_TRUE(Proven({Permute(0)}));
	EXPECT_FALSE(Proven({Permute(1)}));
	EXPECT_TRUE(Proven(Insert(Region(), 5, Permute(1))));
}

UT_END();
