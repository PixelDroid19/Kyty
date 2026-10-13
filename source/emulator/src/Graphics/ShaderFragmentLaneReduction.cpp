#include "Emulator/Graphics/ShaderFragmentLaneReduction.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include "ShaderLaneFlow.h"
#include "ShaderNativeWaveInternal.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <map>
#include <optional>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

using Type    = ShaderInstructionType;
using Operand = ShaderOperandType;

constexpr uint32_t kLanes = 64;
constexpr int      kVgprs = 256;

// Per guest lane: the lanes whose seed the register holds the OR of.
using LaneSets  = std::array<uint64_t, kLanes>;
using Network   = std::map<int, LaneSets>;
using VectorSet = std::bitset<kVgprs>;

bool Unmodified(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp;
}

bool PlainRegister(const ShaderOperand& operand)
{
	return Unmodified(operand) && !operand.dpp && operand.type == Operand::Vgpr && operand.size == 1 && operand.register_id >= 0 &&
	       operand.register_id < kVgprs;
}

bool Constant(const ShaderOperand& operand, uint32_t* value)
{
	if (!Unmodified(operand) || operand.dpp || operand.size != 0 ||
	    (operand.type != Operand::IntegerInlineConstant && operand.type != Operand::LiteralConstant))
	{
		return false;
	}
	*value = operand.constant.u;
	return true;
}

bool PlainVector(const ShaderInstruction& inst)
{
	return !inst.vop_sdwa && inst.vop3_omod == 0u && PlainRegister(inst.dst) &&
	       (inst.dst2.type == Operand::Unknown || inst.dst2.type == Operand::Null) && inst.dst2.size == 0;
}

bool LabelAt(const ShaderCode& code, uint32_t pc)
{
	for (const auto* labels: {&code.GetLabels(), &code.GetIndirectLabels()})
	{
		for (const auto& label: *labels)
		{
			if (!label.IsDisabled() && label.GetDst() == pc) { return true; }
		}
	}
	return false;
}

bool RowControl(uint32_t control)
{
	return (control >= 0x101u && control <= 0x10fu) || (control >= 0x111u && control <= 0x11fu) || (control >= 0x121u && control <= 0x12fu);
}

// DPP_ROW_SL/SR/RR (RDNA2 ISA, DPP_CTRL): the lane of the same 16-lane row that `lane` reads. A shift
// past the row edge reads nothing.
bool RowSource(uint32_t control, uint32_t lane, uint32_t* source)
{
	const uint32_t row_lane = lane & 15u;
	const uint32_t count    = control & 15u;
	if (control <= 0x10fu)
	{
		*source = lane + count;
		return row_lane + count <= 15u;
	}
	if (control <= 0x11fu)
	{
		*source = lane - count;
		return row_lane >= count;
	}
	*source = (lane & ~15u) | ((row_lane + 16u - count) & 15u);
	return true;
}

// V_PERMLANE16_B32 / V_PERMLANEX16_B32 (RDNA2 ISA): lane i of a row reads lane select[i] of its own row,
// or of the other row of its 32-lane half; the selects are {S2, S1} in nibbles.
uint32_t PermuteSource(bool cross, uint32_t select_low, uint32_t select_high, uint32_t lane)
{
	const uint32_t i      = lane & 15u;
	const uint32_t select = i < 8u ? (select_low >> (4u * i)) & 15u : (select_high >> (4u * (i - 8u))) & 15u;
	const uint32_t row    = (lane >> 4u) ^ (cross ? 1u : 0u);
	return row * 16u + select;
}

const LaneSets* Known(const Network& network, const ShaderOperand& operand)
{
	const auto found = network.find(operand.register_id);
	return found == network.end() ? nullptr : &found->second;
}

// D = S0 | S1 with S0 optionally row-shifted. A lane whose shift leaves the row keeps D or takes zero
// (BOUND_CTRL); with D the unshifted source both give that source's value, so the bit's meaning never matters.
bool OrStep(const ShaderInstruction& inst, Network* network)
{
	if (inst.src_num != 2 || inst.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || inst.vop3_op_sel != 0u) { return false; }
	const auto& shifted = inst.src[0];
	const auto& other   = inst.src[1];
	if (!PlainRegister(other) || !Unmodified(shifted) || shifted.type != Operand::Vgpr || shifted.size != 1) { return false; }
	const auto* left  = Known(*network, shifted);
	const auto* right = Known(*network, other);
	if (left == nullptr || right == nullptr) { return false; }
	if (shifted.dpp && (!RowControl(shifted.dpp_ctrl) || shifted.dpp_row_mask != 15u || shifted.dpp_bank_mask != 15u ||
	                    inst.dst.register_id != other.register_id))
	{
		return false;
	}
	LaneSets result {};
	for (uint32_t lane = 0; lane < kLanes; ++lane)
	{
		uint32_t source = lane;
		const bool read = !shifted.dpp || RowSource(shifted.dpp_ctrl, lane, &source);
		result[lane]    = (*right)[lane] | (read ? (*left)[source] : 0u);
	}
	(*network)[inst.dst.register_id] = result;
	return true;
}

// Every lane is active inside the region, so the FI and BOUND_CTRL bits (OP_SEL[1:0]) change nothing.
bool PermuteStep(const ShaderInstruction& inst, Network* network)
{
	uint32_t select_low  = 0;
	uint32_t select_high = 0;
	if (inst.src_num != 3 || inst.format != ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2 || inst.vop3_op_sel > 3u ||
	    !PlainRegister(inst.src[0]) || !Constant(inst.src[1], &select_low) || !Constant(inst.src[2], &select_high))
	{
		return false;
	}
	const auto* source = Known(*network, inst.src[0]);
	if (source == nullptr) { return false; }
	const bool cross = inst.type == Type::VPermlanex16B32;
	LaneSets   result {};
	for (uint32_t lane = 0; lane < kLanes; ++lane) { result[lane] = (*source)[PermuteSource(cross, select_low, select_high, lane)]; }
	(*network)[inst.dst.register_id] = result;
	return true;
}

bool Step(const ShaderInstruction& inst, Network* network)
{
	if (!PlainVector(inst)) { return false; }
	if (inst.type == Type::VOrB32) { return OrStep(inst, network); }
	if (inst.type == Type::VPermlane16B32 || inst.type == Type::VPermlanex16B32) { return PermuteStep(inst, network); }
	return false;
}

// V_READLANE_B32 sdst, vsrc, lane with a static lane and a register of the network.
bool NetworkRead(const ShaderInstruction& inst, const Network& network, uint64_t* lanes)
{
	uint32_t lane = 0;
	if (inst.type != Type::VReadlaneB32 || inst.src_num != 2 || inst.vop3_op_sel != 0u || !Unmodified(inst.dst) ||
	    (inst.dst.type != Operand::Sgpr && inst.dst.type != Operand::VccLo && inst.dst.type != Operand::VccHi) ||
	    !PlainRegister(inst.src[0]) || !Constant(inst.src[1], &lane) || lane >= kLanes)
	{
		return false;
	}
	const auto* sets = Known(network, inst.src[0]);
	if (sets == nullptr) { return false; }
	*lanes = (*sets)[lane];
	return true;
}

void AddRange(const ShaderOperand& operand, VectorSet* set)
{
	if (operand.type != Operand::Vgpr || operand.register_id < 0) { return; }
	const int count = std::max(operand.size, 1);
	for (int reg = operand.register_id; reg < operand.register_id + count && reg < kVgprs; ++reg) { set->set(static_cast<size_t>(reg)); }
}

// A VALU write of every lane EXEC holds, reading nothing of its destination: no accumulator, interpolation,
// lane write or relative form, no 16-bit result that could keep the other half, no DPP (a lane without a
// source keeps its old value) and no SDWA or OP_SEL destination selection.
bool OverwritesDestination(const ShaderInstruction& inst)
{
	const auto name = ShaderInstructionTypeName(inst.type);
	if (name.substr(0, 1) != "V" || name.find("16") != std::string_view::npos || inst.vop_sdwa || inst.vop3_op_sel != 0u) { return false; }
	for (const char* prefix: {"VCmp", "VMac", "VFmac", "VInterp", "VWritelane", "VReadlane", "VReadfirstlane", "VMovrel"})
	{
		if (ShaderInstructionTypeStartsWith(inst.type, prefix)) { return false; }
	}
	for (const auto& source: inst.src)
	{
		if (source.dpp) { return false; }
	}
	return inst.dst.type == Operand::Vgpr && (inst.dst2.type != Operand::Vgpr);
}

using ScalarSet = std::bitset<LaneFlow::kWords>;

// What one path knows of the network registers. A live register still holds a value the host never
// computed in every lane. A residual one was overwritten while EXEC was the reference mask R: it is right in
// every lane of R and stale only in launched lanes outside R, which the program must never let compute
// with it again. `exec_in_reference` (EXEC is a subset of R) and `in_reference` (scalar mask words that are)
// carry that obligation; `exec_is_reference` holds until EXEC is next written, so a later overwrite under
// the same EXEC shares R.
struct PathState
{
	VectorSet live;
	VectorSet residual;
	ScalarSet in_reference;
	bool      exec_in_reference = false;
	bool      exec_is_reference = false;

	bool operator==(const PathState& other) const
	{
		return live == other.live && residual == other.residual && in_reference == other.in_reference &&
		       exec_in_reference == other.exec_in_reference && exec_is_reference == other.exec_is_reference;
	}
};

// The state of two paths meeting at an instruction: a register live on either is live, and only facts known on
// both remain. The facts are about each path's own R, which the rules treat alike. A path without a residual
// register needs none of them, but has no R either, so no later overwrite may become residual against the
// other path's. Liveness only grows and knowledge only shrinks, so the walk settles.
PathState Join(const PathState& a, const PathState& b)
{
	if (a.residual.none() && b.residual.none()) { return {a.live | b.live, {}, {}, false, false}; }
	PathState joined = a.residual.any() ? a : b;
	if (a.residual.any() && b.residual.any())
	{
		joined.in_reference      = a.in_reference & b.in_reference;
		joined.exec_in_reference = a.exec_in_reference && b.exec_in_reference;
		joined.exec_is_reference = a.exec_is_reference && b.exec_is_reference;
	} else
	{
		joined.exec_is_reference = false;
	}
	joined.live     = a.live | b.live;
	joined.residual = (a.residual | b.residual) & ~joined.live;
	return joined;
}

bool ScalarWords(const ShaderOperand& operand, unsigned* first, unsigned* count)
{
	return LaneFlow::ScalarRange(operand, first, count);
}

bool InReference(const PathState& state, const ShaderOperand& operand)
{
	if (operand.type == Operand::IntegerInlineConstant && operand.constant.u == 0u) { return true; }
	if (operand.type == Operand::ExecLo && operand.size == 2) { return state.exec_in_reference; }
	unsigned first = 0;
	unsigned count = 0;
	if (operand.size != 2 || !ScalarWords(operand, &first, &count)) { return false; }
	return state.in_reference.test(first) && state.in_reference.test(first + 1u);
}

// Whether a 64-bit mask result is a subset of R: V_CMP results (inactive lanes read zero) and copies of EXEC
// taken inside R, and AND/ANDN2/OR/select/move of masks that are.
bool ResultInReference(const ShaderInstruction& inst, const PathState& state)
{
	const auto in = [&](int source) { return source < inst.src_num && InReference(state, inst.src[source]); };
	if (ShaderInstructionTypeStartsWith(inst.type, "VCmp")) { return state.exec_in_reference; }
	if (ShaderInstructionTypeChangesExec(inst.type)) { return state.exec_in_reference; } // the saved old EXEC
	switch (inst.type)
	{
		case Type::SMovB64: return in(0);
		case Type::SAndB64: return in(0) || in(1);
		case Type::SAndn2B64: return in(0);
		case Type::SOrB64:
		case Type::SCselectB64: return in(0) && in(1);
		default: return false;
	}
}

void TrackReference(const ShaderInstruction& inst, PathState* state)
{
	if (LaneFlow::WritesExecOnly(inst.type))
	{
		state->exec_is_reference = false; // EXEC &= compare stays inside R
		return;
	}
	const bool result_in = ResultInReference(inst, *state);
	if (ShaderInstructionWritesExec(inst))
	{
		bool exec_in = false;
		if (inst.dst.type == Operand::ExecLo) { exec_in = result_in; }
		if (ShaderInstructionTypeChangesExec(inst.type) && inst.dst.type != Operand::ExecLo)
		{
			// S_AND_SAVEEXEC: EXEC = S0 & EXEC; the other SAVEEXEC forms may leave R.
			exec_in = inst.type == Type::SAndSaveexecB64 && (state->exec_in_reference || InReference(*state, inst.src[0]));
		}
		state->exec_in_reference = exec_in;
		state->exec_is_reference = false;
	}
	unsigned first = 0;
	unsigned count = 0;
	// A carry or compare mask in the second destination is not followed.
	if (ScalarWords(inst.dst2, &first, &count))
	{
		for (unsigned word = first; word < first + count && word < LaneFlow::kWords; ++word) { state->in_reference.reset(word); }
	}
	if (inst.dst.type == Operand::ExecLo || inst.dst.type == Operand::ExecHi || !ScalarWords(inst.dst, &first, &count)) { return; }
	for (unsigned word = first; word < first + count && word < LaneFlow::kWords; ++word)
	{
		state->in_reference.set(word, result_in && inst.dst.size == 2);
	}
}

// A residual register may be read only where it is right: by an instruction whose result in each lane
// depends on that lane alone, while EXEC stays inside R.
bool ResidualReadAllowed(const ShaderInstruction& inst, const PathState& state)
{
	if (!state.exec_in_reference ||
	    LaneFlow::AnyPrefix(inst.type, {"VReadlane", "VWritelane", "VPermlane", "VPermlanex"}))
	{
		return false;
	}
	if (ShaderInstructionTypeStartsWith(inst.type, "Image") && !LaneFlow::IsLaneLocalImageRead(inst.type)) { return false; }
	if (ShaderInstructionTypeStartsWith(inst.type, "Ds")) { return false; }
	for (const auto& source: inst.src)
	{
		if (source.dpp) { return false; }
	}
	return true;
}

// Applies one instruction to the network registers of a path. Any mention of a live register other than
// an overwrite would observe a value the host never computed; an overwrite while EXEC holds every launched
// lane ends the register, any other overwrite leaves it residual against the EXEC of that moment.
bool Visit(const ShaderInstruction& inst, bool exec_full, PathState* state)
{
	VectorSet read;
	for (const auto& source: inst.src) { AddRange(source, &read); }
	for (const auto& address: inst.mimg_address) { AddRange(address, &read); }
	AddRange(inst.dst2, &read);
	VectorSet written;
	AddRange(inst.dst, &written);
	const bool overwrites = OverwritesDestination(inst);
	if ((read & state->live).any() || ((written & state->live).any() && !overwrites)) { return false; }
	if (((read & state->residual).any() || ((written & state->residual).any() && !overwrites)) && !ResidualReadAllowed(inst, *state))
	{
		return false;
	}
	const VectorSet killed = written & state->live;
	if (exec_full && overwrites)
	{
		state->live &= ~written;
		state->residual &= ~written;
	} else if (killed.any())
	{
		if (state->residual.none())
		{
			state->in_reference.reset();
			state->exec_in_reference = state->exec_is_reference = true;
		}
		if (!state->exec_is_reference) { return false; }
		state->live &= ~killed;
		state->residual |= killed;
	}
	TrackReference(inst, state);
	return true;
}

bool IsBranch(Type type)
{
	return type == Type::SBranch || type == Type::SCbranchScc0 || type == Type::SCbranchScc1 || type == Type::SCbranchVccz ||
	       type == Type::SCbranchVccnz || type == Type::SCbranchExecz || type == Type::SCbranchExecnz;
}

bool IndexOfPc(const ShaderCode& code, int64_t pc, uint32_t* index)
{
	const auto& instructions = code.GetInstructions();
	uint32_t    low          = 0;
	uint32_t    high         = instructions.Size();
	while (low < high)
	{
		const uint32_t middle = low + (high - low) / 2u;
		if (static_cast<int64_t>(instructions.At(middle).pc) < pc)
		{
			low = middle + 1u;
		} else
		{
			high = middle;
		}
	}
	*index = low;
	return low < instructions.Size() && static_cast<int64_t>(instructions.At(low).pc) == pc;
}

// Every path from `start` ends each network register, or leaves it residual and never lets a stale lane
// compute with it.
bool NetworkDead(const ShaderCode& code, const ShaderFragmentMaskFlow& masks, uint32_t start, const VectorSet& network)
{
	const auto&                                 instructions = code.GetInstructions();
	std::vector<std::optional<PathState>>       seen(instructions.Size());
	PathState                                   entry;
	entry.live = network;
	std::vector<std::pair<uint32_t, PathState>> pending {{start, entry}};
	while (!pending.empty())
	{
		auto [index, state] = pending.back();
		pending.pop_back();
		while (index < instructions.Size() && (state.live | state.residual).any())
		{
			auto& known = seen[index];
			if (known.has_value())
			{
				const PathState joined = Join(*known, state);
				if (joined == *known) { break; }
				state = joined;
			}
			known = state;
			const auto& inst = instructions.At(index);
			if (!Visit(inst, masks.exec_full[index], &state)) { return false; }
			if (inst.type == Type::SEndpgm) { break; }
			if (ShaderInstructionTypeStartsWith(inst.type, "SSetpc") || ShaderInstructionTypeStartsWith(inst.type, "SSwappc")) { return false; }
			if (!IsBranch(inst.type))
			{
				++index;
				continue;
			}
			uint32_t target = 0;
			if (inst.src_num < 1 || !IndexOfPc(code, static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i, &target)) { return false; }
			if (inst.type != Type::SBranch) { pending.emplace_back(index + 1u, state); }
			index = target;
		}
	}
	return true;
}

bool ProveReduction(const ShaderCode& code, const ShaderFragmentMaskFlow& masks, uint32_t entry, uint32_t restore,
                    ShaderFragmentLaneReduction* reduction)
{
	const auto&    instructions = code.GetInstructions();
	const uint32_t seed_index   = entry + 1u;
	// The region proof made the seed V_CNDMASK_B32 0, x, cond with cond inside the region's EXEC; the
	// mask flow adds that cond holds only covered lanes.
	if (!masks.live_condition[seed_index]) { return false; }
	const int seed = instructions.At(seed_index).dst.register_id;
	Network   network;
	auto&     own = network[seed];
	for (uint32_t lane = 0; lane < kLanes; ++lane) { own[lane] = uint64_t {1} << lane; }
	for (uint32_t index = seed_index + 1u; index < restore; ++index)
	{
		if (!Step(instructions.At(index), &network)) { return false; }
	}
	// The reads directly follow the restore, before anything can change the seed register.
	uint32_t cursor = restore + 1u;
	for (; cursor < instructions.Size() && !LabelAt(code, instructions.At(cursor).pc); ++cursor)
	{
		uint64_t lanes = 0;
		if (!NetworkRead(instructions.At(cursor), network, &lanes)) { break; }
		reduction->reads.push_back({cursor, lanes});
	}
	VectorSet live;
	for (const auto& [reg, sets]: network)
	{
		(void)sets;
		live.set(static_cast<size_t>(reg));
	}
	if (reduction->reads.empty() || !NetworkDead(code, masks, cursor, live)) { return false; }
	reduction->seed_index = seed_index;
	reduction->restore    = restore;
	reduction->seed       = seed;
	return true;
}

} // namespace

bool ShaderFragmentLaneReductions::IsStep(uint32_t index) const
{
	return std::any_of(reductions.begin(), reductions.end(),
	                   [index](const auto& reduction) { return index > reduction.seed_index && index < reduction.restore; });
}

const ShaderFragmentLaneReduction* ShaderFragmentLaneReductions::ReadAt(uint32_t index, uint64_t* lanes) const
{
	for (const auto& reduction: reductions)
	{
		for (const auto& read: reduction.reads)
		{
			if (read.index != index) { continue; }
			*lanes = read.lanes;
			return &reduction;
		}
	}
	return nullptr;
}

ShaderFragmentLaneReductions ShaderFindFragmentLaneReductions(const ShaderCode& code, const ShaderFragmentMaskFlow& masks)
{
	ShaderFragmentLaneReductions result;
	const auto&                  instructions = code.GetInstructions();
	if (code.GetType() != ShaderType::Pixel || !masks.lane_local || masks.exec_full.size() != instructions.Size()) { return result; }
	for (uint32_t entry = 0; entry < instructions.Size(); ++entry)
	{
		uint32_t restore = 0;
		if (instructions.At(entry).type != Type::SOrn2SaveexecB64 || !ShaderFragmentNeutralRegionEnd(code, entry, &restore)) { continue; }
		ShaderFragmentLaneReduction reduction;
		if (ProveReduction(code, masks, entry, restore, &reduction)) { result.reductions.push_back(std::move(reduction)); }
	}
	return result;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
