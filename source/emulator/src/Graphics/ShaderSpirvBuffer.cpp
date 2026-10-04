#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvLds.h"
#include "ShaderSpirvTemplates.h"
#include "ShaderStorageAnalysis.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveLds.h"
#include "Emulator/Graphics/ShaderComputeWaveResourceAnalysis.h"

#include <cinttypes>
#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static String8 GetBufferOffsetIntConstant(Spirv* spirv, ShaderOperand op)
{
	if (!operand_is_constant(op)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(op) condition ignored (continuing)\n"); }
	int value = 0;
	if (op.type == ShaderOperandType::IntegerInlineConstant)
	{
		value = op.constant.i;
	} else if (op.type == ShaderOperandType::LiteralConstant)
	{
		value = static_cast<int>(op.constant.u);
	} else if (op.type == ShaderOperandType::FloatInlineConstant)
	{
		// Rare: treat bit pattern as unsigned immediate.
		value = static_cast<int>(op.constant.u);
	} else
	{
		if (true) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: true condition ignored (continuing)\n"); }
	}
	String8 id = spirv->GetConstantInt(value);
	if (id == "unknown_int_constant") { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: id == unknown_int_constant condition ignored (continuing)\n"); }
	return id;
}

enum class BufferAddressRangeCheck
{
	None,
	RawZeroRecord,
};

static constexpr uint32_t SPIRV_SCOPE_DEVICE             = 1;
static constexpr uint32_t SPIRV_SCOPE_WORKGROUP          = 2;
static constexpr uint32_t SPIRV_ACQUIRE_RELEASE          = 0x8;
static constexpr uint32_t SPIRV_UNIFORM_MEMORY           = 0x40;
static constexpr uint32_t SPIRV_WORKGROUP_MEMORY         = 0x100;
const uint32_t SPIRV_DEVICE_MEMORY_ACQ_REL    = SPIRV_ACQUIRE_RELEASE | SPIRV_UNIFORM_MEMORY;
const uint32_t SPIRV_WORKGROUP_MEMORY_ACQ_REL = SPIRV_ACQUIRE_RELEASE | SPIRV_WORKGROUP_MEMORY;

struct BufferAddressSetup
{
	String8 source;
	String8 access_enabled;
	String8 buffer_index;
	String8 byte_offset;
};

// Materialize a descriptor-driven byte address into the legacy buffer helper
// scratch arguments. All MUBUF/MTBUF users share this so index/offset ordering
// and the Gen5 swizzle equation cannot drift between loads and stores.
static bool emit_buffer_address_setup(Spirv* spirv, const ShaderInstruction& inst, int instruction_index,
                                      uint32_t component_byte_offset, uint32_t access_size_bytes,
                                      BufferAddressRangeCheck range_check, BufferAddressSetup* setup,
                                      bool dynamic_packed_format_width = false)
{
	EXIT_IF(spirv == nullptr);
	EXIT_IF(setup == nullptr);

	if (inst.src_num < 3 || inst.src[1].size < 4 || access_size_bytes == 0)
	{
		return false;
	}

	const auto descriptor0 = operand_variable_to_str(inst.src[1], 0);
	const auto descriptor1 = operand_variable_to_str(inst.src[1], 1);
	const auto descriptor3 = operand_variable_to_str(inst.src[1], 3);
	if (descriptor0.type != SpirvType::Uint || descriptor1.type != SpirvType::Uint || descriptor3.type != SpirvType::Uint)
	{
		return false;
	}

	const auto tag = String8::FromPrintf("%u_%u", instruction_index, component_byte_offset);
	const auto make_name = [&tag](const char* prefix) { return String8::FromPrintf("%s_%s", prefix, tag.c_str()); };
	const auto make_id = [](const String8& name) { return String8::FromPrintf("%%%s", name.c_str()); };

	String8 range_guard;
	setup->access_enabled = "";
	if (range_check == BufferAddressRangeCheck::RawZeroRecord)
	{
		const auto descriptor2 = operand_variable_to_str(inst.src[1], 2);
		if (descriptor2.type != SpirvType::Uint)
		{
			return false;
		}

		const auto access_enabled_name = make_name("buf_addr_access_enabled");
		setup->access_enabled          = make_id(access_enabled_name);
		range_guard = String8(R"(
        %buf_addr_desc2_<tag> = OpLoad %uint %<desc2>
        %buf_addr_records_empty_<tag> = OpIEqual %bool %buf_addr_desc2_<tag> %uint_0
        %buf_addr_access_enabled_<tag> = OpLogicalNot %bool %buf_addr_records_empty_<tag>
)")
		                  .ReplaceStr("<tag>", tag)
		                  .ReplaceStr("<desc2>", descriptor2.value);
	}

	String8 index_load;
	String8 index_id = "%uint_0";
	if (inst.buffer_idxen)
	{
		const auto index_name = make_name("buf_addr_index");
		// With IDXEN+OFFEN, VADDR is [element index, byte offset].
		const int  index_part = inst.buffer_offen ? 0 : -1;
		if (!operand_load_uint(spirv, inst.src[0], index_name, tag, &index_load, index_part))
		{
			return false;
		}
		index_id = make_id(index_name);
	}

	String8 vector_offset_load;
	String8 vector_offset_id;
	if (inst.buffer_offen)
	{
		const auto offset_name = make_name("buf_addr_voffset");
		const int  offset_part = inst.buffer_idxen ? 1 : 0;
		if (!operand_load_uint(spirv, inst.src[0], offset_name, tag, &vector_offset_load, offset_part))
		{
			return false;
		}
		vector_offset_id = make_id(offset_name);
	}

	const auto scalar_offset_name = make_name("buf_addr_soffset");
	String8    scalar_offset_load;
	if (!operand_load_uint(spirv, inst.src[2], scalar_offset_name, tag, &scalar_offset_load))
	{
		return false;
	}

	const auto immediate_id = spirv->GetConstantUint(inst.buffer_imm_offset);
	if (immediate_id == "unknown_uint_constant")
	{
		return false;
	}
	String8 offset_setup;
	String8 offset_id = make_id(immediate_id);
	if (component_byte_offset != 0)
	{
		const auto component_name = make_name("buf_addr_imm");
		const auto component_id   = spirv->GetConstantUint(component_byte_offset);
		if (component_id == "unknown_uint_constant")
		{
			return false;
		}
		offset_setup += String8("%<name> = OpIAdd %uint <offset> <component>\n")
		                    .ReplaceStr("<name>", component_name)
		                    .ReplaceStr("<offset>", offset_id)
		                    .ReplaceStr("<component>", make_id(component_id));
		offset_id = make_id(component_name);
	}
	if (inst.buffer_offen)
	{
		const auto offset_name = make_name("buf_addr_offset");
		offset_setup += String8("%<name> = OpIAdd %uint <offset> <voffset>\n")
		                    .ReplaceStr("<name>", offset_name)
		                    .ReplaceStr("<offset>", offset_id)
		                    .ReplaceStr("<voffset>", vector_offset_id);
		offset_id = make_id(offset_name);
	}

	// desc0 is a rewritten slot only when it is strictly less than the bound
	// SSBO count. A live V# keeps the 48-bit guest base in that word; using
	// it as buf[i] is an OOB index and loses the device. Start from the slot
	// path or zero, overlay a stream span, then clamp again.
	String8     live_resolve;
	String8     live_access_guard;
	String8     access_size_setup;
	String8     resolver_probe;
	String8     resolved_access_width = spirv->GetConstantUint(access_size_bytes);
	String8     addr_value = String8::FromPrintf("%%buf_addr_%s", tag.c_str());
	String8     slot_value = String8::FromPrintf("%%buf_addr_desc0_%s", tag.c_str());
	const auto* vs_input   = spirv->GetVsInputInfo();
	const auto* bind_info  = spirv->GetBindInfo();
	if (vs_input != nullptr && vs_input->fetch_embedded && bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		const auto bound_id = spirv->GetConstantUint(static_cast<uint32_t>(bind_info->storage_buffers.buffers_num));
		const auto access_size_id = spirv->GetConstantUint(access_size_bytes);
		if (bound_id != "unknown_uint_constant" && access_size_id != "unknown_uint_constant")
		{
			String8 access_size_value = access_size_id;
			if (dynamic_packed_format_width && (access_size_bytes == 8u || access_size_bytes == 12u))
			{
				const uint32_t packed_format = access_size_bytes == 8u ? 29u : 71u;
				const uint32_t packed_size   = access_size_bytes == 8u ? 4u : 8u;
				const auto     packed_format_id = spirv->GetConstantInt(static_cast<int>(packed_format));
				const auto     packed_size_id   = spirv->GetConstantUint(packed_size);
				if (packed_format_id == "unknown_uint_constant" || packed_size_id == "unknown_uint_constant")
				{
					return false;
				}
				access_size_setup = String8(R"(
		%buf_addr_format_u_<tag> = OpShiftRightLogical %uint %buf_addr_desc3_<tag> %uint_12
		%buf_addr_format_<tag> = OpBitwiseAnd %uint %buf_addr_format_u_<tag> %uint_127
		%buf_addr_format_i_<tag> = OpBitcast %int %buf_addr_format_<tag>
		%buf_addr_packed_<tag> = OpIEqual %bool %buf_addr_format_i_<tag> %<packed_format>
		%buf_addr_access_size_<tag> = OpSelect %uint %buf_addr_packed_<tag> %<packed_size> %<unpacked_size>
)")
				                        .ReplaceStr("<tag>", tag)
				                        .ReplaceStr("<packed_format>", packed_format_id)
				                        .ReplaceStr("<packed_size>", packed_size_id)
				                        .ReplaceStr("<unpacked_size>", access_size_id);
				access_size_value = String8::FromPrintf("buf_addr_access_size_%s", tag.c_str());
				resolved_access_width = access_size_value;
			}
			const char* vsharp_ptr = bind_info->vsharp_uniform_buffer ? "_ptr_Uniform_uint" : "_ptr_PushConstant_uint";
			live_resolve           = String8(R"(
        %buf_addr_is_slot_<tag> = OpULessThan %bool %buf_addr_desc0_<tag> %<bound>
        %buf_addr_slot_0_<tag> = OpSelect %uint %buf_addr_is_slot_<tag> %buf_addr_desc0_<tag> %uint_0
        %buf_addr_off_0_<tag> = OpSelect %uint %buf_addr_is_slot_<tag> %buf_addr_<tag> %uint_0
)")
			                  .ReplaceStr("<tag>", tag)
			                  .ReplaceStr("<bound>", bound_id);
				String8 slot_name = String8::FromPrintf("buf_addr_slot_0_%s", tag.c_str());
				String8 off_name  = String8::FromPrintf("buf_addr_off_0_%s", tag.c_str());
				String8 valid_name = String8::FromPrintf("buf_addr_is_slot_%s", tag.c_str());
				int     span_i    = 0;
			for (int i = 0; i < vs_input->buffers_num; i++)
			{
				const auto& stream = vs_input->buffers[i];
				if (stream.storage_slot < 0)
				{
					continue;
				}
				const auto slot_const = spirv->GetConstantInt(stream.storage_slot);
				const auto slot_u     = spirv->GetConstantUint(static_cast<uint32_t>(stream.storage_slot));
				if (slot_const == "unknown_int_constant" || slot_u == "unknown_uint_constant")
				{
					continue;
				}
					const auto next_slot = String8::FromPrintf("buf_addr_slot_%d_%s", span_i + 1, tag.c_str());
					const auto next_off  = String8::FromPrintf("buf_addr_off_%d_%s", span_i + 1, tag.c_str());
					const auto span = String8::FromPrintf("%d", span_i);
				live_resolve += String8(R"(
        %buf_addr_bptr_<tag>_<span> = OpAccessChain %<vsharp_ptr> %vsharp %int_0 %<slot_idx> %int_0
        %buf_addr_base_<tag>_<span> = OpLoad %uint %buf_addr_bptr_<tag>_<span>
        %buf_addr_sptr_<tag>_<span> = OpAccessChain %<vsharp_ptr> %vsharp %int_0 %<slot_idx> %int_1
        %buf_addr_w1_<tag>_<span> = OpLoad %uint %buf_addr_sptr_<tag>_<span>
        %buf_addr_sh_<tag>_<span> = OpShiftRightLogical %uint %buf_addr_w1_<tag>_<span> %uint_16
        %buf_addr_stride_<tag>_<span> = OpBitwiseAnd %uint %buf_addr_sh_<tag>_<span> %uint_0x00003fff
        %buf_addr_rptr_<tag>_<span> = OpAccessChain %<vsharp_ptr> %vsharp %int_0 %<slot_idx> %int_2
        %buf_addr_recs_<tag>_<span> = OpLoad %uint %buf_addr_rptr_<tag>_<span>
        %buf_addr_bytes_<tag>_<span> = OpIMul %uint %buf_addr_stride_<tag>_<span> %buf_addr_recs_<tag>_<span>
        %buf_addr_rel_<tag>_<span> = OpISub %uint %buf_addr_desc0_<tag> %buf_addr_base_<tag>_<span>
        %buf_addr_sum_<tag>_<span> = OpIAdd %uint %buf_addr_rel_<tag>_<span> %buf_addr_<tag>
		%buf_addr_bytes_enough_<tag>_<span> = OpUGreaterThanEqual %bool %buf_addr_bytes_<tag>_<span> %<access_size>
		%buf_addr_last_<tag>_<span> = OpISub %uint %buf_addr_bytes_<tag>_<span> %<access_size>
		%buf_addr_fits_<tag>_<span> = OpULessThanEqual %bool %buf_addr_sum_<tag>_<span> %buf_addr_last_<tag>_<span>
		%buf_addr_in_<tag>_<span> = OpLogicalAnd %bool %buf_addr_bytes_enough_<tag>_<span> %buf_addr_fits_<tag>_<span>
		%buf_addr_no_wrap_<tag>_<span> = OpUGreaterThanEqual %bool %buf_addr_sum_<tag>_<span> %buf_addr_rel_<tag>_<span>
        %buf_addr_ge_<tag>_<span> = OpUGreaterThanEqual %bool %buf_addr_desc0_<tag> %buf_addr_base_<tag>_<span>
        %buf_addr_hi_<tag>_<span> = OpBitwiseAnd %uint %buf_addr_desc1_<tag> %uint_0x0000ffff
        %buf_addr_vhi_<tag>_<span> = OpBitwiseAnd %uint %buf_addr_w1_<tag>_<span> %uint_0x0000ffff
		%buf_addr_same_hi_<tag>_<span> = OpIEqual %bool %buf_addr_hi_<tag>_<span> %buf_addr_vhi_<tag>_<span>
		%buf_addr_same_page_forward_<tag>_<span> = OpLogicalAnd %bool %buf_addr_same_hi_<tag>_<span> %buf_addr_ge_<tag>_<span>
		%buf_addr_next_vhi_<tag>_<span> = OpIAdd %uint %buf_addr_vhi_<tag>_<span> %uint_1
		%buf_addr_vhi_can_advance_<tag>_<span> = OpULessThan %bool %buf_addr_vhi_<tag>_<span> %uint_0x0000ffff
		%buf_addr_next_hi_match_<tag>_<span> = OpIEqual %bool %buf_addr_hi_<tag>_<span> %buf_addr_next_vhi_<tag>_<span>
		%buf_addr_next_hi_valid_<tag>_<span> = OpLogicalAnd %bool %buf_addr_vhi_can_advance_<tag>_<span> %buf_addr_next_hi_match_<tag>_<span>
		%buf_addr_low_wrapped_<tag>_<span> = OpULessThan %bool %buf_addr_desc0_<tag> %buf_addr_base_<tag>_<span>
		%buf_addr_next_page_forward_<tag>_<span> = OpLogicalAnd %bool %buf_addr_next_hi_valid_<tag>_<span> %buf_addr_low_wrapped_<tag>_<span>
		%buf_addr_contiguous_<tag>_<span> = OpLogicalOr %bool %buf_addr_same_page_forward_<tag>_<span> %buf_addr_next_page_forward_<tag>_<span>
		%buf_addr_range_<tag>_<span> = OpLogicalAnd %bool %buf_addr_in_<tag>_<span> %buf_addr_no_wrap_<tag>_<span>
		%buf_addr_live_<tag>_<span> = OpLogicalAnd %bool %buf_addr_range_<tag>_<span> %buf_addr_contiguous_<tag>_<span>
	        %<next_slot> = OpSelect %uint %buf_addr_live_<tag>_<span> %<slot_u> %<prev_slot>
	        %<next_off> = OpSelect %uint %buf_addr_live_<tag>_<span> %buf_addr_sum_<tag>_<span> %<prev_off>
			%<next_valid> = OpLogicalOr %bool %<prev_valid> %buf_addr_live_<tag>_<span>
)")
				                    .ReplaceStr("<tag>", tag)
				                    .ReplaceStr("<span>", span)
				                    .ReplaceStr("<vsharp_ptr>", vsharp_ptr)
				                    .ReplaceStr("<slot_idx>", slot_const)
				                    .ReplaceStr("<slot_u>", slot_u)
				                    .ReplaceStr("<access_size>", access_size_value)
					                    .ReplaceStr("<next_slot>", next_slot)
					                    .ReplaceStr("<next_off>", next_off)
					                    .ReplaceStr("<next_valid>", String8::FromPrintf("buf_addr_valid_%d_%s", span_i + 1, tag.c_str()))
					                    .ReplaceStr("<prev_slot>", slot_name)
					                    .ReplaceStr("<prev_off>", off_name)
					                    .ReplaceStr("<prev_valid>", valid_name);
					slot_name = next_slot;
					off_name  = next_off;
					valid_name = String8::FromPrintf("buf_addr_valid_%d_%s", span_i + 1, tag.c_str());
				++span_i;
			}
			live_resolve += String8(R"(
		%buf_addr_clamp_<tag> = OpULessThan %bool %<prev_slot> %<bound>
			%buf_addr_valid_c_<tag> = OpLogicalAnd %bool %<prev_valid> %buf_addr_clamp_<tag>
			%buf_addr_slot_c_<tag> = OpSelect %uint %buf_addr_valid_c_<tag> %<prev_slot> %uint_0
			%buf_addr_off_c_<tag> = OpSelect %uint %buf_addr_valid_c_<tag> %<prev_off> %uint_0
)")
			                    .ReplaceStr("<tag>", tag)
			                    .ReplaceStr("<bound>", bound_id)
					                    .ReplaceStr("<prev_slot>", slot_name)
					                    .ReplaceStr("<prev_off>", off_name)
					                    .ReplaceStr("<prev_valid>", valid_name);
				addr_value = String8::FromPrintf("%%buf_addr_off_c_%s", tag.c_str());
				slot_value = String8::FromPrintf("%%buf_addr_slot_c_%s", tag.c_str());
			const auto live_valid = String8::FromPrintf("%%buf_addr_valid_c_%s", tag.c_str());
			if (setup->access_enabled.IsEmpty())
			{
				setup->access_enabled = live_valid;
			} else
			{
				const auto final_name = make_name("buf_addr_access_final");
				live_access_guard = String8("        %<final> = OpLogicalAnd %bool <range_valid> <live_valid>\n")
				                        .ReplaceStr("<final>", final_name)
				                        .ReplaceStr("<range_valid>", setup->access_enabled)
				                        .ReplaceStr("<live_valid>", live_valid);
				setup->access_enabled = make_id(final_name);
			}
		}
	}
	if (spirv->UsesVertexClipProbe() && !setup->access_enabled.IsEmpty())
	{
		const auto instruction_pc = spirv->GetConstantUint(inst.pc);
		if (instruction_pc == "unknown_uint_constant" || resolved_access_width == "unknown_uint_constant")
		{
			return false;
		}
		resolver_probe = String8(R"(
%vertex_resolver_claim_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_22
%vertex_resolver_claim_prior_<tag> = OpAtomicCompareExchange %uint %vertex_resolver_claim_ptr_<tag> %uint_1 %uint_72 %uint_0 %uint_1 %uint_0
%vertex_resolver_claim_won_<tag> = OpIEqual %bool %vertex_resolver_claim_prior_<tag> %uint_0
               OpSelectionMerge %vertex_resolver_claim_merge_<tag> None
               OpBranchConditional %vertex_resolver_claim_won_<tag> %vertex_resolver_claim_store_<tag> %vertex_resolver_claim_merge_<tag>
%vertex_resolver_claim_store_<tag> = OpLabel
%vertex_resolver_pc_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_23
               OpStore %vertex_resolver_pc_ptr_<tag> %<instruction_pc>
%vertex_resolver_width_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_24
               OpStore %vertex_resolver_width_ptr_<tag> %<access_width>
%vertex_resolver_desc0_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_25
               OpStore %vertex_resolver_desc0_ptr_<tag> %buf_addr_desc0_<tag>
%vertex_resolver_desc1_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_26
               OpStore %vertex_resolver_desc1_ptr_<tag> %buf_addr_desc1_<tag>
%vertex_resolver_raw_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_27
               OpStore %vertex_resolver_raw_ptr_<tag> %buf_addr_<tag>
%vertex_resolver_valid_u_<tag> = OpSelect %uint <final_valid> %uint_1 %uint_0
%vertex_resolver_valid_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_28
               OpStore %vertex_resolver_valid_ptr_<tag> %vertex_resolver_valid_u_<tag>
%vertex_resolver_slot_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_29
               OpStore %vertex_resolver_slot_ptr_<tag> <final_slot>
%vertex_resolver_offset_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_30
               OpStore %vertex_resolver_offset_ptr_<tag> <final_offset>
               OpBranch %vertex_resolver_claim_merge_<tag>
%vertex_resolver_claim_merge_<tag> = OpLabel
)")
		                     .ReplaceStr("<tag>", tag)
		                     .ReplaceStr("<instruction_pc>", instruction_pc)
		                     .ReplaceStr("<access_width>", resolved_access_width)
		                     .ReplaceStr("<final_valid>", setup->access_enabled)
		                     .ReplaceStr("<final_slot>", slot_value)
		                     .ReplaceStr("<final_offset>", addr_value);
	}

	static const char* text = R"(
        %buf_addr_desc0_<tag> = OpLoad %uint %<desc0>
        %buf_addr_desc1_<tag> = OpLoad %uint %<desc1>
        %buf_addr_desc3_<tag> = OpLoad %uint %<desc3>
<range_guard>
        %buf_addr_<tag> = OpFunctionCall %uint %buffer_raw_address <index_value> <offset_value> <soffset_value> %buf_addr_desc1_<tag> %buf_addr_desc3_<tag>
<access_size_setup>
<live_resolve>
<live_access_guard>
<resolver_probe>
        %buf_addr_i_<tag> = OpBitcast %int <addr_value>
        %buf_addr_buffer_i_<tag> = OpBitcast %int <slot_value>
               OpStore %temp_int_1 %int_0
               OpStore %temp_int_2 %buf_addr_i_<tag>
               OpStore %temp_int_3 %int_0
               OpStore %temp_int_4 %buf_addr_buffer_i_<tag>
)";

	setup->source = index_load;
	if (!setup->source.IsEmpty())
	{
		setup->source += '\n';
	}
	setup->source += vector_offset_load;
	if (!setup->source.IsEmpty() && !scalar_offset_load.IsEmpty())
	{
		setup->source += '\n';
	}
	setup->source += scalar_offset_load;
	if (!setup->source.IsEmpty() && !offset_setup.IsEmpty())
	{
		setup->source += '\n';
	}
	setup->source += offset_setup;
	if (!setup->source.IsEmpty())
	{
		setup->source += '\n';
	}
	setup->source += String8(text)
	                     .ReplaceStr("<tag>", tag)
	                     .ReplaceStr("<desc0>", descriptor0.value)
	                     .ReplaceStr("<desc1>", descriptor1.value)
	                     .ReplaceStr("<desc3>", descriptor3.value)
	                     .ReplaceStr("<range_guard>", range_guard)
	                     .ReplaceStr("<access_size_setup>", access_size_setup)
	                     .ReplaceStr("<live_resolve>", live_resolve)
	                     .ReplaceStr("<live_access_guard>", live_access_guard)
	                     .ReplaceStr("<resolver_probe>", resolver_probe)
	                     .ReplaceStr("<addr_value>", addr_value)
	                     .ReplaceStr("<slot_value>", slot_value)
	                     .ReplaceStr("<index_value>", index_id)
	                     .ReplaceStr("<offset_value>", offset_id)
	                     .ReplaceStr("<soffset_value>", make_id(scalar_offset_name));
	setup->buffer_index = slot_value;
	setup->byte_offset  = addr_value;

	return true;
}

static bool emit_gen5_raw_buffer_load(Spirv* spirv, const ShaderInstruction& inst, int instruction_index,
                                      uint32_t dwords, String8* dst_source)
{
	EXIT_IF(spirv == nullptr);
	EXIT_IF(dst_source == nullptr);

	const auto zero_float_id = spirv->GetConstantFloat(0.0f);
	if (zero_float_id == "unknown_float_constant")
	{
		return false;
	}
	if (dwords == 0 || dwords > 4u)
	{
		return false;
	}

	SpirvValue values[4];
	String8    body;
	String8    zero_body;
	for (uint32_t component = 0; component < dwords; component++)
	{
		values[component] = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (values[component].type != SpirvType::Float)
		{
			return false;
		}
		if (component != 0)
		{
			const auto component_id = spirv->GetConstantUint(component * 4u);
			if (component_id == "unknown_uint_constant")
			{
				return false;
			}
			body += String8(R"(
		%gen5_raw_load_off_<index>_<component> = OpIAdd %uint <byte_offset> %<component_offset>
		%gen5_raw_load_off_i_<index>_<component> = OpBitcast %int %gen5_raw_load_off_<index>_<component>
		       OpStore %temp_int_2 %gen5_raw_load_off_i_<index>_<component>
)")
			            .ReplaceStr("<index>", String8::FromPrintf("%u", instruction_index))
			            .ReplaceStr("<component>", String8::FromPrintf("%u", component))
			            .ReplaceStr("<byte_offset>", "<byte_offset>")
			            .ReplaceStr("<component_offset>", component_id);
		}
		body += String8::FromPrintf("%%gen5_raw_load_%u_%u = OpFunctionCall %%void %%buffer_load_float1 %%%s %%temp_int_1 %%temp_int_2 %%temp_int_3 %%temp_int_4\n",
		                            instruction_index, component, values[component].value.c_str());
		zero_body += String8::FromPrintf("               OpStore %%%s %%%s\n", values[component].value.c_str(), zero_float_id.c_str());
	}

	BufferAddressSetup address_setup;
	if (!emit_buffer_address_setup(spirv, inst, instruction_index, 0, dwords * 4u, BufferAddressRangeCheck::RawZeroRecord,
	                               &address_setup) ||
	    address_setup.access_enabled.IsEmpty())
	{
		return false;
	}
	body = body.ReplaceStr("<byte_offset>", address_setup.byte_offset);
	const auto tag = String8::FromPrintf("%u_0", instruction_index);
	*dst_source += String8(R"(
<address_setup>
               OpSelectionMerge %gen5_raw_load_merge_<tag> None
               OpBranchConditional <access_enabled> %gen5_raw_load_then_<tag> %gen5_raw_load_oob_<tag>
        %gen5_raw_load_then_<tag> = OpLabel
<body>
               OpBranch %gen5_raw_load_merge_<tag>
        %gen5_raw_load_oob_<tag> = OpLabel
<zero_body>
               OpBranch %gen5_raw_load_merge_<tag>
        %gen5_raw_load_merge_<tag> = OpLabel
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<access_enabled>", address_setup.access_enabled)
	                   .ReplaceStr("<body>", body)
	                   .ReplaceStr("<zero_body>", zero_body)
	                   .ReplaceStr("<tag>", tag);
	return true;
}

// True when the instruction's V# register range is one of the bound storage
// buffers; any other V# is built at run time (for example from s_getpc).
static bool buffer_resource_is_bound(const Spirv* spirv, const ShaderOperand& resource)
{
	const auto* bind = spirv->GetBindInfo();
	const auto* vertex = spirv->GetVsInputInfo();
	const int user_data_base = vertex != nullptr && vertex->gs_prolog ? 8 : 0;
	return bind != nullptr && ShaderStorageBufferResourceIsBound(*bind, resource, user_data_base);
}

// MUBUF load through a V# that no binding describes: the descriptor's 48-bit
// base plus the RDNA2 buffer offset (index * stride + voffset + soffset +
// imm) is read through guest device addressing. Out-of-range accesses read
// zero: structured (stride != 0 with IDXEN) checks the index against
// NUM_RECORDS, raw checks the byte range.
static bool emit_guest_buffer_load(Spirv* spirv, const ShaderInstruction& inst, int instruction_index, uint32_t dwords,
                                   String8* dst_source)
{
	if (!spirv->UsesGuestDeviceAddress() || dwords == 0 || dwords > 4u || inst.src_num < 3 || inst.src[1].size < 4)
	{
		return false;
	}
	const auto tag  = String8::FromPrintf("gbl_%d", instruction_index);
	const auto zero = spirv->GetConstantFloat(0.0f);
	String8    loads;
	if (inst.buffer_idxen && !operand_load_uint(spirv, inst.src[0], tag + "_idx", tag, &loads, inst.buffer_offen ? 0 : -1))
	{
		return false;
	}
	String8 voffset;
	if (inst.buffer_offen && !operand_load_uint(spirv, inst.src[0], tag + "_voff", tag, &voffset, inst.buffer_idxen ? 1 : 0))
	{
		return false;
	}
	String8 soffset;
	if (!operand_load_uint(spirv, inst.src[2], tag + "_soff", tag, &soffset))
	{
		return false;
	}
	String8 source = loads + "\n" + voffset + "\n" + soffset + "\n";
	source += String8(R"(%<t>_d0 = OpLoad %uint %<desc0>
%<t>_d1 = OpLoad %uint %<desc1>
%<t>_d2 = OpLoad %uint %<desc2>
%<t>_stride_raw = OpShiftRightLogical %uint %<t>_d1 %uint_16
%<t>_stride = OpBitwiseAnd %uint %<t>_stride_raw %uint_0x00003fff
%<t>_base_hi = OpBitwiseAnd %uint %<t>_d1 %<mask16>
%<t>_elem = OpIMul %uint <index> %<t>_stride
%<t>_o0 = OpIAdd %uint %<t>_elem %<t>_soff
%<t>_o1 = OpIAdd %uint %<t>_o0 %<imm>
%<t>_off = OpIAdd %uint %<t>_o1 <voffset>
%<t>_structured_s = OpINotEqual %bool %<t>_stride %uint_0
%<t>_structured = OpLogicalAnd %bool %<t>_structured_s %<idxen>
%<t>_in_index = OpULessThan %bool <index> %<t>_d2
%<t>_room = OpUGreaterThanEqual %bool %<t>_d2 %<size>
%<t>_last = OpISub %uint %<t>_d2 %<size>
%<t>_fits = OpULessThanEqual %bool %<t>_off %<t>_last
%<t>_in_raw = OpLogicalAnd %bool %<t>_room %<t>_fits
%<t>_in = OpSelect %bool %<t>_structured %<t>_in_index %<t>_in_raw
%<t>_lo = OpIAdd %uint %<t>_d0 %<t>_off
%<t>_carry_b = OpULessThan %bool %<t>_lo %<t>_d0
%<t>_carry = OpSelect %uint %<t>_carry_b %uint_1 %uint_0
%<t>_hi = OpIAdd %uint %<t>_base_hi %<t>_carry
OpSelectionMerge %<t>_merge None
OpBranchConditional %<t>_in %<t>_then %<t>_oob
%<t>_then = OpLabel
)")
	              .ReplaceStr("<t>", tag)
	              .ReplaceStr("<desc0>", operand_variable_to_str(inst.src[1], 0).value)
	              .ReplaceStr("<desc1>", operand_variable_to_str(inst.src[1], 1).value)
	              .ReplaceStr("<desc2>", operand_variable_to_str(inst.src[1], 2).value)
	              .ReplaceStr("<mask16>", spirv->GetConstantUint(0xffffu))
	              .ReplaceStr("<index>", inst.buffer_idxen ? "%" + tag + "_idx" : String8("%uint_0"))
	              .ReplaceStr("<voffset>", inst.buffer_offen ? "%" + tag + "_voff" : String8("%uint_0"))
	              .ReplaceStr("<imm>", spirv->GetConstantUint(inst.buffer_imm_offset))
	              .ReplaceStr("<idxen>", inst.buffer_idxen ? "true" : "false")
	              .ReplaceStr("<size>", spirv->GetConstantUint(dwords * 4u));
	if (!spirv->EmitGuestLoad(tag + "_lo", tag + "_hi", static_cast<int>(dwords), tag + "_g", &source))
	{
		return false;
	}
	String8 zeros;
	for (uint32_t component = 0; component < dwords; component++)
	{
		const auto dst = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (dst.type != SpirvType::Float)
		{
			return false;
		}
		source += String8::FromPrintf("%%%s_f%u = OpBitcast %%float %%%s_g_d%u\nOpStore %%%s %%%s_f%u\n", tag.c_str(), component, tag.c_str(),
		                              component, dst.value.c_str(), tag.c_str(), component);
		zeros += String8::FromPrintf("OpStore %%%s %%%s\n", dst.value.c_str(), zero.c_str());
	}
	source += String8::FromPrintf("OpBranch %%%s_merge\n%%%s_oob = OpLabel\n%sOpBranch %%%s_merge\n%%%s_merge = OpLabel\n", tag.c_str(),
	                              tag.c_str(), zeros.c_str(), tag.c_str(), tag.c_str());
	*dst_source += source;
	return true;
}

static bool emit_gen5_raw_buffer_store(Spirv* spirv, const ShaderInstruction& inst, int instruction_index,
                                       uint32_t dwords, String8* dst_source)
{
	EXIT_IF(spirv == nullptr);
	EXIT_IF(dst_source == nullptr);

	SpirvValue values[4];
	if (dwords == 0 || dwords > 4u)
	{
		return false;
	}
	for (uint32_t component = 0; component < dwords; component++)
	{
		values[component] = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (values[component].type != SpirvType::Float)
		{
			return false;
		}
	}

	BufferAddressSetup first_address_setup;
	if (!emit_buffer_address_setup(spirv, inst, instruction_index, 0, dwords * 4u, BufferAddressRangeCheck::RawZeroRecord,
	                               &first_address_setup) ||
	    first_address_setup.access_enabled.IsEmpty())
	{
		return false;
	}

	String8 body = String8::FromPrintf("%%gen5_raw_store_%u_0 = OpFunctionCall %%void %%buffer_store_float1 %%%s %%temp_int_1 %%temp_int_2 %%temp_int_3 %%temp_int_4\n",
	                                  instruction_index, values[0].value.c_str());
	for (uint32_t component = 1; component < dwords; component++)
	{
		const auto component_id = spirv->GetConstantUint(component * 4u);
		if (component_id == "unknown_uint_constant")
		{
			return false;
		}
		body += String8(R"(
		%gen5_raw_store_off_<index>_<component> = OpIAdd %uint <byte_offset> %<component_offset>
		%gen5_raw_store_off_i_<index>_<component> = OpBitcast %int %gen5_raw_store_off_<index>_<component>
		       OpStore %temp_int_2 %gen5_raw_store_off_i_<index>_<component>
)")
		            .ReplaceStr("<index>", String8::FromPrintf("%u", instruction_index))
		            .ReplaceStr("<component>", String8::FromPrintf("%u", component))
		            .ReplaceStr("<byte_offset>", first_address_setup.byte_offset)
		            .ReplaceStr("<component_offset>", component_id);
		body += String8::FromPrintf("%%gen5_raw_store_%u_%u = OpFunctionCall %%void %%buffer_store_float1 %%%s %%temp_int_1 %%temp_int_2 %%temp_int_3 %%temp_int_4\n",
		                             instruction_index, component, values[component].value.c_str());
	}

	static const char* text = R"(
<address_setup>
        %gen5_raw_store_exec_<index> = OpLoad %uint %exec_lo
        %gen5_raw_store_active_<index> = OpINotEqual %bool %gen5_raw_store_exec_<index> %uint_0
        %gen5_raw_store_enabled_<index> = OpLogicalAnd %bool %gen5_raw_store_active_<index> <access_enabled>
               OpSelectionMerge %gen5_raw_store_merge_<index> None
               OpBranchConditional %gen5_raw_store_enabled_<index> %gen5_raw_store_then_<index> %gen5_raw_store_merge_<index>
        %gen5_raw_store_then_<index> = OpLabel
<body>
               OpBranch %gen5_raw_store_merge_<index>
        %gen5_raw_store_merge_<index> = OpLabel
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", instruction_index))
	                   .ReplaceStr("<address_setup>", first_address_setup.source)
	                   .ReplaceStr("<access_enabled>", first_address_setup.access_enabled)
	                   .ReplaceStr("<body>", body);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferAtomicAdd_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();
	if (!Config::IsNextGen() || bind_info == nullptr || bind_info->storage_buffers.buffers_num == 0)
	{
		return false;
	}

	const auto value = operand_variable_to_str(inst.dst);
	if (value.type != SpirvType::Float)
	{
		return false;
	}

	BufferAddressSetup address_setup;
	if (!emit_buffer_address_setup(spirv, inst, static_cast<int>(index), 0, 4u, BufferAddressRangeCheck::RawZeroRecord,
	                               &address_setup) ||
	    address_setup.access_enabled.IsEmpty())
	{
		return false;
	}

	const auto tag        = String8::FromPrintf("%u_0", index);
	const auto scope      = spirv->GetConstantUint(SPIRV_SCOPE_DEVICE);
	const auto semantics  = spirv->GetConstantUint(SPIRV_DEVICE_MEMORY_ACQ_REL);
	if (scope == "unknown_uint_constant" || semantics == "unknown_uint_constant")
	{
		return false;
	}

	String8 return_value;
	if (inst.buffer_return_old_value)
	{
		return_value = String8(R"(
        %gen5_atomic_prior_f_<tag> = OpBitcast %float %gen5_atomic_prior_<tag>
               OpStore %<value> %gen5_atomic_prior_f_<tag>
)")
		                   .ReplaceStr("<tag>", tag)
		                   .ReplaceStr("<value>", value.value);
	}

	*dst_source += String8(R"(
<address_setup>
        %gen5_atomic_exec_<tag> = OpLoad %uint %exec_lo
        %gen5_atomic_active_<tag> = OpINotEqual %bool %gen5_atomic_exec_<tag> %uint_0
        %gen5_atomic_enabled_<tag> = OpLogicalAnd %bool %gen5_atomic_active_<tag> <access_enabled>
               OpSelectionMerge %gen5_atomic_merge_<tag> None
               OpBranchConditional %gen5_atomic_enabled_<tag> %gen5_atomic_then_<tag> %gen5_atomic_merge_<tag>
        %gen5_atomic_then_<tag> = OpLabel
		%gen5_atomic_word_<tag> = OpShiftRightLogical %uint <byte_offset> %uint_2
		%gen5_atomic_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %buf_uint <buffer_index> %int_0 %gen5_atomic_word_<tag>
        %gen5_atomic_value_f_<tag> = OpLoad %float %<value>
        %gen5_atomic_value_<tag> = OpBitcast %uint %gen5_atomic_value_f_<tag>
        %gen5_atomic_prior_<tag> = OpAtomicIAdd %uint %gen5_atomic_ptr_<tag> %<scope> %uint_0 %gen5_atomic_value_<tag>
               OpMemoryBarrier %<scope> %<semantics>
<return_value>               OpBranch %gen5_atomic_merge_<tag>
        %gen5_atomic_merge_<tag> = OpLabel
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<access_enabled>", address_setup.access_enabled)
	                   .ReplaceStr("<buffer_index>", address_setup.buffer_index)
	                   .ReplaceStr("<byte_offset>", address_setup.byte_offset)
	                   .ReplaceStr("<tag>", tag)
	                   .ReplaceStr("<scope>", scope)
	                   .ReplaceStr("<semantics>", semantics)
	                   .ReplaceStr("<value>", value.value)
	                   .ReplaceStr("<return_value>", return_value);
	return true;
}

/* Generalized Gen5 buffer atomic with a single data operand
 * (sub/smin/umin/smax/umax/and/or/xor). param[0] selects the SPIR-V atomic
 * opcode. Modeled on buffer_atomic_add. */
KYTY_RECOMPILER_FUNC(Recompile_BufferAtomic_XXX_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();
	if (!Config::IsNextGen() || bind_info == nullptr || bind_info->storage_buffers.buffers_num == 0)
	{
		return false;
	}

	const auto value = operand_variable_to_str(inst.dst);
	if (value.type != SpirvType::Float)
	{
		return false;
	}

	BufferAddressSetup address_setup;
	if (!emit_buffer_address_setup(spirv, inst, static_cast<int>(index), 0, 4u, BufferAddressRangeCheck::RawZeroRecord,
	                               &address_setup) ||
	    address_setup.access_enabled.IsEmpty())
	{
		return false;
	}

	const auto tag        = String8::FromPrintf("%u_0", index);
	const auto scope      = spirv->GetConstantUint(SPIRV_SCOPE_DEVICE);
	const auto semantics  = spirv->GetConstantUint(SPIRV_DEVICE_MEMORY_ACQ_REL);
	if (scope == "unknown_uint_constant" || semantics == "unknown_uint_constant")
	{
		return false;
	}

	String8 return_value;
	if (inst.buffer_return_old_value)
	{
		return_value = String8(R"(
        %gen5_atomic_prior_f_<tag> = OpBitcast %float %gen5_atomic_prior_<tag>
               OpStore %<value> %gen5_atomic_prior_f_<tag>
)")
		                   .ReplaceStr("<tag>", tag)
		                   .ReplaceStr("<value>", value.value);
	}

	*dst_source += String8(R"(
<address_setup>
        %gen5_atomic_exec_<tag> = OpLoad %uint %exec_lo
        %gen5_atomic_active_<tag> = OpINotEqual %bool %gen5_atomic_exec_<tag> %uint_0
        %gen5_atomic_enabled_<tag> = OpLogicalAnd %bool %gen5_atomic_active_<tag> <access_enabled>
               OpSelectionMerge %gen5_atomic_merge_<tag> None
               OpBranchConditional %gen5_atomic_enabled_<tag> %gen5_atomic_then_<tag> %gen5_atomic_merge_<tag>
        %gen5_atomic_then_<tag> = OpLabel
		%gen5_atomic_word_<tag> = OpShiftRightLogical %uint <byte_offset> %uint_2
		%gen5_atomic_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %buf_uint <buffer_index> %int_0 %gen5_atomic_word_<tag>
        %gen5_atomic_value_f_<tag> = OpLoad %float %<value>
        %gen5_atomic_value_<tag> = OpBitcast %uint %gen5_atomic_value_f_<tag>
        %gen5_atomic_prior_<tag> = <atomic_op> %uint %gen5_atomic_ptr_<tag> %<scope> %uint_0 %gen5_atomic_value_<tag>
               OpMemoryBarrier %<scope> %<semantics>
<return_value>               OpBranch %gen5_atomic_merge_<tag>
        %gen5_atomic_merge_<tag> = OpLabel
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<access_enabled>", address_setup.access_enabled)
	                   .ReplaceStr("<buffer_index>", address_setup.buffer_index)
	                   .ReplaceStr("<byte_offset>", address_setup.byte_offset)
	                   .ReplaceStr("<tag>", tag)
	                   .ReplaceStr("<scope>", scope)
	                   .ReplaceStr("<semantics>", semantics)
	                   .ReplaceStr("<value>", value.value)
	                   .ReplaceStr("<return_value>", return_value)
	                   .ReplaceStr("<atomic_op>", param[0]);
	return true;
}

static bool emit_gen5_tbuffer_load(Spirv* spirv, const ShaderInstruction& inst, int instruction_index,
	                                  const char* function_name, int format, uint32_t components, String8* dst_source)
{
	EXIT_IF(spirv == nullptr);
	EXIT_IF(dst_source == nullptr);

	const auto zero_float_id = spirv->GetConstantFloat(0.0f);
	if (zero_float_id == "unknown_float_constant")
	{
		return false;
	}
	String8 outputs;
	String8 zero_outputs;
	for (uint32_t component = 0; component < components; component++)
	{
		const auto dst = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (dst.type != SpirvType::Float)
		{
			return false;
		}
		outputs += String8::FromPrintf(" %%%s", dst.value.c_str());
		zero_outputs += String8::FromPrintf("               OpStore %%%s %%%s\n", dst.value.c_str(), zero_float_id.c_str());
	}

	BufferAddressSetup address_setup;
	uint32_t access_size = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(format));
	if (access_size == 0)
	{
		access_size = components * 4u;
	}
	if (!emit_buffer_address_setup(spirv, inst, instruction_index, 0, access_size, BufferAddressRangeCheck::None,
	                               &address_setup))
	{
		return false;
	}
	const auto format_id = spirv->GetConstantInt(format);
	if (format_id == "unknown_int_constant")
	{
		return false;
	}

	String8 body = String8(R"(
<address_setup>
               OpStore %temp_int_5 <format>
%gen5_tbuffer_load_<index> = OpFunctionCall %void %<function><outputs> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<format>", String8::FromPrintf("%%%s", format_id.c_str()))
	                   .ReplaceStr("<function>", function_name)
	                   .ReplaceStr("<outputs>", outputs)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", instruction_index));
	if (!address_setup.access_enabled.IsEmpty())
	{
		const auto tag = String8::FromPrintf("%u_0", instruction_index);
		body = String8(R"(
<address_setup>
               OpSelectionMerge %gen5_tbuffer_load_merge_<tag> None
               OpBranchConditional <access_enabled> %gen5_tbuffer_load_then_<tag> %gen5_tbuffer_load_oob_<tag>
        %gen5_tbuffer_load_then_<tag> = OpLabel
               OpStore %temp_int_5 <format>
%gen5_tbuffer_load_<tag> = OpFunctionCall %void %<function><outputs> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
               OpBranch %gen5_tbuffer_load_merge_<tag>
        %gen5_tbuffer_load_oob_<tag> = OpLabel
<zero_outputs>               OpBranch %gen5_tbuffer_load_merge_<tag>
        %gen5_tbuffer_load_merge_<tag> = OpLabel
)")
		           .ReplaceStr("<address_setup>", address_setup.source)
		           .ReplaceStr("<access_enabled>", address_setup.access_enabled)
		           .ReplaceStr("<format>", String8::FromPrintf("%%%s", format_id.c_str()))
		           .ReplaceStr("<function>", function_name)
		           .ReplaceStr("<outputs>", outputs)
		           .ReplaceStr("<zero_outputs>", zero_outputs)
		           .ReplaceStr("<tag>", tag);
	}
	*dst_source += body;
	return true;
}

static bool emit_gen5_mubuf_format_load(Spirv* spirv, const ShaderInstruction& inst, int instruction_index,
	                                      const char* function_name, uint32_t components, String8* dst_source)
{
	EXIT_IF(spirv == nullptr);
	EXIT_IF(dst_source == nullptr);

	const auto zero_float_id = spirv->GetConstantFloat(0.0f);
	if (zero_float_id == "unknown_float_constant")
	{
		return false;
	}
	String8 outputs;
	String8 zero_outputs;
	for (uint32_t component = 0; component < components; component++)
	{
		const auto dst = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (dst.type != SpirvType::Float)
		{
			return false;
		}
		outputs += String8::FromPrintf(" %%%s", dst.value.c_str());
		zero_outputs += String8::FromPrintf("               OpStore %%%s %%%s\n", dst.value.c_str(), zero_float_id.c_str());
	}

	BufferAddressSetup address_setup;
	if (!emit_buffer_address_setup(spirv, inst, instruction_index, 0, components * 4u, BufferAddressRangeCheck::None,
	                               &address_setup, true))
	{
		return false;
	}
	const auto tag = String8::FromPrintf("%u_0", instruction_index);

	String8 body = String8(R"(
<address_setup>
        %gen5_mubuf_format_u_<tag> = OpShiftRightLogical %uint %buf_addr_desc3_<tag> %uint_12
        %gen5_mubuf_format_m_<tag> = OpBitwiseAnd %uint %gen5_mubuf_format_u_<tag> %uint_127
        %gen5_mubuf_format_i_<tag> = OpBitcast %int %gen5_mubuf_format_m_<tag>
               OpStore %temp_int_5 %gen5_mubuf_format_i_<tag>
%gen5_mubuf_load_<tag> = OpFunctionCall %void %<function><outputs> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<tag>", tag)
	                   .ReplaceStr("<function>", function_name)
	                   .ReplaceStr("<outputs>", outputs);
	if (!address_setup.access_enabled.IsEmpty())
	{
		body = String8(R"(
<address_setup>
               OpSelectionMerge %gen5_mubuf_load_merge_<tag> None
               OpBranchConditional <access_enabled> %gen5_mubuf_load_then_<tag> %gen5_mubuf_load_oob_<tag>
        %gen5_mubuf_load_then_<tag> = OpLabel
        %gen5_mubuf_format_u_<tag> = OpShiftRightLogical %uint %buf_addr_desc3_<tag> %uint_12
        %gen5_mubuf_format_m_<tag> = OpBitwiseAnd %uint %gen5_mubuf_format_u_<tag> %uint_127
        %gen5_mubuf_format_i_<tag> = OpBitcast %int %gen5_mubuf_format_m_<tag>
               OpStore %temp_int_5 %gen5_mubuf_format_i_<tag>
%gen5_mubuf_load_<tag> = OpFunctionCall %void %<function><outputs> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
               OpBranch %gen5_mubuf_load_merge_<tag>
        %gen5_mubuf_load_oob_<tag> = OpLabel
<zero_outputs>               OpBranch %gen5_mubuf_load_merge_<tag>
        %gen5_mubuf_load_merge_<tag> = OpLabel
)")
		           .ReplaceStr("<address_setup>", address_setup.source)
		           .ReplaceStr("<access_enabled>", address_setup.access_enabled)
		           .ReplaceStr("<tag>", tag)
		           .ReplaceStr("<function>", function_name)
		           .ReplaceStr("<outputs>", outputs)
		           .ReplaceStr("<zero_outputs>", zero_outputs);
	}
	*dst_source += body;
	return true;
}

static bool emit_gen5_mubuf_format_store(Spirv* spirv, const ShaderInstruction& inst, int instruction_index,
	                                       const char* function_name, uint32_t components, String8* dst_source)
{
	EXIT_IF(spirv == nullptr);
	EXIT_IF(dst_source == nullptr);

	String8 inputs;
	for (uint32_t component = 0; component < components; component++)
	{
		const auto value = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (value.type != SpirvType::Float)
		{
			return false;
		}
		inputs += String8::FromPrintf(" %%%s", value.value.c_str());
	}

	BufferAddressSetup address_setup;
	if (!emit_buffer_address_setup(spirv, inst, instruction_index, 0, components * 4u, BufferAddressRangeCheck::None,
	                               &address_setup))
	{
		return false;
	}
	const auto tag = String8::FromPrintf("%u_0", instruction_index);
	String8    enable_source;
	String8    enable_condition = String8::FromPrintf("%%gen5_mubuf_store_active_%s", tag.c_str());
	if (!address_setup.access_enabled.IsEmpty())
	{
		enable_source = String8("        %gen5_mubuf_store_enabled_<tag> = OpLogicalAnd %bool %gen5_mubuf_store_active_<tag> <access_enabled>\n")
		                    .ReplaceStr("<tag>", tag)
		                    .ReplaceStr("<access_enabled>", address_setup.access_enabled);
		enable_condition = String8::FromPrintf("%%gen5_mubuf_store_enabled_%s", tag.c_str());
	}

	*dst_source += String8(R"(
<address_setup>
        %gen5_mubuf_store_exec_<tag> = OpLoad %uint %exec_lo
        %gen5_mubuf_store_active_<tag> = OpINotEqual %bool %gen5_mubuf_store_exec_<tag> %uint_0
<enable_source>
               OpSelectionMerge %gen5_mubuf_store_merge_<tag> None
               OpBranchConditional <enable_condition> %gen5_mubuf_store_then_<tag> %gen5_mubuf_store_merge_<tag>
        %gen5_mubuf_store_then_<tag> = OpLabel
        %gen5_mubuf_store_format_u_<tag> = OpShiftRightLogical %uint %buf_addr_desc3_<tag> %uint_12
        %gen5_mubuf_store_format_m_<tag> = OpBitwiseAnd %uint %gen5_mubuf_store_format_u_<tag> %uint_127
        %gen5_mubuf_store_format_i_<tag> = OpBitcast %int %gen5_mubuf_store_format_m_<tag>
               OpStore %temp_int_5 %gen5_mubuf_store_format_i_<tag>
%gen5_mubuf_store_<tag> = OpFunctionCall %void %<function><inputs> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
               OpBranch %gen5_mubuf_store_merge_<tag>
        %gen5_mubuf_store_merge_<tag> = OpLabel
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<enable_source>", enable_source)
	                   .ReplaceStr("<enable_condition>", enable_condition)
	                   .ReplaceStr("<tag>", tag)
	                   .ReplaceStr("<function>", function_name)
	                   .ReplaceStr("<inputs>", inputs);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferLoadUbyte_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			const auto dst = operand_variable_to_str(inst.dst);
			if (dst.type != SpirvType::Float)
			{
				return false;
			}
			const auto zero_float_id = spirv->GetConstantFloat(0.0f);
			if (zero_float_id == "unknown_float_constant")
			{
				return false;
			}
			const auto zero_float = String8::FromPrintf("%%%s", zero_float_id.c_str());
			BufferAddressSetup address_setup;
			if (!emit_buffer_address_setup(spirv, inst, static_cast<int>(index), 0, 1u, BufferAddressRangeCheck::RawZeroRecord,
			                               &address_setup) ||
			    address_setup.access_enabled.IsEmpty())
			{
				return false;
			}
			const auto tag = String8::FromPrintf("%u_0", index);
			*dst_source += String8(R"(
<address_setup>
               OpSelectionMerge %gen5_raw_ubyte_merge_<tag> None
               OpBranchConditional <access_enabled> %gen5_raw_ubyte_then_<tag> %gen5_raw_ubyte_oob_<tag>
        %gen5_raw_ubyte_then_<tag> = OpLabel
%gen5_raw_ubyte_<tag> = OpFunctionCall %void %buffer_load_ubyte %temp_uint_0 %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
%gen5_raw_ubyte_value_<tag> = OpLoad %uint %temp_uint_0
%gen5_raw_ubyte_float_<tag> = OpBitcast %float %gen5_raw_ubyte_value_<tag>
               OpStore %<dst> %gen5_raw_ubyte_float_<tag>
               OpBranch %gen5_raw_ubyte_merge_<tag>
        %gen5_raw_ubyte_oob_<tag> = OpLabel
               OpStore %<dst> <zero>
               OpBranch %gen5_raw_ubyte_merge_<tag>
        %gen5_raw_ubyte_merge_<tag> = OpLabel
)")
			                   .ReplaceStr("<address_setup>", address_setup.source)
			                   .ReplaceStr("<access_enabled>", address_setup.access_enabled)
			                   .ReplaceStr("<tag>", tag)
			                   .ReplaceStr("<zero>", zero_float)
			                   .ReplaceStr("<dst>", dst.value);
			return true;
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_load_ubyte %temp_uint_0 %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t111_<index> = OpLoad %uint %temp_uint_0
        %t112_<index> = OpBitcast %float %t111_<index>
               OpStore %<dst> %t112_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<dst>", dst_value.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferLoadDword)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (Config::IsNextGen() && !buffer_resource_is_bound(spirv, inst.src[1]) && spirv->UsesGuestDeviceAddress())
	{
		return emit_guest_buffer_load(spirv, inst, static_cast<int>(index), 1, dst_source);
	}
	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_raw_buffer_load(spirv, inst, static_cast<int>(index), 1, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		const bool has_vgpr_offset = inst.format == ShaderInstructionFormat::Vdata1Vaddr2SvSoffsOffenIdxen;
		if (!has_vgpr_offset && inst.format != ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !has_vgpr_offset && inst.format != ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen condition ignored (continuing)\n"); }

		auto dst_value   = operand_variable_to_str(inst.dst);
		auto src0_index  = buffer_index_variable_to_str(inst);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value1 = operand_variable_to_str(inst.src[1], 1);
		// auto   src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset    = GetBufferOffsetIntConstant(spirv, inst.src[2]);
		String8 index_str = String8::FromPrintf("%u", index);

		if (dst_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_index.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_index.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }
		// EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		String8 load_offset = R"(
               OpStore %temp_int_2 %<offset>
)";
		if (has_vgpr_offset)
		{
			auto src0_offset = operand_variable_to_str(inst.src[0], 0);
			if (src0_offset.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_offset.type != SpirvType::Float condition ignored (continuing)\n"); }
			load_offset = R"(
       %to100_<index> = OpLoad %float %<src0_offset>
       %to101_<index> = OpBitcast %int %to100_<index>
       %to102_<index> = OpIAdd %int %to101_<index> %<offset>
               OpStore %temp_int_2 %to102_<index>
)";
			load_offset = load_offset.ReplaceStr("<src0_offset>", src0_offset.value);
		}
		load_offset = load_offset.ReplaceStr("<offset>", offset).ReplaceStr("<index>", index_str);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0_index>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
        <load_offset>
		;%t206_<index> = OpLoad %uint %<src1_value3>
        ;%t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        ;%t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        ;%t211_<index> = OpBitcast %int %t210_<index>
        ;       OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %buffer_load_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", index_str)
		                   .ReplaceStr("<src0_index>", src0_index.value)
		                   .ReplaceStr("<load_offset>", load_offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   //.ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

// buffer_load_dwordx2: two raw dwords at consecutive addresses (offset, offset+4).
KYTY_RECOMPILER_FUNC(Recompile_BufferLoadDwordx2_Vdata2VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (Config::IsNextGen() && !buffer_resource_is_bound(spirv, inst.src[1]) && spirv->UsesGuestDeviceAddress())
	{
		return emit_guest_buffer_load(spirv, inst, static_cast<int>(index), 2, dst_source);
	}
	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_raw_buffer_load(spirv, inst, static_cast<int>(index), 2, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (dst_value1.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value1.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_load_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t200_<index> = OpLoad %int %temp_int_2
        %t201_<index> = OpIAdd %int %t200_<index> %int_4
               OpStore %temp_int_2 %t201_<index>
        %t210_<index> = OpFunctionCall %void %buffer_load_float1 %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value);

		return true;
	}

	return false;
}

// buffer_load_dwordx4: four raw dwords via existing buffer_load_float4 helper.
KYTY_RECOMPILER_FUNC(Recompile_BufferLoadDwordx4_Vdata4VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (Config::IsNextGen() && !buffer_resource_is_bound(spirv, inst.src[1]) && spirv->UsesGuestDeviceAddress())
	{
		return emit_guest_buffer_load(spirv, inst, static_cast<int>(index), 4, dst_source);
	}
	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_raw_buffer_load(spirv, inst, static_cast<int>(index), 4, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_load_float4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

// buffer_load_dwordx3: three consecutive raw dwords via three float1 loads.
KYTY_RECOMPILER_FUNC(Recompile_BufferLoadDwordx3_Vdata3VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (Config::IsNextGen() && !buffer_resource_is_bound(spirv, inst.src[1]) && spirv->UsesGuestDeviceAddress())
	{
		return emit_guest_buffer_load(spirv, inst, static_cast<int>(index), 3, dst_source);
	}
	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_raw_buffer_load(spirv, inst, static_cast<int>(index), 3, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_load_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t200_<index> = OpLoad %int %temp_int_2
        %t201_<index> = OpIAdd %int %t200_<index> %int_4
               OpStore %temp_int_2 %t201_<index>
        %t210_<index> = OpFunctionCall %void %buffer_load_float1 %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t220_<index> = OpLoad %int %temp_int_2
        %t221_<index> = OpIAdd %int %t220_<index> %int_4
               OpStore %temp_int_2 %t221_<index>
        %t230_<index> = OpFunctionCall %void %buffer_load_float1 %<p2> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferLoadFormatX_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_mubuf_format_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_x", 1, dst_source);
		}

		// EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
		// EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value3.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value3.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_x %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferLoadFormatXy_Vdata2VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr || bind_info->storage_buffers.buffers_num <= 0 || !Config::IsNextGen())
	{
		return false;
	}
	return emit_gen5_mubuf_format_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_xy", 2, dst_source);
}

// buffer_load_format_xyz: Gen5 unified format 74 is R32G32B32_SFLOAT. The
// xyzw helper rejects 74 (it only allows 75-77 / 119), so RGB32F vertex
// streams that are not remapped to Fetch must use this 3-component path.
KYTY_RECOMPILER_FUNC(Recompile_BufferLoadFormatXyz_Vdata3VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr || bind_info->storage_buffers.buffers_num <= 0 || !Config::IsNextGen())
	{
		return false;
	}
	return emit_gen5_mubuf_format_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_xyz", 3, dst_source);
}

// buffer_load_format_xyzw v[0:3], v4, s[0:3], 0, idxen — same addressing as
// BufferLoadFormatX but four-component destination (captured post-menu load).
KYTY_RECOMPILER_FUNC(Recompile_BufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_mubuf_format_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_xyzw", 4, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value3.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value3.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferStoreDword_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_raw_buffer_store(spirv, inst, static_cast<int>(index), 1, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		const bool has_vgpr_offset = inst.format == ShaderInstructionFormat::Vdata1VaddrSvSoffsOffen;
		if (!has_vgpr_offset && inst.format != ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !has_vgpr_offset && inst.format != ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen condition ignored (continuing)\n"); }

		auto dst_value   = operand_variable_to_str(inst.dst);
		auto src0_value  = operand_variable_to_str(inst.src[0]);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value1 = operand_variable_to_str(inst.src[1], 1);
		// auto   src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }
		// EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP

		String8 address_setup = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
               OpStore %temp_int_2 %<offset>
)";
		if (has_vgpr_offset)
		{
			address_setup = R"(
               OpStore %temp_int_1 %int_0
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
        %t102_<index> = OpIAdd %int %t101_<index> %<offset>
               OpStore %temp_int_2 %t102_<index>
)";
		}

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel

        <address_setup>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
		;%t206_<index> = OpLoad %uint %<src1_value3>
        ;%t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        ;%t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        ;%t211_<index> = OpBitcast %int %t210_<index>
        ;       OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %buffer_store_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4

               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<address_setup>", address_setup)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   //.ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

// buffer_store_dwordx2: two raw dwords via buffer_store_float2.
KYTY_RECOMPILER_FUNC(Recompile_BufferStoreDwordx2_Vdata2VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_raw_buffer_store(spirv, inst, static_cast<int>(index), 2, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_store_float2 %<p0> %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value);

		return true;
	}

	return false;
}

// buffer_store_dwordx4: four raw dwords via buffer_store_float4.
KYTY_RECOMPILER_FUNC(Recompile_BufferStoreDwordx4_Vdata4VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_raw_buffer_store(spirv, inst, static_cast<int>(index), 4, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_store_float4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

// buffer_store_dwordx3: three consecutive raw dwords via three float1 stores.
KYTY_RECOMPILER_FUNC(Recompile_BufferStoreDwordx3_Vdata3VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_raw_buffer_store(spirv, inst, static_cast<int>(index), 3, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_store_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t200_<index> = OpLoad %int %temp_int_2
        %t201_<index> = OpIAdd %int %t200_<index> %int_4
               OpStore %temp_int_2 %t201_<index>
        %t210_<index> = OpFunctionCall %void %buffer_store_float1 %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t220_<index> = OpLoad %int %temp_int_2
        %t221_<index> = OpIAdd %int %t220_<index> %int_4
               OpStore %temp_int_2 %t221_<index>
        %t230_<index> = OpFunctionCall %void %buffer_store_float1 %<p2> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferStoreFormatX_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_mubuf_format_store(spirv, inst, static_cast<int>(index), "tbuffer_store_format_x", 1, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value3.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value3.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel

        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_store_format_x %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5

               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferStoreFormatXy_Vdata2VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_mubuf_format_store(spirv, inst, static_cast<int>(index), "tbuffer_store_format_xy", 2, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value3.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value3.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel

        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_store_format_xy %<p0> %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5

               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferStoreFormatXyzw_Vdata4VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!ShaderInstructionLoweringPreconditions(inst)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_mubuf_format_store(spirv, inst, static_cast<int>(index), "tbuffer_store_format_xyzw", 4, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (dst_value1.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value1.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (dst_value2.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value2.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (dst_value3.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value3.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value3.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value3.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel

        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_store_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5

               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_DsAppend_VdstGds)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->gds_pointers.pointers_num > 0)
	{
		String8 index_str = String8::FromPrintf("%u", index);

		if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

		auto dst_value = operand_variable_to_str(inst.dst);

		if (dst_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Float condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t198_<index> = OpAtomicIAdd %uint %gds_counter_<index>_ptr %uint_1 %uint_0 %uint_1
        %t199_<index> = OpBitcast %float %t198_<index>
               OpStore %<dst> %t199_<index>
               OpMemoryBarrier %uint_1 %uint_72
)";
		*dst_source += spirv->EmitGdsCounterPointer(inst.ds_offset, "gds_counter_" + index_str);
		*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_DsConsume_VdstGds)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->gds_pointers.pointers_num > 0)
	{
		String8 index_str = String8::FromPrintf("%u", index);

		if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

		auto dst_value = operand_variable_to_str(inst.dst);

		if (dst_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Float condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t198_<index> = OpAtomicISub %uint %gds_counter_<index>_ptr %uint_1 %uint_0 %uint_1
        %t199_<index> = OpBitcast %float %t198_<index>
               OpStore %<dst> %t199_<index>
               OpMemoryBarrier %uint_1 %uint_72
)";
		*dst_source += spirv->EmitGdsCounterPointer(inst.ds_offset, "gds_counter_" + index_str);
		*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

namespace {

String8 LdsAddressSnapshot(const String8& address, const String8& tag)
{
	return String8(R"(
%lds_addr_f_<tag> = OpLoad %float %<address>
%lds_addr_u_<tag> = OpBitcast %uint %lds_addr_f_<tag>
)").ReplaceStr("<address>", address).ReplaceStr("<tag>", tag);
}

// Return zero on the out-of-range arm, with both pointer and load dominated by
// the valid-span branch. Explicit else/merge blocks also prevent generic bank
// if-conversion from speculating a Workgroup load.
String8 LdsReadSpan(Spirv* spirv, const ShaderOperand& destination, int first, int count, const String8& tag)
{
	String8 loads;
	String8 results;
	for (int word = 0; word < count; ++word)
	{
		const auto id = String8::FromPrintf("%s_%d", tag.c_str(), word);
		loads += EmitLdsWordPointer(spirv, word, tag);
		loads += String8::FromPrintf("%%lds_data_u_%s = OpLoad %%uint %%lds_ptr_%s\n", id.c_str(), id.c_str());
		results += String8::FromPrintf(
		    "%%lds_result_%s = OpPhi %%uint %%lds_data_u_%s %%lds_read_then_%s %%uint_0 %%lds_read_else_%s\n",
		    id.c_str(), id.c_str(), tag.c_str(), tag.c_str());
	}
	// All Phi instructions must precede the ordinary instructions in the merge.
	for (int word = 0; word < count; ++word)
	{
		const auto id = String8::FromPrintf("%s_%d", tag.c_str(), word);
		const auto dst = operand_variable_to_str(destination, first + word);
		results += String8::FromPrintf("%%lds_data_f_%s = OpBitcast %%float %%lds_result_%s\nOpStore %%%s %%lds_data_f_%s\n",
		                               id.c_str(), id.c_str(), dst.value.c_str(), id.c_str());
	}
	return String8(R"(
               OpSelectionMerge %lds_read_merge_<tag> None
               OpBranchConditional %lds_valid_<tag> %lds_read_then_<tag> %lds_read_else_<tag>
%lds_read_then_<tag> = OpLabel
<loads>
               OpBranch %lds_read_merge_<tag>
%lds_read_else_<tag> = OpLabel
               OpBranch %lds_read_merge_<tag>
%lds_read_merge_<tag> = OpLabel
<results>
)").ReplaceStr("<tag>", tag).ReplaceStr("<loads>", loads).ReplaceStr("<results>", results);
}

} // namespace

KYTY_RECOMPILER_FUNC(Recompile_DsWriteB32_VaddrVdataOffset)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();
	const int   dwords     = inst.src[1].size;
	if (input_info == nullptr || input_info->lds_dwords == 0 || !ShaderLdsMemoryInstructionSupported(inst))
	{
		return false;
	}

	const auto address   = operand_variable_to_str(inst.src[0]);
	const auto index_str = String8::FromPrintf("%u", index);
	String8    body;
	for (int word = 0; word < dwords; word++)
	{
		const auto data = dwords == 1 ? operand_variable_to_str(inst.src[1]) : operand_variable_to_str(inst.src[1], word);
		const auto id   = String8::FromPrintf("%s_%d", index_str.c_str(), word);
		body += EmitLdsWordPointer(spirv, word, index_str);
		body += String8::FromPrintf("        %%lds_data_f_%s = OpLoad %%float %%%s\n"
		                            "        %%lds_data_u_%s = OpBitcast %%uint %%lds_data_f_%s\n"
		                            "               OpStore %%lds_ptr_%s %%lds_data_u_%s\n",
		                            id.c_str(), data.value.c_str(), id.c_str(), id.c_str(), id.c_str(), id.c_str());
	}
	const auto checked = LdsAddressSnapshot(address.value, index_str) +
	                     EmitLdsAddressCheck(spirv, "lds_addr_u_" + index_str, inst.ds_offset, dwords, index_str) +
	                     GateLdsByBounds(body, index_str);
	*dst_source += GateLdsByExec(checked, index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_DsAddU32_VaddrVdataOffset)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0 || !ShaderLdsMemoryInstructionSupported(inst))
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);
	auto data    = operand_variable_to_str(inst.src[1]);

	const auto index_str  = String8::FromPrintf("%u", index);
	const auto scope_str  = spirv->GetConstantUint(2u);
	const auto semantics  = spirv->GetConstantUint(0x108u);

	static const char* text = R"(
        %lds_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
        %lds_data_f_<index> = OpLoad %float %<data>
        %lds_data_u_<index> = OpBitcast %uint %lds_data_f_<index>
        %lds_prior_<index> = OpAtomicIAdd %uint %lds_ptr_<index> %<scope> %<semantics> %lds_data_u_<index>
)";
	const auto body = String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<data>", data.value)
	                   .ReplaceStr("<scope>", scope_str)
	                   .ReplaceStr("<semantics>", semantics);
	*dst_source += GateLdsByExec(LdsAddressSnapshot(address.value, index_str) +
	                           EmitLdsAddressCheck(spirv, "lds_addr_u_" + index_str, inst.ds_offset, 1, index_str) +
	                           GateLdsByBounds(body, index_str), index_str);
	return true;
}

/* Generalized LDS atomic with a data operand (sub/min/max/and/or/xor).
 * param[0] selects the SPIR-V atomic opcode. */
KYTY_RECOMPILER_FUNC(Recompile_DsAtomic_XXX_VaddrVdataOffset)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0 || !ShaderLdsMemoryInstructionSupported(inst) ||
	    param == nullptr || param[0] == nullptr)
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);
	auto data    = operand_variable_to_str(inst.src[1]);

	const auto index_str  = String8::FromPrintf("%u", index);
	const auto scope_str  = spirv->GetConstantUint(2u);
	const auto semantics  = spirv->GetConstantUint(0x108u);

	static const char* text = R"(
        %lds_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
        %lds_data_f_<index> = OpLoad %float %<data>
        %lds_data_u_<index> = OpBitcast %uint %lds_data_f_<index>
        %lds_prior_<index> = <atomic_op> %uint %lds_ptr_<index> %<scope> %<semantics> %lds_data_u_<index>
)";
	const auto body = String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<data>", data.value)
	                   .ReplaceStr("<scope>", scope_str)
	                   .ReplaceStr("<semantics>", semantics)
	                   .ReplaceStr("<atomic_op>", param[0]);
	*dst_source += GateLdsByExec(LdsAddressSnapshot(address.value, index_str) +
	                           EmitLdsAddressCheck(spirv, "lds_addr_u_" + index_str, inst.ds_offset, 1, index_str) +
	                           GateLdsByBounds(body, index_str), index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_DsAtomicIncDec_VaddrOffset)
{
	// ISA DS_INC/DEC wrap at DATA0's unsigned limit; the VaddrOffset IR
	// discarded that source. Refuse this lossy tuple rather than substitute
	// SPIR-V's unconditional increment/decrement. A correct lowering needs the
	// data operand retained by the parser and a compare/exchange loop.
	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_DsReadB32_VdstVaddrOffset)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();
	const int   dwords     = inst.dst.size;
	if (input_info == nullptr || input_info->lds_dwords == 0 || !ShaderLdsMemoryInstructionSupported(inst))
	{
		return false;
	}

	const auto address   = operand_variable_to_str(inst.src[0]);
	const auto index_str = String8::FromPrintf("%u", index);
	const auto body = LdsAddressSnapshot(address.value, index_str) +
	                  EmitLdsAddressCheck(spirv, "lds_addr_u_" + index_str, inst.ds_offset, dwords, index_str) +
	                  LdsReadSpan(spirv, inst.dst, 0, dwords, index_str);
	*dst_source += GateLdsByExec(body, index_str);
	return true;
}

// ds_read2[st64]_b32 / ds_write2[st64]_b32 carry two independent dword offsets, each scaled by
// 4 bytes (times 64 for the st64 forms, param[0] == "64").
static uint32_t Ds2OffsetBytes(const ShaderInstruction& inst, uint32_t word, const char* const* param)
{
	const uint32_t stride = (param != nullptr && param[0] != nullptr && param[0][0] == '6') ? 64u : 1u;
	return ((inst.ds_offset >> (word * 8u)) & 0xffu) * 4u * stride;
}

KYTY_RECOMPILER_FUNC(Recompile_DsRead2B32_Vdst2VaddrOffset01)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0 || !ShaderLdsMemoryInstructionSupported(inst))
	{
		return false;
	}

	const auto address = operand_variable_to_str(inst.src[0]);
	const auto index_str = String8::FromPrintf("%u", index);
	String8 body = LdsAddressSnapshot(address.value, index_str);
	for (int word = 0; word < 2; ++word)
	{
		// These are independent addresses, not a contiguous two-dword span.
		const auto tag = String8::FromPrintf("%u_read2_%d", index, word);
		body += EmitLdsAddressCheck(spirv, "lds_addr_u_" + index_str, Ds2OffsetBytes(inst, static_cast<uint32_t>(word), param), 1, tag);
		body += LdsReadSpan(spirv, inst.dst, word, 1, tag);
	}
	*dst_source += GateLdsByExec(body, index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_DsWrite2B32_VaddrVdata2Offset01)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0 || !ShaderLdsMemoryInstructionSupported(inst))
	{
		return false;
	}

	const auto address   = operand_variable_to_str(inst.src[0]);
	const auto index_str = String8::FromPrintf("%u", index);
	String8    body      = LdsAddressSnapshot(address.value, index_str);
	// Both data words are read before either store; the two stores target independent addresses.
	for (int word = 0; word < 2; ++word)
	{
		const auto tag  = String8::FromPrintf("%u_write2_%d", index, word);
		const auto data = operand_variable_to_str(inst.src[1 + word]);
		String8    store = EmitLdsWordPointer(spirv, 0, tag);
		store += String8::FromPrintf("        %%lds_data_f_%s = OpLoad %%float %%%s\n"
		                             "        %%lds_data_u_%s = OpBitcast %%uint %%lds_data_f_%s\n"
		                             "               OpStore %%lds_ptr_%s_0 %%lds_data_u_%s\n",
		                             tag.c_str(), data.value.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str());
		body += EmitLdsAddressCheck(spirv, "lds_addr_u_" + index_str, Ds2OffsetBytes(inst, static_cast<uint32_t>(word), param), 1, tag);
		body += GateLdsByBounds(store, tag);
	}
	*dst_source += GateLdsByExec(body, index_str);
	return true;
}

namespace {

// Byte address for ds_*_addtid_b32: M0[15:0] + ds_offset (+ TID*4 on the
// shared-LDS compute path, where `tid` is the lane index inside the wave).
// `active` names a %bool lane-execution predicate; the caller computes it
// (EXEC load, or a banked mask bit). Produces %addtid_idx_<tag> (word index),
// %addtid_inb_<tag> (bounds check against `bound` dwords) and
// %addtid_gate_<tag> (in-bounds AND exec-active).
String8 DsAddtidIndex(Spirv* spirv, uint16_t offset, const String8& bound, const String8& tid, const String8& active,
                      const String8& tag)
{
	String8 text = String8::FromPrintf(
	    "         %%addtid_m0_%s = OpLoad %%uint %%m0\n"
	    "       %%addtid_base_%s = OpBitwiseAnd %%uint %%addtid_m0_%s %%%s\n",
	    tag.c_str(), tag.c_str(), tag.c_str(), spirv->GetConstantUint(0xffffu).c_str());
	if (!tid.IsEmpty())
	{
		text += String8::FromPrintf(
		    "       %%addtid_tidw_%s = OpShiftLeftLogical %%uint %%%s %%uint_2\n"
		    "      %%addtid_bytet_%s = OpIAdd %%uint %%addtid_base_%s %%addtid_tidw_%s\n",
		    tag.c_str(), tid.c_str(), tag.c_str(), tag.c_str(), tag.c_str());
	}
	text += String8::FromPrintf(
	    "       %%addtid_byte_%s = OpIAdd %%uint %%addtid_%s_%s %%%s\n"
	    "        %%addtid_idx_%s = OpShiftRightLogical %%uint %%addtid_byte_%s %%uint_2\n"
	    "        %%addtid_inb_%s = OpULessThan %%bool %%addtid_idx_%s %%%s\n"
	    "      %%addtid_gate_%s = OpLogicalAnd %%bool %%%s %%addtid_inb_%s\n",
	    tag.c_str(), tid.IsEmpty() ? "base" : "bytet", tag.c_str(), spirv->GetConstantUint(offset).c_str(), tag.c_str(),
	    tag.c_str(), tag.c_str(), tag.c_str(), bound.c_str(), tag.c_str(), active.c_str(), tag.c_str());
	return text;
}

// Lane-execution predicate for the shared-LDS path: a plain EXEC word test in
// virtualized mode, or the bank's mask bit under paired compute waves.
bool EmitDsAddtidActive(const Spirv* spirv, ShaderWaveBank bank, const String8& tag, String8* dst_source,
                        String8* active_id)
{
	if (spirv->UsesComputeWaveBanks())
	{
		ShaderOperand exec {};
		exec.type = ShaderOperandType::ExecLo;
		exec.size = 2;
		*active_id = "addtid_exec_" + tag;
		return spirv->EmitComputeWaveMaskBit(exec, bank, *active_id, dst_source);
	}
	*active_id = "addtid_active_" + tag;
	*dst_source += String8::FromPrintf(
	    "       %%addtid_exec_%s = OpLoad %%uint %%exec_lo\n"
	    "    %%addtid_active_%s = OpINotEqual %%bool %%addtid_exec_%s %%uint_0\n",
	    tag.c_str(), tag.c_str(), tag.c_str());
	return true;
}

} // namespace

// ds_write_addtid_b32 / ds_read_addtid_b32: a per-lane LDS slot at
// M0[15:0] + ds_offset + TID*4 (RDNA2 ISA 10.4). Compute with an LDS allocation
// lowers it onto shared %lds. The existing zero-LDS/graphics route retains its
// function-local spill array and capacity; these are lowering policy, not proof
// of an architectural LDS allocation or general cross-lane spill equivalence.
static bool EmitDsAddtidLdsStore(const SpirvValue& data, const String8& active, const String8& tag,
                                 const String8& bound, const String8& tid, Spirv* spirv, const ShaderInstruction& inst,
                                 String8* dst_source)
{
	*dst_source += DsAddtidIndex(spirv, inst.ds_offset, bound, tid, active, tag);
	*dst_source += String8::FromPrintf(
	    "               OpSelectionMerge %%addtid_merge_%s None\n"
	    "               OpBranchConditional %%addtid_gate_%s %%addtid_then_%s %%addtid_merge_%s\n"
	    "        %%addtid_then_%s = OpLabel\n"
	    "         %%addtid_ptr_%s = OpAccessChain %%_ptr_Workgroup_uint %%lds %%addtid_idx_%s\n"
	    "        %%addtid_dataf_%s = OpLoad %%float %%%s\n"
	    "         %%addtid_data_%s = OpBitcast %%uint %%addtid_dataf_%s\n"
	    "               OpStore %%addtid_ptr_%s %%addtid_data_%s\n"
	    "               OpBranch %%addtid_merge_%s\n"
	    "       %%addtid_merge_%s = OpLabel\n",
	    tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(),
	    data.value.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str());
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_DsWriteAddtidB32_VdataOffset)
{
	const auto& inst = code.GetInstructions().At(index);
	if (!spirv->UsesDsAddtid())
	{
		return false;
	}
	const auto index_str = String8::FromPrintf("%u", index);

	if (spirv->UsesDsAddtidLds())
	{
		const auto bound = spirv->GetConstantUint(spirv->GetCsInputInfo()->lds_dwords);
		if (bound == "unknown_uint_constant")
		{
			return false;
		}
		if (spirv->UsesComputeWaveBanks())
		{
			for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
			{
				const auto data   = spirv->GetComputeWaveRegister(inst.src[0], bank, 0);
				const auto bname  = bank == ShaderWaveBank::Low ? "low" : "high";
				if (data.value.IsEmpty())
				{
					return false;
				}
				const auto tag = String8::FromPrintf("%u_%s", index, bname);
				String8    active;
				if (!EmitDsAddtidActive(spirv, bank, tag, dst_source, &active))
				{
					return false;
				}
				*dst_source += String8::FromPrintf(
				    "        %%addtid_tid_%s = OpBitwiseAnd %%uint %%wave_logical_%s %%uint_63\n", tag.c_str(), bname);
				if (!EmitDsAddtidLdsStore(data, active, tag, bound, "addtid_tid_" + tag, spirv, inst, dst_source))
				{
					return false;
				}
			}
			return true;
		}
		String8 active;
		if (!EmitDsAddtidActive(spirv, ShaderWaveBank::Low, index_str, dst_source, &active))
		{
			return false;
		}
		*dst_source += String8::FromPrintf("        %%addtid_tidl_%s = OpLoad %%uint %%gl_LocalInvocationIndex\n"
		                                   "         %%addtid_tid_%s = OpBitwiseAnd %%uint %%addtid_tidl_%s %%uint_63\n",
		                                   index_str.c_str(), index_str.c_str(), index_str.c_str());
		const auto data = operand_variable_to_str(inst.src[0]);
		if (data.value.IsEmpty())
		{
			return false;
		}
		return EmitDsAddtidLdsStore(data, active, index_str, bound, "addtid_tid_" + index_str, spirv, inst, dst_source);
	}

	// Private spill array: compute without an LDS allocation or any graphics
	// stage. Under paired compute waves each invocation still emulates two
	// lanes, so the bank loop and TID*4 term are kept — only the storage is
	// per-invocation instead of the shared %lds array.
	const auto bound = spirv->GetConstantUint(kDsAddtidSpillDwords);
	if (bound == "unknown_uint_constant")
	{
		return false;
	}
	if (spirv->UsesComputeWaveBanks())
	{
		for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
		{
			const auto data  = spirv->GetComputeWaveRegister(inst.src[0], bank, 0);
			const auto bname = bank == ShaderWaveBank::Low ? "low" : "high";
			if (data.value.IsEmpty())
			{
				return false;
			}
			const auto tag = String8::FromPrintf("%u_%s", index, bname);
			String8    active;
			if (!EmitDsAddtidActive(spirv, bank, tag, dst_source, &active))
			{
				return false;
			}
			*dst_source += String8::FromPrintf(
			    "        %%addtid_tid_%s = OpBitwiseAnd %%uint %%wave_logical_%s %%uint_63\n", tag.c_str(), bname);
			*dst_source += DsAddtidIndex(spirv, inst.ds_offset, bound, "addtid_tid_" + tag, active, tag);
			*dst_source += String8::FromPrintf(
			    "               OpSelectionMerge %%addtid_merge_%s None\n"
			    "               OpBranchConditional %%addtid_gate_%s %%addtid_then_%s %%addtid_merge_%s\n"
			    "        %%addtid_then_%s = OpLabel\n"
			    "         %%addtid_ptr_%s = OpAccessChain %%_ptr_Function_float %%lds_addtid %%addtid_idx_%s\n"
			    "        %%addtid_dataf_%s = OpLoad %%float %%%s\n"
			    "               OpStore %%addtid_ptr_%s %%addtid_dataf_%s\n"
			    "               OpBranch %%addtid_merge_%s\n"
			    "       %%addtid_merge_%s = OpLabel\n",
			    tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(),
			    data.value.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str());
		}
		return true;
	}
	const auto data = operand_variable_to_str(inst.src[0]);
	if (data.value.IsEmpty())
	{
		return false;
	}
	String8 active;
	if (!EmitDsAddtidActive(spirv, ShaderWaveBank::Low, index_str, dst_source, &active))
	{
		return false;
	}
	*dst_source += DsAddtidIndex(spirv, inst.ds_offset, bound, String8(), active, index_str);
	*dst_source += String8::FromPrintf(
	    "               OpSelectionMerge %%addtid_merge_%s None\n"
	    "               OpBranchConditional %%addtid_gate_%s %%addtid_then_%s %%addtid_merge_%s\n"
	    "        %%addtid_then_%s = OpLabel\n"
	    "         %%addtid_ptr_%s = OpAccessChain %%_ptr_Function_float %%lds_addtid %%addtid_idx_%s\n"
	    "        %%addtid_dataf_%s = OpLoad %%float %%%s\n"
	    "               OpStore %%addtid_ptr_%s %%addtid_dataf_%s\n"
	    "               OpBranch %%addtid_merge_%s\n"
	    "       %%addtid_merge_%s = OpLabel\n",
	    index_str.c_str(), index_str.c_str(), index_str.c_str(), index_str.c_str(), index_str.c_str(), index_str.c_str(),
	    index_str.c_str(), index_str.c_str(), data.value.c_str(), index_str.c_str(), index_str.c_str(), index_str.c_str(),
	    index_str.c_str());
	return true;
}

static bool EmitDsAddtidLoad(const SpirvValue& dst, const String8& active, const String8& tag,
                             const String8& bound, const String8& tid, Spirv* spirv, const ShaderInstruction& inst,
                             String8* dst_source)
{
	*dst_source += DsAddtidIndex(spirv, inst.ds_offset, bound, tid, active, tag);
	// EXEC and bounds have different result contracts: inactive preserves the
	// VGPR; active OOB produces zero. Both must branch before evaluating memory.
	String8 body = R"(
OpSelectionMerge %addtid_merge_<t> None
OpBranchConditional %<active> %addtid_active_body_<t> %addtid_merge_<t>
%addtid_active_body_<t> = OpLabel
OpSelectionMerge %addtid_read_merge_<t> None
OpBranchConditional %addtid_inb_<t> %addtid_read_<t> %addtid_oob_<t>
%addtid_read_<t> = OpLabel
)";
	if (spirv->UsesDsAddtidLds())
	{
		body += R"(
%addtid_ptr_<t> = OpAccessChain %_ptr_Workgroup_uint %lds %addtid_idx_<t>
%addtid_val_<t> = OpLoad %uint %addtid_ptr_<t>
%addtid_valf_<t> = OpBitcast %float %addtid_val_<t>
)";
	} else
	{
		body += R"(
%addtid_ptr_<t> = OpAccessChain %_ptr_Function_float %lds_addtid %addtid_idx_<t>
%addtid_valf_<t> = OpLoad %float %addtid_ptr_<t>
)";
	}
	body += R"(
OpBranch %addtid_read_merge_<t>
%addtid_oob_<t> = OpLabel
%addtid_zero_<t> = OpBitcast %float %uint_0
OpBranch %addtid_read_merge_<t>
%addtid_read_merge_<t> = OpLabel
%addtid_res_<t> = OpPhi %float %addtid_valf_<t> %addtid_read_<t> %addtid_zero_<t> %addtid_oob_<t>
OpStore %<dst> %addtid_res_<t>
OpBranch %addtid_merge_<t>
%addtid_merge_<t> = OpLabel
)";
	*dst_source += body.ReplaceStr("<t>", tag).ReplaceStr("<active>", active).ReplaceStr("<dst>", dst.value);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_DsReadAddtidB32_VdstOffset)
{
	const auto& inst = code.GetInstructions().At(index);
	if (!spirv->UsesDsAddtid())
	{
		return false;
	}
	const auto index_str = String8::FromPrintf("%u", index);

	if (spirv->UsesDsAddtidLds())
	{
		const auto bound = spirv->GetConstantUint(spirv->GetCsInputInfo()->lds_dwords);
		if (bound == "unknown_uint_constant")
		{
			return false;
		}
		if (spirv->UsesComputeWaveBanks())
		{
			for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
			{
				const auto dst    = spirv->GetComputeWaveRegister(inst.dst, bank, 0);
				const auto bname  = bank == ShaderWaveBank::Low ? "low" : "high";
				if (dst.value.IsEmpty())
				{
					return false;
				}
				const auto tag = String8::FromPrintf("%u_%s", index, bname);
				String8    active;
				if (!EmitDsAddtidActive(spirv, bank, tag, dst_source, &active))
				{
					return false;
				}
				*dst_source += String8::FromPrintf(
				    "        %%addtid_tid_%s = OpBitwiseAnd %%uint %%wave_logical_%s %%uint_63\n", tag.c_str(), bname);
				if (!EmitDsAddtidLoad(dst, active, tag, bound, "addtid_tid_" + tag, spirv, inst, dst_source))
				{
					return false;
				}
			}
			return true;
		}
		String8 active;
		if (!EmitDsAddtidActive(spirv, ShaderWaveBank::Low, index_str, dst_source, &active))
		{
			return false;
		}
		*dst_source += String8::FromPrintf("        %%addtid_tidl_%s = OpLoad %%uint %%gl_LocalInvocationIndex\n"
		                                   "         %%addtid_tid_%s = OpBitwiseAnd %%uint %%addtid_tidl_%s %%uint_63\n",
		                                   index_str.c_str(), index_str.c_str(), index_str.c_str());
		const auto dst = operand_variable_to_str(inst.dst);
		if (dst.value.IsEmpty())
		{
			return false;
		}
		return EmitDsAddtidLoad(dst, active, index_str, bound, "addtid_tid_" + index_str, spirv, inst, dst_source);
	}

	// Private spill array (compute without LDS or a graphics stage): the bank
	// loop and TID*4 term are kept so the two emulated lanes stay distinct.
	const auto bound = spirv->GetConstantUint(kDsAddtidSpillDwords);
	if (bound == "unknown_uint_constant")
	{
		return false;
	}
	if (spirv->UsesComputeWaveBanks())
	{
		for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
		{
			const auto dst   = spirv->GetComputeWaveRegister(inst.dst, bank, 0);
			const auto bname = bank == ShaderWaveBank::Low ? "low" : "high";
			if (dst.value.IsEmpty())
			{
				return false;
			}
			const auto tag = String8::FromPrintf("%u_%s", index, bname);
			String8    active;
			if (!EmitDsAddtidActive(spirv, bank, tag, dst_source, &active))
			{
				return false;
			}
			*dst_source += String8::FromPrintf(
			    "        %%addtid_tid_%s = OpBitwiseAnd %%uint %%wave_logical_%s %%uint_63\n", tag.c_str(), bname);
			if (!EmitDsAddtidLoad(dst, active, tag, bound, "addtid_tid_" + tag, spirv, inst, dst_source)) { return false; }
		}
		return true;
	}
	const auto dst = operand_variable_to_str(inst.dst);
	if (dst.value.IsEmpty())
	{
		return false;
	}
	String8 active;
	if (!EmitDsAddtidActive(spirv, ShaderWaveBank::Low, index_str, dst_source, &active))
	{
		return false;
	}
	return EmitDsAddtidLoad(dst, active, index_str, bound, String8(), spirv, inst, dst_source);
}

KYTY_RECOMPILER_FUNC(Recompile_SBarrier_Empty)
{
	if (spirv->GetCsInputInfo() == nullptr)
	{
		return false;
	}

	const auto execution_scope = spirv->GetConstantUint(SPIRV_SCOPE_WORKGROUP);
	const auto memory_scope    = spirv->GetConstantUint(SPIRV_SCOPE_WORKGROUP);
	const auto semantics       = spirv->GetConstantUint(SPIRV_WORKGROUP_MEMORY_ACQ_REL);

	*dst_source += String8::FromPrintf("               OpControlBarrier %%%s %%%s %%%s\n", execution_scope.c_str(),
	                                  memory_scope.c_str(), semantics.c_str());
	return true;
}


static String8 GuestScalarBufferAddress(Spirv* spirv, const ShaderInstruction& inst, const String8& tag, const String8& offset)
{
	// SMEM ignores format and selectors. Stride changes the byte extent only;
	// the scalar offset already contains any element-index multiplication.
	return String8(R"(
%<t>_base_raw = OpLoad %uint %<base_lo>
%<t>_descriptor_hi = OpLoad %uint %<base_hi>
%<t>_records = OpLoad %uint %<records>
%<t>_base_lo = OpBitwiseAnd %uint %<t>_base_raw %<align>
%<t>_base_hi = OpBitwiseAnd %uint %<t>_descriptor_hi %<mask16>
%<t>_base_lo64 = OpUConvert %ulong %<t>_base_lo
%<t>_base_hi64 = OpUConvert %ulong %<t>_base_hi
%<t>_base_shift = OpShiftLeftLogical %ulong %<t>_base_hi64 %uint_32
%<t>_base = OpBitwiseOr %ulong %<t>_base_shift %<t>_base_lo64
%<t>_stride_raw = OpShiftRightLogical %uint %<t>_descriptor_hi %uint_16
%<t>_stride = OpBitwiseAnd %uint %<t>_stride_raw %<stride_mask>
%<t>_raw = OpIEqual %bool %<t>_stride %uint_0
%<t>_scale = OpSelect %uint %<t>_raw %uint_1 %<t>_stride
%<t>_scale64 = OpUConvert %ulong %<t>_scale
%<t>_records64 = OpUConvert %ulong %<t>_records
%<t>_bytes = OpIMul %ulong %<t>_scale64 %<t>_records64
%<t>_offset_aligned = OpBitwiseAnd %uint %<offset> %<align>
%<t>_offset64 = OpUConvert %ulong %<t>_offset_aligned
)")
	    .ReplaceStr("<t>", tag).ReplaceStr("<offset>", offset)
	    .ReplaceStr("<base_lo>", operand_variable_to_str(inst.src[0], 0).value)
	    .ReplaceStr("<base_hi>", operand_variable_to_str(inst.src[0], 1).value)
	    .ReplaceStr("<records>", operand_variable_to_str(inst.src[0], 2).value)
	    .ReplaceStr("<align>", spirv->GetConstantUint(0xfffffffcu))
	    .ReplaceStr("<mask16>", spirv->GetConstantUint(0xffffu))
	    .ReplaceStr("<stride_mask>", spirv->GetConstantUint(0x3fffu));
}

static bool EmitGuestScalarBufferWord(Spirv* spirv, const String8& tag, uint32_t word, String8* source)
{
	const auto part = tag + String8::FromPrintf("_%u", word);
	*source += String8(R"(
%<p>_offset = OpIAdd %ulong %<t>_offset64 %gda_u64_<byte>
%<p>_inside = OpULessThan %bool %<p>_offset %<t>_bytes
OpSelectionMerge %<p>_merge None
OpBranchConditional %<p>_inside %<p>_read %<p>_oob
%<p>_read = OpLabel
%<p>_address = OpIAdd %ulong %<t>_base %<p>_offset
%<p>_lo = OpUConvert %uint %<p>_address
%<p>_shift = OpShiftRightLogical %ulong %<p>_address %uint_32
%<p>_hi = OpUConvert %uint %<p>_shift
)")
	    .ReplaceStr("<p>", part).ReplaceStr("<t>", tag).ReplaceStr("<byte>", String8::FromPrintf("%u", word * 4u));
	if (!spirv->EmitGuestLoad(part + "_lo", part + "_hi", 1, part + "_load", source)) { return false; }
	*source += String8(R"(
OpBranch %<p>_merge
%<p>_oob = OpLabel
OpBranch %<p>_merge
%<p>_merge = OpLabel
%<p>_value = OpPhi %uint %<p>_load_d0 %<p>_read %uint_0 %<p>_oob
)").ReplaceStr("<p>", part);
	return true;
}

static bool EmitGuestScalarBufferLoad(Spirv* spirv, const ShaderInstruction& inst, uint32_t index, uint32_t components,
                                      String8* dst_source)
{
	const uint32_t alignment = components == 1u ? 1u : (components == 2u ? 2u : 4u);
	if (!spirv->UsesGuestDeviceAddress() || components == 0u || components > 16u || inst.smem_flags != 0u ||
	    inst.smem_imm_offset != 0 || inst.src_num != 2 || inst.src[0].type != ShaderOperandType::Sgpr ||
	    inst.src[0].size != 4 || inst.src[0].register_id < 0 || inst.src[0].register_id > 100 ||
	    (inst.src[0].register_id % 4) != 0 || inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != components ||
	    inst.dst.register_id < 0 || inst.dst.register_id + components > 106u || (inst.dst.register_id % alignment) != 0u ||
	    inst.src[1].dpp || inst.src[1].absolute || inst.src[1].negate || inst.src[1].swizzle != 6u)
	{
		return false;
	}
	const auto tag = String8::FromPrintf("gsb_%u", index);
	String8 source;
	if (!operand_load_uint(spirv, inst.src[1], tag + "_offset", tag, &source)) { return false; }
	source += GuestScalarBufferAddress(spirv, inst, tag, tag + "_offset");
	for (uint32_t word = 0; word < components; ++word)
	{
		if (!EmitGuestScalarBufferWord(spirv, tag, word, &source)) { return false; }
	}
	// Load every word before committing results: SDST may overlap SBASE/SOFFSET.
	for (uint32_t word = 0; word < components; ++word)
	{
		const auto dst = operand_variable_to_str(inst.dst, static_cast<int>(word));
		source += String8::FromPrintf("OpStore %%%s %%%s_%u_value\n", dst.value.c_str(), tag.c_str(), word);
	}
	*dst_source += source;
	return true;
}

static bool PixelScalarBufferUsesGuestAddress(const Spirv* spirv, const ShaderInstruction& inst)
{
	const auto* bind = spirv->GetBindInfo();
	return Config::IsNextGen() && spirv->GetPsInputInfo() != nullptr && bind != nullptr &&
	       ShaderScalarBufferUsesRuntimeDescriptor(*bind, inst);
}

static bool RecompileZeroSBufferLoad(const ShaderInstruction& inst, uint32_t components, const ShaderBindResources* bind_info,
                                     String8* dst_source)
{
	if (bind_info == nullptr || dst_source == nullptr || inst.src_num == 0 || inst.src[0].type != ShaderOperandType::Sgpr)
	{
		return false;
	}
	bool zero_descriptor = false;
	for (int i = 0; i < bind_info->zero_sbuffer_resources.buffers_num; ++i)
	{
		zero_descriptor = zero_descriptor || bind_info->zero_sbuffer_resources.start_register[i] == inst.src[0].register_id;
	}
	if (!zero_descriptor)
	{
		return false;
	}
	for (uint32_t component = 0; component < components; ++component)
	{
		const auto dst = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (dst.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst.type != SpirvType::Uint condition ignored (continuing)\n"); }
		*dst_source += String8::FromPrintf("               OpStore %%%s %%uint_0\n", dst.value.c_str());
	}
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDword_SdstSvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (PixelScalarBufferUsesGuestAddress(spirv, inst))
	{
		return EmitGuestScalarBufferLoad(spirv, inst, index, 1u, dst_source);
	}
	if (RecompileZeroSBufferLoad(inst, 1, bind_info, dst_source))
	{
		return true;
	}

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 index_str   = String8::FromPrintf("%u", index);
		String8 load1;

		if (dst_value.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }
		if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
		{
			return false;
		}

		static const char* text_plain = R"(
        <load1>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword %<p0> %temp_int_1 %temp_int_2
)";
		static const char* text_imm   = R"(
        <load1>
        %t1imm_<index> = OpIAdd %uint %t1_<index> %<imm>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1imm_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword %<p0> %temp_int_1 %temp_int_2
)";
		const char*        text       = (inst.smem_imm_offset != 0) ? text_imm : text_plain;
		*dst_source += String8(text)
		                   .ReplaceStr("<load1>", load1)
		                   .ReplaceStr("<index>", index_str)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<p0>", dst_value.value)
		                   .ReplaceStr("<imm>", spirv->GetConstantUint(static_cast<uint32_t>(inst.smem_imm_offset)));

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx2_Sdst2SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (PixelScalarBufferUsesGuestAddress(spirv, inst))
	{
		return EmitGuestScalarBufferLoad(spirv, inst, index, 2u, dst_source);
	}
	if (RecompileZeroSBufferLoad(inst, 2, bind_info, dst_source))
	{
		return true;
	}

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 index_str   = String8::FromPrintf("%u", index);
		String8 load1;

		if (dst_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }
		if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
		{
			return false;
		}

		static const char* text_plain = R"(
        <load1>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_2 %<p0> %<p1> %temp_int_1 %temp_int_2
)";
		static const char* text_imm   = R"(
        <load1>
        %t1imm_<index> = OpIAdd %uint %t1_<index> %<imm>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1imm_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_2 %<p0> %<p1> %temp_int_1 %temp_int_2
)";
		const char*        text       = (inst.smem_imm_offset != 0) ? text_imm : text_plain;
		*dst_source += String8(text)
		                   .ReplaceStr("<load1>", load1)
		                   .ReplaceStr("<index>", index_str)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<imm>", spirv->GetConstantUint(static_cast<uint32_t>(inst.smem_imm_offset)));

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx4_Sdst4SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (PixelScalarBufferUsesGuestAddress(spirv, inst))
	{
		return EmitGuestScalarBufferLoad(spirv, inst, index, 4u, dst_source);
	}
	if (RecompileZeroSBufferLoad(inst, 4, bind_info, dst_source))
	{
		return true;
	}

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		// EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));

		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		// String8 offset      = spirv->GetConstant(inst.src[1]);

		if (dst_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }

		if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }

		String8 index_str = String8::FromPrintf("%u", index);

		String8 load1;

		if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
		{
			return false;
		}

		// Optional SMEM immediate: final byte offset = SGPR soffset + signed imm
		// (captured s_buffer_load_dwordx4 s[…], s[…], s24 offset:0x10).
		static const char* text_plain = R"(
        <load1>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2
)";
		static const char* text_imm   = R"(
        <load1>
        %t1imm_<index> = OpIAdd %uint %t1_<index> %<imm>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1imm_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2
)";
		const char*        text       = (inst.smem_imm_offset != 0) ? text_imm : text_plain;
		*dst_source += String8(text)
		                   .ReplaceStr("<load1>", load1)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value)
		                   .ReplaceStr("<imm>", spirv->GetConstantUint(static_cast<uint32_t>(inst.smem_imm_offset)))
		                   .ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

// s_buffer_load byte offset as %t102_<index> (int): an inline/literal
// constant, or an SGPR, VCC word or M0 read at execution time.
String8 Spirv::WrapVertexScalarBufferProbe(const ShaderInstruction& inst, uint32_t index,
                                          uint32_t site, const String8& original)
{
	if (!UsesVertexClipProbe() || site >= kVertexScalarBufferProbeSites || inst.src_num < 2 ||
	    inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].size != 4 ||
	    inst.dst.size <= 0 || inst.dst.size > 16)
	{
		return original;
	}
	const uint32_t components = static_cast<uint32_t>(inst.dst.size);
	// A destination word with no SPIR-V variable (for example the second word of a VCC pair) cannot be observed.
	for (uint32_t word = 0; word < components; ++word)
	{
		if (operand_variable_to_str(inst.dst, word).value.IsEmpty()) { return original; }
	}
	String8 before;
	const auto tag = String8::FromPrintf("vs_sbuffer_%u", index);
	if (!operand_load_uint(this, inst.src[1], tag + "_offset_raw", tag, &before)) { return original; }
	before += String8::FromPrintf("\n%%%s_offset = OpIAdd %%uint %%%s_offset_raw %%%s\n",
	                              tag.c_str(), tag.c_str(), GetConstantUint(static_cast<uint32_t>(inst.smem_imm_offset)).c_str());
	for (uint32_t word = 0; word < 4u; ++word)
	{
		before += String8::FromPrintf("%%vs_sbuffer_desc_%u_%u = OpLoad %%uint %%%s\n", index, word,
		                              operand_variable_to_str(inst.src[0], word).value.c_str());
	}
	const auto pointer = [&](uint32_t field)
	{
		return String8::FromPrintf("%%vs_sbuffer_ptr_%u_%u = OpAccessChain %%_ptr_StorageBuffer_uint %%vertex_clip_probe %%int_51 %%%s\n",
		                           index, field, GetConstantInt(static_cast<int>(site * kVertexScalarBufferProbeWords + field)).c_str());
	};
	String8 after = pointer(0u);
	after += String8::FromPrintf(
	    "%%vs_sbuffer_claim_ptr_%u = OpCopyObject %%_ptr_StorageBuffer_uint %%vs_sbuffer_ptr_%u_0\n"
	    "%%vs_sbuffer_prior_%u = OpAtomicCompareExchange %%uint %%vs_sbuffer_claim_ptr_%u %%uint_1 %%uint_72 %%uint_0 %%uint_1 %%uint_0\n"
	    "%%vs_sbuffer_won_%u = OpIEqual %%bool %%vs_sbuffer_prior_%u %%uint_0\n"
	    "OpSelectionMerge %%vs_sbuffer_merge_%u None\n"
	    "OpBranchConditional %%vs_sbuffer_won_%u %%vs_sbuffer_store_%u %%vs_sbuffer_merge_%u\n"
	    "%%vs_sbuffer_store_%u = OpLabel\n",
	    index, index, index, index, index, index, index, index, index, index, index);
	const auto store = [&](uint32_t field, const String8& value)
	{
		after += pointer(field);
		after += String8::FromPrintf("OpStore %%vs_sbuffer_ptr_%u_%u %s\n", index, field, value.c_str());
	};
	store(1u, String8("%") + GetConstantUint(inst.pc));
	store(2u, String8("%") + GetConstantUint(components));
	store(3u, String8::FromPrintf("%%%s_offset", tag.c_str()));
	for (uint32_t word = 0; word < 4u; ++word)
	{
		store(4u + word, String8::FromPrintf("%%vs_sbuffer_desc_%u_%u", index, word));
	}
	for (uint32_t word = 0; word < components; ++word)
	{
		after += String8::FromPrintf("%%vs_sbuffer_value_%u_%u = OpLoad %%uint %%%s\n", index, word,
		                             operand_variable_to_str(inst.dst, word).value.c_str());
		store(8u + word, String8::FromPrintf("%%vs_sbuffer_value_%u_%u", index, word));
	}
	after += String8::FromPrintf("OpBranch %%vs_sbuffer_merge_%u\n%%vs_sbuffer_merge_%u = OpLabel\n", index, index);
	return before + original + after;
}

static String8 SBufferOffsetDefinition(Spirv* spirv, const ShaderOperand& offset, uint32_t index)
{
	if (operand_is_constant(offset))
	{
		return String8::FromPrintf("        %%t102_%u = OpBitcast %%int %%%s", index, spirv->GetConstant(offset).c_str());
	}
	return String8::FromPrintf("        %%t102_raw_%u = OpLoad %%uint %%%s\n        %%t102_%u = OpBitcast %%int %%t102_raw_%u", index,
	                           operand_variable_to_str(offset).value.c_str(), index, index);
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx8_Sdst8SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (PixelScalarBufferUsesGuestAddress(spirv, inst))
	{
		return EmitGuestScalarBufferLoad(spirv, inst, index, 8u, dst_source);
	}
	if (RecompileZeroSBufferLoad(inst, 8, bind_info, dst_source))
	{
		return true;
	}

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (!operand_is_constant(inst.src[1])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[1]) condition ignored (continuing)\n"); }

		SpirvValue dst_value[8];

		for (int i = 0; i < 8; i++)
		{
			dst_value[i] = operand_variable_to_str(inst.dst, i);
		}

		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 offset      = spirv->GetConstant(inst.src[1]);

		if (dst_value[0].type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value[0].type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }

		String8 text = R"(
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
<offset_def>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_8 %<p0> %<p1> %<p2> %<p3> %<p4> %<p5> %<p6> %<p7> %temp_int_1 %temp_int_2
)";

		for (int i = 0; i < 8; i++)
		{
			text = text.ReplaceStr(String8::FromPrintf("<p%d>", i), dst_value[i].value);
		}

		*dst_source += text.ReplaceStr("<offset_def>", SBufferOffsetDefinition(spirv, inst.src[1], index))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src0_value0>", src0_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx16_Sdst16SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (PixelScalarBufferUsesGuestAddress(spirv, inst))
	{
		return EmitGuestScalarBufferLoad(spirv, inst, index, 16u, dst_source);
	}
	if (RecompileZeroSBufferLoad(inst, 16, bind_info, dst_source))
	{
		return true;
	}

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (!operand_is_constant(inst.src[1])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[1]) condition ignored (continuing)\n"); }

		SpirvValue dst_value[16];

		for (int i = 0; i < 16; i++)
		{
			dst_value[i] = operand_variable_to_str(inst.dst, i);
		}

		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 offset      = spirv->GetConstant(inst.src[1]);

		if (dst_value[0].type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value[0].type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }

		String8 text = R"(
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
<offset_def>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_16 %<p0> %<p1> %<p2> %<p3> %<p4> %<p5> %<p6> %<p7> %<p8> %<p9> %<p10> %<p11> %<p12> %<p13> %<p14> %<p15> %temp_int_1 %temp_int_2
)";

		for (int i = 0; i < 16; i++)
		{
			text = text.ReplaceStr(String8::FromPrintf("<p%d>", i), dst_value[i].value);
		}

		*dst_source += text.ReplaceStr("<offset_def>", SBufferOffsetDefinition(spirv, inst.src[1], index))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src0_value0>", src0_value0.value);

		return true;
	}

	return false;
}

// KYTY_RECOMPILER_FUNC(Recompile_SCbranchExecz_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	EXIT_NOT_IMPLEMENTED(code.ReadBlock(ShaderLabel(inst).GetDst()).is_discard);
//
//	String8 label = ShaderLabel(inst).ToString();
//
//	static const char* text = R"(
//         %execz_u_<index> = OpLoad %uint %execz
//         %execz_b_<index> = OpINotEqual %bool %execz_u_<index> %uint_0
//                OpSelectionMerge %<label> None
//                OpBranchConditional %execz_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(text).ReplaceStr("<index>", String8::FromPrintf("%u", index)).ReplaceStr("<label>", label);
//
//	return true;
// }


static bool recompile_sload_from_extended(uint32_t index, const ShaderInstruction& inst, Spirv* spirv, String8* dst_source, int dword_count)
{
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr || !bind_info->extended.used)
	{
		return false;
	}

	const auto* vs_info    = spirv->GetVsInputInfo();
	int         shift_regs = (vs_info != nullptr && vs_info->gs_prolog ? 8 : 0);

	if (shift_regs != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: shift_regs != 0 condition ignored (continuing)\n"); }
	if (!operand_is_constant(inst.src[1])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[1]) condition ignored (continuing)\n"); }
	if (inst.src[0].register_id != bind_info->extended.start_register) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.src[0].register_id != bind_info->extended.start_register condition ignored (continuing)\n"); }

	// TODO() check pointer

	if (dword_count <= 0 || dword_count > 16)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dword_count <= 0 || dword_count > 8 condition ignored (continuing)\n");
	}
	if (inst.dst.size != dword_count)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.dst.size != dword_count condition ignored (continuing)\n");
	}

	SpirvValue dst_value[16];
	for (int i = 0; i < dword_count; i++)
	{
		dst_value[i] = (dword_count == 1 ? operand_variable_to_str(inst.dst) : operand_variable_to_str(inst.dst, i));
		if (dst_value[i].type != SpirvType::Uint)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value[i].type != SpirvType::Uint condition ignored (continuing)\n");
		}
	}

	auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
	auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
	int offset = static_cast<int>(inst.src[1].constant.u >> 2u);

	if (src0_value0.type != SpirvType::Uint)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Uint condition ignored (continuing)\n");
	}
	if (src0_value1.type != SpirvType::Uint)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value1.type != SpirvType::Uint condition ignored (continuing)\n");
	}

	for (int i = 0; i < dword_count; i++)
	{
		int buffer = 0;
		int field = 0;
		if (!spirv->GetDynamicSLoadMappedIndex(inst.pc, offset + i, &buffer, &field))
		{
			spirv->GetMappedIndex(offset + i, &buffer, &field);
		}

		const auto id = String8::FromPrintf("vsharp_%u_%d_value_%s", index, i, dst_value[i].value.c_str());
		*dst_source += spirv->EmitMetadataLoad(buffer, field, id);
		*dst_source += String8::FromPrintf("OpStore %%%s %%%s\n", dst_value[i].value.c_str(), id.c_str());
	}

	return true;
}

// Materialize S_LOAD from the Gen5 vertex attribute table (fetch_attrib_reg).
// Destinations receive the snapshotted guest dwords so later SBfe/SLshl see defined values.
static bool recompile_sload_from_fetch_attrib(uint32_t index, const ShaderInstruction& inst, Spirv* spirv, String8* dst_source,
                                              int dword_count)
{
	const auto* vs_info = spirv->GetVsInputInfo();
	if (vs_info == nullptr || !vs_info->fetch_embedded || vs_info->fetch_external || vs_info->fetch_inline)
	{
		return false;
	}

	int shift_regs = (vs_info->gs_prolog ? 8 : 0);
	if (inst.src[0].register_id != vs_info->fetch_attrib_reg + shift_regs)
	{
		return false;
	}

	if (!operand_is_constant(inst.src[1])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[1]) condition ignored (continuing)\n"); }
	if (dword_count <= 0 || dword_count > 4) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dword_count <= 0 || dword_count > 4 condition ignored (continuing)\n"); }
	if (inst.dst.size != dword_count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.dst.size != dword_count condition ignored (continuing)\n"); }

	int dword_index = static_cast<int>(inst.src[1].constant.u >> 2u);
	if (dword_index < 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dword_index < 0 condition ignored (continuing)\n"); }
	if (dword_index + dword_count > vs_info->fetch_attrib_data_num) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dword_index + dword_count > vs_info->fetch_attrib_data_num condition ignored (continuing)\n"); }

	static const char* text = R"(
		         %sload_attr_<index> = OpBitcast %uint %<const>
		               OpStore %<reg> %sload_attr_<index>
				)";

	for (int i = 0; i < dword_count; i++)
	{
		auto dst = (dword_count == 1 ? operand_variable_to_str(inst.dst) : operand_variable_to_str(inst.dst, i));
		if (dst.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst.type != SpirvType::Uint condition ignored (continuing)\n"); }

		uint32_t value = vs_info->fetch_attrib_data[dword_index + i];
		*dst_source += String8(text)
		                   .ReplaceStr("<reg>", dst.value)
		                   .ReplaceStr("<const>", spirv->GetConstantUint(value))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u_%d", index, i));
	}

	return true;
}

// fetch_buffer_reg SLoads feed DetectFetch buffer-descriptor tracking; destinations
// are not consumed after BufferLoad→Fetch rewrite. Keep as recognized no-op.
static bool recompile_sload_fetch_buffer_meta(const ShaderInstruction& inst, Spirv* spirv)
{
	const auto* vs_info = spirv->GetVsInputInfo();
	if (vs_info == nullptr || !vs_info->fetch_embedded || vs_info->fetch_external || vs_info->fetch_inline)
	{
		return false;
	}

	int shift_regs = (vs_info->gs_prolog ? 8 : 0);
	return inst.src[0].register_id == vs_info->fetch_buffer_reg + shift_regs;
}

// s_load through a guest pointer that is not the mapped extended pointer:
// ADDR = SGPR[base][47:0] + offset with the low two bits cleared (RDNA2 SMEM).
static bool recompile_sload_guest(uint32_t index, const ShaderInstruction& inst, Spirv* spirv, String8* dst_source, int dword_count)
{
	const auto* bind = spirv->GetBindInfo();
	if (!spirv->UsesGuestDeviceAddress() || bind == nullptr || inst.src[0].type != ShaderOperandType::Sgpr ||
	    (bind->extended.used && inst.src[0].register_id == bind->extended.start_register))
	{
		return false;
	}
	const auto i      = String8::FromPrintf("sg_%u", index);
	const auto base0  = operand_variable_to_str(inst.src[0], 0);
	const auto base1  = operand_variable_to_str(inst.src[0], 1);
	String8    offset = String8::FromPrintf("%%%s_off = OpCopyObject %%uint %%%s\n", i.c_str(), spirv->GetConstantUint(0u).c_str());
	if (operand_is_constant(inst.src[1]))
	{
		offset = String8::FromPrintf("%%%s_off = OpCopyObject %%uint %%%s\n", i.c_str(), spirv->GetConstantUint(inst.src[1].constant.u).c_str());
	} else if (inst.src[1].type == ShaderOperandType::Sgpr || inst.src[1].type == ShaderOperandType::VccLo ||
	           inst.src[1].type == ShaderOperandType::VccHi || inst.src[1].type == ShaderOperandType::M0)
	{
		offset = String8::FromPrintf("%%%s_off = OpLoad %%uint %%%s\n", i.c_str(), operand_variable_to_str(inst.src[1]).value.c_str());
	} else
	{
		return false;
	}
	String8 source = offset;
	source += String8::FromPrintf("%%%s_imm = OpIAdd %%uint %%%s_off %%%s\n", i.c_str(), i.c_str(),
	                              spirv->GetConstantUint(static_cast<uint32_t>(inst.smem_imm_offset)).c_str());
	source += String8(R"(
%<i>_blo = OpLoad %uint %<b0>
%<i>_braw = OpLoad %uint %<b1>
%<i>_bhi = OpBitwiseAnd %uint %<i>_braw %<mask48>
%<i>_sum = OpIAdd %uint %<i>_blo %<i>_imm
%<i>_carry_b = OpULessThan %bool %<i>_sum %<i>_blo
%<i>_carry = OpSelect %uint %<i>_carry_b %uint_1 %uint_0
%<i>_hi = OpIAdd %uint %<i>_bhi %<i>_carry
%<i>_lo = OpBitwiseAnd %uint %<i>_sum %<align>
)")
	              .ReplaceStr("<i>", i)
	              .ReplaceStr("<b0>", base0.value)
	              .ReplaceStr("<b1>", base1.value)
	              .ReplaceStr("<mask48>", spirv->GetConstantUint(0xffffu))
	              .ReplaceStr("<align>", spirv->GetConstantUint(0xfffffffcu));
	if (!spirv->EmitGuestLoad(i + "_lo", i + "_hi", dword_count, i, &source))
	{
		return false;
	}
	for (int word = 0; word < dword_count; word++)
	{
		const auto dst = dword_count == 1 ? operand_variable_to_str(inst.dst) : operand_variable_to_str(inst.dst, word);
		source += String8::FromPrintf("               OpStore %%%s %%%s_d%d\n", dst.value.c_str(), i.c_str(), word);
	}
	*dst_source += source;
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SLoadDword_SdstSbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);

	if (recompile_sload_guest(index, inst, spirv, dst_source, 1))
	{
		return true;
	}

	if (recompile_sload_from_fetch_attrib(index, inst, spirv, dst_source, 1))
	{
		return true;
	}
	if (recompile_sload_fetch_buffer_meta(inst, spirv))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 1);
}

KYTY_RECOMPILER_FUNC(Recompile_SLoadDwordx2_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	if (recompile_sload_guest(index, inst, spirv, dst_source, 2))
	{
		return true;
	}

	if (recompile_sload_from_fetch_attrib(index, inst, spirv, dst_source, 2))
	{
		return true;
	}
	if (recompile_sload_fetch_buffer_meta(inst, spirv))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 2);
}

KYTY_RECOMPILER_FUNC(Recompile_SLoadDwordx4_Sdst4SbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);

	if (recompile_sload_guest(index, inst, spirv, dst_source, 4))
	{
		return true;
	}

	if (recompile_sload_from_fetch_attrib(index, inst, spirv, dst_source, 4))
	{
		return true;
	}
	if (recompile_sload_fetch_buffer_meta(inst, spirv))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 4);
}

KYTY_RECOMPILER_FUNC(Recompile_SLoadDwordx8_Sdst8SbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);

	if (recompile_sload_guest(index, inst, spirv, dst_source, 8))
	{
		return true;
	}

	if (recompile_sload_fetch_buffer_meta(inst, spirv))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 8);
}

KYTY_RECOMPILER_FUNC(Recompile_SLoadDwordx16_Sdst16SbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);
	if (recompile_sload_guest(index, inst, spirv, dst_source, 16))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 16);
}


static bool TBufferFloatLoadSupported(const ShaderInstruction& inst, uint8_t components)
{
	if (!ShaderInstructionLoweringPreconditions(inst) || inst.src_num != 3 || inst.dst.size != components ||
	    inst.mtbuf_components != components || inst.mtbuf_format_is_gen5 != Config::IsNextGen()) { return false; }
	// These emitters perform unconverted 32-bit float loads. Other memory
	// formats/component combinations need their own conversion/default rules.
	switch (components)
	{
		case 1: return inst.mtbuf_format == (inst.mtbuf_format_is_gen5 ? 22u : 0x74u);
		case 2: return inst.mtbuf_format == (inst.mtbuf_format_is_gen5 ? 64u : 0x7bu);
		case 4: return inst.mtbuf_format == (inst.mtbuf_format_is_gen5 ? 77u : 0x7eu);
		default: return false;
	}
}

KYTY_RECOMPILER_FUNC(Recompile_TBufferLoadFormatX_Vdata1VaddrSvSoffsIdxenFloat1)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!TBufferFloatLoadSupported(inst, 1)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_tbuffer_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_x", inst.mtbuf_format, 1, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
               OpStore %temp_int_5 %int_36
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_x %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_TBufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxenFloat4)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!TBufferFloatLoadSupported(inst, 4)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_tbuffer_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_xyzw", inst.mtbuf_format, 4, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
               OpStore %temp_int_5 %int_119
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_TBufferLoadFormatXy_Vdata2VaddrSvSoffsIdxenFloat2)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!TBufferFloatLoadSupported(inst, 2)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr || bind_info->storage_buffers.buffers_num == 0)
	{
		return false;
	}
	if (Config::IsNextGen())
	{
		return emit_gen5_tbuffer_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_xy", inst.mtbuf_format, 2, dst_source);
	}
	if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }
	auto dst0 = operand_variable_to_str(inst.dst, 0);
	auto dst1 = operand_variable_to_str(inst.dst, 1);
	auto addr = operand_variable_to_str(inst.src[0]);
	auto desc0 = operand_variable_to_str(inst.src[1], 0);
	auto desc1 = operand_variable_to_str(inst.src[1], 1);
	String8 offset = GetBufferOffsetIntConstant(spirv, inst.src[2]);
	if (dst0.type != SpirvType::Float || dst1.type != SpirvType::Float || addr.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst0.type != SpirvType::Float || dst1.type != SpirvType::Float || addr.type != SpirvType::Float condition ignored (continuing)\n"); }
	if (desc0.type != SpirvType::Uint || desc1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: desc0.type != SpirvType::Uint || desc1.type != SpirvType::Uint condition ignored (continuing)\n"); }
	static const char* text = R"(
%txy_addr_<index> = OpLoad %float %<addr>
%txy_index_<index> = OpBitcast %int %txy_addr_<index>
OpStore %temp_int_1 %txy_index_<index>
%txy_desc1_<index> = OpLoad %uint %<desc1>
%txy_stride_u_<index> = OpShiftRightLogical %uint %txy_desc1_<index> %int_16
%txy_stride_mask_<index> = OpBitwiseAnd %uint %txy_stride_u_<index> %uint_0x00003fff
%txy_stride_<index> = OpBitcast %int %txy_stride_mask_<index>
OpStore %temp_int_3 %txy_stride_<index>
%txy_desc0_<index> = OpLoad %uint %<desc0>
%txy_buffer_<index> = OpBitcast %int %txy_desc0_<index>
OpStore %temp_int_4 %txy_buffer_<index>
OpStore %temp_int_2 %<offset>
OpStore %temp_int_5 %int_64
%txy_call_<index> = OpFunctionCall %void %tbuffer_load_format_xy %<dst0> %<dst1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<addr>", addr.value)
	                   .ReplaceStr("<desc0>", desc0.value)
	                   .ReplaceStr("<desc1>", desc1.value)
	                   .ReplaceStr("<offset>", offset)
	                   .ReplaceStr("<dst0>", dst0.value)
	                   .ReplaceStr("<dst1>", dst1.value);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_TBufferLoadFormatXyzw_Vdata4Vaddr2SvSoffsOffenIdxenFloat4)
{
	const auto& inst      = code.GetInstructions().At(index);
	if (!TBufferFloatLoadSupported(inst, 4)) { return false; }
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_tbuffer_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_xyzw", inst.mtbuf_format, 4, dst_source);
		}

		if (!operand_is_constant(inst.src[2])) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_constant(inst.src[2]) condition ignored (continuing)\n"); }

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto    src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value1.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value1.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src1_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
       %to100_<index> = OpLoad %float %<src0_value1>
       %to101_<index> = OpBitcast %int %to100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
      %offset_<index> = OpIAdd %int %to101_<index> %<offset>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %offset_<index>
               OpStore %temp_int_5 %int_119
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

/* XXX: F, Eq, Ge, Gt, Le, Lg, Lt, Neq, Nge, Ngt, Nlg, Nlt, O, Tru, U */

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
