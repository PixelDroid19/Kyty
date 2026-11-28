#include "ShaderParseInternal.h"

#include "ShaderLaneFlow.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

using namespace LaneFlow;

namespace {

using Type    = ShaderInstructionType;
using Operand = ShaderOperandType;

bool SameRegister(const ShaderOperand& a, const ShaderOperand& b)
{
	return a.type == b.type && a.register_id == b.register_id && a.size == b.size && !a.negate && !a.absolute && !b.negate &&
	       !b.absolute && !a.dpp && !b.dpp;
}

// A lane mask is one word in wave32 (EXEC_LO) and two in wave64.
bool IsSgprMask(const ShaderOperand& op, int words)
{
	return op.type == Operand::Sgpr && op.size == words;
}

bool IsExec(const ShaderOperand& op, int words)
{
	return op.type == Operand::ExecLo && op.size == words;
}

bool Covers(const ShaderOperand& op, unsigned word)
{
	unsigned first = 0;
	unsigned count = 0;
	return ScalarRange(op, &first, &count) && word >= first && word < first + count;
}

// M0 also feeds LDS/GDS addressing, interpolation, messages and the relative moves without naming it. A relative
// move this pass lowered reads its lane's index from src[1] instead.
bool ReadsM0Implicitly(const ShaderInstruction& inst)
{
	if (AnyPrefix(inst.type, {"VMovrels", "VMovreld"}) && inst.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1)
	{
		return false;
	}
	return AnyPrefix(inst.type, {"Ds", "VInterp", "SSendmsg", "VMovrel", "SMovrel"});
}

bool Reads(const ShaderInstruction& inst, unsigned word)
{
	for (int source = 0; source < inst.src_num; ++source)
	{
		if (Covers(inst.src[source], word)) { return true; }
	}
	return word == kM0Word && ReadsM0Implicitly(inst);
}

bool Writes(const ShaderInstruction& inst, unsigned word)
{
	return Covers(inst.dst, word) || Covers(inst.dst2, word);
}

bool WritesMask(const ShaderInstruction& inst, unsigned first, int words)
{
	return Writes(inst, first) || (words == 2 && Writes(inst, first + 1u));
}

// V_CMPX and S_*_SAVEEXEC replace EXEC without naming it as their destination.
bool WritesExec(const ShaderInstruction& inst)
{
	return Writes(inst, kExecLoWord) || Writes(inst, kExecLoWord + 1u) || WritesExecOnly(inst.type) ||
	       ShaderInstructionTypeName(inst.type).find("Saveexec") != std::string_view::npos;
}

bool IsBranch(Type type)
{
	return type == Type::SBranch || StartsWith(type, "SCbranch");
}

bool DeadFromChecked(const ShaderCode& code, uint32_t from, unsigned word, std::set<uint32_t>* started);

// A back edge carries the live value to `target_pc`, before or inside the pass: check the paths from there too.
bool DeadAlongBackEdge(const ShaderCode& code, uint32_t target_pc, unsigned word, std::set<uint32_t>* started)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		if (instructions.At(index).pc == target_pc) { return DeadFromChecked(code, index, word, started); }
	}
	return false;
}

// No path from instruction `from` reads `word` before writing it. One pass in program order sees every path that
// carries the value forward; a back edge that carries it where the pass did not is followed from its target. A pass
// already started from that instruction covers it, so loops end.
bool DeadFromChecked(const ShaderCode& code, uint32_t from, unsigned word, std::set<uint32_t>* started)
{
	if (!started->insert(from).second) { return true; }
	const auto&                  instructions = code.GetInstructions();
	std::map<uint32_t, bool>     reaching; // live value arriving at a branch target
	std::map<uint32_t, bool>     live_at;  // whether the pass carried the value into each instruction
	bool                         live = true;
	for (uint32_t index = from; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (const auto target = reaching.find(inst.pc); target != reaching.end()) { live = live || target->second; }
		live_at[inst.pc] = live;
		if (live && Reads(inst, word)) { return false; }
		if (Writes(inst, word)) { live = false; }
		if (IsBranch(inst.type))
		{
			const int64_t target = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
			if (target <= static_cast<int64_t>(inst.pc))
			{
				const auto carried = live_at.find(static_cast<uint32_t>(target));
				if (live && (carried == live_at.end() || !carried->second) &&
				    (target < 0 || !DeadAlongBackEdge(code, static_cast<uint32_t>(target), word, started)))
				{
					return false;
				}
			} else
			{
				reaching[static_cast<uint32_t>(target)] = reaching[static_cast<uint32_t>(target)] || live;
			}
			if (inst.type == Type::SBranch) { live = false; }
		} else if (AnyPrefix(inst.type, {"SSetpc", "SSwappc"}))
		{
			if (live) { return false; }
		} else if (StartsWith(inst.type, "SEndpgm"))
		{
			live = false;
		}
	}
	return true;
}

bool DeadFrom(const ShaderCode& code, uint32_t from, unsigned word)
{
	std::set<uint32_t> started;
	return DeadFromChecked(code, from, word, &started);
}

bool LabelInRange(const ShaderCode& code, uint32_t first_pc, uint32_t last_pc, uint32_t except_src)
{
	for (const auto& label: code.GetLabels())
	{
		if (!label.IsDisabled() && label.GetSrc() != except_src && label.GetDst() >= first_pc && label.GetDst() <= last_pc)
		{
			return true;
		}
	}
	return false;
}

// Values a relative move's lane index can hold, from a forward scan of the straight-line code that reaches the move.
// Each VGPR keeps its values with the EXEC version they were written under. A version names a lane set: a copy keeps
// its source's version, AND, ANDN2, V_CMPX and S_AND_SAVEEXEC derive a child holding only lanes of its parent, and any
// other write starts a new one. The values hold for a later EXEC only when it descends from that version: a lane
// outside it could hold an older value.
constexpr uint32_t kLaneValueSetMax = 32;

struct LaneValues
{
	bool                    known = false;
	std::array<uint32_t, 2> exec {};
	std::vector<uint32_t>   values;
};

struct LaneScan
{
	std::vector<uint32_t>          parent;
	std::array<uint32_t, kWords>   word {};
	std::array<LaneValues, kVgprs> vgpr;

	LaneScan() { Reset(); }

	uint32_t Version(uint32_t of = UINT32_MAX)
	{
		parent.push_back(of);
		return static_cast<uint32_t>(parent.size() - 1u);
	}
	void Reset()
	{
		for (auto& version: word) { version = Version(); }
		vgpr.fill({});
	}
	[[nodiscard]] std::array<uint32_t, 2> Exec() const { return {word[kExecLoWord], word[kExecLoWord + 1u]}; }
	[[nodiscard]] bool                    Within(const std::array<uint32_t, 2>& exec, const std::array<uint32_t, 2>& ancestor) const
	{
		for (unsigned half = 0; half < 2u; ++half)
		{
			uint32_t version = exec[half];
			while (version != UINT32_MAX && version != ancestor[half]) { version = parent[version]; }
			if (version == UINT32_MAX) { return false; }
		}
		return true;
	}
};

bool IsLiteral(const ShaderOperand& op)
{
	return (op.type == Operand::LiteralConstant || op.type == Operand::IntegerInlineConstant) && !op.negate && !op.absolute &&
	       !op.dpp && op.multiplier == 1.0f;
}

bool OperandValues(const LaneScan& scan, const ShaderOperand& op, const std::array<uint32_t, 2>& exec, std::vector<uint32_t>* values)
{
	if (IsLiteral(op))
	{
		*values = {op.constant.u};
		return true;
	}
	if (op.type != Operand::Vgpr || op.size != 1 || op.register_id < 0 || op.register_id >= kVgprs || op.negate || op.absolute ||
	    op.dpp || op.multiplier != 1.0f)
	{
		return false;
	}
	const auto& lane = scan.vgpr[op.register_id];
	if (!lane.known || !scan.Within(exec, lane.exec)) { return false; }
	*values = lane.values;
	return true;
}

// Values a single-VGPR definition produces under |exec|: moves, bit-field and mask extracts, and products and shifts
// of known values (32-bit wrap). Any other definition is unknown.
bool DefinitionValues(const LaneScan& scan, const ShaderInstruction& inst, const std::array<uint32_t, 2>& exec,
                      std::vector<uint32_t>* values)
{
	if (inst.vop_sdwa || inst.dst.clamp || inst.dst.multiplier != 1.0f) { return false; }
	if (inst.type == Type::VMovB32) { return inst.src_num == 1 && OperandValues(scan, inst.src[0], exec, values); }
	if (inst.type == Type::VBfeU32)
	{
		if (inst.src_num != 3 || !IsLiteral(inst.src[2]) || (1u << (inst.src[2].constant.u & 31u)) > kLaneValueSetMax) { return false; }
		for (uint32_t value = 0; value < (1u << (inst.src[2].constant.u & 31u)); ++value) { values->push_back(value); }
		return true;
	}
	if (inst.src_num != 2) { return false; }
	if (inst.type == Type::VAndB32)
	{
		const auto* mask = IsLiteral(inst.src[0]) ? &inst.src[0] : IsLiteral(inst.src[1]) ? &inst.src[1] : nullptr;
		if (mask == nullptr || mask->constant.u >= kLaneValueSetMax) { return false; }
		for (uint32_t value = 0; value <= mask->constant.u; ++value)
		{
			if ((value & mask->constant.u) == value) { values->push_back(value); }
		}
		return true;
	}
	std::vector<uint32_t> a;
	std::vector<uint32_t> b;
	if (!AnyPrefix(inst.type, {"VMulU32U24", "VMulLoU32", "VLshlrevB32", "VLshrrevB32"}) || !OperandValues(scan, inst.src[0], exec, &a) ||
	    !OperandValues(scan, inst.src[1], exec, &b))
	{
		return false;
	}
	for (const auto x: a)
	{
		for (const auto y: b)
		{
			uint32_t value = 0;
			switch (inst.type)
			{
				case Type::VMulU32U24: value = (x & 0xffffffu) * (y & 0xffffffu); break;
				case Type::VMulLoU32: value = x * y; break;
				case Type::VLshlrevB32: value = y << (x & 31u); break;
				default: value = y >> (x & 31u); break;
			}
			if (std::find(values->begin(), values->end(), value) == values->end()) { values->push_back(value); }
			if (values->size() > kLaneValueSetMax) { return false; }
		}
	}
	return true;
}

void WriteLane(LaneScan* scan, int reg, const std::vector<uint32_t>* values, const std::array<uint32_t, 2>& exec)
{
	auto& lane = scan->vgpr[reg];
	if (values == nullptr)
	{
		lane = {};
		return;
	}
	if (lane.known && lane.exec != exec && scan->Within(exec, lane.exec))
	{
		// Only some lanes of the earlier version change: those hold a new value, the rest the old one.
		for (const auto value: *values)
		{
			if (std::find(lane.values.begin(), lane.values.end(), value) == lane.values.end()) { lane.values.push_back(value); }
		}
		if (lane.values.size() > kLaneValueSetMax) { lane = {}; }
		return;
	}
	lane = {true, exec, *values};
}

void StepVgprs(LaneScan* scan, const ShaderInstruction& inst)
{
	const auto exec = scan->Exec();
	if (inst.type == Type::VMovreldB32 || inst.type == Type::VMovrelsdB32)
	{
		// A lowered move writes VGPR[base + its lane's index]; any other relative write may reach every VGPR from its base up.
		std::vector<uint32_t> offsets;
		const bool lowered = inst.type == Type::VMovreldB32 && inst.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1 &&
		                     inst.src_num == 2 && OperandValues(*scan, inst.src[1], exec, &offsets);
		for (int reg = std::max(inst.dst.register_id, 0); reg < kVgprs; ++reg)
		{
			const auto offset = static_cast<uint32_t>(reg - inst.dst.register_id);
			if (!lowered || std::find(offsets.begin(), offsets.end(), offset) != offsets.end()) { WriteLane(scan, reg, nullptr, exec); }
		}
		return;
	}
	unsigned first = 0;
	unsigned count = 0;
	const bool second = VgprRange(inst.dst2, &first, &count);
	for (unsigned reg = 0; second && reg < count; ++reg) { WriteLane(scan, static_cast<int>(first + reg), nullptr, exec); }
	if (!VgprRange(inst.dst, &first, &count)) { return; }
	std::vector<uint32_t> values;
	const bool            known = count == 1u && !second && DefinitionValues(*scan, inst, exec, &values);
	for (unsigned reg = 0; reg < count; ++reg) { WriteLane(scan, static_cast<int>(first + reg), known ? &values : nullptr, exec); }
}

void StepMasks(LaneScan* scan, const ShaderInstruction& inst)
{
	unsigned   first  = 0;
	unsigned   count  = 0;
	unsigned   from   = 0;
	unsigned   width  = 0;
	const bool mask   = ScalarRange(inst.dst, &first, &count) && count <= 2u && inst.dst2.type == Operand::Unknown;
	const bool source = inst.src_num > 0 && ScalarRange(inst.src[0], &from, &width) && width == count;
	const bool copy   = (inst.type == Type::SMovB32 || inst.type == Type::SMovB64) && inst.src_num == 1;
	const bool narrow = AnyPrefix(inst.type, {"SAndB32", "SAndB64", "SAndn2B32", "SAndn2B64"}) && inst.src_num == 2;
	std::array<uint32_t, 2> next {};
	if (mask && source && (copy || narrow))
	{
		for (unsigned half = 0; half < count; ++half) { next[half] = copy ? scan->word[from + half] : scan->Version(scan->word[from + half]); }
		for (unsigned half = 0; half < count; ++half) { scan->word[first + half] = next[half]; }
	} else if (mask && (inst.type == Type::SAndSaveexecB32 || inst.type == Type::SAndSaveexecB64))
	{
		for (unsigned half = 0; half < count; ++half)
		{
			scan->word[first + half]            = scan->word[kExecLoWord + half];
			scan->word[kExecLoWord + half] = scan->Version(scan->word[kExecLoWord + half]);
		}
	} else if (WritesExecOnly(inst.type))
	{
		for (unsigned half = 0; half < 2u; ++half) { scan->word[kExecLoWord + half] = scan->Version(scan->word[kExecLoWord + half]); }
		return;
	} else
	{
		for (const auto* op: {&inst.dst, &inst.dst2})
		{
			for (unsigned half = 0; ScalarRange(*op, &first, &count) && half < count; ++half) { scan->word[first + half] = scan->Version(); }
		}
		if (WritesExec(inst))
		{
			for (unsigned half = 0; half < 2u; ++half) { scan->word[kExecLoWord + half] = scan->Version(); }
		}
	}
	if (WritesScc(inst.type)) { scan->word[kSccWord] = scan->Version(); }
}

bool IsLabelTarget(const ShaderCode& code, uint32_t pc)
{
	return code.GetLabels().Contains(pc, [](auto label, auto target) { return !label.IsDisabled() && label.GetDst() == target; });
}

// Values of |vgpr| in every lane active after |last| runs, along the straight-line code that reaches it.
bool LaneValuesAfter(const ShaderCode& code, uint32_t last, int vgpr, std::vector<uint32_t>* values)
{
	const auto& instructions = code.GetInstructions();
	if (vgpr < 0 || vgpr >= kVgprs || last >= instructions.Size()) { return false; }
	uint32_t start = last;
	while (start > 0 && !IsLabelTarget(code, instructions.At(start).pc)) { --start; }
	LaneScan scan;
	for (uint32_t index = start; index <= last; ++index)
	{
		const auto& inst = instructions.At(index);
		if (AnyPrefix(inst.type, {"SSetpc", "SSwappc"}))
		{
			scan.Reset();
			continue;
		}
		StepVgprs(&scan, inst);
		StepMasks(&scan, inst);
	}
	const auto& lane = scan.vgpr[vgpr];
	if (!lane.known || !scan.Within(scan.Exec(), lane.exec)) { return false; }
	*values = lane.values;
	return true;
}

// Whether a move of the loop at |head| can write its index register. A lane's index keeps its value on loop entry
// until its own moves run (the moves of other lanes write their own registers), so a destination-relative move
// misses the index unless an entry value of the index lands it there.
bool MoveCanWriteIndex(const ShaderCode& code, uint32_t head, const ShaderInstruction& move, const ShaderOperand& index)
{
	if (move.type == Type::VMovrelsB32) { return move.dst.register_id == index.register_id; }
	if (move.dst.register_id > index.register_id) { return false; }
	std::vector<uint32_t> values;
	if (head == 0 || code.GetInstructions().At(head - 1u).type == Type::SBranch || !LaneValuesAfter(code, head - 1u, index.register_id, &values))
	{
		return true;
	}
	return std::find(values.begin(), values.end(), static_cast<uint32_t>(index.register_id - move.dst.register_id)) != values.end();
}

struct Waterfall
{
	uint32_t head = 0; // V_READFIRSTLANE
	uint32_t moves_end = 0; // the EXEC update after the relative moves and the REST update
	uint32_t branch = 0; // S_CBRANCH_SCC1 back to the head
	ShaderOperand index;
	bool wave32 = false; // the masks are one word: the loop means this only in a wave32 program
};

// A loop that hands each distinct per-lane index to M0 in turn and moves VGPR[base + M0] for the lanes holding it:
//
//   s_mov_b64 rest, exec            ; or the _b32 forms on EXEC_LO in a wave32 program
// head:
//   v_readfirstlane_b32 s, index
//   v_cmpx_eq_u32 s, index          ; EXEC = remaining lanes with this index
//   s_mov_b32 m0, s
//   v_movrels_b32 dst, base ...     ; dst = VGPR[base + m0], or
//   v_movreld_b32 base, src ...     ; VGPR[base + m0] = src
//   s_andn2_b64 rest, rest, exec    ; also scheduled between the moves
//   s_mov_b64 exec, rest
//   s_cbranch_scc1 head
//
// Every lane active at the head ends with the moves applied at its own index, however the lanes are grouped into
// waves, and the loop leaves REST and EXEC empty and SCC clear. A lane's index is read once, before its moves, so no
// move may follow one that can overwrite it (MoveCanWriteIndex). Only S and M0 keep a value chosen by one lane, so
// both must be dead after the loop.
bool MatchWaterfall(const ShaderCode& code, uint32_t head, Waterfall* out)
{
	const auto& instructions = code.GetInstructions();
	const auto  at           = [&](uint32_t index) -> const ShaderInstruction* {
        return index < instructions.Size() ? &instructions.At(index) : nullptr;
	};
	const auto* first   = at(head);
	const auto* compare = at(head + 1u);
	const auto* m0      = at(head + 2u);
	if (first == nullptr || compare == nullptr || m0 == nullptr || first->type != Type::VReadfirstlaneB32 || first->src_num != 1 ||
	    first->src[0].type != Operand::Vgpr || first->src[0].size != 1 || first->dst.type != Operand::Sgpr || first->dst.size != 1)
	{
		return false;
	}
	const auto& scalar = first->dst;
	const auto& index  = first->src[0];
	const bool  compare_ok =
	    compare->type == Type::VCmpxEqU32 && compare->src_num == 2 && !compare->vop_sdwa &&
	    ((SameRegister(compare->src[0], scalar) && SameRegister(compare->src[1], index)) ||
	     (SameRegister(compare->src[0], index) && SameRegister(compare->src[1], scalar)));
	if (!compare_ok || m0->type != Type::SMovB32 || m0->dst.type != Operand::M0 || m0->src_num != 1 || !SameRegister(m0->src[0], scalar))
	{
		return false;
	}

	uint32_t next          = head + 3u;
	uint32_t moves         = 0;
	uint32_t andn2_index   = UINT32_MAX;
	bool     index_written = false;
	for (const ShaderInstruction* move = at(next); move != nullptr; move = at(++next))
	{
		// The REST update reads EXEC and writes REST and SCC, none of which a move touches, so it may sit among them.
		if (moves > 0 && andn2_index == UINT32_MAX && (move->type == Type::SAndn2B64 || move->type == Type::SAndn2B32))
		{
			andn2_index = next;
			continue;
		}
		if (move->type != Type::VMovrelsB32 && move->type != Type::VMovreldB32) { break; }
		if (index_written || move->src_num != 1 || move->vop_sdwa || move->dst.type != Operand::Vgpr || move->dst.size != 1 ||
		    move->src[0].type != Operand::Vgpr || move->src[0].size != 1 || move->src[0].dpp || move->src[0].negate ||
		    move->src[0].absolute || move->dst.clamp)
		{
			return false;
		}
		index_written = MoveCanWriteIndex(code, head, *move, index);
		++moves;
	}
	const auto* andn2  = andn2_index == UINT32_MAX ? nullptr : at(andn2_index);
	const auto* exec   = at(next);
	const auto* branch = at(next + 1u);
	const int   words  = andn2 != nullptr && andn2->type == Type::SAndn2B32 ? 1 : 2;
	const auto  mov    = words == 1 ? Type::SMovB32 : Type::SMovB64;
	if (moves == 0 || andn2 == nullptr || exec == nullptr || branch == nullptr ||
	    (andn2->type != Type::SAndn2B64 && andn2->type != Type::SAndn2B32) || andn2->src_num != 2 || !IsSgprMask(andn2->dst, words) ||
	    !SameRegister(andn2->src[0], andn2->dst) || !IsExec(andn2->src[1], words) || exec->type != mov || !IsExec(exec->dst, words) ||
	    exec->src_num != 1 || !SameRegister(exec->src[0], andn2->dst) || branch->type != Type::SCbranchScc1)
	{
		return false;
	}
	// The back edge lands on the head or on waits just before it, which no register read observes.
	const int64_t loop_target = static_cast<int64_t>(branch->pc) + 4 + branch->src[0].constant.i;
	bool          target_ok   = loop_target == first->pc;
	for (uint32_t index_before = head; !target_ok && index_before-- > 0u;)
	{
		const auto& prev = instructions.At(index_before);
		if (prev.type != Type::SWaitcnt) { break; }
		target_ok = prev.pc == loop_target;
	}
	if (!target_ok) { return false; }

	// REST holds EXEC on entry. In straight-line code before the head, the nearest write of REST copies EXEC, or
	// copies a pair X that EXEC was copied from, with neither EXEC nor X written in between.
	const auto&          rest       = andn2->dst;
	unsigned             rest_first = 0;
	unsigned             rest_count = 0;
	bool                 entry_ok   = false;
	const ShaderOperand* source     = nullptr; // X, once the copy into REST is found
	ScalarRange(rest, &rest_first, &rest_count);
	for (uint32_t index_before = head; index_before-- > 0u;)
	{
		const auto& prev = instructions.At(index_before);
		if (IsBranch(prev.type) || LabelInRange(code, prev.pc + 4u, first->pc, branch->pc)) { break; }
		const bool copy = prev.type == mov && prev.src_num == 1;
		if (source != nullptr)
		{
			unsigned source_first = 0;
			unsigned source_count = 0;
			ScalarRange(*source, &source_first, &source_count);
			entry_ok = copy && IsExec(prev.dst, words) && SameRegister(prev.src[0], *source);
			if (entry_ok || WritesExec(prev) || WritesMask(prev, source_first, words)) { break; }
			continue;
		}
		if (WritesExec(prev)) { break; }
		if (WritesMask(prev, rest_first, words))
		{
			if (!copy || !SameRegister(prev.dst, rest) || (!IsExec(prev.src[0], words) && !IsSgprMask(prev.src[0], words))) { break; }
			entry_ok = IsExec(prev.src[0], words);
			if (entry_ok) { break; }
			source = &prev.src[0];
		}
	}
	unsigned scalar_word = 0;
	unsigned scalar_count = 0;
	if (!entry_ok || LabelInRange(code, first->pc + 4u, branch->pc, branch->pc) || !ScalarRange(scalar, &scalar_word, &scalar_count) ||
	    Covers(rest, scalar_word) || !DeadFrom(code, next + 2u, scalar_word) || !DeadFrom(code, next + 2u, kM0Word))
	{
		return false;
	}
	out->head      = head;
	out->moves_end = next;
	out->branch    = next + 1u;
	out->index     = index;
	out->wave32    = words == 1;
	return true;
}

} // namespace

bool ShaderLaneValuesOnEntry(const ShaderCode& code, uint32_t index, int vgpr, Vector<uint32_t>* values)
{
	const auto& instructions = code.GetInstructions();
	if (index == 0 || index >= instructions.Size() || IsLabelTarget(code, instructions.At(index).pc) ||
	    instructions.At(index - 1u).type == Type::SBranch)
	{
		return false;
	}
	std::vector<uint32_t> found;
	if (!LaneValuesAfter(code, index - 1u, vgpr, &found)) { return false; }
	values->Clear();
	for (const auto value: found) { values->Add(value); }
	return true;
}

// The loop becomes its per-lane result: each relative move reads its lane's index instead of M0, the mask updates stay
// (they empty REST and EXEC as the last iteration did), and the readfirstlane, compare, M0 copy and back edge go away.
void ShaderLowerWaterfallMoves(ShaderCode* code)
{
	auto& instructions = code->GetInstructions();
	for (uint32_t head = 0; head < instructions.Size(); ++head)
	{
		Waterfall loop;
		if (!MatchWaterfall(*code, head, &loop)) { continue; }
		const uint32_t branch_pc = instructions.At(loop.branch).pc;
		for (auto* labels: {&code->GetLabels(), &code->GetIndirectLabels()})
		{
			for (auto& label: *labels)
			{
				if (label.GetSrc() == branch_pc) { label.Disable(); }
			}
		}
		for (uint32_t move = loop.head + 3u; move < loop.moves_end; ++move)
		{
			auto& inst = instructions[move];
			if (inst.type != Type::VMovrelsB32 && inst.type != Type::VMovreldB32) { continue; }
			inst.src[1]  = loop.index;
			inst.src_num = 2;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
		}
		instructions.RemoveAt(loop.branch);
		instructions.RemoveAt(loop.head, 3u);
		if (loop.wave32) { code->SetWave32Masks(); }
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
