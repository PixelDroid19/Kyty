#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/Shader.h"
#include "../../../emulator/src/Graphics/ShaderParseInternal.h"

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

static void AppendWaterfallBody(WaterfallFixture* fixture)
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
	const auto branch  = BranchTo(fixture->branch_pc, head, ShaderInstructionType::SCbranchScc1);
	code.GetInstructions().Add(branch);
	code.GetLabels().Add(ShaderLabel(branch));

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

static ShaderInstruction M0Write(uint32_t pc)
{
	ShaderInstruction inst = ScalarWrite(pc, 0u);
	inst.dst.type             = ShaderOperandType::M0;
	inst.dst.size             = 1;
	return inst;
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

UT_END();
