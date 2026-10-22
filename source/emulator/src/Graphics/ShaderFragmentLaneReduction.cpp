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

// Per guest lane (the first guest_wave_size are used): the lanes whose seed the register holds the OR of.
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

// D = S0 op S1 with S0 optionally row-shifted. A lane whose shift leaves the row keeps D, which is the unshifted
// source, or takes zero (BOUND_CTRL), the identity of OR but not of a maximum.
bool FoldStep(const ShaderInstruction& inst, ShaderLaneReductionOp op, uint32_t lanes, Network* network)
{
	if (inst.src_num != 2 || inst.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || inst.vop3_op_sel != 0u) { return false; }
	const auto& shifted = inst.src[0];
	const auto& other   = inst.src[1];
	if (!PlainRegister(other) || !Unmodified(shifted) || shifted.type != Operand::Vgpr || shifted.size != 1) { return false; }
	const auto* left  = Known(*network, shifted);
	const auto* right = Known(*network, other);
	if (left == nullptr || right == nullptr) { return false; }
	if (shifted.dpp && (!RowControl(shifted.dpp_ctrl) || shifted.dpp_row_mask != 15u || shifted.dpp_bank_mask != 15u ||
	                    inst.dst.register_id != other.register_id || (shifted.dpp_bound_ctrl && op != ShaderLaneReductionOp::Or)))
	{
		return false;
	}
	LaneSets result {};
	for (uint32_t lane = 0; lane < lanes; ++lane)
	{
		uint32_t source = lane;
		const bool read = !shifted.dpp || RowSource(shifted.dpp_ctrl, lane, &source);
		result[lane]    = (*right)[lane] | (read ? (*left)[source] : 0u);
	}
	(*network)[inst.dst.register_id] = result;
	return true;
}

// Every lane is active inside the region, so the FI and BOUND_CTRL bits (OP_SEL[1:0]) change nothing.
bool PermuteStep(const ShaderInstruction& inst, uint32_t lanes, Network* network)
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
	for (uint32_t lane = 0; lane < lanes; ++lane) { result[lane] = (*source)[PermuteSource(cross, select_low, select_high, lane)]; }
	(*network)[inst.dst.register_id] = result;
	return true;
}

bool Step(const ShaderInstruction& inst, ShaderLaneReductionOp op, uint32_t lanes, Network* network)
{
	if (!PlainVector(inst)) { return false; }
	if (inst.type == (op == ShaderLaneReductionOp::Or ? Type::VOrB32 : Type::VMaxF32)) { return FoldStep(inst, op, lanes, network); }
	if (inst.type == Type::VPermlane16B32 || inst.type == Type::VPermlanex16B32) { return PermuteStep(inst, lanes, network); }
	return false;
}

// V_READLANE_B32 sdst, vsrc, lane with a static lane of the guest wave and a register of the network. The ISA
// takes the lane select modulo the wave width; a select past it is refused.
bool NetworkRead(const ShaderInstruction& inst, const Network& network, uint32_t wave_lanes, uint64_t* lanes)
{
	uint32_t lane = 0;
	if (inst.type != Type::VReadlaneB32 || inst.src_num != 2 || inst.vop3_op_sel != 0u || !Unmodified(inst.dst) ||
	    (inst.dst.type != Operand::Sgpr && inst.dst.type != Operand::VccLo && inst.dst.type != Operand::VccHi) ||
	    !PlainRegister(inst.src[0]) || !Constant(inst.src[1], &lane) || lane >= wave_lanes)
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

bool IsBranch(Type type)
{
	return type == Type::SBranch || type == Type::SCbranchScc0 || type == Type::SCbranchScc1 || type == Type::SCbranchVccz ||
	       type == Type::SCbranchVccnz || type == Type::SCbranchExecz || type == Type::SCbranchExecnz;
}

// A straight-line instruction that names no network register and keeps EXEC: the reads after it see the
// same network.
bool Independent(const ShaderInstruction& inst, const VectorSet& network)
{
	if (IsBranch(inst.type) || inst.type == Type::SEndpgm || ShaderInstructionTypeStartsWith(inst.type, "SSetpc") ||
	    ShaderInstructionTypeStartsWith(inst.type, "SSwappc") || ShaderInstructionWritesExec(inst))
	{
		return false;
	}
	VectorSet named;
	for (const auto& source: inst.src) { AddRange(source, &named); }
	for (const auto& address: inst.mimg_address) { AddRange(address, &named); }
	AddRange(inst.dst, &named);
	AddRange(inst.dst2, &named);
	return (named & network).none();
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

// The lane mask width of the guest wave: an SGPR pair and the B64 forms in a Wave64 program, one SGPR and the
// B32 forms in a Wave32 program (its VALU lane masks narrowed to their low word).
struct MaskWidth
{
	bool wave32 = false;

	[[nodiscard]] int  Words() const { return wave32 ? 1 : 2; }
	[[nodiscard]] bool Is(Type type, Type b64, Type b32) const { return type == (wave32 ? b32 : b64); }
};

bool InReference(const PathState& state, const ShaderOperand& operand, MaskWidth width)
{
	if (operand.type == Operand::IntegerInlineConstant && operand.constant.u == 0u) { return true; }
	if (operand.type == Operand::ExecLo && operand.size == width.Words()) { return state.exec_in_reference; }
	unsigned first = 0;
	unsigned count = 0;
	if (operand.size != width.Words() || !ScalarWords(operand, &first, &count)) { return false; }
	for (unsigned word = first; word < first + count; ++word)
	{
		if (word >= LaneFlow::kWords || !state.in_reference.test(word)) { return false; }
	}
	return true;
}

// Whether a lane mask result is a subset of R: V_CMP results (inactive lanes read zero) and copies of EXEC
// taken inside R, and AND/ANDN2/OR/select/move of masks that are.
bool ResultInReference(const ShaderInstruction& inst, const PathState& state, MaskWidth width)
{
	const auto in = [&](int source) { return source < inst.src_num && InReference(state, inst.src[source], width); };
	if (ShaderInstructionTypeStartsWith(inst.type, "VCmp")) { return state.exec_in_reference; }
	if (ShaderInstructionTypeChangesExec(inst.type)) { return state.exec_in_reference; } // the saved old EXEC
	if (width.Is(inst.type, Type::SMovB64, Type::SMovB32) || width.Is(inst.type, Type::SAndn2B64, Type::SAndn2B32)) { return in(0); }
	if (width.Is(inst.type, Type::SAndB64, Type::SAndB32)) { return in(0) || in(1); }
	if (width.Is(inst.type, Type::SOrB64, Type::SOrB32) || width.Is(inst.type, Type::SCselectB64, Type::SCselectB32))
	{
		return in(0) && in(1);
	}
	return false;
}

void TrackReference(const ShaderInstruction& inst, MaskWidth width, PathState* state)
{
	if (LaneFlow::WritesExecOnly(inst.type))
	{
		state->exec_is_reference = false; // EXEC &= compare stays inside R
		return;
	}
	const bool result_in = ResultInReference(inst, *state, width);
	if (ShaderInstructionWritesExec(inst))
	{
		bool exec_in = false;
		if (inst.dst.type == Operand::ExecLo) { exec_in = result_in; }
		if (ShaderInstructionTypeChangesExec(inst.type) && inst.dst.type != Operand::ExecLo)
		{
			// S_AND_SAVEEXEC: EXEC = S0 & EXEC; the other SAVEEXEC forms may leave R.
			exec_in = width.Is(inst.type, Type::SAndSaveexecB64, Type::SAndSaveexecB32) &&
			          (state->exec_in_reference || InReference(*state, inst.src[0], width));
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
		state->in_reference.set(word, result_in && inst.dst.size == width.Words());
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
bool Visit(const ShaderInstruction& inst, bool exec_full, MaskWidth width, PathState* state)
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
	TrackReference(inst, width, state);
	return true;
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
			if (!Visit(inst, masks.exec_full[index], MaskWidth {masks.guest_wave_size == 32u}, &state)) { return false; }
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

bool ProveReduction(const ShaderCode& code, const ShaderFragmentMaskFlow& masks, uint32_t seed_index, uint32_t restore,
                    ShaderLaneReduction* reduction)
{
	const auto& instructions = code.GetInstructions();
	// The region proof made the seed V_CNDMASK_B32 0, x, cond with cond inside the region's EXEC; the
	// mask flow adds that cond holds only covered lanes.
	if (!masks.live_condition[seed_index]) { return false; }
	if (!ShaderProveLaneReductionNetwork(code, masks.guest_wave_size, seed_index, restore, reduction)) { return false; }
	VectorSet live;
	live.set(static_cast<size_t>(reduction->seed));
	for (const uint32_t step: reduction->steps) { live.set(static_cast<size_t>(instructions.At(step).dst.register_id)); }
	return NetworkDead(code, masks, reduction->reads.back().index + 1u, live);
}

} // namespace

bool ShaderProveLaneReductionNetwork(const ShaderCode& code, uint32_t guest_wave_size, uint32_t seed_index, uint32_t restore,
                                     ShaderLaneReduction* reduction)
{
	const auto& instructions = code.GetInstructions();
	if (guest_wave_size > kLanes || seed_index >= restore || restore >= instructions.Size()) { return false; }
	const int seed = instructions.At(seed_index).dst.register_id;
	Network   network;
	auto&     own = network[seed];
	for (uint32_t lane = 0; lane < guest_wave_size; ++lane) { own[lane] = uint64_t {1} << lane; }
	reduction->steps.clear();
	for (uint32_t index = seed_index + 1u; index < restore; ++index)
	{
		const auto& inst = instructions.At(index);
		if (ShaderReductionRegionScalarLoad(inst) && !ShaderInstructionWritesExec(inst)) { continue; }
		if (!Step(inst, reduction->op, guest_wave_size, &network)) { return false; }
		reduction->steps.push_back(index);
	}
	VectorSet live;
	for (const auto& [reg, sets]: network)
	{
		(void)sets;
		live.set(static_cast<size_t>(reg));
	}
	// The reads follow the restore, before anything can change the seed register; an independent
	// instruction may sit between them.
	reduction->reads.clear();
	for (uint32_t cursor = restore + 1u; cursor < instructions.Size() && !LabelAt(code, instructions.At(cursor).pc); ++cursor)
	{
		const auto& inst  = instructions.At(cursor);
		uint64_t    lanes = 0;
		if (NetworkRead(inst, network, guest_wave_size, &lanes))
		{
			reduction->reads.push_back({cursor, lanes});
			continue;
		}
		if (!Independent(inst, live)) { break; }
	}
	reduction->seed_index = seed_index;
	reduction->restore    = restore;
	reduction->seed       = seed;
	return !reduction->reads.empty();
}

bool ShaderLaneReduction::IsStep(uint32_t index) const
{
	return std::find(steps.begin(), steps.end(), index) != steps.end();
}

bool ShaderLaneReductions::IsStep(uint32_t index) const
{
	return std::any_of(reductions.begin(), reductions.end(), [index](const auto& reduction) { return reduction.IsStep(index); });
}

const ShaderLaneReduction* ShaderLaneReductions::ReadAt(uint32_t index, uint64_t* lanes) const
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

ShaderLaneReductions ShaderFindFragmentLaneReductions(const ShaderCode& code, const ShaderFragmentMaskFlow& masks)
{
	ShaderLaneReductions result;
	if (code.GetType() != ShaderType::Pixel || !masks.lane_local || masks.exec_full.size() != code.GetInstructions().Size() ||
	    (masks.guest_wave_size != 32u && masks.guest_wave_size != 64u))
	{
		return result;
	}
	// The mask flow of a Wave32 program was proven on its narrowed lane masks; the regions are too.
	const bool  wave32       = masks.guest_wave_size == 32u;
	const auto  narrowed     = wave32 ? ShaderWave32LaneMaskProgram(code) : ShaderCode {};
	const auto& program      = wave32 ? narrowed : code;
	const auto& instructions = program.GetInstructions();
	const Type  entry_type   = wave32 ? Type::SOrn2SaveexecB32 : Type::SOrn2SaveexecB64;
	for (uint32_t entry = 0; entry < instructions.Size(); ++entry)
	{
		uint32_t seed    = 0;
		uint32_t restore = 0;
		if (instructions.At(entry).type != entry_type ||
		    !ShaderFragmentNeutralRegionBounds(program, masks.guest_wave_size, entry, &seed, &restore))
		{
			continue;
		}
		ShaderLaneReduction reduction;
		if (ProveReduction(program, masks, seed, restore, &reduction)) { result.reductions.push_back(std::move(reduction)); }
	}
	return result;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
