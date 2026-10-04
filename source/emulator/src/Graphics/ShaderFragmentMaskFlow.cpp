#include "Emulator/Graphics/ShaderFragmentMaskFlow.h"

#include "ShaderLaneFlow.h"
#include "ShaderNativeWaveInternal.h"

#include <map>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

using namespace LaneFlow;

using Type    = ShaderInstructionType;
using Operand = ShaderOperandType;

bool Plain(const ShaderOperand& operand)
{
	return !operand.dpp && !operand.absolute && !operand.negate && !operand.clamp;
}

bool IsCompare(Type type)
{
	return StartsWith(type, "SCmp") || StartsWith(type, "SBitcmp");
}

// Scalar ALU forms that always define SCC from their result (S_MUL/S_BFM/S_BREV and the
// bit-scan family do not), so they replace its taint instead of accumulating onto it.
bool DefinesScc(Type type)
{
	return AnyPrefix(type, {"SAnd", "SOr", "SXor", "SNand", "SNor", "SXnor", "SNot", "SWqm", "SLsh", "SAshr", "SAdd", "SSub", "SBfe",
	                        "SMin", "SMax", "SAbs", "SBcnt"}) &&
	       !StartsWith(type, "SMulk");
}

bool IsConditionalBranch(Type type)
{
	return type == Type::SCbranchScc0 || type == Type::SCbranchScc1 || type == Type::SCbranchVccz || type == Type::SCbranchVccnz ||
	       type == Type::SCbranchExecz || type == Type::SCbranchExecnz;
}

// A branch region whose entry is lane divergent, with the scalar words written in it.
struct Region
{
	uint32_t            end_pc = 0;
	std::bitset<kWords> written;
	// Mask words whose bit is zero for every lane outside the region (a subset of the EXEC it was entered with):
	// compares evaluated and EXEC copies taken while EXEC was such a subset (VOPC clears inactive lanes).
	std::bitset<kWords> zero_outside;
	bool                exec_subset = true; // EXEC is still a subset of the entry EXEC
};

// A loop found from a backward branch; its body is the instructions with pc in [head_pc, back_pc].
struct Loop
{
	uint32_t head_pc = 0;
	uint32_t back_pc = 0;
	// A lane-divergent branch leaves the loop or repeats it, so lanes run different numbers of
	// iterations and the host subgroup's count is not the guest wave's.
	bool divergent = false;
	// Every scalar word some instruction of the body may write.
	std::bitset<kWords> written;

	[[nodiscard]] bool Contains(uint32_t pc) const { return pc >= head_pc && pc <= back_pc; }
};

// At most this many walks of the program before the flow gives up on a fixpoint. Taint only
// grows, so a walk that changes no loop-head state is the last; real shaders need a handful.
constexpr uint32_t kMaxPasses = 64;

bool BranchTarget(const ShaderInstruction& inst, uint32_t* target)
{
	if (inst.src_num < 1) { return false; }
	const int64_t where = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
	if (where < 0 || where > UINT32_MAX) { return false; }
	*target = static_cast<uint32_t>(where);
	return true;
}

bool IsLaneDependentBranch(Type type)
{
	return type == Type::SCbranchExecz || type == Type::SCbranchExecnz || type == Type::SCbranchVccz || type == Type::SCbranchVccnz;
}

// The scalar words an instruction can write (EXEC is never region state).
void CollectWrites(const ShaderInstruction& inst, std::bitset<kWords>* written)
{
	if (WritesExecOnly(inst.type)) { return; }
	for (const ShaderOperand* dst: {&inst.dst, &inst.dst2})
	{
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(*dst, &first, &count) || dst->type == Operand::ExecLo || dst->type == Operand::ExecHi) { continue; }
		for (unsigned word = 0; word < count; ++word) { written->set(first + word); }
	}
	if (IsCompare(inst.type) || (IsScalarAlu(inst.type) && WritesScc(inst.type))) { written->set(kSccWord); }
}

class Flow
{
public:
	ShaderFragmentMaskFlow Run(const ShaderCode& code);

private:
	bool Fail(const ShaderInstruction& inst, const char* reason)
	{
		m_result.refused_pc = inst.pc;
		m_result.located    = true;
		m_result.reason     = reason;
		return false;
	}
	[[nodiscard]] uint8_t Taint(const ShaderOperand& operand) const;
	[[nodiscard]] uint8_t SourceTaint(const ShaderInstruction& inst) const;
	bool                  CheckSources(const ShaderInstruction& inst, bool* propagates);
	bool                  Define(const ShaderInstruction& inst, const ShaderOperand& dst, uint8_t taint);
	void                  MarkWritten(unsigned first, unsigned count);
	[[nodiscard]] bool    ZeroOutsideRegion(const ShaderOperand& operand) const;
	[[nodiscard]] bool    PreservesLanesOutsideRegion(const ShaderInstruction& inst) const;
	void                  TrackRegionSubset(const ShaderInstruction& inst, bool exec_subset_before);
	void                  UpdateScc(const ShaderInstruction& inst, uint8_t source_taint);
	bool                  UniformComparison(const ShaderInstruction& inst) const;
	void                  TrackUniformMask(const ShaderInstruction& inst);
	[[nodiscard]] bool    IsUniformMask(const ShaderOperand& operand) const;
	void                  SetUniformMask(const ShaderOperand& dst, bool uniform);
	bool                  Step(const ShaderInstruction& inst);
	bool                  Branch(const ShaderInstruction& inst);
	void                  EnterJoin(uint32_t pc);
	bool                  Walk();
	void                  FindLoops();
	bool                  InDivergentLoop(uint32_t pc) const;
	void                  LeaveLoops(FlowState* state, uint32_t from_pc, uint32_t to_pc) const;
	bool                  HasInstructionAt(uint32_t pc) const;
	void                  MarkAcceptedExecWrites(const ShaderCode& code);
	bool                  ExecWide() const { return (m_state.scalar[kExecLoWord] & kWide) != 0u; }
	[[nodiscard]] uint8_t WideResult(const ShaderInstruction& inst) const;
	bool                  InertDeadTail(uint32_t pc) const;

	ShaderFragmentMaskFlow        m_result;
	FlowState                     m_state;
	std::map<uint32_t, FlowState> m_entry;   // state arriving at each branch target, by pc; kept across walks
	std::vector<Region>           m_regions; // open lane-divergent regions
	std::vector<Loop>             m_loops;
	bool                          m_changed = false; // a back edge changed a loop-head state during this walk
	bool                          m_lane_preserving = false; // the write being defined keeps every inactive lane's bit
	const ShaderCode*             m_code    = nullptr;
};

uint8_t Flow::Taint(const ShaderOperand& operand) const
{
	if (operand.type == Operand::ExecZ || operand.type == Operand::VccZ) { return kMask; }
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(operand, &first, &count)) { return kClean; }
	uint8_t taint = kClean;
	bool    any_mask = false;
	bool    all_mask = true;
	for (unsigned word = 0; word < count; ++word)
	{
		taint |= m_state.scalar[first + word];
		any_mask = any_mask || (m_state.scalar[first + word] & kMask) != 0u;
		all_mask = all_mask && (m_state.scalar[first + word] & kMask) != 0u;
	}
	// A 64-bit mask with one word replaced by a number is neither a mask nor a number.
	return (any_mask && !all_mask) ? static_cast<uint8_t>(taint | kMixed) : taint;
}

uint8_t Flow::SourceTaint(const ShaderInstruction& inst) const
{
	uint8_t taint = kClean;
	for (int source = 0; source < inst.src_num; ++source) { taint |= Taint(inst.src[source]); }
	for (int address = 0; address < inst.mimg_address_num; ++address) { taint |= Taint(inst.mimg_address[address]); }
	return taint;
}

// Each tainted source must be consumed as a lane bit or by mask algebra, and only
// scalar ALU may carry a diverged word or a pair that is half mask, half number (VCC
// enters undefined, so a load into VCC_LO leaves such a pair). Neither is refused until
// something observes it. `propagates` reports whether the result carries the taint
// of its sources.
bool Flow::CheckSources(const ShaderInstruction& inst, bool* propagates)
{
	*propagates = false;
	for (int address = 0; address < inst.mimg_address_num; ++address)
	{
		if (Taint(inst.mimg_address[address]) != kClean) { return Fail(inst, "mask used as an image address"); }
	}
	for (int source = 0; source < inst.src_num; ++source)
	{
		const auto& operand = inst.src[source];
		if (operand.type == Operand::ExecZ || operand.type == Operand::VccZ)
		{
			return Fail(inst, "mask summary flag observed");
		}
		const uint8_t taint = Taint(operand);
		if (taint == kClean) { continue; }
		if ((taint & kMixed) != 0u && !IsScalarAlu(inst.type)) { return Fail(inst, "mask pair partly overwritten by a number"); }
		if ((taint & kDiverged) != 0u && !IsScalarAlu(inst.type))
		{
			return Fail(inst, "scalar written under divergent control flow is read after the join");
		}
		// A select's choice is already checked against SCC; its data may be masks.
		if ((taint & kMask) != 0u && !IsLaneBit(inst, source) && !IsMaskAlgebra(inst.type) && !IsSelect(inst.type))
		{
			return Fail(inst, "mask observed as a number");
		}
		if (!IsLaneBit(inst, source)) { *propagates = true; }
	}
	return true;
}

void Flow::MarkWritten(unsigned first, unsigned count)
{
	for (auto& region: m_regions)
	{
		for (unsigned word = 0; word < count; ++word)
		{
			region.zero_outside.reset(first + word);
			if (!m_lane_preserving) { region.written.set(first + word); }
		}
	}
}

bool Flow::ZeroOutsideRegion(const ShaderOperand& operand) const
{
	if (m_regions.empty()) { return false; }
	if (operand.type == Operand::ExecLo) { return m_regions.back().exec_subset; }
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(operand, &first, &count)) { return false; }
	for (unsigned word = 0; word < count; ++word)
	{
		if (!m_regions.back().zero_outside.test(first + word)) { return false; }
	}
	return true;
}

// D = D OR/ANDN2/XOR M with M zero outside the region leaves the bit of every lane outside it unchanged, so it does
// not matter that the guest runs the region for the wave while a host subgroup with no lane inside skips it.
bool Flow::PreservesLanesOutsideRegion(const ShaderInstruction& inst) const
{
	if (inst.type != Type::SOrB64 && inst.type != Type::SAndn2B64 && inst.type != Type::SXorB64) { return false; }
	if (inst.src_num != 2 || inst.dst.type != inst.src[0].type || inst.dst.register_id != inst.src[0].register_id ||
	    inst.dst.type == Operand::ExecLo)
	{
		return false;
	}
	return ZeroOutsideRegion(inst.src[1]);
}

// Keeps the innermost region's subset facts after `inst`: which mask words are zero outside the region and whether
// EXEC is still a subset of the EXEC the region was entered with. Narrowing keeps it, a restore from a subset copy
// re-establishes it, a widening (S_WQM) or any other EXEC write loses it.
void Flow::TrackRegionSubset(const ShaderInstruction& inst, bool exec_subset_before)
{
	if (m_regions.empty()) { return; }
	auto&      region     = m_regions.back();
	const auto subset_of  = [&](const ShaderOperand& op) {
		if (op.type == Operand::ExecLo) { return exec_subset_before; }
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(op, &first, &count)) { return false; }
		for (unsigned word = 0; word < count; ++word)
		{
			if (!region.zero_outside.test(first + word)) { return false; }
		}
		return true;
	};
	const auto mark = [&](const ShaderOperand& op) {
		unsigned first = 0;
		unsigned count = 0;
		if (!ScalarRange(op, &first, &count)) { return; }
		for (unsigned word = 0; word < count; ++word) { region.zero_outside.set(first + word); }
	};
	const bool writes_exec = ShaderInstructionWritesExec(inst);
	if (StartsWith(inst.type, "VCmp") && !writes_exec && exec_subset_before) { mark(inst.dst); }
	if (inst.type == Type::SMovB64 && inst.src[0].type == Operand::ExecLo && inst.dst.type != Operand::ExecLo && exec_subset_before)
	{
		mark(inst.dst);
	}
	if (!writes_exec) { return; }
	if (inst.type == Type::SAndSaveexecB64 && exec_subset_before) { mark(inst.dst); }
	bool subset = false;
	if (WritesExecOnly(inst.type)) { subset = exec_subset_before; }
	if (inst.type == Type::SMovB64 && inst.dst.type == Operand::ExecLo) { subset = subset_of(inst.src[0]); }
	if (inst.type == Type::SAndB64 && inst.dst.type == Operand::ExecLo) { subset = subset_of(inst.src[0]) || subset_of(inst.src[1]); }
	if (inst.type == Type::SAndSaveexecB64) { subset = exec_subset_before || subset_of(inst.src[0]); }
	if (inst.type == Type::SAndn2B64 && inst.dst.type == Operand::ExecLo) { subset = subset_of(inst.src[0]); }
	region.exec_subset = subset;
}

bool Flow::Define(const ShaderInstruction& inst, const ShaderOperand& dst, uint8_t taint)
{
	if (dst.type == Operand::ExecZ || dst.type == Operand::VccZ) { return Fail(inst, "mask summary flag written"); }
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(dst, &first, &count))
	{
		if (dst.type == Operand::Sgpr) { return Fail(inst, "scalar destination out of range"); }
		return true;
	}
	if (dst.type == Operand::ExecLo || dst.type == Operand::ExecHi)
	{
		if ((taint & kMask) == 0u) { return Fail(inst, "EXEC assigned a numeric value"); }
		if ((taint & kDiverged) != 0u) { return Fail(inst, "EXEC assigned a divergent scalar"); }
		if ((taint & kMixed) != 0u) { return Fail(inst, "EXEC assigned a mask pair partly overwritten by a number"); }
		m_state.scalar[kExecLoWord]      = static_cast<uint8_t>(kMask | (taint & kWide));
		m_state.scalar[kExecLoWord + 1u] = m_state.scalar[kExecLoWord];
		return true; // EXEC is one bit per lane by construction; it is never region state
	}
	for (unsigned word = 0; word < count; ++word)
	{
		m_state.scalar[first + word] = taint;
		m_state.uniform_mask.reset(first + word);
	}
	MarkWritten(first, count);
	if (dst.type == Operand::VccLo || dst.type == Operand::VccHi) { m_state.vcc_uniform = false; }
	return true;
}

// SCC is written implicitly by most scalar ALU operations. A compare always defines
// it from its sources, so it is the only instruction that can clear the taint; any
// other scalar operation that saw a tainted value may have made SCC tainted.
void Flow::UpdateScc(const ShaderInstruction& inst, uint8_t source_taint)
{
	if (IsCompare(inst.type) || DefinesScc(inst.type))
	{
		m_state.scalar[kSccWord] = source_taint;
		MarkWritten(kSccWord, 1u);
		return;
	}
	if (StartsWith(inst.type, "S") && WritesScc(inst.type) && (source_taint != kClean || ShaderInstructionTypeChangesExec(inst.type)))
	{
		m_state.scalar[kSccWord] |= static_cast<uint8_t>(source_taint | (ShaderInstructionTypeChangesExec(inst.type) ? kMask : kClean));
		MarkWritten(kSccWord, 1u);
	}
}

// Scalar words that hold a compare result identical in every lane. A VCC branch on such a mask decides
// the same way for the whole wave (and for a one-lane host invocation), so it opens no divergent region.
bool Flow::IsUniformMask(const ShaderOperand& operand) const
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(operand, &first, &count)) { return false; }
	for (unsigned word = 0; word < count; ++word)
	{
		if (!m_state.uniform_mask.test(first + word)) { return false; }
	}
	return true;
}

void Flow::SetUniformMask(const ShaderOperand& dst, bool uniform)
{
	unsigned first = 0;
	unsigned count = 0;
	if (!ScalarRange(dst, &first, &count)) { return; }
	for (unsigned word = 0; word < count; ++word) { m_state.uniform_mask.set(first + word, uniform); }
	if (dst.type == Operand::VccLo) { m_state.vcc_uniform = uniform; } // wave32 compares define VCC_LO only
}

// Compare of uniform sources, a copy of such a mask, or EXEC narrowed by one (nonzero exactly when the compare holds).
void Flow::TrackUniformMask(const ShaderInstruction& inst)
{
	if (StartsWith(inst.type, "VCmp")) { SetUniformMask(inst.dst, UniformComparison(inst)); return; }
	const bool copy = (inst.type == Type::SMovB64 || inst.type == Type::SMovB32) && inst.src_num == 1 && IsUniformMask(inst.src[0]);
	const bool narrowed = (inst.type == Type::SAndB64 || inst.type == Type::SAndB32) && inst.src_num == 2 &&
	                      ((inst.src[0].type == Operand::ExecLo && IsUniformMask(inst.src[1])) ||
	                       (inst.src[1].type == Operand::ExecLo && IsUniformMask(inst.src[0])));
	if (copy || narrowed) { SetUniformMask(inst.dst, true); }
}

// A vector compare of constants and scalar numbers gives every lane the same bit.
bool Flow::UniformComparison(const ShaderInstruction& inst) const
{
	if (!StartsWith(inst.type, "VCmp") || ShaderInstructionTypeChangesExec(inst.type)) { return false; }
	for (int source = 0; source < inst.src_num; ++source)
	{
		const auto& operand = inst.src[source];
		unsigned    first   = 0;
		unsigned    count   = 0;
		if (IsConstant(operand)) { continue; }
		if (!ScalarRange(operand, &first, &count) || Taint(operand) != kClean) { return false; }
	}
	return true;
}

// Width of the mask an instruction produces: live-only (narrow) or possibly including the
// helper lanes of a quad (kWide). Bits of a compare exist only for the lanes that were active.
uint8_t Flow::WideResult(const ShaderInstruction& inst) const
{
	const Type type = inst.type;
	if (StartsWith(type, "SWqm")) { return kWide; }
	if (StartsWith(type, "VCmp") || ShaderNativePackedResult(inst)) { return ExecWide() ? kWide : kClean; }
	bool any_wide = false, all_wide = true, any_mask = false, nonzero_constant = false;
	for (int source = 0; source < inst.src_num; ++source)
	{
		const uint8_t taint = Taint(inst.src[source]);
		if ((taint & kMask) != 0u)
		{
			any_mask = true;
			any_wide = any_wide || (taint & kWide) != 0u;
			all_wide = all_wide && (taint & kWide) != 0u;
		} else if (IsConstant(inst.src[source]) && inst.src[source].constant.u != 0u)
		{
			nonzero_constant = true;
		}
	}
	const bool first_wide = inst.src_num > 0 && (Taint(inst.src[0]) & kWide) != 0u;
	// a & ~b is a subset of a; a & b of each operand; a complement is never confined to live lanes.
	if (StartsWith(type, "SAndn2")) { return first_wide ? kWide : kClean; }
	if (StartsWith(type, "SAnd")) { return (any_mask && all_wide) ? kWide : kClean; }
	if (AnyPrefix(type, {"SOrn2", "SNand", "SNor", "SXnor", "SNot"})) { return kWide; }
	return (any_wide || (nonzero_constant && StartsWith(type, "S") && !IsSelect(type) && !StartsWith(type, "SMov"))) ? kWide : kClean;
}

// Code that runs when no lane survives and produces nothing: optionally EXEC = 0, a null
// MRT export, S_ENDPGM. Reaching it early is what the hardware's wave-wide "any lane left"
// branch does, and the host model takes it per subgroup, so neither can be told apart.
bool Flow::InertDeadTail(uint32_t pc) const
{
	const auto& instructions = m_code->GetInstructions();
	uint32_t    index        = 0;
	while (index < instructions.Size() && instructions.At(index).pc != pc) { ++index; }
	if (index >= instructions.Size()) { return false; }
	const auto zero_exec = [](const ShaderInstruction& inst)
	{
		return (inst.type == Type::SMovB64 || inst.type == Type::SMovB32) && inst.dst.type == Operand::ExecLo && inst.src_num == 1 &&
		       inst.src[0].type == Operand::IntegerInlineConstant && inst.src[0].constant.i == 0;
	};
	if (zero_exec(instructions.At(index))) { ++index; }
	if (index + 2u != instructions.Size()) { return false; }
	const auto& export_inst = instructions.At(index);
	// An empty valid mask exported through an MRT slot or through the dedicated null target: either
	// way every lane of the wave is killed, which is also what the main path does when none survives.
	const bool null_export = ShaderIsNullMrtDoneFormat(export_inst.format) || export_inst.format == ShaderInstructionFormat::NullVmDone;
	return export_inst.type == Type::Exp && null_export && export_inst.src_num == 0 &&
	       instructions.At(index + 1u).type == Type::SEndpgm;
}

bool Flow::HasInstructionAt(uint32_t pc) const
{
	for (const auto& candidate: m_code->GetInstructions())
	{
		if (candidate.pc == pc) { return true; }
	}
	return false;
}

bool Flow::InDivergentLoop(uint32_t pc) const
{
	for (const auto& loop: m_loops)
	{
		if (loop.divergent && loop.Contains(pc)) { return true; }
	}
	return false;
}

// Control leaving a loop whose lanes ran different iteration counts: every scalar the body
// writes holds, for a lane that stopped early, the value of an iteration the guest wave may
// have run further. Lanes still running are unaffected, so nothing changes inside the loop.
void Flow::LeaveLoops(FlowState* state, uint32_t from_pc, uint32_t to_pc) const
{
	for (const auto& loop: m_loops)
	{
		if (!loop.divergent || !loop.Contains(from_pc) || loop.Contains(to_pc)) { continue; }
		for (unsigned word = 0; word < kWords; ++word)
		{
			if (loop.written.test(word)) { state->scalar[word] |= kDiverged; }
		}
	}
}

void Flow::FindLoops()
{
	const auto& instructions = m_code->GetInstructions();
	for (const auto& inst: instructions)
	{
		uint32_t target = 0;
		if ((inst.type == Type::SBranch || IsConditionalBranch(inst.type)) && BranchTarget(inst, &target) && target <= inst.pc)
		{
			Loop loop;
			loop.head_pc = target;
			loop.back_pc = inst.pc;
			m_loops.push_back(loop);
		}
	}
	for (auto& loop: m_loops)
	{
		for (const auto& inst: instructions)
		{
			if (!loop.Contains(inst.pc)) { continue; }
			CollectWrites(inst, &loop.written);
			uint32_t target = 0;
			if (!IsLaneDependentBranch(inst.type) || !BranchTarget(inst, &target)) { continue; }
			loop.divergent = loop.divergent || !loop.Contains(target) || target == loop.head_pc;
		}
	}
}

bool Flow::Branch(const ShaderInstruction& inst)
{
	bool divergent = false;
	uint32_t target = 0;
	if (!BranchTarget(inst, &target)) { return Fail(inst, "branch without a target"); }
	switch (inst.type)
	{
		case Type::SBranch: break;
		case Type::SCbranchExecz:
		case Type::SCbranchExecnz: divergent = true; break;
		case Type::SCbranchVccz:
		case Type::SCbranchVccnz:
		{
			ShaderOperand vcc {};
			vcc.type = Operand::VccLo;
			vcc.size = 2;
			if ((Taint(vcc) & kMixed) != 0u) { return Fail(inst, "VCC branch on a mask pair partly overwritten by a number"); }
			divergent = !m_state.vcc_uniform;
			break;
		}
		case Type::SCbranchScc0:
		case Type::SCbranchScc1:
		{
			const uint8_t scc = m_state.scalar[kSccWord];
			// "No lane survives" (SCC of a narrow mask) may skip to a tail that does nothing.
			const bool survivors_only = (scc & kMask) != 0u && (scc & (kDiverged | kMixed | kWide)) == 0u;
			if (scc != kClean && !(survivors_only && target > inst.pc && InertDeadTail(target)))
			{
				return Fail(inst, "SCC branch on a mask or divergent value");
			}
			break;
		}
		default: return Fail(inst, "branch form is not supported");
	}
	if (!HasInstructionAt(target)) { return Fail(inst, "branch target is not an instruction"); }
	const bool backward = target <= inst.pc;
	FlowState  outgoing = m_state;
	LeaveLoops(&outgoing, inst.pc, target);
	const auto found = m_entry.find(target);
	if (found == m_entry.end())
	{
		m_entry.emplace(target, outgoing);
		m_changed = m_changed || backward;
	} else if (found->second.Merge(outgoing))
	{
		m_changed = m_changed || backward;
	}
	// A backward branch is the loop's own business (see Loop::divergent); a region has no end to reach.
	if (divergent && !backward) { m_regions.push_back({target, {}}); }
	if (inst.type == Type::SBranch)
	{
		m_state.reachable = false;
	} else if (backward)
	{
		LeaveLoops(&m_state, inst.pc, inst.pc + 4u); // falling through leaves the loop
	}
	return true;
}

// Paths rejoin here: merge the branch states, then mark what the skipped block wrote. Entries
// stay: the next walk starts from them again until no back edge changes anything.
void Flow::EnterJoin(uint32_t pc)
{
	if (const auto entry = m_entry.find(pc); entry != m_entry.end()) { m_state.Merge(entry->second); }
	for (auto region = m_regions.begin(); region != m_regions.end();)
	{
		if (region->end_pc != pc)
		{
			++region;
			continue;
		}
		for (unsigned word = 0; word < kWords; ++word)
		{
			if (region->written.test(word)) { m_state.scalar[word] |= kDiverged; }
		}
		region = m_regions.erase(region);
	}
}

bool Flow::Step(const ShaderInstruction& inst)
{
	if (WritesMemory(inst.type)) { return Fail(inst, "memory write: helper lanes would be observable"); }
	if (inst.type == Type::Exp && ExecWide()) { return Fail(inst, "export while EXEC may include helper lanes"); }
	if (inst.type == Type::SBarrier) { return Fail(inst, "barrier"); }
	if (inst.type == Type::SBranch || IsConditionalBranch(inst.type)) { return Branch(inst); }
	if (inst.type == Type::SEndpgm)
	{
		m_state.reachable = false; // this path ends; the next label revives the walk
		return true;
	}
	// Forward EXEC/VCC branches are emitted quad-uniform (helpers take their quad's vote), so a fetch inside such a
	// region computes derivatives from its whole quad. Inside a loop exited by a lane mask the quad can still split
	// across iterations, so implicit derivatives stay refused there.
	if (InDivergentLoop(inst.pc) && UsesImplicitDerivatives(inst.type))
	{
		return Fail(inst, "derivative fetch inside a lane-divergent loop");
	}
	// A select or conditional move reads SCC; a tainted SCC makes its choice lane dependent.
	if (ReadsSccImplicitly(inst.type) && m_state.scalar[kSccWord] != kClean)
	{
		return Fail(inst, "SCC derived from a mask selects a value");
	}
	bool propagates = false;
	if (!CheckSources(inst, &propagates)) { return false; }
	const uint8_t source_taint = SourceTaint(inst);
	uint8_t       produced     = propagates ? static_cast<uint8_t>(source_taint & (kMask | kDiverged | kMixed)) : kClean;
	if (ShaderNativePackedResult(inst) || ShaderInstructionTypeChangesExec(inst.type)) { produced |= kMask; }
	// An empty mask is a mask: EXEC = 0 ends a discarded lane's execution.
	const bool empty_mask = (inst.type == Type::SMovB64 || inst.type == Type::SMovB32) && inst.src_num == 1 &&
	                        inst.src[0].type == Operand::IntegerInlineConstant && inst.src[0].constant.i == 0;
	if (empty_mask && inst.dst.type == Operand::ExecLo) { produced |= kMask; }
	if ((produced & kMask) != 0u) { produced = static_cast<uint8_t>((produced & ~kWide) | WideResult(inst)); }
	if (WritesExecOnly(inst.type))
	{
		// EXEC &= compare: it narrows, so it stays wide only if it already was.
		m_state.scalar[kExecLoWord]      = static_cast<uint8_t>(kMask | (ExecWide() ? kWide : kClean));
		m_state.scalar[kExecLoWord + 1u] = m_state.scalar[kExecLoWord];
		return true;
	}
	const bool exec_subset_before = !m_regions.empty() && m_regions.back().exec_subset;
	m_lane_preserving = PreservesLanesOutsideRegion(inst);
	const bool defined = Define(inst, inst.dst, produced) && Define(inst, inst.dst2, produced);
	m_lane_preserving = false;
	if (!defined) { return false; }
	TrackRegionSubset(inst, exec_subset_before);
	if (ShaderInstructionTypeChangesExec(inst.type) && inst.dst.type != Operand::ExecLo)
	{
		// S_*_SAVEEXEC: the destination took the old EXEC; EXEC is now the combination.
		const bool narrowing = StartsWith(inst.type, "SAnd") && !StartsWith(inst.type, "SAndn");
		const bool wide = narrowing ? (ExecWide() && (Taint(inst.src[0]) & kWide) != 0u) : true;
		m_state.scalar[kExecLoWord]      = static_cast<uint8_t>(kMask | (wide ? kWide : kClean));
		m_state.scalar[kExecLoWord + 1u] = m_state.scalar[kExecLoWord];
	}
	TrackUniformMask(inst);
	// SCC = "result is nonzero": it is as wide as the mask the instruction produced, not as the
	// sources it read (ANDN2 of a narrow mask by a wide one is narrow).
	const uint8_t scc_source = (produced & kMask) != 0u ? static_cast<uint8_t>((source_taint & ~kWide) | (produced & kWide)) : source_taint;
	UpdateScc(inst, scc_source);
	return true;
}

// Every EXEC write the walk accepted is pointwise mask algebra over existing lanes. The
// widening ones are safe because an export is refused while EXEC may include helper
// lanes (the kWide bit), so no closed-bracket shape has to be recognized.
void Flow::MarkAcceptedExecWrites(const ShaderCode& code)
{
	const auto& instructions = code.GetInstructions();
	m_result.closed_exec_write.assign(instructions.Size(), false);
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		m_result.closed_exec_write[index] = ShaderInstructionWritesExec(instructions.At(index));
	}
}

// One pass over the program in address order from the entry state, merging the states that
// arrive at branch targets. Forward edges are complete within a pass; back edges feed the next.
bool Flow::Walk()
{
	m_state   = {};
	m_changed = false;
	m_regions.clear();
	// EXEC, VCC and SCC enter undefined or lane dependent: treat them as masks.
	for (unsigned word : {kVccLoWord, kVccLoWord + 1u, kExecLoWord, kExecLoWord + 1u, kSccWord}) { m_state.scalar[word] = kMask; }
	const auto& instructions = m_code->GetInstructions();
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		EnterJoin(inst.pc);
		if (!m_state.reachable) { continue; }
		if (!Step(inst)) { return false; }
	}
	if (!m_regions.empty())
	{
		m_result.reason = "a divergent region was never closed";
		return false;
	}
	return true;
}

ShaderFragmentMaskFlow Flow::Run(const ShaderCode& code)
{
	m_code                   = &code;
	const auto& instructions = code.GetInstructions();
	if (code.GetType() != ShaderType::Pixel || instructions.IsEmpty() ||
	    instructions.At(instructions.Size() - 1u).type != Type::SEndpgm)
	{
		m_result.reason = "not a pixel program ending in S_ENDPGM";
		return m_result;
	}
	FindLoops();
	for (uint32_t pass = 0; pass < kMaxPasses; ++pass)
	{
		if (!Walk()) { return m_result; }
		if (!m_changed)
		{
			m_result.lane_local = true;
			MarkAcceptedExecWrites(code);
			return m_result;
		}
	}
	m_result.reason = "the mask flow did not reach a fixpoint";
	return m_result;
}

} // namespace

ShaderFragmentMaskFlow ShaderAnalyzeFragmentMaskFlow(const ShaderCode& code)
{
	Flow flow;
	return flow.Run(code);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
