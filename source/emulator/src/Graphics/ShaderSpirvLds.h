#ifndef EMULATOR_SRC_GRAPHICS_SHADER_SPIRV_LDS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_SPIRV_LDS_H_

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// The indexed DS domain here is dword-aligned. Misalignment/traps are not
// modeled: do not silently round a bad byte address down onto another word.
// ISA 3.6.1 defines zero reads and discarded writes wholly outside LDS. A
// partially out-of-range wide write is undefined there; requiring the whole
// contiguous span to fit is a host-memory safety policy, not a partial-write
// hardware rule. READ2 instead invokes this check separately for each address.
inline String8 EmitLdsAddressCheck(Spirv* spirv, const String8& address, uint32_t offset, int dwords, const String8& tag)
{
	return String8(R"(
%lds_byte_addr_<tag> = OpIAdd %uint %<address> %<offset>
%lds_no_wrap_<tag> = OpUGreaterThanEqual %bool %lds_byte_addr_<tag> %<address>
%lds_low_bits_<tag> = OpBitwiseAnd %uint %lds_byte_addr_<tag> %uint_3
%lds_aligned_<tag> = OpIEqual %bool %lds_low_bits_<tag> %uint_0
%lds_index_<tag> = OpShiftRightLogical %uint %lds_byte_addr_<tag> %uint_2
%lds_last_<tag> = OpIAdd %uint %lds_index_<tag> %<last>
%lds_in_bounds_<tag> = OpULessThan %bool %lds_last_<tag> %lds_length
%lds_address_valid_<tag> = OpLogicalAnd %bool %lds_no_wrap_<tag> %lds_aligned_<tag>
%lds_valid_<tag> = OpLogicalAnd %bool %lds_address_valid_<tag> %lds_in_bounds_<tag>
)")
	    .ReplaceStr("<tag>", tag)
	    .ReplaceStr("<address>", address)
	    .ReplaceStr("<offset>", spirv->GetConstantUint(offset))
	    .ReplaceStr("<last>", spirv->GetConstantUint(static_cast<uint32_t>(dwords - 1)));
}

inline String8 GateLdsByExec(const String8& body, const String8& tag)
{
	return String8(R"(
%lds_exec_<tag> = OpLoad %uint %exec_lo
%lds_active_<tag> = OpINotEqual %bool %lds_exec_<tag> %uint_0
               OpSelectionMerge %lds_exec_merge_<tag> None
               OpBranchConditional %lds_active_<tag> %lds_exec_then_<tag> %lds_exec_merge_<tag>
%lds_exec_then_<tag> = OpLabel
<body>
               OpBranch %lds_exec_merge_<tag>
%lds_exec_merge_<tag> = OpLabel
)")
	    .ReplaceStr("<body>", body).ReplaceStr("<tag>", tag);
}

inline String8 GateLdsByBounds(const String8& body, const String8& tag)
{
	return String8(R"(
               OpSelectionMerge %lds_bounds_merge_<tag> None
               OpBranchConditional %lds_valid_<tag> %lds_bounds_then_<tag> %lds_bounds_merge_<tag>
%lds_bounds_then_<tag> = OpLabel
<body>
               OpBranch %lds_bounds_merge_<tag>
%lds_bounds_merge_<tag> = OpLabel
)")
	    .ReplaceStr("<body>", body).ReplaceStr("<tag>", tag);
}

// Every caller places this inside the valid span's branch. In particular an
// OpSelect of a loaded value is insufficient: it would still evaluate an OOB
// OpAccessChain. The address is an instruction snapshot, never a reloaded VGPR.
inline String8 EmitLdsWordPointer(Spirv* spirv, int word, const String8& tag)
{
	return String8(R"(
%lds_word_<tag>_<word> = OpIAdd %uint %lds_index_<tag> %<constant>
%lds_ptr_<tag>_<word> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_word_<tag>_<word>
)")
	    .ReplaceStr("<tag>", tag)
	    .ReplaceStr("<word>", String8::FromPrintf("%d", word))
	    .ReplaceStr("<constant>", spirv->GetConstantUint(static_cast<uint32_t>(word)));
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
#endif // EMULATOR_SRC_GRAPHICS_SHADER_SPIRV_LDS_H_
