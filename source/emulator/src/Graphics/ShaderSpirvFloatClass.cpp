#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvInternal.h"
#include "ShaderSpirvTemplates.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// The sign, exponent and quiet bits of a 16-, 32- or 64-bit float; a double keeps them in its high word.
struct FloatClassMasks
{
	uint32_t exponent;
	uint32_t fraction;
	uint32_t quiet;
	uint32_t sign;
	uint32_t magnitude;
};

static const FloatClassMasks& FloatClassMasksFor(int width)
{
	static const FloatClassMasks half {0x7c00u, 0x03ffu, 0x0200u, 0x8000u, 0x7fffu};
	static const FloatClassMasks single {0x7f800000u, 0x007fffffu, 0x00400000u, 0x80000000u, 0x7fffffffu};
	static const FloatClassMasks wide {0x7ff00000u, 0x000fffffu, 0x00080000u, 0x80000000u, 0x7fffffffu};
	return width == 16 ? half : (width == 32 ? single : wide);
}

// v_cmp(x)_class_f16/f32/f64. A v_cmpx writes the lane bit ANDed with EXEC to EXEC and leaves VCC alone.
KYTY_RECOMPILER_FUNC(Recompile_VCmpClass)
{
	const auto& instruction = code.GetInstructions().At(index);
	if (!ShaderFloatClassComparisonSupported(instruction)) { return false; }
	const int  width        = ShaderFloatClassWidth(instruction.type);
	const bool exec_compare = ShaderInstructionTypeStartsWith(instruction.type, "VCmpx");
	const auto dst0 = operand_variable_to_str(instruction.dst, 0);
	const auto dst1 = operand_variable_to_str(instruction.dst, 1);
	const auto i = String8::FromPrintf("%u", index);
	ShaderOperand source = instruction.src[0];
	source.absolute      = false;
	source.negate        = false;
	String8 value;
	String8 mask;
	if (!exec_compare && (dst0.type != SpirvType::Uint || dst1.type != SpirvType::Uint)) { return false; }
	if (width == 64)
	{
		// The high word holds sign, exponent and quiet bit, with abs and neg applied; the low word only adds fraction bits.
		String8 low;
		if (!operand_load_f64_word(spirv, instruction.src[0], "class_raw_<index>", i, &value, 1) ||
		    !operand_load_f64_word(spirv, source, "class_low_<index>", i, &low, 0)) { return false; }
		value += "\n" + low;
	} else if (!operand_load_uint(spirv, source, "class_raw_<index>", i, &value))
	{
		return false;
	}
	if (!operand_load_uint(spirv, instruction.src[1], "class_mask_<index>", i, &mask)) { return false; }
	// Integer classification preserves NaN payloads and subnormals regardless
	// of the host's floating-point flushing and NaN canonicalization modes.
	static const char* text = R"(
<value>
<mask>
%class_value_<index> = <modifier>
%class_exponent_<index> = OpBitwiseAnd %uint %class_value_<index> %<exponent_mask>
%class_fraction_<index> = OpBitwiseAnd %uint %class_value_<index> %<fraction_mask>
<fraction_low>
%class_sign_<index> = OpBitwiseAnd %uint %class_value_<index> %<sign_mask>
%class_quiet_<index> = OpBitwiseAnd %uint %class_value_<index> %<quiet_mask>
%class_negative_<index> = OpINotEqual %bool %class_sign_<index> %uint_0
%class_exp_zero_<index> = OpIEqual %bool %class_exponent_<index> %uint_0
%class_exp_ones_<index> = OpIEqual %bool %class_exponent_<index> %<exponent_mask>
%class_frac_zero_<index> = OpIEqual %bool %<fraction_word> %uint_0
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
<store>
)";
	static const char* vcc_store = R"(%class_exec_<index> = OpLoad %uint %exec_lo
%class_active_<index> = OpINotEqual %bool %class_exec_<index> %uint_0
%class_result_<index> = OpSelect %uint %class_active_<index> %t3_<index> %uint_0
OpStore %<dst0> %class_result_<index>
OpStore %<dst1> %uint_0)";
	// Input modifiers only rewrite the sign bit; NaN payloads are kept.
	const auto& src0     = instruction.src[0];
	String8     modifier = "OpCopyObject %uint %class_raw_<index>";
	if (src0.absolute && src0.negate) { modifier = "OpBitwiseOr %uint %class_raw_<index> %<sign_mask>"; }
	else if (src0.absolute) { modifier = "OpBitwiseAnd %uint %class_raw_<index> %<magnitude_mask>"; }
	else if (src0.negate) { modifier = "OpBitwiseXor %uint %class_raw_<index> %<sign_mask>"; }
	if (width == 64) { modifier = "OpCopyObject %uint %class_raw_<index>"; } // operand_load_f64_word applied abs and neg
	const auto& masks         = FloatClassMasksFor(width);
	const auto  fraction_low =
	    width == 64 ? String8("%class_fraction_any_<index> = OpBitwiseOr %uint %class_fraction_<index> %class_low_<index>") : String8 {};
	const auto  fraction_word = width == 64 ? String8("class_fraction_any_<index>") : String8("class_fraction_<index>");
	const auto  store         = exec_compare ? String8(CMPX_EXEC_STORE).ReplaceStr("<execz>", EXECZ) : String8(vcc_store);
	*dst_source += String8(text).ReplaceStr("<store>", store).ReplaceStr("<fraction_low>", fraction_low)
	                           .ReplaceStr("<fraction_word>", fraction_word).ReplaceStr("<modifier>", modifier)
	                           .ReplaceStr("<value>", value).ReplaceStr("<mask>", mask)
	                           .ReplaceStr("<index>", i)
	                           .ReplaceStr("<exponent_mask>", spirv->GetConstantUint(masks.exponent))
	                           .ReplaceStr("<fraction_mask>", spirv->GetConstantUint(masks.fraction))
	                           .ReplaceStr("<quiet_mask>", spirv->GetConstantUint(masks.quiet))
	                           .ReplaceStr("<sign_mask>", spirv->GetConstantUint(masks.sign))
	                           .ReplaceStr("<magnitude_mask>", spirv->GetConstantUint(masks.magnitude))
	                           .ReplaceStr("<dst0>", dst0.value).ReplaceStr("<dst1>", dst1.value);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
