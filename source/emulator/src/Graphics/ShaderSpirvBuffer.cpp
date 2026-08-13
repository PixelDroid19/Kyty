#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvTemplates.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"

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
};

// Materialize a descriptor-driven byte address into the legacy buffer helper
// scratch arguments. All MUBUF/MTBUF users share this so index/offset ordering
// and the Gen5 swizzle equation cannot drift between loads and stores.
static bool emit_buffer_address_setup(Spirv* spirv, const ShaderInstruction& inst, int instruction_index,
                                      uint32_t component_byte_offset, BufferAddressRangeCheck range_check,
                                      BufferAddressSetup* setup)
{
	EXIT_IF(spirv == nullptr);
	EXIT_IF(setup == nullptr);

	if (inst.src_num < 3 || inst.src[1].size < 4)
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
		const int  index_part = inst.buffer_offen ? 1 : -1;
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
		if (!operand_load_uint(spirv, inst.src[0], offset_name, tag, &vector_offset_load, 0))
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
	String8     addr_value = String8::FromPrintf("%%buf_addr_%s", tag.c_str());
	String8     slot_value = String8::FromPrintf("%%buf_addr_desc0_%s", tag.c_str());
	const auto* vs_input   = spirv->GetVsInputInfo();
	const auto* bind_info  = spirv->GetBindInfo();
	if (vs_input != nullptr && vs_input->fetch_embedded && bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		const auto bound_id = spirv->GetConstantUint(static_cast<uint32_t>(bind_info->storage_buffers.buffers_num));
		if (bound_id != "unknown_uint_constant")
		{
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
				const auto span      = String8::FromPrintf("%d", span_i);
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
        %buf_addr_in_<tag>_<span> = OpULessThan %bool %buf_addr_rel_<tag>_<span> %buf_addr_bytes_<tag>_<span>
        %buf_addr_ge_<tag>_<span> = OpUGreaterThanEqual %bool %buf_addr_desc0_<tag> %buf_addr_base_<tag>_<span>
        %buf_addr_hi_<tag>_<span> = OpBitwiseAnd %uint %buf_addr_desc1_<tag> %uint_0x0000ffff
        %buf_addr_vhi_<tag>_<span> = OpBitwiseAnd %uint %buf_addr_w1_<tag>_<span> %uint_0x0000ffff
        %buf_addr_hieq_<tag>_<span> = OpIEqual %bool %buf_addr_hi_<tag>_<span> %buf_addr_vhi_<tag>_<span>
        %buf_addr_range_<tag>_<span> = OpLogicalAnd %bool %buf_addr_ge_<tag>_<span> %buf_addr_in_<tag>_<span>
        %buf_addr_live_<tag>_<span> = OpLogicalAnd %bool %buf_addr_range_<tag>_<span> %buf_addr_hieq_<tag>_<span>
        %buf_addr_sum_<tag>_<span> = OpIAdd %uint %buf_addr_rel_<tag>_<span> %buf_addr_<tag>
        %<next_slot> = OpSelect %uint %buf_addr_live_<tag>_<span> %<slot_u> %<prev_slot>
        %<next_off> = OpSelect %uint %buf_addr_live_<tag>_<span> %buf_addr_sum_<tag>_<span> %<prev_off>
)")
				                    .ReplaceStr("<tag>", tag)
				                    .ReplaceStr("<span>", span)
				                    .ReplaceStr("<vsharp_ptr>", vsharp_ptr)
				                    .ReplaceStr("<slot_idx>", slot_const)
				                    .ReplaceStr("<slot_u>", slot_u)
				                    .ReplaceStr("<next_slot>", next_slot)
				                    .ReplaceStr("<next_off>", next_off)
				                    .ReplaceStr("<prev_slot>", slot_name)
				                    .ReplaceStr("<prev_off>", off_name);
				slot_name = next_slot;
				off_name  = next_off;
				++span_i;
			}
			live_resolve += String8(R"(
        %buf_addr_clamp_<tag> = OpULessThan %bool %<prev_slot> %<bound>
        %buf_addr_slot_c_<tag> = OpSelect %uint %buf_addr_clamp_<tag> %<prev_slot> %uint_0
        %buf_addr_off_c_<tag> = OpSelect %uint %buf_addr_clamp_<tag> %<prev_off> %uint_0
)")
			                    .ReplaceStr("<tag>", tag)
			                    .ReplaceStr("<bound>", bound_id)
			                    .ReplaceStr("<prev_slot>", slot_name)
			                    .ReplaceStr("<prev_off>", off_name);
			addr_value = String8::FromPrintf("%%buf_addr_off_c_%s", tag.c_str());
			slot_value = String8::FromPrintf("%%buf_addr_slot_c_%s", tag.c_str());
		}
	}

	static const char* text = R"(
        %buf_addr_desc0_<tag> = OpLoad %uint %<desc0>
        %buf_addr_desc1_<tag> = OpLoad %uint %<desc1>
        %buf_addr_desc3_<tag> = OpLoad %uint %<desc3>
<range_guard>
        %buf_addr_<tag> = OpFunctionCall %uint %buffer_raw_address <index_value> <offset_value> <soffset_value> %buf_addr_desc1_<tag> %buf_addr_desc3_<tag>
<live_resolve>
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
	                     .ReplaceStr("<live_resolve>", live_resolve)
	                     .ReplaceStr("<addr_value>", addr_value)
	                     .ReplaceStr("<slot_value>", slot_value)
	                     .ReplaceStr("<index_value>", index_id)
	                     .ReplaceStr("<offset_value>", offset_id)
	                     .ReplaceStr("<soffset_value>", make_id(scalar_offset_name));

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
	const auto zero_float = String8::FromPrintf("%%%s", zero_float_id.c_str());

	String8 source;
	for (uint32_t component = 0; component < dwords; component++)
	{
		const auto dst = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (dst.type != SpirvType::Float)
		{
			return false;
		}
		BufferAddressSetup address_setup;
		if (!emit_buffer_address_setup(spirv, inst, instruction_index, component * 4u, BufferAddressRangeCheck::RawZeroRecord,
		                               &address_setup) ||
		    address_setup.access_enabled.IsEmpty())
		{
			return false;
		}
		const auto tag = String8::FromPrintf("%u_%u", instruction_index, component);
		source += String8(R"(
<address_setup>
               OpSelectionMerge %gen5_raw_load_merge_<tag> None
               OpBranchConditional <access_enabled> %gen5_raw_load_then_<tag> %gen5_raw_load_oob_<tag>
        %gen5_raw_load_then_<tag> = OpLabel
%gen5_raw_load_<tag> = OpFunctionCall %void %buffer_load_float1 %<dst> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpBranch %gen5_raw_load_merge_<tag>
        %gen5_raw_load_oob_<tag> = OpLabel
               OpStore %<dst> <zero>
               OpBranch %gen5_raw_load_merge_<tag>
        %gen5_raw_load_merge_<tag> = OpLabel
)")
		              .ReplaceStr("<address_setup>", address_setup.source)
		              .ReplaceStr("<access_enabled>", address_setup.access_enabled)
		              .ReplaceStr("<tag>", tag)
		              .ReplaceStr("<zero>", zero_float)
		              .ReplaceStr("<dst>", dst.value);
	}

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
	if (!emit_buffer_address_setup(spirv, inst, instruction_index, 0, BufferAddressRangeCheck::RawZeroRecord,
	                               &first_address_setup) ||
	    first_address_setup.access_enabled.IsEmpty())
	{
		return false;
	}

	String8 body = String8::FromPrintf("%%gen5_raw_store_%u_0 = OpFunctionCall %%void %%buffer_store_float1 %%%s %%temp_int_1 %%temp_int_2 %%temp_int_3 %%temp_int_4\n",
	                                  instruction_index, values[0].value.c_str());
	for (uint32_t component = 1; component < dwords; component++)
	{
		BufferAddressSetup address_setup;
		if (!emit_buffer_address_setup(spirv, inst, instruction_index, component * 4u, BufferAddressRangeCheck::None, &address_setup))
		{
			return false;
		}
		body += address_setup.source;
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
	if (!emit_buffer_address_setup(spirv, inst, static_cast<int>(index), 0, BufferAddressRangeCheck::RawZeroRecord,
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
        %gen5_atomic_word_<tag> = OpShiftRightLogical %uint %buf_addr_<tag> %uint_2
        %gen5_atomic_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %buf_uint %buf_addr_desc0_<tag> %int_0 %gen5_atomic_word_<tag>
        %gen5_atomic_value_f_<tag> = OpLoad %float %<value>
        %gen5_atomic_value_<tag> = OpBitcast %uint %gen5_atomic_value_f_<tag>
        %gen5_atomic_prior_<tag> = OpAtomicIAdd %uint %gen5_atomic_ptr_<tag> %<scope> %uint_0 %gen5_atomic_value_<tag>
               OpMemoryBarrier %<scope> %<semantics>
<return_value>               OpBranch %gen5_atomic_merge_<tag>
        %gen5_atomic_merge_<tag> = OpLabel
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<access_enabled>", address_setup.access_enabled)
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
	if (!emit_buffer_address_setup(spirv, inst, static_cast<int>(index), 0, BufferAddressRangeCheck::RawZeroRecord,
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
        %gen5_atomic_word_<tag> = OpShiftRightLogical %uint %buf_addr_<tag> %uint_2
        %gen5_atomic_ptr_<tag> = OpAccessChain %_ptr_StorageBuffer_uint %buf_uint %buf_addr_desc0_<tag> %int_0 %gen5_atomic_word_<tag>
        %gen5_atomic_value_f_<tag> = OpLoad %float %<value>
        %gen5_atomic_value_<tag> = OpBitcast %uint %gen5_atomic_value_f_<tag>
        %gen5_atomic_prior_<tag> = <atomic_op> %uint %gen5_atomic_ptr_<tag> %<scope> %uint_0 %gen5_atomic_value_<tag>
               OpMemoryBarrier %<scope> %<semantics>
<return_value>               OpBranch %gen5_atomic_merge_<tag>
        %gen5_atomic_merge_<tag> = OpLabel
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<access_enabled>", address_setup.access_enabled)
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

	String8 outputs;
	for (uint32_t component = 0; component < components; component++)
	{
		const auto dst = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (dst.type != SpirvType::Float)
		{
			return false;
		}
		outputs += String8::FromPrintf(" %%%s", dst.value.c_str());
	}

	BufferAddressSetup address_setup;
	if (!emit_buffer_address_setup(spirv, inst, instruction_index, 0, BufferAddressRangeCheck::None, &address_setup))
	{
		return false;
	}
	const auto format_id = spirv->GetConstantInt(format);
	if (format_id == "unknown_int_constant")
	{
		return false;
	}

	*dst_source += String8(R"(
<address_setup>
               OpStore %temp_int_5 <format>
%gen5_tbuffer_load_<index> = OpFunctionCall %void %<function><outputs> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<format>", String8::FromPrintf("%%%s", format_id.c_str()))
	                   .ReplaceStr("<function>", function_name)
	                   .ReplaceStr("<outputs>", outputs)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", instruction_index));
	return true;
}

static bool emit_gen5_mubuf_format_load(Spirv* spirv, const ShaderInstruction& inst, int instruction_index,
	                                      const char* function_name, uint32_t components, String8* dst_source)
{
	EXIT_IF(spirv == nullptr);
	EXIT_IF(dst_source == nullptr);

	String8 outputs;
	for (uint32_t component = 0; component < components; component++)
	{
		const auto dst = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (dst.type != SpirvType::Float)
		{
			return false;
		}
		outputs += String8::FromPrintf(" %%%s", dst.value.c_str());
	}

	BufferAddressSetup address_setup;
	if (!emit_buffer_address_setup(spirv, inst, instruction_index, 0, BufferAddressRangeCheck::None, &address_setup))
	{
		return false;
	}
	const auto tag = String8::FromPrintf("%u_0", instruction_index);

	*dst_source += String8(R"(
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
	if (!emit_buffer_address_setup(spirv, inst, instruction_index, 0, BufferAddressRangeCheck::None, &address_setup))
	{
		return false;
	}
	const auto tag = String8::FromPrintf("%u_0", instruction_index);

	*dst_source += String8(R"(
        %gen5_mubuf_store_exec_<tag> = OpLoad %uint %exec_lo
        %gen5_mubuf_store_active_<tag> = OpINotEqual %bool %gen5_mubuf_store_exec_<tag> %uint_0
               OpSelectionMerge %gen5_mubuf_store_merge_<tag> None
               OpBranchConditional %gen5_mubuf_store_active_<tag> %gen5_mubuf_store_then_<tag> %gen5_mubuf_store_merge_<tag>
        %gen5_mubuf_store_then_<tag> = OpLabel
<address_setup>
        %gen5_mubuf_store_format_u_<tag> = OpShiftRightLogical %uint %buf_addr_desc3_<tag> %uint_12
        %gen5_mubuf_store_format_m_<tag> = OpBitwiseAnd %uint %gen5_mubuf_store_format_u_<tag> %uint_127
        %gen5_mubuf_store_format_i_<tag> = OpBitcast %int %gen5_mubuf_store_format_m_<tag>
               OpStore %temp_int_5 %gen5_mubuf_store_format_i_<tag>
%gen5_mubuf_store_<tag> = OpFunctionCall %void %<function><inputs> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
               OpBranch %gen5_mubuf_store_merge_<tag>
        %gen5_mubuf_store_merge_<tag> = OpLabel
)")
	                   .ReplaceStr("<address_setup>", address_setup.source)
	                   .ReplaceStr("<tag>", tag)
	                   .ReplaceStr("<function>", function_name)
	                   .ReplaceStr("<inputs>", inputs);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferLoadUbyte_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
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
			if (!emit_buffer_address_setup(spirv, inst, static_cast<int>(index), 0, BufferAddressRangeCheck::RawZeroRecord,
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
	const auto* bind_info = spirv->GetBindInfo();

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
	const auto* bind_info = spirv->GetBindInfo();

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
	const auto* bind_info = spirv->GetBindInfo();

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
	const auto* bind_info = spirv->GetBindInfo();

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
        %t192_<index> = OpLoad %uint %m0
        %t194_<index> = OpShiftRightLogical %uint %t192_<index> %int_16
        %t196_<index> = OpAccessChain %_ptr_StorageBuffer_uint %gds %int_0 %t194_<index>
        %t198_<index> = OpAtomicIAdd %uint %t196_<index> %uint_1 %uint_0 %uint_1
        %t199_<index> = OpBitcast %float %t198_<index>
               OpStore %<dst> %t199_<index>
               OpMemoryBarrier %uint_1 %uint_72
)";
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
        %t192_<index> = OpLoad %uint %m0
        %t194_<index> = OpShiftRightLogical %uint %t192_<index> %int_16
        %t196_<index> = OpAccessChain %_ptr_StorageBuffer_uint %gds %int_0 %t194_<index>
        %t198_<index> = OpAtomicISub %uint %t196_<index> %uint_1 %uint_0 %uint_1
        %t199_<index> = OpBitcast %float %t198_<index>
               OpStore %<dst> %t199_<index>
               OpMemoryBarrier %uint_1 %uint_72
)";
		*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_DsWriteB32_VaddrVdataOffset)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0)
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);
	auto data    = operand_variable_to_str(inst.src[1]);

	if (address.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: address.type != SpirvType::Float condition ignored (continuing)\n"); }
	if (data.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: data.type != SpirvType::Float condition ignored (continuing)\n"); }

	const auto index_str  = String8::FromPrintf("%u", index);
	const auto offset_str = spirv->GetConstantUint(inst.ds_offset);

	static const char* text = R"(
        %lds_addr_f_<index> = OpLoad %float %<address>
        %lds_addr_u_<index> = OpBitcast %uint %lds_addr_f_<index>
        %lds_byte_addr_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset>
        %lds_index_<index> = OpShiftRightLogical %uint %lds_byte_addr_<index> %uint_2
        %lds_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
        %lds_data_f_<index> = OpLoad %float %<data>
        %lds_data_u_<index> = OpBitcast %uint %lds_data_f_<index>
               OpStore %lds_ptr_<index> %lds_data_u_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<address>", address.value)
	                   .ReplaceStr("<data>", data.value)
	                   .ReplaceStr("<offset>", offset_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_DsAddU32_VaddrVdataOffset)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0)
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);
	auto data    = operand_variable_to_str(inst.src[1]);

	if (address.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: address.type != SpirvType::Float condition ignored (continuing)\n"); }
	if (data.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: data.type != SpirvType::Float condition ignored (continuing)\n"); }

	const auto index_str  = String8::FromPrintf("%u", index);
	const auto offset_str = spirv->GetConstantUint(inst.ds_offset);
	const auto scope_str  = spirv->GetConstantUint(2u);
	const auto semantics  = spirv->GetConstantUint(0x108u);

	static const char* text = R"(
        %lds_addr_f_<index> = OpLoad %float %<address>
        %lds_addr_u_<index> = OpBitcast %uint %lds_addr_f_<index>
        %lds_byte_addr_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset>
        %lds_index_<index> = OpShiftRightLogical %uint %lds_byte_addr_<index> %uint_2
        %lds_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
        %lds_data_f_<index> = OpLoad %float %<data>
        %lds_data_u_<index> = OpBitcast %uint %lds_data_f_<index>
        %lds_prior_<index> = OpAtomicIAdd %uint %lds_ptr_<index> %<scope> %<semantics> %lds_data_u_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<address>", address.value)
	                   .ReplaceStr("<data>", data.value)
	                   .ReplaceStr("<offset>", offset_str)
	                   .ReplaceStr("<scope>", scope_str)
	                   .ReplaceStr("<semantics>", semantics);
	return true;
}

/* Generalized LDS atomic with a data operand (sub/min/max/and/or/xor).
 * param[0] selects the SPIR-V atomic opcode. */
KYTY_RECOMPILER_FUNC(Recompile_DsAtomic_XXX_VaddrVdataOffset)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0)
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);
	auto data    = operand_variable_to_str(inst.src[1]);

	if (address.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: address.type != SpirvType::Float condition ignored (continuing)\n"); }
	if (data.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: data.type != SpirvType::Float condition ignored (continuing)\n"); }

	const auto index_str  = String8::FromPrintf("%u", index);
	const auto offset_str = spirv->GetConstantUint(inst.ds_offset);
	const auto scope_str  = spirv->GetConstantUint(2u);
	const auto semantics  = spirv->GetConstantUint(0x108u);

	static const char* text = R"(
        %lds_addr_f_<index> = OpLoad %float %<address>
        %lds_addr_u_<index> = OpBitcast %uint %lds_addr_f_<index>
        %lds_byte_addr_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset>
        %lds_index_<index> = OpShiftRightLogical %uint %lds_byte_addr_<index> %uint_2
        %lds_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
        %lds_data_f_<index> = OpLoad %float %<data>
        %lds_data_u_<index> = OpBitcast %uint %lds_data_f_<index>
        %lds_prior_<index> = <atomic_op> %uint %lds_ptr_<index> %<scope> %<semantics> %lds_data_u_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<address>", address.value)
	                   .ReplaceStr("<data>", data.value)
	                   .ReplaceStr("<offset>", offset_str)
	                   .ReplaceStr("<scope>", scope_str)
	                   .ReplaceStr("<semantics>", semantics)
	                   .ReplaceStr("<atomic_op>", param[0]);
	return true;
}

/* Generalized LDS atomic increment/decrement (no data operand).
 * param[0] selects the SPIR-V atomic opcode. */
KYTY_RECOMPILER_FUNC(Recompile_DsAtomicIncDec_VaddrOffset)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0)
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);

	if (address.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: address.type != SpirvType::Float condition ignored (continuing)\n"); }

	const auto index_str  = String8::FromPrintf("%u", index);
	const auto offset_str = spirv->GetConstantUint(inst.ds_offset);
	const auto scope_str  = spirv->GetConstantUint(2u);
	const auto semantics  = spirv->GetConstantUint(0x108u);

	static const char* text = R"(
        %lds_addr_f_<index> = OpLoad %float %<address>
        %lds_addr_u_<index> = OpBitcast %uint %lds_addr_f_<index>
        %lds_byte_addr_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset>
        %lds_index_<index> = OpShiftRightLogical %uint %lds_byte_addr_<index> %uint_2
        %lds_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
        %lds_prior_<index> = <atomic_op> %uint %lds_ptr_<index> %<scope> %<semantics>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<address>", address.value)
	                   .ReplaceStr("<offset>", offset_str)
	                   .ReplaceStr("<scope>", scope_str)
	                   .ReplaceStr("<semantics>", semantics)
	                   .ReplaceStr("<atomic_op>", param[0]);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_DsReadB32_VdstVaddrOffset)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0)
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);
	auto dst     = operand_variable_to_str(inst.dst);

	if (address.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: address.type != SpirvType::Float condition ignored (continuing)\n"); }
	if (dst.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst.type != SpirvType::Float condition ignored (continuing)\n"); }

	const auto index_str  = String8::FromPrintf("%u", index);
	const auto offset_str = spirv->GetConstantUint(inst.ds_offset);

	static const char* text = R"(
        %lds_addr_f_<index> = OpLoad %float %<address>
        %lds_addr_u_<index> = OpBitcast %uint %lds_addr_f_<index>
        %lds_byte_addr_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset>
        %lds_index_<index> = OpShiftRightLogical %uint %lds_byte_addr_<index> %uint_2
        %lds_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
        %lds_data_u_<index> = OpLoad %uint %lds_ptr_<index>
        %lds_data_f_<index> = OpBitcast %float %lds_data_u_<index>
               OpStore %<dst> %lds_data_f_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<address>", address.value)
	                   .ReplaceStr("<dst>", dst.value)
	                   .ReplaceStr("<offset>", offset_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_DsRead2B32_Vdst2VaddrOffset01)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0)
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);
	auto dst0    = operand_variable_to_str(inst.dst, 0);
	auto dst1    = operand_variable_to_str(inst.dst, 1);

	if (address.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: address.type != SpirvType::Float condition ignored (continuing)\n"); }
	if (dst0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst0.type != SpirvType::Float condition ignored (continuing)\n"); }
	if (dst1.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst1.type != SpirvType::Float condition ignored (continuing)\n"); }

	const uint32_t offset0       = inst.ds_offset & 0xffu;
	const uint32_t offset1       = (inst.ds_offset >> 8u) & 0xffu;
	const auto     index_str     = String8::FromPrintf("%u", index);
	const auto     offset0_bytes = spirv->GetConstantUint(offset0 * 4u);
	const auto     offset1_bytes = spirv->GetConstantUint(offset1 * 4u);

	// Same Workgroup byte-addressed storage as ds_write_b32; read2 offsets are
	// dword-scaled, so convert to byte offsets before the shared >>2 index path.
	static const char* text = R"(
        %lds_addr_f_<index> = OpLoad %float %<address>
        %lds_addr_u_<index> = OpBitcast %uint %lds_addr_f_<index>
        %lds_byte_addr0_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset0>
        %lds_index0_<index> = OpShiftRightLogical %uint %lds_byte_addr0_<index> %uint_2
        %lds_ptr0_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index0_<index>
        %lds_data0_u_<index> = OpLoad %uint %lds_ptr0_<index>
        %lds_data0_f_<index> = OpBitcast %float %lds_data0_u_<index>
               OpStore %<dst0> %lds_data0_f_<index>
        %lds_byte_addr1_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset1>
        %lds_index1_<index> = OpShiftRightLogical %uint %lds_byte_addr1_<index> %uint_2
        %lds_ptr1_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index1_<index>
        %lds_data1_u_<index> = OpLoad %uint %lds_ptr1_<index>
        %lds_data1_f_<index> = OpBitcast %float %lds_data1_u_<index>
               OpStore %<dst1> %lds_data1_f_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<address>", address.value)
	                   .ReplaceStr("<dst0>", dst0.value)
	                   .ReplaceStr("<dst1>", dst1.value)
	                   .ReplaceStr("<offset0>", offset0_bytes)
	                   .ReplaceStr("<offset1>", offset1_bytes);
	return true;
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

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx8_Sdst8SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
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
        %t102_<index> = OpBitcast %int %<offset>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_8 %<p0> %<p1> %<p2> %<p3> %<p4> %<p5> %<p6> %<p7> %temp_int_1 %temp_int_2
)";

		for (int i = 0; i < 8; i++)
		{
			text = text.ReplaceStr(String8::FromPrintf("<p%d>", i), dst_value[i].value);
		}

		*dst_source += text.ReplaceStr("<index>", String8::FromPrintf("%u", index))
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
        %t102_<index> = OpBitcast %int %<offset>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_16 %<p0> %<p1> %<p2> %<p3> %<p4> %<p5> %<p6> %<p7> %<p8> %<p9> %<p10> %<p11> %<p12> %<p13> %<p14> %<p15> %temp_int_1 %temp_int_2
)";

		for (int i = 0; i < 16; i++)
		{
			text = text.ReplaceStr(String8::FromPrintf("<p%d>", i), dst_value[i].value);
		}

		*dst_source += text.ReplaceStr("<index>", String8::FromPrintf("%u", index))
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

	if (dword_count <= 0 || dword_count > 8) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dword_count <= 0 || dword_count > 8 condition ignored (continuing)\n"); }
	if (inst.dst.size != dword_count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.dst.size != dword_count condition ignored (continuing)\n"); }

	SpirvValue dst_value[8];
	for (int i = 0; i < dword_count; i++)
	{
		dst_value[i] = (dword_count == 1 ? operand_variable_to_str(inst.dst) : operand_variable_to_str(inst.dst, i));
		if (dst_value[i].type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value[i].type != SpirvType::Uint condition ignored (continuing)\n"); }
	}

	auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
	auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
	int  offset      = static_cast<int>(inst.src[1].constant.u >> 2u);

	if (src0_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
	if (src0_value1.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value1.type != SpirvType::Uint condition ignored (continuing)\n"); }

	static const char* text = R"(
		         %vsharp_<index>_<reg> = OpAccessChain %<vsharp_uint_ptr> %vsharp %int_0 %int_<buffer> %int_<field>
		         %vsharp_<index>_value_<reg> = OpLoad %uint %vsharp_<index>_<reg>
		               OpStore %<reg> %vsharp_<index>_value_<reg>
				)";

	for (int i = 0; i < dword_count; i++)
	{
		int buffer = 0;
		int field  = 0;
		if (!spirv->GetDynamicSLoadMappedIndex(inst.pc, offset + i, &buffer, &field))
		{
			spirv->GetMappedIndex(offset + i, &buffer, &field);
		}

		*dst_source += String8(text)
		                   .ReplaceStr("<vsharp_uint_ptr>", bind_info->vsharp_uniform_buffer ? "_ptr_Uniform_uint" : "_ptr_PushConstant_uint")
		                   .ReplaceStr("<reg>", dst_value[i].value)
		                   .ReplaceStr("<buffer>", String8::FromPrintf("%d", buffer))
		                   .ReplaceStr("<field>", String8::FromPrintf("%d", field))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u_%d", index, i));
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

KYTY_RECOMPILER_FUNC(Recompile_SLoadDword_SdstSbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);

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

	if (recompile_sload_fetch_buffer_meta(inst, spirv))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 8);
}


KYTY_RECOMPILER_FUNC(Recompile_TBufferLoadFormatX_Vdata1VaddrSvSoffsIdxenFloat1)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_tbuffer_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_x", 36, 1, dst_source);
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
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_tbuffer_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_xyzw", 119, 4, dst_source);
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
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr || bind_info->storage_buffers.buffers_num == 0)
	{
		return false;
	}
	if (Config::IsNextGen())
	{
		return emit_gen5_tbuffer_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_xy", 64, 2, dst_source);
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
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		if (Config::IsNextGen())
		{
			return emit_gen5_tbuffer_load(spirv, inst, static_cast<int>(index), "tbuffer_load_format_xyzw", 119, 4, dst_source);
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
