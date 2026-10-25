#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvF16.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static bool LoadMixedFloat(Spirv* spirv, const ShaderInstruction& inst, uint32_t source_index,
                           const String8& tag, String8* source)
{
	const auto& operand = inst.src[source_index];
	if (operand.dpp || operand.swizzle != 6u || operand.multiplier != 1.0f || operand.clamp)
	{
		return false;
	}
	auto raw = operand;
	raw.absolute = false;
	raw.negate = false;
	String8 load;
	const bool half = (inst.vop3p_op_sel_hi & (1u << source_index)) != 0u;
	if (half)
	{
		// The decoder materializes inline FP16 constants at their own width.
		if (raw.type == ShaderOperandType::FloatInlineConstant ||
		    !operand_load_uint(spirv, raw, tag + "_bits", tag, &load))
		{
			return false;
		}
		const uint32_t component = (inst.vop3_op_sel >> source_index) & 1u;
		load += String8::FromPrintf("\n%%%s_halves = OpExtInst %%v2float %%GLSL_std_450 UnpackHalf2x16 %%%s_bits\n"
		                           "%%%s_raw = OpCompositeExtract %%float %%%s_halves %u\n",
		                           tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), component);
	} else if (!operand_load_float(spirv, raw, tag + "_raw", tag, &load))
	{
		return false;
	}
	*source += load + "\n";
	const auto magnitude = operand.absolute ? tag + "_abs" : tag + "_raw";
	if (operand.absolute)
	{
		*source += String8::FromPrintf("%%%s = OpExtInst %%float %%GLSL_std_450 FAbs %%%s_raw\n",
		                              magnitude.c_str(), tag.c_str());
	}
	*source += String8::FromPrintf("%%%s = %s %%float %%%s\n", tag.c_str(),
	                              operand.negate ? "OpFNegate" : "OpCopyObject", magnitude.c_str());
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VFmaMixF32)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.src_num != 3 || inst.vop3p_op_sel_hi > 7u || inst.vop3_op_sel > 7u || inst.vop3_omod != 0u ||
	    inst.vop_sdwa || inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size != 1 ||
	    inst.dst.register_id < 0 || inst.dst.register_id > 255 || inst.dst.multiplier != 1.0f || inst.dst.clamp ||
	    inst.dst.absolute || inst.dst.negate || inst.dst.dpp || inst.dst.swizzle != 6u)
	{
		return false;
	}
	const auto tag = String8::FromPrintf("mix_%u", index);
	String8 source;
	for (uint32_t operand = 0; operand < 3u; ++operand)
	{
		if (!LoadMixedFloat(spirv, inst, operand, tag + String8::FromPrintf("_s%u", operand), &source)) { return false; }
	}
	// Convert and capture all operands before updating an aliased destination.
	// Fma retains one FP32 rounding step after the exact FP16 expansions.
	source += String8(R"(
%<t>_result = OpExtInst %float %GLSL_std_450 Fma %<t>_s0 %<t>_s1 %<t>_s2
%<t>_exec = OpLoad %uint %exec_lo
%<t>_active = OpINotEqual %bool %<t>_exec %uint_0
OpSelectionMerge %<t>_done None
OpBranchConditional %<t>_active %<t>_write %<t>_done
%<t>_write = OpLabel
OpStore %<dst> %<t>_result
OpBranch %<t>_done
%<t>_done = OpLabel
)").ReplaceStr("<t>", tag).ReplaceStr("<dst>", operand_variable_to_str(inst.dst).value);
	*dst_source += source;
	return true;
}

// V_FMA_MIXLO_F16 and V_FMA_MIXHI_F16: the mixed-precision FMA of the
// selected sources, rounded to half and written to the low or the high half
// of the destination. The other half is kept. The arithmetic follows the engine's half contract:
// binary32 intermediates with the addition residue recovered, then one
// rounding of the result to half.
KYTY_RECOMPILER_FUNC(Recompile_VFmaMixLoF16)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.src_num != 3 || inst.vop3p_op_sel_hi > 7u || inst.vop3_op_sel > 7u || inst.vop3_omod != 0u ||
	    inst.vop_sdwa || inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size != 1 ||
	    inst.dst.register_id < 0 || inst.dst.register_id > 255 || inst.dst.multiplier != 1.0f || inst.dst.clamp ||
	    inst.dst.absolute || inst.dst.negate || inst.dst.dpp || inst.dst.swizzle != 6u)
	{
		return false;
	}
	F16Arithmetic::Mode mode;
	if (!F16Arithmetic::ReadMode(spirv, &mode) || !mode.fp16_overflow_known)
	{
		return false;
	}
	// The half rounding implements every static rounding mode, but the
	// arithmetic keeps binary32 intermediates: only nearest-even without IEEE
	// exception state is admitted.
	if (((mode.value >> 2u) & 3u) != 0u || mode.ieee)
	{
		return false;
	}
	const String8 index_str = String8::FromPrintf("%u", index);
	String8       source;
	for (uint32_t operand = 0; operand < 3u; ++operand)
	{
		const String8 tag = String8::FromPrintf("h%u_", operand) + index_str;
		if (!LoadMixedFloat(spirv, inst, operand, tag, &source))
		{
			return false;
		}
	}
	// A source read as binary32 enters the half operation as a half: convert it
	// and widen it back so the fused operation sees the hardware's input.
	for (uint32_t operand = 0; operand < 3u; ++operand)
	{
		if ((inst.vop3p_op_sel_hi & (1u << operand)) != 0u)
		{
			continue;
		}
		const String8 tag = String8::FromPrintf("h%u_", operand) + index_str;
		source += String8("%<t>_bits = OpBitcast %uint %<t>\n").ReplaceStr("<t>", tag);
		source += F16Arithmetic::Narrow(spirv, tag + "_bits", tag + "_half", tag, 0u, (mode.value & 0x80u) == 0u,
		                                mode.fp16_overflow, "");
		source += F16Arithmetic::Expand(tag + "_half", tag + "_narrow", tag);
		source += String8("%<t> = OpCopyObject %float %<t>_narrow\n").ReplaceStr("<t>", tag);
	}
	// A product of two finite halves is exact in binary32; the addition error
	// is recovered in integers so an intermediate binary32 tie cannot introduce
	// double rounding.
	source += String8(R"(
%fh_product_<index> = OpFMul %float %h0_<index> %h1_<index>
%t_<index> = OpFAdd %float %fh_product_<index> %h2_<index>
)").ReplaceStr("<index>", index_str);
	source += F16Arithmetic::FmaResidue(index_str);
	source += String8("%fh_value_bits_<index> = OpBitcast %uint %t_<index>\n").ReplaceStr("<index>", index_str);
	source += F16Arithmetic::Narrow(spirv, "fh_value_bits_" + index_str, "fh_result_" + index_str, index_str, 0u,
	                                (mode.value & 0x80u) == 0u, mode.fp16_overflow, "fh_residue_" + index_str);
	// The low half of the destination is written, the high half is kept.
	source += String8(R"(
%fh_old_float_<index> = OpLoad %float %<dst>
%fh_old_<index> = OpBitcast %uint %fh_old_float_<index>
%fh_merged_<index> = OpBitFieldInsert %uint %fh_old_<index> %fh_result_<index> %uint_<offset> %uint_16
%fh_exec_<index> = OpLoad %uint %exec_lo
%fh_active_<index> = OpINotEqual %bool %fh_exec_<index> %uint_0
%fh_stored_<index> = OpSelect %uint %fh_active_<index> %fh_merged_<index> %fh_old_<index>
%fh_stored_float_<index> = OpBitcast %float %fh_stored_<index>
OpStore %<dst> %fh_stored_float_<index>
)").ReplaceStr("<index>", index_str).ReplaceStr("<offset>", (param[0] != nullptr && param[0][0] == 'h') ? "16" : "0")
	   .ReplaceStr("<dst>", operand_variable_to_str(inst.dst).value);
	*dst_source += source;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
