#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

// Loads word `word` of a 64-bit source. Inline integers sign-extend to 64 bits;
// a 32-bit literal's 64-bit extension is not established, so it fails closed.
bool LoadWord64(Spirv* spirv, const ShaderOperand& operand, int word, const String8& id, String8* output)
{
	if (operand.type == ShaderOperandType::IntegerInlineConstant)
	{
		const uint32_t value = word == 0 ? operand.constant.u : (operand.constant.i < 0 ? 0xffffffffu : 0u);
		*output += String8::FromPrintf("%%%s = OpCopyObject %%uint %%%s\n", id.c_str(), spirv->GetConstantUint(value).c_str());
		return true;
	}
	const auto value = operand_variable_to_str(operand, word);
	if (value.value.IsEmpty())
	{
		return false;
	}
	if (value.type == SpirvType::Float)
	{
		*output += String8::FromPrintf("%%%s_f = OpLoad %%float %%%s\n%%%s = OpBitcast %%uint %%%s_f\n", id.c_str(), value.value.c_str(),
		                               id.c_str(), id.c_str());
		return true;
	}
	if (value.type == SpirvType::Uint)
	{
		*output += String8::FromPrintf("%%%s = OpLoad %%uint %%%s\n", id.c_str(), value.value.c_str());
		return true;
	}
	return false;
}

} // namespace

// v_cmp_{lt,eq,le,gt,ne,ge}_u64: lane predicate %t3_<index> (0/1) from the
// unsigned 64-bit comparison of (hi, lo) word pairs, then the per-lane result
// stored like the 32-bit compares. param[0] names the predicate: Lt, Eq, Le,
// Gt, Ne or Ge.
KYTY_RECOMPILER_FUNC(Recompile_VCmp_XXX_U64)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto  dst0 = operand_variable_to_str(inst.dst, 0);
	const auto  dst1 = operand_variable_to_str(inst.dst, 1);
	const auto  i    = String8::FromPrintf("%u", index);
	String8     source;
	if (dst0.type != SpirvType::Uint || !LoadWord64(spirv, inst.src[0], 0, "c64_a_lo_" + i, &source) ||
	    !LoadWord64(spirv, inst.src[0], 1, "c64_a_hi_" + i, &source) || !LoadWord64(spirv, inst.src[1], 0, "c64_b_lo_" + i, &source) ||
	    !LoadWord64(spirv, inst.src[1], 1, "c64_b_hi_" + i, &source))
	{
		return false;
	}
	source += R"(
%c64_hi_eq_<i> = OpIEqual %bool %c64_a_hi_<i> %c64_b_hi_<i>
%c64_lo_eq_<i> = OpIEqual %bool %c64_a_lo_<i> %c64_b_lo_<i>
%c64_eq_<i> = OpLogicalAnd %bool %c64_hi_eq_<i> %c64_lo_eq_<i>
%c64_hi_lt_<i> = OpULessThan %bool %c64_a_hi_<i> %c64_b_hi_<i>
%c64_lo_lt_<i> = OpULessThan %bool %c64_a_lo_<i> %c64_b_lo_<i>
%c64_lo_part_<i> = OpLogicalAnd %bool %c64_hi_eq_<i> %c64_lo_lt_<i>
%c64_lt_<i> = OpLogicalOr %bool %c64_hi_lt_<i> %c64_lo_part_<i>
%c64_le_<i> = OpLogicalOr %bool %c64_lt_<i> %c64_eq_<i>
%c64_gt_<i> = OpLogicalNot %bool %c64_le_<i>
%c64_ge_<i> = OpLogicalNot %bool %c64_lt_<i>
%c64_ne_<i> = OpLogicalNot %bool %c64_eq_<i>
%t3_<i> = OpSelect %uint %c64_<pred>_<i> %uint_1 %uint_0
               OpStore %<dst0> %t3_<i>
               OpStore %<dst1> %uint_0
)";
	*dst_source += source.ReplaceStr("<pred>", param[0]).ReplaceStr("<dst0>", dst0.value).ReplaceStr("<dst1>", dst1.value).ReplaceStr("<i>", i);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
