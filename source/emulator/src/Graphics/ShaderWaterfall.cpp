#include "ShaderParseInternal.h"

#include "ShaderLaneFlow.h"

#include <map>

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

bool IsSgprPair(const ShaderOperand& op)
{
	return op.type == Operand::Sgpr && op.size == 2;
}

bool IsExec(const ShaderOperand& op)
{
	return op.type == Operand::ExecLo && op.size == 2;
}

bool Covers(const ShaderOperand& op, unsigned word)
{
	unsigned first = 0;
	unsigned count = 0;
	return ScalarRange(op, &first, &count) && word >= first && word < first + count;
}

// M0 also feeds LDS/GDS addressing, interpolation, messages and the relative moves without naming it.
bool ReadsM0Implicitly(Type type)
{
	return AnyPrefix(type, {"Ds", "VInterp", "SSendmsg", "VMovrel", "SMovrel"});
}

bool Reads(const ShaderInstruction& inst, unsigned word)
{
	for (int source = 0; source < inst.src_num; ++source)
	{
		if (Covers(inst.src[source], word)) { return true; }
	}
	return word == kM0Word && ReadsM0Implicitly(inst.type);
}

bool Writes(const ShaderInstruction& inst, unsigned word)
{
	return Covers(inst.dst, word) || Covers(inst.dst2, word);
}

// V_CMPX and S_*_SAVEEXEC replace EXEC without naming it as their destination.
bool WritesExec(const ShaderInstruction& inst)
{
	return Writes(inst, kExecLoWord) || WritesExecOnly(inst.type) ||
	       ShaderInstructionTypeName(inst.type).find("Saveexec") != std::string_view::npos;
}

bool IsBranch(Type type)
{
	return type == Type::SBranch || StartsWith(type, "SCbranch");
}

// No path from instruction `from` reads `word` before writing it. Control flow after `from` must be forward
// branches only, so one pass in program order sees every path.
bool DeadFrom(const ShaderCode& code, uint32_t from, unsigned word)
{
	const auto&                  instructions = code.GetInstructions();
	std::map<uint32_t, bool>     reaching; // live value arriving at a branch target
	bool                         live = true;
	for (uint32_t index = from; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (const auto target = reaching.find(inst.pc); target != reaching.end()) { live = live || target->second; }
		if (live && Reads(inst, word)) { return false; }
		if (Writes(inst, word)) { live = false; }
		if (IsBranch(inst.type))
		{
			const int64_t target = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
			if (target <= static_cast<int64_t>(inst.pc)) { return false; }
			reaching[static_cast<uint32_t>(target)] = reaching[static_cast<uint32_t>(target)] || live;
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

struct Waterfall
{
	uint32_t head = 0; // V_READFIRSTLANE
	uint32_t moves_end = 0; // first instruction after the relative moves (S_ANDN2)
	uint32_t branch = 0; // S_CBRANCH_SCC1 back to the head
	ShaderOperand index;
};

// A loop that hands each distinct per-lane index to M0 in turn and moves VGPR[base + M0] for the lanes holding it:
//
//   s_mov_b64 rest, exec
// head:
//   v_readfirstlane_b32 s, index
//   v_cmpx_eq_u32 s, index          ; EXEC = remaining lanes with this index
//   s_mov_b32 m0, s
//   v_movrels_b32 dst, base ...     ; dst = VGPR[base + m0]
//   s_andn2_b64 rest, rest, exec
//   s_mov_b64 exec, rest
//   s_cbranch_scc1 head
//
// Every lane active at the head ends with dst = VGPR[base + its own index], however the lanes are grouped into waves,
// and the loop leaves REST and EXEC empty and SCC clear. Only S and M0 keep a value chosen by one lane, so both must be
// dead after the loop.
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

	uint32_t next = head + 3u;
	for (const ShaderInstruction* move = at(next); move != nullptr && move->type == Type::VMovrelsB32; move = at(++next))
	{
		if (move->src_num != 1 || move->vop_sdwa || move->dst.type != Operand::Vgpr || move->dst.size != 1 ||
		    move->src[0].type != Operand::Vgpr || move->src[0].size != 1 || move->src[0].dpp || move->src[0].negate ||
		    move->src[0].absolute || move->dst.clamp || move->dst.register_id == index.register_id)
		{
			return false;
		}
	}
	const auto* andn2  = at(next);
	const auto* exec   = at(next + 1u);
	const auto* branch = at(next + 2u);
	if (next == head + 3u || andn2 == nullptr || exec == nullptr || branch == nullptr || andn2->type != Type::SAndn2B64 ||
	    andn2->src_num != 2 || !IsSgprPair(andn2->dst) || !SameRegister(andn2->src[0], andn2->dst) || !IsExec(andn2->src[1]) ||
	    exec->type != Type::SMovB64 || !IsExec(exec->dst) || exec->src_num != 1 || !SameRegister(exec->src[0], andn2->dst) ||
	    branch->type != Type::SCbranchScc1 || branch->pc + 4 + branch->src[0].constant.i != first->pc)
	{
		return false;
	}

	// REST holds EXEC on entry: the nearest earlier write of REST or EXEC copies EXEC into REST, in straight-line code.
	const auto& rest       = andn2->dst;
	unsigned    rest_first = 0;
	unsigned    rest_count = 0;
	bool        entry_ok   = false;
	ScalarRange(rest, &rest_first, &rest_count);
	for (uint32_t index_before = head; index_before-- > 0u;)
	{
		const auto& prev = instructions.At(index_before);
		if (IsBranch(prev.type) || LabelInRange(code, prev.pc + 4u, first->pc, branch->pc) || WritesExec(prev)) { break; }
		if (Writes(prev, rest_first) || Writes(prev, rest_first + 1u))
		{
			entry_ok = prev.type == Type::SMovB64 && SameRegister(prev.dst, rest) && prev.src_num == 1 && IsExec(prev.src[0]);
			break;
		}
	}
	unsigned scalar_word = 0;
	unsigned scalar_count = 0;
	if (!entry_ok || LabelInRange(code, first->pc + 4u, branch->pc, branch->pc) || !ScalarRange(scalar, &scalar_word, &scalar_count) ||
	    Covers(rest, scalar_word) || !DeadFrom(code, next + 3u, scalar_word) || !DeadFrom(code, next + 3u, kM0Word))
	{
		return false;
	}
	out->head      = head;
	out->moves_end = next;
	out->branch    = next + 2u;
	out->index     = index;
	return true;
}

} // namespace

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
		for (auto& label: code->GetLabels())
		{
			if (label.GetSrc() == branch_pc) { label.Disable(); }
		}
		for (uint32_t move = loop.head + 3u; move < loop.moves_end; ++move)
		{
			auto& inst   = instructions[move];
			inst.src[1]  = loop.index;
			inst.src_num = 2;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
		}
		instructions.RemoveAt(loop.branch);
		instructions.RemoveAt(loop.head, 3u);
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
