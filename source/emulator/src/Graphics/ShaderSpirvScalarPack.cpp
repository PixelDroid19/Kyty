#include "ShaderSpirvInternal.h"
#include "ShaderSpirvTemplates.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static bool pack_source_is_supported(const ShaderOperand& op)
{
	if (op.dpp || op.negate || op.absolute || op.swizzle != 6u || op.clamp || op.multiplier != 1.0f)
	{
		return false;
	}
	if (operand_is_constant(op))
	{
		return op.size == 0;
	}
	if (op.type == ShaderOperandType::Null)
	{
		return op.size == 1;
	}
	return operand_is_variable(op) && op.type != ShaderOperandType::Vgpr && op.size == 1;
}

KYTY_RECOMPILER_FUNC(Recompile_SPackLlB32B16)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || inst.src_num != 2 || inst.dst.size != 1 || inst.dst.dpp ||
	    inst.dst.negate || inst.dst.absolute || inst.dst.swizzle != 6u || inst.dst.clamp || inst.dst.multiplier != 1.0f ||
	    !pack_source_is_supported(inst.src[0]) ||
	    !pack_source_is_supported(inst.src[1]))
	{
		return false;
	}
	if (inst.dst.type == ShaderOperandType::Null)
	{
		return true;
	}

	const auto dst_value = operand_variable_to_str(inst.dst);
	if (dst_value.type != SpirvType::Uint)
	{
		return false;
	}

	const auto index_str = String8::FromPrintf("%u", index);
	String8    load_s0;
	String8    load_s1;
	if (!operand_load_uint(spirv, inst.src[0], "pack_s0_<index>", index_str, &load_s0) ||
	    !operand_load_uint(spirv, inst.src[1], "pack_s1_<index>", index_str, &load_s1))
	{
		return false;
	}

	*dst_source += String8(R"(
<load_s0>
<load_s1>
%pack_s0_shift_<index> = OpShiftLeftLogical %uint %pack_s0_<index> %uint_16
%pack_low_<index> = OpShiftRightLogical %uint %pack_s0_shift_<index> %uint_16
%pack_high_<index> = OpShiftLeftLogical %uint %pack_s1_<index> %uint_16
%pack_result_<index> = OpBitwiseOr %uint %pack_low_<index> %pack_high_<index>
OpStore %<dst> %pack_result_<index>
<execz>
)")
	                   .ReplaceStr("<load_s0>", load_s0)
	                   .ReplaceStr("<load_s1>", load_s1)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<execz>", operand_is_exec(inst.dst) ? String8(EXECZ).ReplaceStr("<index>", index_str) : String8())
	                   .ReplaceStr("<index>", index_str);

	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
