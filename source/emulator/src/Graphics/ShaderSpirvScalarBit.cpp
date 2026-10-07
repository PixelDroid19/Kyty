#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_RECOMPILER_FUNC(Recompile_SBitset1B32)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != 1 || inst.src_num != 2 ||
	    inst.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || !(inst.src[1] == inst.dst))
	{
		return false;
	}

	const auto dst_value = operand_variable_to_str(inst.dst);
	if (dst_value.type != SpirvType::Uint)
	{
		return false;
	}

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load_index;
	String8       load_old_dst;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load_index) ||
	    !operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load_old_dst))
	{
		return false;
	}

	static const char* text = R"(
    <load_index>
    <load_old_dst>
    %bitset_index_<index> = OpBitwiseAnd %uint %t0_<index> %uint_31
    %bitset_mask_<index> = OpShiftLeftLogical %uint %uint_1 %bitset_index_<index>
    %bitset_result_<index> = OpBitwiseOr %uint %t1_<index> %bitset_mask_<index>
    OpStore %<dst> %bitset_result_<index>
)";

	*dst_source += String8(text)
	                  .ReplaceStr("<load_index>", load_index)
	                  .ReplaceStr("<load_old_dst>", load_old_dst)
	                  .ReplaceStr("<dst>", dst_value.value)
	                  .ReplaceStr("<index>", index_str);

	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
