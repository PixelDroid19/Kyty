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
	// A source read as binary32 enters the half operation as a half: it is
	// loaded as <tag>_wide, converted and widened back into <tag> so the fused
	// operation sees the hardware's input.
	const auto reads_binary32 = [&](uint32_t operand) { return (inst.vop3p_op_sel_hi & (1u << operand)) == 0u; };
	for (uint32_t operand = 0; operand < 3u; ++operand)
	{
		const String8 tag = String8::FromPrintf("h%u_", operand) + index_str;
		if (!LoadMixedFloat(spirv, inst, operand, reads_binary32(operand) ? tag + "_wide" : tag, &source))
		{
			return false;
		}
	}
	for (uint32_t operand = 0; operand < 3u; ++operand)
	{
		if (!reads_binary32(operand))
		{
			continue;
		}
		const String8 tag = String8::FromPrintf("h%u_", operand) + index_str;
		source += String8("%<t>_bits = OpBitcast %uint %<t>_wide\n").ReplaceStr("<t>", tag);
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

// V_PK_FMA_F16, V_PK_ADD_F16 and V_PK_MUL_F16: two independent half
// operations. OPSEL and NEG select the source halves and their signs for the
// low result, OPSEL_HI and NEG_HI for the high one. binary32 holds
// 24 >= 2 * 11 + 2 bits, so a binary32 sum or product of two halves rounded
// once more to half equals the directly rounded half result; the fused form
// recovers its addition residue as V_FMA_F16 does.
KYTY_RECOMPILER_FUNC(Recompile_VPkF16)
{
	const auto& inst = code.GetInstructions().At(index);
	const bool  fused = inst.type == ShaderInstructionType::VPkFmaF16;
	const auto  sources = static_cast<uint32_t>(fused ? 3 : 2);
	if (inst.src_num != static_cast<int>(sources) || inst.vop3p_op_sel_hi > 7u || inst.vop3_op_sel > 7u ||
	    inst.vop3_omod != 0u || inst.vop_sdwa || inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size != 1 ||
	    inst.dst.register_id < 0 || inst.dst.register_id > 255 || inst.dst.multiplier != 1.0f || inst.dst.clamp ||
	    inst.dst.absolute || inst.dst.negate || inst.dst.dpp || inst.dst.swizzle != 6u)
	{
		return false;
	}
	for (uint32_t operand = 0; operand < sources; ++operand)
	{
		if (operand_is_constant(inst.src[operand])) { return false; }
	}
	F16Arithmetic::Mode mode;
	if (!F16Arithmetic::ReadMode(spirv, &mode) || !mode.fp16_overflow_known)
	{
		return false;
	}
	// As for the other half arithmetic: nearest-even without IEEE exception state.
	if (((mode.value >> 2u) & 3u) != 0u || mode.ieee)
	{
		return false;
	}
	const String8 index_str = String8::FromPrintf("%u", index);
	String8       source;
	for (uint32_t high = 0; high < 2u; ++high)
	{
		ShaderInstruction half = inst;
		half.vop3_op_sel = high != 0u ? inst.vop3p_op_sel_hi : inst.vop3_op_sel;
		for (uint32_t operand = 0; operand < sources; ++operand)
		{
			half.src[operand].negate = high != 0u ? inst.src[operand].absolute : inst.src[operand].negate;
			half.src[operand].absolute = false;
		}
		const String8 half_index = index_str + (high != 0u ? "_hi" : "_lo");
		for (uint32_t operand = 0; operand < sources; ++operand)
		{
			if (!F16Arithmetic::LoadHalf(spirv, half, operand, half_index, (mode.value & 0x40u) == 0u, false, &source))
			{
				return false;
			}
		}
		if (fused)
		{
			source += String8(R"(
%fh_product_<index> = OpFMul %float %h0_<index> %h1_<index>
%t_<index> = OpFAdd %float %fh_product_<index> %h2_<index>
)").ReplaceStr("<index>", half_index);
			source += F16Arithmetic::FmaResidue(half_index);
		} else if (inst.type == ShaderInstructionType::VPkMinF16 || inst.type == ShaderInstructionType::VPkMaxF16)
		{
			source += F16Arithmetic::MinMax("h0_" + half_index, "h1_" + half_index, "t_" + half_index, half_index,
			                                inst.type == ShaderInstructionType::VPkMaxF16);
		} else
		{
			source += String8(param[0]).ReplaceStr("<index>", half_index) + "\n";
		}
		source += String8("%fh_value_bits_<index> = OpBitcast %uint %t_<index>\n").ReplaceStr("<index>", half_index);
		source += F16Arithmetic::Narrow(spirv, "fh_value_bits_" + half_index, "fh_result_" + half_index, half_index, 0u,
		                                (mode.value & 0x80u) == 0u, mode.fp16_overflow,
		                                fused ? "fh_residue_" + half_index : String8());
	}
	source += String8("%fh_packed_<index> = OpBitFieldInsert %uint %fh_result_<index>_lo %fh_result_<index>_hi %uint_16 %uint_16\n")
	              .ReplaceStr("<index>", index_str);
	source += F16Arithmetic::Store(inst, index_str, "fh_packed_" + index_str);
	*dst_source += source.ReplaceStr("<float_zero>", spirv->GetConstantFloat(0.0f))
	                   .ReplaceStr("uint_112", spirv->GetConstantUint(112))
	                   .ReplaceStr("uint_255", spirv->GetConstantUint(255));
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
