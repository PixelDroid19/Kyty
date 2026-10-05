#include "Emulator/Graphics/ShaderNggFrontBody.h"

#include "Emulator/Graphics/ShaderComputeWaveSdwa.h"

#include "ShaderLaneFlow.h"
#include "ShaderNativeWaveInternal.h"
#include "ShaderSpirvInternal.h"

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
	uint32_t compare = 0; // pc of the compare
	int      under   = 0; // EXEC node it was evaluated under (inactive lanes read 0)
	bool operator==(const CompareMask& other) const { return compare == other.compare && under == other.under; }
};

// Per path: the EXEC node (-1 when it is no node of the partition), the scalar pairs holding a node or a compare,
// and the nodes under which each VGPR is defined beyond FlowState::defined (every lane).
struct ExecShape
{
	int                                exec = 0;
	std::map<unsigned, int>            saved;
	std::map<unsigned, CompareMask>    compare;
	std::map<unsigned, std::set<int>>  partial;
};

class Body
{
public:
	ShaderNggFrontBodyProof Run(const ShaderCode& code, uint32_t first_index, const ShaderNggScalarDependencies& wave_scalars);

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

	bool Step(const ShaderInstruction& inst);
	bool Branch(const ShaderInstruction& inst);
	bool ScalarAlu(const ShaderInstruction& inst);
	bool ScalarLoad(const ShaderInstruction& inst);
	bool Vector(const ShaderInstruction& inst);
	bool ScalarSpill(const ShaderInstruction& inst);
	bool VectorSources(const ShaderInstruction& inst);
	bool VectorDestinations(const ShaderInstruction& inst, bool uniform_result);
	bool Export(const ShaderInstruction& inst);
	bool ImageRead(const ShaderInstruction& inst);
	bool ExecWrite(const ShaderInstruction& inst);
	void OpenRegion(const ShaderInstruction& inst, uint32_t target);
	void CloseRegions(uint32_t pc);
	[[nodiscard]] bool VgprDefined(unsigned vgpr) const;
	void DefineVgpr(unsigned vgpr);
	void ForgetScalarShape(const ShaderOperand& op);
	void MergeShape(ExecShape* into, const ExecShape& other, uint32_t pc) const;
	int  ExecAfter(const ShaderInstruction& inst) const;

	ShaderNggFrontBodyProof m_result;
	FlowState               m_state;
	std::map<uint32_t, FlowState> m_pending; // forward branch targets by pc
	// Lane-divergent regions by join pc: the scalars and VGPRs some path through the region may write.
	struct Region
	{
		std::bitset<kWords> scalars;
		std::bitset<kVgprs> vgprs;
		std::vector<int>    roots; // EXEC node at each branch into this join
	};
	std::map<uint32_t, Region> m_regions;
	std::map<uint32_t, ExecShape> m_pending_shape;
	std::set<uint32_t>          m_uniform_targets; // joins also reached by an edge every lane takes
	ExecPartition               m_partition;
	ExecShape                   m_shape;
	// Taint of the scalars every static spill write put in a (VGPR, lane) slot.
	std::map<std::pair<int, int>, uint8_t> m_spills;
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
	if (op.type == Operand::VccLo) { m_state.vcc_uniform = uniform; } // wave32 compares define VCC_LO only
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

// Sources that are wave-uniform numbers: constants, launch-free scalars, uniform VGPRs.
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
				if (!m_state.uniform[first + word]) { return false; }
			}
			continue;
		}
		if (Taint(op) != kClean || !ScalarRange(op, &first, &count)) { return false; }
	}
	return true;
}

bool Body::Branch(const ShaderInstruction& inst)
{
	// A branch on EXEC is decided per host subgroup but per guest wave. It skips a region only for lanes that are all
	// inactive, so the region's vector writes are masked either way; its scalar writes are not (see OpenRegion).
	// A VCC branch leaves EXEC alone: a non-uniform one would run vector writes for lanes chosen by other lanes.
	const bool divergent = inst.type == Type::SCbranchExecz || inst.type == Type::SCbranchExecnz;
	if (inst.type == Type::SCbranchVccz || inst.type == Type::SCbranchVccnz)
	{
		if (!m_state.vcc_uniform) { return Fail(inst, "VCC branch on a comparison that is not wave-uniform"); }
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
	if (divergent) { OpenRegion(inst, join); } else { m_uniform_targets.insert(join); }
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
void Body::OpenRegion(const ShaderInstruction& inst, uint32_t target)
{
	auto& region = m_regions[target];
	region.roots.push_back(m_shape.exec);
	for (const auto& candidate: m_code->GetInstructions())
	{
		if (candidate.pc <= inst.pc || candidate.pc >= target || WritesExecOnly(candidate.type)) { continue; }
		for (const ShaderOperand* dst: {&candidate.dst, &candidate.dst2})
		{
			unsigned first = 0;
			unsigned count = 0;
			if (ScalarRange(*dst, &first, &count))
			{
				for (unsigned word = 0; word < count; ++word) { region.scalars.set(first + word); }
			} else if (VgprRange(*dst, &first, &count))
			{
				for (unsigned word = 0; word < count && first + word < static_cast<unsigned>(kVgprs); ++word) { region.vgprs.set(first + word); }
			}
		}
		if (IsScalarAlu(candidate.type) && WritesScc(candidate.type)) { region.scalars.set(kSccWord); }
	}
	// EXEC is a mask either way; its per-lane bit is tracked exactly.
	region.scalars.reset(kExecLoWord);
	region.scalars.reset(kExecLoWord + 1u);
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
		if ((source_taint & kInfo) != 0u || (source_taint & kMask) == 0u) { return Fail(inst, "EXEC assigned a value that is not a mask"); }
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
	}
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
	const int  exec = m_shape.exec;
	CompareMask mask {};
	switch (inst.type)
	{
		case Type::SMovB64: return node_of(inst.src[0]);
		case Type::SAndSaveexecB64:
			// EXEC &= (compare under EXEC): the lanes of EXEC where the compare holds.
			return exec >= 0 && compare_of(inst.src[0], &mask) && mask.under == exec ? const_cast<ExecPartition&>(m_partition).Child(exec, mask.compare, true) : -1;
		case Type::SAndB64:
		{
			const bool exec_first = inst.src[0].type == Operand::ExecLo;
			const auto& other     = exec_first ? inst.src[1] : inst.src[0];
			if (exec < 0 || (!exec_first && inst.src[1].type != Operand::ExecLo) || !compare_of(other, &mask) || mask.under != exec) { return -1; }
			return const_cast<ExecPartition&>(m_partition).Child(exec, mask.compare, true);
		}
		case Type::SAndn2B64:
		{
			// EXEC = saved \ EXEC: the else half of the split EXEC is the then half of.
			const int saved = node_of(inst.src[0]);
			if (inst.src[1].type != Operand::ExecLo || saved < 0 || m_partition.Parent(exec) != saved || !m_partition.IsThen(exec)) { return -1; }
			return const_cast<ExecPartition&>(m_partition).Child(saved, m_partition.Compare(exec), false);
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
	const int  exec_copy    = inst.type == Type::SMovB64 && inst.src_num == 1 && inst.src[0].type == Operand::ExecLo ? m_shape.exec : -1;
	const bool uniform_copy = ((inst.type == Type::SMovB64 || inst.type == Type::SMovB32) && inst.src_num == 1 && IsUniformMask(inst.src[0])) ||
	                          ((inst.type == Type::SAndB64 || inst.type == Type::SAndB32) && inst.src_num == 2 &&
	                           ((inst.src[0].type == Operand::ExecLo && IsUniformMask(inst.src[1])) ||
	                            (inst.src[1].type == Operand::ExecLo && IsUniformMask(inst.src[0]))));
	Define(inst.dst, taint);
	Define(inst.dst2, taint);
	if (uniform_copy) { DefineUniformMask(inst.dst, true); }
	if (exec_copy >= 0)
	{
		unsigned first = 0;
		unsigned count = 0;
		if (ScalarRange(inst.dst, &first, &count) && count == 2u) { m_shape.saved[first] = exec_copy; }
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
		if (taint == kClean || (taint == kMask && IsLaneBit(inst, source))) { continue; }
		return Fail(inst, (taint & kInfo) != 0u ? "launch-dependent scalar reaches a vector instruction" : "mask observed as a number");
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
			}
			continue;
		}
		if (dst->type == Operand::ExecLo || dst->type == Operand::ExecHi) { return Fail(inst, "vector instruction writes EXEC"); }
		Define(*dst, kMask); // compare or carry result: one bit per lane
		unsigned first_word = 0;
		unsigned words      = 0;
		if (StartsWith(inst.type, "VCmp") && ScalarRange(*dst, &first_word, &words) && words == 2u && m_shape.exec >= 0)
		{
			m_shape.compare[first_word] = {inst.pc, m_shape.exec};
		}
	}
	return true;
}

// SDWA with whole-dword selects and no sub-dword modifiers computes what the plain encoding does;
// anything else selects bytes or words of its sources and is not modelled here.
bool SdwaIsIdentity(const ShaderInstruction& inst)
{
	return StartsWith(inst.type, "VCmp") ? (!StartsWith(inst.type, "VCmpx") && ShaderComputeWaveSdwaCompareIdentityTuple(inst)) : ShaderComputeWaveSdwaVop2IdentitySupported(inst);
}

// A V_WRITELANE/V_READLANE with a constant lane that only moves a scalar through one VGPR lane and
// back (a compiler spill) is lowered to a private scalar slot, as the generic lowering proves with the
// same predicates: it exchanges no lane and leaves the VGPR untouched. The read yields the scalar
// written there, so the slot carries the taint of every write to it.
bool Body::ScalarSpill(const ShaderInstruction& inst)
{
	int vgpr = 0;
	int lane = 0;
	if (IsStaticScalarSpillWrite(inst, &vgpr, &lane) && HasFutureScalarSpillRead(*m_code, m_index, vgpr, lane))
	{
		m_spills[{vgpr, lane}] |= Taint(inst.src[0]);
		return true;
	}
	if (IsStaticScalarSpillRead(inst, &vgpr, &lane) && HasLiveScalarSpill(*m_code, m_index, vgpr, lane))
	{
		const auto slot = m_spills.find({vgpr, lane});
		if (slot == m_spills.end()) { return Fail(inst, "scalar spill read before the body writes its slot"); }
		Define(inst.dst, slot->second);
		return true;
	}
	return Fail(inst, "lane exchange");
}

bool Body::Vector(const ShaderInstruction& inst)
{
	const bool valu = StartsWith(inst.type, "V");
	if (!valu && !IsVectorLoad(inst.type)) { return Fail(inst, "instruction class is not supported in a fused front body"); }
	if (IsLaneExchange(inst.type)) { return Fail(inst, "lane exchange"); }
	if (!VectorSources(inst)) { return false; }
	const bool compare = StartsWith(inst.type, "VCmp");
	const bool pure    = valu && (!inst.vop_sdwa || SdwaIsIdentity(inst));
	const bool uniform = pure && SourcesAreUniformClean(inst);
	if (!VectorDestinations(inst, uniform)) { return false; }
	if (compare) { DefineUniformMask(inst.dst, uniform); }
	return true;
}

// An explicit-LOD image read: its address VGPRs (the NSA list or the contiguous range) must be
// defined, its T#/S# must not depend on the launch; the result is a per-lane number.
bool Body::ImageRead(const ShaderInstruction& inst)
{
	const bool nsa       = inst.mimg_address_num > 0;
	const int  addresses = inst.src[0].size;
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

bool Body::Export(const ShaderInstruction& inst)
{
	if (inst.format == ShaderInstructionFormat::PrimVsrc0OffOffOffDone) { return Fail(inst, "primitive export outside the prologue"); }
	return VectorSources(inst);
}

bool Body::Step(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case Type::SInstPrefetch:
		case Type::SWaitcnt: return true;
		case Type::SEndpgm: return true;
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
	if (IsScalarAlu(inst.type)) { return ScalarAlu(inst); }
	if (IsLaneLocalImageRead(inst.type)) { return ImageRead(inst); }
	if (inst.type == Type::VWritelaneB32 || inst.type == Type::VReadlaneB32) { return ScalarSpill(inst); }
	return Vector(inst);
}

ShaderNggFrontBodyProof Body::Run(const ShaderCode& code, uint32_t first_index, const ShaderNggScalarDependencies& wave_scalars)
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
		if (!Step(inst)) { return m_result; }
	}
	m_result.lane_local = m_state.reachable && m_pending.empty() && m_regions.empty();
	if (!m_result.lane_local) { m_result.reason = "program does not reach its terminator"; }
	return m_result;
}

} // namespace

ShaderNggFrontBodyProof ShaderProveNggFrontBodyLaneLocal(const ShaderCode& code, uint32_t first_index,
                                                        const ShaderNggScalarDependencies& wave_scalars)
{
	Body body;
	return body.Run(code, first_index, wave_scalars);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
