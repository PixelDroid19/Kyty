#include "ShaderSpirvScJoin.h"

#include "ShaderSpirvInternal.h"

#include "Kyty/Core/DbgAssert.h"

#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

bool instruction_is_conditional_branch(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchExecnz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz: return true;
		default: return false;
	}
}

bool instruction_is_block_terminator(const ShaderInstruction& inst)
{
	return instruction_is_conditional_branch(inst) || inst.type == ShaderInstructionType::SBranch ||
	       inst.type == ShaderInstructionType::SEndpgm || inst.type == ShaderInstructionType::SSetpcB64;
}

uint32_t loop_last_backedge(const ShaderCode& code, uint32_t header)
{
	uint32_t last = header;
	for (const auto& label: code.GetLabels())
	{
		if (!label.IsDisabled() && label.GetDst() == header && label.GetSrc() > last)
		{
			last = label.GetSrc();
		}
	}
	return last;
}

bool loop_has_unconditional_backedges(const ShaderCode& code, const SpirvSBranchLoop& loop)
{
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.pc < loop.header || inst.pc > loop.latch)
		{
			continue;
		}
		if (inst.type == ShaderInstructionType::SSetpcB64 || inst.type == ShaderInstructionType::SSwappcB64)
		{
			return false;
		}
		if (instruction_is_conditional_branch(inst) && ShaderLabel(inst).GetDst() == loop.header)
		{
			return false;
		}
		if (inst.pc == loop.latch)
		{
			return inst.type == ShaderInstructionType::SBranch && ShaderLabel(inst).GetDst() == loop.header;
		}
	}
	return false;
}

} // namespace

String8 SpirvSBranchLoop::HeaderName() const
{
	return String8::FromPrintf("label_%04" PRIx32 "_%04" PRIx32, header, latch);
}

String8 SpirvSBranchLoop::MergeName() const
{
	return String8::FromPrintf("label_%04" PRIx32 "_%04" PRIx32, merge, exit);
}

bool ScJoinFindSBranchLoop(const ShaderCode& code, uint32_t header, SpirvSBranchLoop* loop)
{
	SpirvSBranchLoop candidate {header, loop_last_backedge(code, header), 0, 0};
	if (candidate.latch == header || !loop_has_unconditional_backedges(code, candidate))
	{
		return false;
	}
	for (const auto& label: code.GetLabels())
	{
		if (label.IsDisabled())
		{
			continue;
		}
		const bool inside = label.GetSrc() >= header && label.GetSrc() <= candidate.latch;
		const bool target_inside = label.GetDst() >= header && label.GetDst() <= candidate.latch;
		// Incoming branches into the interval need a separate canonical-entry
		// transformation. Admit only a fallthrough entry here.
		if (!inside && target_inside)
		{
			return false;
		}
		if (!inside || target_inside)
		{
			continue;
		}
		if (label.GetDst() <= candidate.latch ||
		    (candidate.merge != 0 && candidate.merge != label.GetDst()))
		{
			return false;
		}
		candidate.merge = label.GetDst();
		candidate.exit = candidate.exit > label.GetSrc() ? candidate.exit : label.GetSrc();
	}
	// The final unconditional branch has no fallthrough. The following guest
	// block is reached only through the loop's explicit break edges.
	if (candidate.merge == 0 || static_cast<uint64_t>(candidate.latch) + 4 != candidate.merge)
	{
		return false;
	}
	*loop = candidate;
	return true;
}

bool ScJoinFindSBranchLoopContaining(const ShaderCode& code, uint32_t pc, SpirvSBranchLoop* loop)
{
	bool found = false;
	for (const auto& backedge: code.GetLabels())
	{
		if (backedge.IsDisabled() || backedge.GetDst() > pc || backedge.GetSrc() < pc ||
		    (found && backedge.GetDst() <= loop->header))
		{
			continue;
		}
		SpirvSBranchLoop candidate;
		if (ScJoinFindSBranchLoop(code, backedge.GetDst(), &candidate))
		{
			*loop = candidate;
			found = true;
		}
	}
	return found;
}

bool ScJoinFindSBranchLoopExit(const ShaderCode& code, const ShaderInstruction& inst, SpirvSBranchLoop* loop)
{
	if ((!instruction_is_conditional_branch(inst) && inst.type != ShaderInstructionType::SBranch) ||
	    !operand_is_constant(inst.src[0]))
	{
		return false;
	}
	const auto edge = ShaderLabel(inst);
	if (edge.GetDst() <= inst.pc)
	{
		return false;
	}
	return ScJoinFindSBranchLoopContaining(code, inst.pc, loop) && loop->merge == edge.GetDst();
}

// True when this forward conditional is a structured loop exit (jumps past a
// backward SBranch that closes a loop containing the exit).
bool ScJoinIsLoopExitEdge(const ShaderCode& code, const ShaderInstruction& exit_inst)
{
	if (!instruction_is_conditional_branch(exit_inst) || !operand_is_constant(exit_inst.src[0]))
	{
		return false;
	}
	const auto exit = ShaderLabel(exit_inst);
	if (exit.GetDst() <= exit_inst.pc)
	{
		return false;
	}
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type != ShaderInstructionType::SBranch || !operand_is_constant(inst.src[0]))
		{
			continue;
		}
		const auto backedge = ShaderLabel(inst);
		if (backedge.GetDst() >= backedge.GetSrc())
		{
			continue;
		}
		// Exit sits in the loop body and targets past the back-edge (loop merge).
		if (exit_inst.pc > backedge.GetDst() && exit_inst.pc < backedge.GetSrc() &&
		    exit.GetDst() > loop_last_backedge(code, backedge.GetDst()))
		{
			return true;
		}
	}
	return false;
}

bool ScJoinIsLoopMergeLabel(const ShaderCode& code, const ShaderLabel& label)
{
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.pc != label.GetSrc())
		{
			continue;
		}
		SpirvSBranchLoop loop;
		if (ScJoinFindSBranchLoopExit(code, inst, &loop))
		{
			return label.GetSrc() == loop.exit;
		}
		return ScJoinIsLoopExitEdge(code, inst);
	}
	return false;
}

int ScJoinCountLabelSources(const ShaderCode& code, uint32_t pc)
{
	int sources = 0;
	for (const auto& join_label: code.GetLabels())
	{
		if (!join_label.IsDisabled() && join_label.GetDst() == pc)
		{
			++sources;
		}
	}
	return sources;
}

uint32_t ScJoinFindTakenPathMultiJoin(const ShaderCode& code, uint32_t start_pc)
{
	const auto& instructions = code.GetInstructions();
	bool        started      = false;
	for (uint32_t i = 0; i < instructions.Size(); i++)
	{
		const auto& inst = instructions.At(i);
		if (inst.pc == start_pc)
		{
			started = true;
		}
		if (!started)
		{
			continue;
		}
		// Multi-join at or after the taken entry is the reconvergence target
		// when the path reaches it by fallthrough.
		// >=2 predecessors: if/else reconvergence and multi-way joins.
		if (inst.pc != start_pc && ScJoinCountLabelSources(code, inst.pc) >= 2)
		{
			return inst.pc;
		}
		if (inst.pc != start_pc && instruction_is_block_terminator(inst))
		{
			if (inst.type == ShaderInstructionType::SBranch && operand_is_constant(inst.src[0]))
			{
				const auto target = ShaderLabel(inst).GetDst();
				if (ScJoinCountLabelSources(code, target) >= 2)
				{
					return target;
				}
			}
			// Divergent or non-join terminator: no simple linear multi-join.
			return 0;
		}
	}
	if (ScJoinCountLabelSources(code, start_pc) >= 2)
	{
		return start_pc;
	}
	return 0;
}

uint32_t ScJoinFindNextMultiJoin(const ShaderCode& code, uint32_t start_pc)
{
	const auto& instructions = code.GetInstructions();
	bool        started      = false;
	for (uint32_t i = 0; i < instructions.Size(); i++)
	{
		const auto& inst = instructions.At(i);
		if (inst.pc == start_pc)
		{
			started = true;
		}
		if (!started)
		{
			continue;
		}
		if (ScJoinCountLabelSources(code, inst.pc) >= 2)
		{
			return inst.pc;
		}
	}
	return 0;
}

// Collect forward SBranch targets in [start_pc, end_pc) (end exclusive;
// end_pc==0 means unbounded). Stops at an SEndpgm.
// When only_multi_join is true, keep targets with >=2 label predecessors.
static void CollectForwardSBranchTargets(const ShaderCode& code, uint32_t start_pc, uint32_t end_pc, bool only_multi_join,
                                         Vector<uint32_t>* out)
{
	EXIT_IF(out == nullptr);
	const auto& instructions = code.GetInstructions();
	bool        started      = false;
	for (uint32_t i = 0; i < instructions.Size(); i++)
	{
		const auto& inst = instructions.At(i);
		if (inst.pc == start_pc)
		{
			started = true;
		}
		if (!started)
		{
			continue;
		}
		if (end_pc != 0 && inst.pc >= end_pc)
		{
			break;
		}
		if (inst.type == ShaderInstructionType::SEndpgm)
		{
			break;
		}
		if (inst.type == ShaderInstructionType::SBranch && operand_is_constant(inst.src[0]))
		{
			const uint32_t target = ShaderLabel(inst).GetDst();
			if (target <= inst.pc)
			{
				continue;
			}
			if (only_multi_join && ScJoinCountLabelSources(code, target) < 2)
			{
				continue;
			}
			bool seen = false;
			for (int j = 0; j < out->Size(); j++)
			{
				if ((*out)[j] == target)
				{
					seen = true;
					break;
				}
			}
			if (!seen)
			{
				out->Add(target);
			}
		}
	}
}

// True if a forward path from start_pc can land on target_pc along the
// "primary" progression used for reconvergence:
//   - fallthrough
//   - forward SBranch
//   - forward SCbranch taken when it is a structured loop exit
// Nested non-exit SCbranch taken arms are ignored (separate constructs).
// Backward SBranch ends the walk.
static bool PathCanReachPc(const ShaderCode& code, uint32_t start_pc, uint32_t target_pc)
{
	if (start_pc == target_pc)
	{
		return true;
	}
	if (start_pc > target_pc)
	{
		return false;
	}

	const auto&      instructions = code.GetInstructions();
	Vector<uint32_t> visited;
	uint32_t         pc = start_pc;

	auto seen = [&](uint32_t p) -> bool {
		for (int i = 0; i < visited.Size(); i++)
		{
			if (visited[i] == p)
			{
				return true;
			}
		}
		return false;
	};

	while (pc <= target_pc)
	{
		if (pc == target_pc)
		{
			return true;
		}
		if (seen(pc))
		{
			return false;
		}
		visited.Add(pc);

		int idx = -1;
		for (uint32_t i = 0; i < instructions.Size(); i++)
		{
			if (instructions.At(i).pc == pc)
			{
				idx = static_cast<int>(i);
				break;
			}
		}
		if (idx < 0)
		{
			return false;
		}

		bool advanced = false;
		for (uint32_t i = static_cast<uint32_t>(idx); i < instructions.Size(); i++)
		{
			const auto& inst = instructions.At(i);
			if (inst.pc == target_pc)
			{
				return true;
			}
			if (inst.pc > target_pc)
			{
				return false;
			}
			if (!instruction_is_block_terminator(inst))
			{
				continue;
			}
			if (inst.type == ShaderInstructionType::SBranch && operand_is_constant(inst.src[0]))
			{
				const uint32_t dst = ShaderLabel(inst).GetDst();
				if (dst == target_pc)
				{
					return true;
				}
				if (dst > inst.pc && dst <= target_pc)
				{
					pc       = dst;
					advanced = true;
					break;
				}
				return false;
			}
			if (instruction_is_conditional_branch(inst) && operand_is_constant(inst.src[0]))
			{
				const uint32_t taken = ShaderLabel(inst).GetDst();
				if (taken == target_pc)
				{
					return true;
				}
				// Loop exit is the primary way out of a loop toward a later join.
				if (taken > inst.pc && taken <= target_pc && ScJoinIsLoopExitEdge(code, inst))
				{
					pc       = taken;
					advanced = true;
					break;
				}
				// Non-exit conditional: continue on fallthrough only.
				if (i + 1 < instructions.Size())
				{
					const uint32_t fall = instructions.At(i + 1).pc;
					if (fall <= target_pc)
					{
						pc       = fall;
						advanced = true;
						break;
					}
				}
				return false;
			}
			return false;
		}
		if (!advanced)
		{
			return false;
		}
	}
	return false;
}

uint32_t ScJoinFindReconvergence(const ShaderCode& code, uint32_t taken_dst, uint32_t fallthrough_pc)
{
	// Return value contract:
	//   taken_dst — soft empty-case: fallthrough reaches taken (merge at taken)
	//   other PC  — both arms reconverge there (skip-over / shared SBranch join)
	//   0         — cannot form a structured selection merge for this edge

	// 1) Soft empty-case FIRST: fallthrough eventually reaches the taken label.
	//    Must precede multi-join-on-taken-path; otherwise a far multi-join such as
	//    1598 steals early mid-labels (1218) into one giant selection construct.
	if (PathCanReachPc(code, fallthrough_pc, taken_dst))
	{
		return taken_dst;
	}

	// 2) Fallthrough cannot reach taken (e.g. loop body then s_branch over the
	//    mid-label). Prefer an SBranch from a loop-merge block that jumps past
	//    taken_dst to T, when the taken arm can also reach T.
	//
	//    Example (PS 210017d0): SCbranchVccz → 0a84; false path loops then
	//    s_branch 0be8; taken falls through 0a84→0be8. Merge is 0be8, not 0a84.
	Vector<uint32_t> fall_targets;
	CollectForwardSBranchTargets(code, fallthrough_pc, taken_dst, /*only_multi_join=*/false, &fall_targets);

	uint32_t best = 0;
	for (int i = 0; i < fall_targets.Size(); i++)
	{
		const uint32_t j = fall_targets[i];
		if (j <= taken_dst)
		{
			continue;
		}
		if (!PathCanReachPc(code, taken_dst, j))
		{
			continue;
		}
		if (best == 0 || j < best)
		{
			best = j;
		}
	}
	if (best != 0)
	{
		return best;
	}

	// 3) Classic case cascade: taken path is a linear case that closes at a
	//    multi-join that fallthrough can also reach (without going through taken).
	const uint32_t linear_join = ScJoinFindTakenPathMultiJoin(code, taken_dst);
	if (linear_join != 0 && PathCanReachPc(code, fallthrough_pc, linear_join))
	{
		return linear_join;
	}

	// 4) Shared multi-join targets (stricter): both arms s_branch to the same join.
	Vector<uint32_t> fall_multi;
	Vector<uint32_t> taken_multi;
	CollectForwardSBranchTargets(code, fallthrough_pc, taken_dst, /*only_multi_join=*/true, &fall_multi);
	CollectForwardSBranchTargets(code, taken_dst, 0, /*only_multi_join=*/true, &taken_multi);
	for (int i = 0; i < fall_multi.Size(); i++)
	{
		const uint32_t j = fall_multi[i];
		if (j <= taken_dst)
		{
			continue;
		}
		for (int k = 0; k < taken_multi.Size(); k++)
		{
			if (taken_multi[k] == j && (best == 0 || j < best))
			{
				best = j;
			}
		}
	}
	return best;
}

bool ScJoinEdgeTakenDst(const ShaderCode& code, uint32_t src_pc, uint32_t join_pc, uint32_t* taken_dst)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t i = 0; i < instructions.Size(); i++)
	{
		const auto& cand = instructions.At(i);
		if (cand.pc != src_pc || !instruction_is_conditional_branch(cand) || !operand_is_constant(cand.src[0]))
		{
			continue;
		}
		const auto taken = ShaderLabel(cand);
		if (taken.GetDst() <= cand.pc)
		{
			return false;
		}
		uint32_t fallthrough_pc = taken.GetDst();
		if (i + 1 < instructions.Size())
		{
			fallthrough_pc = instructions.At(i + 1).pc;
		}
		if (ScJoinIsLoopExitEdge(code, cand))
		{
			return false;
		}
		const uint32_t recon = ScJoinFindReconvergence(code, taken.GetDst(), fallthrough_pc);
		// Edge owns join_pc only when reconvergence analysis says so (soft empty
		// returns taken_dst; skip-over returns the shared later join).
		if (recon != join_pc)
		{
			return false;
		}
		if (taken_dst != nullptr)
		{
			*taken_dst = taken.GetDst();
		}
		return true;
	}
	return false;
}

static uint32_t InstructionIndex(const ShaderCode& code, uint32_t pc)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t i = 0; i < instructions.Size(); ++i)
	{
		if (instructions.At(i).pc == pc)
		{
			return i;
		}
	}
	return instructions.Size();
}

static bool ArmReachesBeforeJoin(const ShaderCode& code, uint32_t start, uint32_t target, uint32_t join)
{
	const auto& instructions = code.GetInstructions();
	std::vector<uint32_t> pending {InstructionIndex(code, start)};
	std::vector<bool> visited(instructions.Size(), false);
	while (!pending.empty())
	{
		const uint32_t index = pending.back();
		pending.pop_back();
		if (index >= instructions.Size() || visited[index])
		{
			continue;
		}
		visited[index] = true;
		const auto& inst = instructions.At(index);
		if (inst.pc < start || inst.pc >= join)
		{
			continue;
		}
		if (inst.pc == target)
		{
			return true;
		}
		if (inst.type == ShaderInstructionType::SBranch || instruction_is_conditional_branch(inst))
		{
			pending.push_back(InstructionIndex(code, ShaderLabel(inst).GetDst()));
		}
		if (inst.type != ShaderInstructionType::SBranch && inst.type != ShaderInstructionType::SEndpgm &&
		    inst.type != ShaderInstructionType::SSetpcB64)
		{
			pending.push_back(index + 1);
		}
	}
	return false;
}

bool ScJoinIsNestedIn(const ShaderCode& code, uint32_t parent_src, uint32_t child_src, uint32_t join_pc)
{
	if (parent_src >= child_src || child_src >= join_pc)
	{
		return false;
	}
	uint32_t parent_taken = 0;
	if (!ScJoinEdgeTakenDst(code, parent_src, join_pc, &parent_taken))
	{
		return false;
	}
	// False-path / cascade arm: child sits before the parent's taken target.
	if (child_src < parent_taken)
	{
		return ArmReachesBeforeJoin(code, parent_src + 4, child_src, join_pc);
	}
	// Taken arm: child is at/after the mid-label. Nest only for skip-over parents
	// (mid body continues to join and can host further same-join edges).
	// Linear case bodies that end with SBranch to join must not claim later
	// sibling edges on divergent outer paths.
	if (parent_taken < join_pc && child_src >= parent_taken)
	{
		// A sibling case can lie in the same address interval while every path
		// from this arm branches over it. Follow both conditional successors.
		return ArmReachesBeforeJoin(code, parent_taken, child_src, join_pc);
	}
	return false;
}

// Fallthrough PC of the conditional edge at src_pc, or 0 if not found.
static uint32_t EdgeFallthroughPc(const ShaderCode& code, uint32_t src_pc)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t i = 0; i < instructions.Size(); i++)
	{
		if (instructions.At(i).pc != src_pc)
		{
			continue;
		}
		if (i + 1 < instructions.Size())
		{
			return instructions.At(i + 1).pc;
		}
		return 0;
	}
	return 0;
}

uint32_t ScJoinFindOwner(const ShaderCode& code, uint32_t pc, uint32_t join_pc, const Vector<uint32_t>& sc_join_srcs)
{
	// Ownership for a terminator/fallthrough at `pc` reconverging at `join_pc`.
	//
	// 1) Exact case closer (taken block ends at pc) always wins.
	// 2) Otherwise collect edges whose *execution path* contains `pc` (not merely
	//    PC-range overlap) and pick the deepest nested one.
	//
	// PC-range alone is insufficient: an outer selection's true arm can sit at a
	// PC between a nested soft-empty's src and the shared join without ever
	// entering that soft-empty. Claiming the nested sc_join then creates an
	// illegal entry into a selection mid-construct (PS 09b580e5 regression).
	// Reachability from the edge's fallthrough/taken arm filters those cases
	// while still letting true nested fallthrough close sc_join_06dc → parent
	// (PS 62eb07df).
	uint32_t best_exact  = 0;
	bool     found_exact = false;
	Vector<uint32_t> candidates;

	for (int i = 0; i < sc_join_srcs.Size(); i++)
	{
		const uint32_t src = sc_join_srcs[i];
		if (src >= pc)
		{
			continue;
		}
		uint32_t taken_dst = 0;
		if (!ScJoinEdgeTakenDst(code, src, join_pc, &taken_dst))
		{
			continue;
		}
		// Exact case closer: the taken case block terminates at this pc.
		const auto taken_block = code.ReadBlock(taken_dst);
		if (taken_block.is_valid && taken_block.last.pc == pc)
		{
			if (!found_exact || src > best_exact)
			{
				best_exact  = src;
				found_exact = true;
			}
			continue;
		}
		const uint32_t fall_pc = EdgeFallthroughPc(code, src);
		// Cascade fallthrough / default: between the edge and its taken case body.
		if (taken_dst != join_pc && pc > src && pc < taken_dst)
		{
			if (fall_pc != 0 && PathCanReachPc(code, fall_pc, pc))
			{
				candidates.Add(src);
			}
			continue;
		}
		// Case body span [taken, join): must actually be reachable from taken.
		if (taken_dst != join_pc && taken_dst <= pc && pc < join_pc)
		{
			if (PathCanReachPc(code, taken_dst, pc))
			{
				candidates.Add(src);
			}
			continue;
		}
		// Empty-case / soft empty-case: taken is the join; pc on the false path.
		if (taken_dst == join_pc && pc > src && pc < join_pc)
		{
			if (fall_pc != 0 && PathCanReachPc(code, fall_pc, pc))
			{
				candidates.Add(src);
			}
		}
	}
	if (found_exact)
	{
		return best_exact;
	}
	if (candidates.Size() == 0)
	{
		return 0;
	}
	// Deepest nesting first; tie-break highest src (innermost cascade).
	uint32_t best      = candidates[0];
	int      best_depth = ScJoinNestingDepth(code, best, join_pc, sc_join_srcs);
	for (int i = 1; i < candidates.Size(); i++)
	{
		const uint32_t src   = candidates[i];
		const int      depth = ScJoinNestingDepth(code, src, join_pc, sc_join_srcs);
		if (depth > best_depth || (depth == best_depth && src > best))
		{
			best       = src;
			best_depth = depth;
		}
	}
	return best;
}

uint32_t ScJoinFindParent(const ShaderCode& code, uint32_t src, uint32_t join_pc, const Vector<uint32_t>& sc_join_srcs)
{
	uint32_t parent = 0;
	for (int i = 0; i < sc_join_srcs.Size(); i++)
	{
		const uint32_t cand = sc_join_srcs[i];
		if (cand == src)
		{
			continue;
		}
		if (ScJoinIsNestedIn(code, cand, src, join_pc))
		{
			if (parent == 0 || cand > parent)
			{
				parent = cand;
			}
		}
	}
	return parent;
}

void ScJoinCollectSources(const ShaderCode& code, uint32_t join_pc, Vector<uint32_t>* out_srcs)
{
	EXIT_IF(out_srcs == nullptr);
	out_srcs->Clear();
	// Zero denotes an unresolved forward reconvergence, never a forward join.
	if (join_pc == 0)
	{
		return;
	}
	const auto& instructions = code.GetInstructions();
	for (uint32_t i = 0; i < instructions.Size(); i++)
	{
		const auto& cand = instructions.At(i);
		if (!instruction_is_conditional_branch(cand) || !operand_is_constant(cand.src[0]))
		{
			continue;
		}
		const auto taken = ShaderLabel(cand);
		if (taken.GetDst() <= cand.pc)
		{
			continue;
		}
		// Loop exits keep the guest merge label under OpLoopMerge — never sc_join.
		if (ScJoinIsLoopExitEdge(code, cand))
		{
			continue;
		}
		const uint32_t fall_pc = (i + 1 < instructions.Size()) ? instructions.At(i + 1).pc : taken.GetDst();
		const uint32_t recon   = ScJoinFindReconvergence(code, taken.GetDst(), fall_pc);
		if (recon == join_pc)
		{
			out_srcs->Add(cand.pc);
		}
	}
}

String8 ScJoinMergeName(uint32_t join_pc, uint32_t src_pc)
{
	return String8::FromPrintf("sc_join_%04" PRIx32 "_%04" PRIx32, join_pc, src_pc);
}

int ScJoinNestingDepth(const ShaderCode& code, uint32_t src, uint32_t join_pc, const Vector<uint32_t>& sc_join_srcs)
{
	int d = 0;
	for (int k = 0; k < sc_join_srcs.Size(); k++)
	{
		if (ScJoinIsNestedIn(code, sc_join_srcs[k], src, join_pc))
		{
			d++;
		}
	}
	return d;
}

void ScJoinOrderForEmission(const ShaderCode& code, uint32_t join_pc, const Vector<uint32_t>& sc_join_srcs, Vector<uint32_t>* out_order)
{
	EXIT_IF(out_order == nullptr);
	out_order->Clear();
	for (int i = 0; i < sc_join_srcs.Size(); i++)
	{
		out_order->Add(sc_join_srcs[i]);
	}
	// Deepest first; tie-break higher src pc (innermost cascade).
	for (int a = 0; a < out_order->Size(); a++)
	{
		for (int b = a + 1; b < out_order->Size(); b++)
		{
			const int da = ScJoinNestingDepth(code, (*out_order)[a], join_pc, sc_join_srcs);
			const int db = ScJoinNestingDepth(code, (*out_order)[b], join_pc, sc_join_srcs);
			if (db > da || (db == da && (*out_order)[b] > (*out_order)[a]))
			{
				const auto tmp   = (*out_order)[a];
				(*out_order)[a]  = (*out_order)[b];
				(*out_order)[b]  = tmp;
			}
		}
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
