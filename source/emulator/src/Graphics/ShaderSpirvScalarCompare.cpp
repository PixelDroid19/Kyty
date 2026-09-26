#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_RECOMPILER_FUNC(Recompile_SCmpLgU64)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.src_num != 2) { return false; }
	String8 loads;
	const auto tag = String8::FromPrintf("%u", index);
	for (int src_index = 0; src_index < 2; ++src_index)
	{
		const auto& op = inst.src[src_index];
		// Ordinary scalar pairs, the VCC and EXEC pairs and integer inline
		// constants have a defined 64-bit representation in this backend.
		const bool mask_pair = (op.type == ShaderOperandType::VccLo || op.type == ShaderOperandType::ExecLo) && op.register_id == 0;
		if (op.size != 2 || (op.type != ShaderOperandType::Sgpr && op.type != ShaderOperandType::IntegerInlineConstant && !mask_pair) ||
		    op.negate || op.absolute || op.dpp || op.swizzle != 6u ||
		    (op.type == ShaderOperandType::Sgpr && (op.register_id < 0 || op.register_id > 102)))
		{
			return false;
		}
		for (int word = 0; word < 2; ++word)
		{
			String8 load;
			const auto name = String8::FromPrintf("cmp64_word%d_<index>", src_index * 2 + word);
			if (!operand_load_uint(spirv, op, name, tag, &load, word)) { return false; }
			loads += load + "\n";
		}
	}
	// Inequality is true when either half differs. This needs no host Int64
	// capability and writes only SCC, independent of EXEC.
	*dst_source += (loads + String8(R"(
%cmp64_low_<index> = OpINotEqual %bool %cmp64_word0_<index> %cmp64_word2_<index>
%cmp64_high_<index> = OpINotEqual %bool %cmp64_word1_<index> %cmp64_word3_<index>
%cmp64_different_<index> = OpLogicalOr %bool %cmp64_low_<index> %cmp64_high_<index>
%cmp64_result_<index> = OpSelect %uint %cmp64_different_<index> %uint_1 %uint_0
OpStore %scc %cmp64_result_<index>
)")).ReplaceStr("<index>", tag);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
