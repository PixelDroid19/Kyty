#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_RECOMPILER_FUNC(Recompile_VCmpClassF32)
{
	const auto& instruction = code.GetInstructions().At(index);
	if (!ShaderFloatClassComparisonSupported(instruction)) { return false; }
	const auto dst0 = operand_variable_to_str(instruction.dst, 0);
	const auto dst1 = operand_variable_to_str(instruction.dst, 1);
	const auto i = String8::FromPrintf("%u", index);
	String8 value;
	String8 mask;
	if (dst0.type != SpirvType::Uint || dst1.type != SpirvType::Uint ||
	    !operand_load_uint(spirv, instruction.src[0], "class_value_<index>", i, &value) ||
	    !operand_load_uint(spirv, instruction.src[1], "class_mask_<index>", i, &mask)) { return false; }
	// Integer classification preserves NaN payloads and subnormals regardless
	// of the host's floating-point flushing and NaN canonicalization modes.
	static const char* text = R"(
<value>
<mask>
%class_exponent_<index> = OpBitwiseAnd %uint %class_value_<index> %<exponent_mask>
%class_fraction_<index> = OpBitwiseAnd %uint %class_value_<index> %<fraction_mask>
%class_sign_<index> = OpBitwiseAnd %uint %class_value_<index> %<sign_mask>
%class_quiet_<index> = OpBitwiseAnd %uint %class_value_<index> %<quiet_mask>
%class_negative_<index> = OpINotEqual %bool %class_sign_<index> %uint_0
%class_exp_zero_<index> = OpIEqual %bool %class_exponent_<index> %uint_0
%class_exp_ones_<index> = OpIEqual %bool %class_exponent_<index> %<exponent_mask>
%class_frac_zero_<index> = OpIEqual %bool %class_fraction_<index> %uint_0
%class_is_quiet_<index> = OpINotEqual %bool %class_quiet_<index> %uint_0
%class_normal_<index> = OpSelect %uint %class_negative_<index> %uint_3 %uint_8
%class_subnormal_<index> = OpSelect %uint %class_negative_<index> %uint_4 %uint_7
%class_zero_<index> = OpSelect %uint %class_negative_<index> %uint_5 %uint_6
%class_infinity_<index> = OpSelect %uint %class_negative_<index> %uint_2 %uint_9
%class_nan_<index> = OpSelect %uint %class_is_quiet_<index> %uint_1 %uint_0
%class_special_<index> = OpSelect %uint %class_frac_zero_<index> %class_infinity_<index> %class_nan_<index>
%class_nonzero_exp_<index> = OpSelect %uint %class_exp_ones_<index> %class_special_<index> %class_normal_<index>
%class_zero_exp_<index> = OpSelect %uint %class_frac_zero_<index> %class_zero_<index> %class_subnormal_<index>
%class_index_<index> = OpSelect %uint %class_exp_zero_<index> %class_zero_exp_<index> %class_nonzero_exp_<index>
%class_bit_<index> = OpShiftLeftLogical %uint %uint_1 %class_index_<index>
%class_selected_<index> = OpBitwiseAnd %uint %class_mask_<index> %class_bit_<index>
%class_matches_<index> = OpINotEqual %bool %class_selected_<index> %uint_0
%t3_<index> = OpSelect %uint %class_matches_<index> %uint_1 %uint_0
%class_exec_<index> = OpLoad %uint %exec_lo
%class_active_<index> = OpINotEqual %bool %class_exec_<index> %uint_0
%class_result_<index> = OpSelect %uint %class_active_<index> %t3_<index> %uint_0
OpStore %<dst0> %class_result_<index>
OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text).ReplaceStr("<value>", value).ReplaceStr("<mask>", mask)
	                           .ReplaceStr("<index>", i)
	                           .ReplaceStr("<exponent_mask>", spirv->GetConstantUint(0x7f800000u))
	                           .ReplaceStr("<fraction_mask>", spirv->GetConstantUint(0x007fffffu))
	                           .ReplaceStr("<quiet_mask>", spirv->GetConstantUint(0x00400000u))
	                           .ReplaceStr("<sign_mask>", spirv->GetConstantUint(0x80000000u))
	                           .ReplaceStr("<dst0>", dst0.value).ReplaceStr("<dst1>", dst1.value);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
