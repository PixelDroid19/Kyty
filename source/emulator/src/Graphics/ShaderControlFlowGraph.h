#ifndef EMULATOR_SRC_GRAPHICS_SHADERCONTROLFLOWGRAPH_H_
#define EMULATOR_SRC_GRAPHICS_SHADERCONTROLFLOWGRAPH_H_

// Guest control-flow graph for structured SPIR-V emission.
//
// Built once per emitter from decoded instruction PCs and static branch
// targets; branch labels are emission metadata and are not consulted. A block
// starts at the entry, at every static branch target and after every
// terminator. A fallthrough reaches the next decoded instruction, so wide
// encodings keep their real extent. S_ENDPGM and S_SETPC_B64 leave the program.
// Dominators cover only the blocks reached from the entry; unreachable blocks
// have no dominator, take part in no edge check and belong to no loop.
//
// Loops are natural loops of dominator back edges. Unconditional back edges to
// one header share a single continue and form one loop. Each conditional back
// edge closes its own loop, and the loops of one header must nest by latch.
// A loop whose latch is conditional merges at the latch fallthrough. Otherwise
// the merge is the first exit target after the latch, in program order of the
// exiting branch, that every other exit leaves alone: an other exit may only
// enter a region dominated by the loop header that neither reaches the merge
// nor shares a block with the merge's region. A conditional branch into a
// discard tail (as ShaderCode::ReadBlock reports it) is a terminating arm that
// the emitter lowers with its own kill body, not a loop exit.
//
// The graph refuses every transfer the structured emitter cannot express and
// names the guest PC of the offending branch. The stages run in order:
// decoding, edges, loop headers, loop nesting, latches, loop exits; within a stage the
// first offender in program order wins. Guest-sized work is bounded: block
// count, aggregate loop membership and the steps of every walk have fixed
// budgets, and exceeding one refuses the program as TooLarge.

#include "Kyty/Core/Common.h"

#include "Emulator/Graphics/Shader.h"

#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

inline constexpr uint32_t kShaderCfgNone = UINT32_MAX;

enum class ShaderCfgReject : uint8_t
{
	None,
	// Instruction PCs decrease, or an analysis budget is exhausted.
	TooLarge,
	// A branch target is not the PC of a decoded instruction, its operand is
	// not a constant, or a conditional branch falls off the program end.
	UnresolvedTarget,
	// S_SWAPPC_B64: the return site is not modeled.
	IndirectTransfer,
	// A backward branch whose target does not dominate it: an irreducible cycle
	// or a backward jump that the emitter would lower as a loop.
	IrreducibleLoop,
	// A back edge to a header placed after its latch.
	ForwardBackEdge,
	// Back edges of one header that mix kinds, or conditional ones whose loops
	// do not nest. Reported at the later latch.
	SharedHeader,
	// A back edge that does not close the innermost loop around its latch: it
	// continues a header outside a nested loop it starts in.
	MultiLevelContinue,
	// A loop exit that also leaves the enclosing loop. Reported at the exit.
	MultiLevelBreak,
	// Loop exits that reconverge with the merge or outside the loop header's
	// dominance region. Reported at the first exit that is not the merge.
	MultipleLoopExits,
};

struct ShaderCfgBlock
{
	uint32_t pc      = 0; // first instruction
	uint32_t last_pc = 0; // last instruction, the terminator when there is one
	uint32_t last    = 0; // instruction index of last_pc at build time
	// Taken successor of a branch, then the fallthrough successor.
	uint32_t              succ[2] = {kShaderCfgNone, kShaderCfgNone};
	std::vector<uint32_t> preds; // ascending; every static predecessor, reached or not
	bool                  reachable = false;
	uint32_t              idom      = kShaderCfgNone;
	uint32_t              loop      = kShaderCfgNone; // innermost loop
	// Dominator-tree preorder interval of a reached block.
	uint32_t dom_in  = 0;
	uint32_t dom_out = 0;
};

struct ShaderCfgLoop
{
	uint32_t header            = kShaderCfgNone; // block
	uint32_t latch             = kShaderCfgNone; // block ending in the last back edge
	uint32_t merge             = kShaderCfgNone; // block; none when no exit follows the loop
	// First conditional branch, in program order, that exits to the merge;
	// kShaderCfgNone when only unconditional transfers reach it.
	uint32_t merge_branch_pc = kShaderCfgNone;
	uint32_t parent          = kShaderCfgNone; // loop
	uint32_t depth             = 0;              // 1 for an outermost loop
	bool     conditional_latch = false;
	// Ascending block indices.
	std::vector<uint32_t> blocks;
};

struct ShaderControlFlowGraph
{
	ShaderCfgReject reject    = ShaderCfgReject::None;
	uint32_t        reject_pc = 0;
	// Program order; block 0 is the entry.
	std::vector<ShaderCfgBlock> blocks;
	// A loop precedes the loops it contains; otherwise header program order.
	std::vector<ShaderCfgLoop> loops;

	[[nodiscard]] bool Structurable() const { return reject == ShaderCfgReject::None; }
	// Block whose instructions cover pc, or kShaderCfgNone.
	[[nodiscard]] uint32_t BlockAt(uint32_t pc) const;
	// Innermost loop containing the block at pc, or kShaderCfgNone.
	[[nodiscard]] uint32_t InnermostLoopAt(uint32_t pc) const;
	// False when either block is unreached.
	[[nodiscard]] bool Dominates(uint32_t dominator, uint32_t block) const;
};

[[nodiscard]] ShaderControlFlowGraph ShaderBuildControlFlowGraph(const ShaderCode& code);
[[nodiscard]] const char*            ShaderCfgRejectName(ShaderCfgReject reject);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERCONTROLFLOWGRAPH_H_ */
