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

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
