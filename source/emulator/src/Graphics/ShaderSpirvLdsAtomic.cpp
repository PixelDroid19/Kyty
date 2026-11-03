#include "Emulator/Graphics/ShaderComputeWaveLds.h"

#include "ShaderSpirvInternal.h"
#include "ShaderSpirvLds.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

bool IsKnownUintConstant(const String8& value)
{
	return !value.IsEmpty() && value != "unknown_uint_constant";
}

// ds_cmpst: MEM = (MEM == DATA) ? DATA2 : MEM, with DATA (src[1]) the comparand.
// That is OpAtomicCompareExchange with Value = DATA2 and Comparator = DATA, so the
// operands are wired directly and no swap is needed. Every source is loaded before
// the store, so VDST may alias ADDR.
bool EmitLdsCompareStore(uint32_t index, const ShaderCode& code, String8* dst_source, Spirv* spirv, bool returns)
{
	if (dst_source == nullptr || spirv == nullptr || index >= code.GetInstructions().Size())
	{
		return false;
	}

	const auto& inst = code.GetInstructions().At(index);
	if (!ShaderLdsMemoryInstructionSupported(inst))
	{
		return false;
	}

	const auto* input_info = spirv->GetCsInputInfo();
	if (input_info == nullptr || input_info->lds_dwords == 0u)
	{
		return false;
	}

	const auto address = operand_variable_to_str(inst.src[0]);
	const auto compare = operand_variable_to_str(inst.src[1]);
	const auto value   = operand_variable_to_str(inst.src[2]);
	if (address.type != SpirvType::Float || compare.type != SpirvType::Float || value.type != SpirvType::Float ||
	    address.value.IsEmpty() || compare.value.IsEmpty() || value.value.IsEmpty())
	{
		return false;
	}

	String8 result_text;
	if (returns)
	{
		const auto destination = operand_variable_to_str(inst.dst);
		if (destination.type != SpirvType::Float || destination.value.IsEmpty())
		{
			return false;
		}
		result_text = String8(R"(
%native_lds_cmpst_result_<index> = OpPhi %uint %native_lds_cmpst_prior_<index> %native_lds_cmpst_then_<index> %<zero> %native_lds_cmpst_else_<index>
%native_lds_cmpst_result_f_<index> = OpBitcast %float %native_lds_cmpst_result_<index>
               OpStore %<destination> %native_lds_cmpst_result_f_<index>
)")
		                  .ReplaceStr("<destination>", destination.value);
	}

	const auto         index_string = String8::FromPrintf("%u", index);
	const auto         offset       = spirv->GetConstantUint(static_cast<uint32_t>(inst.ds_offset));
	const auto         scope        = spirv->GetConstantUint(2u);
	const auto         semantics    = spirv->GetConstantUint(0x108u);
	const auto         zero         = spirv->GetConstantUint(0u);
	if (!IsKnownUintConstant(offset) || !IsKnownUintConstant(scope) || !IsKnownUintConstant(semantics) || !IsKnownUintConstant(zero))
	{
		return false;
	}

	static const char* text = R"(
%native_lds_cmpst_addr_f_<index> = OpLoad %float %<address>
%native_lds_cmpst_cmp_f_<index> = OpLoad %float %<compare>
%native_lds_cmpst_val_f_<index> = OpLoad %float %<value>
%native_lds_cmpst_addr_u_<index> = OpBitcast %uint %native_lds_cmpst_addr_f_<index>
%native_lds_cmpst_cmp_u_<index> = OpBitcast %uint %native_lds_cmpst_cmp_f_<index>
%native_lds_cmpst_val_u_<index> = OpBitcast %uint %native_lds_cmpst_val_f_<index>
<address_check>
               OpSelectionMerge %native_lds_cmpst_merge_<index> None
               OpBranchConditional %lds_valid_<index> %native_lds_cmpst_then_<index> %native_lds_cmpst_else_<index>
%native_lds_cmpst_then_<index> = OpLabel
%native_lds_cmpst_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
%native_lds_cmpst_prior_<index> = OpAtomicCompareExchange %uint %native_lds_cmpst_ptr_<index> %<scope> %<semantics> %<zero> %native_lds_cmpst_val_u_<index> %native_lds_cmpst_cmp_u_<index>
               OpBranch %native_lds_cmpst_merge_<index>
%native_lds_cmpst_else_<index> = OpLabel
               OpBranch %native_lds_cmpst_merge_<index>
%native_lds_cmpst_merge_<index> = OpLabel
<result>
)";
	const auto body = String8(text)
	                      .ReplaceStr("<result>", result_text)
	                      .ReplaceStr("<address_check>", EmitLdsAddressCheck(spirv, "native_lds_cmpst_addr_u_" + index_string,
	                                                                        inst.ds_offset, 1, index_string))
	                      .ReplaceStr("<index>", index_string)
	                      .ReplaceStr("<address>", address.value)
	                      .ReplaceStr("<compare>", compare.value)
	                      .ReplaceStr("<value>", value.value)
	                      .ReplaceStr("<scope>", scope)
	                      .ReplaceStr("<semantics>", semantics)
	                      .ReplaceStr("<zero>", zero);
	*dst_source += GateLdsByExec(body, index_string);
	return true;
}

} // namespace

// Returning LDS atomics (ds_add_rtn_u32, ds_wrxchg_rtn_b32): param[0] is the
// SPIR-V atomic; inactive lanes keep their destination VGPR.
KYTY_RECOMPILER_FUNC(Recompile_DsAtomicRtn_VdstVaddrVdataOffset)
{
	if (dst_source == nullptr || spirv == nullptr || index >= code.GetInstructions().Size())
	{
		return false;
	}

	const auto& inst = code.GetInstructions().At(index);
	if (param == nullptr || param[0] == nullptr || !ShaderLdsMemoryInstructionSupported(inst))
	{
		return false;
	}

	const auto* input_info = spirv->GetCsInputInfo();
	if (input_info == nullptr || input_info->lds_dwords == 0u)
	{
		return false;
	}

	const auto destination = operand_variable_to_str(inst.dst);
	const auto address     = operand_variable_to_str(inst.src[0]);
	const auto data        = operand_variable_to_str(inst.src[1]);
	if (destination.type != SpirvType::Float || address.type != SpirvType::Float || data.type != SpirvType::Float ||
	    destination.value.IsEmpty() || address.value.IsEmpty() || data.value.IsEmpty())
	{
		return false;
	}

	const auto offset    = spirv->GetConstantUint(static_cast<uint32_t>(inst.ds_offset));
	const auto scope     = spirv->GetConstantUint(2u);
	const auto semantics = spirv->GetConstantUint(0x108u);
	const auto zero      = spirv->GetConstantUint(0u);
	if (!IsKnownUintConstant(offset) || !IsKnownUintConstant(scope) || !IsKnownUintConstant(semantics) || !IsKnownUintConstant(zero))
	{
		return false;
	}

	const auto         index_string = String8::FromPrintf("%u", index);
	static const char* text         = R"(
%native_lds_add_rtn_addr_f_<index> = OpLoad %float %<address>
%native_lds_add_rtn_data_f_<index> = OpLoad %float %<data>
%native_lds_add_rtn_addr_u_<index> = OpBitcast %uint %native_lds_add_rtn_addr_f_<index>
%native_lds_add_rtn_data_u_<index> = OpBitcast %uint %native_lds_add_rtn_data_f_<index>
<address_check>
               OpSelectionMerge %native_lds_add_rtn_merge_<index> None
               OpBranchConditional %lds_valid_<index> %native_lds_add_rtn_then_<index> %native_lds_add_rtn_else_<index>
%native_lds_add_rtn_then_<index> = OpLabel
%native_lds_add_rtn_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
%native_lds_add_rtn_prior_<index> = <atomic> %uint %native_lds_add_rtn_ptr_<index> %<scope> %<semantics> %native_lds_add_rtn_data_u_<index>
               OpBranch %native_lds_add_rtn_merge_<index>
%native_lds_add_rtn_else_<index> = OpLabel
               OpBranch %native_lds_add_rtn_merge_<index>
%native_lds_add_rtn_merge_<index> = OpLabel
%native_lds_add_rtn_result_<index> = OpPhi %uint %native_lds_add_rtn_prior_<index> %native_lds_add_rtn_then_<index> %<zero> %native_lds_add_rtn_else_<index>
%native_lds_add_rtn_result_f_<index> = OpBitcast %float %native_lds_add_rtn_result_<index>
               OpStore %<destination> %native_lds_add_rtn_result_f_<index>
)";
	const auto body = String8(text)
	                   .ReplaceStr("<address_check>", EmitLdsAddressCheck(spirv, "native_lds_add_rtn_addr_u_" + index_string,
	                                                                     inst.ds_offset, 1, index_string))
	                   .ReplaceStr("<index>", index_string)
	                   .ReplaceStr("<address>", address.value)
	                   .ReplaceStr("<data>", data.value)
	                   .ReplaceStr("<destination>", destination.value)
	                   .ReplaceStr("<scope>", scope)
	                   .ReplaceStr("<semantics>", semantics)
	                   .ReplaceStr("<zero>", zero)
	                   .ReplaceStr("<atomic>", param[0]);
	*dst_source += GateLdsByExec(body, index_string);
	return true;
}

// ds_cmpst_rtn_b32: VDST receives the prior value; inactive lanes keep their VDST.
KYTY_RECOMPILER_FUNC(Recompile_DsCmpstRtn_VdstVaddrVdata2Offset)
{
	return EmitLdsCompareStore(index, code, dst_source, spirv, true);
}

// ds_cmpst_b32: the same compare-and-store without a VDST write.
KYTY_RECOMPILER_FUNC(Recompile_DsCmpst_VaddrVdata2Offset)
{
	return EmitLdsCompareStore(index, code, dst_source, spirv, false);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
