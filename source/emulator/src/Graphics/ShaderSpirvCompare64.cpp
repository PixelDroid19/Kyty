#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvInternal.h"
#include "ShaderSpirvTemplates.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// v_cmp(x)_{f,lt,eq,le,gt,ne,ge,t}_{i64,u64}: lane predicate %t3_<index> (0/1) from the 64-bit comparison of
// (hi, lo) word pairs. param[0] names the predicate (f, lt, eq, le, gt, ne, ge or t) and param[1] is "s" for the
// signed forms, which compare the high words signed and the low words unsigned. ISA 6.2: a literal source is zero
// extended for the unsigned forms and sign extended for the signed ones. A v_cmpx writes EXEC only; the others store
// the lane bit to the SGPR pair with the high word cleared.
KYTY_RECOMPILER_FUNC(Recompile_VCmp_XXX_64)
{
	const auto& inst      = code.GetInstructions().At(index);
	const bool  is_signed = param[1] != nullptr && strcmp(param[1], "s") == 0;
	const auto  i         = String8::FromPrintf("%u", index);
	for (int source = 0; source < 2; source++)
	{
		// Input modifiers are float-only; an integer compare has no meaning for them.
		if (inst.src[source].absolute || inst.src[source].negate) { return false; }
	}
	const auto load = [&](const ShaderOperand& operand, int word, const char* id, String8* output)
	{
		String8 text;
		const bool loaded = is_signed ? operand_load_i64_word(spirv, operand, id, i, &text, word)
		                              : operand_load_uint(spirv, operand, id, i, &text, word);
		*output += text + "\n";
		return loaded;
	};
	String8 source;
	if (!load(inst.src[0], 0, "c64_a_lo_<index>", &source) || !load(inst.src[0], 1, "c64_a_hi_<index>", &source) ||
	    !load(inst.src[1], 0, "c64_b_lo_<index>", &source) || !load(inst.src[1], 1, "c64_b_hi_<index>", &source))
	{
		return false;
	}
	String8 store;
	if (ShaderInstructionTypeStartsWith(inst.type, "VCmpx"))
	{
		store = String8(CMPX_EXEC_STORE).ReplaceStr("<execz>", EXECZ);
	} else
	{
		const auto dst0 = operand_variable_to_str(inst.dst, 0);
		const auto dst1 = operand_variable_to_str(inst.dst, 1);
		if (dst0.type != SpirvType::Uint) { return false; }
		store = String8("OpStore %<dst0> %t3_<index>\nOpStore %<dst1> %uint_0\n").ReplaceStr("<dst0>", dst0.value).ReplaceStr("<dst1>", dst1.value);
	}
	source += R"(
%c64_hi_eq_<index> = OpIEqual %bool %c64_a_hi_<index> %c64_b_hi_<index>
%c64_lo_eq_<index> = OpIEqual %bool %c64_a_lo_<index> %c64_b_lo_<index>
%c64_eq_<index> = OpLogicalAnd %bool %c64_hi_eq_<index> %c64_lo_eq_<index>
%c64_hi_lt_<index> = <hi_less> %bool %c64_a_hi_<index> %c64_b_hi_<index>
%c64_lo_lt_<index> = OpULessThan %bool %c64_a_lo_<index> %c64_b_lo_<index>
%c64_lo_part_<index> = OpLogicalAnd %bool %c64_hi_eq_<index> %c64_lo_lt_<index>
%c64_lt_<index> = OpLogicalOr %bool %c64_hi_lt_<index> %c64_lo_part_<index>
%c64_le_<index> = OpLogicalOr %bool %c64_lt_<index> %c64_eq_<index>
%c64_gt_<index> = OpLogicalNot %bool %c64_le_<index>
%c64_ge_<index> = OpLogicalNot %bool %c64_lt_<index>
%c64_ne_<index> = OpLogicalNot %bool %c64_eq_<index>
<constant>%t3_<index> = OpSelect %uint %c64_<pred>_<index> %uint_1 %uint_0
<store>
)";
	// The always-false and always-true forms ignore the comparison.
	const bool constant_false = strcmp(param[0], "f") == 0;
	const bool constant_true  = strcmp(param[0], "t") == 0;
	const auto constant       = constant_false || constant_true
	                                ? String8::FromPrintf("%%c64_%s_<index> = OpIEqual %%bool %%uint_0 %%uint_%d\n", param[0], constant_false ? 1 : 0)
	                                : String8 {};
	*dst_source += source.ReplaceStr("<constant>", constant)
	                   .ReplaceStr("<hi_less>", is_signed ? "OpSLessThan" : "OpULessThan")
	                   .ReplaceStr("<pred>", param[0])
	                   .ReplaceStr("<store>", store)
	                   .ReplaceStr("<index>", i);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
