#include "Emulator/Graphics/ShaderComputeWaveControlFlowAnalysis.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

uint32_t ShaderComputeBarrierWorkspaceDwords(const ShaderCode& code, const ShaderComputeWaveLayout& layout)
{
	if (layout.strategy != ShaderComputeWaveStrategy::Paired64On32 || !code.HasAnyOf({ShaderInstructionType::SBarrier}))
	{
		return 0;
	}
	const bool branches = code.HasAnyOf({ShaderInstructionType::SBranch, ShaderInstructionType::SCbranchScc0,
	                                    ShaderInstructionType::SCbranchScc1, ShaderInstructionType::SCbranchVccz,
	                                    ShaderInstructionType::SCbranchVccnz, ShaderInstructionType::SCbranchExecz,
	                                    ShaderInstructionType::SCbranchExecnz});
	return branches ? layout.waves : 0u;
}

namespace {

constexpr uint32_t kNoInstruction = UINT32_MAX;

bool IsExecBranch(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SCbranchExecz || type == ShaderInstructionType::SCbranchExecnz;
}

bool IsUnsupportedBranch(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SCbranchScc0 || type == ShaderInstructionType::SCbranchScc1 ||
	       type == ShaderInstructionType::SCbranchVccz || type == ShaderInstructionType::SCbranchVccnz ||
	       type == ShaderInstructionType::SSetpcB64 || type == ShaderInstructionType::SSwappcB64;
}

ShaderComputeWaveAnalysisResult Failure(uint32_t pc, const char* reason)
{
	ShaderComputeWaveAnalysisResult result {};
	result.unsupported_pc = pc;
	result.reason         = reason;
	return result;
}

uint32_t FindInstruction(const Vector<ShaderInstruction>& instructions, uint32_t pc)
{
	for (uint32_t index = 0; index < instructions.Size(); ++index)
		if (instructions.At(index).pc == pc)
		{
			return index;
		}
	return kNoInstruction;
}

bool BranchTarget(const ShaderInstruction& branch, uint32_t* target)
{
	if (target == nullptr || branch.format != ShaderInstructionFormat::Label || branch.src_num != 1 ||
	    branch.src[0].type != ShaderOperandType::LiteralConstant || branch.src[0].size != 0)
	{
		return false;
	}
	const int64_t value = static_cast<int64_t>(branch.pc) + 4 + branch.src[0].constant.i;
	if (value < 0 || value > UINT32_MAX || (value & 3) != 0)
	{
		return false;
	}
	*target = static_cast<uint32_t>(value);
	return true;
}

bool IsCanonicalBarrier(const ShaderInstruction& instruction)
{
	const auto& immediate = instruction.src[0];
	return instruction.format == ShaderInstructionFormat::Empty && instruction.src_num == 0 &&
	       immediate.type == ShaderOperandType::LiteralConstant && immediate.size == 0 && immediate.constant.u == 0 &&
	       immediate.multiplier == 1.0f && !immediate.absolute && !immediate.negate && !immediate.clamp && immediate.swizzle == 6u &&
	       !immediate.dpp && immediate.dpp_ctrl == 0 && immediate.dpp_row_mask == 0 && immediate.dpp_bank_mask == 0 &&
	       !immediate.dpp_fetch_inactive && !immediate.dpp_bound_ctrl;
}

bool IsDiscardAt(const Vector<ShaderInstruction>& instructions, uint32_t index)
{
	if (index == 0 || index + 1 >= instructions.Size())
	{
		return false;
	}
	const auto& previous = instructions.At(index - 1);
	const auto& current  = instructions.At(index);
	const auto& next     = instructions.At(index + 1);
	return current.type == ShaderInstructionType::Exp && ShaderIsNullMrtDoneFormat(current.format) &&
	       previous.type == ShaderInstructionType::SMovB64 && previous.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
	       previous.dst.type == ShaderOperandType::ExecLo && previous.src[0].type == ShaderOperandType::IntegerInlineConstant &&
	       previous.src[0].constant.i == 0 && next.type == ShaderInstructionType::SEndpgm;
}

ShaderComputeWaveAnalysisResult ValidateBranchMetadata(const ShaderCode& code, const ShaderInstruction& conditional,
                                                       uint32_t conditional_target, const ShaderInstruction& unconditional,
                                                       uint32_t join_target)
{
	uint32_t conditional_labels = 0;
	uint32_t join_labels        = 0;
	uint32_t active_labels      = 0;
	for (uint32_t index = 0; index < code.GetLabels().Size(); ++index)
	{
		const auto& label = code.GetLabels().At(index);
		if (label.IsDisabled())
		{
			continue;
		}
		++active_labels;
		if (label.GetSrc() == conditional.pc && label.GetDst() == conditional_target)
			++conditional_labels;
		else if (label.GetSrc() == unconditional.pc && label.GetDst() == join_target)
		{
			++join_labels;
		} else
		{
			return Failure(label.GetSrc(), "active branch-label metadata does not match the decoded branch targets");
		}
	}
	if (active_labels != 2 || conditional_labels != 1 || join_labels != 1)
	{
		return Failure(conditional.pc, "paired diamond requires exactly one active label for each of its two branches");
	}

	uint32_t fallthrough_labels = 0;
	for (uint32_t index = 0; index < code.GetIndirectLabels().Size(); ++index)
	{
		const auto& label = code.GetIndirectLabels().At(index);
		if (label.IsDisabled())
		{
			continue;
		}
		if (label.GetSrc() != conditional.pc || label.GetDst() != conditional.pc + 4)
			return Failure(label.GetSrc(), "indirect-label metadata must describe only the conditional fallthrough");
		++fallthrough_labels;
	}
	return fallthrough_labels == 1 ? ShaderComputeWaveAnalysisResult {true, 0, {}}
	                               : Failure(conditional.pc, "paired diamond requires one matching conditional fallthrough label");
}

} // namespace

ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveControlFlow(const ShaderCode& code)
{
	const auto& instructions = code.GetInstructions();
	if (instructions.IsEmpty())
	{
		if (code.GetLabels().IsEmpty() && code.GetIndirectLabels().IsEmpty())
		{
			return {true, 0, {}};
		}
		return Failure(0, "branch metadata exists without parsed instructions");
	}

	uint32_t conditional_index = kNoInstruction;
	uint32_t branch_index      = kNoInstruction;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& instruction = instructions.At(index);
		if ((instruction.pc & 3u) != 0 || instruction.pc > UINT32_MAX - 4u ||
		    (index != 0 && instruction.pc <= instructions.At(index - 1).pc))
		{
			return Failure(instruction.pc, "paired CFG requires strictly increasing, aligned instruction PCs");
		}
		if (IsUnsupportedBranch(instruction.type))
		{
			return Failure(instruction.pc, "non-EXEC or indirect control flow is not admitted");
		}
		if (IsExecBranch(instruction.type))
		{
			if (conditional_index != kNoInstruction)
			{
				return Failure(instruction.pc, "paired CFG admits exactly one EXEC conditional branch");
			}
			conditional_index = index;
		}
		if (instruction.type == ShaderInstructionType::SBranch)
		{
			if (branch_index != kNoInstruction)
			{
				return Failure(instruction.pc, "paired CFG admits exactly one arm-to-join SBranch");
			}
			branch_index = index;
		}
	}

	if (conditional_index == kNoInstruction && branch_index == kNoInstruction)
	{
		if (!code.GetLabels().IsEmpty() || !code.GetIndirectLabels().IsEmpty())
		{
			return Failure(0, "branch metadata exists without a decoded branch");
		}
		return {true, 0, {}};
	}
	if (conditional_index == kNoInstruction)
	{
		return Failure(instructions.At(branch_index).pc, "SBranch requires one paired EXEC conditional branch");
	}
	if (branch_index == kNoInstruction)
	{
		return Failure(instructions.At(conditional_index).pc, "EXEC conditional requires a forward SBranch and shared join");
	}

	const auto& conditional        = instructions.At(conditional_index);
	const auto& unconditional      = instructions.At(branch_index);
	uint32_t    conditional_target = 0;
	uint32_t    join_target        = 0;
	if (!BranchTarget(conditional, &conditional_target))
	{
		return Failure(conditional.pc, "EXEC conditional has an invalid PC-relative target");
	}
	if (!BranchTarget(unconditional, &join_target))
	{
		return Failure(unconditional.pc, "SBranch has an invalid PC-relative join target");
	}
	const uint32_t fallthrough_pc    = conditional.pc + 4;
	const uint32_t alternate_pc      = unconditional.pc + 4;
	const uint32_t fallthrough_index = FindInstruction(instructions, fallthrough_pc);
	const uint32_t alternate_index   = FindInstruction(instructions, alternate_pc);
	const uint32_t join_index        = FindInstruction(instructions, join_target);
	if (conditional_target <= conditional.pc || fallthrough_index == kNoInstruction)
	{
		return Failure(conditional.pc, "EXEC conditional must have a forward target and an instruction-boundary fallthrough");
	}
	if (conditional_index >= branch_index || branch_index <= conditional_index + 1u)
	{
		return Failure(conditional.pc, "non-taken arm must contain an instruction before its terminal SBranch");
	}
	if (alternate_index == kNoInstruction || conditional_target != alternate_pc || alternate_index != branch_index + 1u)
	{
		return Failure(conditional.pc, "EXEC target must be the first alternate-arm instruction immediately after the non-taken SBranch");
	}
	if (join_target <= alternate_pc || join_index == kNoInstruction || join_index <= alternate_index)
	{
		return Failure(unconditional.pc, "SBranch must target a later instruction-boundary join after a nonempty alternate arm");
	}

	if (auto metadata = ValidateBranchMetadata(code, conditional, conditional_target, unconditional, join_target); !metadata.supported)
	{
		return metadata;
	}
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& instruction = instructions.At(index);
		if (instruction.type == ShaderInstructionType::SBarrier)
		{
			if (!IsCanonicalBarrier(instruction))
			{
				return Failure(instruction.pc, "only the canonical zero-immediate SBarrier is admitted");
			}
			if ((instruction.pc >= fallthrough_pc && instruction.pc <= unconditional.pc) ||
			    (instruction.pc >= alternate_pc && instruction.pc < join_target))
			{
				return Failure(instruction.pc, "a workgroup barrier inside either divergent arm lacks a convergence proof");
			}
		}
		if (instruction.type == ShaderInstructionType::DsWriteB32 || instruction.type == ShaderInstructionType::DsReadB32 ||
		    instruction.type == ShaderInstructionType::DsAddRtnU32)
		{
			return Failure(instruction.pc, "LDS effects in a branch-containing shader require CFG-aware address and mask proofs");
		}
		if ((instruction.pc >= fallthrough_pc && instruction.pc < unconditional.pc) ||
		    (instruction.pc >= alternate_pc && instruction.pc < join_target))
		{
			if (instruction.type == ShaderInstructionType::SEndpgm)
			{
				return Failure(instruction.pc, "SEndpgm exits a divergent arm before the shared join");
			}
			if (IsDiscardAt(instructions, index))
			{
				return Failure(instruction.pc, "discard control flow is not admitted in a paired branch shader");
			}
		} else if (IsDiscardAt(instructions, index))
		{
			return Failure(instruction.pc, "discard control flow is not admitted in a branch-containing paired shader");
		}
	}

	return {true, 0, {}};
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
