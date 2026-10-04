#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// s_bitcmp{0,1}_b{32,64}: SCC = (S0[S1 bit index] == param[0]). The b64 form
// indexes the 64-bit pair with S1[5:0].
KYTY_RECOMPILER_FUNC(Recompile_SBitcmp_XXX)
{
	const auto& inst  = code.GetInstructions().At(index);
	const bool  wide  = inst.src[0].size == 2;
	const auto  index_str = String8::FromPrintf("%u", index);
	String8     load_lo;
	String8     load_hi;
	String8     load_bit;
	if (!operand_load_uint(spirv, inst.src[0], "bc_lo_<index>", index_str, &load_lo, wide ? 0 : -1) ||
	    (wide && !operand_load_uint(spirv, inst.src[0], "bc_hi_<index>", index_str, &load_hi, 1)) ||
	    !operand_load_uint(spirv, inst.src[1], "bc_bit_<index>", index_str, &load_bit))
	{
		return false;
	}
	String8 source = load_lo + "\n" + load_hi + "\n" + load_bit + "\n";
	if (wide)
	{
		source += R"(
%bc_index_<index> = OpBitwiseAnd %uint %bc_bit_<index> %uint_63
%bc_high_<index> = OpUGreaterThanEqual %bool %bc_index_<index> %uint_32
%bc_word_<index> = OpSelect %uint %bc_high_<index> %bc_hi_<index> %bc_lo_<index>
)";
	} else
	{
		source += "%bc_index_<index> = OpBitwiseAnd %uint %bc_bit_<index> %uint_31\n%bc_word_<index> = OpCopyObject %uint %bc_lo_<index>\n";
	}
	source += R"(
%bc_shift_<index> = OpBitwiseAnd %uint %bc_index_<index> %uint_31
%bc_moved_<index> = OpShiftRightLogical %uint %bc_word_<index> %bc_shift_<index>
%bc_value_<index> = OpBitwiseAnd %uint %bc_moved_<index> %uint_1
%bc_match_<index> = OpIEqual %bool %bc_value_<index> %uint_<expected>
%bc_scc_<index> = OpSelect %uint %bc_match_<index> %uint_1 %uint_0
               OpStore %scc %bc_scc_<index>
)";
	*dst_source += source.ReplaceStr("<expected>", param[0]).ReplaceStr("<index>", index_str);
	return true;
}

// s_bcnt{0,1}_i32_b{32,64}: number of clear (param[0] == "0") or set bits of the
// source; SCC = (result != 0).
KYTY_RECOMPILER_FUNC(Recompile_SBcnt_XXX)
{
	const auto& inst      = code.GetInstructions().At(index);
	const bool  wide      = inst.src[0].size == 2;
	const bool  count_set = param[0][0] == '1';
	const auto  dst       = operand_variable_to_str(inst.dst);
	const auto  index_str = String8::FromPrintf("%u", index);
	String8     load_lo;
	String8     load_hi;
	if (dst.type != SpirvType::Uint || !operand_load_uint(spirv, inst.src[0], "bn_lo_<index>", index_str, &load_lo, wide ? 0 : -1) ||
	    (wide && !operand_load_uint(spirv, inst.src[0], "bn_hi_<index>", index_str, &load_hi, 1)))
	{
		return false;
	}
	String8 source = load_lo + "\n" + load_hi + "\n";
	if (!count_set)
	{
		source += "%bn_lo_in_<index> = OpNot %uint %bn_lo_<index>\n";
		source += wide ? "%bn_hi_in_<index> = OpNot %uint %bn_hi_<index>\n" : "";
	}
	const char* lo = count_set ? "bn_lo" : "bn_lo_in";
	const char* hi = count_set ? "bn_hi" : "bn_hi_in";
	source += String8::FromPrintf("%%bn_count_lo_<index> = OpBitCount %%uint %%%s_<index>\n", lo);
	if (wide)
	{
		source += String8::FromPrintf("%%bn_count_hi_<index> = OpBitCount %%uint %%%s_<index>\n", hi);
		source += "%bn_count_<index> = OpIAdd %uint %bn_count_lo_<index> %bn_count_hi_<index>\n";
	} else
	{
		source += "%bn_count_<index> = OpCopyObject %uint %bn_count_lo_<index>\n";
	}
	source += "               OpStore %<dst> %bn_count_<index>\n    <scc>\n";
	*dst_source += source.ReplaceStr("<scc>", get_scc_check(scc_check, 1)).ReplaceStr("<dst>", dst.value).ReplaceStr("<index>", index_str);
	return true;
}

// s_ff1_i32_b{32,64}: index of the least significant set bit, or -1.
KYTY_RECOMPILER_FUNC(Recompile_SFf1I32_XXX)
{
	const auto& inst      = code.GetInstructions().At(index);
	const bool  wide      = inst.src[0].size == 2;
	const auto  dst       = operand_variable_to_str(inst.dst);
	const auto  index_str = String8::FromPrintf("%u", index);
	String8     load_lo;
	String8     load_hi;
	if (dst.type != SpirvType::Uint || !operand_load_uint(spirv, inst.src[0], "ff_lo_<index>", index_str, &load_lo, wide ? 0 : -1) ||
	    (wide && !operand_load_uint(spirv, inst.src[0], "ff_hi_<index>", index_str, &load_hi, 1)))
	{
		return false;
	}
	// FindILsb yields -1 for a zero word.
	String8 source = load_lo + "\n" + load_hi + "\n%ff_lsb_lo_<index> = OpExtInst %uint %GLSL_std_450 FindILsb %ff_lo_<index>\n";
	if (wide)
	{
		source += R"(
%ff_lsb_hi_<index> = OpExtInst %uint %GLSL_std_450 FindILsb %ff_hi_<index>
%ff_hi_pos_<index> = OpIAdd %uint %ff_lsb_hi_<index> %uint_32
%ff_hi_any_<index> = OpINotEqual %bool %ff_hi_<index> %uint_0
%ff_hi_result_<index> = OpSelect %uint %ff_hi_any_<index> %ff_hi_pos_<index> %ff_lsb_hi_<index>
%ff_lo_any_<index> = OpINotEqual %bool %ff_lo_<index> %uint_0
%ff_result_<index> = OpSelect %uint %ff_lo_any_<index> %ff_lsb_lo_<index> %ff_hi_result_<index>
)";
	} else
	{
		source += "%ff_result_<index> = OpCopyObject %uint %ff_lsb_lo_<index>\n";
	}
	source += "               OpStore %<dst> %ff_result_<index>\n";
	*dst_source += source.ReplaceStr("<dst>", dst.value).ReplaceStr("<index>", index_str);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
