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
	bool      producer = false;
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

bool BuildMaskSuccessors(const ShaderCode& code, std::vector<std::vector<uint32_t>>* successors)
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


} // namespace

std::vector<bool> ShaderLaneLocalMaskResults(const ShaderCode& code)
{
	const auto&                        instructions = code.GetInstructions();
	const uint32_t                     count        = instructions.Size();
	std::vector<bool>                  local(count, false);
	std::vector<std::vector<uint32_t>> successors;
	if (!BuildMaskSuccessors(code, &successors))
	{
		return local;
	}
	std::vector<MaskDemandStep> steps(count);
	for (uint32_t i = 0; i < count; i++)
	{
		steps[i] = MaskDemandOf(instructions.At(i));
	}
	// Backward: the words some later reader needs with every lane's bit.
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
				in |= step.uses_if_demanded;
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
		local[i] = steps[i].producer && (demanded_out[i] & steps[i].defs).none();
	}
	return local;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
