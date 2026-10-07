#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_RECOMPILER_FUNC(Recompile_SGetpcB64)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto* bind = spirv->GetBindInfo();
	// PCs of an appended continuation are shifted away from the program base,
	// so only the program's own segment resolves through it.
	if (inst.pc >= code.GetContinuationPc() || bind == nullptr || !bind->program_base_used ||
	    inst.src_num != 0 || inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != 2 ||
	    inst.dst.register_id < 0 || inst.dst.register_id > 102 || inst.dst.negate || inst.dst.absolute ||
	    inst.dst.dpp || inst.dst.swizzle != 6u || inst.pc > UINT32_MAX - 4u || (inst.pc % 4u) != 0u)
	{
		return false;
	}
	const uint32_t size_dw = bind->push_constant_size / 4u;
	if ((bind->push_constant_size % 16u) != 0u || size_dw < 4u ||
	    bind->program_base_offset_dw != size_dw - 4u)
	{
		return false;
	}
	const auto next_pc = spirv->GetConstantUint(inst.pc + 4u);
	const auto block   = spirv->GetConstantInt(static_cast<int>(bind->program_base_offset_dw / 4u));
	if (next_pc == "unknown_uint_constant" || block == "unknown_int_constant") { return false; }
	// Two uint words keep this independent of the host shaderInt64 capability.
	// Scalar GETPC executes with empty EXEC and changes neither EXEC nor SCC.
	*dst_source += String8(R"(
%getpc_lo_ptr_<i> = OpAccessChain %<ptr> %vsharp %int_0 %<block> %int_0
%getpc_hi_ptr_<i> = OpAccessChain %<ptr> %vsharp %int_0 %<block> %int_1
%getpc_base_lo_<i> = OpLoad %uint %getpc_lo_ptr_<i>
%getpc_base_hi_<i> = OpLoad %uint %getpc_hi_ptr_<i>
%getpc_lo_<i> = OpIAdd %uint %getpc_base_lo_<i> %<next_pc>
%getpc_carry_bool_<i> = OpULessThan %bool %getpc_lo_<i> %getpc_base_lo_<i>
%getpc_carry_<i> = OpSelect %uint %getpc_carry_bool_<i> %uint_1 %uint_0
%getpc_hi_<i> = OpIAdd %uint %getpc_base_hi_<i> %getpc_carry_<i>
OpStore %<dst_lo> %getpc_lo_<i>
OpStore %<dst_hi> %getpc_hi_<i>
)")
	                   .ReplaceStr("<ptr>", bind->vsharp_uniform_buffer ? "_ptr_Uniform_uint" : "_ptr_PushConstant_uint")
	                   .ReplaceStr("<block>", block).ReplaceStr("<next_pc>", next_pc)
	                   .ReplaceStr("<dst_lo>", operand_variable_to_str(inst.dst, 0).value)
	                   .ReplaceStr("<dst_hi>", operand_variable_to_str(inst.dst, 1).value)
	                   .ReplaceStr("<i>", String8::FromPrintf("%u", index));
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
