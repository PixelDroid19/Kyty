#include "Emulator/Graphics/ShaderNggFrontBody.h"

#include "Emulator/Graphics/ShaderComputeWaveSdwa.h"

#include "ShaderLaneFlow.h"
#include "ShaderNativeWaveInternal.h"
#include "ShaderSpirvInternal.h"

#include <algorithm>
#include <bitset>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

using namespace LaneFlow;

using Type    = ShaderInstructionType;
using Operand = ShaderOperandType;

constexpr unsigned kVertexIdVgpr   = 5;
constexpr unsigned kInstanceIdVgpr = 8;
constexpr unsigned kPosition0Seen  = 1u << 0u;
constexpr unsigned kPosition1Seen  = 1u << 1u;
constexpr unsigned kPositionDone   = 1u << 2u;
constexpr unsigned kPositionHistoryCount = 8u;

// Symbolic EXEC of the body. Node 0 is every launched lane; a compare splits a node into the lanes where it holds
// (then) and the rest (else). A VGPR written under a node is defined for exactly the lanes of that node, and it is
// defined for a node once it is defined for an ancestor or for both halves of one split of it.
class ExecPartition
{
public:
	int Child(int parent, uint32_t compare, bool then_side)
	{
		for (size_t node = 1; node < m_nodes.size(); ++node)
		{
			const auto& candidate = m_nodes[node];
			if (candidate.parent == parent && candidate.compare == compare && candidate.then_side == then_side) { return static_cast<int>(node); }
		}
		m_nodes.push_back({parent, compare, then_side});
		return static_cast<int>(m_nodes.size() - 1u);
	}
	// The split of `node` that `child` belongs to and the other half of it, when `child` is one half.
	[[nodiscard]] bool Sibling(int child, int* parent, int* other) const
	{
		if (child <= 0) { return false; }
		const auto& node = m_nodes[static_cast<size_t>(child)];
		for (size_t candidate = 1; candidate < m_nodes.size(); ++candidate)
		{
			const auto& entry = m_nodes[candidate];
			if (entry.parent == node.parent && entry.compare == node.compare && entry.then_side != node.then_side)
			{
				*parent = node.parent;
				*other  = static_cast<int>(candidate);
				return true;
			}
		}
		*parent = node.parent;
		*other  = -1;
		return true;
	}
	[[nodiscard]] int  Parent(int node) const { return node <= 0 ? -1 : m_nodes[static_cast<size_t>(node)].parent; }
	[[nodiscard]] bool IsThen(int node) const { return node > 0 && m_nodes[static_cast<size_t>(node)].then_side; }
	[[nodiscard]] uint32_t Compare(int node) const { return node > 0 ? m_nodes[static_cast<size_t>(node)].compare : 0u; }
	[[nodiscard]] bool Descends(int node, int ancestor) const
	{
		for (int current = node; current >= 0; current = Parent(current))
		{
			if (current == ancestor) { return true; }
		}
		return false;
	}
	[[nodiscard]] bool Covers(const std::set<int>& nodes, int node) const
	{
		if (node < 0) { return false; }
		for (int current = node; current >= 0; current = Parent(current))
		{
			if (nodes.count(current) != 0u) { return true; }
		}
		for (size_t then_node = 1; then_node < m_nodes.size(); ++then_node)
		{
			const auto& entry = m_nodes[then_node];
			if (entry.parent != node || !entry.then_side) { continue; }
			for (size_t else_node = 1; else_node < m_nodes.size(); ++else_node)
			{
				const auto& other = m_nodes[else_node];
				if (other.parent == node && other.compare == entry.compare && !other.then_side &&
				    Covers(nodes, static_cast<int>(then_node)) && Covers(nodes, static_cast<int>(else_node)))
				{
					return true;
				}
			}
		}
		return false;
	}

private:
	struct Node
	{
		int      parent;
		uint32_t compare;
		bool     then_side;
	};
	std::vector<Node> m_nodes {{-1, 0u, true}};
};

struct CompareMask
{
	uint32_t compare = 0;     // pc of the compare
	int      under   = 0;     // EXEC node it was evaluated under (inactive lanes read 0)
	bool     uniform = false; // of wave-uniform values: every lane of `under` holds the same bit
	bool operator==(const CompareMask& other) const { return compare == other.compare && under == other.under && uniform == other.uniform; }
};

// Per path: the EXEC node (-1 when it is no node of the partition), the scalar pairs holding a node or a compare,
// and the nodes under which each VGPR is defined beyond FlowState::defined (every lane).
// A VGPR in `uniform_under` holds one clean value in every lane of its node (FlowState::uniform: of node 0). A scalar
// in `scope` (taint kDiverged) is a value read from such a VGPR under a partial EXEC: the host reads it from a lane of
// its own subgroup, so it is the guest value only for the lanes of the EXEC it was read under.
struct ExecShape
{
	int                                exec = 0;
	uint8_t                            position_histories = 1u; // bit 0: no position export has occurred
	std::map<unsigned, int>            saved;
	std::map<unsigned, CompareMask>    compare;
	std::map<unsigned, std::set<int>>  partial;
	std::map<unsigned, int>            uniform_under;
	std::map<unsigned, int>            scope;
	// A compare word moved through a scalar spill slot, and the half (0 low, 1 high) of the pair it is. A restore
	// of both halves into one pair names the compare again.
	std::map<std::pair<int, int>, std::pair<CompareMask, unsigned>> spill_compare;
	std::map<unsigned, std::pair<CompareMask, unsigned>>             restored_compare;
	// The same for a word of EXEC or of a saved EXEC: the node it holds and the half it is.
	std::map<std::pair<int, int>, std::pair<int, unsigned>> spill_saved;
	std::map<unsigned, std::pair<int, unsigned>>             restored_saved;
	bool operator==(const ExecShape& other) const
	{
		return exec == other.exec && position_histories == other.position_histories && saved == other.saved && compare == other.compare &&
		       partial == other.partial && uniform_under == other.uniform_under && scope == other.scope &&
		       spill_compare == other.spill_compare && restored_compare == other.restored_compare && spill_saved == other.spill_saved &&
		       restored_saved == other.restored_saved;
	}
};

constexpr uint32_t kNegativeInfinity = 0xff800000u;

bool PlainVgpr(const ShaderOperand& op)
{
	return op.type == Operand::Vgpr && op.size == 1 && op.register_id >= 0 && op.register_id < kVgprs && !op.dpp && !op.absolute &&
	       !op.negate && !op.clamp && op.multiplier == 1.0f;
}

bool ZeroConstant(const ShaderOperand& op)
{
	return op.type == Operand::IntegerInlineConstant && op.constant.u == 0u;
}

bool PlainVop(const ShaderInstruction& inst)
{
	return !inst.vop_sdwa && inst.vop3_op_sel == 0u && inst.vop3_omod == 0u;
}

bool WritesVgpr(const ShaderInstruction& inst, int vgpr)
{
	for (const ShaderOperand* dst: {&inst.dst, &inst.dst2})
	{
		unsigned first = 0;
		unsigned count = 0;
		if (VgprRange(*dst, &first, &count) && static_cast<unsigned>(vgpr) >= first && static_cast<unsigned>(vgpr) < first + count) { return true; }
	}
	return false;
}

// Whether an instruction writes one of the scalar words [first, first + count); V_CMPX writes only EXEC.
bool WritesScalar(const ShaderInstruction& inst, unsigned first, unsigned count)
{
	if (first == kExecLoWord && ShaderInstructionWritesExec(inst)) { return true; }
	if (WritesExecOnly(inst.type)) { return false; }
	for (const ShaderOperand* dst: {&inst.dst, &inst.dst2})
	{
		unsigned word  = 0;
		unsigned words = 0;
		if (ScalarRange(*dst, &word, &words) && word < first + count && first < word + words) { return true; }
	}
	return false;
}

bool PairOf(const ShaderOperand& op, unsigned* first)
{
	unsigned count = 0;
	return ScalarRange(op, first, &count) && count == 2u;
}

// A wave maximum feeding a monotone atomic (ShaderFindNggFrontMaxReductions): the indices of its parts.
struct MaxReduction
{
	ShaderLaneReduction lanes;       // from the seed to the EXEC restore, and the two reads
	uint32_t            compare = 0; // V_CMP of 0 < x: the lanes the any-test counts
	uint32_t            any     = 0; // V_CMP_GT_U64 of that mask and 0
	uint32_t            branch  = 0; // S_CBRANCH_VCCZ to the join unless a lane counts
	uint32_t            join    = 0; // its target
	uint32_t            combine = 0; // V_MAX_F32 of the reads
	uint32_t            atomic  = 0; // BUFFER_ATOMIC_UMAX of the combined maximum
	int                 source  = 0; // x
};

class MaxReductionMatcher
{
public:
	MaxReductionMatcher(const ShaderCode& code, uint32_t guest_wave_size): m_code(code), m_wave(guest_wave_size)
	{
		const auto& instructions = code.GetInstructions();
		std::map<uint32_t, uint32_t> index_of;
		for (uint32_t index = 0; index < instructions.Size(); ++index) { index_of[instructions.At(index).pc] = index; }
		for (uint32_t index = 0; index < instructions.Size(); ++index)
		{
			const auto& inst = instructions.At(index);
			if ((inst.type != Type::SBranch && !StartsWith(inst.type, "SCbranch")) || inst.src_num < 1) { continue; }
			const int64_t target = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
			const auto    found  = target >= 0 && target <= UINT32_MAX ? index_of.find(static_cast<uint32_t>(target)) : index_of.end();
			m_edges.emplace_back(index, found == index_of.end() ? UINT32_MAX : found->second);
		}
	}

	bool Match(uint32_t entry, MaxReduction* unit) const
	{
		return Region(entry, unit) && Reads(unit) && Store(unit) && AnyTest(unit) && Closed(*unit);
	}

private:
	const ShaderInstruction& At(uint32_t index) const { return m_code.GetInstructions().At(index); }

	// The last writer of the scalar words [first, first + count) in [low, from], or UINT32_MAX.
	uint32_t LastWriter(uint32_t from, uint32_t low, unsigned first, unsigned count) const
	{
		for (uint32_t index = from + 1u; index-- > low;)
		{
			if (WritesScalar(At(index), first, count)) { return index; }
		}
		return UINT32_MAX;
	}

	// S_CBRANCH_VCCZ join; S_ORN2_SAVEEXEC_B64 e, EXEC; V_CNDMASK_B32 v, -inf, x, e; steps; S_MOV_B64 EXEC, e.
	bool Region(uint32_t entry, MaxReduction* unit) const
	{
		const auto& instructions = m_code.GetInstructions();
		const auto& enter        = At(entry);
		unsigned    saved        = 0;
		if (entry == 0u || entry + 2u >= instructions.Size() || enter.type != Type::SOrn2SaveexecB64 || enter.src_num != 1 ||
		    enter.src[0].type != Operand::ExecLo || !PairOf(enter.dst, &saved))
		{
			return false;
		}
		unit->branch = entry - 1u;
		const auto edge = std::find_if(m_edges.begin(), m_edges.end(), [&](const auto& candidate) { return candidate.first == unit->branch; });
		if (At(unit->branch).type != Type::SCbranchVccz || edge == m_edges.end() || edge->second == UINT32_MAX || edge->second <= entry) { return false; }
		unit->join     = edge->second;
		const auto& seed = At(entry + 1u);
		unsigned    mask = 0;
		if (seed.type != Type::VCndmaskB32 || seed.src_num != 3 || !PlainVop(seed) || !PlainVgpr(seed.dst) ||
		    seed.src[0].type != Operand::LiteralConstant || seed.src[0].constant.u != kNegativeInfinity || !PlainVgpr(seed.src[1]) ||
		    !PairOf(seed.src[2], &mask) || mask != saved)
		{
			return false;
		}
		unit->source = seed.src[1].register_id;
		for (uint32_t index = entry + 2u; index < unit->join; ++index)
		{
			const auto& inst = At(index);
			if (!ShaderInstructionWritesExec(inst)) { continue; }
			unsigned restored = 0;
			if (inst.type != Type::SMovB64 || inst.src_num != 1 || !PairOf(inst.src[0], &restored) || restored != saved) { return false; }
			unit->lanes.op = ShaderLaneReductionOp::MaxF32;
			return ShaderProveLaneReductionNetwork(m_code, m_wave, entry + 1u, index, &unit->lanes);
		}
		return false;
	}

	// Two reads that together cover every lane, folded by one V_MAX_F32 before either is rewritten.
	bool Reads(MaxReduction* unit) const
	{
		const auto& reads = unit->lanes.reads;
		const uint64_t every = m_wave == 64u ? ~uint64_t {0} : (uint64_t {1} << m_wave) - 1u;
		if (reads.size() != 2u || (reads[0].lanes | reads[1].lanes) != every) { return false; }
		unsigned words[2] = {};
		for (size_t read = 0; read < 2u; ++read)
		{
			const auto& dst   = At(reads[read].index).dst;
			unsigned    count = 0;
			if (dst.type != Operand::Sgpr || !ScalarRange(dst, &words[read], &count) || count != 1u) { return false; }
		}
		if (words[0] == words[1]) { return false; }
		for (uint32_t index = reads[0].index + 1u; index < unit->join; ++index)
		{
			if (index == reads[1].index) { continue; }
			const auto& inst   = At(index);
			unsigned    first  = 0;
			unsigned    second = 0;
			unsigned    count  = 0;
			// Two scalar sources need the SDWA encoding; only its whole-dword identity form is a plain maximum.
			const bool  plain  = PlainVop(inst) || ShaderComputeWaveSdwaVop2IdentitySupported(inst);
			const bool  folds  = index > reads[1].index && inst.type == Type::VMaxF32 && inst.src_num == 2 && plain && PlainVgpr(inst.dst) &&
			                   ScalarRange(inst.src[0], &first, &count) && ScalarRange(inst.src[1], &second, &count) &&
			                   ((first == words[0] && second == words[1]) || (first == words[1] && second == words[0]));
			if (folds)
			{
				unit->combine = index;
				return true;
			}
			if (WritesScalar(inst, words[0], 1u) || WritesScalar(inst, words[1], 1u) || ShaderInstructionWritesExec(inst)) { return false; }
		}
		return false;
	}

	// After the fold EXEC is only narrowed to the lanes holding the maximum and restored, and the one atomic stores
	// the maximum, unreturned, at an index the walk proves wave-uniform.
	bool Store(MaxReduction* unit) const
	{
		const int maximum = At(unit->combine).dst.register_id;
		unit->atomic      = UINT32_MAX;
		for (uint32_t index = unit->combine + 1u; index < unit->join; ++index)
		{
			const auto& inst = At(index);
			// VDATA names the stored value: an atomic without return writes no VGPR.
			if (inst.type == Type::BufferAtomicUmax)
			{
				constexpr uint8_t kLdsTfeReserved = 0x85u;
				if (unit->atomic != UINT32_MAX || inst.dst.type != Operand::Vgpr || inst.dst.register_id != maximum || inst.buffer_return_old_value ||
				    !inst.buffer_idxen || inst.buffer_offen || (inst.buffer_flags & kLdsTfeReserved) != 0u || inst.src_num < 3 ||
				    inst.src[0].type != Operand::Vgpr)
				{
					return false;
				}
				unit->atomic = index;
				continue;
			}
			if (WritesVgpr(inst, maximum) || WritesVgpr(inst, unit->source)) { return false; }
			if (!ShaderInstructionWritesExec(inst)) { continue; }
			const bool narrows = inst.type == Type::VCmpxEqF32 && inst.src_num == 2 && PlainVop(inst) && PlainVgpr(inst.src[0]) &&
			                     PlainVgpr(inst.src[1]) &&
			                     ((inst.src[0].register_id == maximum && inst.src[1].register_id == unit->source) ||
			                      (inst.src[1].register_id == maximum && inst.src[0].register_id == unit->source));
			unsigned restored = 0;
			unsigned saved    = 0;
			const bool restores = unit->atomic != UINT32_MAX && inst.type == Type::SMovB64 && inst.src_num == 1 && PairOf(inst.src[0], &restored) &&
			                      PairOf(At(unit->branch + 1u).dst, &saved) && restored == saved;
			if (!narrows && !restores) { return false; }
		}
		return unit->atomic != UINT32_MAX;
	}

	// VCC at the branch is a & EXEC or a & (u ? EXEC : 0), a = V_CMP_GT_U64 of a compare of 0 < x and 0, all in the
	// straight line before the branch with EXEC unchanged since the compare.
	bool AnyTest(MaxReduction* unit) const
	{
		const uint32_t both = LastWriter(unit->branch - 1u, 0u, kVccLoWord, 2u);
		if (both == UINT32_MAX) { return false; }
		const auto& conjunction = At(both);
		unsigned    vcc         = 0;
		if (conjunction.type != Type::SAndB64 || conjunction.src_num != 2 || !PairOf(conjunction.dst, &vcc) || vcc != kVccLoWord) { return false; }
		for (int side = 0; side < 2; ++side)
		{
			unsigned       any_pair = 0;
			const auto&    other    = conjunction.src[1 - side];
			const uint32_t any      = PairOf(conjunction.src[side], &any_pair) ? LastWriter(both - 1u, 0u, any_pair, 2u) : UINT32_MAX;
			if (any == UINT32_MAX || !Counts(any, unit) || !EnabledLanes(other, both, unit->compare + 1u)) { continue; }
			return true;
		}
		return false;
	}

	// EXEC, or a select of EXEC and 0 written from `low` on (the walk proves its SCC wave-uniform).
	bool EnabledLanes(const ShaderOperand& op, uint32_t before, uint32_t low) const
	{
		unsigned pair = 0;
		if (op.type == Operand::ExecLo && op.size == 2) { return true; }
		if (!PairOf(op, &pair) || before <= low) { return false; }
		const uint32_t writer = LastWriter(before - 1u, low, pair, 2u);
		if (writer == UINT32_MAX) { return false; }
		const auto& select = At(writer);
		return select.type == Type::SCselectB64 && select.src_num == 2 && select.src[0].type == Operand::ExecLo && ZeroConstant(select.src[1]);
	}

	bool Counts(uint32_t any, MaxReduction* unit) const
	{
		const auto& test = At(any);
		unsigned    vcc  = 0;
		if (test.type != Type::VCmpGtU64 || test.src_num != 2 || !PairOf(test.src[0], &vcc) || vcc != kVccLoWord || !ZeroConstant(test.src[1]) || any == 0u)
		{
			return false;
		}
		const uint32_t compare = LastWriter(any - 1u, 0u, kVccLoWord, 2u);
		if (compare == UINT32_MAX) { return false; }
		const auto& cmp      = At(compare);
		const bool  positive = cmp.src_num == 2 && PlainVop(cmp) &&
		                      ((cmp.type == Type::VCmpLtF32 && ZeroConstant(cmp.src[0]) && PlainVgpr(cmp.src[1]) && cmp.src[1].register_id == unit->source) ||
		                       (cmp.type == Type::VCmpGtF32 && PlainVgpr(cmp.src[0]) && cmp.src[0].register_id == unit->source && ZeroConstant(cmp.src[1])));
		if (!positive) { return false; }
		for (uint32_t index = compare + 1u; index <= unit->atomic; ++index)
		{
			if (WritesVgpr(At(index), unit->source)) { return false; }
			if (index <= unit->branch && ShaderInstructionWritesExec(At(index))) { return false; }
		}
		unit->compare = compare;
		unit->any     = any;
		return true;
	}

	// Straight-line code from the compare to the branch, and a region that only its own branches enter or jump
	// within.
	bool Closed(const MaxReduction& unit) const
	{
		for (const auto& [from, to]: m_edges)
		{
			const bool inside = from > unit.branch && from < unit.join;
			if (inside && (to <= from || to > unit.join)) { return false; }
			if (!inside && from != unit.branch && to > unit.compare && to < unit.join) { return false; }
			if (from >= unit.compare && from < unit.branch) { return false; }
		}
		return true;
	}

	const ShaderCode&                          m_code;
	uint32_t                                   m_wave;
	std::vector<std::pair<uint32_t, uint32_t>> m_edges; // (branch index, target index or UINT32_MAX)
};

std::vector<MaxReduction> FindMaxReductions(const ShaderCode& code, uint32_t guest_wave_size)
{
	std::vector<MaxReduction> found;
	const MaxReductionMatcher matcher(code, guest_wave_size);
	for (uint32_t entry = 0; entry < code.GetInstructions().Size(); ++entry)
	{
		MaxReduction unit;
		if (matcher.Match(entry, &unit)) { found.push_back(std::move(unit)); }
	}
	return found;
}

class Body
{
public:
	ShaderNggFrontBodyProof Run(const ShaderCode& code, uint32_t first_index, uint32_t guest_wave_size, const ShaderNggScalarDependencies& wave_scalars);

private:
	bool Fail(const ShaderInstruction& inst, const char* reason)
	{
		m_result.refused_pc = inst.pc;
		m_result.reason     = reason;
		return false;
	}
	uint8_t Taint(const ShaderOperand& op) const;
	void    Define(const ShaderOperand& op, uint8_t taint);
	void    DefineUniformMask(const ShaderOperand& op, bool uniform);
	[[nodiscard]] bool IsUniformMask(const ShaderOperand& op) const;
	bool    SourcesAreUniformClean(const ShaderInstruction& inst) const;
	[[nodiscard]] bool VgprUniformHere(unsigned vgpr) const;
	[[nodiscard]] bool InScope(const ShaderOperand& op) const;
	[[nodiscard]] int  SourceScope(const ShaderInstruction& inst) const;
	void    SetScope(const ShaderOperand& op, int node);

	bool Step(const ShaderInstruction& inst);
	bool Branch(const ShaderInstruction& inst);
	bool ScalarAlu(const ShaderInstruction& inst);
	bool ScalarLoad(const ShaderInstruction& inst);
	bool Vector(const ShaderInstruction& inst);
	bool ScalarSpill(const ShaderInstruction& inst);
	bool UniformReadFirstLane(const ShaderInstruction& inst);
	bool VectorSources(const ShaderInstruction& inst);
	bool VectorDestinations(const ShaderInstruction& inst, bool uniform_result);
	bool Export(const ShaderInstruction& inst);
	bool PositionExport(const ShaderInstruction& inst, bool position0, bool done);
	bool EndProgram(const ShaderInstruction& inst);
	bool ImageRead(const ShaderInstruction& inst);
	bool ExecWrite(const ShaderInstruction& inst);
	void OpenRegion(const ShaderInstruction& inst, uint32_t target);
	void CloseRegions(uint32_t pc);
	[[nodiscard]] bool VgprDefined(unsigned vgpr) const;
	void DefineVgpr(unsigned vgpr);
	void ForgetScalarShape(const ShaderOperand& op);
	[[nodiscard]] bool CompareHalf(const ShaderOperand& op, std::pair<CompareMask, unsigned>* half) const;
	void RestoreCompareWord(const ShaderOperand& op, const std::pair<CompareMask, unsigned>& half);
	[[nodiscard]] bool SavedHalf(const ShaderOperand& op, std::pair<int, unsigned>* half) const;
	void RestoreSavedWord(const ShaderOperand& op, const std::pair<int, unsigned>& half);
	[[nodiscard]] bool ExecRestrictedCompare(const ShaderInstruction& inst, CompareMask* mask) const;
	void MergeShape(ExecShape* into, const ExecShape& other, uint32_t pc) const;
	int  ExecAfter(const ShaderInstruction& inst) const;
	bool FindLoops();
	bool EnterLoop(uint32_t header);
	bool CloseLoop(uint32_t header, bool* again);
	void RestartLoop(uint32_t header);
	void ExitLoops(uint32_t join);
	[[nodiscard]] const MaxReduction* ReductionAt(uint32_t index) const;
	bool MaxReductionPart(const ShaderInstruction& inst, const MaxReduction& unit, bool* done);
	bool MaxReductionAtomic(const ShaderInstruction& inst);

	ShaderNggFrontBodyProof m_result;
	FlowState               m_state;
	std::map<uint32_t, FlowState> m_pending; // forward branch targets by pc
	// Lane-divergent regions by join pc: the scalars and VGPRs some path through the region may write.
	struct Region
	{
		std::bitset<kWords> scalars;
		std::bitset<kVgprs> vgprs;
		std::vector<int>    roots; // EXEC node at each branch into this join
		bool                undefines = false; // its VGPR writes are dead after the join (a wave maximum)
	};
	std::map<uint32_t, Region> m_regions;
	// A loop under analysis, by header index: the state assumed at its header for every iteration, and the
	// analyzer state on entry that each further pass over the body starts from.
	struct Loop
	{
		uint32_t                      back  = 0;  // index of the back edge
		int                           lanes = -1; // EXEC node of the lanes still looping
		bool                          entry_lanes = false; // `lanes` is the entry EXEC: no back edge has narrowed it
		unsigned                      passes = 0;
		FlowState                     state;
		ExecShape                     shape;
		std::map<uint32_t, FlowState> pending;
		std::map<uint32_t, ExecShape> pending_shape;
		std::map<uint32_t, Region>    regions;
		std::set<uint32_t>            uniform_targets;
		// Spill slot taints when the pass began: a write late in the body reaches reads early in the next iteration.
		std::map<std::pair<int, int>, uint8_t> spills;
	};
	std::map<uint32_t, uint32_t> m_back_edges; // header index -> back-edge index
	std::map<uint32_t, uint32_t> m_loop_ends;  // back-edge index -> header index
	std::map<uint32_t, Loop>     m_loops;
	std::map<uint32_t, ExecShape> m_pending_shape;
	std::set<uint32_t>          m_uniform_targets; // joins also reached by an edge every lane takes
	ExecPartition               m_partition;
	ExecShape                   m_shape;
	// Taint of the scalars every static spill write put in a (VGPR, lane) slot.
	std::map<std::pair<int, int>, uint8_t> m_spills;
	std::vector<MaxReduction>              m_reductions;
	const ShaderCode*       m_code  = nullptr;
	uint32_t                m_index = 0;
};

uint8_t Body::Taint(const ShaderOperand& op) const
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &first, &count)) { return kClean; }
	uint8_t taint = kClean;
	for (unsigned word = 0; word < count; ++word) { taint |= m_state.scalar[first + word]; }
	return taint;
}

void Body::Define(const ShaderOperand& op, uint8_t taint)
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &first, &count)) { return; }
	for (unsigned word = 0; word < count; ++word)
	{
		m_state.scalar[first + word] = taint;
		m_state.uniform_mask.reset(first + word);
		m_shape.scope.erase(first + word);
	}
	ForgetScalarShape(op);
	if (op.type == Operand::VccLo || op.type == Operand::VccHi) { m_state.vcc_uniform = false; }
}

// Records that a scalar register range now holds a compare result identical in every lane; VCC is
// wave-uniform when both of its words do.
void Body::DefineUniformMask(const ShaderOperand& op, bool uniform)
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &first, &count)) { return; }
	for (unsigned word = 0; word < count; ++word) { m_state.uniform_mask.set(first + word, uniform); }
	if (op.type == Operand::VccLo || op.type == Operand::VccHi)
	{
		ShaderOperand vcc {};
		vcc.type = Operand::VccLo;
		vcc.size = 2;
		m_state.vcc_uniform = IsUniformMask(vcc);
	}
}

bool Body::IsUniformMask(const ShaderOperand& op) const
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &first, &count)) { return false; }
	for (unsigned word = 0; word < count; ++word)
	{
		if (!m_state.uniform_mask.test(first + word)) { return false; }
	}
	return true;
}

// A VGPR holding one clean value in every lane of the current EXEC.
bool Body::VgprUniformHere(unsigned vgpr) const
{
	if (m_state.uniform[vgpr]) { return true; }
	const auto under = m_shape.uniform_under.find(vgpr);
	return under != m_shape.uniform_under.end() && m_shape.exec >= 0 && m_partition.Descends(m_shape.exec, under->second);
}

// Sources holding one number in every lane of the current EXEC: constants, launch-free scalars, scoped scalars
// read under an ancestor of it, VGPRs uniform under an ancestor of it, lane bits of a uniform mask.
bool Body::SourcesAreUniformClean(const ShaderInstruction& inst) const
{
	for (int source = 0; source < inst.src_num; ++source)
	{
		const auto& op = inst.src[source];
		if (IsConstant(op)) { continue; }
		unsigned first = 0;
		unsigned count = 0;
		if (VgprRange(op, &first, &count))
		{
			for (unsigned word = 0; word < count; ++word)
			{
				if (!VgprUniformHere(first + word)) { return false; }
			}
			continue;
		}
		// Every launched lane holds the same bit of a uniform mask.
		if (IsLaneBit(inst, source) && IsUniformMask(op)) { continue; }
		if (!ScalarRange(op, &first, &count) || (Taint(op) != kClean && !InScope(op))) { return false; }
	}
	if (inst.type == Type::VMacF32)
	{
		unsigned first = 0;
		unsigned count = 0;
		if (!VgprRange(inst.dst, &first, &count)) { return false; }
		for (unsigned word = 0; word < count; ++word)
		{
			if (!VgprUniformHere(first + word)) { return false; }
		}
	}
	return true;
}

// A scoped scalar the current EXEC may read: every word holds a value read under an ancestor of EXEC.
bool Body::InScope(const ShaderOperand& op) const
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &first, &count) || Taint(op) != kDiverged || m_shape.exec < 0) { return false; }
	for (unsigned word = first; word < first + count; ++word)
	{
		const auto scope = m_shape.scope.find(word);
		if (m_state.scalar[word] == kClean) { continue; }
		if (scope == m_shape.scope.end() || !m_partition.Descends(m_shape.exec, scope->second)) { return false; }
	}
	return true;
}

// The scope of a scalar computed from these sources: the deepest scope among them when they lie on one chain of
// nodes (the value is then known for the lanes of all of them), or -1.
int Body::SourceScope(const ShaderInstruction& inst) const
{
	int deepest = 0;
	for (int source = 0; source < inst.src_num; ++source)
	{
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(inst.src[source], &first, &count)) { continue; }
		for (unsigned word = first; word < first + count; ++word)
		{
			if (m_state.scalar[word] == kClean) { continue; }
			const auto scope = m_shape.scope.find(word);
			if (m_state.scalar[word] != kDiverged || scope == m_shape.scope.end()) { return -1; }
			if (m_partition.Descends(scope->second, deepest)) { deepest = scope->second; }
			if (!m_partition.Descends(deepest, scope->second)) { return -1; }
		}
	}
	return deepest;
}

void Body::SetScope(const ShaderOperand& op, int node)
{
	unsigned first = 0;
	unsigned count = 0;
	if (node < 0 || !ScalarRange(op, &first, &count)) { return; }
	for (unsigned word = first; word < first + count; ++word) { m_shape.scope[word] = node; }
}

bool Body::Branch(const ShaderInstruction& inst)
{
	// EXEC node 0 is the entry vertex-count mask, never empty: a launch has a vertex and a host subgroup a live
	// invocation. An EXECZ branch under it is never taken, so its edge adds no path.
	if (inst.type == Type::SCbranchExecz && m_shape.exec == 0) { return true; }
	// A branch on EXEC is decided per host subgroup but per guest wave. It skips a region only for lanes that are all
	// inactive, so the region's vector writes are masked either way; its scalar writes are not (see OpenRegion).
	// A VCC branch leaves EXEC alone: a non-uniform one would run vector writes for lanes chosen by other lanes.
	const bool divergent = inst.type == Type::SCbranchExecz || inst.type == Type::SCbranchExecnz;
	// A compare of wave-uniform values under a partial EXEC holds the same bit in every lane of its EXEC, so a VCC
	// branch on it is uniform for the lanes still inside that EXEC and splits only lanes outside it, as an EXEC
	// branch does. Unlike an EXEC branch, active lanes take both edges, so the join keeps only what both define.
	bool exec_split = false;
	// Host subgroups take a wave maximum's branch apart; its region is admitted as a whole (MaxReduction).
	const auto* unit      = ReductionAt(m_index);
	const bool  reduction = unit != nullptr && unit->branch == m_index;
	if (inst.type == Type::SCbranchVccz || inst.type == Type::SCbranchVccnz)
	{
		ShaderOperand vcc {};
		vcc.type = Operand::VccLo;
		vcc.size = 2;
		const auto compare = m_shape.compare.find(kVccLoWord);
		exec_split = !reduction && !IsUniformMask(vcc) && compare != m_shape.compare.end() && compare->second.uniform && m_shape.exec >= 0 &&
		             m_partition.Descends(m_shape.exec, compare->second.under);
		if (!IsUniformMask(vcc) && !exec_split && !reduction) { return Fail(inst, "VCC branch on a comparison that is not wave-uniform"); }
	} else if (inst.type == Type::SCbranchScc0 || inst.type == Type::SCbranchScc1)
	{
		if (m_state.scalar[kSccWord] != kClean) { return Fail(inst, "SCC branch on launch-dependent data"); }
	} else if (inst.type != Type::SBranch && !divergent)
	{
		return Fail(inst, "branch form is not supported");
	}
	if (inst.src_num < 1) { return Fail(inst, "branch without a target"); }
	const int64_t target = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
	if (target <= static_cast<int64_t>(inst.pc) || target > UINT32_MAX) { return Fail(inst, "only forward branches are supported"); }
	bool exists = false;
	for (const auto& candidate: m_code->GetInstructions()) { exists = exists || candidate.pc == static_cast<uint32_t>(target); }
	if (!exists) { return Fail(inst, "branch target is not an instruction"); }
	const auto join = static_cast<uint32_t>(target);
	if (divergent || exec_split || reduction) { OpenRegion(inst, join); }
	if (reduction) { m_regions[join].undefines = true; }
	ExitLoops(join);
	if (!divergent) { m_uniform_targets.insert(join); }
	const auto found = m_pending.find(join);
	if (found == m_pending.end())
	{
		m_pending.emplace(join, m_state);
		m_pending_shape[join] = m_shape;
	} else
	{
		found->second.Merge(m_state);
		MergeShape(&m_pending_shape[join], m_shape, join);
	}
	if (inst.type == Type::SBranch) { m_state.reachable = false; }
	return true;
}

// Records what any instruction between the branch and its target may write. The guest runs the region once any
// lane of the wave is active, the host once any lane of the subgroup is: after the join such a scalar can hold
// either value, and a VGPR it wrote is no longer the same in every lane. Vector writes are EXEC-masked, so each
// lane still holds its own guest value.
static void AddWrites(const ShaderInstruction& candidate, std::bitset<kWords>* scalars, std::bitset<kVgprs>* vgprs)
{
	if (WritesExecOnly(candidate.type)) { return; }
	for (const ShaderOperand* dst: {&candidate.dst, &candidate.dst2})
	{
		unsigned first = 0;
		unsigned count = 0;
		if (ScalarRange(*dst, &first, &count))
		{
			for (unsigned word = 0; word < count; ++word) { scalars->set(first + word); }
		} else if (VgprRange(*dst, &first, &count))
		{
			for (unsigned word = 0; word < count && first + word < static_cast<unsigned>(kVgprs); ++word) { vgprs->set(first + word); }
		}
	}
	if (IsScalarAlu(candidate.type) && WritesScc(candidate.type)) { scalars->set(kSccWord); }
	// EXEC is a mask either way; its per-lane bit is tracked exactly.
	scalars->reset(kExecLoWord);
	scalars->reset(kExecLoWord + 1u);
}

void Body::OpenRegion(const ShaderInstruction& inst, uint32_t target)
{
	auto& region = m_regions[target];
	region.roots.push_back(m_shape.exec);
	for (const auto& candidate: m_code->GetInstructions())
	{
		if (candidate.pc > inst.pc && candidate.pc < target) { AddWrites(candidate, &region.scalars, &region.vgprs); }
	}
}

// Back edges are unconditional branches closing properly nested loops entered only through their header.
bool Body::FindLoops()
{
	const auto& instructions = m_code->GetInstructions();
	std::map<uint32_t, uint32_t> index_of;
	for (uint32_t index = 0; index < instructions.Size(); ++index) { index_of[instructions.At(index).pc] = index; }
	std::vector<std::pair<uint32_t, uint32_t>> branches; // (index, target index)
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (inst.type != Type::SBranch && !StartsWith(inst.type, "SCbranch")) { continue; }
		if (inst.src_num < 1) { return Fail(inst, "branch without a target"); }
		const int64_t target = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
		const auto    found  = target >= 0 && target <= UINT32_MAX ? index_of.find(static_cast<uint32_t>(target)) : index_of.end();
		if (found == index_of.end()) { return Fail(inst, "branch target is not an instruction"); }
		branches.emplace_back(index, found->second);
		if (target > static_cast<int64_t>(inst.pc)) { continue; }
		if (inst.type != Type::SBranch) { return Fail(inst, "only unconditional back edges are supported"); }
		if (!m_back_edges.emplace(found->second, index).second) { return Fail(inst, "loop with several back edges"); }
		m_loop_ends[index] = found->second;
	}
	for (const auto& [header, back]: m_back_edges)
	{
		for (const auto& [other_header, other_back]: m_back_edges)
		{
			const bool disjoint = other_back < header || other_header > back;
			const bool nested   = (other_header >= header && other_back <= back) || (other_header <= header && other_back >= back);
			if (!disjoint && !nested) { return Fail(instructions.At(back), "loops overlap"); }
		}
		for (const auto& [from, to]: branches)
		{
			if (to > header && to <= back && (from < header || from > back)) { return Fail(instructions.At(from), "branch enters a loop below its header"); }
		}
	}
	return true;
}

// On entry the loop's lanes are EXEC; inside, a node below it stands for the lanes still looping, so what holds for
// the entry lanes holds for them and every split inside the body divides them. A loop that never writes EXEC runs
// every iteration and leaves under the EXEC it entered with, and so does one whose back edge always restores the EXEC
// of its header: no lane leaves it before the others. The proof assumes the latter first (see CloseLoop).
bool Body::EnterLoop(uint32_t header)
{
	const auto& instructions = m_code->GetInstructions();
	const auto& inst         = instructions.At(header);
	if (m_shape.exec < 0) { return Fail(inst, "loop entered under an EXEC the proof does not track"); }
	Loop loop;
	loop.back = m_back_edges.at(header);
	bool writes_exec = false;
	for (uint32_t index = header; index <= loop.back; ++index) { writes_exec = writes_exec || ShaderInstructionWritesExec(instructions.At(index)); }
	loop.lanes           = m_shape.exec;
	loop.entry_lanes     = writes_exec;
	loop.state           = m_state;
	loop.shape           = m_shape;
	loop.pending         = m_pending;
	loop.pending_shape   = m_pending_shape;
	loop.regions         = m_regions;
	loop.uniform_targets = m_uniform_targets;
	loop.spills          = m_spills;
	m_loops[header]      = std::move(loop);
	return true;
}

// Joins the back edge into the state assumed at the header. The next iteration runs the lanes EXEC holds at the back
// edge, which must lie below the looping lanes; a VGPR defined for them is defined for the looping lanes of that
// iteration. When the join adds nothing, the pass over the body held for every iteration; otherwise the body is
// analysed again from the joined state.
bool Body::CloseLoop(uint32_t header, bool* again)
{
	auto&       loop = m_loops.at(header);
	const auto& inst = m_code->GetInstructions().At(loop.back);
	if (loop.entry_lanes && m_shape.exec >= 0 && m_shape.exec != loop.lanes && m_partition.Descends(m_shape.exec, loop.lanes))
	{
		// A back edge under fewer lanes: some lanes leave before others, so the body is analysed again under a node for
		// the lanes still looping.
		const auto& header_inst = m_code->GetInstructions().At(header);
		loop.entry_lanes = false;
		loop.lanes       = m_partition.Child(loop.lanes, header_inst.pc | 1u, true); // odd: no compare has this pc
		loop.shape.exec  = loop.lanes;
		RestartLoop(header);
		*again = true;
		return true;
	}
	if (m_shape.exec < 0 || !m_partition.Descends(m_shape.exec, loop.lanes))
	{
		return Fail(inst, "loop iterates under an EXEC outside the lanes that entered it");
	}
	auto       state   = loop.state;
	const bool changed = state.Merge(m_state);
	ExecShape  shape   = loop.shape;
	shape.position_histories |= m_shape.position_histories;
	const auto keep_common = [](auto* mine, const auto& theirs) {
		for (auto it = mine->begin(); it != mine->end();)
		{
			const auto found = theirs.find(it->first);
			it = (found != theirs.end() && found->second == it->second) ? std::next(it) : mine->erase(it);
		}
	};
	keep_common(&shape.saved, m_shape.saved);
	keep_common(&shape.compare, m_shape.compare);
	keep_common(&shape.uniform_under, m_shape.uniform_under);
	keep_common(&shape.scope, m_shape.scope);
	keep_common(&shape.spill_compare, m_shape.spill_compare);
	keep_common(&shape.restored_compare, m_shape.restored_compare);
	keep_common(&shape.spill_saved, m_shape.spill_saved);
	keep_common(&shape.restored_saved, m_shape.restored_saved);
	std::map<unsigned, std::set<int>> partial;
	for (unsigned vgpr = 0; vgpr < static_cast<unsigned>(kVgprs); ++vgpr)
	{
		const auto mine   = loop.shape.partial.find(vgpr);
		const auto theirs = m_shape.partial.find(vgpr);
		if (mine != loop.shape.partial.end() && theirs != m_shape.partial.end())
		{
			for (int node: mine->second)
			{
				if (theirs->second.count(node) != 0u) { partial[vgpr].insert(node); }
			}
		}
		const bool at_header = loop.state.defined.test(vgpr) || (mine != loop.shape.partial.end() && m_partition.Covers(mine->second, loop.lanes));
		const bool at_back   = m_state.defined.test(vgpr) || (theirs != m_shape.partial.end() && m_partition.Covers(theirs->second, m_shape.exec));
		if (at_header && at_back && !state.defined.test(vgpr)) { partial[vgpr].insert(loop.lanes); }
	}
	shape.partial = std::move(partial);
	*again        = changed || !(shape == loop.shape) || m_spills != loop.spills;
	if (!*again)
	{
		m_loops.erase(header);
		return true;
	}
	constexpr unsigned kPassesMax = 64;
	if (++loop.passes > kPassesMax) { return Fail(inst, "loop proof does not converge"); }
	loop.state = state;
	loop.shape = shape;
	RestartLoop(header);
	return true;
}

// The walk resumes at the header from the state the loop now assumes there; loops nested in it are entered anew.
void Body::RestartLoop(uint32_t header)
{
	auto& loop = m_loops.at(header);
	for (auto it = m_loops.begin(); it != m_loops.end();)
	{
		it = it->first > header && it->first <= loop.back ? m_loops.erase(it) : std::next(it);
	}
	loop.spills       = m_spills;
	m_state           = loop.state;
	m_shape           = loop.shape;
	m_pending         = loop.pending;
	m_pending_shape   = loop.pending_shape;
	m_regions         = loop.regions;
	m_uniform_targets = loop.uniform_targets;
}

// A branch out of loops: the guest wave leaves after the iterations of its slowest lane, the host subgroup after
// those of its own, so anything the loops write can differ at the target, as across a divergent region.
void Body::ExitLoops(uint32_t join)
{
	const auto& instructions = m_code->GetInstructions();
	for (const auto& [header, loop]: m_loops)
	{
		if (join <= instructions.At(loop.back).pc) { continue; }
		auto& region = m_regions[join];
		for (uint32_t index = header; index <= loop.back; ++index) { AddWrites(instructions.At(index), &region.scalars, &region.vgprs); }
	}
}

void Body::CloseRegions(uint32_t pc)
{
	const auto region = m_regions.find(pc);
	if (region == m_regions.end()) { return; }
	for (unsigned word = 0; word < kWords; ++word)
	{
		if (!region->second.scalars.test(word)) { continue; }
		m_state.scalar[word] = static_cast<uint8_t>(m_state.scalar[word] | kInfo);
		m_state.uniform_mask.reset(word);
		m_shape.saved.erase(word);
		m_shape.compare.erase(word);
		m_shape.restored_compare.erase(word);
		m_shape.restored_saved.erase(word);
		m_shape.scope.erase(word);
	}
	for (unsigned vgpr = 0; vgpr < static_cast<unsigned>(kVgprs); ++vgpr)
	{
		if (!region->second.vgprs.test(vgpr)) { continue; }
		m_shape.uniform_under.erase(vgpr);
		if (!region->second.undefines) { continue; }
		m_state.defined.reset(vgpr);
		m_shape.partial.erase(vgpr);
	}
	if (region->second.scalars.test(kVccLoWord) || region->second.scalars.test(kVccLoWord + 1u)) { m_state.vcc_uniform = false; }
	m_state.uniform &= ~region->second.vgprs;
	m_regions.erase(region);
}

// EXEC is rewritten only by mask algebra: V_CMPX narrows it, S_*_SAVEEXEC and S_MOV/S_AND/S_ANDN2/... combine
// masks. A launch-dependent number never reaches it, so each lane's bit evolves as on the guest.
bool Body::ExecWrite(const ShaderInstruction& inst)
{
	if (WritesExecOnly(inst.type))
	{
		if (!VectorSources(inst)) { return false; }
		m_shape.exec = m_shape.exec >= 0 ? m_partition.Child(m_shape.exec, inst.pc, true) : -1;
		return true;
	}
	if (!IsMaskAlgebra(inst.type)) { return Fail(inst, "EXEC is rewritten by an operation that is not mask algebra"); }
	uint8_t taint = kClean;
	for (int source = 0; source < inst.src_num; ++source)
	{
		if (IsConstant(inst.src[source])) { continue; }
		const uint8_t source_taint = Taint(inst.src[source]);
		if ((source_taint & (kInfo | kDiverged)) != 0u || (source_taint & kMask) == 0u) { return Fail(inst, "EXEC assigned a value that is not a mask"); }
		taint |= source_taint;
	}
	const bool saveexec = inst.dst.type != Operand::ExecLo && inst.dst.type != Operand::ExecHi;
	const int  before   = m_shape.exec;
	const int  after    = ExecAfter(inst);
	if (saveexec)
	{
		Define(inst.dst, kMask);
		unsigned first = 0;
		unsigned count = 0;
		if (ScalarRange(inst.dst, &first, &count) && count == 2u && before >= 0) { m_shape.saved[first] = before; }
	}
	m_shape.exec                     = after;
	m_state.scalar[kExecLoWord]      = kMask;
	m_state.scalar[kExecLoWord + 1u] = kMask;
	if (WritesScc(inst.type)) { m_state.scalar[kSccWord] = kMask; }
	return true;
}

bool Body::VgprDefined(unsigned vgpr) const
{
	if (m_state.defined.test(vgpr)) { return true; }
	const auto partial = m_shape.partial.find(vgpr);
	return partial != m_shape.partial.end() && m_partition.Covers(partial->second, m_shape.exec);
}

// A masked write defines the VGPR for the lanes of the current EXEC node only.
void Body::DefineVgpr(unsigned vgpr)
{
	if (m_shape.exec == 0)
	{
		m_state.defined.set(vgpr);
		return;
	}
	if (m_shape.exec < 0) { return; }
	auto& nodes = m_shape.partial[vgpr];
	nodes.insert(m_shape.exec);
	if (m_partition.Covers(nodes, 0)) { m_state.defined.set(vgpr); }
}

void Body::ForgetScalarShape(const ShaderOperand& op)
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &first, &count)) { return; }
	for (unsigned word = first; word < first + count; ++word)
	{
		for (unsigned base: {word, word - 1u})
		{
			m_shape.saved.erase(base);
			m_shape.compare.erase(base);
		}
		m_shape.restored_compare.erase(word);
		m_shape.restored_saved.erase(word);
	}
}

// The compare pair one scalar word belongs to, and which half of it the word is.
bool Body::CompareHalf(const ShaderOperand& op, std::pair<CompareMask, unsigned>* half) const
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &first, &count) || count != 1u) { return false; }
	for (unsigned part: {0u, 1u})
	{
		const auto found = first >= part ? m_shape.compare.find(first - part) : m_shape.compare.end();
		if (found == m_shape.compare.end()) { continue; }
		*half = {found->second, part};
		return true;
	}
	return false;
}

void Body::RestoreCompareWord(const ShaderOperand& op, const std::pair<CompareMask, unsigned>& half)
{
	unsigned word  = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &word, &count) || count != 1u || word < half.second) { return; }
	m_shape.restored_compare[word] = half;
	const unsigned pair     = word - half.second;
	const auto     low      = m_shape.restored_compare.find(pair);
	const auto     high     = m_shape.restored_compare.find(pair + 1u);
	const bool     complete = low != m_shape.restored_compare.end() && high != m_shape.restored_compare.end() &&
	                      low->second == std::make_pair(half.first, 0u) && high->second == std::make_pair(half.first, 1u);
	if (complete) { m_shape.compare[pair] = half.first; }
}

// The EXEC node a scalar word holds half of: a word of EXEC or of a saved EXEC pair.
bool Body::SavedHalf(const ShaderOperand& op, std::pair<int, unsigned>* half) const
{
	if ((op.type == Operand::ExecLo || op.type == Operand::ExecHi) && op.size == 1 && m_shape.exec >= 0)
	{
		*half = {m_shape.exec, op.type == Operand::ExecLo ? 0u : 1u};
		return true;
	}
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &first, &count) || count != 1u) { return false; }
	for (unsigned part: {0u, 1u})
	{
		const auto found = first >= part ? m_shape.saved.find(first - part) : m_shape.saved.end();
		if (found == m_shape.saved.end()) { continue; }
		*half = {found->second, part};
		return true;
	}
	return false;
}

void Body::RestoreSavedWord(const ShaderOperand& op, const std::pair<int, unsigned>& half)
{
	unsigned word  = 0;
	unsigned count = 0;
	if (!ScalarRange(op, &word, &count) || count != 1u || word < half.second) { return; }
	m_shape.restored_saved[word] = half;
	const unsigned pair = word - half.second;
	const auto     low  = m_shape.restored_saved.find(pair);
	const auto     high = m_shape.restored_saved.find(pair + 1u);
	if (low != m_shape.restored_saved.end() && high != m_shape.restored_saved.end() && low->second == std::make_pair(half.first, 0u) &&
	    high->second == std::make_pair(half.first, 1u))
	{
		m_shape.saved[pair] = half.first;
	}
}

// EXEC & C for a compare C made under an ancestor of EXEC is C evaluated under EXEC: inactive lanes read 0 and active
// ones were active when C ran. Compilers emit it to restore a spilled compare before a VCC branch, reading EXEC
// itself or a saved copy of it.
bool Body::ExecRestrictedCompare(const ShaderInstruction& inst, CompareMask* mask) const
{
	if (inst.type != Type::SAndB64 || inst.src_num != 2 || m_shape.exec < 0) { return false; }
	const auto is_exec = [this](const ShaderOperand& op) {
		unsigned first = 0;
		unsigned count = 0;
		if (op.type == Operand::ExecLo) { return true; }
		if (!ScalarRange(op, &first, &count) || count != 2u) { return false; }
		const auto saved = m_shape.saved.find(first);
		return saved != m_shape.saved.end() && saved->second == m_shape.exec;
	};
	const bool exec_first = is_exec(inst.src[0]);
	if (!exec_first && !is_exec(inst.src[1])) { return false; }
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(exec_first ? inst.src[1] : inst.src[0], &first, &count) || count != 2u) { return false; }
	const auto found = m_shape.compare.find(first);
	if (found == m_shape.compare.end() || !m_partition.Descends(m_shape.exec, found->second.under)) { return false; }
	*mask = {found->second.compare, m_shape.exec, found->second.uniform};
	return true;
}

// EXEC node after a mask-algebra write, or -1 when it is no node of the partition.
int Body::ExecAfter(const ShaderInstruction& inst) const
{
	const auto node_of = [this](const ShaderOperand& op) -> int {
		if (op.type == Operand::ExecLo) { return m_shape.exec; }
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(op, &first, &count) || count != 2u) { return -1; }
		const auto saved = m_shape.saved.find(first);
		return saved != m_shape.saved.end() ? saved->second : -1;
	};
	const auto compare_of = [this](const ShaderOperand& op, CompareMask* mask) {
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(op, &first, &count) || count != 2u) { return false; }
		const auto found = m_shape.compare.find(first);
		if (found == m_shape.compare.end()) { return false; }
		*mask = found->second;
		return true;
	};
	// EXEC = mask & EXEC (or ~mask & EXEC) splits EXEC in two halves that partition it. A compare made under an
	// ancestor of EXEC holds each lane of EXEC exactly, since that lane was active when it ran, so the compare names
	// the split; any other mask splits it by this instruction alone.
	const int exec  = m_shape.exec;
	const auto split = [&](const ShaderOperand& op, bool then_side) {
		if (exec < 0) { return -1; }
		CompareMask mask {};
		const bool named = compare_of(op, &mask) && m_partition.Descends(exec, mask.under);
		return const_cast<ExecPartition&>(m_partition).Child(exec, named ? mask.compare : inst.pc, then_side);
	};
	switch (inst.type)
	{
		case Type::SMovB64: return node_of(inst.src[0]);
		case Type::SAndSaveexecB64: return split(inst.src[0], true);
		case Type::SAndn1SaveexecB64: return split(inst.src[0], false);
		case Type::SAndB64:
		{
			const bool exec_first = inst.src[0].type == Operand::ExecLo;
			if (!exec_first && inst.src[1].type != Operand::ExecLo) { return -1; }
			return split(exec_first ? inst.src[1] : inst.src[0], true);
		}
		case Type::SAndn2B64:
		{
			// EXEC = saved \ EXEC: the other half of the split EXEC is one half of.
			const int saved = node_of(inst.src[0]);
			if (inst.src[1].type != Operand::ExecLo || saved < 0 || exec <= 0 || m_partition.Parent(exec) != saved) { return -1; }
			return const_cast<ExecPartition&>(m_partition).Child(saved, m_partition.Compare(exec), !m_partition.IsThen(exec));
		}
		case Type::SOrB64:
		{
			const int a = node_of(inst.src[0]);
			const int b = node_of(inst.src[1]);
			if (a < 0 || b < 0) { return -1; }
			if (m_partition.Descends(a, b)) { return b; }
			if (m_partition.Descends(b, a)) { return a; }
			int parent = -1;
			int other  = -1;
			return m_partition.Sibling(a, &parent, &other) && other == b ? parent : -1;
		}
		default: return -1;
	}
}

// Joins two paths arriving at `pc`. An EXEC branch skips its region only for lanes that are all inactive, so what
// the region defined under its own EXEC node holds on the skipping path too (vacuously). Joins that an edge every
// lane takes also reaches keep only what both paths define.
void Body::MergeShape(ExecShape* into, const ExecShape& other, uint32_t pc) const
{
	into->position_histories |= other.position_histories;
	std::vector<int> vacuous;
	if (const auto region = m_regions.find(pc); region != m_regions.end() && m_uniform_targets.count(pc) == 0u) { vacuous = region->second.roots; }
	const auto vacuous_node = [&](int node) {
		for (int root: vacuous)
		{
			if (root >= 0 && m_partition.Descends(node, root)) { return true; }
		}
		return false;
	};
	if (into->exec != other.exec) { into->exec = -1; }
	for (auto it = into->saved.begin(); it != into->saved.end();)
	{
		const auto found = other.saved.find(it->first);
		it = (found != other.saved.end() && found->second == it->second) ? std::next(it) : into->saved.erase(it);
	}
	for (auto it = into->compare.begin(); it != into->compare.end();)
	{
		const auto found = other.compare.find(it->first);
		it = (found != other.compare.end() && found->second == it->second) ? std::next(it) : into->compare.erase(it);
	}
	for (auto* facts: {&into->uniform_under, &into->scope})
	{
		const auto& theirs = facts == &into->scope ? other.scope : other.uniform_under;
		for (auto it = facts->begin(); it != facts->end();)
		{
			const auto found = theirs.find(it->first);
			it = (found != theirs.end() && found->second == it->second) ? std::next(it) : facts->erase(it);
		}
	}
	const auto keep_common = [](auto* mine, const auto& theirs) {
		for (auto it = mine->begin(); it != mine->end();)
		{
			const auto found = theirs.find(it->first);
			it = (found != theirs.end() && found->second == it->second) ? std::next(it) : mine->erase(it);
		}
	};
	keep_common(&into->spill_compare, other.spill_compare);
	keep_common(&into->restored_compare, other.restored_compare);
	keep_common(&into->spill_saved, other.spill_saved);
	keep_common(&into->restored_saved, other.restored_saved);
	std::map<unsigned, std::set<int>> merged;
	const std::map<unsigned, std::set<int>>* sides[] = {&into->partial, &other.partial};
	for (const auto* side: sides)
	{
		for (const auto& [vgpr, nodes]: *side)
		{
			const auto mine   = into->partial.find(vgpr);
			const auto theirs = other.partial.find(vgpr);
			for (int node: nodes)
			{
				const bool both = mine != into->partial.end() && mine->second.count(node) != 0u && theirs != other.partial.end() &&
				                  theirs->second.count(node) != 0u;
				if (both || vacuous_node(node)) { merged[vgpr].insert(node); }
			}
		}
	}
	into->partial = std::move(merged);
}

bool Body::ScalarLoad(const ShaderInstruction& inst)
{
	for (int source = 0; source < inst.src_num; ++source)
	{
		if (Taint(inst.src[source]) != kClean) { return Fail(inst, "scalar load addressed by launch-dependent data"); }
	}
	Define(inst.dst, kClean);
	return true;
}

bool Body::ScalarAlu(const ShaderInstruction& inst)
{
	uint8_t taint = kClean;
	for (int source = 0; source < inst.src_num; ++source) { taint |= Taint(inst.src[source]); }
	if (ReadsSccImplicitly(inst.type))
	{
		if (m_state.scalar[kSccWord] != kClean) { return Fail(inst, "select chosen by launch-dependent SCC"); }
	}
	if ((taint & kMask) != 0u && !IsMaskAlgebra(inst.type) && !IsSelect(inst.type))
	{
		return Fail(inst, "mask observed as a number");
	}
	if (inst.dst.type == Operand::ExecLo || inst.dst.type == Operand::ExecHi) { return ExecWrite(inst); }
	// Snapshot an exact saved EXEC node before Define invalidates an aliased destination pair.
	const int  exec_copy    = inst.type == Type::SMovB64 && inst.src_num == 1 ? ExecAfter(inst) : -1;
	const bool uniform_copy = ((inst.type == Type::SMovB64 || inst.type == Type::SMovB32) && inst.src_num == 1 && IsUniformMask(inst.src[0])) ||
	                          ((inst.type == Type::SAndB64 || inst.type == Type::SAndB32) && inst.src_num == 2 && m_shape.exec == 0 &&
	                           ((inst.src[0].type == Operand::ExecLo && IsUniformMask(inst.src[1])) ||
	                            (inst.src[1].type == Operand::ExecLo && IsUniformMask(inst.src[0]))));
	// Each complete uniform mask holds every launched lane or none, so their intersection or union does too, including
	// when the destination aliases a source.
	const bool uniform_and = (inst.type == Type::SAndB64 || inst.type == Type::SOrB64) && inst.src_num == 2 && inst.dst.size == 2 &&
	                         inst.src[0].size == 2 && inst.src[1].size == 2 &&
	                         IsUniformMask(inst.src[0]) && IsUniformMask(inst.src[1]);
	const int   scope = (taint & kDiverged) != 0u ? SourceScope(inst) : -1;
	CompareMask exec_compare {};
	const bool  restricted = ExecRestrictedCompare(inst, &exec_compare);
	// A copy of a compare pair holds the same lane bits, so it names the same split.
	CompareMask copied_compare {};
	bool        compare_copy = false;
	if (unsigned first = 0, count = 0; inst.type == Type::SMovB64 && inst.src_num == 1 && ScalarRange(inst.src[0], &first, &count) && count == 2u)
	{
		const auto found = m_shape.compare.find(first);
		compare_copy     = found != m_shape.compare.end();
		if (compare_copy) { copied_compare = found->second; }
	}
	Define(inst.dst, taint);
	Define(inst.dst2, taint);
	SetScope(inst.dst, scope);
	SetScope(inst.dst2, scope);
	if (uniform_copy || uniform_and) { DefineUniformMask(inst.dst, true); }
	if (exec_copy >= 0)
	{
		unsigned first = 0;
		unsigned count = 0;
		if (ScalarRange(inst.dst, &first, &count) && count == 2u) { m_shape.saved[first] = exec_copy; }
	}
	if (restricted)
	{
		unsigned first = 0;
		unsigned count = 0;
		if (ScalarRange(inst.dst, &first, &count) && count == 2u) { m_shape.compare[first] = exec_compare; }
	}
	if (compare_copy)
	{
		unsigned first = 0;
		unsigned count = 0;
		if (ScalarRange(inst.dst, &first, &count) && count == 2u) { m_shape.compare[first] = copied_compare; }
	}
	if (WritesScc(inst.type)) { m_state.scalar[kSccWord] = taint; }
	return true;
}

// Sources of a vector instruction: launch data never; mask bits only as the lane bit.
bool Body::VectorSources(const ShaderInstruction& inst)
{
	for (int source = 0; source < inst.src_num; ++source)
	{
		const auto& op = inst.src[source];
		if (op.dpp) { return Fail(inst, "data-parallel primitive reads another lane"); }
		unsigned first = 0;
		unsigned count = 0;
		if (VgprRange(op, &first, &count))
		{
			// A buffer address holds one VGPR per enabled part (index, offset); the decoder's
			// operand width also counts OFFEN alone as a pair.
			if (source == 0 && IsBufferLoad(inst.type)) { count = (inst.buffer_idxen ? 1u : 0u) + (inst.buffer_offen ? 1u : 0u); }
			for (unsigned word = 0; word < count; ++word)
			{
				if (!VgprDefined(first + word)) { return Fail(inst, "VGPR read before it is defined on every path"); }
			}
			continue;
		}
		const uint8_t taint = Taint(op);
		if (taint == kClean || (taint == kMask && IsLaneBit(inst, source)) || InScope(op)) { continue; }
		if (taint == kDiverged) { return Fail(inst, "scalar read from a lane reaches lanes outside the EXEC it was read under"); }
		return Fail(inst, (taint & kInfo) != 0u ? "launch-dependent scalar reaches a vector instruction" : "mask observed as a number");
	}
	if (inst.type == Type::VMacF32)
	{
		unsigned first = 0;
		unsigned count = 0;
		if (!VgprRange(inst.dst, &first, &count)) { return Fail(inst, "MAC destination is not a VGPR"); }
		for (unsigned word = 0; word < count; ++word)
		{
			if (!VgprDefined(first + word)) { return Fail(inst, "VGPR read before it is defined on every path"); }
		}
	}
	return true;
}

bool Body::VectorDestinations(const ShaderInstruction& inst, bool uniform_result)
{
	for (const auto* dst: {&inst.dst, &inst.dst2})
	{
		unsigned first = 0;
		unsigned count = 0;
		if (VgprRange(*dst, &first, &count))
		{
			for (unsigned word = 0; word < count; ++word)
			{
				DefineVgpr(first + word);
				// A write under part of the lanes leaves the others their old value.
				m_state.uniform[first + word] = uniform_result && m_shape.exec == 0;
				m_shape.uniform_under.erase(first + word);
				if (uniform_result && m_shape.exec > 0) { m_shape.uniform_under[first + word] = m_shape.exec; }
			}
			continue;
		}
		if (dst->type == Operand::ExecLo || dst->type == Operand::ExecHi) { return Fail(inst, "vector instruction writes EXEC"); }
		Define(*dst, kMask); // compare or carry result: one bit per lane
		unsigned first_word = 0;
		unsigned words      = 0;
		if (StartsWith(inst.type, "VCmp") && ScalarRange(*dst, &first_word, &words) && words == 2u && m_shape.exec >= 0)
		{
			m_shape.compare[first_word] = {inst.pc, m_shape.exec, uniform_result};
		}
	}
	return true;
}

// SDWA compare source modifiers are lane-local; all other control fields must match a whole-dword compare.
bool SdwaPreservesUniformity(const ShaderInstruction& inst)
{
	if (StartsWith(inst.type, "VCmp"))
	{
		if (StartsWith(inst.type, "VCmpx")) { return false; }
		constexpr uint32_t kSourceModifierBits = (1u << 20u) | (1u << 21u) | (1u << 28u) | (1u << 29u);
		auto normalized = inst;
		normalized.vop_sdwa_ctrl &= ~kSourceModifierBits;
		return ShaderComputeWaveSdwaCompareIdentityTuple(normalized);
	}
	return ShaderComputeWaveSdwaVop2IdentitySupported(inst);
}

// A V_WRITELANE/V_READLANE with a constant lane that only moves a scalar through one VGPR lane and
// back (a compiler spill) is lowered to a private scalar slot, as the generic lowering proves with the
// same predicates: it exchanges no lane and leaves the VGPR untouched. The read yields the scalar
// written there, so the slot carries the taint of every write to it.
bool Body::ScalarSpill(const ShaderInstruction& inst)
{
	int vgpr = 0;
	int lane = 0;
	if (IsStaticScalarSpillWrite(inst, &vgpr, &lane) && IsScalarSpillStore(*m_code, m_index, vgpr, lane))
	{
		m_spills[{vgpr, lane}] |= Taint(inst.src[0]);
		std::pair<CompareMask, unsigned> half;
		if (CompareHalf(inst.src[0], &half)) { m_shape.spill_compare[{vgpr, lane}] = half; } else { m_shape.spill_compare.erase({vgpr, lane}); }
		std::pair<int, unsigned> saved;
		if (SavedHalf(inst.src[0], &saved)) { m_shape.spill_saved[{vgpr, lane}] = saved; } else { m_shape.spill_saved.erase({vgpr, lane}); }
		return true;
	}
	if (IsStaticScalarSpillRead(inst, &vgpr, &lane) && HasLiveScalarSpill(*m_code, m_index, vgpr, lane))
	{
		const auto slot = m_spills.find({vgpr, lane});
		if (slot == m_spills.end()) { return Fail(inst, "scalar spill read before the body writes its slot"); }
		Define(inst.dst, slot->second);
		if (const auto half = m_shape.spill_compare.find({vgpr, lane}); half != m_shape.spill_compare.end()) { RestoreCompareWord(inst.dst, half->second); }
		if (const auto half = m_shape.spill_saved.find({vgpr, lane}); half != m_shape.spill_saved.end()) { RestoreSavedWord(inst.dst, half->second); }
		return true;
	}
	return Fail(inst, "lane exchange");
}

// V_READFIRSTLANE of a VGPR holding one clean value in every lane reads that value whichever
// lane is first: a uniform copy, not a lane exchange. Under a partial EXEC the VGPR need only hold one value in the
// lanes of an ancestor of EXEC: the guest reads its first EXEC lane, the host the first EXEC lane of its subgroup,
// which is the same value wherever the subgroup holds a lane of EXEC. The scalar is the guest value for those lanes
// only, so it is scoped to EXEC (see ExecShape).
bool Body::UniformReadFirstLane(const ShaderInstruction& inst)
{
	unsigned first = 0;
	unsigned count = 0;
	if (inst.src_num != 1 || !VgprRange(inst.src[0], &first, &count) || count != 1u || !VgprDefined(first) || !VgprUniformHere(first))
	{
		return Fail(inst, "lane exchange");
	}
	if (m_state.uniform[first])
	{
		Define(inst.dst, kClean);
		return true;
	}
	Define(inst.dst, kDiverged);
	SetScope(inst.dst, m_shape.exec);
	return true;
}

bool Body::Vector(const ShaderInstruction& inst)
{
	const bool valu = StartsWith(inst.type, "V");
	if (!valu && !IsVectorLoad(inst.type)) { return Fail(inst, "instruction class is not supported in a fused front body"); }
	if (IsLaneExchange(inst.type)) { return Fail(inst, "lane exchange"); }
	if (!VectorSources(inst)) { return false; }
	const bool compare = StartsWith(inst.type, "VCmp");
	const bool pure    = valu && (!inst.vop_sdwa || SdwaPreservesUniformity(inst));
	// A relative move reads a register its operands do not name.
	const bool uniform = pure && !StartsWith(inst.type, "VMovrel") && SourcesAreUniformClean(inst);
	if (!VectorDestinations(inst, uniform)) { return false; }
	if (compare) { DefineUniformMask(inst.dst, uniform && m_shape.exec == 0); }
	return true;
}

// An explicit-LOD image read: its address VGPRs (the NSA list or the contiguous range) must be
// defined, its T#/S# must not depend on the launch; the result is a per-lane number.
bool Body::ImageRead(const ShaderInstruction& inst)
{
	const bool nsa       = inst.mimg_address_num > 0;
	const int  addresses = std::min(inst.src[0].size, ImageReadAddressCount(inst));
	if (nsa && addresses > inst.mimg_address_num) { return Fail(inst, "image address list shorter than its format"); }
	for (int address = 0; address < addresses; ++address)
	{
		const ShaderOperand& op    = nsa ? inst.mimg_address[address] : inst.src[0];
		const int            index = op.register_id + (nsa ? 0 : address);
		if (op.type != Operand::Vgpr || index < 0 || index >= kVgprs || !VgprDefined(static_cast<unsigned>(index)))
		{
			return Fail(inst, "image address VGPR read before it is defined on every path");
		}
	}
	for (int source = 1; source < inst.src_num; ++source)
	{
		if (Taint(inst.src[source]) != kClean) { return Fail(inst, "image resource selected by launch-dependent data"); }
	}
	return VectorDestinations(inst, false);
}

bool Body::PositionExport(const ShaderInstruction& inst, bool position0, bool done)
{
	if (m_shape.exec != 0) { return Fail(inst, "position export under partial or unknown EXEC"); }
	if (!VectorSources(inst)) { return false; }

	const unsigned target_bit = position0 ? kPosition0Seen : kPosition1Seen;
	uint8_t        next_histories = 0;
	for (unsigned history = 0; history < kPositionHistoryCount; ++history)
	{
		if ((m_shape.position_histories & (1u << history)) == 0u) { continue; }
		if ((history & kPositionDone) != 0u) { return Fail(inst, "position export follows DONE on an incoming path"); }
		if ((history & target_bit) != 0u) { return Fail(inst, "position target is exported more than once on an incoming path"); }
		const unsigned next = history | target_bit | (done ? kPositionDone : 0u);
		next_histories |= static_cast<uint8_t>(1u << next);
	}
	if (next_histories == 0u) { return Fail(inst, "position export has no reachable history"); }
	m_shape.position_histories = next_histories;
	return true;
}

bool Body::Export(const ShaderInstruction& inst)
{
	if (inst.format == ShaderInstructionFormat::PrimVsrc0OffOffOffDone) { return Fail(inst, "primitive export outside the prologue"); }

	const uint32_t target = (inst.raw_word >> 4u) & 0x3fu;
	const bool position0 = inst.format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done ||
	                       inst.format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3;
	const bool position1 = inst.format == ShaderInstructionFormat::Pos1Vsrc0OffOffOff ||
	                       inst.format == ShaderInstructionFormat::Pos1Vsrc0OffOffOffDone ||
	                       inst.format == ShaderInstructionFormat::Pos1OffOffVsrc0Off;
	if ((position0 && target != 12u) || (position1 && target != 13u))
	{
		return Fail(inst, "position format disagrees with its raw export target");
	}
	if ((target == 12u || target == 13u) &&
	    ((inst.raw_word & 0xfu) != inst.exp_enable_mask || ((inst.raw_word >> 10u) & 7u) != inst.exp_control))
	{
		return Fail(inst, "position control tuple disagrees with its raw export fields");
	}
	if (target == 0x0cu)
	{
		if (inst.format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done)
		{
			if (inst.exp_enable_mask != 0x0fu || inst.exp_control != 0x02u || inst.src_num != 4)
			{
				return Fail(inst, "POS0 DONE requires EN=0xf and four full-precision sources");
			}
			return PositionExport(inst, true, true);
		}
		if (inst.format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3)
		{
			if (inst.exp_enable_mask != 0x0fu || inst.exp_control != 0x00u || inst.src_num != 4)
			{
				return Fail(inst, "non-final POS0 requires EN=0xf and four full-precision sources");
			}
			return PositionExport(inst, true, false);
		}
		return Fail(inst, "unsupported POS0 export control form");
	}
	if (target == 0x0du)
	{
		if (inst.format == ShaderInstructionFormat::Pos1Vsrc0OffOffOff)
		{
			if (inst.exp_enable_mask != 0x01u || inst.exp_control != 0x00u || inst.src_num != 1)
			{
				return Fail(inst, "non-final POS1-X requires EN=1 and one source");
			}
			return PositionExport(inst, false, false);
		}
		if (inst.format == ShaderInstructionFormat::Pos1Vsrc0OffOffOffDone)
		{
			if (inst.exp_enable_mask != 0x01u || inst.exp_control != 0x02u || inst.src_num != 1)
			{
				return Fail(inst, "final POS1-X requires EN=1 and one source");
			}
			return PositionExport(inst, false, true);
		}
		if (inst.format == ShaderInstructionFormat::Pos1OffOffVsrc0Off)
		{
			if (inst.exp_enable_mask != 0x04u || inst.exp_control != 0x00u || inst.src_num != 1)
			{
				return Fail(inst, "non-final POS1-Z requires EN=4 and one source");
			}
			return PositionExport(inst, false, false);
		}
		return Fail(inst, "unsupported POS1 export control form");
	}
	return VectorSources(inst);
}

bool Body::EndProgram(const ShaderInstruction& inst)
{
	for (unsigned history = 0; history < kPositionHistoryCount; ++history)
	{
		if ((m_shape.position_histories & (1u << history)) == 0u) { continue; }
		if ((history & (kPosition0Seen | kPositionDone)) != (kPosition0Seen | kPositionDone))
		{
			return Fail(inst, "every path must export POS0 and reach a final position DONE");
		}
	}
	return m_shape.position_histories != 0u || Fail(inst, "terminator has no position history");
}

const MaxReduction* Body::ReductionAt(uint32_t index) const
{
	for (const auto& unit: m_reductions)
	{
		if (index >= unit.any && index < unit.join) { return &unit; }
	}
	return nullptr;
}

// The parts of a wave maximum the other rules refuse; every other instruction of it takes those rules.
bool Body::MaxReductionPart(const ShaderInstruction& inst, const MaxReduction& unit, bool* done)
{
	*done = true;
	if (m_index == unit.any)
	{
		const auto compare = m_shape.compare.find(kVccLoWord);
		if (!m_loops.empty()) { return Fail(inst, "wave maximum inside a loop"); }
		if (m_shape.exec < 0 || compare == m_shape.compare.end() || compare->second.compare != m_code->GetInstructions().At(unit.compare).pc ||
		    compare->second.under != m_shape.exec)
		{
			return Fail(inst, "wave maximum counts lanes outside the EXEC of its region");
		}
		// Whether some lane of the subgroup holds 0 < x: it differs between host subgroups.
		Define(inst.dst, kInfo);
		return true;
	}
	if (m_index == unit.lanes.seed_index || unit.lanes.IsStep(m_index)) { return true; } // folded by the reads
	for (const auto& read: unit.lanes.reads)
	{
		if (read.index != m_index) { continue; }
		Define(inst.dst, kInfo); // the maximum of the subgroup's lanes
		return true;
	}
	if (m_index == unit.combine) { return m_shape.exec >= 0 ? VectorDestinations(inst, false) : Fail(inst, "wave maximum under an untracked EXEC"); }
	if (m_index == unit.atomic) { return MaxReductionAtomic(inst); }
	if (inst.type == Type::Exp && m_index > unit.branch) { return Fail(inst, "export inside a wave maximum region"); }
	*done = false;
	return true;
}

// Every lane that stores must name the same dword: a wave-uniform index into a resource free of launch data.
bool Body::MaxReductionAtomic(const ShaderInstruction& inst)
{
	const auto index = static_cast<unsigned>(inst.src[0].register_id);
	const auto data  = static_cast<unsigned>(inst.dst.register_id);
	if (index >= static_cast<unsigned>(kVgprs) || !VgprDefined(index) || !VgprUniformHere(index) || !VgprDefined(data))
	{
		return Fail(inst, "wave maximum stored at a lane-dependent address");
	}
	for (int source = 1; source < inst.src_num; ++source)
	{
		if (Taint(inst.src[source]) != kClean) { return Fail(inst, "wave maximum stored through launch-dependent data"); }
	}
	return true;
}

bool Body::Step(const ShaderInstruction& inst)
{
	if (const auto* unit = ReductionAt(m_index); unit != nullptr)
	{
		bool       done = false;
		const bool ok   = MaxReductionPart(inst, *unit, &done);
		if (done) { return ok; }
	}
	switch (inst.type)
	{
		case Type::SInstPrefetch:
		case Type::SWaitcnt: return true;
		case Type::SEndpgm: return EndProgram(inst);
		case Type::SBranch:
		case Type::SCbranchScc0:
		case Type::SCbranchScc1:
		case Type::SCbranchVccz:
		case Type::SCbranchVccnz:
		case Type::SCbranchExecz:
		case Type::SCbranchExecnz: return Branch(inst);
		case Type::Exp: return Export(inst);
		default: break;
	}
	if (ShaderInstructionTypeChangesExec(inst.type)) { return ExecWrite(inst); }
	if (IsScalarLoad(inst.type)) { return ScalarLoad(inst); }
	if (inst.type == Type::SSubbU32) { return Fail(inst, "S_SUBB_U32 borrow-in is not modeled by the NGG front proof"); }
	if (inst.type == Type::SAshrI64) { return Fail(inst, "S_ASHR_I64 pair shifts are not modeled by the NGG front proof"); }
	if (IsScalarAlu(inst.type)) { return ScalarAlu(inst); }
	if (IsLaneLocalImageRead(inst.type)) { return ImageRead(inst); }
	if (inst.type == Type::VWritelaneB32 || inst.type == Type::VReadlaneB32) { return ScalarSpill(inst); }
	if (inst.type == Type::VReadfirstlaneB32) { return UniformReadFirstLane(inst); }
	return Vector(inst);
}

ShaderNggFrontBodyProof Body::Run(const ShaderCode& code, uint32_t first_index, uint32_t guest_wave_size, const ShaderNggScalarDependencies& wave_scalars)
{
	m_code                   = &code;
	const auto& instructions = code.GetInstructions();
	if (first_index >= instructions.Size() || instructions.At(instructions.Size() - 1u).type != Type::SEndpgm)
	{
		m_result.reason = "no body or no terminator";
		return m_result;
	}
	for (unsigned word = 0; word < kWords; ++word) { m_state.scalar[word] = wave_scalars.test(word) ? kInfo : kClean; }
	m_state.scalar[kExecLoWord]      = kMask;
	m_state.scalar[kExecLoWord + 1u] = kMask;
	m_state.defined.set(kVertexIdVgpr);
	m_state.defined.set(kInstanceIdVgpr);
	if (!FindLoops()) { return m_result; }
	m_reductions = FindMaxReductions(code, guest_wave_size);
	if (!m_back_edges.empty() && m_back_edges.begin()->first < first_index)
	{
		Fail(instructions.At(m_back_edges.begin()->second), "loop around the prologue");
		return m_result;
	}
	for (uint32_t index = first_index; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (const auto pending = m_pending.find(inst.pc); pending != m_pending.end())
		{
			const bool fallthrough = m_state.reachable;
			m_state.Merge(pending->second);
			if (fallthrough) { MergeShape(&m_shape, m_pending_shape[inst.pc], inst.pc); } else { m_shape = m_pending_shape[inst.pc]; }
			m_pending.erase(pending);
			m_pending_shape.erase(inst.pc);
		}
		if (m_state.reachable) { CloseRegions(inst.pc); }
		if (!m_state.reachable) { continue; }
		m_index = index;
		if (m_back_edges.count(index) != 0u && m_loops.count(index) == 0u && !EnterLoop(index)) { return m_result; }
		if (const auto end = m_loop_ends.find(index); end != m_loop_ends.end())
		{
			bool again = false;
			if (!CloseLoop(end->second, &again)) { return m_result; }
			if (again)
			{
				index = end->second - 1u; // the next iteration of the walk is the header
				continue;
			}
			m_state.reachable = false; // the back edge holds: the joined header state covers it
			continue;
		}
		if (!Step(inst))
		{
			return m_result;
		}
	}
	m_result.lane_local = m_state.reachable && m_pending.empty() && m_regions.empty() && m_loops.empty();
	if (!m_result.lane_local) { m_result.reason = "program does not reach its terminator"; }
	return m_result;
}

} // namespace

ShaderNggFrontBodyProof ShaderProveNggFrontBodyLaneLocal(const ShaderCode& code, uint32_t first_index, uint32_t guest_wave_size,
                                                        const ShaderNggScalarDependencies& wave_scalars)
{
	Body body;
	return body.Run(code, first_index, guest_wave_size, wave_scalars);
}

ShaderLaneReductions ShaderFindNggFrontMaxReductions(const ShaderCode& code, uint32_t guest_wave_size)
{
	ShaderLaneReductions result;
	for (auto& unit: FindMaxReductions(code, guest_wave_size)) { result.reductions.push_back(std::move(unit.lanes)); }
	return result;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
