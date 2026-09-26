#include "Emulator/Graphics/Shader.h"

#include "ShaderMaskAnalysis.h"
#include "ShaderStorageAnalysis.h"

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

} // namespace

bool ShaderReverseBorrowMaskHasProvenance(const ShaderCode& code, uint32_t instruction_index)
{
	const auto& instructions = code.GetInstructions();
	if (instruction_index >= instructions.Size())
	{
		return false;
	}

	const auto& consumer = instructions.At(instruction_index);
	if (consumer.type != ShaderInstructionType::VSubrevCoCiU32 || consumer.src_num < 3 || consumer.src[2].size != 2)
	{
		return false;
	}

	const auto& mask = consumer.src[2];
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

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
