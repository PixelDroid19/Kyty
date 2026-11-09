#include "Emulator/Graphics/Shader.h"

#include "ShaderLaneFlow.h"
#include "ShaderMaskAnalysis.h"
#include "ShaderNativeWaveInternal.h"
#include "ShaderStorageAnalysis.h"

#include <algorithm>
#include <bitset>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

bool ShaderInstructionIsScalarBooleanCompare(ShaderInstructionType type)
{
	switch (type)
	{
		// Keep this list aligned with the established Recompile_VCmp_XXX_{F32,I32,U32}
		// emitters. Half, double, and cmpx/special compare paths are not proven here.
		case ShaderInstructionType::VCmpEqF32:
		case ShaderInstructionType::VCmpFF32:
		case ShaderInstructionType::VCmpGeF32:
		case ShaderInstructionType::VCmpGtF32:
		case ShaderInstructionType::VCmpLeF32:
		case ShaderInstructionType::VCmpLgF32:
		case ShaderInstructionType::VCmpLtF32:
		case ShaderInstructionType::VCmpNeqF32:
		case ShaderInstructionType::VCmpNgeF32:
		case ShaderInstructionType::VCmpNgtF32:
		case ShaderInstructionType::VCmpNleF32:
		case ShaderInstructionType::VCmpNlgF32:
		case ShaderInstructionType::VCmpNltF32:
		case ShaderInstructionType::VCmpOF32:
		case ShaderInstructionType::VCmpTruF32:
		case ShaderInstructionType::VCmpUF32:
		case ShaderInstructionType::VCmpEqI32:
		case ShaderInstructionType::VCmpEqU32:
		case ShaderInstructionType::VCmpFI32:
		case ShaderInstructionType::VCmpGeI32:
		case ShaderInstructionType::VCmpGtI32:
		case ShaderInstructionType::VCmpLeI32:
		case ShaderInstructionType::VCmpLtI32:
		case ShaderInstructionType::VCmpNeI32:
		case ShaderInstructionType::VCmpNeU32:
		case ShaderInstructionType::VCmpTI32:
		case ShaderInstructionType::VCmpFU32:
		case ShaderInstructionType::VCmpGeU32:
		case ShaderInstructionType::VCmpGtU32:
		case ShaderInstructionType::VCmpLeU32:
		case ShaderInstructionType::VCmpLtU32:
		case ShaderInstructionType::VCmpTU32: return true;
		default: return false;
	}
}

bool ShaderInstructionWritesProvenScalarMaskPair(const ShaderInstruction& inst, int start_register)
{
	return ShaderInstructionIsScalarBooleanCompare(inst.type) && inst.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1 &&
	       inst.src_num == 2 && inst.dst.type == ShaderOperandType::Sgpr && inst.dst.register_id == start_register &&
	       inst.dst.size == 2 && !ShaderOperandOverlapsSgprRange(inst.dst2, start_register, 2);
}

bool ShaderCodeHasLabelTarget(const ShaderCode& code, uint32_t pc)
{
	for (const auto& label: code.GetLabels())
	{
		if (!label.IsDisabled() && label.GetDst() == pc)
		{
			return true;
		}
	}
	for (const auto& label: code.GetIndirectLabels())
	{
		if (!label.IsDisabled() && label.GetDst() == pc)
		{
			return true;
		}
	}
	return false;
}

bool ShaderInstructionBreaksStraightLineProof(ShaderInstructionType type)
{
	return ShaderInstructionHasStaticBranchTarget(type) || type == ShaderInstructionType::SSetpcB64 ||
	       type == ShaderInstructionType::SSwappcB64 || type == ShaderInstructionType::SEndpgm;
}

bool ShaderInstructionWritesVccHigh(const ShaderInstruction& inst)
{
	const auto overlaps_high = [](const ShaderOperand& operand)
	{
		return operand.type == ShaderOperandType::VccHi || (operand.type == ShaderOperandType::VccLo && operand.size >= 2);
	};
	// Native wave32 carry output occupies one scalar word, including the
	// implicit VCC_LO destination of a VOP2 instruction.
	const bool single_carry = inst.type == ShaderInstructionType::VAddCoCiU32 || inst.type == ShaderInstructionType::VSubrevCoCiU32;
	return overlaps_high(inst.dst) || inst.dst2.type == ShaderOperandType::VccHi || (!single_carry && overlaps_high(inst.dst2));
}

bool ShaderReverseBorrowVccHighHasProvenance(const ShaderCode& code, uint32_t instruction_index)
{
	const auto& instructions = code.GetInstructions();
	if (ShaderCodeHasLabelTarget(code, instructions.At(instruction_index).pc))
	{
		return false;
	}
	for (uint32_t candidate_index = instruction_index; candidate_index > 0;)
	{
		const auto& candidate = instructions.At(--candidate_index);
		if (ShaderInstructionWritesVccHigh(candidate))
		{
			return ShaderInstructionIsScalarBooleanCompare(candidate.type) &&
			       candidate.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1 && candidate.src_num == 2 &&
			       candidate.dst.type == ShaderOperandType::VccHi && candidate.dst.size == 1 &&
			       candidate.dst2.type == ShaderOperandType::Unknown;
		}
		if (ShaderInstructionBreaksStraightLineProof(candidate.type) || ShaderCodeHasLabelTarget(code, candidate.pc))
		{
			return false;
		}
	}
	return false;
}

} // namespace

bool ShaderReverseBorrowMaskHasProvenance(const ShaderCode& code, uint32_t instruction_index, bool native_wave32)
{
	const auto& instructions = code.GetInstructions();
	if (instruction_index >= instructions.Size())
	{
		return false;
	}

	const auto& consumer = instructions.At(instruction_index);
	if (consumer.type != ShaderInstructionType::VSubrevCoCiU32 || consumer.src_num < 3)
	{
		return false;
	}

	const auto& mask = consumer.src[2];
	if (native_wave32 && mask.type == ShaderOperandType::VccHi && mask.size == 1)
	{
		return ShaderReverseBorrowVccHighHasProvenance(code, instruction_index);
	}
	if (mask.size != 2)
	{
		return false;
	}
	if (mask.type == ShaderOperandType::VccLo || mask.type == ShaderOperandType::ExecLo)
	{
		return true;
	}
	if (mask.type != ShaderOperandType::Sgpr || mask.register_id < 0 || ShaderCodeHasLabelTarget(code, consumer.pc))
	{
		return false;
	}

	for (uint32_t candidate_index = instruction_index; candidate_index > 0;)
	{
		const auto& candidate = instructions.At(--candidate_index);
		const bool overlaps_mask = ShaderOperandOverlapsSgprRange(candidate.dst, mask.register_id, 2) ||
		                           ShaderOperandOverlapsSgprRange(candidate.dst2, mask.register_id, 2);

		// A control-flow target at the compare itself is safe: execution begins
		// with the producer. Targets at any later instruction can bypass it.
		if (overlaps_mask)
		{
			return ShaderInstructionWritesProvenScalarMaskPair(candidate, mask.register_id);
		}
		if (ShaderInstructionBreaksStraightLineProof(candidate.type) || ShaderCodeHasLabelTarget(code, candidate.pc))
		{
			return false;
		}
	}

	return false;
}

bool ShaderInstructionSuccessors(const ShaderCode& code, std::vector<std::vector<uint32_t>>* successors)
{
	const auto&    instructions = code.GetInstructions();
	const uint32_t count        = instructions.Size();
	std::vector<uint32_t> pcs(count);
	for (uint32_t i = 0; i < count; i++)
	{
		pcs[i] = instructions.At(i).pc;
	}
	successors->assign(count, {});
	for (uint32_t i = 0; i < count; i++)
	{
		const auto& inst = instructions.At(i);
		if (inst.src_num < 0 || inst.src_num > 4)
		{
			return false;
		}
		if (inst.type == ShaderInstructionType::SEndpgm)
		{
			continue;
		}
		if (!ShaderInstructionHasStaticBranchTarget(inst.type))
		{
			if (LaneFlow::AnyPrefix(inst.type, {"SCbranch", "SBranch", "SSetpc", "SSwappc", "SCall", "SRfe"}))
			{
				return false;
			}
			if (i + 1 < count)
			{
				(*successors)[i].push_back(i + 1);
			}
			continue;
		}
		const auto target = static_cast<uint32_t>(static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i);
		const auto it     = std::lower_bound(pcs.begin(), pcs.end(), target);
		if (it == pcs.end() || *it != target)
		{
			return false;
		}
		(*successors)[i].push_back(static_cast<uint32_t>(it - pcs.begin()));
		if (inst.type != ShaderInstructionType::SBranch && i + 1 < count)
		{
			(*successors)[i].push_back(i + 1);
		}
	}
	return true;
}

namespace {

using MaskWords = std::bitset<LaneFlow::kWords>;

void AddMaskWords(ShaderOperand operand, MaskWords* words)
{
	if (operand.type == ShaderOperandType::ExecZ || operand.type == ShaderOperandType::VccZ)
	{
		operand.type = operand.type == ShaderOperandType::ExecZ ? ShaderOperandType::ExecLo : ShaderOperandType::VccLo;
		operand.size = 2;
	}
	unsigned first = 0;
	unsigned count = 0;
	if (!LaneFlow::ScalarRange(operand, &first, &count))
	{
		return;
	}
	for (unsigned word = first; word < first + count; word++)
	{
		words->set(word);
	}
}

void AddMaskPair(ShaderOperandType low, MaskWords* words)
{
	ShaderOperand pair {};
	pair.type = low;
	pair.size = 2;
	AddMaskWords(pair, words);
}

// What one instruction does to the wave-wide demand on scalar words: it kills
// `defs`, always needs `uses` complete, and needs `uses_if_demanded` complete
// only when a later reader needs one of its `defs` complete.
struct MaskDemandStep
{
	MaskWords defs;
	MaskWords uses;
	MaskWords uses_if_demanded;
	// Sources a gathering instruction takes from an in-block producer or
	// gather candidate: the gather, not they, completes the result.
	MaskWords tree_uses;
	bool      producer = false;
	bool      gather_candidate = false;
};

bool UsesWholeExec(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SCbranchExecz || type == ShaderInstructionType::SCbranchExecnz ||
	       type == ShaderInstructionType::VReadfirstlaneB32 || type == ShaderInstructionType::DsAppend ||
	       type == ShaderInstructionType::DsConsume;
}

// Pointwise on the 64 lane bits, so each lane's result bit depends only on
// that lane's source bits.
bool IsPointwiseMaskOp(ShaderInstructionType type)
{
	const auto name = ShaderInstructionTypeName(type);
	return (LaneFlow::IsMaskAlgebra(type) && !LaneFlow::StartsWith(type, "SWqm") && name.size() > 3 &&
	        name.substr(name.size() - 3) == "B64") ||
	       type == ShaderInstructionType::SCselectB64;
}

// Under-approximates the SCC definers: a missed one only keeps SCC demanded.
bool DefinesScc(ShaderInstructionType type)
{
	return LaneFlow::AnyPrefix(type, {"SAnd", "SOr", "SXor", "SNand", "SNor", "SXnor", "SNot", "SWqm", "SLsh", "SAshr", "SAdd", "SSub",
	                                  "SBfe", "SMin", "SMax", "SAbs", "SBcnt", "SCmp", "SBitcmp"});
}

MaskDemandStep MaskDemandOf(const ShaderInstruction& inst)
{
	MaskDemandStep step;
	const bool     scalar    = LaneFlow::StartsWith(inst.type, "S");
	const bool     cmpx      = LaneFlow::StartsWith(inst.type, "VCmpx");
	const bool     exec_op   = scalar && ShaderInstructionTypeName(inst.type).find("exec") != std::string_view::npos;
	const bool     pointwise = scalar && IsPointwiseMaskOp(inst.type);
	auto*          sources   = pointwise ? &step.uses_if_demanded : &step.uses;
	step.producer            = !scalar && ShaderNativePackedResult(inst);
	for (int s = 0; s < inst.src_num; s++)
	{
		if (!scalar && LaneFlow::IsLaneBit(inst, s))
		{
			continue;
		}
		AddMaskWords(inst.src[s], sources);
	}
	if (cmpx)
	{
		AddMaskPair(ShaderOperandType::ExecLo, &step.defs);
	} else
	{
		AddMaskWords(inst.dst, &step.defs);
		AddMaskWords(inst.dst2, &step.defs);
	}
	if (exec_op)
	{
		AddMaskPair(ShaderOperandType::ExecLo, &step.defs);
		AddMaskPair(ShaderOperandType::ExecLo, sources);
	}
	if (inst.type == ShaderInstructionType::SMulkI32)
	{
		// The decoder lists the destination read of S_CMOV(K), S_ADDK and S_BITSET as a source; S_MULK keeps it implicit.
		AddMaskWords(inst.dst, &step.uses);
	}
	if (scalar && DefinesScc(inst.type))
	{
		step.defs.set(LaneFlow::kSccWord);
	}
	// A select must pick the same source in every part, even for a pointwise result.
	if (LaneFlow::ReadsSccImplicitly(inst.type) || inst.type == ShaderInstructionType::SCbranchScc0 ||
	    inst.type == ShaderInstructionType::SCbranchScc1)
	{
		step.uses.set(LaneFlow::kSccWord);
	}
	if (inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz)
	{
		AddMaskPair(ShaderOperandType::VccLo, &step.uses);
	}
	if (UsesWholeExec(inst.type))
	{
		AddMaskPair(ShaderOperandType::ExecLo, &step.uses);
	}
	return step;
}


bool IsGatherType(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::SAndB64:
		case ShaderInstructionType::SAndn2B64:
		case ShaderInstructionType::SNandB64:
		case ShaderInstructionType::SNorB64:
		case ShaderInstructionType::SOrB64:
		case ShaderInstructionType::SOrn2B64:
		case ShaderInstructionType::SXnorB64:
		case ShaderInstructionType::SXorB64: break;
		default: return false;
	}
	return inst.format == ShaderInstructionFormat::Sdst2Ssrc02Ssrc12 && inst.dst.size == 2 &&
	       (inst.dst.type == ShaderOperandType::Sgpr || inst.dst.type == ShaderOperandType::VccLo);
}

// The instruction whose whole-pair write of `pair` reaches `index` in its
// straight-line block, or -1.
int64_t InBlockPairDefinition(const ShaderCode& code, const std::vector<bool>& label_targets, const std::vector<MaskDemandStep>& steps,
                              uint32_t index, const ShaderOperand& pair)
{
	MaskWords words;
	AddMaskWords(pair, &words);
	if (words.count() != 2 || words.test(LaneFlow::kSccWord))
	{
		return -1;
	}
	const auto& instructions = code.GetInstructions();
	for (uint32_t at = index; at > 0 && !label_targets[at];)
	{
		const uint32_t candidate = --at;
		if (ShaderInstructionBreaksStraightLineProof(instructions.At(candidate).type))
		{
			return -1;
		}
		const auto written = steps[candidate].defs & words;
		if (written.any())
		{
			return written == words ? static_cast<int64_t>(candidate) : -1;
		}
	}
	return -1;
}

// Marks the gather candidates: pointwise S_*_B64 mask algebra with at least
// one source from an in-block compare or candidate. Such a tree costs one
// gather at its demanded root instead of one per compare leaf, executed as
// often since the whole tree is one block.
void MarkGatherCandidates(const ShaderCode& code, std::vector<MaskDemandStep>* steps, std::vector<std::vector<int64_t>>* tree_definitions)
{
	const auto&    instructions = code.GetInstructions();
	const uint32_t count        = instructions.Size();
	std::vector<bool> label_targets(count, false);
	for (uint32_t i = 0; i < count; i++)
	{
		label_targets[i] = ShaderCodeHasLabelTarget(code, instructions.At(i).pc);
	}
	tree_definitions->assign(count, {});
	for (uint32_t i = 0; i < count; i++)
	{
		const auto& inst = instructions.At(i);
		if (!IsGatherType(inst))
		{
			continue;
		}
		auto& step = (*steps)[i];
		for (int s = 0; s < 2; s++)
		{
			const auto definition = InBlockPairDefinition(code, label_targets, *steps, i, inst.src[s]);
			if (definition < 0 || !((*steps)[definition].producer || (*steps)[definition].gather_candidate))
			{
				continue;
			}
			AddMaskWords(inst.src[s], &step.tree_uses);
			(*tree_definitions)[i].push_back(definition);
		}
		step.gather_candidate = step.tree_uses.any();
	}
}

// Backward: the words some later reader needs with every lane's bit.
void SolveMaskPlan(const std::vector<MaskDemandStep>& steps, const std::vector<std::vector<uint32_t>>& successors,
                   const std::vector<std::vector<int64_t>>& tree_definitions, ShaderLaneMaskPlan* plan)
{
	const auto             count = static_cast<uint32_t>(steps.size());
	std::vector<MaskWords> demanded_in(count);
	std::vector<MaskWords> demanded_out(count);
	for (bool changed = true; changed;)
	{
		changed = false;
		for (uint32_t i = count; i-- > 0;)
		{
			MaskWords out;
			for (auto succ: successors[i])
			{
				out |= demanded_in[succ];
			}
			const auto& step = steps[i];
			auto        in   = (out & ~step.defs) | step.uses;
			if ((out & step.defs).any())
			{
				in |= step.uses_if_demanded & ~step.tree_uses;
			}
			if (in != demanded_in[i] || out != demanded_out[i])
			{
				demanded_in[i]  = in;
				demanded_out[i] = out;
				changed         = true;
			}
		}
	}
	for (uint32_t i = 0; i < count; i++)
	{
		const auto& step = steps[i];
		const bool  demanded = (demanded_out[i] & step.defs).any();
		plan->local[i]       = step.producer && !demanded;
		// A tree whose every leaf is complete anyway needs no gather.
		const bool partial = std::any_of(tree_definitions[i].begin(), tree_definitions[i].end(),
		                                 [&](int64_t d) { return plan->local[d] || (steps[d].gather_candidate && !plan->gather[d]); });
		plan->gather[i] = step.gather_candidate && demanded && partial;
	}
}

MaskWords ExecWords()
{
	MaskWords words;
	AddMaskPair(ShaderOperandType::ExecLo, &words);
	return words;
}

// Over-approximates the scalar words an instruction reads. EXEC is left out: no skipped range defines it.
MaskWords ScalarReadsOf(const ShaderInstruction& inst)
{
	MaskWords words;
	for (int s = 0; s < inst.src_num; s++)
	{
		AddMaskWords(inst.src[s], &words);
	}
	// Forms that keep part of their destination, whether or not the decoder lists it as a source.
	if (LaneFlow::AnyPrefix(inst.type, {"SCmov", "SBitset", "SAddk", "SMulk"}))
	{
		AddMaskWords(inst.dst, &words);
	}
	if (LaneFlow::ReadsSccImplicitly(inst.type) || LaneFlow::StartsWith(inst.type, "SCbranchScc"))
	{
		words.set(LaneFlow::kSccWord);
	}
	if (LaneFlow::MayReadVcc(inst.type))
	{
		AddMaskPair(ShaderOperandType::VccLo, &words);
	}
	if (LaneFlow::AnyPrefix(inst.type, {"Ds", "VInterp", "SSendmsg"}))
	{
		words.set(LaneFlow::kM0Word);
	}
	return words & ~ExecWords();
}

// Over-approximates the scalar words an instruction a part may skip writes.
MaskWords ScalarWritesOf(const ShaderInstruction& inst)
{
	const auto name = ShaderInstructionTypeName(inst.type);
	const auto has  = [&](std::string_view part) { return name.find(part) != std::string_view::npos; };
	MaskWords  words;
	AddMaskWords(inst.dst, &words);
	AddMaskWords(inst.dst2, &words);
	if (LaneFlow::IsScalarAlu(inst.type) && LaneFlow::WritesScc(inst.type))
	{
		words.set(LaneFlow::kSccWord);
	}
	if (LaneFlow::StartsWith(inst.type, "V") && (has("Cmp") || has("Class") || has("Co") || has("Div")))
	{
		AddMaskPair(ShaderOperandType::VccLo, &words);
	}
	return words & ~ExecWords();
}

// Scalar words live before each instruction. Only scalar ALU, scalar loads and lane-mask producers kill:
// a missed definition keeps a word alive. A wave64 producer writes both words of its mask pair.
std::vector<MaskWords> ScalarLiveIn(const ShaderCode& code, const std::vector<std::vector<uint32_t>>& successors,
                                    const std::vector<MaskDemandStep>& steps)
{
	const auto&            instructions = code.GetInstructions();
	const uint32_t         count        = instructions.Size();
	std::vector<MaskWords> reads(count);
	std::vector<MaskWords> kills(count);
	for (uint32_t i = 0; i < count; i++)
	{
		const auto type = instructions.At(i).type;
		reads[i]        = ScalarReadsOf(instructions.At(i));
		if (LaneFlow::IsScalarAlu(type) || LaneFlow::IsScalarLoad(type) || steps[i].producer)
		{
			kills[i] = steps[i].defs;
		}
	}
	std::vector<MaskWords> live_in(count);
	for (bool changed = true; changed;)
	{
		changed = false;
		for (uint32_t i = count; i-- > 0;)
		{
			MaskWords out;
			for (auto succ: successors[i])
			{
				out |= live_in[succ];
			}
			const auto in = (out & ~kills[i]) | reads[i];
			changed       = changed || in != live_in[i];
			live_in[i]    = in;
		}
	}
	return live_in;
}

bool HasDppSource(const ShaderInstruction& inst)
{
	return std::any_of(inst.src, inst.src + inst.src_num, [](const ShaderOperand& source) { return source.dpp; });
}

// An instruction the active parts run without a rendezvous while a part with no active lane skips it:
// its vector work is masked per lane and its scalar results are checked dead after the range. V_CMPX
// only narrows EXEC; whether it stays lane-local is the plan's to say.
bool PartMaySkip(const ShaderInstruction& inst, const MaskDemandStep& step)
{
	const auto type = inst.type;
	if (HasDppSource(inst))
	{
		return false;
	}
	if (LaneFlow::StartsWith(type, "VCmpx"))
	{
		return true;
	}
	if ((step.defs & ExecWords()).any())
	{
		return false;
	}
	if (LaneFlow::StartsWith(type, "V"))
	{
		return !LaneFlow::IsLaneExchange(type);
	}
	if (LaneFlow::IsScalarAlu(type) || LaneFlow::IsScalarLoad(type) || LaneFlow::AnyPrefix(type, {"SWaitcnt", "SNop", "SInstPrefetch"}))
	{
		return true;
	}
	return (LaneFlow::StartsWith(type, "Image") || LaneFlow::IsBufferLoad(type)) && !LaneFlow::WritesMemory(type);
}

// Target index of each forward S_CBRANCH_EXECZ whose skipped range (branch, target) a part may skip alone
// as far as the range itself tells; 0 for every other instruction. The range is entered only through the
// branch, leaves only at the target, and defines no scalar word that is live there.
std::vector<uint32_t> PartLocalSkipTargets(const ShaderCode& code, const std::vector<std::vector<uint32_t>>& successors,
                                           const std::vector<MaskDemandStep>& steps)
{
	const auto&           instructions = code.GetInstructions();
	const uint32_t        count        = instructions.Size();
	std::vector<uint32_t> targets(count, 0);
	// Branch sources of each target, as the range they span.
	std::vector<uint32_t> first_source(count, count);
	std::vector<uint32_t> last_source(count, 0);
	for (uint32_t i = 0; i < count; i++)
	{
		const auto type = instructions.At(i).type;
		if (LaneFlow::AnyPrefix(type, {"SMovrel", "VMovrel"}))
		{
			return targets;
		}
		if (ShaderInstructionHasStaticBranchTarget(type))
		{
			const uint32_t target = successors[i][0];
			first_source[target]  = std::min(first_source[target], i);
			last_source[target]   = std::max(last_source[target], i);
		}
	}
	const auto live_in = ScalarLiveIn(code, successors, steps);
	for (uint32_t branch = 0; branch < count; branch++)
	{
		if (instructions.At(branch).type != ShaderInstructionType::SCbranchExecz || successors[branch][0] <= branch + 1)
		{
			continue;
		}
		const uint32_t target = successors[branch][0];
		MaskWords      written;
		bool           skippable = true;
		for (uint32_t i = branch + 1; i < target && skippable; i++)
		{
			const auto& inst    = instructions.At(i);
			const bool  entered = first_source[i] != count && (first_source[i] <= branch || last_source[i] >= target);
			const bool  jumps   = ShaderInstructionHasStaticBranchTarget(inst.type);
			skippable = !entered && (jumps ? successors[i][0] > i && successors[i][0] <= target : PartMaySkip(inst, steps[i]));
			written |= ScalarWritesOf(inst);
		}
		if (skippable && (written & live_in[target]).none())
		{
			targets[branch] = target;
		}
	}
	return targets;
}

} // namespace

ShaderLaneMaskPlan ShaderLaneMaskPlanOf(const ShaderCode& code, bool part_local_skips)
{
	const auto&                        instructions = code.GetInstructions();
	const uint32_t                     count        = instructions.Size();
	ShaderLaneMaskPlan                 plan;
	plan.local.assign(count, false);
	plan.gather.assign(count, false);
	plan.part_skipped.assign(count, false);
	std::vector<std::vector<uint32_t>> successors;
	if (!ShaderInstructionSuccessors(code, &successors))
	{
		return plan;
	}
	std::vector<MaskDemandStep> steps(count);
	for (uint32_t i = 0; i < count; i++)
	{
		steps[i] = MaskDemandOf(instructions.At(i));
	}
	std::vector<std::vector<int64_t>> tree_definitions;
	MarkGatherCandidates(code, &steps, &tree_definitions);
	auto skip_targets = part_local_skips ? PartLocalSkipTargets(code, successors, steps) : std::vector<uint32_t>(count, 0);
	for (uint32_t i = 0; i < count; i++)
	{
		if (skip_targets[i] != 0)
		{
			steps[i].uses &= ~ExecWords();
		}
	}
	// A skip whose range still gathers a mask loses its exemption; that only adds demand, so this ends.
	for (bool dropped = true; dropped;)
	{
		dropped = false;
		SolveMaskPlan(steps, successors, tree_definitions, &plan);
		for (uint32_t branch = 0; branch < count; branch++)
		{
			const auto gathers = [&](uint32_t i) { return (steps[i].producer && !plan.local[i]) || plan.gather[i]; };
			bool       rendezvous = false;
			for (uint32_t i = branch + 1; i < skip_targets[branch] && !rendezvous; i++)
			{
				rendezvous = gathers(i);
			}
			if (rendezvous)
			{
				skip_targets[branch] = 0;
				steps[branch].uses |= ExecWords();
				dropped = true;
			}
		}
	}
	for (uint32_t branch = 0; branch < count; branch++)
	{
		for (uint32_t i = branch + 1; i < skip_targets[branch]; i++)
		{
			plan.part_skipped[i] = true;
		}
	}
	return plan;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
