#include "ShaderControlFlowGraph.h"

#include "ShaderStorageAnalysis.h"

#include <algorithm>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

// Every guest-sized walk spends from one step budget and loop membership is
// capped in aggregate, so deep nests and adversarial programs fail closed
// instead of growing host time or memory.
constexpr uint32_t kMaxBlocks         = 16384;
constexpr uint64_t kMaxLoopMembership = uint64_t {1} << 20u;
constexpr uint64_t kMaxSteps          = uint64_t {1} << 24u;
constexpr uint32_t kMaxExitTargets    = 16;

using Type = ShaderInstructionType;

bool IsConditional(Type type)
{
	return type != Type::SBranch && ShaderInstructionHasStaticBranchTarget(type);
}

bool EndsBlock(Type type)
{
	return ShaderInstructionHasStaticBranchTarget(type) || type == Type::SEndpgm || type == Type::SSetpcB64 ||
	       type == Type::SSwappcB64;
}

bool FallsThrough(Type type)
{
	return type != Type::SBranch && type != Type::SEndpgm && type != Type::SSetpcB64 && type != Type::SSwappcB64;
}

bool Contains(const ShaderCfgLoop& loop, uint32_t block)
{
	return std::binary_search(loop.blocks.begin(), loop.blocks.end(), block);
}

struct BackEdge
{
	uint32_t header      = 0;
	uint32_t latch       = 0;
	bool     conditional = false;
};

struct LoopExit
{
	uint32_t pc          = 0; // exiting instruction
	uint32_t target      = 0; // block
	bool     conditional = false;
};

// Blocks reached from one exit target, and whether any of them lies outside
// the loop header's dominance region.
struct Reach
{
	uint32_t              target  = kShaderCfgNone;
	bool                  escapes = false;
	std::vector<uint64_t> bits;
};

class Builder
{
public:
	Builder(const ShaderCode& code, ShaderControlFlowGraph* cfg): m_code(code), m_cfg(*cfg) {}

	void Run()
	{
		if (m_code.GetInstructions().IsEmpty())
		{
			return;
		}
		static_cast<void>(DecodeTargets() && CreateBlocks() && LinkBlocks() && FindReachable() && ComputeDominators() &&
		                  CheckEdges() && BuildLoops() && NestLoops() && CheckLatches() &&
		                  CheckExits());
	}

private:
	bool Reject(ShaderCfgReject reject, uint32_t pc)
	{
		m_cfg.reject    = reject;
		m_cfg.reject_pc = pc;
		return false;
	}

	bool Spend(uint64_t steps, uint32_t pc)
	{
		m_steps += steps;
		return m_steps <= kMaxSteps || Reject(ShaderCfgReject::TooLarge, pc);
	}

	[[nodiscard]] uint32_t LatchPc(uint32_t block) const { return m_cfg.blocks[block].last_pc; }

	bool ResolveTarget(const ShaderInstruction& inst, uint32_t* index) const
	{
		const auto& operand = inst.src[0];
		if (inst.src_num < 1 ||
		    (operand.type != ShaderOperandType::LiteralConstant && operand.type != ShaderOperandType::IntegerInlineConstant))
		{
			return false;
		}
		const int64_t target = static_cast<int64_t>(inst.pc) + 4 + operand.constant.i;
		if (target < 0 || target > UINT32_MAX)
		{
			return false;
		}
		const auto found = std::lower_bound(m_pcs.begin(), m_pcs.end(), static_cast<uint32_t>(target));
		if (found == m_pcs.end() || *found != static_cast<uint32_t>(target))
		{
			return false;
		}
		*index = static_cast<uint32_t>(found - m_pcs.begin());
		return true;
	}

	bool DecodeTargets()
	{
		const auto&    instructions = m_code.GetInstructions();
		const uint32_t count        = instructions.Size();
		m_pcs.resize(count);
		for (uint32_t i = 0; i < count; i++)
		{
			m_pcs[i] = instructions.At(i).pc;
			if (i > 0 && m_pcs[i] < m_pcs[i - 1])
			{
				return Reject(ShaderCfgReject::TooLarge, m_pcs[i]);
			}
		}
		m_targets.assign(count, kShaderCfgNone);
		for (uint32_t i = 0; i < count; i++)
		{
			const auto& inst = instructions.At(i);
			if (ShaderInstructionHasStaticBranchTarget(inst.type) && !ResolveTarget(inst, &m_targets[i]))
			{
				return Reject(ShaderCfgReject::UnresolvedTarget, inst.pc);
			}
		}
		for (const auto& inst: instructions)
		{
			if (inst.type == Type::SSwappcB64)
			{
				return Reject(ShaderCfgReject::IndirectTransfer, inst.pc);
			}
		}
		return true;
	}

	bool CreateBlocks()
	{
		const auto&       instructions = m_code.GetInstructions();
		const uint32_t    count        = instructions.Size();
		std::vector<bool> leader(count, false);
		leader[0] = true;
		for (uint32_t i = 0; i < count; i++)
		{
			if (m_targets[i] != kShaderCfgNone)
			{
				leader[m_targets[i]] = true;
			}
			if (i + 1 < count && EndsBlock(instructions.At(i).type))
			{
				leader[i + 1] = true;
			}
		}
		auto& blocks = m_cfg.blocks;
		m_block_of.resize(count);
		for (uint32_t i = 0; i < count; i++)
		{
			if (leader[i])
			{
				if (blocks.size() == kMaxBlocks)
				{
					return Reject(ShaderCfgReject::TooLarge, m_pcs[i]);
				}
				blocks.emplace_back();
				blocks.back().pc = m_pcs[i];
			}
			blocks.back().last_pc = m_pcs[i];
			blocks.back().last    = i;
			m_block_of[i]         = static_cast<uint32_t>(blocks.size() - 1);
		}
		return true;
	}

	bool LinkBlocks()
	{
		auto&          blocks = m_cfg.blocks;
		const uint32_t count  = static_cast<uint32_t>(blocks.size());
		for (uint32_t b = 0; b < count; b++)
		{
			auto&      block = blocks[b];
			const auto type  = m_code.GetInstructions().At(block.last).type;
			if (m_targets[block.last] != kShaderCfgNone)
			{
				block.succ[0] = m_block_of[m_targets[block.last]];
			}
			if (FallsThrough(type) && b + 1 < count)
			{
				block.succ[1] = b + 1;
			} else if (FallsThrough(type) && IsConditional(type))
			{
				return Reject(ShaderCfgReject::UnresolvedTarget, block.last_pc);
			}
		}
		for (uint32_t b = 0; b < count; b++)
		{
			const auto& succ = blocks[b].succ;
			if (succ[0] != kShaderCfgNone)
			{
				blocks[succ[0]].preds.push_back(b);
			}
			if (succ[1] != kShaderCfgNone && succ[1] != succ[0])
			{
				blocks[succ[1]].preds.push_back(b);
			}
		}
		return true;
	}

	// Depth-first from the entry; records the postorder used by dominators.
	bool FindReachable()
	{
		auto& blocks = m_cfg.blocks;
		m_post_number.assign(blocks.size(), kShaderCfgNone);
		std::vector<std::pair<uint32_t, uint32_t>> stack {{0u, 0u}};
		blocks[0].reachable = true;
		while (!stack.empty())
		{
			const uint32_t block = stack.back().first;
			const uint32_t next  = stack.back().second++;
			if (!Spend(1, blocks[block].pc))
			{
				return false;
			}
			if (next < 2)
			{
				const uint32_t succ = blocks[block].succ[next];
				if (succ != kShaderCfgNone && !blocks[succ].reachable)
				{
					blocks[succ].reachable = true;
					stack.emplace_back(succ, 0u);
				}
				continue;
			}
			m_post_number[block] = static_cast<uint32_t>(m_postorder.size());
			m_postorder.push_back(block);
			stack.pop_back();
		}
		return true;
	}

	uint32_t Intersect(const std::vector<uint32_t>& doms, uint32_t a, uint32_t b)
	{
		while (a != b)
		{
			if (!Spend(1, m_cfg.blocks[a].pc))
			{
				return kShaderCfgNone;
			}
			if (m_post_number[a] < m_post_number[b])
			{
				a = doms[a];
			} else
			{
				b = doms[b];
			}
		}
		return a;
	}

	// Immediate dominator from the reached predecessors processed so far, or
	// kShaderCfgNone when the step budget runs out.
	uint32_t MeetPredecessors(const std::vector<uint32_t>& doms, uint32_t block)
	{
		uint32_t meet = kShaderCfgNone;
		for (uint32_t pred: m_cfg.blocks[block].preds)
		{
			if (doms[pred] == kShaderCfgNone)
			{
				continue;
			}
			meet = meet == kShaderCfgNone ? pred : Intersect(doms, pred, meet);
			if (meet == kShaderCfgNone)
			{
				return kShaderCfgNone;
			}
		}
		return meet;
	}

	// Iterative dominators over reverse postorder (Cooper, Harvey, Kennedy).
	bool ComputeDominators()
	{
		auto&                 blocks = m_cfg.blocks;
		std::vector<uint32_t> doms(blocks.size(), kShaderCfgNone);
		doms[0]      = 0;
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (auto it = m_postorder.rbegin(); it != m_postorder.rend(); ++it)
			{
				if (*it == 0)
				{
					continue;
				}
				const uint32_t meet = MeetPredecessors(doms, *it);
				if (meet == kShaderCfgNone)
				{
					return Reject(ShaderCfgReject::TooLarge, blocks[*it].pc);
				}
				changed   = changed || doms[*it] != meet;
				doms[*it] = meet;
			}
		}
		for (uint32_t b = 1; b < blocks.size(); b++)
		{
			blocks[b].idom = doms[b];
		}
		NumberDominatorTree();
		return true;
	}

	void NumberDominatorTree()
	{
		auto&                 blocks = m_cfg.blocks;
		std::vector<uint32_t> child(blocks.size(), kShaderCfgNone);
		std::vector<uint32_t> sibling(blocks.size(), kShaderCfgNone);
		for (uint32_t b = static_cast<uint32_t>(blocks.size()); b > 1; b--)
		{
			const uint32_t idom = blocks[b - 1].idom;
			if (idom != kShaderCfgNone)
			{
				sibling[b - 1] = child[idom];
				child[idom]    = b - 1;
			}
		}
		uint32_t              counter = 0;
		std::vector<uint32_t> stack {0u};
		blocks[0].dom_in = counter++;
		while (!stack.empty())
		{
			const uint32_t node = stack.back();
			const uint32_t next = child[node];
			if (next == kShaderCfgNone)
			{
				blocks[node].dom_out = counter++;
				stack.pop_back();
				continue;
			}
			child[node]         = sibling[next];
			blocks[next].dom_in = counter++;
			stack.push_back(next);
		}
	}

	// Classifies every reached edge: a backward edge must return to a
	// dominating header and a dominating target must lie behind its source.
	bool CheckEdges()
	{
		const auto& blocks = m_cfg.blocks;
		for (uint32_t b = 0; b < blocks.size(); b++)
		{
			if (!blocks[b].reachable)
			{
				continue;
			}
			for (uint32_t k = 0; k < 2; k++)
			{
				const uint32_t target = blocks[b].succ[k];
				if (target == kShaderCfgNone || (k == 1 && target == blocks[b].succ[0]))
				{
					continue;
				}
				const bool dominates = m_cfg.Dominates(target, b);
				if (target <= b && !dominates)
				{
					return Reject(ShaderCfgReject::IrreducibleLoop, blocks[b].last_pc);
				}
				if (target > b && dominates)
				{
					return Reject(ShaderCfgReject::ForwardBackEdge, blocks[b].last_pc);
				}
				if (target <= b)
				{
					const auto type = m_code.GetInstructions().At(blocks[b].last).type;
					m_back_edges.push_back({target, b, IsConditional(type)});
				}
			}
		}
		return true;
	}

	// Natural loop of the given latches: the header plus every reached block
	// that reaches a latch without passing the header.
	bool AddLoop(uint32_t header, const BackEdge* first, const BackEdge* last)
	{
		const auto&   blocks = m_cfg.blocks;
		ShaderCfgLoop loop;
		loop.header            = header;
		loop.latch             = (last - 1)->latch;
		loop.conditional_latch = first->conditional;
		const auto stamp       = static_cast<uint32_t>(m_cfg.loops.size() + 1);
		std::vector<uint32_t> pending;
		const auto            visit = [&](uint32_t block)
		{
			if (m_mark[block] == stamp)
			{
				return true;
			}
			m_mark[block] = stamp;
			loop.blocks.push_back(block);
			pending.push_back(block);
			return ++m_membership <= kMaxLoopMembership || Reject(ShaderCfgReject::TooLarge, LatchPc(loop.latch));
		};
		m_mark[header] = stamp;
		loop.blocks.push_back(header);
		for (const auto* edge = first; edge != last; ++edge)
		{
			if (!visit(edge->latch))
			{
				return false;
			}
		}
		while (!pending.empty())
		{
			const uint32_t block = pending.back();
			pending.pop_back();
			if (!Spend(1 + blocks[block].preds.size(), blocks[block].pc))
			{
				return false;
			}
			for (uint32_t pred: blocks[block].preds)
			{
				if (blocks[pred].reachable && !visit(pred))
				{
					return false;
				}
			}
		}
		std::sort(loop.blocks.begin(), loop.blocks.end());
		m_cfg.loops.push_back(std::move(loop));
		return true;
	}

	// One header's back edges: unconditional ones share one loop, conditional
	// ones each close a loop that must contain the previous latch.
	bool AddHeaderLoops(const BackEdge* first, const BackEdge* last)
	{
		for (const auto* edge = first + 1; edge != last; ++edge)
		{
			if (edge->conditional != first->conditional)
			{
				return Reject(ShaderCfgReject::SharedHeader, LatchPc(edge->latch));
			}
		}
		if (!first->conditional)
		{
			return AddLoop(first->header, first, last);
		}
		for (const auto* edge = first; edge != last; ++edge)
		{
			if (!AddLoop(edge->header, edge, edge + 1))
			{
				return false;
			}
			if (edge != first && !Contains(m_cfg.loops.back(), (edge - 1)->latch))
			{
				return Reject(ShaderCfgReject::SharedHeader, LatchPc(edge->latch));
			}
		}
		return true;
	}

	bool BuildLoops()
	{
		std::sort(m_back_edges.begin(), m_back_edges.end(), [](const BackEdge& a, const BackEdge& b)
		          { return a.header != b.header ? a.header < b.header : a.latch < b.latch; });
		m_mark.assign(m_cfg.blocks.size(), 0);
		for (size_t first = 0; first < m_back_edges.size();)
		{
			size_t last = first + 1;
			while (last < m_back_edges.size() && m_back_edges[last].header == m_back_edges[first].header)
			{
				last++;
			}
			if (!AddHeaderLoops(m_back_edges.data() + first, m_back_edges.data() + last))
			{
				return false;
			}
			first = last;
		}
		return true;
	}

	// Loops overlap without nesting. Name the back edge whose latch lies inside
	// the other loop while its header does not.
	bool RejectOverlap(uint32_t loop, uint32_t parent, uint32_t other)
	{
		uint32_t partner = other;
		for (uint32_t outer = parent; outer != kShaderCfgNone; outer = m_cfg.loops[outer].parent)
		{
			if (outer == other)
			{
				partner = parent;
				break;
			}
		}
		if (other == kShaderCfgNone)
		{
			partner = parent;
		}
		const auto& inner = m_cfg.loops[loop];
		const auto& outer = m_cfg.loops[partner];
		const bool  outer_leaves = Contains(inner, outer.latch) && !Contains(inner, outer.header);
		return Reject(ShaderCfgReject::MultiLevelContinue, LatchPc(outer_leaves ? outer.latch : inner.latch));
	}

	// Loops sorted outer first; each loop must lie wholly inside the innermost
	// earlier loop that contains its header.
	bool NestLoops()
	{
		auto& loops = m_cfg.loops;
		std::stable_sort(loops.begin(), loops.end(), [](const ShaderCfgLoop& a, const ShaderCfgLoop& b)
		                 {
			                 if (a.header != b.header)
			                 {
				                 return a.header < b.header;
			                 }
			                 return a.blocks.size() != b.blocks.size() ? a.blocks.size() > b.blocks.size() : a.latch > b.latch;
		                 });
		auto& blocks = m_cfg.blocks;
		for (uint32_t l = 0; l < loops.size(); l++)
		{
			const uint32_t parent = blocks[loops[l].header].loop;
			if (!Spend(loops[l].blocks.size(), blocks[loops[l].header].pc))
			{
				return false;
			}
			for (uint32_t block: loops[l].blocks)
			{
				if (blocks[block].loop != parent)
				{
					return RejectOverlap(l, parent, blocks[block].loop);
				}
			}
			loops[l].parent = parent;
			loops[l].depth  = parent == kShaderCfgNone ? 1 : loops[parent].depth + 1;
			for (uint32_t block: loops[l].blocks)
			{
				blocks[block].loop = l;
			}
		}
		return true;
	}

	// Every back edge must close the innermost loop around its latch. A back
	// edge that leaves a deeper loop for an outer header (a multi-level
	// continue) still yields properly nested natural loops: the deeper loop
	// reaches that latch without the outer header, so it lies inside the outer
	// edge's natural loop and the overlap check cannot see it.
	bool CheckLatches()
	{
		uint32_t offender = kShaderCfgNone;
		for (const auto& edge: m_back_edges)
		{
			const auto& loop   = m_cfg.loops[m_cfg.blocks[edge.latch].loop];
			const bool  closes = loop.header == edge.header && loop.conditional_latch == edge.conditional &&
			                    (!edge.conditional || loop.latch == edge.latch);
			if (!closes && (offender == kShaderCfgNone || edge.latch < offender))
			{
				offender = edge.latch;
			}
		}
		return offender == kShaderCfgNone || Reject(ShaderCfgReject::MultiLevelContinue, LatchPc(offender));
	}

	bool ComputeReach(uint32_t header, Reach* reach)
	{
		const auto& blocks = m_cfg.blocks;
		reach->bits.assign((blocks.size() + 63) / 64, 0);
		std::vector<uint32_t> pending {reach->target};
		reach->bits[reach->target / 64] |= uint64_t {1} << (reach->target % 64);
		while (!pending.empty())
		{
			const uint32_t block = pending.back();
			pending.pop_back();
			if (!Spend(1, blocks[block].pc))
			{
				return false;
			}
			reach->escapes = reach->escapes || !m_cfg.Dominates(header, block);
			for (uint32_t succ: blocks[block].succ)
			{
				const uint64_t bit = succ == kShaderCfgNone ? 0 : uint64_t {1} << (succ % 64);
				if (bit != 0 && (reach->bits[succ / 64] & bit) == 0)
				{
					reach->bits[succ / 64] |= bit;
					pending.push_back(succ);
				}
			}
		}
		return true;
	}

	// True when the loop's single exit target needs no region check because
	// it is the merge.
	[[nodiscard]] bool SoleTargetIsMerge(const ShaderCfgLoop& loop, const std::vector<uint32_t>& targets) const
	{
		const auto& latch = m_cfg.blocks[loop.latch];
		if (targets.size() != 1)
		{
			return false;
		}
		return loop.conditional_latch ? targets[0] == latch.succ[1] : m_cfg.blocks[targets[0]].pc > latch.last_pc;
	}

	// Reach of every distinct exit target. The common loop with one exit to its
	// merge needs none.
	bool ComputeReaches(const ShaderCfgLoop& loop, const std::vector<uint32_t>& targets, std::vector<Reach>* reaches)
	{
		const bool needed = !SoleTargetIsMerge(loop, targets);
		reaches->clear();
		reaches->resize(targets.size());
		for (size_t i = 0; i < targets.size(); i++)
		{
			(*reaches)[i].target = targets[i];
			if (needed && !ComputeReach(loop.header, &(*reaches)[i]))
			{
				return false;
			}
		}
		return true;
	}

	// The other exit's region stays under the header and never meets the merge.
	static bool LeavesMergeAlone(const Reach& other, const Reach* merge)
	{
		if (other.escapes)
		{
			return false;
		}
		if (merge == nullptr)
		{
			return true;
		}
		for (size_t w = 0; w < other.bits.size(); w++)
		{
			if ((other.bits[w] & merge->bits[w]) != 0)
			{
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] const LoopExit* FirstExitTo(const std::vector<LoopExit>& exits, uint32_t target) const
	{
		for (const auto& exit: exits)
		{
			if (exit.target == target)
			{
				return &exit;
			}
		}
		return nullptr;
	}

	// A conditional branch into a discard tail ends the invocation: the emitter
	// lowers it as a terminating arm with its own copy of the kill body, so the
	// tail belongs to no loop region. Uses the emitter's own tail definition.
	bool IsDiscardArm(uint32_t target, bool* discard)
	{
		if (m_discard.empty())
		{
			m_discard.assign(m_cfg.blocks.size(), -1);
		}
		if (m_discard[target] < 0)
		{
			if (!Spend(m_code.GetInstructions().Size(), m_cfg.blocks[target].pc))
			{
				return false;
			}
			m_discard[target] = m_code.ReadBlock(m_cfg.blocks[target].pc).is_discard ? 1 : 0;
		}
		*discard = m_discard[target] == 1;
		return true;
	}

	bool CollectExits(const ShaderCfgLoop& loop, std::vector<LoopExit>* exits, std::vector<uint32_t>* targets)
	{
		const auto& blocks = m_cfg.blocks;
		for (uint32_t block: loop.blocks)
		{
			for (uint32_t k = 0; k < 2; k++)
			{
				const uint32_t target = blocks[block].succ[k];
				if (target == kShaderCfgNone || (k == 1 && target == blocks[block].succ[0]) || Contains(loop, target))
				{
					continue;
				}
				const bool conditional = k == 0 && IsConditional(m_code.GetInstructions().At(blocks[block].last).type);
				bool       discard     = false;
				if (conditional && !IsDiscardArm(target, &discard))
				{
					return false;
				}
				if (discard)
				{
					continue;
				}
				exits->push_back({blocks[block].last_pc, target, conditional});
				if (std::find(targets->begin(), targets->end(), target) != targets->end())
				{
					continue;
				}
				if (targets->size() == kMaxExitTargets)
				{
					return Reject(ShaderCfgReject::MultipleLoopExits, blocks[block].last_pc);
				}
				targets->push_back(target);
			}
		}
		return true;
	}

	[[nodiscard]] bool MergeLeavesOthersAlone(const std::vector<Reach>& reaches, const Reach& merge) const
	{
		for (const auto& other: reaches)
		{
			if (&other != &merge && !LeavesMergeAlone(other, &merge))
			{
				return false;
			}
		}
		return true;
	}

	// Conditional latches merge at their fallthrough. Otherwise take the first
	// target after the latch that every other exit leaves alone; when none
	// does, the first target after the latch, so the conflict is reported.
	[[nodiscard]] const Reach* ChooseMerge(const ShaderCfgLoop& loop, const std::vector<Reach>& reaches) const
	{
		const auto& blocks = m_cfg.blocks;
		if (loop.conditional_latch)
		{
			const uint32_t fallthrough = blocks[loop.latch].succ[1];
			for (const auto& reach: reaches)
			{
				if (reach.target == fallthrough)
				{
					return &reach;
				}
			}
			return nullptr;
		}
		const Reach* first = nullptr;
		for (const auto& reach: reaches)
		{
			if (blocks[reach.target].pc <= blocks[loop.latch].last_pc)
			{
				continue;
			}
			first = first == nullptr ? &reach : first;
			if (MergeLeavesOthersAlone(reaches, reach))
			{
				return &reach;
			}
		}
		return first;
	}

	// A merge outside the parent loop is only a region that ends inside the
	// parent's construct; otherwise the exits leave two loops at once.
	bool CheckMergeInParent(const ShaderCfgLoop& loop, const std::vector<LoopExit>& exits)
	{
		if (loop.parent == kShaderCfgNone || loop.merge == kShaderCfgNone || Contains(m_cfg.loops[loop.parent], loop.merge))
		{
			return true;
		}
		const auto& parent = m_cfg.loops[loop.parent];
		Reach       inner;
		Reach       outer;
		inner.target = loop.merge;
		outer.target = parent.merge;
		if (!ComputeReach(parent.header, &inner) || (parent.merge != kShaderCfgNone && !ComputeReach(parent.header, &outer)))
		{
			return false;
		}
		if (LeavesMergeAlone(inner, parent.merge == kShaderCfgNone ? nullptr : &outer))
		{
			return true;
		}
		return Reject(ShaderCfgReject::MultiLevelBreak, FirstExitTo(exits, loop.merge)->pc);
	}

	bool CheckLoopExits(uint32_t l)
	{
		auto&                 loop = m_cfg.loops[l];
		std::vector<LoopExit> exits;
		std::vector<uint32_t> targets;
		std::vector<Reach>    reaches;
		if (!CollectExits(loop, &exits, &targets) || !ComputeReaches(loop, targets, &reaches))
		{
			return false;
		}
		if (loop.conditional_latch && Contains(loop, m_cfg.blocks[loop.latch].succ[1]))
		{
			return Reject(ShaderCfgReject::MultipleLoopExits, LatchPc(loop.latch));
		}
		const Reach* merge = ChooseMerge(loop, reaches);
		for (const auto& other: reaches)
		{
			if (&other == merge || LeavesMergeAlone(other, merge))
			{
				continue;
			}
			const bool leaves_parent = loop.parent != kShaderCfgNone && !Contains(m_cfg.loops[loop.parent], other.target);
			return Reject(leaves_parent ? ShaderCfgReject::MultiLevelBreak : ShaderCfgReject::MultipleLoopExits,
			              FirstExitTo(exits, other.target)->pc);
		}
		loop.merge = merge == nullptr ? kShaderCfgNone : merge->target;
		for (const auto& exit: exits)
		{
			if (exit.target == loop.merge && exit.conditional)
			{
				loop.merge_branch_pc = exit.pc;
				break;
			}
		}
		return CheckMergeInParent(loop, exits);
	}

	bool CheckExits()
	{
		for (uint32_t l = 0; l < m_cfg.loops.size(); l++)
		{
			if (!CheckLoopExits(l))
			{
				return false;
			}
		}
		return true;
	}

	const ShaderCode&       m_code;
	ShaderControlFlowGraph& m_cfg;
	uint64_t                m_steps      = 0;
	uint64_t                m_membership = 0;
	std::vector<uint32_t>   m_pcs;
	std::vector<uint32_t>   m_targets;  // instruction index of each static target
	std::vector<uint32_t>   m_block_of; // block of each instruction
	std::vector<uint32_t>   m_postorder;
	std::vector<uint32_t>   m_post_number;
	std::vector<uint32_t>   m_mark;
	std::vector<int8_t>     m_discard; // per block: unknown, no, yes
	std::vector<BackEdge>   m_back_edges;
};

} // namespace

uint32_t ShaderControlFlowGraph::BlockAt(uint32_t pc) const
{
	const auto next = std::upper_bound(blocks.begin(), blocks.end(), pc,
	                                   [](uint32_t value, const ShaderCfgBlock& block) { return value < block.pc; });
	if (next == blocks.begin())
	{
		return kShaderCfgNone;
	}
	const auto block = static_cast<uint32_t>(next - blocks.begin() - 1);
	return next != blocks.end() || pc <= blocks[block].last_pc ? block : kShaderCfgNone;
}

uint32_t ShaderControlFlowGraph::InnermostLoopAt(uint32_t pc) const
{
	const uint32_t block = BlockAt(pc);
	return block == kShaderCfgNone ? kShaderCfgNone : blocks[block].loop;
}

bool ShaderControlFlowGraph::Dominates(uint32_t dominator, uint32_t block) const
{
	if (dominator >= blocks.size() || block >= blocks.size() || !blocks[dominator].reachable || !blocks[block].reachable)
	{
		return false;
	}
	return blocks[dominator].dom_in <= blocks[block].dom_in && blocks[block].dom_out <= blocks[dominator].dom_out;
}

ShaderControlFlowGraph ShaderBuildControlFlowGraph(const ShaderCode& code)
{
	ShaderControlFlowGraph cfg;
	Builder(code, &cfg).Run();
	return cfg;
}

const char* ShaderCfgRejectName(ShaderCfgReject reject)
{
	switch (reject)
	{
		case ShaderCfgReject::None: return "none";
		case ShaderCfgReject::TooLarge: return "too-large";
		case ShaderCfgReject::UnresolvedTarget: return "unresolved-target";
		case ShaderCfgReject::IndirectTransfer: return "indirect-transfer";
		case ShaderCfgReject::IrreducibleLoop: return "irreducible-loop";
		case ShaderCfgReject::ForwardBackEdge: return "forward-back-edge";
		case ShaderCfgReject::SharedHeader: return "shared-header";
		case ShaderCfgReject::MultiLevelContinue: return "multi-level-continue";
		case ShaderCfgReject::MultiLevelBreak: return "multi-level-break";
		case ShaderCfgReject::MultipleLoopExits: return "multiple-loop-exits";
	}
	return "unknown";
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
