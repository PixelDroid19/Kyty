#include "Emulator/Graphics/GuestDeviceAddress.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

// Byte offsets reachable from one translated address: up to a 128-byte BVH
// node or a 64-byte s_load_dwordx16, plus the table's entry fields.
constexpr uint32_t kMaxLoadBytes = 128;

} // namespace

bool Spirv::UsesGuestDeviceAddress() const
{
	return m_bind != nullptr && m_bind->device_address_used;
}

String8 Spirv::GuestDeviceAddressTypes(bool ulong_declared) const
{
	String8 types = ulong_declared ? "" : "\n %ulong = OpTypeInt 64 0\n %slong = OpTypeInt 64 1\n";
	types += R"(
%_ptr_PhysicalStorageBuffer_uint = OpTypePointer PhysicalStorageBuffer %uint
%_ptr_Function_ulong_gda = OpTypePointer Function %ulong
%_ptr_Function_uint_gda = OpTypePointer Function %uint
%function_gda = OpTypeFunction %ulong %ulong %ulong
)";
	for (uint32_t offset = 0; offset <= kMaxLoadBytes; offset += 4)
	{
		types += String8::FromPrintf("%%gda_u64_%u = OpConstant %%ulong %u\n", offset, offset);
	}
	types += String8::FromPrintf("%%gda_u64_null = OpConstant %%ulong %u\n", kGuestDeviceAddressNullBytes);
	return types;
}

// ulong guest_device_address(ulong guest, ulong bytes): device address of
// `guest`, or the table's zero prefix when no imported range contains the
// whole access (GuestDeviceAddressAccessFits).
String8 Spirv::GuestDeviceAddressFunction() const
{
	static const char* text = R"(
%guest_device_address = OpFunction %ulong DontInline %function_gda
%gda_addr = OpFunctionParameter %ulong
%gda_bytes = OpFunctionParameter %ulong
%gda_entry = OpLabel
%gda_i = OpVariable %_ptr_Function_uint_gda Function
%gda_result = OpVariable %_ptr_Function_ulong_gda Function
%gda_tlo_p = OpAccessChain %<ptr> %vsharp %int_0 %<block> %int_0
%gda_thi_p = OpAccessChain %<ptr> %vsharp %int_0 %<block> %int_1
%gda_cnt_p = OpAccessChain %<ptr> %vsharp %int_0 %<block> %int_2
%gda_tlo = OpLoad %uint %gda_tlo_p
%gda_thi = OpLoad %uint %gda_thi_p
%gda_count = OpLoad %uint %gda_cnt_p
%gda_tlo64 = OpUConvert %ulong %gda_tlo
%gda_thi64 = OpUConvert %ulong %gda_thi
%gda_this = OpShiftLeftLogical %ulong %gda_thi64 %uint_32
%gda_table = OpBitwiseOr %ulong %gda_this %gda_tlo64
%gda_entries = OpIAdd %ulong %gda_table %gda_u64_null
OpStore %gda_i %uint_0
OpStore %gda_result %gda_table
OpBranch %gda_header
%gda_header = OpLabel
OpLoopMerge %gda_merge %gda_continue None
OpBranch %gda_cond
%gda_cond = OpLabel
%gda_iv = OpLoad %uint %gda_i
%gda_more = OpULessThan %bool %gda_iv %gda_count
OpBranchConditional %gda_more %gda_body %gda_merge
%gda_body = OpLabel
%gda_off = OpIMul %uint %gda_iv %uint_32
%gda_off64 = OpUConvert %ulong %gda_off
%gda_e = OpIAdd %ulong %gda_entries %gda_off64
<load64 base 0>
<load64 size 8>
<load64 dev 16>
<load64 span 24>
%gda_rel = OpISub %ulong %gda_addr %gda_base
%gda_started = OpULessThan %bool %gda_rel %gda_size
%gda_inspan = OpULessThan %bool %gda_rel %gda_span
%gda_room = OpISub %ulong %gda_span %gda_rel
%gda_fit = OpUGreaterThanEqual %bool %gda_room %gda_bytes
%gda_end_ok = OpLogicalAnd %bool %gda_inspan %gda_fit
%gda_inside = OpLogicalAnd %bool %gda_started %gda_end_ok
OpSelectionMerge %gda_skip None
OpBranchConditional %gda_inside %gda_hit %gda_skip
%gda_hit = OpLabel
%gda_device = OpIAdd %ulong %gda_dev %gda_rel
OpStore %gda_result %gda_device
OpBranch %gda_merge
%gda_skip = OpLabel
OpBranch %gda_continue
%gda_continue = OpLabel
%gda_next = OpIAdd %uint %gda_iv %uint_1
OpStore %gda_i %gda_next
OpBranch %gda_header
%gda_merge = OpLabel
%gda_ret = OpLoad %ulong %gda_result
OpReturnValue %gda_ret
OpFunctionEnd
)";
	auto load64 = [](const char* name, uint32_t offset)
	{
		return String8::FromPrintf("%%gda_%s_a0 = OpIAdd %%ulong %%gda_e %%gda_u64_%u\n"
		                           "%%gda_%s_a1 = OpIAdd %%ulong %%gda_e %%gda_u64_%u\n"
		                           "%%gda_%s_p0 = OpConvertUToPtr %%_ptr_PhysicalStorageBuffer_uint %%gda_%s_a0\n"
		                           "%%gda_%s_p1 = OpConvertUToPtr %%_ptr_PhysicalStorageBuffer_uint %%gda_%s_a1\n"
		                           "%%gda_%s_lo = OpLoad %%uint %%gda_%s_p0 Aligned 4\n"
		                           "%%gda_%s_hi = OpLoad %%uint %%gda_%s_p1 Aligned 4\n"
		                           "%%gda_%s_lo64 = OpUConvert %%ulong %%gda_%s_lo\n"
		                           "%%gda_%s_hi64 = OpUConvert %%ulong %%gda_%s_hi\n"
		                           "%%gda_%s_his = OpShiftLeftLogical %%ulong %%gda_%s_hi64 %%uint_32\n"
		                           "%%gda_%s = OpBitwiseOr %%ulong %%gda_%s_his %%gda_%s_lo64\n",
		                           name, offset, name, offset + 4, name, name, name, name, name, name, name, name, name, name, name, name,
		                           name, name, name, name, name);
	};
	return String8(text)
	    .ReplaceStr("<load64 base 0>", load64("base", 0))
	    .ReplaceStr("<load64 size 8>", load64("size", 8))
	    .ReplaceStr("<load64 dev 16>", load64("dev", 16))
	    .ReplaceStr("<load64 span 24>", load64("span", 24))
	    .ReplaceStr("<ptr>", m_bind->vsharp_uniform_buffer ? "_ptr_Uniform_uint" : "_ptr_PushConstant_uint")
	    .ReplaceStr("<block>", GetConstantInt(static_cast<int>(m_bind->device_address_offset_dw / 4u)));
}

// Loads `dwords` consecutive dwords at the 64-bit guest address {lo, hi} as
// %<prefix>_d<i> (uint).
bool Spirv::EmitGuestLoad(const String8& lo, const String8& hi, int dwords, const String8& prefix, String8* output) const
{
	if (output == nullptr || !UsesGuestDeviceAddress() || dwords < 1 || static_cast<uint32_t>(dwords) * 4u > kMaxLoadBytes)
	{
		return false;
	}
	const auto* p = prefix.c_str();
	*output += String8::FromPrintf("%%%s_lo64 = OpUConvert %%ulong %%%s\n"
	                               "%%%s_hi64 = OpUConvert %%ulong %%%s\n"
	                               "%%%s_his = OpShiftLeftLogical %%ulong %%%s_hi64 %%uint_32\n"
	                               "%%%s_guest = OpBitwiseOr %%ulong %%%s_his %%%s_lo64\n"
	                               "%%%s_device = OpFunctionCall %%ulong %%guest_device_address %%%s_guest %%gda_u64_%u\n",
	                               p, lo.c_str(), p, hi.c_str(), p, p, p, p, p, p, p, static_cast<uint32_t>(dwords) * 4u);
	for (int word = 0; word < dwords; word++)
	{
		*output += String8::FromPrintf("%%%s_a%d = OpIAdd %%ulong %%%s_device %%gda_u64_%d\n"
		                               "%%%s_p%d = OpConvertUToPtr %%_ptr_PhysicalStorageBuffer_uint %%%s_a%d\n"
		                               "%%%s_d%d = OpLoad %%uint %%%s_p%d Aligned 4\n",
		                               p, word, p, word * 4, p, word, p, word, p, word, p, word);
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
