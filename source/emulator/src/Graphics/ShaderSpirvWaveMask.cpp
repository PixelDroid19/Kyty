#include "ShaderSpirvInternal.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

bool WaveMaskOperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool WaveMaskPairIsSupported(const Spirv& spirv, const ShaderOperand& operand)
{
	if (!WaveMaskOperandIsPlain(operand) || operand.size != 2)
	{
		return false;
	}

	switch (operand.type)
	{
		case ShaderOperandType::VccLo:
		case ShaderOperandType::ExecLo:
		case ShaderOperandType::Sgpr: break;
		default: return false;
	}

	const auto low  = spirv.GetComputeWaveRegister(operand, ShaderWaveBank::Low, 0);
	const auto high = spirv.GetComputeWaveRegister(operand, ShaderWaveBank::High, 1);
	return low.type == SpirvType::Uint && high.type == SpirvType::Uint && !low.value.IsEmpty() && !high.value.IsEmpty();
}

bool WaveMaskSourceIsSupported(const Spirv& spirv, const ShaderOperand& operand)
{
	if (!WaveMaskOperandIsPlain(operand))
	{
		return false;
	}

	switch (operand.type)
	{
		case ShaderOperandType::Vgpr:
		case ShaderOperandType::Sgpr:
		{
			if (operand.size != 1)
			{
				return false;
			}
			const auto value = spirv.GetComputeWaveRegister(operand, ShaderWaveBank::Low, 0);
			return !value.value.IsEmpty();
		}
		case ShaderOperandType::LiteralConstant:
		case ShaderOperandType::IntegerInlineConstant:
		case ShaderOperandType::FloatInlineConstant: return operand.size == 0;
		default: return false;
	}
}

// The mask pair a compare result lands in. v_cmpx_* writes EXEC; its parsed
// VccLo destination is only the implicit placeholder of the plain VOPC form.
ShaderOperand WaveMaskCompareDestination(const ShaderInstruction& instruction)
{
	if (!ShaderComputeWaveTypeIsExecCompare(instruction.type))
	{
		return instruction.dst;
	}
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	return exec;
}

bool WaveMaskCompareDestinationIsSupported(const Spirv& spirv, const ShaderInstruction& instruction)
{
	if (instruction.dst2.type != ShaderOperandType::Unknown || instruction.dst2.size != 0)
	{
		return false;
	}
	const auto destination = WaveMaskCompareDestination(instruction);
	if (!WaveMaskPairIsSupported(spirv, destination))
	{
		return false;
	}
	if (destination.type == ShaderOperandType::ExecLo)
	{
		// Only the plain VOPC placeholder form is admitted for EXEC writes.
		return instruction.dst.type == ShaderOperandType::VccLo;
	}
	// Non-CMPX comparisons preserve EXEC. An EXEC destination needs its own
	// write contract and is deliberately outside this first admission set.
	return destination.type == ShaderOperandType::VccLo || destination.type == ShaderOperandType::Sgpr;
}

bool WaveMaskSgprPairAliasesSource(const ShaderOperand& destination, const ShaderOperand& source)
{
	if (destination.type != ShaderOperandType::Sgpr || source.type != ShaderOperandType::Sgpr || destination.register_id < 0 ||
	    source.register_id < 0)
	{
		return false;
	}

	const int relative_register = source.register_id - destination.register_id;
	return relative_register == 0 || relative_register == 1;
}

const char* ExpectedComputeWaveComparePredicate(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::VCmpEqU32:
		case ShaderInstructionType::VCmpxEqU32: return "OpIEqual";
		case ShaderInstructionType::VCmpLtU32:
		case ShaderInstructionType::VCmpxLtU32: return "OpULessThan";
		case ShaderInstructionType::VCmpNeU32:
		case ShaderInstructionType::VCmpxNeU32: return "OpINotEqual";
		case ShaderInstructionType::VCmpGeU32:
		case ShaderInstructionType::VCmpxGeU32: return "OpUGreaterThanEqual";
		case ShaderInstructionType::VCmpGtU32:
		case ShaderInstructionType::VCmpxGtU32: return "OpUGreaterThan";
		case ShaderInstructionType::VCmpLeU32:
		case ShaderInstructionType::VCmpxLeU32: return "OpULessThanEqual";
		default: return nullptr;
	}
}

bool IsKnownUintConstant(const String8& value)
{
	return !value.IsEmpty() && value != "unknown_uint_constant";
}

bool IsKnownBank(ShaderWaveBank bank)
{
	return bank == ShaderWaveBank::Low || bank == ShaderWaveBank::High;
}

} // namespace

bool Spirv::EmitComputeWaveMaskBit(const ShaderOperand& mask, ShaderWaveBank bank, const String8& result_id, String8* output) const
{
	if (output == nullptr || result_id.IsEmpty() || !UsesComputeWaveBanks() || !IsKnownBank(bank) || !WaveMaskPairIsSupported(*this, mask))
	{
		return false;
	}

	const int  word  = bank == ShaderWaveBank::Low ? 0 : 1;
	const auto value = GetComputeWaveRegister(mask, bank, word);
	const auto zero  = GetConstantUint(0u);
	const auto one   = GetConstantUint(1u);
	if (value.type != SpirvType::Uint || value.value.IsEmpty() || !IsKnownUintConstant(zero) || !IsKnownUintConstant(one))
	{
		return false;
	}

	*output += String8(R"(
%<result>_word = OpLoad %uint %<mask>
%<result>_bit = OpShiftLeftLogical %uint %<one> %wave_lane_id
%<result>_masked = OpBitwiseAnd %uint %<result>_word %<result>_bit
%<result> = OpINotEqual %bool %<result>_masked %<zero>
)")
	               .ReplaceStr("<result>", result_id)
	               .ReplaceStr("<mask>", value.value)
	               .ReplaceStr("<one>", one)
	               .ReplaceStr("<zero>", zero);
	return true;
}

bool Spirv::EmitComputeWaveBallot(const String8& low_predicate, const String8& high_predicate, const String8& low_result,
                                  const String8& high_result, String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks() || low_predicate.IsEmpty() || high_predicate.IsEmpty() || low_result.IsEmpty() ||
	    high_result.IsEmpty() || low_result == high_result)
	{
		return false;
	}

	const auto subgroup_scope = GetConstantUint(3u);
	if (!IsKnownUintConstant(subgroup_scope))
	{
		return false;
	}

	// Both collectives are unconditional: incoming EXEC changes the predicate,
	// never participation in either host subgroup operation.
	*output += String8(R"(
%<low_result>_ballot = OpGroupNonUniformBallot %v4uint %<scope> %<low_predicate>
%<high_result>_ballot = OpGroupNonUniformBallot %v4uint %<scope> %<high_predicate>
%<low_result> = OpCompositeExtract %uint %<low_result>_ballot 0
%<high_result> = OpCompositeExtract %uint %<high_result>_ballot 0
)")
	               .ReplaceStr("<low_predicate>", low_predicate)
	               .ReplaceStr("<high_predicate>", high_predicate)
	               .ReplaceStr("<low_result>", low_result)
	               .ReplaceStr("<high_result>", high_result)
	               .ReplaceStr("<scope>", subgroup_scope);
	return true;
}

bool Spirv::EmitComputeWaveCompareU32(const ShaderInstruction& instruction, uint32_t index, const char* predicate_op, String8* output) const
{
	const auto* expected_predicate = ExpectedComputeWaveComparePredicate(instruction.type);
	if (output == nullptr || predicate_op == nullptr || !UsesComputeWaveBanks() ||
	    instruction.format != ShaderInstructionFormat::SmaskVsrc0Vsrc1 || instruction.src_num != 2 || expected_predicate == nullptr ||
	    std::strcmp(predicate_op, expected_predicate) != 0 || !WaveMaskCompareDestinationIsSupported(*this, instruction) ||
	    !WaveMaskSourceIsSupported(*this, instruction.src[0]) || !WaveMaskSourceIsSupported(*this, instruction.src[1]) ||
	    WaveMaskSgprPairAliasesSource(WaveMaskCompareDestination(instruction), instruction.src[0]) ||
	    WaveMaskSgprPairAliasesSource(WaveMaskCompareDestination(instruction), instruction.src[1]))
	{
		return false;
	}

	const auto destination      = WaveMaskCompareDestination(instruction);
	const auto destination_low  = GetComputeWaveRegister(destination, ShaderWaveBank::Low, 0);
	const auto destination_high = GetComputeWaveRegister(destination, ShaderWaveBank::High, 1);
	if (destination_low.type != SpirvType::Uint || destination_high.type != SpirvType::Uint || destination_low.value.IsEmpty() ||
	    destination_high.value.IsEmpty())
	{
		return false;
	}

	const auto index_str      = String8::FromPrintf("%u", index);
	const auto src0_low       = String8("wave_cmp_src0_low_") + index_str;
	const auto src0_high      = String8("wave_cmp_src0_high_") + index_str;
	const auto src1_low       = String8("wave_cmp_src1_low_") + index_str;
	const auto src1_high      = String8("wave_cmp_src1_high_") + index_str;
	const auto exec_low       = String8("wave_cmp_exec_low_") + index_str;
	const auto exec_high      = String8("wave_cmp_exec_high_") + index_str;
	const auto predicate_low  = String8("wave_cmp_predicate_low_") + index_str;
	const auto predicate_high = String8("wave_cmp_predicate_high_") + index_str;
	const auto active_low     = String8("wave_cmp_active_low_") + index_str;
	const auto active_high    = String8("wave_cmp_active_high_") + index_str;
	const auto mask_low       = String8("wave_cmp_mask_low_") + index_str;
	const auto mask_high      = String8("wave_cmp_mask_high_") + index_str;

	// Capture both source banks and the incoming EXEC words before producing
	// either destination word. This is required for pair aliases and keeps the
	// comparison's source state independent of its result stores.
	String8 source;
	if (!EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::Low, src0_low, &source) ||
	    !EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::High, src0_high, &source) ||
	    !EmitComputeWaveOperandUint(instruction.src[1], ShaderWaveBank::Low, src1_low, &source) ||
	    !EmitComputeWaveOperandUint(instruction.src[1], ShaderWaveBank::High, src1_high, &source))
	{
		return false;
	}

	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	if (!EmitComputeWaveMaskBit(exec, ShaderWaveBank::Low, exec_low, &source) ||
	    !EmitComputeWaveMaskBit(exec, ShaderWaveBank::High, exec_high, &source))
	{
		return false;
	}

	*output += source;
	*output += String8(R"(
%<predicate_low> = <predicate> %bool %<src0_low> %<src1_low>
%<predicate_high> = <predicate> %bool %<src0_high> %<src1_high>
%<active_low> = OpLogicalAnd %bool %<predicate_low> %<exec_low>
%<active_high> = OpLogicalAnd %bool %<predicate_high> %<exec_high>
)")
	               .ReplaceStr("<predicate>", predicate_op)
	               .ReplaceStr("<src0_low>", src0_low)
	               .ReplaceStr("<src0_high>", src0_high)
	               .ReplaceStr("<src1_low>", src1_low)
	               .ReplaceStr("<src1_high>", src1_high)
	               .ReplaceStr("<exec_low>", exec_low)
	               .ReplaceStr("<exec_high>", exec_high)
	               .ReplaceStr("<predicate_low>", predicate_low)
	               .ReplaceStr("<predicate_high>", predicate_high)
	               .ReplaceStr("<active_low>", active_low)
	               .ReplaceStr("<active_high>", active_high);
	if (!EmitComputeWaveBallot(active_low, active_high, mask_low, mask_high, output))
	{
		return false;
	}

	*output += String8("               OpStore %<low_destination> %<low_result>\n"
	                   "               OpStore %<high_destination> %<high_result>\n")
	               .ReplaceStr("<low_destination>", destination_low.value)
	               .ReplaceStr("<high_destination>", destination_high.value)
	               .ReplaceStr("<low_result>", mask_low)
	               .ReplaceStr("<high_result>", mask_high);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
