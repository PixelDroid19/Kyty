#ifndef KYTY_SHADER_SPIRV_F16_H_
#define KYTY_SHADER_SPIRV_F16_H_

#include "ShaderSpirvInternal.h"

#include <cinttypes>
#include <cstdio>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::F16Arithmetic {

enum class Operation
{
	FromFloat,
	ToFloat,
	FromSigned,
	FromUnsigned,
	ToSigned,
	ToUnsigned,
	Unary,
	Binary,
	Ternary,
};

struct Mode
{
	uint8_t value = 0;
	bool ieee = false;
	bool dx10_clamp = false;
	bool fp16_overflow = false;
	bool fp16_overflow_known = false;
};

inline bool ReadMode(const Spirv* spirv, Mode* mode)
{
	if (spirv->GetCode().GetType() == ShaderType::Pixel && spirv->GetPsInputInfo() != nullptr)
	{
		const auto* info = spirv->GetPsInputInfo();
		*mode = {info->float_mode, info->ieee_mode, info->dx10_clamp, info->fp16_overflow, info->fp16_overflow_known};
		return true;
	}
	if (spirv->GetCode().GetType() == ShaderType::Vertex && spirv->GetVsInputInfo() != nullptr)
	{
		const auto* info = spirv->GetVsInputInfo();
		*mode = {info->float_mode, info->ieee_mode, info->dx10_clamp, info->fp16_overflow, info->fp16_overflow_known};
		return true;
	}
	if (spirv->GetCode().GetType() == ShaderType::Compute && spirv->GetCsInputInfo() != nullptr)
	{
		const auto* info = spirv->GetCsInputInfo();
		if (!info->fp_mode_known) { return false; }
		*mode = {info->float_mode, info->ieee_mode, info->dx10_clamp, info->fp16_overflow, info->fp16_overflow_known};
		return true;
	}
	// Missing initial MODE evidence must not become an assumed host/default mode.
	return false;
}

// Narrow raw binary32 to binary16 with integer operations. Unlike GLSL
// PackHalf2x16, this has explicit rounding/overflow/denormal behavior and does
// not depend on the host's float16 rounding or denormal controls. Every shift
// is defined even on the unselected side of an OpSelect.
inline String8 Narrow(const Spirv* spirv, const String8& bits, const String8& result, const String8& suffix,
                      uint32_t round, bool flush_output, bool fp16_overflow, const String8& residue = {})
{
	String8 text = R"(
%hn_sign_shift_<s> = OpShiftRightLogical %uint %<bits> %uint_16
%hn_sign_<s> = OpBitwiseAnd %uint %hn_sign_shift_<s> %uint_0x00008000
%hn_mag_<s> = OpBitwiseAnd %uint %<bits> %uint_0x7fffffff
%hn_exp_shift_<s> = OpShiftRightLogical %uint %<bits> %uint_23
%hn_exp_<s> = OpBitwiseAnd %uint %hn_exp_shift_<s> %uint_255
%hn_mant_<s> = OpBitwiseAnd %uint %<bits> %uint_0x007fffff
%hn_hidden_<s> = OpBitwiseOr %uint %hn_mant_<s> %uint_0x00800000
%hn_normal_<s> = OpUGreaterThan %bool %hn_exp_<s> %uint_112
%hn_sub_shift_raw_<s> = OpISub %uint %uint_126 %hn_exp_<s>
%hn_sub_shift_<s> = OpExtInst %uint %GLSL_std_450 UMin %hn_sub_shift_raw_<s> %uint_24
%hn_shift_<s> = OpSelect %uint %hn_normal_<s> %uint_13 %hn_sub_shift_<s>
%hn_significand_<s> = OpSelect %uint %hn_normal_<s> %hn_mant_<s> %hn_hidden_<s>
%hn_truncated_<s> = OpShiftRightLogical %uint %hn_significand_<s> %hn_shift_<s>
%hn_limit_<s> = OpShiftLeftLogical %uint %uint_1 %hn_shift_<s>
%hn_mask_<s> = OpISub %uint %hn_limit_<s> %uint_1
%hn_remainder_<s> = OpBitwiseAnd %uint %hn_significand_<s> %hn_mask_<s>
%hn_halfway_<s> = OpShiftRightLogical %uint %hn_limit_<s> %uint_1
%hn_half_exp_<s> = OpISub %uint %hn_exp_<s> %uint_112
%hn_half_exp_shift_<s> = OpShiftLeftLogical %uint %hn_half_exp_<s> %uint_10
%hn_exp_bits_<s> = OpSelect %uint %hn_normal_<s> %hn_half_exp_shift_<s> %uint_0
%hn_base_<s> = OpBitwiseOr %uint %hn_exp_bits_<s> %hn_truncated_<s>
%hn_min_exp_<s> = OpISub %uint %uint_103 %uint_1
%hn_tiny_<s> = OpULessThan %bool %hn_exp_<s> %hn_min_exp_<s>
%hn_base_safe_<s> = OpSelect %uint %hn_tiny_<s> %uint_0 %hn_base_<s>
%hn_nonzero_<s> = OpINotEqual %bool %hn_mag_<s> %uint_0
%hn_lost_<s> = OpINotEqual %bool %hn_remainder_<s> %uint_0
%hn_inexact_<s> = OpSelect %bool %hn_tiny_<s> %hn_nonzero_<s> %hn_lost_<s>
%hn_negative_<s> = OpINotEqual %bool %hn_sign_<s> %uint_0
<round>
%hn_increment_<s> = OpSelect %uint %hn_round_up_<s> %uint_1 %uint_0
%hn_rounded_<s> = OpIAdd %uint %hn_base_safe_<s> %hn_increment_<s>
%hn_large_exp_<s> = OpUGreaterThanEqual %bool %hn_exp_<s> %uint_143
%hn_large_result_<s> = OpUGreaterThanEqual %bool %hn_rounded_<s> %uint_0x00007c00
%hn_overflow_<s> = OpLogicalOr %bool %hn_large_exp_<s> %hn_large_result_<s>
<overflow>
%hn_finite_<s> = OpSelect %uint %hn_overflow_<s> %hn_overflow_value_<s> %hn_rounded_<s>
%hn_nan_mant_<s> = OpShiftRightLogical %uint %hn_mant_<s> %uint_13
%hn_quiet_<s> = OpBitwiseOr %uint %hn_nan_mant_<s> %uint_0x00000200
%hn_nan_<s> = OpBitwiseOr %uint %hn_quiet_<s> %uint_0x00007c00
%hn_mant_zero_<s> = OpIEqual %bool %hn_mant_<s> %uint_0
%hn_special_<s> = OpSelect %uint %hn_mant_zero_<s> %uint_0x00007c00 %hn_nan_<s>
%hn_is_special_<s> = OpIEqual %bool %hn_exp_<s> %uint_255
%hn_payload_<s> = OpSelect %uint %hn_is_special_<s> %hn_special_<s> %hn_finite_<s>
%hn_signed_<s> = OpBitwiseOr %uint %hn_sign_<s> %hn_payload_<s>
<result>
)";
	String8 rounding;
	String8 overflow;
	if (round == 0u)
	{
		rounding = R"(
%hn_odd_bit_<s> = OpBitwiseAnd %uint %hn_base_safe_<s> %uint_1
%hn_odd_<s> = OpINotEqual %bool %hn_odd_bit_<s> %uint_0
<tie_direction>
%hn_tie_<s> = OpIEqual %bool %hn_remainder_<s> %hn_halfway_<s>
%hn_tie_odd_<s> = OpLogicalAnd %bool %hn_tie_<s> %hn_tie_up_<s>
%hn_above_<s> = OpUGreaterThan %bool %hn_remainder_<s> %hn_halfway_<s>
%hn_nearest_up_<s> = OpLogicalOr %bool %hn_above_<s> %hn_tie_odd_<s>
%hn_round_up_<s> = OpSelect %bool %hn_tiny_<s> %false %hn_nearest_up_<s>
)";
		rounding = rounding.ReplaceStr("<tie_direction>", residue.IsEmpty() ?
		    "%hn_tie_up_<s> = OpCopyObject %bool %hn_odd_<s>" : R"(
%hn_residue_nonzero_<s> = OpINotEqual %bool %<residue> %int_0
%hn_residue_negative_<s> = OpSLessThan %bool %<residue> %int_0
%hn_residue_away_<s> = OpLogicalEqual %bool %hn_residue_negative_<s> %hn_negative_<s>
%hn_tie_up_<s> = OpSelect %bool %hn_residue_nonzero_<s> %hn_residue_away_<s> %hn_odd_<s>
)").ReplaceStr("<residue>", residue);
		overflow = "%hn_overflow_value_<s> = OpCopyObject %uint %uint_0x00007c00\n";
	} else if (round == 3u)
	{
		rounding = "%hn_round_up_<s> = OpCopyObject %bool %false\n";
		overflow = "%hn_overflow_value_<s> = OpCopyObject %uint %uint_0x00007bff\n";
	} else
	{
		rounding = (round == 1u ? "%hn_direction_<s> = OpLogicalNot %bool %hn_negative_<s>\n" :
		                          "%hn_direction_<s> = OpCopyObject %bool %hn_negative_<s>\n");
		rounding += "%hn_round_up_<s> = OpLogicalAnd %bool %hn_direction_<s> %hn_inexact_<s>\n";
		overflow = "%hn_overflow_value_<s> = OpSelect %uint %hn_direction_<s> %uint_0x00007c00 %uint_0x00007bff\n";
	}
	// MODE.FP16_OVFL overrides only finite overflow, in every round mode.
	// The separate special-value selection above still preserves true INF
	// and quieted NaN sign/payload rather than saturating their bit patterns.
	if (fp16_overflow) { overflow = "%hn_overflow_value_<s> = OpCopyObject %uint %uint_0x00007bff\n"; }
	const String8 store = flush_output ? R"(
%hn_result_exp_<s> = OpBitwiseAnd %uint %hn_signed_<s> %uint_0x00007c00
%hn_flush_<s> = OpIEqual %bool %hn_result_exp_<s> %uint_0
%<result> = OpSelect %uint %hn_flush_<s> %hn_sign_<s> %hn_signed_<s>
)" : "%<result> = OpCopyObject %uint %hn_signed_<s>\n";
	return text.ReplaceStr("<round>", rounding)
	    .ReplaceStr("<overflow>", overflow)
	    .ReplaceStr("<result>", store.ReplaceStr("<result>", result))
	    .ReplaceStr("<bits>", bits)
	    .ReplaceStr("<s>", suffix)
	    .ReplaceStr("uint_103", spirv->GetConstantUint(103))
	    .ReplaceStr("uint_112", spirv->GetConstantUint(112))
	    .ReplaceStr("uint_126", spirv->GetConstantUint(126))
	    .ReplaceStr("uint_143", spirv->GetConstantUint(143))
	    .ReplaceStr("uint_255", spirv->GetConstantUint(255));
}

inline bool HasHalfDestinationSelect(ShaderInstructionType type)
{
	return type == ShaderInstructionType::VFmaF16 || type == ShaderInstructionType::VMin3F16 ||
	       type == ShaderInstructionType::VMax3F16 || type == ShaderInstructionType::VMed3F16;
}

// Only the direction of p + c - rounded is needed to resolve a half midpoint.
// Align the three binary32 significands with three low guard bits and a sticky
// bit. If alignment loses bits, only the smaller addend can lose them: the
// larger addend and rounded occupy the coarse float grid. Cancellation has
// nearby exponents and loses no bits. The integer difference has the error's sign.
// Half products and their nonzero sums are normal binary32 values; each aligned
// magnitude is below 2^27, so the signed sum/difference cannot overflow.
// Integer operations keep this proof independent of float reassociation.
inline String8 FmaResidue(const String8& index)
{
	String8 text;
	const char* values[] = {"fh_product", "h2", "t"};
	for (uint32_t term = 0; term < 3u; ++term)
	{
		text += String8(R"(
%hr_bits_<term>_<index> = OpBitcast %uint %<value>_<index>
%hr_exp_<term>_<index> = OpBitFieldUExtract %uint %hr_bits_<term>_<index> %uint_23 %uint_8
)").ReplaceStr("<term>", String8::FromPrintf("%u", term)).ReplaceStr("<value>", values[term]);
	}
	text += R"(
%hr_max_pc_<index> = OpExtInst %uint %GLSL_std_450 UMax %hr_exp_0_<index> %hr_exp_1_<index>
%hr_exp_<index> = OpExtInst %uint %GLSL_std_450 UMax %hr_max_pc_<index> %hr_exp_2_<index>
)";
	for (uint32_t term = 0; term < 3u; ++term)
	{
		text += String8(R"(
%hr_mant_<term>_<index> = OpBitwiseAnd %uint %hr_bits_<term>_<index> %uint_0x007fffff
%hr_hidden_<term>_<index> = OpBitwiseOr %uint %hr_mant_<term>_<index> %uint_0x00800000
%hr_zero_<term>_<index> = OpIEqual %bool %hr_exp_<term>_<index> %uint_0
%hr_sig_<term>_<index> = OpSelect %uint %hr_zero_<term>_<index> %uint_0 %hr_hidden_<term>_<index>
%hr_extended_<term>_<index> = OpShiftLeftLogical %uint %hr_sig_<term>_<index> %uint_3
%hr_gap_<term>_<index> = OpISub %uint %hr_exp_<index> %hr_exp_<term>_<index>
%hr_shift_<term>_<index> = OpExtInst %uint %GLSL_std_450 UMin %hr_gap_<term>_<index> %uint_31
%hr_limit_<term>_<index> = OpShiftLeftLogical %uint %uint_1 %hr_shift_<term>_<index>
%hr_mask_<term>_<index> = OpISub %uint %hr_limit_<term>_<index> %uint_1
%hr_lost_bits_<term>_<index> = OpBitwiseAnd %uint %hr_extended_<term>_<index> %hr_mask_<term>_<index>
%hr_lost_<term>_<index> = OpINotEqual %bool %hr_lost_bits_<term>_<index> %uint_0
%hr_sticky_<term>_<index> = OpSelect %uint %hr_lost_<term>_<index> %uint_1 %uint_0
%hr_truncated_<term>_<index> = OpShiftRightLogical %uint %hr_extended_<term>_<index> %hr_shift_<term>_<index>
%hr_magnitude_<term>_<index> = OpBitwiseOr %uint %hr_truncated_<term>_<index> %hr_sticky_<term>_<index>
%hr_positive_<term>_<index> = OpBitcast %int %hr_magnitude_<term>_<index>
%hr_negative_value_<term>_<index> = OpISub %int %int_0 %hr_positive_<term>_<index>
%hr_sign_<term>_<index> = OpBitwiseAnd %uint %hr_bits_<term>_<index> %uint_0x80000000
%hr_negative_<term>_<index> = OpINotEqual %bool %hr_sign_<term>_<index> %uint_0
%hr_signed_<term>_<index> = OpSelect %int %hr_negative_<term>_<index> %hr_negative_value_<term>_<index> %hr_positive_<term>_<index>
)").ReplaceStr("<term>", String8::FromPrintf("%u", term));
	}
	text += R"(
%hr_sum_<index> = OpIAdd %int %hr_signed_0_<index> %hr_signed_1_<index>
%fh_residue_<index> = OpISub %int %hr_sum_<index> %hr_signed_2_<index>
)";
	return text.ReplaceStr("<index>", index);
}

// Widen without a host half operation. In particular, preserve denormals and
// NaN sign/payload while quieting signaling NaNs, which UnpackHalf2x16 is not
// required to preserve bit-for-bit on every implementation.
inline String8 Expand(const String8& bits, const String8& result, const String8& suffix)
{
	return String8(R"(
%he_exp_<s> = OpBitFieldUExtract %uint %<bits> %uint_10 %uint_5
%he_mant_<s> = OpBitFieldUExtract %uint %<bits> %uint_0 %uint_10
%he_msb_<s> = OpExtInst %int %GLSL_std_450 FindUMsb %he_mant_<s>
%he_shift_i_<s> = OpISub %int %int_10 %he_msb_<s>
%he_shift_<s> = OpBitcast %uint %he_shift_i_<s>
%he_sub_sig_<s> = OpShiftLeftLogical %uint %he_mant_<s> %he_shift_<s>
%he_sub_mant_<s> = OpBitFieldUExtract %uint %he_sub_sig_<s> %uint_0 %uint_10
%he_bias_<s> = OpIAdd %uint %uint_112 %uint_1
%he_sub_exp_<s> = OpISub %uint %he_bias_<s> %he_shift_<s>
%he_normal_exp_<s> = OpIAdd %uint %he_exp_<s> %uint_112
%he_subnormal_<s> = OpIEqual %bool %he_exp_<s> %uint_0
%he_finite_exp_<s> = OpSelect %uint %he_subnormal_<s> %he_sub_exp_<s> %he_normal_exp_<s>
%he_special_<s> = OpIEqual %bool %he_exp_<s> %uint_31
%he_float_exp_<s> = OpSelect %uint %he_special_<s> %uint_255 %he_finite_exp_<s>
%he_finite_mant_<s> = OpSelect %uint %he_subnormal_<s> %he_sub_mant_<s> %he_mant_<s>
%he_mant_nonzero_<s> = OpINotEqual %bool %he_mant_<s> %uint_0
%he_nan_<s> = OpLogicalAnd %bool %he_special_<s> %he_mant_nonzero_<s>
%he_quiet_mant_<s> = OpBitwiseOr %uint %he_mant_<s> %uint_0x00000200
%he_float_mant_<s> = OpSelect %uint %he_nan_<s> %he_quiet_mant_<s> %he_finite_mant_<s>
%he_exp_bits_<s> = OpShiftLeftLogical %uint %he_float_exp_<s> %uint_23
%he_mant_bits_<s> = OpShiftLeftLogical %uint %he_float_mant_<s> %uint_13
%he_payload_<s> = OpBitwiseOr %uint %he_exp_bits_<s> %he_mant_bits_<s>
%he_magnitude_<s> = OpBitFieldUExtract %uint %<bits> %uint_0 %uint_15
%he_zero_<s> = OpIEqual %bool %he_magnitude_<s> %uint_0
%he_nonzero_bits_<s> = OpSelect %uint %he_zero_<s> %uint_0 %he_payload_<s>
%he_half_sign_<s> = OpBitwiseAnd %uint %<bits> %uint_0x00008000
%he_sign_<s> = OpShiftLeftLogical %uint %he_half_sign_<s> %uint_16
%he_bits_<s> = OpBitwiseOr %uint %he_nonzero_bits_<s> %he_sign_<s>
%<result> = OpBitcast %float %he_bits_<s>
)").ReplaceStr("<bits>", bits).ReplaceStr("<result>", result).ReplaceStr("<s>", suffix);
}

// IEEE_MODE=0 min/max: one NaN yields the other input. Unlike GLSL FMin/
// FMax, the ISA also fixes which signed zero is selected. Operate on the
// binary32 images of unpacked halves so neither choice loses payload bits.
inline String8 MinMax(const String8& a, const String8& b, const String8& result, const String8& suffix, bool maximum)
{
	return String8(R"(
%hm_a_bits_<s> = OpBitcast %uint %<a>
%hm_b_bits_<s> = OpBitcast %uint %<b>
%hm_a_nan_<s> = OpIsNan %bool %<a>
%hm_b_nan_<s> = OpIsNan %bool %<b>
%hm_a_zero_<s> = OpFOrdEqual %bool %<a> %<float_zero>
%hm_b_zero_<s> = OpFOrdEqual %bool %<b> %<float_zero>
%hm_both_zero_<s> = OpLogicalAnd %bool %hm_a_zero_<s> %hm_b_zero_<s>
%hm_zero_<s> = <zero_op> %uint %hm_a_bits_<s> %hm_b_bits_<s>
%hm_compare_<s> = <compare> %bool %<a> %<b>
%hm_ordered_<s> = OpSelect %uint %hm_compare_<s> %hm_a_bits_<s> %hm_b_bits_<s>
%hm_ordered_zero_<s> = OpSelect %uint %hm_both_zero_<s> %hm_zero_<s> %hm_ordered_<s>
%hm_b_nan_value_<s> = OpSelect %uint %hm_b_nan_<s> %hm_a_bits_<s> %hm_ordered_zero_<s>
%hm_bits_<s> = OpSelect %uint %hm_a_nan_<s> %hm_b_bits_<s> %hm_b_nan_value_<s>
%<result> = OpBitcast %float %hm_bits_<s>
)").ReplaceStr("<a>", a).ReplaceStr("<b>", b).ReplaceStr("<result>", result).ReplaceStr("<s>", suffix)
	    .ReplaceStr("<zero_op>", maximum ? "OpBitwiseAnd" : "OpBitwiseOr")
	    .ReplaceStr("<compare>", maximum ? "OpFOrdGreaterThan" : "OpFOrdLessThan");
}

inline String8 Trig(const String8& index, bool cosine)
{
	// Reduce turns before multiplying by 2*pi. This keeps finite half inputs
	// in the host trig operation's accuracy domain, including +/-65504, and
	// gives exact zeros at quadrant boundaries instead of one half denormal.
	return String8(R"(
%ht_abs_<index> = OpExtInst %float %GLSL_std_450 FAbs %h0_<index>
%ht_inf_<index> = OpIsInf %bool %h0_<index>
%ht_nan_<index> = OpIsNan %bool %h0_<index>
%ht_special_<index> = OpLogicalOr %bool %ht_inf_<index> %ht_nan_<index>
%ht_safe_<index> = OpSelect %float %ht_special_<index> %<float_zero> %h0_<index>
%ht_whole_<index> = OpExtInst %float %GLSL_std_450 RoundEven %ht_safe_<index>
%ht_turn_<index> = OpFSub %float %ht_safe_<index> %ht_whole_<index>
%ht_abs_turn_<index> = OpExtInst %float %GLSL_std_450 FAbs %ht_turn_<index>
%ht_quarter_<index> = OpFMul %float %<float_half> %<float_half>
%ht_is_quarter_<index> = OpFOrdEqual %bool %ht_abs_turn_<index> %ht_quarter_<index>
%ht_is_half_<index> = OpFOrdEqual %bool %ht_abs_turn_<index> %<float_half>
%ht_radians_<index> = OpFMul %float %ht_turn_<index> %float_2pi
%ht_approx_<index> = OpExtInst %float %GLSL_std_450 <trig> %ht_radians_<index>
%ht_negative_one_<index> = OpFNegate %float %<float_one>
<quadrants>
%ht_inf_bits_<index> = OpShiftLeftLogical %uint %uint_255 %uint_23
%ht_quiet_bit_<index> = OpShiftLeftLogical %uint %uint_0x00000200 %uint_13
%ht_nan_bits_<index> = OpBitwiseOr %uint %ht_inf_bits_<index> %ht_quiet_bit_<index>
%ht_signed_nan_<index> = OpBitwiseOr %uint %ht_nan_bits_<index> %uint_0x80000000
%ht_special_value_<index> = OpBitcast %float %ht_signed_nan_<index>
%t_<index> = OpSelect %float %ht_special_<index> %ht_special_value_<index> %ht_finite_<index>
)").ReplaceStr("<trig>", cosine ? "Cos" : "Sin")
	    .ReplaceStr("<quadrants>", cosine ? R"(
%ht_axis_<index> = OpSelect %float %ht_is_quarter_<index> %<float_zero> %ht_approx_<index>
%ht_finite_<index> = OpSelect %float %ht_is_half_<index> %ht_negative_one_<index> %ht_axis_<index>
)" : R"(
%ht_negative_<index> = OpFOrdLessThan %bool %ht_turn_<index> %<float_zero>
%ht_peak_<index> = OpSelect %float %ht_negative_<index> %ht_negative_one_<index> %<float_one>
%ht_axis_<index> = OpSelect %float %ht_is_half_<index> %<float_zero> %ht_approx_<index>
%ht_peak_value_<index> = OpSelect %float %ht_is_quarter_<index> %ht_peak_<index> %ht_axis_<index>
%ht_zero_<index> = OpFOrdEqual %bool %h0_<index> %<float_zero>
%ht_finite_<index> = OpSelect %float %ht_zero_<index> %h0_<index> %ht_peak_value_<index>
)").ReplaceStr("<index>", index);
}

inline String8 Unary(const ShaderInstruction& inst, const char* const* params, const String8& index)
{
	// GLSL Log2/Sqrt have restricted domains. Classify guest special values
	// before evaluating them, then restore the ISA result explicitly.
	String8 text = R"(
%hu_nan_<index> = OpIsNan %bool %h0_<index>
%hu_inf_<index> = OpIsInf %bool %h0_<index>
%hu_special_<index> = OpLogicalOr %bool %hu_nan_<index> %hu_inf_<index>
%hu_negative_<index> = OpFOrdLessThan %bool %h0_<index> %<float_zero>
%hu_zero_<index> = OpFOrdEqual %bool %h0_<index> %<float_zero>
%hu_nonpositive_<index> = OpFOrdLessThanEqual %bool %h0_<index> %<float_zero>
%hu_finite_<index> = OpSelect %float %hu_special_<index> %<float_zero> %h0_<index>
)";
	if (inst.type == ShaderInstructionType::VLogF16)
	{
		text += "%hu_arg_<index> = OpSelect %float %hu_nonpositive_<index> %<float_one> %hu_finite_<index>\n";
		// Positive infinity was changed to zero above, which also must be
		// replaced before Log2; its result is restored below.
		text += "%hu_safe_<index> = OpSelect %float %hu_special_<index> %<float_one> %hu_arg_<index>\n";
	} else if (inst.type == ShaderInstructionType::VSqrtF16)
	{
		text += "%hu_safe_<index> = OpSelect %float %hu_negative_<index> %<float_zero> %hu_finite_<index>\n";
	} else if (inst.type == ShaderInstructionType::VExpF16)
	{
		// Beyond these bounds nearest-even half results already under/overflow.
		// Keep finite Exp2 results in binary32 range so FP16_OVFL can distinguish
		// a finite half overflow (65536 here) from a true +INF input below.
		text += R"(
%hu_max_<index> = OpConvertUToF %float %uint_16
%hu_underflow_<index> = OpConvertUToF %float %uint_25
%hu_min_<index> = OpFNegate %float %hu_underflow_<index>
%hu_safe_<index> = OpExtInst %float %GLSL_std_450 FClamp %hu_finite_<index> %hu_min_<index> %hu_max_<index>
)";
	} else
	{
		text += "%hu_safe_<index> = OpCopyObject %float %hu_finite_<index>\n";
	}
	for (int p = 0; p < 4; ++p)
	{
		if (params[p] != nullptr)
		{
			text += String8(params[p]).ReplaceStr("%h0_", "%hu_safe_").ReplaceStr("%t_", "%hu_calculated_") + "\n";
		}
	}
	text += "%hu_restored_<index> = OpSelect %float %hu_special_<index> %h0_<index> %hu_calculated_<index>\n";
	if (inst.type == ShaderInstructionType::VLogF16 || inst.type == ShaderInstructionType::VSqrtF16)
	{
		text += R"(
%hu_inf_bits_<index> = OpShiftLeftLogical %uint %uint_255 %uint_23
%hu_quiet_bit_<index> = OpShiftLeftLogical %uint %uint_0x00000200 %uint_13
%hu_neg_inf_bits_<index> = OpBitwiseOr %uint %hu_inf_bits_<index> %uint_0x80000000
%hu_neg_nan_bits_<index> = OpBitwiseOr %uint %hu_neg_inf_bits_<index> %hu_quiet_bit_<index>
%hu_neg_inf_<index> = OpBitcast %float %hu_neg_inf_bits_<index>
%hu_neg_nan_<index> = OpBitcast %float %hu_neg_nan_bits_<index>
%hu_validated_<index> = OpSelect %float %hu_negative_<index> %hu_neg_nan_<index> %hu_restored_<index>
%t_<index> = OpSelect %float %hu_zero_<index> %<zero> %hu_validated_<index>
)";
		text = text.ReplaceStr("<zero>", inst.type == ShaderInstructionType::VLogF16 ? "hu_neg_inf_<index>" : "h0_<index>");
	} else if (inst.type == ShaderInstructionType::VExpF16)
	{
		text += R"(
%hu_negative_inf_<index> = OpLogicalAnd %bool %hu_inf_<index> %hu_negative_<index>
%hu_tiny_<index> = OpFOrdLessThanEqual %bool %h0_<index> %hu_min_<index>
%hu_flush_<index> = OpLogicalOr %bool %hu_negative_inf_<index> %hu_tiny_<index>
%t_<index> = OpSelect %float %hu_flush_<index> %<float_zero> %hu_restored_<index>
)";
	} else
	{
		text += "%t_<index> = OpCopyObject %float %hu_restored_<index>\n";
	}
	return text.ReplaceStr("<index>", index);
}

inline bool LoadHalf(Spirv* spirv, const ShaderInstruction& inst, uint32_t source, const String8& index,
                     bool flush_input, bool integer, String8* text)
{
	auto operand = inst.src[source];
	if (operand.dpp || operand.swizzle != 6u || operand.size > 1 || (integer && (operand.negate || operand.absolute)))
	{
		return false;
	}
	const bool absolute = operand.absolute;
	const bool negate = operand.negate;
	operand.absolute = false;
	operand.negate = false;
	const String8 suffix = String8::FromPrintf("%u_", source) + index;
	String8 load;
	if (!operand_load_uint(spirv, operand, "fh_raw_" + suffix, index, &load)) { return false; }
	*text += load + "\n";
	String8 raw = "fh_raw_" + suffix;
	if (operand.type == ShaderOperandType::FloatInlineConstant)
	{
		// Inline floating constants are precision-dependent (ISA 6.2), not
		// the low half of a binary32 encoding. This also handles 1/(2*pi).
		// The architectural inline constants are finite and in half range;
		// their encoding is independent of the wave's overflow control.
		*text += Narrow(spirv, raw, "fh_inline_" + suffix, "inline_" + suffix, 0u, false, false);
		raw = "fh_inline_" + suffix;
	}
	const uint32_t half = (inst.vop3_op_sel >> source) & 1u;
	if (half != 0u && operand_is_constant(operand)) { return false; }
	*text += String8(R"(
%fh_abs_mask_<s> = OpBitwiseXor %uint %uint_0x0000ffff %<abs_sign>
%fh_selected_<s> = OpBitFieldUExtract %uint %<raw> %uint_<offset> %uint_16
%fh_abs_<s> = OpBitwiseAnd %uint %fh_selected_<s> %fh_abs_mask_<s>
%fh_modified_<s> = OpBitwiseXor %uint %fh_abs_<s> %<neg_mask>
)")
	             .ReplaceStr("<s>", suffix)
	             .ReplaceStr("<raw>", raw)
	             .ReplaceStr("<offset>", half != 0u ? "16" : "0")
	             .ReplaceStr("<abs_sign>", spirv->GetConstantUint(absolute ? 0x8000u : 0u))
	             .ReplaceStr("<neg_mask>", spirv->GetConstantUint(negate ? 0x8000u : 0u));
	if (flush_input)
	{
		*text += String8(R"(
%fh_exp_<s> = OpBitwiseAnd %uint %fh_modified_<s> %uint_0x00007c00
%fh_subnormal_<s> = OpIEqual %bool %fh_exp_<s> %uint_0
%fh_sign_<s> = OpBitwiseAnd %uint %fh_modified_<s> %uint_0x00008000
%fh_bits_<s> = OpSelect %uint %fh_subnormal_<s> %fh_sign_<s> %fh_modified_<s>
)").ReplaceStr("<s>", suffix);
	} else
	{
		*text += String8("%fh_bits_<s> = OpCopyObject %uint %fh_modified_<s>\n").ReplaceStr("<s>", suffix);
	}
	if (!integer)
	{
		*text += Expand("fh_bits_" + suffix, String8::FromPrintf("h%u_", source) + index, suffix);
	}
	return true;
}

// All arithmetic entry points share raw-bit storage. Ordinary VOP1/VOP2
// half results clear the upper word; native OPSEL operations preserve the
// other destination half. Inactive lanes preserve the entire original VGPR.
inline String8 Store(const ShaderInstruction& inst, const String8& index, const String8& value)
{
	String8 text = R"(
%fh_old_float_<index> = OpLoad %float %<dst>
%fh_old_<index> = OpBitcast %uint %fh_old_float_<index>
<merge>
%fh_exec_<index> = OpLoad %uint %exec_lo
%fh_active_<index> = OpINotEqual %bool %fh_exec_<index> %uint_0
%fh_stored_<index> = OpSelect %uint %fh_active_<index> %fh_merged_<index> %fh_old_<index>
%fh_stored_float_<index> = OpBitcast %float %fh_stored_<index>
OpStore %<dst> %fh_stored_float_<index>
)";
	const String8 merge = HasHalfDestinationSelect(inst.type) ?
	    "%fh_merged_<index> = OpBitFieldInsert %uint %fh_old_<index> %<value> %uint_<offset> %uint_16" :
	    "%fh_merged_<index> = OpCopyObject %uint %<value>";
	return text.ReplaceStr("<merge>", merge)
	    .ReplaceStr("<offset>", (inst.vop3_op_sel & 8u) != 0u ? "16" : "0")
	    .ReplaceStr("<value>", value)
	    .ReplaceStr("<dst>", operand_variable_to_str(inst.dst).value)
	    .ReplaceStr("<index>", index);
}

inline bool Emit(uint32_t instruction_index, const ShaderCode& code, String8* output, Spirv* spirv,
                 const char* const* params, Operation operation)
{
	const auto& inst = code.GetInstructions().At(instruction_index);
	const auto reject = [&](const char* reason)
	{
		// Keep the failed contract alongside the generator's final diagnostic.
		// A generic missing-emitter exit alone cannot identify a mode refusal.
		std::fprintf(stderr, "shader fp16 unsupported: stage=%u instruction=%u pc=0x%08" PRIx32 " reason=%s\n",
		             static_cast<unsigned>(code.GetType()), static_cast<unsigned>(inst.type), inst.pc, reason);
		return false;
	};
	const String8 index = String8::FromPrintf("%u", instruction_index);
	const bool integer_input = operation == Operation::FromSigned || operation == Operation::FromUnsigned;
	const bool integer_output = operation == Operation::ToSigned || operation == Operation::ToUnsigned;
	const bool arithmetic = operation == Operation::Unary || operation == Operation::Binary || operation == Operation::Ternary;
	const int sources = operation == Operation::Ternary ? 3 : (operation == Operation::Binary ? 2 : 1);
	Mode mode;
	// Stage metadata also owns the shared constant declarations. Widening is
	// mode-independent, but still needs an actual graphics/compute interface.
	if (spirv->GetVsInputInfo() == nullptr && spirv->GetPsInputInfo() == nullptr && spirv->GetCsInputInfo() == nullptr)
	{
		return reject("missing-stage-metadata");
	}
	if (inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size != 1 || inst.src_num != sources)
	{
		return reject("invalid-operand-tuple");
	}
	if (inst.vop_sdwa) { return reject("unsupported-sdwa"); }
	if (inst.dst.multiplier != 1.0f || inst.vop3_omod != 0u) { return reject("unsupported-omod"); }
	if (!HasHalfDestinationSelect(inst.type) && inst.vop3_op_sel != 0u) { return reject("unsupported-opsel"); }
	for (int source = 0; source < sources; ++source)
	{
		if (inst.src[source].dpp) { return reject("unsupported-dpp"); }
	}
	if (!ReadMode(spirv, &mode) && (operation != Operation::ToFloat || inst.dst.clamp))
	{
		return reject("unknown-initial-mode");
	}
	// FP16_OVFL is separate from FLOAT_MODE/IEEE/DX10 provenance. Default
	// construction must not silently select disabled saturation for half results.
	if (!integer_output && operation != Operation::ToFloat && !mode.fp16_overflow_known)
	{
		return reject("unknown-fp16-overflow-mode");
	}
	// The integer narrowing below implements every static half rounding mode.
	// Arithmetic uses binary32 intermediates: admit nearest-even only; IEEE
	// signaling-NaN precedence/exception state needs separate modeling. OMOD,
	// SDWA and DPP are refused above rather than dropping their semantics.
	if (arithmetic && ((mode.value >> 2u) & 3u) != 0u)
	{
		return reject("unsupported-arithmetic-round-mode");
	}
	if (arithmetic && mode.ieee) { return reject("unsupported-arithmetic-ieee-mode"); }
	if (integer_output && inst.dst.clamp) { return reject("unsupported-integer-clamp"); }
	// Do not conflate saturation with OMOD/denormal-mode interactions. Admit
	// half saturation only with output denormals disabled; the preservation
	// combination needs a separate contract before applying an output modifier.
	if (inst.dst.clamp && !mode.ieee && (mode.value & 0x80u) != 0u) { return reject("unsupported-output-denorm-clamp"); }
	// Legacy MAD's intermediate rounding is not the fused-half contract.
	if (inst.type == ShaderInstructionType::VMadF16) { return reject("unsupported-legacy-mad"); }
	String8 text;
	if (operation == Operation::FromFloat)
	{
		auto source = inst.src[0];
		if (source.swizzle != 6u || source.size > 1) { return reject("unsupported-source-operand"); }
		const bool absolute = source.absolute;
		const bool negate = source.negate;
		source.absolute = false;
		source.negate = false;
		if (!operand_load_uint(spirv, source, "fh_source_<index>", index, &text)) { return reject("unsupported-source-operand"); }
		text += String8(R"(
%fh_absolute_<index> = OpBitwiseAnd %uint %fh_source_<index> %<abs_mask>
%fh_modified_bits_<index> = OpBitwiseXor %uint %fh_absolute_<index> %<neg_mask>
)").ReplaceStr("<abs_mask>", spirv->GetConstantUint(absolute ? 0x7fffffffu : 0xffffffffu))
		         .ReplaceStr("<neg_mask>", spirv->GetConstantUint(negate ? 0x80000000u : 0u));
		if ((mode.value & 0x10u) == 0u)
		{
			// Source precision selects FP_DENORM's input control. The
			// conversion's explicit creation of half denormals overrides
			// only the half output-denormal control.
			text += R"(
%fh_source_exp_<index> = OpBitFieldUExtract %uint %fh_modified_bits_<index> %uint_23 %uint_8
%fh_source_denorm_<index> = OpIEqual %bool %fh_source_exp_<index> %uint_0
%fh_source_sign_<index> = OpBitwiseAnd %uint %fh_modified_bits_<index> %uint_0x80000000
%fh_value_bits_<index> = OpSelect %uint %fh_source_denorm_<index> %fh_source_sign_<index> %fh_modified_bits_<index>
)";
		} else
		{
			text += "%fh_value_bits_<index> = OpCopyObject %uint %fh_modified_bits_<index>\n";
		}
	} else
	{
		for (int source = 0; source < sources; ++source)
		{
			if (!LoadHalf(spirv, inst, source, index, arithmetic && (mode.value & 0x40u) == 0u, integer_input, &text))
			{
				return reject("unsupported-source-operand");
			}
		}
		if (integer_input)
		{
			if (operation == Operation::FromSigned)
			{
				text += R"(
%fh_int_<index> = OpBitcast %int %fh_bits_0_<index>
%fh_signed_<index> = OpBitFieldSExtract %int %fh_int_<index> %uint_0 %uint_16
%t_<index> = OpConvertSToF %float %fh_signed_<index>
)";
			} else
			{
				text += "%t_<index> = OpConvertUToF %float %fh_bits_0_<index>\n";
			}
		} else if (integer_output)
		{
			// Saturate before conversion: OpConvertFTo[SU] is undefined for
			// NaN, infinity, or an out-of-range integer result.
			text += R"(
%fh_nan_<index> = OpIsNan %bool %h0_<index>
%fh_numeric_<index> = OpSelect %float %fh_nan_<index> %<float_zero> %h0_<index>
%fh_max_u_<index> = OpShiftRightLogical %uint %uint_0x0000ffff %uint_<signed>
%fh_max_<index> = OpConvertUToF %float %fh_max_u_<index>
%fh_min_u_<index> = OpCopyObject %uint %<min>
%fh_min_f_<index> = OpConvertUToF %float %fh_min_u_<index>
%fh_min_<index> = OpFNegate %float %fh_min_f_<index>
%fh_saturated_<index> = OpExtInst %float %GLSL_std_450 FClamp %fh_numeric_<index> %fh_min_<index> %fh_max_<index>
%fh_int_<index> = OpConvertFToS %int %fh_saturated_<index>
%fh_uint_<index> = OpBitcast %uint %fh_int_<index>
%fh_result_<index> = OpBitwiseAnd %uint %fh_uint_<index> %uint_0x0000ffff
)";
			text = text.ReplaceStr("<signed>", operation == Operation::ToSigned ? "1" : "0")
			           .ReplaceStr("<min>", spirv->GetConstantUint(operation == Operation::ToSigned ? 0x8000u : 0u));
		} else if (operation == Operation::ToFloat)
		{
			text += "%fh_result_<index> = OpBitcast %uint %h0_<index>\n";
		} else if (inst.type == ShaderInstructionType::VMinF16 || inst.type == ShaderInstructionType::VMaxF16)
		{
			text += MinMax("h0_<index>", "h1_<index>", "t_<index>", index, inst.type == ShaderInstructionType::VMaxF16);
		} else if (inst.type == ShaderInstructionType::VFmaF16)
		{
			// A product of two finite halves is exact in binary32 (22
			// significant bits, exponent range -48..31). Recover the addition
			// error's direction in integers for final half rounding so an
			// intermediate binary32 tie cannot introduce double rounding.
			text += R"(
%fh_product_<index> = OpFMul %float %h0_<index> %h1_<index>
%t_<index> = OpFAdd %float %fh_product_<index> %h2_<index>
)";
			text += FmaResidue(index);
		} else if (operation == Operation::Ternary)
		{
			const bool maximum = inst.type == ShaderInstructionType::VMax3F16;
			text += MinMax("h0_<index>", "h1_<index>", "fh_min01_<index>", "min01_" + index, maximum);
			text += MinMax("fh_min01_<index>", "h2_<index>", "fh_min3_<index>", "min3_" + index, maximum);
			if (inst.type == ShaderInstructionType::VMed3F16)
			{
				text += MinMax("h0_<index>", "h1_<index>", "fh_max01_<index>", "max01_" + index, true);
				text += MinMax("h0_<index>", "h2_<index>", "fh_max02_<index>", "max02_" + index, true);
				text += MinMax("h1_<index>", "h2_<index>", "fh_max12_<index>", "max12_" + index, true);
				text += MinMax("fh_max01_<index>", "h2_<index>", "fh_max3_<index>", "max3_" + index, true);
				text += R"(
%fh_max_is0_<index> = OpFOrdEqual %bool %fh_max3_<index> %h0_<index>
%fh_max_is1_<index> = OpFOrdEqual %bool %fh_max3_<index> %h1_<index>
%fh_med12_<index> = OpSelect %float %fh_max_is1_<index> %fh_max02_<index> %fh_max01_<index>
%fh_med_<index> = OpSelect %float %fh_max_is0_<index> %fh_max12_<index> %fh_med12_<index>
%fh_nan0_<index> = OpIsNan %bool %h0_<index>
%fh_nan1_<index> = OpIsNan %bool %h1_<index>
%fh_nan2_<index> = OpIsNan %bool %h2_<index>
%fh_nan01_<index> = OpLogicalOr %bool %fh_nan0_<index> %fh_nan1_<index>
%fh_nan_any_<index> = OpLogicalOr %bool %fh_nan01_<index> %fh_nan2_<index>
%t_<index> = OpSelect %float %fh_nan_any_<index> %fh_min3_<index> %fh_med_<index>
)";
			} else
			{
				text += "%t_<index> = OpCopyObject %float %fh_min3_<index>\n";
			}
		} else if (inst.type == ShaderInstructionType::VSinF16 || inst.type == ShaderInstructionType::VCosF16)
		{
			text += Trig(index, inst.type == ShaderInstructionType::VCosF16);
		} else if (operation == Operation::Unary)
		{
			text += Unary(inst, params, index);
		} else
		{
			for (int p = 0; p < 4; ++p)
			{
				if (params[p] != nullptr)
				{
					text += String8(params[p]).ReplaceStr("%hf0_", "%h0_").ReplaceStr("%hf1_", "%h1_") + "\n";
				}
			}
		}
		if (!integer_output && operation != Operation::ToFloat)
		{
			text += "%fh_value_bits_<index> = OpBitcast %uint %t_<index>\n";
		}
	}
	if (!integer_output && operation != Operation::ToFloat)
	{
		// CVT_F16_F32 explicitly creates denormals irrespective of FP_DENORM.
		text += Narrow(spirv, "fh_value_bits_<index>", "fh_result_<index>", index, (mode.value >> 2u) & 3u,
		               arithmetic && (mode.value & 0x80u) == 0u, mode.fp16_overflow,
		               inst.type == ShaderInstructionType::VFmaF16 ? "fh_residue_<index>" : "");
	}
	if (inst.dst.clamp)
	{
		// Clamp of half results is exact. Handle NaN in bits according to
		// DX10_CLAMP; IEEE disables this output modifier, as for f32.
		if (!mode.ieee)
		{
			// F32 conversion destinations need a distinct float clamp contract.
			if (operation == Operation::ToFloat) { return reject("unsupported-widening-clamp"); }
			text += R"(
%fh_clamp_magnitude_<index> = OpBitFieldUExtract %uint %fh_result_<index> %uint_0 %uint_15
%fh_clamp_sign_<index> = OpBitwiseAnd %uint %fh_result_<index> %uint_0x00008000
%fh_clamp_nan_<index> = OpUGreaterThan %bool %fh_clamp_magnitude_<index> %uint_0x00007c00
%fh_clamp_low_<index> = OpINotEqual %bool %fh_clamp_sign_<index> %uint_0
%fh_one_<index> = OpShiftLeftLogical %uint %uint_15 %uint_10
%fh_clamp_nonnegative_<index> = OpSelect %uint %fh_clamp_low_<index> %uint_0 %fh_result_<index>
%fh_clamp_high_<index> = OpUGreaterThan %bool %fh_clamp_nonnegative_<index> %fh_one_<index>
%fh_clamp_finite_<index> = OpSelect %uint %fh_clamp_high_<index> %fh_one_<index> %fh_clamp_nonnegative_<index>
%fh_clamped_<index> = OpSelect %uint %fh_clamp_nan_<index> %<nan_result> %fh_clamp_finite_<index>
)";
			text = text.ReplaceStr("<nan_result>", mode.dx10_clamp ? "uint_0" : "fh_result_<index>");
			text += Store(inst, index, "fh_clamped_<index>");
		} else
		{
			text += Store(inst, index, "fh_result_<index>");
		}
	} else
	{
		text += Store(inst, index, "fh_result_<index>");
	}
	// FindConstants registers these values under the constant pool's own IDs.
	// Resolve placeholders, not guessed aliases or prefixes: replacing float_0
	// as a substring would also corrupt the independently resolved 0.5 ID.
	*output += text.ReplaceStr("<index>", index)
	               .ReplaceStr("<float_zero>", spirv->GetConstantFloat(0.0f))
	               .ReplaceStr("<float_one>", spirv->GetConstantFloat(1.0f))
	               .ReplaceStr("<float_half>", spirv->GetConstantFloat(0.5f))
	               .ReplaceStr("uint_112", spirv->GetConstantUint(112))
	               .ReplaceStr("uint_255", spirv->GetConstantUint(255));
	return true;
}

} // namespace Kyty::Libs::Graphics::F16Arithmetic

#endif // KYTY_EMU_ENABLED
#endif // KYTY_SHADER_SPIRV_F16_H_
