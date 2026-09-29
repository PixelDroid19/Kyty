#include "ShaderSpirvEmitters.h"

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

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
