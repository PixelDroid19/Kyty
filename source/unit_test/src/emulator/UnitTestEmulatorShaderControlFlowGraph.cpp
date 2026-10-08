#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderControlFlowGraph.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <vector>

UT_BEGIN(EmulatorShaderControlFlowGraph);

using namespace Libs::Graphics;

namespace {

enum class Op
{
	Nop,
	Scc0,
	Scc1,
	Branch,
	End,
	Setpc,
	Swappc,
	ClearExec,
	Kill,
};

struct Step
{
	uint32_t pc;
	Op       op;
	uint32_t target = 0;
};

ShaderInstructionType BranchType(Op op)
{
	switch (op)
	{
		case Op::Scc0: return ShaderInstructionType::SCbranchScc0;
		case Op::Scc1: return ShaderInstructionType::SCbranchScc1;
		default: return ShaderInstructionType::SBranch;
	}
}

// Decoded IR as the parser produces it: a label for every static branch. An
// instruction spans up to the next step's PC, so a gap is a wide encoding.
ShaderCode Program(const std::vector<Step>& steps)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	for (const auto& step: steps)
	{
		ShaderInstruction inst;
		inst.pc = step.pc;
		switch (step.op)
		{
			case Op::Nop:
				inst.type              = ShaderInstructionType::SInstPrefetch;
				inst.format            = ShaderInstructionFormat::Imm;
				inst.src_num           = 1;
				inst.src[0].type       = ShaderOperandType::LiteralConstant;
				inst.src[0].constant.u = 0;
				break;
			case Op::Scc0:
			case Op::Scc1:
			case Op::Branch:
				inst.type              = BranchType(step.op);
				inst.format            = ShaderInstructionFormat::Label;
				inst.src_num           = 1;
				inst.src[0].type       = ShaderOperandType::LiteralConstant;
				inst.src[0].constant.i = static_cast<int32_t>(step.target) - static_cast<int32_t>(step.pc + 4u);
				break;
			case Op::End:
				inst.type   = ShaderInstructionType::SEndpgm;
				inst.format = ShaderInstructionFormat::Empty;
				break;
			case Op::Setpc:
				inst.type        = ShaderInstructionType::SSetpcB64;
				inst.format      = ShaderInstructionFormat::Saddr;
				inst.src_num     = 1;
				inst.src[0].type = ShaderOperandType::Sgpr;
				inst.src[0].size = 2;
				break;
			case Op::Swappc:
				inst.type        = ShaderInstructionType::SSwappcB64;
				inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
				inst.src_num     = 1;
				inst.src[0].type = ShaderOperandType::Sgpr;
				inst.src[0].size = 2;
				inst.dst.type    = ShaderOperandType::Sgpr;
				inst.dst.size    = 2;
				break;
			case Op::ClearExec:
				inst.type        = ShaderInstructionType::SMovB64;
				inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
				inst.dst.type    = ShaderOperandType::ExecLo;
				inst.dst.size    = 2;
				inst.src_num     = 1;
				inst.src[0].type = ShaderOperandType::IntegerInlineConstant;
				inst.src[0].size = 2;
				break;
			case Op::Kill:
				inst.type            = ShaderInstructionType::Exp;
				inst.format          = ShaderInstructionFormat::Mrt0OffOffComprVmDone;
				inst.exp_enable_mask = 0;
				inst.exp_control     = 7;
				break;
		}
		code.GetInstructions().Add(inst);
		if (inst.format == ShaderInstructionFormat::Label)
		{
			code.GetLabels().Add(ShaderLabel(inst));
		}
	}
	return code;
}

void ExpectGraphRefused(const ShaderCode& code, ShaderCfgReject reject, uint32_t pc)
{
	const auto cfg = ShaderBuildControlFlowGraph(code);
	EXPECT_FALSE(cfg.Structurable());
	EXPECT_EQ(cfg.reject, reject);
	EXPECT_EQ(cfg.reject_pc, pc);
}

String8 EmitCompute(const ShaderCode& code)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderComputeInputInfo input {};
	input.threads_num[0] = 1;
	input.threads_num[1] = 1;
	input.threads_num[2] = 1;
	return SpirvGenerateSource(code, nullptr, nullptr, &input);
}

bool Validates(const String8& source, String8* error)
{
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		explicit ValidationConfig(bool enabled): m_enabled(enabled) {}
		bool         Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t      GetInteger(const Core::String&) const override { return 0; }
		bool         GetBool(const Core::String&) const override { return m_enabled; }
		Core::String GetString(const Core::String&) const override { return {}; }

	private:
		bool m_enabled;
	};
	const ValidationConfig restore(Config::ShaderValidationEnabled());
	Config::Load(ValidationConfig(true));
	Vector<uint32_t> binary;
	const bool       valid = ShaderToolchain::Run(source, &binary, error) && !binary.IsEmpty();
	Config::Load(restore);
	return valid;
}

void ExpectValidModule(const String8& source)
{
	String8 error;
	EXPECT_TRUE(Validates(source, &error)) << error.c_str();
}

// These graphs need a structurization the emitter does not perform; emitting
// them yields invalid modules. Translation fails closed instead.
void ExpectTranslationRefused(const ShaderCode& code)
{
	const auto source = EmitCompute(code);
	String8    error;
	EXPECT_FALSE(Validates(source, &error));
	EXPECT_TRUE(source.StartsWith("OpKytyControlFlowRejected")) << error.c_str();
}

// Blocks: 0 [00] 1 [04] 2 [08 0c] 3 [10] 4 [14] 5 [18] 6 [1c 20] 7 [24].
// The outer do-while is 1..6 (latch 6, merge 7); the inner one is 2..5
// (latch 5, merge 6). 0c breaks to 1c, the instruction after the inner latch;
// 10 skips 14 and continues at the inner latch.
ShaderCode NestedDoWhile()
{
	return Program({
	    {0x00, Op::Nop},
	    {0x04, Op::Nop},
	    {0x08, Op::Nop},
	    {0x0c, Op::Scc0, 0x1c},
	    {0x10, Op::Scc1, 0x18},
	    {0x14, Op::Nop},
	    {0x18, Op::Scc0, 0x08},
	    {0x1c, Op::Nop},
	    {0x20, Op::Scc1, 0x04},
	    {0x24, Op::End},
	});
}

// Blocks: 0 [00] 1 [04 08] 2 [0c 10] 3 [14]. Block 1 is its own latch; its
// fallthrough reaches the second latch, so loop {1} nests in loop {1, 2}.
ShaderCode NestedConditionalLatches()
{
	return Program({
	    {0x00, Op::Nop},
	    {0x04, Op::Nop},
	    {0x08, Op::Scc0, 0x04},
	    {0x0c, Op::Nop},
	    {0x10, Op::Scc1, 0x04},
	    {0x14, Op::End},
	});
}

// Blocks: 0 [00] 1 [04] 2 [08 0c] 3 [10] 4 [14 18] 5 [1c]. Both back edges are
// unconditional and share the continue of one loop {1, 2, 3, 4}; 10 exits.
ShaderCode SharedContinue()
{
	return Program({
	    {0x00, Op::Nop},
	    {0x04, Op::Scc0, 0x10},
	    {0x08, Op::Nop},
	    {0x0c, Op::Branch, 0x04},
	    {0x10, Op::Scc1, 0x1c},
	    {0x14, Op::Nop},
	    {0x18, Op::Branch, 0x04},
	    {0x1c, Op::End},
	});
}

// Blocks: 0 [00] 1 [04 08] 2 [0c] 3 [10 14] 4 [18 1c] 5 [20 24]. The loop
// {1, 2, 3} exits at 08 to 18 and at 0c to 20. Only 0c reaches 20, which ends
// the program without rejoining 18, so 18 is the merge.
ShaderCode ExitToTerminatingTail()
{
	return Program({
	    {0x00, Op::Nop},
	    {0x04, Op::Nop},
	    {0x08, Op::Scc0, 0x18},
	    {0x0c, Op::Scc1, 0x20},
	    {0x10, Op::Nop},
	    {0x14, Op::Branch, 0x04},
	    {0x18, Op::Nop},
	    {0x1c, Op::End},
	    {0x20, Op::Nop},
	    {0x24, Op::End},
	});
}

// Blocks: 0 [00] 1 [04] 2 [08 0c] 3 [10 14] 4 [18 1c 20] 5 [24]. The inner
// loop {2, 3} merges at 4 (after its latch at 14); 0c jumps to 24, past the
// outer latch at 20. The PC-range rules attribute that branch to the outer
// loop and branch to its merge from inside the inner construct.
ShaderCode MultiLevelBreak()
{
	return Program({
	    {0x00, Op::Nop},
	    {0x04, Op::Nop},
	    {0x08, Op::Nop},
	    {0x0c, Op::Scc0, 0x24},
	    {0x10, Op::Nop},
	    {0x14, Op::Scc1, 0x08},
	    {0x18, Op::Nop},
	    {0x1c, Op::Nop},
	    {0x20, Op::Scc1, 0x04},
	    {0x24, Op::End},
	});
}

// Blocks: 0 [00] 1 [04 08] 2 [0c] 3 [10 14] 4 [18] 5 [1c 20]. The loop exits
// to 18 and to 1c, and 18 falls through into 1c: neither exit can be the
// single merge.
ShaderCode ExitsReconvergingPastTheMerge()
{
	return Program({
	    {0x00, Op::Nop},
	    {0x04, Op::Nop},
	    {0x08, Op::Scc0, 0x18},
	    {0x0c, Op::Scc1, 0x1c},
	    {0x10, Op::Nop},
	    {0x14, Op::Branch, 0x04},
	    {0x18, Op::Nop},
	    {0x1c, Op::Nop},
	    {0x20, Op::End},
	});
}

// Blocks: 0 [00] 1 [04 08] 2 [0c 10] 3 [14]. The cycle 1 -> 2 -> 1 is
// entered at 1 by fallthrough and at 2 by the branch at 00; 1 does not
// dominate 2.
ShaderCode IrreducibleCycle()
{
	return Program({
	    {0x00, Op::Scc0, 0x0c},
	    {0x04, Op::Nop},
	    {0x08, Op::Nop},
	    {0x0c, Op::Nop},
	    {0x10, Op::Scc1, 0x04},
	    {0x14, Op::End},
	});
}

// Blocks: 0 [00] 1 [04 08] 2 [0c 10] 3 [14] 4 [18 1c] 5 [20]. Latch 2 and
// latch 4 both return to 1 from different arms; neither loop contains the
// other's latch.
ShaderCode SiblingLatches()
{
	return Program({
	    {0x00, Op::Nop},
	    {0x04, Op::Nop},
	    {0x08, Op::Scc0, 0x18},
	    {0x0c, Op::Nop},
	    {0x10, Op::Scc1, 0x04},
	    {0x14, Op::End},
	    {0x18, Op::Nop},
	    {0x1c, Op::Scc1, 0x04},
	    {0x20, Op::End},
	});
}

// Blocks: 0 [00] 1 [04] 2 [08 0c] 3 [10] 4 [14] 5 [18]. 0c returns to the
// outer header 04 from inside the loop {2, 3} headed at 08. The natural loops
// still nest ({1..4} contains {1, 2, 3}, which contains {2, 3}) because 3 reaches
// 0c through its own back edge; only the latch's innermost loop exposes it.
ShaderCode MultiLevelContinue()
{
	return Program({
	    {0x00, Op::Nop},
	    {0x04, Op::Nop},
	    {0x08, Op::Nop},
	    {0x0c, Op::Scc0, 0x04},
	    {0x10, Op::Scc1, 0x08},
	    {0x14, Op::Scc0, 0x04},
	    {0x18, Op::End},
	});
}

// Blocks: 0 [00] 1 [04 08] 2 [0c] 3 [10 14] 4 [18 1c 24]. The do-while
// {1, 2} leaves at 08 for the discard tail at 18, which the entry branch also
// reaches, so the tail is not under the loop header. The emitter gives each
// branch its own kill body: 08 is a terminating arm and 10 stays the merge.
ShaderCode LoopWithSharedDiscardTail()
{
	return Program({
	    {0x00, Op::Scc1, 0x18},
	    {0x04, Op::Nop},
	    {0x08, Op::Scc0, 0x18},
	    {0x0c, Op::Scc1, 0x04},
	    {0x10, Op::Nop},
	    {0x14, Op::End},
	    {0x18, Op::ClearExec},
	    {0x1c, Op::Kill},
	    {0x24, Op::End},
	});
}

// depth do-while loops nested around one body: headers at 04.., then latches
// innermost first. Loop i holds 2 * (depth - i) blocks.
ShaderCode DeepLoopNest(uint32_t depth)
{
	std::vector<Step> steps {{0x00, Op::Nop}};
	for (uint32_t i = 0; i < depth; i++)
	{
		steps.push_back({4 + 4 * i, Op::Nop});
	}
	for (uint32_t i = 0; i < depth; i++)
	{
		const uint32_t header = depth - 1 - i;
		steps.push_back({4 + 4 * depth + 4 * i, Op::Scc0, 4 + 4 * header});
	}
	steps.push_back({4 + 8 * depth, Op::End});
	return Program(steps);
}

} // namespace

TEST(EmulatorShaderControlFlowGraph, BuildsBlocksFromInstructionWidthsAndStaticTargets)
{
	// The instruction at 04 is eight bytes wide; the next one starts at 0c.
	const auto cfg = ShaderBuildControlFlowGraph(Program({{0x00, Op::Scc0, 0x0c}, {0x04, Op::Nop}, {0x0c, Op::End}}));

	ASSERT_TRUE(cfg.Structurable());
	ASSERT_EQ(cfg.blocks.size(), 3u);
	EXPECT_EQ(cfg.blocks[0].pc, 0x00u);
	EXPECT_EQ(cfg.blocks[1].pc, 0x04u);
	EXPECT_EQ(cfg.blocks[2].pc, 0x0cu);
	EXPECT_EQ(cfg.blocks[0].succ[0], 2u);
	EXPECT_EQ(cfg.blocks[0].succ[1], 1u);
	EXPECT_EQ(cfg.blocks[1].succ[0], kShaderCfgNone);
	EXPECT_EQ(cfg.blocks[1].succ[1], 2u);
	EXPECT_EQ(cfg.blocks[2].preds, (std::vector<uint32_t> {0u, 1u}));
	EXPECT_EQ(cfg.blocks[2].idom, 0u);
	EXPECT_EQ(cfg.BlockAt(0x08), 1u);
	EXPECT_TRUE(cfg.loops.empty());
}

TEST(EmulatorShaderControlFlowGraph, RefusesTargetsOutsideDecodedInstructions)
{
	// 08 lies inside the eight-byte instruction at 04.
	ExpectGraphRefused(Program({{0x00, Op::Scc0, 0x08}, {0x04, Op::Nop}, {0x0c, Op::End}}), ShaderCfgReject::UnresolvedTarget, 0x00);
	ExpectGraphRefused(Program({{0x00, Op::Branch, 0x40}, {0x04, Op::End}}), ShaderCfgReject::UnresolvedTarget, 0x00);
}

TEST(EmulatorShaderControlFlowGraph, EndsBlocksAtProgramExitsAndRefusesUnknownReturnSites)
{
	const auto cfg = ShaderBuildControlFlowGraph(Program({{0x00, Op::Scc0, 0x08}, {0x04, Op::Setpc}, {0x08, Op::End}}));

	ASSERT_TRUE(cfg.Structurable());
	ASSERT_EQ(cfg.blocks.size(), 3u);
	EXPECT_EQ(cfg.blocks[1].succ[0], kShaderCfgNone);
	EXPECT_EQ(cfg.blocks[1].succ[1], kShaderCfgNone);
	EXPECT_EQ(cfg.blocks[2].preds, (std::vector<uint32_t> {0u}));

	ExpectGraphRefused(Program({{0x00, Op::Nop}, {0x04, Op::Swappc}, {0x08, Op::End}}), ShaderCfgReject::IndirectTransfer, 0x04);
}

TEST(EmulatorShaderControlFlowGraph, ExcludesUnreachableBlocksFromDominanceAndLoops)
{
	// Blocks: 0 [00] 1 [04] 2 [08 0c] 3 [10] 4 [14 18] 5 [1c]. Nothing reaches
	// 14; its backward branch into the loop body must not make 08 a second
	// loop entry.
	const auto cfg = ShaderBuildControlFlowGraph(Program({
	    {0x00, Op::Nop},
	    {0x04, Op::Nop},
	    {0x08, Op::Nop},
	    {0x0c, Op::Scc0, 0x04},
	    {0x10, Op::End},
	    {0x14, Op::Nop},
	    {0x18, Op::Branch, 0x08},
	    {0x1c, Op::End},
	}));

	ASSERT_TRUE(cfg.Structurable());
	ASSERT_EQ(cfg.blocks.size(), 6u);
	EXPECT_TRUE(cfg.blocks[3].reachable);
	EXPECT_FALSE(cfg.blocks[4].reachable);
	EXPECT_FALSE(cfg.blocks[5].reachable);
	EXPECT_EQ(cfg.blocks[4].idom, kShaderCfgNone);
	EXPECT_EQ(cfg.blocks[2].preds, (std::vector<uint32_t> {1u, 4u}));
	EXPECT_EQ(cfg.blocks[2].idom, 1u);
	ASSERT_EQ(cfg.loops.size(), 1u);
	EXPECT_EQ(cfg.loops[0].header, 1u);
	EXPECT_EQ(cfg.loops[0].latch, 2u);
	EXPECT_EQ(cfg.loops[0].merge, 3u);
	EXPECT_EQ(cfg.loops[0].blocks, (std::vector<uint32_t> {1u, 2u}));
	EXPECT_EQ(cfg.blocks[4].loop, kShaderCfgNone);
}

TEST(EmulatorShaderControlFlowGraph, NestsDoWhileLoopsByDominance)
{
	const auto cfg = ShaderBuildControlFlowGraph(NestedDoWhile());

	ASSERT_TRUE(cfg.Structurable());
	ASSERT_EQ(cfg.blocks.size(), 8u);
	EXPECT_EQ(cfg.blocks[6].idom, 2u);
	EXPECT_EQ(cfg.blocks[5].idom, 3u);
	ASSERT_EQ(cfg.loops.size(), 2u);
	EXPECT_EQ(cfg.loops[0].header, 1u);
	EXPECT_EQ(cfg.loops[0].latch, 6u);
	EXPECT_EQ(cfg.loops[0].merge, 7u);
	EXPECT_EQ(cfg.loops[0].parent, kShaderCfgNone);
	EXPECT_EQ(cfg.loops[0].depth, 1u);
	EXPECT_TRUE(cfg.loops[0].conditional_latch);
	EXPECT_EQ(cfg.loops[0].blocks, (std::vector<uint32_t> {1u, 2u, 3u, 4u, 5u, 6u}));
	EXPECT_EQ(cfg.loops[1].header, 2u);
	EXPECT_EQ(cfg.loops[1].latch, 5u);
	EXPECT_EQ(cfg.loops[1].merge, 6u);
	EXPECT_EQ(cfg.loops[1].parent, 0u);
	EXPECT_EQ(cfg.loops[1].depth, 2u);
	EXPECT_EQ(cfg.loops[1].blocks, (std::vector<uint32_t> {2u, 3u, 4u, 5u}));
	EXPECT_EQ(cfg.InnermostLoopAt(0x0c), 1u);
	EXPECT_EQ(cfg.InnermostLoopAt(0x1c), 0u);
	EXPECT_EQ(cfg.InnermostLoopAt(0x24), kShaderCfgNone);
}

TEST(EmulatorShaderControlFlowGraph, NestsConditionalLatchesOfOneHeader)
{
	const auto cfg = ShaderBuildControlFlowGraph(NestedConditionalLatches());

	ASSERT_TRUE(cfg.Structurable());
	ASSERT_EQ(cfg.loops.size(), 2u);
	EXPECT_EQ(cfg.loops[0].header, 1u);
	EXPECT_EQ(cfg.loops[0].latch, 2u);
	EXPECT_EQ(cfg.loops[0].merge, 3u);
	EXPECT_EQ(cfg.loops[0].blocks, (std::vector<uint32_t> {1u, 2u}));
	EXPECT_EQ(cfg.loops[1].header, 1u);
	EXPECT_EQ(cfg.loops[1].latch, 1u);
	EXPECT_EQ(cfg.loops[1].merge, 2u);
	EXPECT_EQ(cfg.loops[1].parent, 0u);
	EXPECT_EQ(cfg.loops[1].blocks, (std::vector<uint32_t> {1u}));
}

TEST(EmulatorShaderControlFlowGraph, SharesOneLoopForUnconditionalBackEdges)
{
	const auto cfg = ShaderBuildControlFlowGraph(SharedContinue());

	ASSERT_TRUE(cfg.Structurable());
	ASSERT_EQ(cfg.loops.size(), 1u);
	EXPECT_EQ(cfg.loops[0].header, 1u);
	EXPECT_EQ(cfg.loops[0].latch, 4u);
	EXPECT_EQ(cfg.loops[0].merge, 5u);
	EXPECT_FALSE(cfg.loops[0].conditional_latch);
	EXPECT_EQ(cfg.loops[0].merge_branch_pc, 0x10u);
	EXPECT_EQ(cfg.loops[0].blocks, (std::vector<uint32_t> {1u, 2u, 3u, 4u}));
}

TEST(EmulatorShaderControlFlowGraph, SelectsTheMergeThatOtherExitsLeaveAlone)
{
	const auto cfg = ShaderBuildControlFlowGraph(ExitToTerminatingTail());

	ASSERT_TRUE(cfg.Structurable());
	ASSERT_EQ(cfg.loops.size(), 1u);
	EXPECT_EQ(cfg.loops[0].header, 1u);
	EXPECT_EQ(cfg.loops[0].latch, 3u);
	EXPECT_EQ(cfg.loops[0].merge, 4u);
	EXPECT_EQ(cfg.loops[0].merge_branch_pc, 0x08u);
	EXPECT_EQ(cfg.loops[0].blocks, (std::vector<uint32_t> {1u, 2u, 3u}));
	EXPECT_EQ(cfg.blocks[5].idom, 2u);

	ExpectGraphRefused(ExitsReconvergingPastTheMerge(), ShaderCfgReject::MultipleLoopExits, 0x0c);
	ExpectGraphRefused(MultiLevelBreak(), ShaderCfgReject::MultiLevelBreak, 0x0c);
}

TEST(EmulatorShaderControlFlowGraph, TreatsConditionalDiscardArmsAsTerminating)
{
	const auto code = LoopWithSharedDiscardTail();
	ASSERT_TRUE(code.ReadBlock(0x18).is_discard);

	const auto cfg = ShaderBuildControlFlowGraph(code);

	ASSERT_TRUE(cfg.Structurable());
	ASSERT_EQ(cfg.loops.size(), 1u);
	EXPECT_EQ(cfg.loops[0].blocks, (std::vector<uint32_t> {1u, 2u}));
	EXPECT_EQ(cfg.loops[0].merge, 3u);
	EXPECT_EQ(cfg.blocks[4].idom, 0u);
}

TEST(EmulatorShaderControlFlowGraph, RefusesLoopNestsBeyondTheMembershipBudget)
{
	const auto nested = ShaderBuildControlFlowGraph(DeepLoopNest(64));
	ASSERT_TRUE(nested.Structurable());
	ASSERT_EQ(nested.loops.size(), 64u);
	EXPECT_EQ(nested.loops[63].depth, 64u);

	// 1100 nested loops hold about 1.2 million block memberships in total.
	const auto cfg = ShaderBuildControlFlowGraph(DeepLoopNest(1100));
	EXPECT_FALSE(cfg.Structurable());
	EXPECT_EQ(cfg.reject, ShaderCfgReject::TooLarge);
}

TEST(EmulatorShaderControlFlowGraph, RefusesIrreducibleAndForwardBackEdges)
{
	ExpectGraphRefused(IrreducibleCycle(), ShaderCfgReject::IrreducibleLoop, 0x10);
	// Blocks: 0 [00] 1 [04] 2 [08 0c] 3 [10]. 2 dominates 1, so 04 -> 08 is
	// a back edge whose header follows its latch.
	ExpectGraphRefused(Program({
	                       {0x00, Op::Branch, 0x08},
	                       {0x04, Op::Branch, 0x08},
	                       {0x08, Op::Nop},
	                       {0x0c, Op::Scc1, 0x04},
	                       {0x10, Op::End},
	                   }),
	                   ShaderCfgReject::ForwardBackEdge, 0x04);
}

TEST(EmulatorShaderControlFlowGraph, RefusesSiblingLatchesAndMultiLevelContinues)
{
	ExpectGraphRefused(SiblingLatches(), ShaderCfgReject::SharedHeader, 0x1c);
	ExpectGraphRefused(MultiLevelContinue(), ShaderCfgReject::MultiLevelContinue, 0x0c);
}

TEST(EmulatorShaderControlFlowGraph, EmitsValidNestedDoWhileWithInnerBreakAndContinue)
{
	const auto source = EmitCompute(NestedDoWhile());

	EXPECT_NE(source.FindIndex("OpLoopMerge %loop_merge_0020 %loop_continue_0020 None"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpLoopMerge %loop_merge_0018 %loop_continue_0018 None"), Core::STRING8_INVALID_INDEX);
	// The break at 0c (instruction 3) leaves only the inner loop.
	EXPECT_NE(source.FindIndex("OpBranchConditional %cc_b_3 %loop_merge_0018 %t230_3"), Core::STRING8_INVALID_INDEX);
	ExpectValidModule(source);
}

TEST(EmulatorShaderControlFlowGraph, EmitsValidNestedConditionalLatches)
{
	const auto source = EmitCompute(NestedConditionalLatches());

	const auto outer = source.FindIndex("OpLoopMerge %loop_merge_0010 %loop_continue_0010 None");
	const auto inner = source.FindIndex("OpLoopMerge %loop_merge_0008 %loop_continue_0008 None");
	ASSERT_NE(outer, Core::STRING8_INVALID_INDEX);
	ASSERT_NE(inner, Core::STRING8_INVALID_INDEX);
	EXPECT_LT(outer, inner);
	ExpectValidModule(source);
}

TEST(EmulatorShaderControlFlowGraph, EmitsValidSharedContinueLoop)
{
	const auto source = EmitCompute(SharedContinue());

	EXPECT_NE(source.FindIndex("OpLoopMerge %label_001c_0010 %loop_continue_0018 None"), Core::STRING8_INVALID_INDEX);
	ExpectValidModule(source);
}

TEST(EmulatorShaderControlFlowGraph, EmitsValidLoopWithExitToTerminatingTail)
{
	const auto source = EmitCompute(ExitToTerminatingTail());

	EXPECT_NE(source.FindIndex("OpLoopMerge %label_0018_0008 %loop_continue_0014 None"), Core::STRING8_INVALID_INDEX);
	ExpectValidModule(source);
}

TEST(EmulatorShaderControlFlowGraph, RefusesMultiLevelBreakTranslation)
{
	ExpectTranslationRefused(MultiLevelBreak());
}

TEST(EmulatorShaderControlFlowGraph, RefusesLoopExitsReconvergingPastTheMergeTranslation)
{
	ExpectTranslationRefused(ExitsReconvergingPastTheMerge());
}

TEST(EmulatorShaderControlFlowGraph, RefusesIrreducibleCycleTranslation)
{
	ExpectTranslationRefused(IrreducibleCycle());
}

TEST(EmulatorShaderControlFlowGraph, RefusesSiblingLatchesTranslation)
{
	ExpectTranslationRefused(SiblingLatches());
}

TEST(EmulatorShaderControlFlowGraph, RefusesMultiLevelContinueTranslation)
{
	ExpectTranslationRefused(MultiLevelContinue());
}

UT_END();
