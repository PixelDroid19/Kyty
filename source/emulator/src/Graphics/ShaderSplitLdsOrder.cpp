#include "ShaderSplitLdsOrder.h"

#include "Emulator/Graphics/ShaderComputeWaveLds.h"

#include "ShaderLaneFlow.h"
#include "ShaderMaskAnalysis.h"
#include "ShaderStorageAnalysis.h"

#include <array>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

using Type = ShaderInstructionType;

// The 24-bit multiplies and the 32-bit adds of an address stay exact below it.
constexpr int64_t kExactLimit = 1 << 24;
// Past this many accesses every one keeps its own order: the pairing is quadratic.
constexpr size_t kMaxAccesses = 64;
// A guest wave whose lanes all exist: no host lane runs outside the thread group.
constexpr uint32_t kWaveLanes = 64;

// k + c[0] * x + c[1] * y + c[2] * z over the thread coordinates, the same expression in every lane.
struct Affine
{
	bool    known = false;
	int64_t c[3]  = {0, 0, 0};
	int64_t k     = 0;

	[[nodiscard]] bool IsConstant() const { return c[0] == 0 && c[1] == 0 && c[2] == 0; }
};

using Vgprs = std::array<Affine, LaneFlow::kVgprs>;

struct Access
{
	uint32_t index = 0;
	bool     write = true;
	// Bytes some lane may touch: [low, high). With a stride, lane n touches `bytes` at low + stride * n.
	bool    ranged = false;
	int64_t low    = 0;
	int64_t high   = 0;
	int64_t stride = 0;
	int64_t bytes  = 0;
};

bool MayConflict(const Access& a, const Access& b)
{
	if (!a.write && !b.write) { return false; }
	if (!a.ranged || !b.ranged) { return true; }
	if (a.high <= b.low || b.high <= a.low) { return false; }
	const bool own_slot = a.stride != 0 && a.stride == b.stride && a.low == b.low && a.bytes <= a.stride && b.bytes <= b.stride;
	return !own_slot;
}

bool IsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && !operand.dpp;
}

bool WritesExec(const ShaderInstruction& inst)
{
	const auto covers = [](const ShaderOperand& operand)
	{
		unsigned first = 0;
		unsigned count = 0;
		return LaneFlow::ScalarRange(operand, &first, &count) && first + count > LaneFlow::kExecLoWord && first < LaneFlow::kExecLoWord + 2u;
	};
	return covers(inst.dst) || covers(inst.dst2) || LaneFlow::WritesExecOnly(inst.type) ||
	       ShaderInstructionTypeName(inst.type).find("Saveexec") != std::string_view::npos;
}

// EXEC = -1, or EXEC | ~EXEC.
bool SetsEveryLane(const ShaderInstruction& inst)
{
	const auto& source = inst.src[0];
	if (inst.type == Type::SOrn2SaveexecB64) { return source.type == ShaderOperandType::ExecLo && source.size == 2; }
	return inst.type == Type::SMovB64 && inst.dst.type == ShaderOperandType::ExecLo && inst.dst.size == 2 &&
	       ((source.type == ShaderOperandType::IntegerInlineConstant && source.constant.i == -1) ||
	        (source.type == ShaderOperandType::LiteralConstant && source.constant.u == 0xffffffffu));
}

bool WritesVgpr(const ShaderInstruction& inst, unsigned vgpr)
{
	for (const auto* operand: {&inst.dst, &inst.dst2})
	{
		unsigned first = 0;
		unsigned count = 0;
		if (LaneFlow::VgprRange(*operand, &first, &count) && vgpr >= first && vgpr < first + count) { return true; }
	}
	return false;
}

Affine ValueOf(const ShaderOperand& operand, const Vgprs& vgprs)
{
	Affine value;
	if (!IsPlain(operand)) { return value; }
	if (operand.type == ShaderOperandType::Vgpr && operand.size == 1 && operand.register_id >= 0 && operand.register_id < LaneFlow::kVgprs)
	{
		return vgprs[static_cast<size_t>(operand.register_id)];
	}
	const bool integer = operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant;
	value.k     = operand.constant.u;
	value.known = integer && value.k < kExactLimit;
	return value;
}

// a * b + c with a constant factor, unknown once the largest lane value leaves the exact range.
Affine MulAdd(const Affine& a, const Affine& b, const Affine& c, const uint32_t threads[3])
{
	const auto& factor = a.IsConstant() ? a : b;
	const auto& value  = a.IsConstant() ? b : a;
	Affine      result;
	if (!a.known || !b.known || !c.known || !factor.IsConstant()) { return result; }
	result.k       = value.k * factor.k + c.k;
	int64_t largest = result.k;
	for (int axis = 0; axis < 3; axis++)
	{
		result.c[axis] = value.c[axis] * factor.k + c.c[axis];
		if (result.c[axis] >= kExactLimit) { return result; }
		largest += result.c[axis] * (static_cast<int64_t>(threads[axis]) - 1);
	}
	result.known = result.k < kExactLimit && largest < kExactLimit;
	return result;
}

// The value a plain integer instruction gives its destination in every lane it runs in.
Affine Evaluate(const ShaderInstruction& inst, const Vgprs& vgprs, const uint32_t threads[3])
{
	const Affine one {true, {0, 0, 0}, 1};
	const Affine zero {true, {0, 0, 0}, 0};
	const auto   value = [&](int source) { return ValueOf(inst.src[source], vgprs); };
	const auto   shift = [&](int source)
	{
		auto bits  = value(source);
		bits.known = bits.known && bits.IsConstant() && bits.k < 24;
		bits.k     = bits.known ? int64_t {1} << bits.k : 0;
		return bits;
	};
	if (inst.vop_sdwa || inst.vop3_omod != 0u || !IsPlain(inst.dst)) { return {}; }
	switch (inst.type)
	{
		case Type::VMovB32: return inst.src_num == 1 ? MulAdd(value(0), one, zero, threads) : Affine {};
		case Type::VMadU32U24: return inst.src_num == 3 ? MulAdd(value(0), value(1), value(2), threads) : Affine {};
		case Type::VLshlrevB32: return inst.src_num == 2 ? MulAdd(value(1), shift(0), zero, threads) : Affine {};
		case Type::VLshlAddU32: return inst.src_num == 3 ? MulAdd(value(0), shift(1), value(2), threads) : Affine {};
		default: return {};
	}
}

bool EndsBlock(Type type)
{
	return ShaderInstructionHasStaticBranchTarget(type) || type == Type::SEndpgm;
}

// What each VGPR holds in every lane for the whole program: its value when the entry block ends, kept only
// when nothing writes the register after it. The entry block runs every lane while EXEC is the entry mask or
// was set to all lanes.
Vgprs ProgramWideValues(const ShaderCode& code, const std::vector<bool>& is_target, const uint32_t threads[3], int thread_ids)
{
	const auto& instructions = code.GetInstructions();
	Vgprs       values {};
	// A loop back to the entry would run its block again on other inputs.
	if (instructions.IsEmpty() || is_target[0]) { return values; }
	for (int axis = 0; axis < thread_ids && axis < 3; axis++)
	{
		values[static_cast<size_t>(axis)].known   = true;
		values[static_cast<size_t>(axis)].c[axis] = 1;
	}
	bool     every_lane = true;
	uint32_t index      = 0;
	for (; index < instructions.Size(); index++)
	{
		const auto& inst = instructions.At(index);
		if (EndsBlock(inst.type) || is_target[index]) { break; }
		if (WritesExec(inst)) { every_lane = SetsEveryLane(inst); }
		const auto value = every_lane && inst.dst.size == 1 ? Evaluate(inst, values, threads) : Affine {};
		for (unsigned vgpr = 0; vgpr < static_cast<unsigned>(LaneFlow::kVgprs); vgpr++)
		{
			if (WritesVgpr(inst, vgpr)) { values[vgpr] = inst.dst2.type == ShaderOperandType::Unknown ? value : Affine {}; }
		}
	}
	for (; index < instructions.Size(); index++)
	{
		const auto& inst = instructions.At(index);
		for (unsigned vgpr = 0; vgpr < static_cast<unsigned>(LaneFlow::kVgprs); vgpr++)
		{
			if (WritesVgpr(inst, vgpr)) { values[vgpr] = {}; }
		}
	}
	return values;
}

// The constant a V_MOV_B32 of the same straight-line block gave `vgpr` under the EXEC the access runs with.
Affine BlockConstant(const ShaderCode& code, const std::vector<bool>& is_target, uint32_t access, unsigned vgpr)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t at = access; at > 0 && !is_target[at];)
	{
		const auto& inst = instructions.At(--at);
		if (EndsBlock(inst.type) || WritesExec(inst)) { break; }
		if (WritesVgpr(inst, vgpr))
		{
			const bool move = inst.type == Type::VMovB32 && inst.src_num == 1 && inst.dst.size == 1 && !inst.vop_sdwa && IsPlain(inst.dst);
			return move ? ValueOf(inst.src[0], Vgprs {}) : Affine {};
		}
	}
	return {};
}

// Dwords a one-range access covers at ADDR + ds_offset; 0 for the two-offset and unsupported forms.
int RangeDwords(const ShaderInstruction& inst)
{
	if (!ShaderLdsMemoryInstructionSupported(inst) || LaneFlow::AnyPrefix(inst.type, {"DsRead2", "DsWrite2"})) { return 0; }
	if (inst.type == Type::DsWriteB32) { return inst.src[1].size; }
	return inst.type == Type::DsReadB32 ? inst.dst.size : 1;
}

// Stride of an address that is stride * (x + size_x * (y + size_y * z)) + k: one slot per lane of the group.
int64_t LaneStride(const Affine& address, const uint32_t threads[3])
{
	int64_t stride = 0;
	int64_t weight = 1;
	for (int axis = 0; axis < 3; axis++)
	{
		if (threads[axis] > 1)
		{
			const bool fits = address.c[axis] > 0 && address.c[axis] % weight == 0 && (stride == 0 || stride == address.c[axis] / weight);
			if (!fits) { return 0; }
			stride = address.c[axis] / weight;
		}
		weight *= threads[axis];
	}
	return stride;
}

void SetRange(const ShaderInstruction& inst, const Affine& address, const uint32_t threads[3], Access* access)
{
	const int64_t lanes  = static_cast<int64_t>(threads[0]) * threads[1] * threads[2];
	const int64_t stride = address.IsConstant() ? 0 : LaneStride(address, threads);
	access->bytes        = int64_t {4} * RangeDwords(inst);
	access->low          = address.k + inst.ds_offset;
	access->high         = access->low + stride * (lanes - 1) + access->bytes;
	access->stride       = stride;
	// An unaligned address is not accessed at all.
	const bool aligned = access->low % 4 == 0 && stride % 4 == 0;
	const bool laned   = stride != 0 && lanes % kWaveLanes == 0;
	access->ranged     = address.known && access->bytes != 0 && aligned && (address.IsConstant() || laned);
}

std::vector<Access> FindAccesses(const ShaderCode& code, const std::vector<bool>& is_target, const uint32_t threads[3], int thread_ids)
{
	const auto&         instructions = code.GetInstructions();
	const auto          values       = ProgramWideValues(code, is_target, threads, thread_ids);
	std::vector<Access> accesses;
	for (uint32_t index = 0; index < instructions.Size(); index++)
	{
		const auto& inst = instructions.At(index);
		if (!ShaderSplitLdsOrdered(inst.type)) { continue; }
		Access access;
		access.index = index;
		access.write = !LaneFlow::StartsWith(inst.type, "DsRead");
		if (RangeDwords(inst) != 0)
		{
			const auto vgpr    = static_cast<unsigned>(inst.src[0].register_id);
			const auto address = values[vgpr].known ? values[vgpr] : BlockConstant(code, is_target, index, vgpr);
			SetRange(inst, address, threads, &access);
		}
		accesses.push_back(access);
	}
	return accesses;
}

struct Flow
{
	std::vector<std::vector<uint32_t>> successors;
	std::vector<bool>                  barrier;
};

// Some path runs from the end of `from` to the start of `to` through no order and no barrier.
bool Unordered(const Flow& flow, const ShaderSplitLdsOrder& order, uint32_t from, uint32_t to)
{
	if (order.after[from]) { return false; }
	std::vector<bool>     seen(flow.successors.size(), false);
	std::vector<uint32_t> pending = flow.successors[from];
	while (!pending.empty())
	{
		const uint32_t at = pending.back();
		pending.pop_back();
		if (seen[at] || order.before[at]) { continue; }
		seen[at] = true;
		if (at == to) { return true; }
		if (flow.barrier[at] || order.after[at]) { continue; }
		pending.insert(pending.end(), flow.successors[at].begin(), flow.successors[at].end());
	}
	return false;
}

// The group sizes are the guest's: keep every product of the analysis inside 64 bits.
bool GroupFits(const uint32_t threads[3])
{
	int64_t lanes = 1;
	for (int axis = 0; axis < 3; axis++)
	{
		if (threads[axis] == 0 || threads[axis] > kExactLimit) { return false; }
		lanes *= threads[axis];
		if (lanes > kExactLimit) { return false; }
	}
	return true;
}

} // namespace

bool ShaderSplitLdsOrdered(ShaderInstructionType type)
{
	// ds_swizzle and ds_bpermute touch no LDS and rendezvous on their own.
	return LaneFlow::StartsWith(type, "Ds") && type != Type::DsSwizzleB32 && type != Type::DsBpermuteB32;
}

ShaderSplitLdsOrder ShaderSplitLdsOrderOf(const ShaderCode& code, const uint32_t threads[3], int thread_ids)
{
	const auto&         instructions = code.GetInstructions();
	const uint32_t      count        = instructions.Size();
	ShaderSplitLdsOrder order;
	order.before.assign(count, false);
	order.after.assign(count, false);
	Flow flow;
	bool resolved = ShaderInstructionSuccessors(code, &flow.successors) && GroupFits(threads);
	std::vector<bool> is_target(count, false);
	// Loops an instruction lies in: a rendezvous there costs once per iteration.
	std::vector<uint32_t> depth(count, 0);
	for (uint32_t index = 0; resolved && index < count; index++)
	{
		const auto type = instructions.At(index).type;
		// The scheduling hints parse as SBarrier records with an operand and emit nothing.
		flow.barrier.push_back(type == Type::SBarrier && instructions.At(index).format == ShaderInstructionFormat::Empty);
		// An indexed move writes a register the instruction does not name.
		resolved = !LaneFlow::AnyPrefix(type, {"VMovrel", "SMovrel"});
		if (!ShaderInstructionHasStaticBranchTarget(type)) { continue; }
		const uint32_t target = flow.successors[index][0];
		is_target[target]     = true;
		for (uint32_t inside = target; inside <= index; inside++) { depth[inside]++; }
	}
	const auto accesses = resolved ? FindAccesses(code, is_target, threads, thread_ids) : std::vector<Access> {};
	if (!resolved || accesses.size() > kMaxAccesses)
	{
		for (uint32_t index = 0; index < count; index++) { order.after[index] = ShaderSplitLdsOrdered(instructions.At(index).type); }
		return order;
	}
	// Every ordered pair, the access itself across a loop included. The rendezvous goes after the first
	// access when that keeps it out of the second one's loop, else before the second, where it also orders
	// every other access that reaches it.
	for (const auto& second: accesses)
	{
		for (auto first = accesses.rbegin(); first != accesses.rend(); ++first)
		{
			if (!MayConflict(*first, second) || !Unordered(flow, order, first->index, second.index)) { continue; }
			if (depth[first->index] < depth[second.index]) { order.after[first->index] = true; }
			else { order.before[second.index] = true; }
		}
	}
	return order;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
