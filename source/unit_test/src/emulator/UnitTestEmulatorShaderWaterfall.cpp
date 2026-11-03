#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"
#include "../../../emulator/src/Graphics/ShaderParseInternal.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include "ShaderArithmeticTestInterpreter.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

UT_BEGIN(EmulatorShaderWaterfall);

using namespace Libs::Graphics;

namespace {

constexpr uint32_t kRestRegister  = 20u;
constexpr uint32_t kScalarRegister = 4u;
constexpr uint32_t kIndexRegister = 7u;

struct WaterfallFixture
{
	ShaderCode code;
	uint32_t   head_pc = 0;
	uint32_t   move_pc = 0;
	uint32_t   andn2_pc = 0;
	uint32_t   exec_pc = 0;
	uint32_t   branch_pc = 0;
	uint32_t   rest_register = kRestRegister;
	uint32_t   scalar_register = kScalarRegister;
	uint32_t   index_register = kIndexRegister;
	uint32_t   original_instruction_count = 0;
};

static ShaderOperand Sgpr(uint32_t first, uint32_t size = 1u)
{
	return {.type = ShaderOperandType::Sgpr, .register_id = static_cast<int>(first), .size = static_cast<int>(size)};
}

static ShaderOperand Vgpr(uint32_t reg)
{
	return {.type = ShaderOperandType::Vgpr, .register_id = static_cast<int>(reg), .size = 1};
}

static ShaderOperand ExecPair()
{
	return {.type = ShaderOperandType::ExecLo, .size = 2};
}

static ShaderInstruction RestFromExec(uint32_t pc, uint32_t rest)
{
	ShaderInstruction inst {};
	inst.pc        = pc;
	inst.type      = ShaderInstructionType::SMovB64;
	inst.format    = ShaderInstructionFormat::Sdst2Ssrc02;
	inst.dst       = Sgpr(rest, 2u);
	inst.src[0]    = ExecPair();
	inst.src_num   = 1;
	return inst;
}

static ShaderInstruction BranchTo(uint32_t pc, uint32_t target, ShaderInstructionType type)
{
	ShaderInstruction inst {};
	inst.pc                = pc;
	inst.type              = type;
	inst.format            = ShaderInstructionFormat::Label;
	inst.src[0].type       = ShaderOperandType::LiteralConstant;
	inst.src[0].constant.i = static_cast<int32_t>(target) - static_cast<int32_t>(pc) - 4;
	inst.src_num           = 1;
	return inst;
}

static ShaderInstruction EndProgram(uint32_t pc)
{
	ShaderInstruction inst {};
	inst.pc     = pc;
	inst.type   = ShaderInstructionType::SEndpgm;
	inst.format = ShaderInstructionFormat::Empty;
	return inst;
}

static void AppendWaterfallBody(WaterfallFixture* fixture, uint32_t back_edge_target = UINT32_MAX)
{
	auto& code = fixture->code;
	const auto head = fixture->head_pc;

	ShaderInstruction readfirstlane {};
	readfirstlane.pc        = head;
	readfirstlane.type      = ShaderInstructionType::VReadfirstlaneB32;
	readfirstlane.format    = ShaderInstructionFormat::SVdstSVsrc0;
	readfirstlane.dst       = Sgpr(fixture->scalar_register);
	readfirstlane.src[0]    = Vgpr(fixture->index_register);
	readfirstlane.src_num   = 1;
	code.GetInstructions().Add(readfirstlane);

	ShaderInstruction compare {};
	compare.pc        = head + 4u;
	compare.type      = ShaderInstructionType::VCmpxEqU32;
	compare.format    = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	compare.dst       = ExecPair();
	compare.src[0]    = Sgpr(fixture->scalar_register);
	compare.src[1]    = Vgpr(fixture->index_register);
	compare.src_num   = 2;
	code.GetInstructions().Add(compare);

	ShaderInstruction set_m0 {};
	set_m0.pc        = head + 8u;
	set_m0.type      = ShaderInstructionType::SMovB32;
	set_m0.format    = ShaderInstructionFormat::SVdstSVsrc0;
	set_m0.dst.type  = ShaderOperandType::M0;
	set_m0.dst.size  = 1;
	set_m0.src[0]    = Sgpr(fixture->scalar_register);
	set_m0.src_num   = 1;
	code.GetInstructions().Add(set_m0);

	fixture->move_pc = head + 12u;
	ShaderInstruction move {};
	move.pc        = fixture->move_pc;
	move.type      = ShaderInstructionType::VMovrelsB32;
	move.format    = ShaderInstructionFormat::SVdstSVsrc0;
	move.dst       = Vgpr(30u);
	move.src[0]    = Vgpr(40u);
	move.src_num   = 1;
	code.GetInstructions().Add(move);

	fixture->andn2_pc = head + 16u;
	ShaderInstruction andn2 {};
	andn2.pc        = fixture->andn2_pc;
	andn2.type      = ShaderInstructionType::SAndn2B64;
	andn2.format    = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
	andn2.dst       = Sgpr(fixture->rest_register, 2u);
	andn2.src[0]    = andn2.dst;
	andn2.src[1]    = ExecPair();
	andn2.src_num   = 2;
	code.GetInstructions().Add(andn2);

	fixture->exec_pc = head + 20u;
	ShaderInstruction restore_exec {};
	restore_exec.pc      = fixture->exec_pc;
	restore_exec.type    = ShaderInstructionType::SMovB64;
	restore_exec.format  = ShaderInstructionFormat::Sdst2Ssrc02;
	restore_exec.dst     = ExecPair();
	restore_exec.src[0]  = Sgpr(fixture->rest_register, 2u);
	restore_exec.src_num = 1;
	code.GetInstructions().Add(restore_exec);

	fixture->branch_pc = head + 24u;
	const auto branch  = BranchTo(fixture->branch_pc, back_edge_target == UINT32_MAX ? head : back_edge_target, ShaderInstructionType::SCbranchScc1);
	code.GetInstructions().Add(branch);
	code.GetLabels().Add(ShaderLabel(branch));
	code.GetIndirectLabels().Add(ShaderLabel(fixture->branch_pc + 4u, fixture->branch_pc));

	code.GetInstructions().Add(EndProgram(fixture->branch_pc + 4u));
	fixture->original_instruction_count = code.GetInstructions().Size();
}

static WaterfallFixture ValidWaterfall()
{
	WaterfallFixture fixture {};
	fixture.code.GetInstructions().Add(RestFromExec(0u, fixture.rest_register));
	fixture.head_pc = 4u;
	AppendWaterfallBody(&fixture);
	return fixture;
}

static ShaderInstruction* AtPc(ShaderCode& code, uint32_t pc)
{
	for (auto& inst: code.GetInstructions())
	{
		if (inst.pc == pc) { return &inst; }
	}
	return nullptr;
}

static const ShaderInstruction* AtPc(const ShaderCode& code, uint32_t pc)
{
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.pc == pc) { return &inst; }
	}
	return nullptr;
}

static bool HasType(const ShaderCode& code, ShaderInstructionType type)
{
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type == type) { return true; }
	}
	return false;
}

static void ExpectWaterfallRetained(const WaterfallFixture& fixture)
{
	const auto& instructions = fixture.code.GetInstructions();
	EXPECT_EQ(instructions.Size(), fixture.original_instruction_count);

	const auto* first = AtPc(fixture.code, fixture.head_pc);
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(first->type, ShaderInstructionType::VReadfirstlaneB32);

	const auto* move = AtPc(fixture.code, fixture.move_pc);
	ASSERT_NE(move, nullptr);
	EXPECT_EQ(move->type, ShaderInstructionType::VMovrelsB32);
	EXPECT_EQ(move->src_num, 1);
	EXPECT_EQ(move->format, ShaderInstructionFormat::SVdstSVsrc0);

	const auto* branch = AtPc(fixture.code, fixture.branch_pc);
	ASSERT_NE(branch, nullptr);
	EXPECT_EQ(branch->type, ShaderInstructionType::SCbranchScc1);
	EXPECT_FALSE(fixture.code.GetLabels().At(0).IsDisabled());
	EXPECT_EQ(fixture.code.GetLabels().At(0).GetDst(), fixture.head_pc);
}

static void AppendForwardBranchTo(ShaderCode* code, uint32_t branch_pc, const ShaderInstruction& use)
{
	const auto branch = BranchTo(branch_pc, use.pc, ShaderInstructionType::SCbranchScc0);
	code->GetInstructions().Add(branch);
	code->GetLabels().Add(ShaderLabel(branch));
	code->GetInstructions().Add(EndProgram(branch_pc + 4u));
	code->GetInstructions().Add(use);
}

static ShaderInstruction IndirectPcTransfer(uint32_t pc, ShaderInstructionType type)
{
	ShaderInstruction inst {};
	inst.pc     = pc;
	inst.type   = type;
	inst.format = type == ShaderInstructionType::SSetpcB64 ? ShaderInstructionFormat::Saddr : ShaderInstructionFormat::Sdst2Ssrc02;
	if (type == ShaderInstructionType::SSwappcB64) { inst.dst = Sgpr(62u, 2u); }
	inst.src[0]    = Sgpr(60u, 2u);
	inst.src_num   = 1;
	return inst;
}

static ShaderInstruction ScalarWrite(uint32_t pc, uint32_t destination)
{
	ShaderInstruction inst {};
	inst.pc                    = pc;
	inst.type                  = ShaderInstructionType::SMovB32;
	inst.format                = ShaderInstructionFormat::SVdstSVsrc0;
	inst.dst                   = Sgpr(destination);
	inst.src[0].type           = ShaderOperandType::IntegerInlineConstant;
	inst.src[0].constant.u     = 0u;
	inst.src_num               = 1;
	return inst;
}

// EXEC is restored from s[24:25] and REST copies that pair; the back edge lands on a wait just before the head.
// `overwrite_source` writes s25 between the two copies.
static WaterfallFixture WaterfallEnteredThroughCopiedExec(bool overwrite_source)
{
	WaterfallFixture fixture {};
	auto&            instructions = fixture.code.GetInstructions();
	ShaderInstruction exec_from_source {};
	exec_from_source.pc      = 0u;
	exec_from_source.type    = ShaderInstructionType::SMovB64;
	exec_from_source.format  = ShaderInstructionFormat::Sdst2Ssrc02;
	exec_from_source.dst     = ExecPair();
	exec_from_source.src[0]  = Sgpr(24u, 2u);
	exec_from_source.src_num = 1;
	instructions.Add(exec_from_source);
	uint32_t pc = 4u;
	if (overwrite_source)
	{
		instructions.Add(ScalarWrite(pc, 25u));
		pc += 4u;
	}
	auto rest_from_source = exec_from_source;
	rest_from_source.pc   = pc;
	rest_from_source.dst  = Sgpr(fixture.rest_register, 2u);
	instructions.Add(rest_from_source);
	ShaderInstruction wait {};
	wait.pc                = pc + 4u;
	wait.type              = ShaderInstructionType::SWaitcnt;
	wait.format            = ShaderInstructionFormat::Imm;
	wait.src[0].type       = ShaderOperandType::LiteralConstant;
	wait.src_num           = 1;
	instructions.Add(wait);
	fixture.head_pc = wait.pc + 4u;
	AppendWaterfallBody(&fixture, wait.pc);
	return fixture;
}

static ShaderInstruction M0Write(uint32_t pc)
{
	ShaderInstruction inst = ScalarWrite(pc, 0u);
	inst.dst.type             = ShaderOperandType::M0;
	inst.dst.size             = 1;
	return inst;
}

static void InitializeWaterfallEmitterTest()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return true; }
		Core::String GetString(const Core::String&) const override { return {}; }
	} validation;
	Config::Load(validation);
}

static void PrintWaterfallSourceFailureContext(const String8& source)
{
	std::istringstream lines(source.c_str());
	std::string        line;
	uint32_t           printed = 0;
	while (std::getline(lines, line))
	{
		const bool relevant = line.find("%m0") != std::string::npos || line.find("%rel_") != std::string::npos ||
		                      line.find("%v30") != std::string::npos || line.find("%v40") != std::string::npos ||
		                      line.find("%v41") != std::string::npos;
		if (!relevant) { continue; }
		if (printed == 32u)
		{
			std::fprintf(stderr, "  ... source context truncated after %u relevant lines\n", printed);
			break;
		}
		const auto size = line.size() > 512u ? 512 : static_cast<int>(line.size());
		std::fprintf(stderr, "  %.*s%s\n", size, line.data(), line.size() > 512u ? "..." : "");
		++printed;
	}
	if (printed == 0u)
	{
		std::fprintf(stderr, "  no M0 or relative-move source lines found (%u source bytes)\n", source.Size());
	}
}

[[noreturn]] static void RunGeneratedLaneIndexValueContract()
{
	InitializeWaterfallEmitterTest();

	auto waterfall = ValidWaterfall();
	ShaderLowerWaterfallMoves(&waterfall.code);
	const auto* lowered_move = AtPc(waterfall.code, waterfall.move_pc);
	if (lowered_move == nullptr || lowered_move->src_num != 2 || lowered_move->format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1)
	{
		std::_Exit(2);
	}

	// Keep the production-lowered move, while giving the emitter one additional
	// named VGPR candidate with a distinct value for the lane and stale-M0 paths.
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderInstruction candidate {};
	candidate.pc        = 4u;
	candidate.type      = ShaderInstructionType::VMovB32;
	candidate.format    = ShaderInstructionFormat::SVdstSVsrc0;
	candidate.dst       = Vgpr(5u);
	candidate.src[0]    = Vgpr(41u);
	candidate.src_num   = 1;
	auto move  = *lowered_move;
	move.pc    = 0u;
	code.GetInstructions().Add(move);
	code.GetInstructions().Add(candidate);
	code.GetInstructions().Add(M0Write(8u)); // Declare the test's seeded stale M0 input after the instruction under test.
	code.GetInstructions().Add(EndProgram(12u));

	ShaderComputeInputInfo input {};
	// Match the established native wave32 compute fixtures: the workgroup has
	// one full guest wave and the physical subgroup has the same width.
	input.threads_num[0] = 32u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.float_mode = 0xc0u;
	input.fp_mode_known = true;
	input.wave_layout = {ShaderComputeWaveStrategy::Native, {32, 1, 1}, {32, 1, 1}, 32, 32, 1, 1, 0};
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Vector<uint32_t> binary;
	String8          error;
	if (source.IsEmpty())
	{
		std::fprintf(stderr, "waterfall lane-index fixture: SpirvGenerateSource returned empty output for native wave32 local size 32\n");
		std::fflush(stderr);
		std::_Exit(3);
	}
	if (!ShaderToolchain::Run(source, &binary, &error))
	{
		std::fprintf(stderr, "waterfall lane-index fixture: ShaderToolchain::Run failed: %s\n", error.c_str());
		PrintWaterfallSourceFailureContext(source);
		std::fflush(stderr);
		std::_Exit(4);
	}
	if (binary.IsEmpty())
	{
		std::fprintf(stderr, "waterfall lane-index fixture: ShaderToolchain::Run returned an empty binary\n");
		std::fflush(stderr);
		std::_Exit(5);
	}

	const ShaderArithmeticTest::Program program(source.c_str());
	const auto result = program.Run({{"%v5", 0u},
	                                 {"%v7", 1u}, // lane index selects VGPR 41
	                                 {"%v30", 0xdeadbeefu},
	                                 {"%v40", 0x3f000000u}, // stale M0=0 would select this value
	                                 {"%v41", 0x3f800000u},
	                                 {"%m0", 0u},
	                                 {"%exec_lo", UINT32_MAX},
	                                 {"%exec_hi", 0u},
	                                 {"%exec_lane_lo", 1u},
	                                 {"%exec_lane_hi", 0u},
	                                 {"%scc", 0u}},
	                                "%v30");
	if (result != 0x3f800000u)
	{
		std::fprintf(stderr, "waterfall lane-index fixture: lane index 1 with M0 0 produced 0x%08x; expected 0x3f800000\n", result);
		std::fflush(stderr);
		std::_Exit(6);
	}
	std::_Exit(0);
}

// V_MOVRELD stores through the lane's own index: lane index 1 with M0 0 writes VGPR 31, not VGPR 30.
[[noreturn]] static void RunGeneratedLaneIndexStoreContract()
{
	InitializeWaterfallEmitterTest();

	auto waterfall = ValidWaterfall();
	auto* original = AtPc(waterfall.code, waterfall.move_pc);
	original->type = ShaderInstructionType::VMovreldB32;
	ShaderLowerWaterfallMoves(&waterfall.code);
	const auto* lowered_move = AtPc(waterfall.code, waterfall.move_pc);
	if (lowered_move == nullptr || lowered_move->src_num != 2 || lowered_move->format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1)
	{
		std::_Exit(2);
	}

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderInstruction candidate {};
	candidate.pc        = 4u;
	candidate.type      = ShaderInstructionType::VMovB32;
	candidate.format    = ShaderInstructionFormat::SVdstSVsrc0;
	candidate.dst       = Vgpr(5u);
	candidate.src[0]    = Vgpr(31u);
	candidate.src_num   = 1;
	auto move  = *lowered_move;
	move.pc    = 0u;
	code.GetInstructions().Add(move);
	code.GetInstructions().Add(candidate);
	code.GetInstructions().Add(M0Write(8u));
	code.GetInstructions().Add(EndProgram(12u));

	ShaderComputeInputInfo input {};
	input.threads_num[0] = 32u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.float_mode = 0xc0u;
	input.fp_mode_known = true;
	input.wave_layout = {ShaderComputeWaveStrategy::Native, {32, 1, 1}, {32, 1, 1}, 32, 32, 1, 1, 0};
	const auto       source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	Vector<uint32_t> binary;
	String8          error;
	if (source.IsEmpty() || !ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		std::fprintf(stderr, "waterfall lane-index store fixture: generation failed: %s\n", error.c_str());
		PrintWaterfallSourceFailureContext(source);
		std::fflush(stderr);
		std::_Exit(3);
	}

	const ShaderArithmeticTest::Program program(source.c_str());
	const auto result = program.Run({{"%v5", 0u},
	                                 {"%v7", 1u},
	                                 {"%v30", 0xdeadbeefu}, // stale M0=0 would store here
	                                 {"%v31", 0xdeadbeefu},
	                                 {"%v40", 0x3f800000u},
	                                 {"%m0", 0u},
	                                 {"%exec_lo", UINT32_MAX},
	                                 {"%exec_hi", 0u},
	                                 {"%exec_lane_lo", 1u},
	                                 {"%exec_lane_hi", 0u},
	                                 {"%scc", 0u}},
	                                "%v31");
	if (result != 0x3f800000u)
	{
		std::fprintf(stderr, "waterfall lane-index store fixture: VGPR 31 holds 0x%08x; expected 0x3f800000\n", result);
		std::fflush(stderr);
		std::_Exit(4);
	}
	std::_Exit(0);
}

// Adds a second relative move after the first one, shifting the mask updates, back edge and end by one dword.
static void AppendSecondMove(WaterfallFixture* fixture, ShaderInstructionType type)
{
	std::vector<ShaderInstruction> instructions;
	for (const auto& inst: fixture->code.GetInstructions()) { instructions.push_back(inst); }
	auto second = *AtPc(fixture->code, fixture->move_pc);
	second.type = type;
	second.pc   = fixture->move_pc + 4u;
	second.dst  = Vgpr(50u);
	for (auto& inst: instructions)
	{
		if (inst.pc <= fixture->move_pc) { continue; }
		inst.pc += 4u;
		if (inst.type == ShaderInstructionType::SCbranchScc1) { inst.src[0].constant.i -= 4; }
	}
	fixture->andn2_pc += 4u;
	fixture->exec_pc += 4u;
	fixture->branch_pc += 4u;
	fixture->code.GetInstructions().Clear();
	fixture->code.GetLabels().Clear();
	fixture->code.GetIndirectLabels().Clear();
	for (const auto& inst: instructions)
	{
		fixture->code.GetInstructions().Add(inst);
		if (inst.pc == fixture->move_pc) { fixture->code.GetInstructions().Add(second); }
		if (inst.type == ShaderInstructionType::SCbranchScc1)
		{
			fixture->code.GetLabels().Add(ShaderLabel(inst));
			fixture->code.GetIndirectLabels().Add(ShaderLabel(inst.pc + 4u, inst.pc));
		}
	}
}

// An outer loop around the waterfall: `first` runs before REST is set, and a back edge after the waterfall returns to it.
static WaterfallFixture WaterfallInsideOuterLoop(const ShaderOperand& first_source)
{
	WaterfallFixture  fixture {};
	ShaderInstruction first {};
	first.pc      = 0u;
	first.type    = ShaderInstructionType::VMovB32;
	first.format  = ShaderInstructionFormat::SVdstSVsrc0;
	first.dst     = Vgpr(5u);
	first.src[0]  = first_source;
	first.src_num = 1;
	fixture.code.GetInstructions().Add(first);
	fixture.code.GetInstructions().Add(RestFromExec(4u, fixture.rest_register));
	fixture.head_pc = 8u;
	AppendWaterfallBody(&fixture);
	auto& instructions = fixture.code.GetInstructions();
	instructions.RemoveAt(instructions.Size() - 1u);
	const auto outer = BranchTo(fixture.branch_pc + 4u, 0u, ShaderInstructionType::SCbranchScc0);
	instructions.Add(outer);
	fixture.code.GetLabels().Add(ShaderLabel(outer));
	fixture.code.GetIndirectLabels().Add(ShaderLabel(outer.pc + 4u, outer.pc));
	instructions.Add(EndProgram(outer.pc + 4u));
	fixture.original_instruction_count = instructions.Size();
	return fixture;
}

} // namespace

TEST(EmulatorShaderWaterfall, RewritesLaneIndicesAndKeepsMaskUpdates)
{
	auto fixture = ValidWaterfall();
	ShaderLowerWaterfallMoves(&fixture.code);

	const auto& instructions = fixture.code.GetInstructions();
	ASSERT_EQ(instructions.Size(), 5u);
	EXPECT_EQ(instructions.At(1).pc, fixture.move_pc);
	EXPECT_EQ(instructions.At(1).type, ShaderInstructionType::VMovrelsB32);
	EXPECT_EQ(instructions.At(1).src_num, 2);
	EXPECT_EQ(instructions.At(1).format, ShaderInstructionFormat::SVdstSVsrc0SVsrc1);
	EXPECT_EQ(instructions.At(1).src[1].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instructions.At(1).src[1].register_id, static_cast<int>(fixture.index_register));

	const auto* mask_update = AtPc(fixture.code, fixture.andn2_pc);
	ASSERT_NE(mask_update, nullptr);
	EXPECT_EQ(mask_update->type, ShaderInstructionType::SAndn2B64);
	EXPECT_EQ(mask_update->dst.type, ShaderOperandType::Sgpr);
	EXPECT_EQ(mask_update->dst.register_id, static_cast<int>(fixture.rest_register));
	EXPECT_EQ(mask_update->dst.size, 2);
	EXPECT_EQ(mask_update->src[1].type, ShaderOperandType::ExecLo);
	EXPECT_EQ(mask_update->src[1].size, 2);

	const auto* exec_restore = AtPc(fixture.code, fixture.exec_pc);
	ASSERT_NE(exec_restore, nullptr);
	EXPECT_EQ(exec_restore->type, ShaderInstructionType::SMovB64);
	EXPECT_EQ(exec_restore->dst.type, ShaderOperandType::ExecLo);
	EXPECT_EQ(exec_restore->dst.size, 2);
	EXPECT_EQ(exec_restore->src[0].type, ShaderOperandType::Sgpr);
	EXPECT_EQ(exec_restore->src[0].register_id, static_cast<int>(fixture.rest_register));
	EXPECT_FALSE(HasType(fixture.code, ShaderInstructionType::VReadfirstlaneB32));
	EXPECT_FALSE(HasType(fixture.code, ShaderInstructionType::VCmpxEqU32));
	EXPECT_FALSE(HasType(fixture.code, ShaderInstructionType::SCbranchScc1));
	EXPECT_TRUE(fixture.code.GetLabels().At(0).IsDisabled());
	EXPECT_TRUE(fixture.code.GetIndirectLabels().At(0).IsDisabled());
}

TEST(EmulatorShaderWaterfall, RefusesLiveScalarAndImplicitM0AlongForwardBranchPaths)
{
	auto fixture = ValidWaterfall();
	fixture.code.GetInstructions().RemoveAt(fixture.code.GetInstructions().Size() - 1u);
	ShaderInstruction scalar_read {};
	scalar_read.pc       = fixture.branch_pc + 12u;
	scalar_read.type     = ShaderInstructionType::SMovB32;
	scalar_read.format   = ShaderInstructionFormat::SVdstSVsrc0;
	scalar_read.dst      = Sgpr(50u);
	scalar_read.src[0]   = Sgpr(fixture.scalar_register);
	scalar_read.src_num = 1;
	AppendForwardBranchTo(&fixture.code, fixture.branch_pc + 4u, scalar_read);
	fixture.original_instruction_count = fixture.code.GetInstructions().Size();

	ShaderLowerWaterfallMoves(&fixture.code);
	ExpectWaterfallRetained(fixture);

	auto m0_fixture = ValidWaterfall();
	m0_fixture.code.GetInstructions().RemoveAt(m0_fixture.code.GetInstructions().Size() - 1u);
	ShaderInstruction relative_read {};
	relative_read.pc        = m0_fixture.branch_pc + 12u;
	relative_read.type      = ShaderInstructionType::VMovrelsB32;
	relative_read.format    = ShaderInstructionFormat::SVdstSVsrc0;
	relative_read.dst       = Vgpr(31u);
	relative_read.src[0]    = Vgpr(41u);
	relative_read.src_num   = 1;
	AppendForwardBranchTo(&m0_fixture.code, m0_fixture.branch_pc + 4u, relative_read);
	m0_fixture.original_instruction_count = m0_fixture.code.GetInstructions().Size();

	ShaderLowerWaterfallMoves(&m0_fixture.code);
	ExpectWaterfallRetained(m0_fixture);
}

TEST(EmulatorShaderWaterfall, LowersLoopsEnteredThroughWaitsAndCopiedExecPairsBeforeLaterLoops)
{
	auto fixture = WaterfallEnteredThroughCopiedExec(false);
	// A later loop overwrites the scalar before it reads anything: the value is dead on its back edge as well.
	fixture.code.GetInstructions().RemoveAt(fixture.code.GetInstructions().Size() - 1u);
	const uint32_t later = fixture.branch_pc + 4u;
	fixture.code.GetInstructions().Add(ScalarWrite(later, fixture.scalar_register));
	const auto back_edge = BranchTo(later + 4u, later, ShaderInstructionType::SCbranchScc1);
	fixture.code.GetInstructions().Add(back_edge);
	fixture.code.GetLabels().Add(ShaderLabel(back_edge));
	fixture.code.GetInstructions().Add(EndProgram(later + 8u));
	ShaderLowerWaterfallMoves(&fixture.code);
	EXPECT_FALSE(HasType(fixture.code, ShaderInstructionType::VReadfirstlaneB32));
	const auto* move = AtPc(fixture.code, fixture.move_pc);
	ASSERT_NE(move, nullptr);
	EXPECT_EQ(move->src_num, 2);

	auto overwritten = WaterfallEnteredThroughCopiedExec(true);
	ShaderLowerWaterfallMoves(&overwritten.code);
	EXPECT_TRUE(HasType(overwritten.code, ShaderInstructionType::VReadfirstlaneB32)) << "REST no longer equals EXEC";
}

TEST(EmulatorShaderWaterfall, RefusesAnExternalEntryIntoTheLoopBody)
{
	auto fixture = ValidWaterfall();
	fixture.code.GetLabels().Add(ShaderLabel(fixture.move_pc, fixture.branch_pc + 16u));

	ShaderLowerWaterfallMoves(&fixture.code);
	ExpectWaterfallRetained(fixture);
}

TEST(EmulatorShaderWaterfall, RefusesUnprovenRestAndExecEntryState)
{
	WaterfallFixture wrong_rest {};
	ShaderInstruction initialize_wrong_rest = RestFromExec(0u, wrong_rest.rest_register);
	initialize_wrong_rest.src[0]           = Sgpr(40u, 2u);
	wrong_rest.code.GetInstructions().Add(initialize_wrong_rest);
	wrong_rest.head_pc = 4u;
	AppendWaterfallBody(&wrong_rest);
	ShaderLowerWaterfallMoves(&wrong_rest.code);
	ExpectWaterfallRetained(wrong_rest);

	WaterfallFixture clobbered_exec {};
	clobbered_exec.code.GetInstructions().Add(RestFromExec(0u, clobbered_exec.rest_register));
	ShaderInstruction exec_write {};
	exec_write.pc      = 4u;
	exec_write.type    = ShaderInstructionType::SMovB32;
	exec_write.format  = ShaderInstructionFormat::SVdstSVsrc0;
	exec_write.dst     = ExecPair();
	exec_write.dst.size = 1;
	exec_write.src[0].type = ShaderOperandType::IntegerInlineConstant;
	exec_write.src_num = 1;
	clobbered_exec.code.GetInstructions().Add(exec_write);
	clobbered_exec.head_pc = 8u;
	AppendWaterfallBody(&clobbered_exec);
	ShaderLowerWaterfallMoves(&clobbered_exec.code);
	ExpectWaterfallRetained(clobbered_exec);
}

TEST(EmulatorShaderWaterfall, RefusesExecHiClobberBeforeLoopHead)
{
	WaterfallFixture clobbered_exec_hi {};
	clobbered_exec_hi.code.GetInstructions().Add(RestFromExec(0u, clobbered_exec_hi.rest_register));
	ShaderInstruction exec_hi_write {};
	exec_hi_write.pc                = 4u;
	exec_hi_write.type              = ShaderInstructionType::SMovB32;
	exec_hi_write.format            = ShaderInstructionFormat::SVdstSVsrc0;
	exec_hi_write.dst.type          = ShaderOperandType::ExecHi;
	exec_hi_write.dst.size          = 1;
	exec_hi_write.src[0].type       = ShaderOperandType::IntegerInlineConstant;
	exec_hi_write.src[0].constant.u = 1u;
	exec_hi_write.src_num           = 1;
	clobbered_exec_hi.code.GetInstructions().Add(exec_hi_write);
	clobbered_exec_hi.head_pc = 8u;
	AppendWaterfallBody(&clobbered_exec_hi);

	ShaderLowerWaterfallMoves(&clobbered_exec_hi.code);
	ExpectWaterfallRetained(clobbered_exec_hi);
}

TEST(EmulatorShaderWaterfall, GeneratedMoveUsesLaneIndexInsteadOfStaleM0)
{
	ASSERT_EXIT(RunGeneratedLaneIndexValueContract(), ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderWaterfall, RefusesLiveValuesAcrossIndirectPcTransfers)
{
	auto live_scalar = ValidWaterfall();
	live_scalar.code.GetInstructions().RemoveAt(live_scalar.code.GetInstructions().Size() - 1u);
	live_scalar.code.GetInstructions().Add(IndirectPcTransfer(live_scalar.branch_pc + 4u, ShaderInstructionType::SSetpcB64));
	live_scalar.original_instruction_count = live_scalar.code.GetInstructions().Size();
	ShaderLowerWaterfallMoves(&live_scalar.code);
	ExpectWaterfallRetained(live_scalar);

	auto live_m0 = ValidWaterfall();
	live_m0.code.GetInstructions().RemoveAt(live_m0.code.GetInstructions().Size() - 1u);
	live_m0.code.GetInstructions().Add(IndirectPcTransfer(live_m0.branch_pc + 4u, ShaderInstructionType::SSwappcB64));
	live_m0.original_instruction_count = live_m0.code.GetInstructions().Size();
	ShaderLowerWaterfallMoves(&live_m0.code);
	ExpectWaterfallRetained(live_m0);

	auto overwritten = ValidWaterfall();
	overwritten.code.GetInstructions().RemoveAt(overwritten.code.GetInstructions().Size() - 1u);
	overwritten.code.GetInstructions().Add(ScalarWrite(overwritten.branch_pc + 4u, overwritten.scalar_register));
	overwritten.code.GetInstructions().Add(M0Write(overwritten.branch_pc + 8u));
	overwritten.code.GetInstructions().Add(
	    IndirectPcTransfer(overwritten.branch_pc + 12u, ShaderInstructionType::SSwappcB64));
	ShaderLowerWaterfallMoves(&overwritten.code);
	EXPECT_EQ(overwritten.code.GetInstructions().Size(), 7u);
	const auto* move = AtPc(overwritten.code, overwritten.move_pc);
	ASSERT_NE(move, nullptr);
	EXPECT_EQ(move->src_num, 2);
	EXPECT_FALSE(HasType(overwritten.code, ShaderInstructionType::VReadfirstlaneB32));
}

TEST(EmulatorShaderWaterfall, RefusesReadfirstlaneDestinationOverlappingRest)
{
	auto fixture = ValidWaterfall();
	fixture.scalar_register = fixture.rest_register + 1u;
	fixture.code.GetInstructions()[1].dst = Sgpr(fixture.scalar_register);
	fixture.code.GetInstructions()[2].src[0] = Sgpr(fixture.scalar_register);
	fixture.code.GetInstructions()[3].src[0] = Sgpr(fixture.scalar_register);

	ShaderLowerWaterfallMoves(&fixture.code);
	ExpectWaterfallRetained(fixture);
}

TEST(EmulatorShaderWaterfall, RefusesRelativeMoveOperandModifiers)
{
	auto source_modifier = ValidWaterfall();
	auto* source_move = AtPc(source_modifier.code, source_modifier.move_pc);
	ASSERT_NE(source_move, nullptr);
	source_move->src[0].absolute = true;
	ShaderLowerWaterfallMoves(&source_modifier.code);
	ExpectWaterfallRetained(source_modifier);
	EXPECT_TRUE(AtPc(source_modifier.code, source_modifier.move_pc)->src[0].absolute);

	auto destination_modifier = ValidWaterfall();
	auto* destination_move = AtPc(destination_modifier.code, destination_modifier.move_pc);
	ASSERT_NE(destination_move, nullptr);
	destination_move->dst.clamp = true;
	ShaderLowerWaterfallMoves(&destination_modifier.code);
	ExpectWaterfallRetained(destination_modifier);
	EXPECT_TRUE(AtPc(destination_modifier.code, destination_modifier.move_pc)->dst.clamp);
}

TEST(EmulatorShaderWaterfall, LowersDestinationRelativeMovesAtEachLaneIndex)
{
	auto fixture = ValidWaterfall();
	AtPc(fixture.code, fixture.move_pc)->type = ShaderInstructionType::VMovreldB32;
	ShaderLowerWaterfallMoves(&fixture.code);
	const auto* move = AtPc(fixture.code, fixture.move_pc);
	ASSERT_NE(move, nullptr);
	EXPECT_EQ(move->type, ShaderInstructionType::VMovreldB32);
	EXPECT_EQ(move->src_num, 2);
	EXPECT_EQ(move->src[1].register_id, static_cast<int>(fixture.index_register));
	EXPECT_FALSE(HasType(fixture.code, ShaderInstructionType::VReadfirstlaneB32));

	// The last move may overwrite the index: every lane already read it.
	auto reaching = ValidWaterfall();
	auto* last    = AtPc(reaching.code, reaching.move_pc);
	last->type    = ShaderInstructionType::VMovreldB32;
	last->dst     = Vgpr(reaching.index_register - 1u);
	ShaderLowerWaterfallMoves(&reaching.code);
	EXPECT_FALSE(HasType(reaching.code, ShaderInstructionType::VReadfirstlaneB32));
}

TEST(EmulatorShaderWaterfall, RefusesMovesAfterOneThatCanWriteTheIndex)
{
	auto unreachable = ValidWaterfall();
	AtPc(unreachable.code, unreachable.move_pc)->type = ShaderInstructionType::VMovreldB32;
	AppendSecondMove(&unreachable, ShaderInstructionType::VMovrelsB32);
	ShaderLowerWaterfallMoves(&unreachable.code);
	EXPECT_FALSE(HasType(unreachable.code, ShaderInstructionType::VReadfirstlaneB32));

	auto reaching = ValidWaterfall();
	auto* first   = AtPc(reaching.code, reaching.move_pc);
	first->type   = ShaderInstructionType::VMovreldB32;
	first->dst    = Vgpr(reaching.index_register - 1u);
	AppendSecondMove(&reaching, ShaderInstructionType::VMovrelsB32);
	const auto count = reaching.code.GetInstructions().Size();
	ShaderLowerWaterfallMoves(&reaching.code);
	EXPECT_EQ(reaching.code.GetInstructions().Size(), count);
	EXPECT_TRUE(HasType(reaching.code, ShaderInstructionType::VReadfirstlaneB32));
}

TEST(EmulatorShaderWaterfall, GeneratedStoreUsesLaneIndexInsteadOfStaleM0)
{
	ASSERT_EXIT(RunGeneratedLaneIndexStoreContract(), ::testing::ExitedWithCode(0), "");
}

// A back edge to before the loop carries S and M0 there: the loop is lowered only when that path writes them first.
TEST(EmulatorShaderWaterfall, FollowsOuterBackEdgesBeforeTheLoop)
{
	auto unread = WaterfallInsideOuterLoop(Vgpr(6u));
	ShaderLowerWaterfallMoves(&unread.code);
	EXPECT_FALSE(HasType(unread.code, ShaderInstructionType::VReadfirstlaneB32));

	auto read = WaterfallInsideOuterLoop(Sgpr(kScalarRegister));
	ShaderLowerWaterfallMoves(&read.code);
	ExpectWaterfallRetained(read);
}

UT_END();
