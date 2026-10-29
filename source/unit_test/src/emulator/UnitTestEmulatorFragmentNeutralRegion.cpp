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

static bool Proven(const Program& program, const std::vector<ShaderLabel>& labels = {})
{
	return ShaderAnalyzeFragmentPartialWaveReads(Build(program, labels)).supported;
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

TEST(EmulatorFragmentNeutralRegion, WaveTierRegionNeedsAZeroSeedNotAnyIdentity)
{
	// Ghost lanes in the wave tier read zero, so a region seeded with another identity (all ones here) is no
	// neutral region for it; the same region with a zero seed is.
	EXPECT_TRUE(Proven(Insert(Region(), 5, Readlane(10))));
	auto program                 = Region();
	program[2].src[0].constant.u = 0xffffffffu;
	EXPECT_FALSE(Proven(Insert(program, 5, Readlane(10))));
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

static ShaderInstruction ReadlaneAt(int vgpr, uint32_t lane)
{
	auto readlane              = Readlane(vgpr);
	readlane.src[1].constant.u = lane;
	return readlane;
}

static ShaderInstruction ReadlaneDynamic(int vgpr)
{
	auto readlane   = Readlane(vgpr);
	readlane.src[1] = Operand(ShaderOperandType::Sgpr, 40, 1);
	return readlane;
}

static ShaderInstruction Mbcnt()
{
	ShaderInstruction mbcnt {};
	mbcnt.type    = ShaderInstructionType::VMbcntLoU32B32;
	mbcnt.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	mbcnt.dst     = Operand(ShaderOperandType::Vgpr, 20, 1);
	mbcnt.src[0]  = Pair(38);
	mbcnt.src[1]  = Operand(ShaderOperandType::Vgpr, 20, 1);
	mbcnt.src_num = 2;
	return mbcnt;
}

static ShaderInstruction NullExport()
{
	ShaderInstruction kill {};
	kill.type   = ShaderInstructionType::Exp;
	kill.format = ShaderInstructionFormat::NullVmDone;
	return kill;
}

static bool Tier(const Program& program, const std::vector<ShaderLabel>& labels = {})
{
	return ShaderAnalyzeFragmentNativeWaveTier(Build(program, labels)).supported;
}

TEST(EmulatorFragmentNativeWaveTier, RequiresAPixelProgramWithLaneExchanges)
{
	EXPECT_TRUE(Tier(Region()));
	EXPECT_FALSE(Tier({Scalar(ShaderInstructionType::SMovB64, Pair(40), Pair(42))}));
	auto compute = Build(Region());
	EXPECT_TRUE(ShaderAnalyzeFragmentNativeWaveTier(compute).supported);
	auto non_pixel = Build(Region());
	non_pixel.SetType(ShaderType::Compute);
	EXPECT_FALSE(ShaderAnalyzeFragmentNativeWaveTier(non_pixel).supported);
}

TEST(EmulatorFragmentNativeWaveTier, RejectsLaneExchangesOutsideProvenRegions)
{
	EXPECT_FALSE(Tier({Reduction()}));
	EXPECT_FALSE(Tier({Permute(0)}));
	EXPECT_FALSE(Tier({ReadlaneAt(10, 63)}));
	EXPECT_TRUE(Tier({ReadlaneAt(10, 31)}));
	EXPECT_TRUE(Tier({ReadlaneAt(10, 5)}));
	EXPECT_FALSE(Tier(Insert(Region(), 5, Permute(0))));
}

TEST(EmulatorFragmentNativeWaveTier, RejectsGroupingSensitivePopcountsAndReservedControls)
{
	EXPECT_FALSE(Tier({Mbcnt()}));
	auto mbcnt_hi = Mbcnt();
	mbcnt_hi.type = ShaderInstructionType::VMbcntHiU32B32;
	EXPECT_FALSE(Tier({mbcnt_hi}));
	EXPECT_FALSE(Tier(Insert(Region(), 5, Mbcnt())));
	EXPECT_FALSE(Tier({Reduction(false, 0x100u)}));
	auto reserved = Insert(Region(), 4, Reduction(false, 0x100u));
	EXPECT_FALSE(Tier(reserved));
}

TEST(EmulatorFragmentNativeWaveTier, GhostReadsNeedAProvenNeutralSource)
{
	EXPECT_TRUE(Tier(Insert(Region(), 5, ReadlaneAt(10, 63))));
	EXPECT_TRUE(Tier(Insert(Region(), 5, ReadlaneAt(10, 31))));
	EXPECT_FALSE(Tier(Insert(Region(), 5, ReadlaneAt(10, 64))));
	EXPECT_FALSE(Tier(Insert(Region(), 5, ReadlaneAt(11, 63))));
	EXPECT_TRUE(Tier(Insert(Region(), 5, ReadlaneDynamic(10))));
	EXPECT_FALSE(Tier(Insert(Region(), 5, ReadlaneDynamic(11))));
	EXPECT_FALSE(Tier(Insert(Region(), 4, ReadlaneAt(10, 63))));
	auto clobber   = Bitwise();
	clobber.dst    = Operand(ShaderOperandType::Vgpr, 10, 1);
	clobber.src[0] = Operand(ShaderOperandType::Vgpr, 11, 1);
	clobber.src[1] = clobber.src[0];
	EXPECT_FALSE(Tier(Insert(Insert(Region(), 5, clobber), 6, ReadlaneAt(10, 63))));
	EXPECT_FALSE(Tier(Insert(Region(), 5, ReadlaneAt(10, 63)), {ShaderLabel(20, 100)}));
}

TEST(EmulatorFragmentNativeWaveTier, LaneOpsInsideTheRegionSpanStayInsideItsProof)
{
	EXPECT_TRUE(Tier(Insert(Region(), 4, Permute(0))));
	EXPECT_FALSE(Tier(Insert(Region(), 4, Permute(1))));
	EXPECT_TRUE(Tier(Insert(Region(), 4, Reduction(false, 0x113u))));
	EXPECT_FALSE(Tier(Insert(Region(), 4, Reduction(false, 0x100u))));
}

TEST(EmulatorFragmentNativeWaveTier, RejectsKillsBeforeLaneExchanges)
{
	auto killed = Insert(Region(), 0, NullExport());
	EXPECT_FALSE(Tier(killed));
	EXPECT_TRUE(Tier(Insert(Region(), 5, NullExport())));
	auto exec_kill       = Region();
	exec_kill[0].type    = ShaderInstructionType::SMovB64;
	exec_kill[0].dst     = Exec();
	exec_kill[0].src[0]  = Operand(ShaderOperandType::LiteralConstant, 0, 0);
	exec_kill[0].src_num = 1;
	EXPECT_FALSE(Tier(exec_kill));
}

UT_END();
