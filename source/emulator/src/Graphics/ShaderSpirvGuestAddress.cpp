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
	types += String8::FromPrintf("%%gda_u64_page = OpConstant %%ulong %u\n", kGuestDeviceAddressPageBytes);
	types += String8::FromPrintf("%%gda_u64_page_mask = OpConstant %%ulong %u\n", kGuestDeviceAddressPageBytes - 1u);
	return types;
}

// ulong guest_device_address(ulong guest, ulong bytes): device address of
// `guest`, or the table's zero prefix when no imported range contains the
// whole access (GuestDeviceAddressAccessFits). Entries are sorted by guest
// base, so a binary search finds the only candidate: the last entry whose
// base is not above `guest`. A linear scan made each load cost hundreds of
// table reads, enough for a large dispatch to outlast GPU preemption.
String8 Spirv::GuestDeviceAddressFunction() const
{
	static const char* text = R"(
%guest_device_address = OpFunction %ulong DontInline %function_gda
%gda_addr = OpFunctionParameter %ulong
%gda_bytes = OpFunctionParameter %ulong
%gda_entry = OpLabel
%gda_lo_v = OpVariable %_ptr_Function_uint_gda Function
%gda_hi_v = OpVariable %_ptr_Function_uint_gda Function
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
OpStore %gda_lo_v %uint_0
OpStore %gda_hi_v %gda_count
OpBranch %gda_header
%gda_header = OpLabel
OpLoopMerge %gda_merge %gda_continue None
OpBranch %gda_cond
%gda_cond = OpLabel
%gda_lo = OpLoad %uint %gda_lo_v
%gda_hi = OpLoad %uint %gda_hi_v
%gda_more = OpULessThan %bool %gda_lo %gda_hi
OpBranchConditional %gda_more %gda_body %gda_merge
%gda_body = OpLabel
%gda_sum = OpIAdd %uint %gda_lo %gda_hi
%gda_mid = OpShiftRightLogical %uint %gda_sum %uint_1
%gda_moff = OpIMul %uint %gda_mid %uint_32
%gda_moff64 = OpUConvert %ulong %gda_moff
%gda_m = OpIAdd %ulong %gda_entries %gda_moff64
<load64 mbase m 0>
%gda_below = OpULessThanEqual %bool %gda_mbase %gda_addr
%gda_mid1 = OpIAdd %uint %gda_mid %uint_1
%gda_lo_n = OpSelect %uint %gda_below %gda_mid1 %gda_lo
%gda_hi_n = OpSelect %uint %gda_below %gda_hi %gda_mid
OpStore %gda_lo_v %gda_lo_n
OpStore %gda_hi_v %gda_hi_n
OpBranch %gda_continue
%gda_continue = OpLabel
OpBranch %gda_header
%gda_merge = OpLabel
%gda_found = OpLoad %uint %gda_lo_v
%gda_any = OpINotEqual %bool %gda_found %uint_0
%gda_pick = OpISub %uint %gda_found %uint_1
%gda_idx = OpSelect %uint %gda_any %gda_pick %uint_0
%gda_off = OpIMul %uint %gda_idx %uint_32
%gda_off64 = OpUConvert %ulong %gda_off
%gda_e = OpIAdd %ulong %gda_entries %gda_off64
<load64 base e 0>
<load64 size e 8>
<load64 dev e 16>
<load64 span e 24>
%gda_rel = OpISub %ulong %gda_addr %gda_base
%gda_started = OpULessThan %bool %gda_rel %gda_size
%gda_inspan = OpULessThan %bool %gda_rel %gda_span
%gda_room = OpISub %ulong %gda_span %gda_rel
%gda_fit = OpUGreaterThanEqual %bool %gda_room %gda_bytes
%gda_end_ok = OpLogicalAnd %bool %gda_inspan %gda_fit
%gda_inside0 = OpLogicalAnd %bool %gda_started %gda_end_ok
%gda_inside = OpLogicalAnd %bool %gda_inside0 %gda_any
%gda_device = OpIAdd %ulong %gda_dev %gda_rel
%gda_ret = OpSelect %ulong %gda_inside %gda_device %gda_table
OpReturnValue %gda_ret
OpFunctionEnd
)";
	auto load64 = [](const char* name, const char* entry, uint32_t offset)
	{
		return String8::FromPrintf("%%gda_%s_a0 = OpIAdd %%ulong %%gda_%s %%gda_u64_%u\n"
		                           "%%gda_%s_a1 = OpIAdd %%ulong %%gda_%s %%gda_u64_%u\n"
		                           "%%gda_%s_p0 = OpConvertUToPtr %%_ptr_PhysicalStorageBuffer_uint %%gda_%s_a0\n"
		                           "%%gda_%s_p1 = OpConvertUToPtr %%_ptr_PhysicalStorageBuffer_uint %%gda_%s_a1\n"
		                           "%%gda_%s_lo = OpLoad %%uint %%gda_%s_p0 Aligned 4\n"
		                           "%%gda_%s_hi = OpLoad %%uint %%gda_%s_p1 Aligned 4\n"
		                           "%%gda_%s_lo64 = OpUConvert %%ulong %%gda_%s_lo\n"
		                           "%%gda_%s_hi64 = OpUConvert %%ulong %%gda_%s_hi\n"
		                           "%%gda_%s_his = OpShiftLeftLogical %%ulong %%gda_%s_hi64 %%uint_32\n"
		                           "%%gda_%s = OpBitwiseOr %%ulong %%gda_%s_his %%gda_%s_lo64\n",
		                           name, entry, offset, name, entry, offset + 4, name, name, name, name, name, name, name, name, name, name,
		                           name, name, name, name, name, name, name);
	};
	return String8(text)
	    .ReplaceStr("<load64 mbase m 0>", load64("mbase", "m", 0))
	    .ReplaceStr("<load64 base e 0>", load64("base", "e", 0))
	    .ReplaceStr("<load64 size e 8>", load64("size", "e", 8))
	    .ReplaceStr("<load64 dev e 16>", load64("dev", "e", 16))
	    .ReplaceStr("<load64 span e 24>", load64("span", "e", 24))
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
	                               "%%%s_device = OpFunctionCall %%ulong %%guest_device_address %%%s_guest %%gda_u64_4\n",
	                               p, lo.c_str(), p, hi.c_str(), p, p, p, p, p, p, p);
	if (dwords > 1)
	{
		// Chunks have page-aligned boundaries. A load of at most 128 bytes
		// therefore needs at most two translations, and the second lookup is
		// only executed when the load crosses a page. Neither import needs to
		// make an otherwise untouched neighbouring guest page resident.
		*output += String8::FromPrintf(
		    "%%%s_page_offset = OpBitwiseAnd %%ulong %%%s_guest %%gda_u64_page_mask\n"
		    "%%%s_last_offset = OpIAdd %%ulong %%%s_page_offset %%gda_u64_%u\n"
		    "%%%s_crosses = OpUGreaterThanEqual %%bool %%%s_last_offset %%gda_u64_page\n"
		    "%%%s_next_delta = OpISub %%ulong %%gda_u64_page %%%s_page_offset\n"
		    "%%%s_next_guest = OpIAdd %%ulong %%%s_guest %%%s_next_delta\n"
		    "OpSelectionMerge %%%s_split_end None\n"
		    "OpBranchConditional %%%s_crosses %%%s_split_load %%%s_split_skip\n"
		    "%%%s_split_load = OpLabel\n"
		    "%%%s_next_device_raw = OpFunctionCall %%ulong %%guest_device_address %%%s_next_guest %%gda_u64_4\n"
		    "OpBranch %%%s_split_end\n"
		    "%%%s_split_skip = OpLabel\n"
		    "OpBranch %%%s_split_end\n"
		    "%%%s_split_end = OpLabel\n"
		    "%%%s_next_device = OpPhi %%ulong %%%s_next_device_raw %%%s_split_load %%%s_device %%%s_split_skip\n",
		    p, p, p, p, static_cast<uint32_t>(dwords - 1) * 4u, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p);
	}
	for (int word = 0; word < dwords; word++)
	{
		if (word == 0)
		{
			*output += String8::FromPrintf("%%%s_a0 = OpCopyObject %%ulong %%%s_device\n", p, p);
		} else
		{
			*output += String8::FromPrintf(
			    "%%%s_offset%d = OpIAdd %%ulong %%%s_page_offset %%gda_u64_%d\n"
			    "%%%s_second%d = OpUGreaterThanEqual %%bool %%%s_offset%d %%gda_u64_page\n"
			    "%%%s_next_offset%d = OpISub %%ulong %%%s_offset%d %%gda_u64_page\n"
			    "%%%s_next_addr%d = OpIAdd %%ulong %%%s_next_device %%%s_next_offset%d\n"
			    "%%%s_first_addr%d = OpIAdd %%ulong %%%s_device %%gda_u64_%d\n"
			    "%%%s_a%d = OpSelect %%ulong %%%s_second%d %%%s_next_addr%d %%%s_first_addr%d\n",
			    p, word, p, word * 4, p, word, p, word, p, word, p, word, p, word, p, p, word,
			    p, word, p, word * 4, p, word, p, word, p, word, p, word);
		}
		*output += String8::FromPrintf("%%%s_p%d = OpConvertUToPtr %%_ptr_PhysicalStorageBuffer_uint %%%s_a%d\n"
		                               "%%%s_d%d = OpLoad %%uint %%%s_p%d Aligned 4\n",
		                               p, word, p, word, p, word, p, word);
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
