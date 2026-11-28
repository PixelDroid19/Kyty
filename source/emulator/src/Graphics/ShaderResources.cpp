#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Log.h"

#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/ShaderScalarLiveness.h"
#include "ShaderControlFlowGraph.h"
#include "ShaderStorageAnalysis.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <climits>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

// Host bound on table offsets (16 KiB): snapshots cover only the dwords a shader loads,
// and guest tables keep descriptors after their constants.
constexpr uint32_t kMaxSrtPointerTableDwords = 4096u;

} // namespace

int ShaderFindImageSampledTextureDescriptor(const ShaderInstruction& inst, const ShaderBindResources& bind,
	                                        int user_data_register_base)
{
	if (inst.src_num < 2 || inst.src[1].type != ShaderOperandType::Sgpr || inst.src[1].size != 8)
	{
		return -1;
	}
	const int texture_register = inst.src[1].register_id;
	// A mapped load replaces the initial descriptor in this SGPR home.
	// Its recorded consumer lifetime takes precedence over the initial binding.
	for (uint32_t mapping = 0; mapping < bind.dynamic_sloads.records.Size(); ++mapping)
	{
		const auto& record = bind.dynamic_sloads.records.At(mapping);
		if (record.kind != ShaderDynamicSLoadResourceKind::Texture || record.destination_register != texture_register ||
		    inst.pc <= record.instruction_pc || inst.pc > record.last_consumer_pc)
		{
			continue;
		}
		const int index = record.resource_index;
		if (index >= 0 && index < bind.textures2D.textures_num && bind.textures2D.desc[index].usage == ShaderTextureUsage::ReadOnly)
		{
			return index;
		}
	}
	for (int index = 0; index < bind.textures2D.textures_num; ++index)
	{
		const auto& descriptor = bind.textures2D.desc[index];
		if (descriptor.usage == ShaderTextureUsage::ReadOnly && !descriptor.dynamic_sload &&
		    descriptor.start_register + user_data_register_base == texture_register)
		{
			return index;
		}
	}
	for (const auto& copied: bind.copied_image_descriptors)
	{
		if (copied.consumer_pc == inst.pc && copied.texture_index >= 0)
		{
			return copied.texture_index;
		}
	}
	// Every record of a heap read binds a texture of one class; the first stands for all.
	for (const auto& heap: bind.heap_textures)
	{
		if (heap.consumer_pc == inst.pc)
		{
			return heap.texture_index;
		}
	}
	return -1;
}

int ShaderFindImageSamplerDescriptor(const ShaderInstruction& inst, const ShaderBindResources& bind,
	                                 int user_data_register_base)
{
	if (inst.src_num < 3 || inst.src[2].type != ShaderOperandType::Sgpr || inst.src[2].size != 4)
	{
		return -1;
	}
	const int sampler_register = inst.src[2].register_id;
	// A mapped load replaces the initial descriptor in this SGPR home.
	// Its recorded consumer lifetime takes precedence over the initial binding.
	for (uint32_t mapping = 0; mapping < bind.dynamic_sloads.records.Size(); ++mapping)
	{
		const auto& record = bind.dynamic_sloads.records.At(mapping);
		if (record.kind != ShaderDynamicSLoadResourceKind::Sampler || record.destination_register != sampler_register ||
		    inst.pc <= record.instruction_pc || inst.pc > record.last_consumer_pc)
		{
			continue;
		}
		const int index = record.resource_index;
		if (index >= 0 && index < bind.samplers.samplers_num)
		{
			return index;
		}
	}
	for (int index = 0; index < bind.samplers.samplers_num; ++index)
	{
		if (!bind.samplers.dynamic_sload[index] && bind.samplers.start_register[index] + user_data_register_base == sampler_register)
		{
			return index;
		}
	}
	for (const auto& copied: bind.copied_image_descriptors)
	{
		if (copied.consumer_pc == inst.pc && copied.sampler_index >= 0)
		{
			return copied.sampler_index;
		}
	}
	for (const auto& constant: bind.constant_samplers)
	{
		if (constant.consumer_pc == inst.pc)
		{
			return constant.sampler_index;
		}
	}
	return -1;
}

void ShaderAssociateSampledTextureSamplers(const ShaderCode& code, ShaderBindResources* bind, int user_data_register_base,
                                           const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(bind == nullptr);
	ShaderCollectCopiedImageDescriptors(code, bind, user_data_register_base, memo);
	for (int index = 0; index < bind->textures2D.textures_num; ++index)
	{
		bind->textures2D.desc[index].sampler_indices_mask = 0u;
	}
	const ShaderInstructionIndices direct  = memo == nullptr ? ShaderIndexInstructions(code) : ShaderInstructionIndices {};
	const auto&                    indices = memo != nullptr ? memo->InstructionIndices(code) : direct;
	for (const uint32_t instruction: indices.image_samplers)
	{
		const auto& inst          = code.GetInstructions().At(instruction);
		const int texture_index = ShaderFindImageSampledTextureDescriptor(inst, *bind, user_data_register_base);
		const int sampler_index = ShaderFindImageSamplerDescriptor(inst, *bind, user_data_register_base);
		if (texture_index < 0 || sampler_index < 0 || sampler_index >= 16)
		{
			continue;
		}
		const auto sampler_bit = static_cast<uint16_t>(1u << sampler_index);
		bind->textures2D.desc[texture_index].sampler_indices_mask |= sampler_bit;
		// A heap read samples the texture of whichever record it reached.
		for (const auto& heap: bind->heap_textures)
		{
			if (heap.consumer_pc == inst.pc)
			{
				bind->textures2D.desc[heap.texture_index].sampler_indices_mask |= sampler_bit;
			}
		}
	}
}



bool Gen5HasEudPointer(const ShaderUserData* user_data)
{
	// An explicit EUD pointer can coexist with direct SRT data. The SRT size
	// describes a separate region; it does not turn this pointer into a V#.
	return user_data != nullptr && user_data->direct_resource_offset != nullptr && user_data->eud_size_dw != 0 &&
	       user_data->direct_resource_count > k_gen5_eud_direct_type && user_data->direct_resource_offset[k_gen5_eud_direct_type] != 0xffff;
}

void ShaderReportMissingGen5EudPointer(const ShaderUserData* user_data, int reg, int user_sgpr_num)
{
	static std::atomic_uint32_t reports {0};
	if (reports.fetch_add(1, std::memory_order_relaxed) >= 8u)
	{
		return;
	}
	KYTY_LOG_DEBUG( "KYTY_SHADER_EUD_NULL reg=%d user_sgpr=%d eud_dw=%u direct_count=%u\n", reg, user_sgpr_num,
	             user_data != nullptr ? user_data->eud_size_dw : 0u, user_data != nullptr ? user_data->direct_resource_count : 0u);
}

// The EUD sharp namespace begins at ABI slot 0x20. Wider user-SGPR windows can
// push that boundary forward, but a narrow window must not reinterpret
// S#@0x20 as an offset from slot 0x10 (14 → 16). Captured PS and CS layouts
// both map S#@0x20 to eud[0].
int ShaderGen5EudOffsetBase(int user_sgpr_num)
{
	if (user_sgpr_num <= 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: user_sgpr_num <= 0 condition ignored (continuing)\n"); }
	constexpr int abi_eud_offset_base = 0x20;
	const int     rounded_user_sgprs  = (user_sgpr_num + 3) & ~3;
	return (rounded_user_sgprs > abi_eud_offset_base ? rounded_user_sgprs : abi_eud_offset_base);
}

uint32_t ShaderResolveGen5UserSgprCount(uint32_t declared_count, uint32_t written_count, uint16_t eud_size_dw)
{
	// Some Gen5 compute packets leave USER_SGPR at zero while writing an EUD
	// pointer through COMPUTE_USER_DATA. EUD metadata is the evidence that the
	// written window is part of this shader's ABI; without it, zero remains zero.
	if (declared_count != 0 || eud_size_dw == 0)
	{
		return declared_count;
	}
	return written_count;
}

bool Gen5SharpNeedsEud(int offset_dw, int dwords, int user_sgpr_num)
{
	return offset_dw < 0 || offset_dw + dwords > user_sgpr_num;
}

// ShaderGet* extended path indexes extended_buffer[start - 16]. Remap a Gen5
// sharp offset so that eud[0] is addressed as start=16.
int Gen5EudApiIndex(int offset_dw, int user_sgpr_num)
{
	const int eud_base = ShaderGen5EudOffsetBase(user_sgpr_num);
	if (offset_dw < eud_base) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: offset_dw < eud_base condition ignored (continuing)\n"); }
	return 16 + (offset_dw - eud_base);
}

static uint32_t Gen5SharpUserSgprDword(int offset_dw, int user_sgpr_num, const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer)
{
	if (offset_dw < user_sgpr_num)
	{
		return user_sgpr.value[offset_dw];
	}
	if (extended_buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: extended_buffer == nullptr condition ignored (continuing)\n"); }
	const int eud_base = ShaderGen5EudOffsetBase(user_sgpr_num);
	if (offset_dw < eud_base) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: offset_dw < eud_base condition ignored (continuing)\n"); }
	return extended_buffer[offset_dw - eud_base];
}

// Gen5 texture type nibble: 8 = 1D, 9 = 2D, 10 = 3D, 12 = 1D array, 13 = 2D array. SizeFlag clear
// selects an 8-dword T#; other type values in this path are 4-dword V#.
static bool Gen5SharpIsImageDescriptor(int offset_dw, int user_sgpr_num, const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer)
{
	const uint32_t word3 = Gen5SharpUserSgprDword(offset_dw + 3, user_sgpr_num, user_sgpr, extended_buffer);
	const uint8_t  type  = static_cast<uint8_t>((word3 >> 28u) & 0xFu);
	return ShaderGen5TextureTypeIsImage(type);
}

bool Gen5SharpUseTextureDescriptor(bool size_flag, int offset_dw, int user_sgpr_num, const HW::UserSgprInfo& user_sgpr,
                                          const uint32_t* extended_buffer)
{
	return !size_flag && Gen5SharpIsImageDescriptor(offset_dw, user_sgpr_num, user_sgpr, extended_buffer);
}

bool Gen5CodeUnavailableDirectResourceLooksStorage(const HW::UserSgprInfo& user_sgpr, int reg)
{
	if (reg < 0 || reg + 3 >= HW::UserSgprInfo::SGPRS_MAX)
	{
		return false;
	}

	ShaderBufferResource resource;
	resource.fields[0] = user_sgpr.value[reg + 0];
	resource.fields[1] = user_sgpr.value[reg + 1];
	resource.fields[2] = user_sgpr.value[reg + 2];
	resource.fields[3] = user_sgpr.value[reg + 3];

	if (resource.Base48() == 0 && resource.NumRecords() == 0)
	{
		return false;
	}

	// Without instructions there is no evidence that this descriptor is a raw
	// byte-addressed resource. Keep that discovery path conservative so random
	// user SGPR data is not bound as a storage buffer.
	const bool conservative_raw = resource.Stride() != 0 && (resource.Stride() & 0x3u) == 0;
	return conservative_raw ||
	       ShaderGen5StorageDescriptorSupported(resource, ShaderStorageAccess::Typed);
}

bool ShaderInstructionIsScalarBufferLoad(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::SBufferLoadDword:
		case ShaderInstructionType::SBufferLoadDwordx2:
		case ShaderInstructionType::SBufferLoadDwordx4:
		case ShaderInstructionType::SBufferLoadDwordx8:
		case ShaderInstructionType::SBufferLoadDwordx16: return true;
		default: return false;
	}
}

static bool ShaderTryGetDwordOffset(const ShaderOperand& operand, int* offset_dw)
{
	EXIT_IF(offset_dw == nullptr);
	if (operand.type != ShaderOperandType::IntegerInlineConstant && operand.type != ShaderOperandType::LiteralConstant)
	{
		return false;
	}
	if ((operand.constant.u & 3u) != 0u || operand.constant.u > static_cast<uint32_t>(INT_MAX))
	{
		return false;
	}
	*offset_dw = static_cast<int>(operand.constant.u >> 2u);
	return true;
}

static bool ShaderGetSmemConstantDwordOffset(const ShaderInstruction& instruction, int* offset_dw)
{
	int source_offset_dw = 0;
	if (offset_dw == nullptr || instruction.src_num < 2 || !ShaderTryGetDwordOffset(instruction.src[1], &source_offset_dw))
	{
		return false;
	}
	const int64_t byte_offset = static_cast<int64_t>(source_offset_dw) * static_cast<int64_t>(sizeof(uint32_t)) +
	                            static_cast<int64_t>(instruction.smem_imm_offset);
	if (byte_offset < 0 || (byte_offset & (sizeof(uint32_t) - 1)) != 0 ||
	    byte_offset / static_cast<int64_t>(sizeof(uint32_t)) > INT_MAX)
	{
		return false;
	}
	*offset_dw = static_cast<int>(byte_offset / static_cast<int64_t>(sizeof(uint32_t)));
	return true;
}

// Materialize the entry table only while both shader pointer words retain
// the API pair's entry values on every incoming path.
static bool ShaderMatchesEudPointer(const ShaderOperand& source, int pointer_register, int user_data_register_base,
                                    const std::bitset<kShaderScalarLivenessSgprs>& entry_values)
{
	return source.type == ShaderOperandType::Sgpr && source.size == 2 && pointer_register >= 0 &&
	       user_data_register_base >= 0 && source.register_id >= user_data_register_base &&
	       source.register_id < kShaderScalarLivenessSgprs - 1 &&
	       source.register_id - user_data_register_base == pointer_register &&
	       entry_values.test(static_cast<size_t>(source.register_id)) &&
	       entry_values.test(static_cast<size_t>(source.register_id + 1));
}

static bool ShaderGen5EudAddRequiredSpan(uint32_t offset_dw, uint32_t dwords, uint32_t* required_end_dw)
{
	if (required_end_dw == nullptr || dwords == 0u || dwords > SHADER_GEN5_EUD_MAX_DWORDS ||
	    offset_dw > SHADER_GEN5_EUD_MAX_DWORDS - dwords)
	{
		return false;
	}
	const uint32_t end_dw = offset_dw + dwords;
	*required_end_dw = std::max(*required_end_dw, end_dw);
	return true;
}

static bool ShaderGen5EudAddSharpSpan(int offset_dw, int dwords, int user_sgpr_num, int eud_base,
                                      uint32_t* required_end_dw)
{
	if (offset_dw < 0 || dwords <= 0)
	{
		return false;
	}
	if (offset_dw <= user_sgpr_num - dwords)
	{
		return true;
	}
	// A descriptor may live wholly in user SGPRs or wholly in EUD. No guest
	// contract establishes a descriptor that straddles those two windows.
	if (offset_dw < eud_base)
	{
		return false;
	}
	return ShaderGen5EudAddRequiredSpan(static_cast<uint32_t>(offset_dw - eud_base), static_cast<uint32_t>(dwords),
	                                    required_end_dw);
}

bool ShaderGen5EudRequiredEndDwords(const ShaderUserData* user_data, int user_sgpr_num, int eud_pointer_register, const ShaderCode* code,
                                    int user_data_register_base, uint32_t* required_end_dw, const ShaderScalarFlow* scalar_flow,
                                    const ShaderCodeResourceMemo* memo)
{
	if (user_data == nullptr || required_end_dw == nullptr || user_sgpr_num < 0 ||
	    user_sgpr_num > HW::UserSgprInfo::SGPRS_MAX || eud_pointer_register < 0 ||
	    eud_pointer_register > user_sgpr_num - 2 || user_data_register_base < 0 || user_data->eud_size_dw == 0u)
	{
		return false;
	}

	uint32_t required = user_data->eud_size_dw;
	const int eud_base = ShaderGen5EudOffsetBase(user_sgpr_num);
	for (uint32_t category = 0; category < 4u; ++category)
	{
		const uint16_t count = user_data->sharp_resource_count[category];
		if (count == 0u)
		{
			continue;
		}
		// API slots can be empty or share a descriptor. Their uint16_t count
		// is not a count of distinct four-dword allocations; bound actual spans.
		if (user_data->sharp_resource_offset[category] == nullptr)
		{
			return false;
		}
		for (uint16_t slot = 0; slot < count; ++slot)
		{
			const auto& sharp = user_data->sharp_resource_offset[category][slot];
			if (sharp.offset_dw != 0x7fffu &&
			    !ShaderGen5EudAddSharpSpan(sharp.offset_dw, 4, user_sgpr_num, eud_base, &required))
			{
				return false;
			}
		}
	}

	if (code != nullptr)
	{
		std::shared_ptr<const ShaderScalarFlow> computed_flow;
		if (scalar_flow == nullptr)
		{
			computed_flow = ShaderScalarFlowOf(*code);
		}
		const auto* flow = scalar_flow != nullptr ? scalar_flow : computed_flow.get();
		const auto& entry_values = flow->holding_entry_value;
		const ShaderInstructionIndices direct       = memo == nullptr ? ShaderIndexInstructions(*code) : ShaderInstructionIndices {};
		const auto&                    indices      = memo != nullptr ? memo->InstructionIndices(*code) : direct;
		for (const uint32_t index: indices.descriptor_loads)
		{
			if (index >= entry_values.size())
			{
				break;
			}
			const auto& inst = code->GetInstructions().At(index);
			const int   dwords = inst.type == ShaderInstructionType::SLoadDwordx4 ? 4 : 8;
			if (inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != dwords || inst.src_num < 2 ||
			    !ShaderMatchesEudPointer(inst.src[0], eud_pointer_register, user_data_register_base, entry_values[index]))
			{
				continue;
			}
			int source_offset_dw = 0;
			if (ShaderGetSmemConstantDwordOffset(inst, &source_offset_dw) &&
			    !ShaderGen5EudAddRequiredSpan(static_cast<uint32_t>(source_offset_dw), static_cast<uint32_t>(dwords), &required))
			{
				return false;
			}
		}
	}

	*required_end_dw = required;
	return true;
}

bool ShaderGen5EudExpandEndDwordsForSharpImages(const ShaderUserData* user_data, int user_sgpr_num,
                                                 const uint32_t* eud_snapshot, uint32_t snapshot_dwords,
                                                 uint32_t* required_end_dw)
{
	if (user_data == nullptr || eud_snapshot == nullptr || required_end_dw == nullptr || user_sgpr_num < 0 ||
	    user_sgpr_num > HW::UserSgprInfo::SGPRS_MAX || snapshot_dwords == 0u ||
	    snapshot_dwords > SHADER_GEN5_EUD_MAX_DWORDS || *required_end_dw > snapshot_dwords)
	{
		return false;
	}

	uint32_t required = *required_end_dw;
	const int eud_base = ShaderGen5EudOffsetBase(user_sgpr_num);
	for (uint32_t category = 0; category < 2u; ++category)
	{
		const uint16_t count = user_data->sharp_resource_count[category];
		if (count == 0u)
		{
			continue;
		}
		if (user_data->sharp_resource_offset[category] == nullptr)
		{
			return false;
		}
		for (uint16_t slot = 0; slot < count; ++slot)
		{
			const auto& sharp = user_data->sharp_resource_offset[category][slot];
			if (sharp.offset_dw == 0x7fffu || sharp.size != 0u || sharp.offset_dw <= user_sgpr_num - 4)
			{
				continue;
			}
			if (sharp.offset_dw < eud_base)
			{
				return false;
			}
			const uint32_t table_offset = static_cast<uint32_t>(sharp.offset_dw - eud_base);
			if (table_offset > snapshot_dwords || snapshot_dwords - table_offset < 4u)
			{
				return false;
			}
			const uint8_t type = static_cast<uint8_t>((eud_snapshot[table_offset + 3u] >> 28u) & 0xfu);
			const bool is_image = ShaderGen5TextureTypeIsImage(type);
			if (is_image && !ShaderGen5EudAddRequiredSpan(table_offset, 8u, &required))
			{
				return false;
			}
		}
	}

	*required_end_dw = required;
	return true;
}

static bool ShaderInstructionReadsSgprRange(const ShaderInstruction& inst, int start_register, int registers_num)
{
	for (int source = 0; source < inst.src_num; ++source)
	{
		if (ShaderOperandOverlapsSgprRange(inst.src[source], start_register, registers_num))
		{
			return true;
		}
	}
	for (int source = 0; source < inst.mimg_address_num; ++source)
	{
		if (ShaderOperandOverlapsSgprRange(inst.mimg_address[source], start_register, registers_num))
		{
			return true;
		}
	}
	return false;
}

static bool ShaderInstructionWritesSgprRange(const ShaderInstruction& inst, int start_register, int registers_num)
{
	return ShaderOperandOverlapsSgprRange(inst.dst, start_register, registers_num) ||
	       ShaderOperandOverlapsSgprRange(inst.dst2, start_register, registers_num);
}

// The mapping scan accepts only consumers between the load and its last
// consumer, so every reader of the destination range in that window is one.
bool ShaderDynamicSLoadScalarSpan(const ShaderCode& code, const ShaderDynamicSLoadMapping& mapping, uint64_t* required_bytes)
{
	EXIT_IF(required_bytes == nullptr);
	bool dynamic_offset = false;
	bool found          = false;
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.pc <= mapping.instruction_pc || inst.pc > mapping.last_consumer_pc ||
		    !ShaderInstructionReadsSgprRange(inst, mapping.destination_register, mapping.dword_count))
		{
			continue;
		}
		const bool scalar_consumer = ShaderInstructionIsScalarBufferLoad(inst) && inst.src[0].type == ShaderOperandType::Sgpr &&
		                             inst.src[0].register_id == mapping.destination_register;
		if (!scalar_consumer)
		{
			return false;
		}
		found = true;
		ShaderAccumulateScalarBufferLoadSpan(inst, required_bytes, &dynamic_offset);
	}
	return found && !dynamic_offset;
}

ShaderDynamicSLoadSpan ShaderCodeResourceMemo::DynamicSLoadScalarSpan(const ShaderCode&                code,
                                                                     const ShaderDynamicSLoadMapping& mapping) const
{
	const auto key = std::make_tuple(mapping.instruction_pc, mapping.last_consumer_pc, mapping.destination_register, mapping.dword_count);
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		if (const auto it = m_dynamic_sload_spans.find(key); it != m_dynamic_sload_spans.end())
		{
			return it->second;
		}
	}
	ShaderDynamicSLoadSpan span {};
	span.exact = ShaderDynamicSLoadScalarSpan(code, mapping, &span.required_bytes);
	std::unique_lock<std::shared_mutex> lock(m_mutex);
	m_dynamic_sload_spans.emplace(key, span);
	return span;
}

ShaderDynamicSLoadSpan AnalyzeShaderDynamicSLoadSpan(const ShaderCode& code, const ShaderDynamicSLoadMapping& mapping,
                                                     const ShaderCodeResourceMemo* memo)
{
	if (memo != nullptr)
	{
		return memo->DynamicSLoadScalarSpan(code, mapping);
	}
	ShaderDynamicSLoadSpan span {};
	span.exact = ShaderDynamicSLoadScalarSpan(code, mapping, &span.required_bytes);
	return span;
}

static bool ShaderStorageResourcesEqual(const ShaderBufferResource& first, const ShaderBufferResource& second)
{
	for (int field = 0; field < 4; ++field)
	{
		if (first.fields[field] != second.fields[field])
		{
			return false;
		}
	}
	return true;
}

static bool ShaderTextureResourcesEqual(const ShaderTextureResource& first, const ShaderTextureResource& second)
{
	for (int field = 0; field < 8; ++field)
	{
		if (first.fields[field] != second.fields[field])
		{
			return false;
		}
	}
	return true;
}

// A loaded T# shares a resource only with one read from the same table words. Merging
// equal descriptors from different words would make the resource layout, and with it
// the shader identity, depend on the guest's data instead of its program.
static bool ShaderTextureBindingMatches(const ShaderTextureDescriptor& descriptor, ShaderTextureUsage usage,
                                        State::ImageSampleOperation operation, const ShaderTextureResource& resource)
{
	return descriptor.usage == usage && descriptor.sample_operation == operation && ShaderTextureResourcesEqual(descriptor.texture, resource);
}

static bool ShaderSamplerResourcesEqual(const ShaderSamplerResource& first, const ShaderSamplerResource& second)
{
	for (int field = 0; field < 4; ++field)
	{
		if (first.fields[field] != second.fields[field])
		{
			return false;
		}
	}
	return true;
}

static bool ShaderAddDynamicSLoadMapping(ShaderDynamicSLoadMappings* mappings, ShaderDynamicSLoadResourceKind kind, int resource_index,
                                         const ShaderInstruction& sload, int offset_dw, int dword_count, int resource_field_offset,
                                         uint32_t last_consumer_pc, bool raw_vmem_oob_guarded, uint32_t instruction_count)
{
	EXIT_IF(mappings == nullptr);
	if (sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != dword_count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != dword_count condition ignored (continuing)\n"); }
	auto& records = mappings->records;
	for (uint32_t mapping = 0; mapping < records.Size(); ++mapping)
	{
		// One load can hold several descriptors: records of one PC must cover
		// disjoint dword ranges, and an equal record is the same mapping again.
		const auto& existing = records.At(mapping);
		if (existing.instruction_pc != sload.pc)
		{
			continue;
		}
		const bool disjoint = offset_dw + dword_count <= existing.offset_dw || existing.offset_dw + existing.dword_count <= offset_dw;
		if (disjoint)
		{
			continue;
		}
		const bool same = existing.kind == kind && existing.resource_index == resource_index && existing.offset_dw == offset_dw &&
		                  existing.dword_count == dword_count && existing.resource_field_offset == resource_field_offset &&
		                  existing.raw_vmem_oob_guarded == raw_vmem_oob_guarded;
		if (same && last_consumer_pc > existing.last_consumer_pc)
		{
			records[mapping].last_consumer_pc = last_consumer_pc;
		}
		return same;
	}
	if (records.Size() >= instruction_count)
	{
		return false;
	}

	ShaderDynamicSLoadMapping record {};
	record.kind                  = kind;
	record.resource_index        = resource_index;
	record.destination_register  = sload.dst.register_id;
	record.instruction_pc        = sload.pc;
	record.offset_dw             = offset_dw;
	record.dword_count           = dword_count;
	record.resource_field_offset = resource_field_offset;
	record.last_consumer_pc      = last_consumer_pc;
	record.raw_vmem_oob_guarded  = raw_vmem_oob_guarded;
	records.Add(record);
	return true;
}

static void ShaderCountStorageUsage(ShaderParsedUsage* info, ShaderStorageUsage usage, int delta)
{
	switch (usage)
	{
		case ShaderStorageUsage::Constant: info->storage_buffers_constant += delta; break;
		case ShaderStorageUsage::ReadOnly: info->storage_buffers_readonly += delta; break;
		case ShaderStorageUsage::ReadWrite: info->storage_buffers_readwrite += delta; break;
		default: break;
	}
}

static bool ShaderAddDynamicScalarStorageResource(ShaderBindResources* bind, ShaderParsedUsage* info,
	                                               const ShaderInstruction& sload, int offset_dw, ShaderStorageUsage usage,
	                                               uint32_t last_consumer_pc, bool raw_vmem_oob_guarded,
	                                               const uint32_t* extended_buffer, uint32_t instruction_count,
	                                               bool* added_resource)
{
	EXIT_IF(bind == nullptr || info == nullptr || extended_buffer == nullptr || added_resource == nullptr);
	if (sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != 4) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != 4 condition ignored (continuing)\n"); }
	*added_resource = false;

	auto& resources = bind->storage_buffers;

	ShaderBufferResource resource {};
	for (int field = 0; field < 4; ++field)
	{
		resource.fields[field] = extended_buffer[offset_dw + field];
	}
	// Null EUD slots are common (unused sharp left zeroed). Static metadata rejects
	// them and lowers S_BUFFER via zero_sbuffer. Dynamic S_LOAD consumers still need
	// a mapping so the S_LOAD rewrites and the later AlwaysOutOfBounds path register
	// zero_sbuffer for the destination; PrepareStorageBuffers binds an empty carrier.

	int storage_index = -1;
	for (int index = 0; index < resources.buffers_num; ++index)
	{
		if (ShaderStorageResourcesEqual(resources.buffers[index], resource))
		{
			storage_index = index;
			break;
		}
	}
	if (storage_index < 0)
	{
		if (resources.buffers_num >= ShaderStorageResources::BUFFERS_MAX)
		{
			return false;
		}
		storage_index                           = resources.buffers_num++;
		resources.buffers[storage_index]        = resource;
		resources.usages[storage_index]         = usage;
		resources.sources[storage_index]        = ShaderStorageBindingSource::DynamicScalarLoad;
		resources.slots[storage_index]          = offset_dw;
		resources.start_register[storage_index] = sload.dst.register_id;
		resources.extended[storage_index]       = false;
		resources.dynamic_sload[storage_index]  = true;
		*added_resource                         = true;
		ShaderCountStorageUsage(info, usage, 1);
	} else if (usage == ShaderStorageUsage::ReadWrite && resources.usages[storage_index] != ShaderStorageUsage::ReadWrite)
	{
		// Equal descriptors can be loaded at several PCs. A later store must
		// retain writable backing even when the first consumer only read it.
		ShaderCountStorageUsage(info, resources.usages[storage_index], -1);
		resources.usages[storage_index] = ShaderStorageUsage::ReadWrite;
		ShaderCountStorageUsage(info, ShaderStorageUsage::ReadWrite, 1);
	}

	return ShaderAddDynamicSLoadMapping(&bind->dynamic_sloads, ShaderDynamicSLoadResourceKind::StorageBuffer, storage_index, sload,
	                                    offset_dw, 4, 0, last_consumer_pc, raw_vmem_oob_guarded, instruction_count);
}

static bool ShaderAddDynamicTextureResource(ShaderBindResources* bind, const ShaderInstruction& sload, int offset_dw,
	                                         uint32_t last_consumer_pc, ShaderTextureUsage usage,
	                                         State::ImageSampleOperation operation,
	                                         ShaderGen5SampledTextureShape sampled_shape, bool sampled_shape_known,
	                                         const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer,
	                                         uint32_t instruction_count, bool* added_resource)
{
	EXIT_IF(bind == nullptr || extended_buffer == nullptr || added_resource == nullptr);
	if (sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != 8 || usage == ShaderTextureUsage::Unknown) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != 8 || usage == ShaderTextureUsage::Unknown condition ignored (continuing)\n"); }
	*added_resource = false;

	ShaderTextureResource resource {};
	for (int field = 0; field < 8; ++field)
	{
		resource.fields[field] = extended_buffer[offset_dw + field];
	}
	// A loaded descriptor of buffer type binds no image; its reads return zero
	// like a user-data one (see CollectZeroImageResources).
	// The ISA defines an all-zero resource as unbound: its stores are discarded.
	const bool unbound_store = usage == ShaderTextureUsage::ReadWrite &&
	                           std::all_of(std::begin(resource.fields), std::end(resource.fields), [](uint32_t word) { return word == 0; });
	if ((usage == ShaderTextureUsage::ReadOnly && !ShaderGen5TextureTypeIsImage(resource.Type())) || unbound_store)
	{
		ShaderAddLoadedZeroImageResource(&bind->zero_image_resources, sload.dst.register_id, sload.pc, last_consumer_pc);
		ShaderAddZeroDescriptorLoad(&bind->zero_descriptor_loads, sload.pc, sload.dst.register_id, 8);
		return true;
	}
	operation = ShaderTextureSampleOperation(resource, operation);

	sampled_shape_known = sampled_shape_known && ShaderGen5InstructionShapeAppliesToType(resource.Type(), sampled_shape);
	int texture_index = -1;
	for (int index = 0; index < bind->textures2D.textures_num; ++index)
	{
		const auto& descriptor = bind->textures2D.desc[index];
		const bool  shape_matches = usage != ShaderTextureUsage::ReadOnly || !sampled_shape_known ||
		                            ShaderResolvedSampledTextureShape(descriptor) == sampled_shape;
		// Extended descriptors, static or loaded, name their EUD words by start register.
		const bool same_words = descriptor.extended && descriptor.start_register == offset_dw + 16;
		if (shape_matches && same_words && ShaderTextureBindingMatches(descriptor, usage, operation, resource))
		{
			texture_index = index;
			break;
		}
	}
	if (texture_index < 0)
	{
		if (bind->textures2D.textures_num >= ShaderTextureResources::RES_MAX)
		{
			return false;
		}
		texture_index = bind->textures2D.textures_num;
		ShaderGetTextureBuffer(&bind->textures2D, nullptr, offset_dw + 16, offset_dw, usage, user_sgpr, extended_buffer);
		bind->textures2D.desc[texture_index].sample_operation = operation;
		bind->textures2D.desc[texture_index].dynamic_sload = true;
		if (usage == ShaderTextureUsage::ReadOnly && sampled_shape_known)
		{
			ShaderSetSampledTextureInstructionShape(&bind->textures2D, texture_index, sampled_shape);
		}
		*added_resource = true;
	}

	return ShaderAddDynamicSLoadMapping(&bind->dynamic_sloads, ShaderDynamicSLoadResourceKind::Texture, texture_index, sload, offset_dw, 8,
	                                    0, last_consumer_pc, false, instruction_count);
}

static bool ShaderAddDynamicSamplerResource(ShaderBindResources* bind, const ShaderInstruction& sload, int offset_dw,
	                                         uint32_t last_consumer_pc, State::ImageSampleOperation operation,
	                                         const HW::UserSgprInfo& user_sgpr,
	                                         const uint32_t* extended_buffer, uint32_t instruction_count,
	                                         bool* added_resource)
{
	EXIT_IF(bind == nullptr || extended_buffer == nullptr || added_resource == nullptr);
	if (sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != 4) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != 4 condition ignored (continuing)\n"); }
	*added_resource = false;

	ShaderSamplerResource resource {};
	for (int field = 0; field < 4; ++field)
	{
		resource.fields[field] = extended_buffer[offset_dw + field];
	}

	int sampler_index = -1;
	for (int index = 0; index < bind->samplers.samplers_num; ++index)
	{
		if (ShaderSamplerResourcesEqual(bind->samplers.samplers[index], resource) && bind->samplers.operations[index] == operation)
		{
			sampler_index = index;
			break;
		}
	}
	if (sampler_index < 0)
	{
		if (bind->samplers.samplers_num >= ShaderSamplerResources::RES_MAX)
		{
			return false;
		}
		sampler_index = bind->samplers.samplers_num;
		ShaderGetSampler(&bind->samplers, nullptr, offset_dw + 16, offset_dw, user_sgpr, extended_buffer);
		bind->samplers.operations[sampler_index]    = operation;
		bind->samplers.dynamic_sload[sampler_index] = true;
		*added_resource                             = true;
	}

	return ShaderAddDynamicSLoadMapping(&bind->dynamic_sloads, ShaderDynamicSLoadResourceKind::Sampler, sampler_index, sload, offset_dw, 4,
	                                    0, last_consumer_pc, false, instruction_count);
}

struct ShaderSplitTextureLoad
{
	const ShaderInstruction* instruction = nullptr;
	int                      offset_dw   = 0;
};

static bool ShaderTryGetExtendedLoadOffset(const ShaderInstruction& load, const ShaderBindResources& bind, int dword_count,
                                           uint16_t eud_size_dw, int user_data_register_base,
                                           const std::bitset<kShaderScalarLivenessSgprs>& entry_values, int* offset_dw)
{
	if (offset_dw == nullptr || load.dst.type != ShaderOperandType::Sgpr || load.dst.size != dword_count || load.src_num < 2)
	{
		return false;
	}
	if (!ShaderMatchesEudPointer(load.src[0], bind.extended.start_register, user_data_register_base, entry_values))
	{
		return false;
	}
	if (!ShaderGetSmemConstantDwordOffset(load, offset_dw) || *offset_dw < 0)
	{
		return false;
	}
	const uint32_t span_dw = static_cast<uint32_t>(dword_count);
	return span_dw <= SHADER_GEN5_EUD_MAX_DWORDS &&
	       static_cast<uint32_t>(*offset_dw) <= SHADER_GEN5_EUD_MAX_DWORDS - span_dw &&
	       ShaderGen5EudSpanAllowed(16 + *offset_dw, dword_count, eud_size_dw);
}

// Walks back from an image consumer to the two S_LOAD_DWORDX4 that write the
// halves of its T#. Any other writer of the T#, a second write of one half or
// a block boundary first means the descriptor is not a split EUD load.
static bool ShaderFindSplitTextureHalves(const ShaderCode& code, uint32_t consumer_index, ShaderSplitTextureCandidate* split)
{
	const auto& consumer = code.GetInstructions().At(consumer_index);
	if (consumer.src_num < 2 || consumer.src[1].type != ShaderOperandType::Sgpr || consumer.src[1].size != 8)
	{
		return false;
	}
	const int descriptor_register = consumer.src[1].register_id;
	bool      low_found           = false;
	bool      high_found          = false;
	for (uint32_t cursor = consumer_index; cursor-- > 0;)
	{
		const auto& candidate = code.GetInstructions().At(cursor);
		if (candidate.type == ShaderInstructionType::Unknown || candidate.type == ShaderInstructionType::SEndpgm ||
		    candidate.type == ShaderInstructionType::SSetpcB64 || ShaderInstructionHasStaticBranchTarget(candidate.type))
		{
			break;
		}
		if (!ShaderInstructionWritesSgprRange(candidate, descriptor_register, 8))
		{
			continue;
		}
		const bool low_half  = candidate.dst.register_id == descriptor_register;
		const bool high_half = candidate.dst.register_id == descriptor_register + 4;
		if (candidate.type != ShaderInstructionType::SLoadDwordx4 || (!low_half && !high_half) || (low_half && low_found) ||
		    (high_half && high_found))
		{
			return false;
		}
		(low_half ? split->low : split->high) = cursor;
		low_found                             = low_found || low_half;
		high_found                            = high_found || high_half;
		if (low_found && high_found)
		{
			split->consumer = consumer_index;
			return true;
		}
	}
	return false;
}

// One half of a split T#, read from the EUD table at draw time.
static bool ShaderTryGetSplitTextureHalf(const ShaderCode& code, uint32_t index, const ShaderBindResources& bind, uint16_t eud_size_dw,
                                         int user_data_register_base,
                                         const std::vector<std::bitset<kShaderScalarLivenessSgprs>>& entry_values,
                                         ShaderSplitTextureLoad* half)
{
	const auto& load = code.GetInstructions().At(index);
	if (index >= entry_values.size() ||
	    !ShaderTryGetExtendedLoadOffset(load, bind, 4, eud_size_dw, user_data_register_base, entry_values[index], &half->offset_dw))
	{
		return false;
	}
	half->instruction = &load;
	return true;
}

static int ShaderFindTextureResource(const ShaderBindResources& bind, const ShaderTextureResource& resource, int slot,
                                     ShaderTextureUsage usage, State::ImageSampleOperation operation, ShaderGen5SampledTextureShape shape,
                                     bool shape_known)
{
	for (int index = 0; index < bind.textures2D.textures_num; ++index)
	{
		const auto& descriptor = bind.textures2D.desc[index];
		const bool  shape_matches =
		    usage != ShaderTextureUsage::ReadOnly || !shape_known || ShaderResolvedSampledTextureShape(descriptor) == shape;
		const bool  same_words = descriptor.extended ? descriptor.start_register == slot + 16
		                                             : descriptor.dynamic_sload && descriptor.slot == slot;
		if (shape_matches && same_words && ShaderTextureBindingMatches(descriptor, usage, operation, resource))
		{
			return index;
		}
	}
	return -1;
}

static int ShaderAddSplitTextureResource(ShaderBindResources* bind, const ShaderTextureResource& resource, int destination_register,
                                         int slot, ShaderTextureUsage usage, State::ImageSampleOperation operation,
                                         ShaderGen5SampledTextureShape shape, bool shape_known, bool* added_resource)
{
	EXIT_IF(bind == nullptr);
	EXIT_IF(added_resource == nullptr);
	*added_resource    = false;
	operation          = ShaderTextureSampleOperation(resource, operation);
	const int existing = ShaderFindTextureResource(*bind, resource, slot, usage, operation, shape, shape_known);
	if (existing >= 0)
	{
		return existing;
	}
	if (bind->textures2D.textures_num >= ShaderTextureResources::RES_MAX)
	{
		return -1;
	}
	*added_resource                       = true;
	const int index                       = bind->textures2D.textures_num++;
	auto&     descriptor                  = bind->textures2D.desc[index];
	descriptor.texture                    = resource;
	descriptor.usage                      = usage;
	descriptor.sample_operation           = operation;
	descriptor.slot                       = slot;
	descriptor.start_register             = destination_register;
	descriptor.extended                   = false;
	descriptor.dynamic_sload              = true;
	descriptor.textures2d_without_sampler = usage == ShaderTextureUsage::ReadWrite;
	if (shape_known)
	{
		descriptor.sampled_shape                  = shape;
		descriptor.sampled_shape_from_instruction = true;
	}
	if (usage == ShaderTextureUsage::ReadWrite)
	{
		bind->textures2D.textures2d_storage_num++;
		return index;
	}
	switch (ShaderResolvedSampledTextureShape(descriptor))
	{
		case ShaderGen5SampledTextureShape::TwoDimensional: bind->textures2D.textures2d_sampled_num++; break;
		case ShaderGen5SampledTextureShape::TwoDimensionalArray: bind->textures2D.textures2d_array_sampled_num++; break;
		case ShaderGen5SampledTextureShape::ThreeDimensional: bind->textures2D.textures3d_sampled_num++; break;
	}
	return index;
}

static void ShaderCollectSplitTextureResources(const ShaderCode& code, ShaderBindResources* bind, ShaderParsedUsage* info,
                                               const uint32_t* extended_buffer, uint16_t eud_size_dw, int user_data_register_base,
                                               const std::vector<std::bitset<kShaderScalarLivenessSgprs>>& entry_values,
                                               const std::vector<ShaderSplitTextureCandidate>& splits)
{
	EXIT_IF(bind == nullptr || info == nullptr || extended_buffer == nullptr);
	const uint32_t instruction_count = code.GetInstructions().Size();
	for (const auto& split: splits)
	{
		const auto& consumer = code.GetInstructions().At(split.consumer);
		const bool  reads    = ShaderInstructionReadsImageResource(consumer.type);
		const bool  writes   = ShaderInstructionWritesImageResource(consumer.type);
		ShaderSplitTextureLoad low {};
		ShaderSplitTextureLoad high {};
		if (!ShaderTryGetSplitTextureHalf(code, split.low, *bind, eud_size_dw, user_data_register_base, entry_values, &low) ||
		    !ShaderTryGetSplitTextureHalf(code, split.high, *bind, eud_size_dw, user_data_register_base, entry_values, &high))
		{
			continue;
		}
		ShaderTextureResource resource {};
		for (int field = 0; field < 4; ++field)
		{
			resource.fields[field]     = extended_buffer[low.offset_dw + field];
			resource.fields[field + 4] = extended_buffer[high.offset_dw + field];
		}
		const auto usage     = writes ? ShaderTextureUsage::ReadWrite : ShaderTextureUsage::ReadOnly;
		const auto operation = ShaderInstructionUsesImageSampler(consumer.type) ? ShaderInstructionSamplerOperation(consumer.type)
		                                                                        : State::ImageSampleOperation::Regular;
		ShaderGen5SampledTextureShape shape {};
		const bool                    shape_known = reads && ShaderGen5SampledTextureShapeForMimgDimension(consumer.mimg_dimension, &shape) &&
		                                            ShaderGen5InstructionShapeAppliesToType(resource.Type(), shape);
		bool                          added_resource = false;
		const int resource_index = ShaderAddSplitTextureResource(bind, resource, consumer.src[1].register_id, low.offset_dw, usage,
		                                                         operation, shape, shape_known, &added_resource);
		if (resource_index < 0)
		{
			continue;
		}
		const bool low_added  = ShaderAddDynamicSLoadMapping(&bind->dynamic_sloads, ShaderDynamicSLoadResourceKind::Texture, resource_index,
		                                                     *low.instruction, low.offset_dw, 4, 0, consumer.pc, false,
		                                                     instruction_count);
		const bool high_added = ShaderAddDynamicSLoadMapping(&bind->dynamic_sloads, ShaderDynamicSLoadResourceKind::Texture, resource_index,
		                                                     *high.instruction, high.offset_dw, 4, 4, consumer.pc, false,
		                                                     instruction_count);
		if (!low_added || !high_added)
		{
			EXIT("unable to materialize split dynamic image descriptor: pc=0x%08" PRIx32 " dst=%d low=%d high=%d\n", consumer.pc,
			     consumer.src[1].register_id, low.offset_dw, high.offset_dw);
		}
		if (!added_resource)
		{
			continue;
		}
		if (usage == ShaderTextureUsage::ReadWrite)
		{
			info->textures2D_readwrite++;
		} else
		{
			info->textures2D_readonly++;
		}
	}
}

static bool ShaderInstructionWritesVectorBufferDescriptor(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::BufferStoreDword:
		case ShaderInstructionType::BufferStoreDwordx2:
		case ShaderInstructionType::BufferStoreDwordx3:
		case ShaderInstructionType::BufferStoreDwordx4:
		case ShaderInstructionType::BufferStoreFormatX:
		case ShaderInstructionType::BufferStoreFormatXy:
		case ShaderInstructionType::BufferStoreFormatXyzw:
		case ShaderInstructionType::BufferAtomicAdd:
		case ShaderInstructionType::BufferAtomicAnd:
		case ShaderInstructionType::BufferAtomicOr:
		case ShaderInstructionType::BufferAtomicSmax:
		case ShaderInstructionType::BufferAtomicSmin:
		case ShaderInstructionType::BufferAtomicSub:
		case ShaderInstructionType::BufferAtomicUmax:
		case ShaderInstructionType::BufferAtomicUmin:
		case ShaderInstructionType::BufferAtomicXor:
		case ShaderInstructionType::BufferAtomicSwap: return true;
		default: return false;
	}
}

static bool ShaderInstructionUsesVectorBufferDescriptor(ShaderInstructionType type)
{
	if (ShaderInstructionWritesVectorBufferDescriptor(type))
	{
		return true;
	}
	switch (type)
	{
		case ShaderInstructionType::BufferLoadUbyte:
		case ShaderInstructionType::BufferLoadDword:
		case ShaderInstructionType::BufferLoadDwordx2:
		case ShaderInstructionType::BufferLoadDwordx3:
		case ShaderInstructionType::BufferLoadDwordx4:
		case ShaderInstructionType::BufferLoadFormatX:
		case ShaderInstructionType::BufferLoadFormatXy:
		case ShaderInstructionType::BufferLoadFormatXyz:
		case ShaderInstructionType::BufferLoadFormatXyzw:
		case ShaderInstructionType::TBufferLoadFormatX:
		case ShaderInstructionType::TBufferLoadFormatXy:
		case ShaderInstructionType::TBufferLoadFormatXyz:
		case ShaderInstructionType::TBufferLoadFormatXyzw: return true;
		default: return false;
	}
}

static bool ShaderDynamicSLoadMatchesConsumer(const ShaderInstruction& inst, const ShaderInstruction& sload,
	                                           ShaderDynamicSLoadUse* use)
{
	EXIT_IF(use == nullptr);
	const int destination = sload.dst.register_id;
	if (sload.dst.size == 4)
	{
		if (ShaderInstructionIsScalarBufferLoad(inst) && inst.src_num > 0 && inst.src[0].type == ShaderOperandType::Sgpr &&
		    inst.src[0].register_id == destination && inst.src[0].size == 4)
		{
			for (int source = 1; source < inst.src_num; ++source)
			{
				if (ShaderOperandOverlapsSgprRange(inst.src[source], destination, 4))
				{
					return false;
				}
			}
			use->kind             = ShaderDynamicSLoadResourceKind::StorageBuffer;
			use->texture_usage    = ShaderTextureUsage::Unknown;
			use->raw_smem_use     = true;
			use->raw_only         = true;
			use->dword_loads_only = true;
			return true;
		}
		if (ShaderInstructionUsesVectorBufferDescriptor(inst.type) && inst.src_num >= 2 &&
		    inst.src[1].type == ShaderOperandType::Sgpr && inst.src[1].register_id == destination && inst.src[1].size == 4)
		{
			for (int source = 0; source < inst.src_num; ++source)
			{
				if (source != 1 && ShaderOperandOverlapsSgprRange(inst.src[source], destination, 4))
				{
					return false;
				}
			}
			const bool is_dword_load =
			    inst.type == ShaderInstructionType::BufferLoadDword || inst.type == ShaderInstructionType::BufferLoadDwordx2 ||
			    inst.type == ShaderInstructionType::BufferLoadDwordx3 || inst.type == ShaderInstructionType::BufferLoadDwordx4;
			const bool is_raw =
			    is_dword_load || inst.type == ShaderInstructionType::BufferLoadUbyte ||
			    inst.type == ShaderInstructionType::BufferStoreDword || inst.type == ShaderInstructionType::BufferStoreDwordx2 ||
			    inst.type == ShaderInstructionType::BufferStoreDwordx3 || inst.type == ShaderInstructionType::BufferStoreDwordx4 ||
			    ShaderInstructionTypeStartsWith(inst.type, "BufferAtomic");
			use->kind                   = ShaderDynamicSLoadResourceKind::StorageBuffer;
			use->storage_usage = ShaderInstructionWritesVectorBufferDescriptor(inst.type) ? ShaderStorageUsage::ReadWrite
			                                                                              : ShaderStorageUsage::ReadOnly;
			use->texture_usage          = ShaderTextureUsage::Unknown;
			use->raw_vmem_oob_guarded = true;
			use->raw_only             = is_raw;
			use->dword_loads_only     = is_dword_load;
			return true;
		}
		if (ShaderInstructionUsesImageSampler(inst.type) && inst.src_num >= 3 && inst.src[2].type == ShaderOperandType::Sgpr &&
		    inst.src[2].register_id == destination && inst.src[2].size == 4)
		{
			use->kind              = ShaderDynamicSLoadResourceKind::Sampler;
			use->texture_usage     = ShaderTextureUsage::Unknown;
			use->sampler_operation = ShaderInstructionSamplerOperation(inst.type);
			return true;
		}
	}
	if (sload.dst.size == 8 && inst.src_num >= 2 && inst.src[1].type == ShaderOperandType::Sgpr &&
	    inst.src[1].register_id == destination && inst.src[1].size == 8)
	{
		if (ShaderInstructionReadsImageResource(inst.type))
		{
			use->kind = ShaderDynamicSLoadResourceKind::Texture;
			// A size query reads no texel and fits a sampled or a storage binding
			// (see ShaderMergeDynamicSLoadUse).
			use->texture_usage =
			    inst.type == ShaderInstructionType::ImageGetResinfo ? ShaderTextureUsage::Unknown : ShaderTextureUsage::ReadOnly;
			use->sampler_operation = ShaderInstructionUsesImageSampler(inst.type)
			                             ? ShaderInstructionSamplerOperation(inst.type)
			                             : State::ImageSampleOperation::Regular;
			use->sampled_shape_known =
			    ShaderGen5SampledTextureShapeForMimgDimension(inst.mimg_dimension, &use->sampled_shape);
			use->storage_readable = !ShaderInstructionUsesImageSampler(inst.type) && inst.type != ShaderInstructionType::ImageGetResinfo;
			return true;
		}
		if (ShaderInstructionWritesImageResource(inst.type))
		{
			use->kind          = ShaderDynamicSLoadResourceKind::Texture;
			use->texture_usage = ShaderTextureUsage::ReadWrite;
			return true;
		}
	}
	return false;
}

// Consumers of one descriptor S_LOAD destination up to its first clobber in
// program order. The use is invalid when any other read or a conflicting
// descriptor contract appears before that point. The scan stops at branches,
// or only at unconditional ones when follow_conditional_branches is set: the
// fall-through path is the next instructions, and a forward target lies in
// the same scanned range.
// Adds the consumer at consumer_pc to use; false when it conflicts with the
// consumers already merged.
static bool ShaderIsTextureSizeQuery(const ShaderDynamicSLoadUse& use)
{
	return use.kind == ShaderDynamicSLoadResourceKind::Texture && use.texture_usage == ShaderTextureUsage::Unknown;
}

// A T# whose only consumers are size queries binds as sampled.
static ShaderDynamicSLoadUse ShaderResolveTextureSizeQueries(ShaderDynamicSLoadUse use)
{
	if (use.found && ShaderIsTextureSizeQuery(use))
	{
		use.texture_usage = ShaderTextureUsage::ReadOnly;
	}
	return use;
}

static bool ShaderMergeDynamicSLoadUse(ShaderDynamicSLoadUse* use, const ShaderDynamicSLoadUse& next_use, uint32_t consumer_pc)
{
	// Size queries take the use of the other consumers of the T#.
	if (use->found && use->kind == ShaderDynamicSLoadResourceKind::Texture && ShaderIsTextureSizeQuery(next_use))
	{
		use->last_consumer_pc = std::max(use->last_consumer_pc, consumer_pc);
		return true;
	}
	if (use->found && ShaderIsTextureSizeQuery(*use) && next_use.kind == ShaderDynamicSLoadResourceKind::Texture)
	{
		const uint32_t query_pc = use->last_consumer_pc;
		*use                    = {};
		const bool merged       = ShaderMergeDynamicSLoadUse(use, next_use, consumer_pc);
		use->last_consumer_pc   = std::max(use->last_consumer_pc, query_pc);
		return merged;
	}
	// A texel read without a sampler joins a store to the same T# through its storage view.
	if (use->found && use->kind == ShaderDynamicSLoadResourceKind::Texture && next_use.kind == ShaderDynamicSLoadResourceKind::Texture &&
	    use->texture_usage != next_use.texture_usage)
	{
		const bool use_reads = use->texture_usage == ShaderTextureUsage::ReadOnly;
		if (!(use_reads ? use->storage_readable : next_use.storage_readable))
		{
			return false;
		}
		if (use_reads)
		{
			const uint32_t read_pc = use->last_consumer_pc;
			*use                   = next_use;
			use->last_consumer_pc  = read_pc;
			use->found             = true;
		}
		use->last_consumer_pc = std::max(use->last_consumer_pc, consumer_pc);
		return true;
	}
	if (use->found && (use->kind != next_use.kind || use->texture_usage != next_use.texture_usage))
	{
		return false;
	}
	if (use->found && use->sampled_shape_known && next_use.sampled_shape_known && use->sampled_shape != next_use.sampled_shape)
	{
		return false;
	}
	const bool operation_sensitive =
	    use->kind == ShaderDynamicSLoadResourceKind::Sampler ||
	    (use->kind == ShaderDynamicSLoadResourceKind::Texture && use->texture_usage == ShaderTextureUsage::ReadOnly);
	const auto sampler_operation = use->found && operation_sensitive && use->sampler_operation != next_use.sampler_operation
	                                   ? State::ImageSampleOperation::Mixed
	                                   : next_use.sampler_operation;
	use->kind = next_use.kind;
	if (next_use.storage_usage == ShaderStorageUsage::ReadWrite)
	{
		use->storage_usage = ShaderStorageUsage::ReadWrite;
	}
	use->texture_usage     = next_use.texture_usage;
	use->sampler_operation = sampler_operation;
	if (next_use.sampled_shape_known)
	{
		use->sampled_shape       = next_use.sampled_shape;
		use->sampled_shape_known = true;
	}
	use->last_consumer_pc     = std::max(use->last_consumer_pc, consumer_pc);
	use->raw_vmem_oob_guarded = use->raw_vmem_oob_guarded || next_use.raw_vmem_oob_guarded;
	use->raw_smem_use         = use->raw_smem_use || next_use.raw_smem_use;
	use->raw_only             = (!use->found || use->raw_only) && next_use.raw_only;
	use->dword_loads_only     = (!use->found || use->dword_loads_only) && next_use.dword_loads_only;
	use->storage_readable     = (!use->found || use->storage_readable) && next_use.storage_readable;
	use->found                = true;
	return true;
}

static ShaderDynamicSLoadUse ShaderFindDynamicSLoadUse(const ShaderCode& code, uint32_t index, const ShaderInstruction& sload)
{
	const int dword_count = sload.dst.size;
	ShaderDynamicSLoadUse use {};
	for (uint32_t next_index = index + 1; next_index < code.GetInstructions().Size(); ++next_index)
	{
		const auto& next = code.GetInstructions().At(next_index);
		if (next.type == ShaderInstructionType::Unknown || next.type == ShaderInstructionType::SEndpgm ||
		    next.type == ShaderInstructionType::SSetpcB64 || ShaderInstructionHasStaticBranchTarget(next.type))
		{
			break;
		}

		ShaderDynamicSLoadUse next_use {};
		if (ShaderDynamicSLoadMatchesConsumer(next, sload, &next_use))
		{
			if (!ShaderMergeDynamicSLoadUse(&use, next_use, next.pc))
			{
				use.valid = false;
				break;
			}
		}
		else if (ShaderInstructionReadsSgprRange(next, sload.dst.register_id, dword_count))
		{
			use.valid = false;
			break;
		}

		if (ShaderInstructionWritesSgprRange(next, sload.dst.register_id, dword_count))
		{
			break;
		}
	}

	return ShaderResolveTextureSizeQueries(use);
}

// An S_LOAD whose destination is a whole T# (8 dwords), S# or V# (4 dwords)
// read through a 64-bit SGPR pointer; 0 for any other instruction.
static int ShaderDescriptorSLoadDwords(const ShaderInstruction& sload)
{
	const int dword_count = (sload.type == ShaderInstructionType::SLoadDwordx4 ? 4 :
	                         (sload.type == ShaderInstructionType::SLoadDwordx8 ? 8 : 0));
	if (dword_count == 0 || sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != dword_count || sload.src_num < 2 ||
	    sload.src[0].type != ShaderOperandType::Sgpr || sload.src[0].size != 2)
	{
		return 0;
	}
	return dword_count;
}

const std::vector<ShaderDynamicSLoadUse>& ShaderCodeResourceMemo::DescriptorSLoadUses(const ShaderCode& code) const
{
	std::call_once(m_descriptor_sload_uses_once,
	               [&]
	               {
		               const uint32_t instruction_count = code.GetInstructions().Size();
		               m_descriptor_sload_uses.resize(instruction_count);
		               for (uint32_t index = 0; index < instruction_count; ++index)
		               {
			               const auto& sload = code.GetInstructions().At(index);
			               if (ShaderDescriptorSLoadDwords(sload) != 0)
			               {
				               m_descriptor_sload_uses[index] = ShaderFindDynamicSLoadUse(code, index, sload);
			               }
		               }
	               });
	return m_descriptor_sload_uses;
}

bool ShaderEudDescriptorLoadIsUnmapped(const ShaderBindResources& bind, const ShaderInstruction& sload)
{
	int offset_dw = 0;
	if (!bind.extended.used || ShaderDescriptorSLoadDwords(sload) == 0 || sload.src[0].register_id != bind.extended.start_register ||
	    !ShaderGetSmemConstantDwordOffset(sload, &offset_dw) || offset_dw < 0)
	{
		return false;
	}
	const int words = sload.dst.size;
	if (words <= 0)
	{
		return false;
	}
	EXIT_IF(words > 32);
	// One pass over the static extended resources marks the loaded words they cover.
	const int first   = offset_dw + 16;
	uint32_t  covered = 0;
	const auto cover  = [&](int start, int dwords)
	{
		for (int reg = std::max(start, first); reg < std::min(start + dwords, first + words); ++reg)
		{
			covered |= 1u << static_cast<uint32_t>(reg - first);
		}
	};
	for (int i = 0; i < bind.storage_buffers.buffers_num; ++i)
	{
		if (bind.storage_buffers.extended[i] && !bind.storage_buffers.dynamic_sload[i])
		{
			cover(bind.storage_buffers.start_register[i], 4);
		}
	}
	for (int i = 0; i < bind.textures2D.textures_num; ++i)
	{
		const auto& texture = bind.textures2D.desc[i];
		if (texture.extended && !texture.dynamic_sload)
		{
			cover(texture.start_register, 8);
		}
	}
	for (int i = 0; i < bind.samplers.samplers_num; ++i)
	{
		if (bind.samplers.extended[i] && !bind.samplers.dynamic_sload[i])
		{
			cover(bind.samplers.start_register[i], 4);
		}
	}
	for (int i = 0; i < bind.gds_pointers.pointers_num; ++i)
	{
		if (bind.gds_pointers.extended[i])
		{
			cover(bind.gds_pointers.start_register[i], 1);
		}
	}
	const uint32_t all_words = words == 32 ? ~0u : (1u << static_cast<uint32_t>(words)) - 1u;
	return covered != all_words;
}

// Where one word of a loaded V# lives: an SGPR, a VGPR lane a static
// V_WRITELANE parked it in, or both (-1 when it is not there).
struct ShaderDescriptorWordPlace
{
	int sgpr = -1;
	int vgpr = -1;
	int lane = -1;
};
using ShaderDescriptorPlaces = std::array<ShaderDescriptorWordPlace, 4>;

static bool ShaderStaticLane(const ShaderOperand& operand, int* lane)
{
	if (operand.type != ShaderOperandType::IntegerInlineConstant || operand.constant.i < 0 || operand.constant.i > 63)
	{
		return false;
	}
	*lane = operand.constant.i;
	return true;
}

// V_WRITELANE of one SGPR into a constant lane, the scalar spill store.
static bool ShaderScalarSpillStore(const ShaderInstruction& inst, int* lane)
{
	return inst.type == ShaderInstructionType::VWritelaneB32 && inst.dst.type == ShaderOperandType::Vgpr && inst.src_num >= 2 &&
	       inst.src[0].type == ShaderOperandType::Sgpr && inst.src[0].size == 1 && ShaderStaticLane(inst.src[1], lane);
}

static bool ShaderOperandCoversVgpr(const ShaderOperand& operand, int reg)
{
	return operand.type == ShaderOperandType::Vgpr && reg >= operand.register_id && reg < operand.register_id + std::max(operand.size, 1);
}

static bool ShaderIsBufferFormatLoad(ShaderInstructionType type)
{
	return type == ShaderInstructionType::BufferLoadFormatX || type == ShaderInstructionType::BufferLoadFormatXy ||
	       type == ShaderInstructionType::BufferLoadFormatXyz || type == ShaderInstructionType::BufferLoadFormatXyzw;
}

static bool ShaderDescriptorPlacesLive(const ShaderDescriptorPlaces& places)
{
	return std::any_of(places.begin(), places.end(), [](const auto& place) { return place.sgpr >= 0 || place.vgpr >= 0; });
}

// False when inst reads a word of the V# other than as a whole-V# format load
// or as the SGPR of a spill store; sets *consumed for a format load.
static bool ShaderDescriptorReadIsFormatLoadOrSpill(const ShaderInstruction& inst, const ShaderDescriptorPlaces& places, bool* consumed)
{
	bool whole = true;
	for (int word = 0; word < 4; ++word)
	{
		whole = whole && places[0].sgpr >= 0 && places[word].sgpr == places[0].sgpr + word;
	}
	int        lane  = 0;
	const bool spill = ShaderScalarSpillStore(inst, &lane);
	auto reads_word = [&](const ShaderOperand& operand)
	{
		return std::any_of(places.begin(), places.end(),
		                   [&](const auto& place) { return place.sgpr >= 0 && ShaderOperandOverlapsSgprRange(operand, place.sgpr, 1); });
	};
	for (int source = 0; source < inst.src_num; ++source)
	{
		const auto& operand = inst.src[source];
		if (!reads_word(operand) || (spill && source == 0))
		{
			continue;
		}
		if (!whole || source != 1 || !ShaderIsBufferFormatLoad(inst.type) || operand.type != ShaderOperandType::Sgpr ||
		    operand.register_id != places[0].sgpr || operand.size != 4)
		{
			return false;
		}
		*consumed = true;
	}
	// A parked word leaves its lane only through a V_READLANE of that constant lane.
	int        read_lane   = 0;
	const bool static_read = inst.type == ShaderInstructionType::VReadlaneB32 && inst.src_num >= 2 && ShaderStaticLane(inst.src[1], &read_lane);
	auto reads_lane = [&](const ShaderOperand& operand)
	{
		return std::any_of(places.begin(), places.end(),
		                   [&](const auto& place) { return place.vgpr >= 0 && ShaderOperandCoversVgpr(operand, place.vgpr); });
	};
	for (int source = 0; source < inst.src_num; ++source)
	{
		if (reads_lane(inst.src[source]) && !(static_read && source == 0))
		{
			return false;
		}
	}
	for (int source = 0; source < inst.mimg_address_num; ++source)
	{
		if (reads_word(inst.mimg_address[source]) || reads_lane(inst.mimg_address[source]))
		{
			return false;
		}
	}
	return true;
}

// Applies inst to where the words live: SGPR writes drop them, a spill store
// parks them in a lane, a VGPR write drops the lanes, and a V_READLANE of a
// parked lane brings the word back into an SGPR. False when a word would live
// in two SGPRs or two lanes, which one place per word cannot follow.
static bool ShaderMoveDescriptorPlaces(const ShaderInstruction& inst, ShaderDescriptorPlaces* places)
{
	std::array<int, 4> restored {-1, -1, -1, -1};
	int                lane = 0;
	if (inst.type == ShaderInstructionType::VReadlaneB32 && inst.src_num >= 2 && inst.src[0].type == ShaderOperandType::Vgpr &&
	    inst.dst.type == ShaderOperandType::Sgpr && inst.dst.size == 1 && ShaderStaticLane(inst.src[1], &lane))
	{
		for (int word = 0; word < 4; ++word)
		{
			if ((*places)[word].vgpr == inst.src[0].register_id && (*places)[word].lane == lane)
			{
				restored[word] = inst.dst.register_id;
			}
		}
	}
	const bool spill = ShaderScalarSpillStore(inst, &lane);
	for (int word = 0; word < 4; ++word)
	{
		auto& place = (*places)[word];
		if (place.sgpr >= 0 && ShaderInstructionWritesSgprRange(inst, place.sgpr, 1))
		{
			place.sgpr = -1;
		}
		const bool lane_written = spill ? place.vgpr == inst.dst.register_id && place.lane == lane
		                                : place.vgpr >= 0 && (ShaderOperandCoversVgpr(inst.dst, place.vgpr) ||
		                                                      ShaderOperandCoversVgpr(inst.dst2, place.vgpr));
		if (lane_written)
		{
			place.vgpr = -1;
			place.lane = -1;
		}
		if (spill && place.sgpr == inst.src[0].register_id)
		{
			if (place.vgpr >= 0)
			{
				return false;
			}
			place.vgpr = inst.dst.register_id;
			place.lane = lane;
		}
		if (restored[word] >= 0)
		{
			if (place.sgpr >= 0 && place.sgpr != restored[word])
			{
				return false;
			}
			place.sgpr = restored[word];
		}
	}
	return true;
}

// Every reader of the V# the S_LOAD at index fills, followed through static
// lane spills, is a format load of the whole V#, and the V# is dead in SGPRs
// and lanes before any branch or the program ends.
static bool ShaderLoadedDescriptorReadOnlyByFormatLoads(const ShaderCode& code, uint32_t index)
{
	const auto&            instructions = code.GetInstructions();
	ShaderDescriptorPlaces places {};
	for (int word = 0; word < 4; ++word)
	{
		places[word].sgpr = instructions.At(index).dst.register_id + word;
	}
	bool consumed = false;
	for (uint32_t next = index + 1; next < instructions.Size() && ShaderDescriptorPlacesLive(places); ++next)
	{
		const auto& inst = instructions.At(next);
		if (inst.type == ShaderInstructionType::SEndpgm)
		{
			return consumed;
		}
		if (inst.type == ShaderInstructionType::Unknown || inst.type == ShaderInstructionType::SSetpcB64 ||
		    ShaderInstructionHasStaticBranchTarget(inst.type) || !ShaderDescriptorReadIsFormatLoadOrSpill(inst, places, &consumed) ||
		    !ShaderMoveDescriptorPlaces(inst, &places))
		{
			return false;
		}
	}
	return consumed && !ShaderDescriptorPlacesLive(places);
}

static bool ShaderIsScalarMemoryLoad(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SLoadDword || type == ShaderInstructionType::SLoadDwordx2 ||
	       type == ShaderInstructionType::SLoadDwordx4 || type == ShaderInstructionType::SLoadDwordx8 ||
	       type == ShaderInstructionType::SLoadDwordx16;
}

bool ShaderEudDescriptorReadOnlyByFormatLoads(const ShaderCode& code, const ShaderBindResources& bind, int eud_offset_dw,
                                              int user_data_register_base, const ShaderScalarFlow& scalar_flow)
{
	const auto& instructions = code.GetInstructions();
	if (!bind.extended.used || bind.extended.read_at_runtime || eud_offset_dw < 0 ||
	    scalar_flow.holding_entry_value.size() != instructions.Size())
	{
		return false;
	}
	const int pointer = bind.extended.start_register + user_data_register_base;
	bool      found   = false;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst    = instructions.At(index);
		const auto& entry   = scalar_flow.holding_entry_value[index];
		const bool  holding = pointer >= 0 && pointer + 1 < kShaderScalarLivenessSgprs && entry.test(static_cast<size_t>(pointer)) &&
		                     entry.test(static_cast<size_t>(pointer + 1));
		if (!holding || !ShaderInstructionReadsSgprRange(inst, pointer, 2))
		{
			continue;
		}
		// Any other use of the pointer may read the V# where this scan cannot follow it.
		int offset_dw = 0;
		if (!ShaderIsScalarMemoryLoad(inst.type) ||
		    !ShaderMatchesEudPointer(inst.src[0], bind.extended.start_register, user_data_register_base, entry) ||
		    !ShaderGetSmemConstantDwordOffset(inst, &offset_dw) || inst.dst.type != ShaderOperandType::Sgpr)
		{
			return false;
		}
		if (offset_dw + inst.dst.size <= eud_offset_dw || offset_dw >= eud_offset_dw + 4)
		{
			continue;
		}
		if (offset_dw != eud_offset_dw || inst.type != ShaderInstructionType::SLoadDwordx4 || inst.dst.size != 4 ||
		    !ShaderLoadedDescriptorReadOnlyByFormatLoads(code, index))
		{
			return false;
		}
		found = true;
	}
	return found;
}

ShaderEudDescriptorLoads ShaderFindEudDescriptorLoads(const ShaderCode& code)
{
	ShaderEudDescriptorLoads loads;
	const uint32_t           instruction_count = code.GetInstructions().Size();
	for (uint32_t index = 0; index < instruction_count; ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		ShaderSplitTextureCandidate split {};
		if ((ShaderInstructionReadsImageResource(inst.type) || ShaderInstructionWritesImageResource(inst.type)) &&
		    ShaderFindSplitTextureHalves(code, index, &split))
		{
			loads.split_textures.push_back(split);
		}
		const int dwords    = ShaderDescriptorSLoadDwords(inst);
		int       offset_dw = 0;
		if (dwords != 0 && ShaderGetSmemConstantDwordOffset(inst, &offset_dw) && offset_dw >= 0)
		{
			loads.sloads.push_back({index, dwords, offset_dw});
		}
	}
	return loads;
}

ShaderInstructionIndices ShaderIndexInstructions(const ShaderCode& code)
{
	ShaderInstructionIndices indices;
	const auto&              instructions = code.GetInstructions();
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (ShaderInstructionIsScalarBufferLoad(inst) || ShaderInstructionIsVectorBufferAccess(inst.type))
		{
			indices.buffer_accesses.push_back(index);
		}
		if (ShaderInstructionUsesImageSampler(inst.type))
		{
			indices.image_samplers.push_back(index);
		}
		if (ShaderImageReadReturnsDmaskComponents(inst.type))
		{
			indices.image_reads.push_back(index);
		}
		if (inst.type == ShaderInstructionType::SLoadDwordx4 || inst.type == ShaderInstructionType::SLoadDwordx8)
		{
			indices.descriptor_loads.push_back(index);
		}
		if (inst.format == ShaderInstructionFormat::VdstGds || inst.dst.type == ShaderOperandType::M0)
		{
			indices.gds_and_m0_writes.push_back(index);
		}
		indices.gds_memory      = indices.gds_memory || inst.format == ShaderInstructionFormat::VaddrVdataOffsetGds;
		indices.has_image_reads = indices.has_image_reads || ShaderInstructionReadsImageResource(inst.type);
	}
	return indices;
}

const ShaderInstructionIndices& ShaderCodeResourceMemo::InstructionIndices(const ShaderCode& code) const
{
	std::call_once(m_instruction_indices_once, [&] { m_instruction_indices = ShaderIndexInstructions(code); });
	return m_instruction_indices;
}

const ShaderEudDescriptorLoads& ShaderCodeResourceMemo::EudDescriptorLoads(const ShaderCode& code) const
{
	std::call_once(m_eud_descriptor_loads_once, [&] { m_eud_descriptor_loads = ShaderFindEudDescriptorLoads(code); });
	return m_eud_descriptor_loads;
}

const std::vector<ShaderNestedTableLoad>& ShaderCodeResourceMemo::NestedTableLoads(const ShaderCode& code) const
{
	std::call_once(m_nested_table_loads_once, [&] { m_nested_table_loads = ShaderFindNestedTableLoads(code); });
	return m_nested_table_loads;
}

enum class ShaderDescriptorLoadPass
{
	NonSampler,
	Sampler,
};

static bool ShaderDescriptorLoadInPass(const ShaderDynamicSLoadUse& use, ShaderDescriptorLoadPass pass)
{
	return (use.kind == ShaderDynamicSLoadResourceKind::Sampler) == (pass == ShaderDescriptorLoadPass::Sampler);
}

// True when every image instruction that samples through the S# loaded by
// sload, up to last_consumer_pc, reads a T# that binds nothing.
static bool ShaderSamplerFeedsOnlyZeroImages(const ShaderCode& code, const ShaderBindResources& bind, const ShaderInstruction& sload,
                                             uint32_t last_consumer_pc, const ShaderCodeResourceMemo* memo)
{
	const ShaderInstructionIndices direct   = memo == nullptr ? ShaderIndexInstructions(code) : ShaderInstructionIndices {};
	const auto&                    indices  = memo != nullptr ? memo->InstructionIndices(code) : direct;
	bool                           consumed = false;
	for (const uint32_t index: indices.image_samplers)
	{
		const auto& inst = code.GetInstructions().At(index);
		if (inst.pc <= sload.pc || inst.pc > last_consumer_pc || inst.src_num < 3 || inst.src[2].type != ShaderOperandType::Sgpr ||
		    inst.src[2].register_id != sload.dst.register_id)
		{
			continue;
		}
		if (!ShaderImageReadsZeroResource(bind, inst))
		{
			return false;
		}
		consumed = true;
	}
	return consumed;
}

// Registers the resource a descriptor S_LOAD materializes and its PC-keyed
// mapping. table holds the loaded words at offset_dw. Samplers are added after
// every T# (ShaderDescriptorLoadPass) so an S# that only feeds reads of a T#
// that binds nothing binds nothing either.
static bool ShaderAddDynamicScalarResource(const ShaderCode& code, ShaderBindResources* bind, ShaderParsedUsage* info,
                                           const ShaderInstruction& sload, int offset_dw, const ShaderDynamicSLoadUse& use,
                                           const HW::UserSgprInfo& user_sgpr, const uint32_t* table, uint32_t instruction_count,
                                           const ShaderCodeResourceMemo* memo)
{
	bool added_resource = false;
	bool added_mapping = false;
	switch (use.kind)
	{
		case ShaderDynamicSLoadResourceKind::StorageBuffer:
			added_mapping = ShaderAddDynamicScalarStorageResource(bind, info, sload, offset_dw, use.storage_usage, use.last_consumer_pc,
			                                                      use.raw_vmem_oob_guarded, table,
			                                                      instruction_count, &added_resource);
			break;
		case ShaderDynamicSLoadResourceKind::Texture:
			added_mapping = ShaderAddDynamicTextureResource(bind, sload, offset_dw, use.last_consumer_pc, use.texture_usage,
			                                                use.sampler_operation, use.sampled_shape, use.sampled_shape_known,
			                                                 user_sgpr, table, instruction_count, &added_resource);
			if (added_resource)
			{
				if (use.texture_usage == ShaderTextureUsage::ReadWrite)
				{
					info->textures2D_readwrite++;
				} else
				{
					info->textures2D_readonly++;
				}
			}
			break;
		case ShaderDynamicSLoadResourceKind::Sampler:
			if (ShaderSamplerFeedsOnlyZeroImages(code, *bind, sload, use.last_consumer_pc, memo))
			{
				ShaderAddZeroDescriptorLoad(&bind->zero_descriptor_loads, sload.pc, sload.dst.register_id, 4);
				return true;
			}
			added_mapping = ShaderAddDynamicSamplerResource(bind, sload, offset_dw, use.last_consumer_pc,
			                                                 use.sampler_operation, user_sgpr,
			                                                 table, instruction_count, &added_resource);
			if (added_resource)
			{
				info->samplers++;
			}
			break;
	}
	return added_mapping;
}

// A dynamic scalar descriptor is safe to materialize only when an
// extended-pointer S_LOAD has a constant in-range offset and every read before
// clobber is a descriptor consumer with the same contract. The mapping remains
// keyed by the S_LOAD PC because the destination registers can be reused.
void ShaderCollectDynamicScalarResources(const ShaderCode& code, ShaderBindResources* bind,
	                                             const HW::UserSgprInfo& user_sgpr, ShaderParsedUsage* info,
	                                             const uint32_t* extended_buffer, uint16_t eud_size_dw, int user_data_register_base,
	                                             const ShaderScalarFlow& scalar_flow, const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(bind == nullptr || info == nullptr);
	if (!bind->extended.used || extended_buffer == nullptr)
	{
		return;
	}
	const auto* memo_uses = memo != nullptr ? &memo->DescriptorSLoadUses(code) : nullptr;
	const ShaderEudDescriptorLoads direct_loads = memo == nullptr ? ShaderFindEudDescriptorLoads(code) : ShaderEudDescriptorLoads {};
	const auto& loads = memo != nullptr ? memo->EudDescriptorLoads(code) : direct_loads;
	const uint32_t instruction_count = code.GetInstructions().Size();
	const auto& entry_values = scalar_flow.holding_entry_value;
	ShaderCollectSplitTextureResources(code, bind, info, extended_buffer, eud_size_dw, user_data_register_base, entry_values,
	                                   loads.split_textures);

	for (const auto pass: {ShaderDescriptorLoadPass::NonSampler, ShaderDescriptorLoadPass::Sampler})
	for (const auto& candidate: loads.sloads)
	{
		const uint32_t index       = candidate.index;
		const auto&    sload       = code.GetInstructions().At(index);
		const int      dword_count = candidate.dwords;
		const int      offset_dw   = candidate.offset_dw;
		if (index >= entry_values.size() ||
		    !ShaderMatchesEudPointer(sload.src[0], bind->extended.start_register, user_data_register_base, entry_values[index]))
		{
			continue;
		}
		const auto use = memo_uses != nullptr ? (*memo_uses)[index] : ShaderFindDynamicSLoadUse(code, index, sload);
		// A V# the program also spills has registers no binding can track: the
		// program reads the table and the buffers through guest addresses.
		if ((!use.valid || !use.found) && ShaderEudDescriptorLoadIsUnmapped(*bind, sload))
		{
			bind->extended.read_at_runtime = true;
		}
		// offset_dw is the EUD table index (byte_offset/4). Metadata eud_size_dw is a
		// lower bound — same overrun policy as ShaderGen5EudSpanAllowed (api = 16+idx).
		const uint32_t span_dw = static_cast<uint32_t>(dword_count);
		if (span_dw > SHADER_GEN5_EUD_MAX_DWORDS ||
		    static_cast<uint32_t>(offset_dw) > SHADER_GEN5_EUD_MAX_DWORDS - span_dw ||
		    !ShaderGen5EudSpanAllowed(16 + offset_dw, dword_count, eud_size_dw))
		{
			continue;
		}

		if (!use.valid || !use.found || !ShaderDescriptorLoadInPass(use, pass))
		{
			continue;
		}

		if (!ShaderAddDynamicScalarResource(code, bind, info, sload, offset_dw, use, user_sgpr, extended_buffer, instruction_count, memo))
		{
			EXIT("unable to materialize dynamic descriptor: pc=0x%08" PRIx32 " offset_dw=%d dwords=%d kind=%u "
			     "storage=%d textures=%d samplers=%d mappings=%u eud_dw=%u\n",
			     sload.pc, offset_dw, dword_count, static_cast<unsigned>(use.kind), bind->storage_buffers.buffers_num,
			     bind->textures2D.textures_num, bind->samplers.samplers_num,
			     static_cast<unsigned>(bind->dynamic_sloads.records.Size()),
			     static_cast<unsigned>(eud_size_dw));
		}
	}
}

// ShaderDescriptorSLoadDwords, plus a 16-dword load of two 8-dword blocks.
static int ShaderTableSLoadDwords(const ShaderInstruction& sload)
{
	if (sload.type == ShaderInstructionType::SLoadDwordx16 && sload.dst.type == ShaderOperandType::Sgpr && sload.dst.size == 16 &&
	    sload.src_num >= 2 && sload.src[0].type == ShaderOperandType::Sgpr && sload.src[0].size == 2)
	{
		return 16;
	}
	return ShaderDescriptorSLoadDwords(sload);
}

// The pointer pair of a descriptor S_LOAD lies in the shader resource table
// (SRT) user SGPRs, is not the EUD pointer, and still holds its entry value.
static bool ShaderLoadsThroughSrtPointer(const ShaderInstruction& sload, const std::bitset<kShaderScalarLivenessSgprs>& entry_values,
                                         uint16_t srt_size_dw, int user_data_register_base, const ShaderBindResources& bind)
{
	const int reg      = sload.src[0].register_id;
	const int user_reg = reg - user_data_register_base;
	if (user_reg < 0 || user_reg + 1 >= static_cast<int>(srt_size_dw) || user_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX ||
	    reg + 1 >= kShaderScalarLivenessSgprs)
	{
		return false;
	}
	if (bind.extended.used && user_reg == bind.extended.start_register)
	{
		return false;
	}
	return entry_values.test(static_cast<size_t>(reg)) && entry_values.test(static_cast<size_t>(reg + 1));
}

static ShaderDynamicSLoadUse ShaderFindReachingSLoadUse(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t load_index,
                                                        const ShaderInstruction& block);

// Splits the destination of a pointer-table load into descriptors with
// consumers: a block used as one T# or S# stays whole, an 8-dword block may
// also hold two S#, and a 16-dword load is two 8-dword halves. Each block is
// analysed as its own load of the same PC, over every CFG path.
static bool ShaderPartitionDescriptorLoad(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t index,
                                          const ShaderInstruction& sload, int first_dword, int dwords,
                                          std::vector<ShaderDescriptorBlock>* blocks)
{
	if (dwords == 16)
	{
		return ShaderPartitionDescriptorLoad(code, cfg, index, sload, first_dword, 8, blocks) &&
		       ShaderPartitionDescriptorLoad(code, cfg, index, sload, first_dword + 8, 8, blocks);
	}
	ShaderInstruction block = sload;
	block.type              = dwords == 4 ? ShaderInstructionType::SLoadDwordx4 : ShaderInstructionType::SLoadDwordx8;
	block.dst.register_id   = sload.dst.register_id + first_dword;
	block.dst.size          = dwords;
	const auto use          = ShaderFindReachingSLoadUse(code, cfg, index, block);
	// A V# that is only read stays a runtime descriptor read through guest
	// device addressing; a written one needs a binding, which that path lacks.
	const bool bound_kind = use.kind != ShaderDynamicSLoadResourceKind::StorageBuffer || use.storage_usage == ShaderStorageUsage::ReadWrite;
	if (use.valid && use.found && bound_kind)
	{
		blocks->push_back({block, first_dword, use});
		return true;
	}
	return dwords == 8 && ShaderPartitionDescriptorLoad(code, cfg, index, sload, first_dword, 4, blocks) &&
	       ShaderPartitionDescriptorLoad(code, cfg, index, sload, first_dword + 4, 4, blocks);
}

static std::vector<ShaderDescriptorBlock> ShaderPartitionTableLoad(const ShaderCode& code, const ShaderControlFlowGraph& cfg,
                                                                   uint32_t index)
{
	std::vector<ShaderDescriptorBlock> blocks;
	const auto&                        sload = code.GetInstructions().At(index);
	if (!cfg.Structurable() || !ShaderPartitionDescriptorLoad(code, cfg, index, sload, 0, ShaderTableSLoadDwords(sload), &blocks))
	{
		blocks.clear();
	}
	return blocks;
}

const std::vector<ShaderDescriptorBlock>& ShaderCodeResourceMemo::TableLoadBlocks(const ShaderCode& code, uint32_t index) const
{
	std::shared_ptr<const ShaderControlFlowGraph> cfg;
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		const auto                          found = m_table_load_blocks.find(index);
		if (found != m_table_load_blocks.end())
		{
			return found->second;
		}
		cfg = m_cfg;
	}
	if (cfg == nullptr)
	{
		cfg = std::make_shared<const ShaderControlFlowGraph>(ShaderBuildControlFlowGraph(code));
	}
	auto                                blocks = ShaderPartitionTableLoad(code, *cfg, index);
	std::unique_lock<std::shared_mutex> lock(m_mutex);
	if (m_cfg == nullptr)
	{
		m_cfg = cfg;
	}
	return m_table_load_blocks.emplace(index, std::move(blocks)).first->second;
}

const std::vector<ShaderPointerTableLoadEntry>& ShaderCodeResourceMemo::PointerTableLoads(const ShaderCode& code) const
{
	std::call_once(
	    m_pointer_table_loads_once,
	    [&]
	    {
		    std::shared_ptr<const ShaderControlFlowGraph> cfg;
		    const auto&                                   instructions = code.GetInstructions();
		    for (uint32_t index = 0; index < instructions.Size(); ++index)
		    {
			    const auto& sload       = instructions.At(index);
			    const int   dword_count = ShaderTableSLoadDwords(sload);
			    int         offset_dw   = 0;
			    if (dword_count == 0 || !ShaderGetSmemConstantDwordOffset(sload, &offset_dw) || offset_dw < 0 ||
			        static_cast<uint32_t>(dword_count) > kMaxSrtPointerTableDwords ||
			        static_cast<uint32_t>(offset_dw) > kMaxSrtPointerTableDwords - static_cast<uint32_t>(dword_count))
			    {
				    continue;
			    }
			    if (cfg == nullptr)
			    {
				    {
					    std::shared_lock<std::shared_mutex> lock(m_mutex);
					    cfg = m_cfg;
				    }
				    if (cfg == nullptr)
				    {
					    cfg = std::make_shared<const ShaderControlFlowGraph>(ShaderBuildControlFlowGraph(code));
					    std::unique_lock<std::shared_mutex> lock(m_mutex);
					    if (m_cfg == nullptr) { m_cfg = cfg; }
				    }
			    }
			    auto blocks = ShaderPartitionTableLoad(code, *cfg, index);
			    if (blocks.empty())
			    {
				    continue;
			    }
			    ShaderPointerTableLoadEntry entry;
			    entry.index     = index;
			    entry.dwords    = dword_count;
			    entry.offset_dw = offset_dw;
			    for (const auto& block: blocks)
			    {
				    entry.has_non_sampler = entry.has_non_sampler || ShaderDescriptorLoadInPass(block.use, ShaderDescriptorLoadPass::NonSampler);
				    entry.has_sampler     = entry.has_sampler || ShaderDescriptorLoadInPass(block.use, ShaderDescriptorLoadPass::Sampler);
			    }
			    entry.blocks = std::move(blocks);
			    m_pointer_table_loads.push_back(std::move(entry));
		    }
	    });
	return m_pointer_table_loads;
}

// The dwords of a nested table that its loads and copied T#s read, or with
// copied_buffers set, that its copied V#s read.
static uint32_t ShaderNestedTableDwords(const ShaderNestedTableLoad& load, bool copied_buffers)
{
	uint32_t table_dwords = 0;
	if (copied_buffers)
	{
		for (const auto& buffer: load.copied_buffers)
		{
			table_dwords = std::max(table_dwords, static_cast<uint32_t>(buffer.offset_dw + 4));
		}
		return table_dwords;
	}
	for (const auto& descriptor: load.loads)
	{
		table_dwords = std::max(table_dwords, static_cast<uint32_t>(descriptor.offset_dw + descriptor.block.dst.size));
	}
	for (const auto& texture: load.copied_textures)
	{
		table_dwords = std::max(table_dwords, static_cast<uint32_t>(texture.offset_dw + 8));
	}
	return table_dwords;
}

// The descriptor table each nested load reads at draw time (the part that
// ShaderNestedTableDwords selects); empty when its pointer load does not read
// through an SRT pointer or that part is empty.
static std::vector<std::vector<uint32_t>> ShaderSnapshotNestedTables(const ShaderCode& code, const std::vector<ShaderNestedTableLoad>& nested,
                                                                     const HW::UserSgprInfo& user_sgpr, uint16_t srt_size_dw,
                                                                     int user_data_register_base, const ShaderScalarFlow& scalar_flow,
                                                                     const ShaderBindResources& bind, bool copied_buffers)
{
	const auto&                        entry_values = scalar_flow.holding_entry_value;
	std::vector<std::vector<uint32_t>> tables(nested.size());
	for (size_t n = 0; n < nested.size(); ++n)
	{
		const auto&    load         = nested[n];
		const auto&    pointer      = code.GetInstructions().At(load.pointer_index);
		const uint32_t table_dwords = ShaderNestedTableDwords(load, copied_buffers);
		if (table_dwords == 0 || load.pointer_index >= entry_values.size() ||
		    !ShaderLoadsThroughSrtPointer(pointer, entry_values[load.pointer_index], srt_size_dw, user_data_register_base, bind))
		{
			continue;
		}
		const int      srt_register = pointer.src[0].register_id - user_data_register_base;
		const uint64_t srt_address  = ((static_cast<uint64_t>(user_sgpr.value[srt_register + 1]) & 0xffffu) << 32u) |
		                             user_sgpr.value[srt_register];
		const auto            pointer_dw = static_cast<uint32_t>(load.pointer_offset_dw);
		std::vector<uint32_t> srt_table(pointer_dw + 2u);
		if (!ShaderSnapshotGuestDescriptorTable(srt_address, pointer_dw + 2u, &srt_table))
		{
			EXIT("unreadable SRT pointer table: pc=0x%08" PRIx32 " sgpr=%d address=0x%016" PRIx64 " dwords=%u\n", pointer.pc,
			     srt_register, srt_address, pointer_dw + 2u);
		}
		const uint64_t table_address = ((static_cast<uint64_t>(srt_table[pointer_dw + 1u]) & 0xffffu) << 32u) | srt_table[pointer_dw];
		tables[n].resize(table_dwords);
		if (table_address == 0 || !ShaderSnapshotGuestDescriptorTable(table_address, table_dwords, &tables[n]))
		{
			EXIT("unreadable descriptor table behind a nested SRT pointer: pc=0x%08" PRIx32 " address=0x%016" PRIx64 " dwords=%u\n",
			     pointer.pc, table_address, table_dwords);
		}
	}
	return tables;
}

// Binds a T# assembled by scalar moves: each piece maps its load's part to the
// T# fields it holds, and the image copy flow carries them to the consumer.
static void ShaderBindNestedCopiedTexture(const ShaderCode& code, const ShaderNestedCopiedTexture& copied, const std::vector<uint32_t>& table,
                                          ShaderBindResources* bind, ShaderParsedUsage* info)
{
	const auto& consumer = code.GetInstructions().At(copied.consumer);
	const bool  reads    = ShaderInstructionReadsImageResource(consumer.type);
	ShaderTextureResource resource {};
	for (int field = 0; field < 8; ++field)
	{
		resource.fields[field] = table[copied.offset_dw + field];
	}
	// A loaded descriptor of buffer type binds no image; its reads return zero.
	if (reads && !ShaderGen5TextureTypeIsImage(resource.Type()))
	{
		ShaderAddLoadedZeroImageResource(&bind->zero_image_resources, consumer.src[1].register_id, consumer.pc - 1u, consumer.pc);
		return;
	}
	const auto usage     = reads ? ShaderTextureUsage::ReadOnly : ShaderTextureUsage::ReadWrite;
	const auto operation = ShaderInstructionUsesImageSampler(consumer.type) ? ShaderInstructionSamplerOperation(consumer.type)
	                                                                        : State::ImageSampleOperation::Regular;
	ShaderGen5SampledTextureShape shape {};
	const bool shape_known = reads && ShaderGen5SampledTextureShapeForMimgDimension(consumer.mimg_dimension, &shape) &&
	                         ShaderGen5InstructionShapeAppliesToType(resource.Type(), shape);
	bool       added_resource = false;
	const int  resource_index = ShaderAddSplitTextureResource(bind, resource, consumer.src[1].register_id, copied.offset_dw, usage,
	                                                          operation, shape, shape_known, &added_resource);
	bool       mapped         = resource_index >= 0;
	for (const auto& piece: copied.pieces)
	{
		const uint32_t first_record = bind->dynamic_sloads.records.Size();
		mapped = mapped && ShaderAddDynamicSLoadMapping(&bind->dynamic_sloads, ShaderDynamicSLoadResourceKind::Texture, resource_index,
		                                                piece.block, piece.offset_dw, piece.block.dst.size, piece.field_offset,
		                                                consumer.pc, false, code.GetInstructions().Size());
		for (uint32_t mapping = first_record; mapping < bind->dynamic_sloads.records.Size(); ++mapping)
		{
			bind->dynamic_sloads.records[mapping].guest_load = true;
		}
	}
	if (!mapped)
	{
		EXIT("unable to materialize copied nested SRT image descriptor: pc=0x%08" PRIx32 " dst=%d offset_dw=%d textures=%d\n", consumer.pc,
		     consumer.src[1].register_id, copied.offset_dw, bind->textures2D.textures_num);
	}
	if (added_resource)
	{
		(usage == ShaderTextureUsage::ReadWrite ? info->textures2D_readwrite : info->textures2D_readonly)++;
	}
}

// Binds what nested loads read from their draw-time tables. Like GS user-data
// loads, they keep reading guest memory and their mappings only name the bound
// resources.
static void ShaderBindNestedTableDescriptors(const ShaderCode& code, const std::vector<ShaderNestedTableLoad>& nested,
                                             const std::vector<std::vector<uint32_t>>& tables, const HW::UserSgprInfo& user_sgpr,
                                             ShaderDescriptorLoadPass pass, ShaderBindResources* bind, ShaderParsedUsage* info,
                                             const ShaderCodeResourceMemo* memo)
{
	for (size_t n = 0; n < nested.size(); ++n)
	{
		for (const auto& texture: nested[n].copied_textures)
		{
			if (!tables[n].empty() && pass == ShaderDescriptorLoadPass::NonSampler)
			{
				ShaderBindNestedCopiedTexture(code, texture, tables[n], bind, info);
			}
		}
		for (const auto& load: nested[n].loads)
		{
			if (tables[n].empty() || !ShaderDescriptorLoadInPass(load.use, pass))
			{
				continue;
			}
			const uint32_t first_record = bind->dynamic_sloads.records.Size();
			if (!ShaderAddDynamicScalarResource(code, bind, info, load.block, load.offset_dw, load.use, user_sgpr, tables[n].data(),
			                                    code.GetInstructions().Size(), memo))
			{
				EXIT("unable to materialize nested SRT descriptor: pc=0x%08" PRIx32 " offset_dw=%d dwords=%d kind=%u textures=%d "
				     "samplers=%d\n",
				     load.block.pc, load.offset_dw, load.block.dst.size, static_cast<unsigned>(load.use.kind),
				     bind->textures2D.textures_num, bind->samplers.samplers_num);
			}
			for (uint32_t mapping = first_record; mapping < bind->dynamic_sloads.records.Size(); ++mapping)
			{
				bind->dynamic_sloads.records[mapping].guest_load = true;
			}
		}
	}
}

// An SRT can hold 64-bit pointers to descriptor tables. A T# or S# read through
// such a pointer at a constant offset comes from the table the pointer selects
// at draw time, so it is materialized like an EUD descriptor load; every
// descriptor of a multi-descriptor load must have consumers. Buffer
// descriptors keep their existing paths.
void ShaderCollectPointerTableResources(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                        ShaderParsedUsage* info, uint16_t srt_size_dw, int user_data_register_base,
                                        const ShaderScalarFlow& scalar_flow, const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(bind == nullptr || info == nullptr);
	if (srt_size_dw < 2u)
	{
		return;
	}
	const auto& entry_values = scalar_flow.holding_entry_value;
	const uint32_t instruction_count = code.GetInstructions().Size();
	const auto     direct_nested     = memo == nullptr ? ShaderFindNestedTableLoads(code) : std::vector<ShaderNestedTableLoad> {};
	const auto&    nested            = memo != nullptr ? memo->NestedTableLoads(code) : direct_nested;
	const auto     nested_tables =
	    ShaderSnapshotNestedTables(code, nested, user_sgpr, srt_size_dw, user_data_register_base, scalar_flow, *bind, false);

	if (memo != nullptr)
	{
		const auto& entries = memo->PointerTableLoads(code);
		// Snapshot each active SRT pointer-table load once for both passes instead
		// of scanning the full instruction stream and re-snapshotting per pass.
		struct ActiveEntry
		{
			const ShaderPointerTableLoadEntry* entry = nullptr;
			std::vector<uint32_t>              table;
		};
		std::vector<ActiveEntry> active;
		if (!entries.empty())
		{
			active.reserve(entries.size());
			for (const auto& entry: entries)
			{
				if (entry.index >= entry_values.size())
				{
					continue;
				}
				const auto& sload = code.GetInstructions().At(entry.index);
				if (!ShaderLoadsThroughSrtPointer(sload, entry_values[entry.index], srt_size_dw, user_data_register_base, *bind))
				{
					continue;
				}
				const int      pointer = sload.src[0].register_id - user_data_register_base;
				const uint64_t address = ((static_cast<uint64_t>(user_sgpr.value[pointer + 1]) & 0xffffu) << 32u) | user_sgpr.value[pointer];
				const uint32_t table_dwords = static_cast<uint32_t>(entry.offset_dw) + static_cast<uint32_t>(entry.dwords);
				ActiveEntry item {&entry, std::vector<uint32_t>(table_dwords)};
				if (!ShaderSnapshotGuestDescriptorTable(address, table_dwords, item.table.data()))
				{
					EXIT("unreadable descriptor table behind an SRT pointer: pc=0x%08" PRIx32 " sgpr=%d address=0x%016" PRIx64
					     " dwords=%d\n",
					     sload.pc, pointer, address, entry.offset_dw + entry.dwords);
				}
				active.push_back(std::move(item));
			}
		}
		for (const auto pass: {ShaderDescriptorLoadPass::NonSampler, ShaderDescriptorLoadPass::Sampler})
		{
			ShaderBindNestedTableDescriptors(code, nested, nested_tables, user_sgpr, pass, bind, info, memo);
			for (const auto& item: active)
			{
				const auto& entry = *item.entry;
				if (!(pass == ShaderDescriptorLoadPass::NonSampler ? entry.has_non_sampler : entry.has_sampler))
				{
					continue;
				}
				const auto& sload = code.GetInstructions().At(entry.index);
				for (const auto& block: entry.blocks)
				{
					if (!ShaderDescriptorLoadInPass(block.use, pass))
					{
						continue;
					}
					if (!ShaderAddDynamicScalarResource(code, bind, info, block.load, entry.offset_dw + block.first_dword, block.use,
					                                    user_sgpr, item.table.data(), instruction_count, memo))
					{
						EXIT("unable to materialize SRT pointer descriptor: pc=0x%08" PRIx32 " offset_dw=%d dwords=%d kind=%u textures=%d "
						     "samplers=%d\n",
						     sload.pc, entry.offset_dw + block.first_dword, block.load.dst.size, static_cast<unsigned>(block.use.kind),
						     bind->textures2D.textures_num, bind->samplers.samplers_num);
					}
				}
			}
		}
		return;
	}

	for (const auto pass: {ShaderDescriptorLoadPass::NonSampler, ShaderDescriptorLoadPass::Sampler})
	{
	ShaderBindNestedTableDescriptors(code, nested, nested_tables, user_sgpr, pass, bind, info, memo);
	for (uint32_t index = 0; index < instruction_count && index < entry_values.size(); ++index)
	{
		const auto& sload       = code.GetInstructions().At(index);
		const int   dword_count = ShaderTableSLoadDwords(sload);
		int         offset_dw   = 0;
		if (dword_count == 0 || !ShaderLoadsThroughSrtPointer(sload, entry_values[index], srt_size_dw, user_data_register_base, *bind) ||
		    !ShaderGetSmemConstantDwordOffset(sload, &offset_dw) || offset_dw < 0 ||
		    static_cast<uint32_t>(dword_count) > kMaxSrtPointerTableDwords ||
		    static_cast<uint32_t>(offset_dw) > kMaxSrtPointerTableDwords - static_cast<uint32_t>(dword_count))
		{
			continue;
		}
		const auto blocks = ShaderPartitionTableLoad(code, ShaderBuildControlFlowGraph(code), index);
		if (blocks.empty())
		{
			continue;
		}

		const int      pointer = sload.src[0].register_id - user_data_register_base;
		const uint64_t address = ((static_cast<uint64_t>(user_sgpr.value[pointer + 1]) & 0xffffu) << 32u) | user_sgpr.value[pointer];
		const uint32_t table_dwords = static_cast<uint32_t>(offset_dw) + static_cast<uint32_t>(dword_count);
		std::vector<uint32_t> table(table_dwords);
		if (!ShaderSnapshotGuestDescriptorTable(address, table_dwords, &table))
		{
			EXIT("unreadable descriptor table behind an SRT pointer: pc=0x%08" PRIx32 " sgpr=%d address=0x%016" PRIx64
			     " dwords=%d\n",
			     sload.pc, pointer, address, offset_dw + dword_count);
		}
		for (const auto& block: blocks)
		{
			if (!ShaderDescriptorLoadInPass(block.use, pass))
			{
				continue;
			}
			if (!ShaderAddDynamicScalarResource(code, bind, info, block.load, offset_dw + block.first_dword, block.use, user_sgpr,
				                                table.data(), instruction_count, memo))
			{
				EXIT("unable to materialize SRT pointer descriptor: pc=0x%08" PRIx32 " offset_dw=%d dwords=%d kind=%u textures=%d "
				     "samplers=%d\n",
				     sload.pc, offset_dw + block.first_dword, block.load.dst.size, static_cast<unsigned>(block.use.kind),
				     bind->textures2D.textures_num, bind->samplers.samplers_num);
			}
		}
	}
	}
}

// The value a scalar instruction writes into its single destination SGPR when
// every source is a constant.
static bool ShaderOperandIsPlainConstant(const ShaderOperand& operand)
{
	return (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant) &&
	       operand.multiplier == 1.0f && !operand.absolute && !operand.negate;
}

static bool ShaderConstantSgprWrite(const ShaderInstruction& inst, uint32_t* value)
{
	const auto constant = ShaderOperandIsPlainConstant;
	if (inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != 1)
	{
		return false;
	}
	if ((inst.type == ShaderInstructionType::SMovB32 || inst.type == ShaderInstructionType::SMovkI32) && inst.src_num == 1 &&
	    constant(inst.src[0]))
	{
		*value = inst.src[0].constant.u;
		return true;
	}
	if (inst.type == ShaderInstructionType::SBfmB32 && inst.src_num == 2 && constant(inst.src[0]) && constant(inst.src[1]))
	{
		// RDNA2 ISA: D.u = ((1 << S0.u[4:0]) - 1) << S1.u[4:0].
		*value = ((uint32_t {1} << (inst.src[0].constant.u & 31u)) - 1u) << (inst.src[1].constant.u & 31u);
		return true;
	}
	if (inst.type == ShaderInstructionType::SLshlB32 && inst.src_num == 2 && constant(inst.src[0]) && constant(inst.src[1]))
	{
		// RDNA2 ISA: D.u = S0.u << S1.u[4:0].
		*value = inst.src[0].constant.u << (inst.src[1].constant.u & 31u);
		return true;
	}
	return false;
}

// S_MOV_B64 of a constant whose sign bit is clear (the high word is zero either way), or S_BFM_B64.
static bool ShaderConstantSgprPairWrite(const ShaderInstruction& inst, uint64_t* value)
{
	const auto constant = ShaderOperandIsPlainConstant;
	if (inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != 2)
	{
		return false;
	}
	if (inst.type == ShaderInstructionType::SMovB64 && inst.src_num == 1 && constant(inst.src[0]) && (inst.src[0].constant.u >> 31u) == 0u)
	{
		*value = inst.src[0].constant.u;
		return true;
	}
	if (inst.type == ShaderInstructionType::SBfmB64 && inst.src_num == 2 && constant(inst.src[0]) && constant(inst.src[1]))
	{
		// RDNA2 ISA: D.u64 = ((1 << S0.u[5:0]) - 1) << S1.u[5:0].
		*value = ((uint64_t {1} << (inst.src[0].constant.u & 63u)) - 1u) << (inst.src[1].constant.u & 63u);
		return true;
	}
	return false;
}

struct ShaderConstantSgprState
{
	std::array<uint32_t, kShaderScalarLivenessSgprs> values {};
	std::bitset<kShaderScalarLivenessSgprs>          known;
	bool                                             reachable = false;
	bool                                             queued    = false;
};

static void ShaderForgetSgprs(const ShaderOperand& operand, ShaderConstantSgprState* state)
{
	if (operand.type != ShaderOperandType::Sgpr)
	{
		return;
	}
	for (int reg = std::max(operand.register_id, 0); reg < operand.register_id + operand.size && reg < kShaderScalarLivenessSgprs; ++reg)
	{
		state->known.reset(static_cast<size_t>(reg));
	}
}

static void ShaderMergeConstantSgprs(uint32_t index, const ShaderConstantSgprState& outgoing, std::vector<ShaderConstantSgprState>* states,
                                     std::vector<uint32_t>* work)
{
	auto& state = (*states)[index];
	auto  known = outgoing.known;
	if (state.reachable)
	{
		known &= state.known;
		for (int reg = 0; reg < kShaderScalarLivenessSgprs; ++reg)
		{
			if (known.test(static_cast<size_t>(reg)) && state.values[static_cast<size_t>(reg)] != outgoing.values[static_cast<size_t>(reg)])
			{
				known.reset(static_cast<size_t>(reg));
			}
		}
	}
	const bool changed = !state.reachable || known != state.known;
	state.values    = outgoing.values;
	state.known     = known;
	state.reachable = true;
	if (changed && !state.queued)
	{
		state.queued = true;
		work->push_back(index);
	}
}

// A forward flow of the SGPR words constant scalar writes leave; joins keep a
// word only when every incoming path agrees on it.
std::vector<ShaderConstantSamplerRead> ShaderFindConstantSamplerReads(const ShaderCode& code)
{
	std::vector<ShaderConstantSamplerRead> reads;
	const auto&                            instructions = code.GetInstructions();
	const auto                             indices      = ShaderIndexInstructions(code);
	if (indices.image_samplers.empty() || instructions.Size() == 0)
	{
		return reads;
	}
	std::unordered_map<uint32_t, uint32_t> by_pc;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		by_pc.emplace(instructions.At(index).pc, index);
	}
	std::vector<ShaderConstantSgprState> states(instructions.Size());
	std::vector<uint32_t>                work;
	ShaderMergeConstantSgprs(0, ShaderConstantSgprState {}, &states, &work);
	while (!work.empty())
	{
		const uint32_t index = work.back();
		work.pop_back();
		states[index].queued = false;
		const auto& inst     = instructions.At(index);
		// Calls and computed jumps leave the static flow: no word is proven.
		if (inst.type == ShaderInstructionType::SSetpcB64 || inst.type == ShaderInstructionType::SSwappcB64)
		{
			return {};
		}
		auto outgoing = states[index];
		ShaderForgetSgprs(inst.dst, &outgoing);
		ShaderForgetSgprs(inst.dst2, &outgoing);
		uint32_t value = 0;
		if (ShaderConstantSgprWrite(inst, &value))
		{
			outgoing.values[static_cast<size_t>(inst.dst.register_id)] = value;
			outgoing.known.set(static_cast<size_t>(inst.dst.register_id));
		}
		uint64_t pair_value = 0;
		if (ShaderConstantSgprPairWrite(inst, &pair_value))
		{
			const auto reg = static_cast<size_t>(inst.dst.register_id);
			outgoing.values[reg]     = static_cast<uint32_t>(pair_value);
			outgoing.values[reg + 1] = static_cast<uint32_t>(pair_value >> 32u);
			outgoing.known.set(reg);
			outgoing.known.set(reg + 1);
		}
		if (ShaderInstructionHasStaticBranchTarget(inst.type))
		{
			const auto target = by_pc.find(ShaderLabel(inst).GetDst());
			if (target == by_pc.end())
			{
				return {};
			}
			ShaderMergeConstantSgprs(target->second, outgoing, &states, &work);
			if (inst.type == ShaderInstructionType::SBranch)
			{
				continue;
			}
		}
		if (inst.type != ShaderInstructionType::SEndpgm && index + 1u < instructions.Size())
		{
			ShaderMergeConstantSgprs(index + 1u, outgoing, &states, &work);
		}
	}
	for (const uint32_t index: indices.image_samplers)
	{
		const auto& inst  = instructions.At(index);
		const auto& state = states[index];
		const int   first = inst.src[2].register_id;
		if (!state.reachable || inst.src_num < 3 || inst.src[2].type != ShaderOperandType::Sgpr || inst.src[2].size != 4 || first < 0 ||
		    first + 4 > kShaderScalarLivenessSgprs)
		{
			continue;
		}
		ShaderConstantSamplerRead read {index, {}};
		bool                      known = true;
		for (int field = 0; field < 4 && known; ++field)
		{
			known                     = state.known.test(static_cast<size_t>(first + field));
			read.resource.fields[field] = state.values[static_cast<size_t>(first + field)];
		}
		if (known)
		{
			reads.push_back(read);
		}
	}
	return reads;
}

const std::vector<ShaderConstantSamplerRead>& ShaderCodeResourceMemo::ConstantSamplerReads(const ShaderCode& code) const
{
	std::call_once(m_constant_sampler_reads_once, [&] { m_constant_sampler_reads = ShaderFindConstantSamplerReads(code); });
	return m_constant_sampler_reads;
}

// Binds an S# whose words are fixed for the draw to the image read inst, reusing
// an equal sampler: the words are not in user data, so they bind like a loaded
// S#, from the recorded fields.
static void ShaderBindFixedSampler(ShaderBindResources* bind, ShaderParsedUsage* info, const ShaderInstruction& inst,
                                   const ShaderSamplerResource& resource, int user_data_register_base)
{
	const auto operation     = ShaderInstructionSamplerOperation(inst.type);
	int        sampler_index = -1;
	for (int index = 0; index < bind->samplers.samplers_num; ++index)
	{
		if (bind->samplers.dynamic_sload[index] && ShaderSamplerResourcesEqual(bind->samplers.samplers[index], resource) &&
		    bind->samplers.operations[index] == operation)
		{
			sampler_index = index;
			break;
		}
	}
	if (sampler_index < 0)
	{
		if (bind->samplers.samplers_num >= ShaderSamplerResources::RES_MAX)
		{
			EXIT("shader sampler resource capacity exceeded by a fixed sampler: pc=0x%08" PRIx32 "\n", inst.pc);
		}
		sampler_index                                = bind->samplers.samplers_num++;
		bind->samplers.samplers[sampler_index]       = resource;
		bind->samplers.operations[sampler_index]     = operation;
		bind->samplers.slots[sampler_index]          = -1;
		bind->samplers.start_register[sampler_index] = inst.src[2].register_id - user_data_register_base;
		bind->samplers.extended[sampler_index]       = false;
		bind->samplers.dynamic_sload[sampler_index]  = true;
		info->samplers++;
	}
	bind->constant_samplers.Add(ShaderConstantSampler {inst.pc, sampler_index});
}

void ShaderCollectConstantSamplers(const ShaderCode& code, ShaderBindResources* bind, ShaderParsedUsage* info,
                                   int user_data_register_base, const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(bind == nullptr || info == nullptr);
	bind->constant_samplers.Clear();
	const auto  direct = memo == nullptr ? ShaderFindConstantSamplerReads(code) : std::vector<ShaderConstantSamplerRead> {};
	const auto& reads  = memo != nullptr ? memo->ConstantSamplerReads(code) : direct;
	for (const auto& read: reads)
	{
		const auto& inst     = code.GetInstructions().At(read.instruction);
		const auto& resource = read.resource;
		if (ShaderFindImageSamplerDescriptor(inst, *bind, user_data_register_base) >= 0)
		{
			continue;
		}
		ShaderBindFixedSampler(bind, info, inst, resource, user_data_register_base);
	}
}

// Writes of the base a scalar load reads through or of a destination range:
// SGPRs, or the VCC pair.
static bool ShaderInstructionWritesLoadBase(const ShaderInstruction& inst, const ShaderOperand& base)
{
	if (base.type == ShaderOperandType::Sgpr)
	{
		return ShaderInstructionWritesSgprRange(inst, base.register_id, base.size);
	}
	// The VCC halves are separate registers; a VccLo operand of two dwords is both.
	const auto halves = [](const ShaderOperand& operand) -> uint32_t
	{
		if (operand.type == ShaderOperandType::VccLo)
		{
			return operand.size >= 2 ? 3u : 1u;
		}
		return operand.type == ShaderOperandType::VccHi ? 2u : 0u;
	};
	return (halves(base) & (halves(inst.dst) | halves(inst.dst2))) != 0u;
}

constexpr uint8_t kShaderReachLoad  = 1u;
constexpr uint8_t kShaderReachOther = 2u;

static uint8_t ShaderReachTransfer(const ShaderCode& code, uint32_t index, uint32_t load_index, const ShaderOperand& range, uint8_t holds)
{
	if (index == load_index)
	{
		return kShaderReachLoad;
	}
	return ShaderInstructionWritesLoadBase(code.GetInstructions().At(index), range) ? kShaderReachOther : holds;
}

static uint32_t ShaderCfgBlockFirstIndex(const ShaderControlFlowGraph& cfg, uint32_t block)
{
	return block == 0 ? 0u : cfg.blocks[block - 1].last + 1u;
}

// What range holds at each CFG block entry: the value the instruction at
// load_index writes, another value, both, or nothing on an unreached block.
static std::vector<uint8_t> ShaderReachingWriteEntries(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t load_index,
                                                       const ShaderOperand& range)
{
	const auto&           blocks = cfg.blocks;
	std::vector<uint8_t>  entry(blocks.size(), 0u);
	std::vector<uint32_t> work {0u};
	entry[0] = kShaderReachOther;
	while (!work.empty())
	{
		const uint32_t b = work.back();
		work.pop_back();
		uint8_t holds = entry[b];
		for (uint32_t index = ShaderCfgBlockFirstIndex(cfg, b); index <= blocks[b].last; ++index)
		{
			holds = ShaderReachTransfer(code, index, load_index, range, holds);
		}
		for (const uint32_t succ: blocks[b].succ)
		{
			if (succ != kShaderCfgNone && (entry[succ] | holds) != entry[succ])
			{
				entry[succ] |= holds;
				work.push_back(succ);
			}
		}
	}
	return entry;
}

// The consumers of the descriptor block a table load fills, over every CFG
// path: a read of the block consumes the load only when the load is its last
// write on every path. Consumers are later matched by PC range, so a read in
// that range that may see another value makes the use invalid.
static ShaderDynamicSLoadUse ShaderFindReachingSLoadUse(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t load_index,
                                                        const ShaderInstruction& block)
{
	const auto& instructions = code.GetInstructions();
	const auto& blocks       = cfg.blocks;
	const auto  entry        = ShaderReachingWriteEntries(code, cfg, load_index, block.dst);

	ShaderDynamicSLoadUse use {};
	uint32_t              first_foreign_read = UINT32_MAX;
	for (uint32_t b = 0; b < blocks.size() && use.valid; ++b)
	{
		uint8_t holds = entry[b];
		if (holds == 0u)
		{
			continue;
		}
		for (uint32_t index = ShaderCfgBlockFirstIndex(cfg, b); index <= blocks[b].last && use.valid; ++index)
		{
			const auto&           inst     = instructions.At(index);
			ShaderDynamicSLoadUse next_use {};
			if (index == load_index || !ShaderInstructionReadsSgprRange(inst, block.dst.register_id, block.dst.size))
			{
				holds = ShaderReachTransfer(code, index, load_index, block.dst, holds);
				continue;
			}
			if (holds == kShaderReachOther)
			{
				first_foreign_read = std::min(first_foreign_read, inst.pc);
			} else
			{
				use.valid = holds == kShaderReachLoad && inst.pc > block.pc && ShaderDynamicSLoadMatchesConsumer(inst, block, &next_use) &&
				            ShaderMergeDynamicSLoadUse(&use, next_use, inst.pc);
			}
			holds = ShaderReachTransfer(code, index, load_index, block.dst, holds);
		}
	}
	if (use.found && first_foreign_read > block.pc && first_foreign_read <= use.last_consumer_pc)
	{
		use.valid = false;
	}
	return ShaderResolveTextureSizeQueries(use);
}

enum class ShaderGsUserDataBufferPolicy
{
	None,
	ReadOnlyDword,
	All,
};

// Keeps the T# and S# blocks of a GS user-data load, and V# blocks permitted by
// buffer_policy, analysed as loads of their own; the other words stay plain guest data.
static void ShaderPartitionGsUserDataLoad(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t index,
                                          const ShaderInstruction& sload, int table_offset_dw, int first_dword, int dwords,
                                          ShaderGsUserDataBufferPolicy buffer_policy, std::vector<ShaderGsUserDataDescriptorLoad>* loads)
{
	if (dwords == 16)
	{
		ShaderPartitionGsUserDataLoad(code, cfg, index, sload, table_offset_dw, first_dword, 8, buffer_policy, loads);
		ShaderPartitionGsUserDataLoad(code, cfg, index, sload, table_offset_dw, first_dword + 8, 8, buffer_policy, loads);
		return;
	}
	ShaderInstruction block = sload;
	block.type              = dwords == 4 ? ShaderInstructionType::SLoadDwordx4 : ShaderInstructionType::SLoadDwordx8;
	block.dst.register_id   = sload.dst.register_id + first_dword;
	block.dst.size          = dwords;
	const auto use          = ShaderFindReachingSLoadUse(code, cfg, index, block);
	bool       keep_buffer  = false;
	if (dwords == 4)
	{
		if (buffer_policy == ShaderGsUserDataBufferPolicy::All)
		{
			keep_buffer = true;
		} else if (buffer_policy == ShaderGsUserDataBufferPolicy::ReadOnlyDword)
		{
			keep_buffer = use.storage_usage == ShaderStorageUsage::ReadOnly && use.raw_only && use.dword_loads_only &&
			              (use.raw_vmem_oob_guarded || use.raw_smem_use);
		}
	}
	const bool kept_kind = use.kind != ShaderDynamicSLoadResourceKind::StorageBuffer || keep_buffer;
	if (use.valid && use.found && kept_kind)
	{
		loads->push_back({block, table_offset_dw + first_dword, use});
		return;
	}
	if (dwords == 8)
	{
		ShaderPartitionGsUserDataLoad(code, cfg, index, sload, table_offset_dw, first_dword, 4, buffer_policy, loads);
		ShaderPartitionGsUserDataLoad(code, cfg, index, sload, table_offset_dw, first_dword + 4, 4, buffer_policy, loads);
	}
}

std::vector<ShaderGsUserDataDescriptorLoad> ShaderFindGsUserDataDescriptorLoads(const ShaderCode& code)
{
	std::vector<ShaderGsUserDataDescriptorLoad> loads;
	const auto                                  cfg = ShaderBuildControlFlowGraph(code);
	if (!cfg.Structurable())
	{
		return loads;
	}
	const auto      flow         = ShaderScalarFlowOf(code);
	const auto&     entry_values = flow->holding_entry_value;
	const uint32_t  count        = code.GetInstructions().Size();
	for (uint32_t index = 0; index < count && index < entry_values.size(); ++index)
	{
		const auto& sload       = code.GetInstructions().At(index);
		const int   dword_count = ShaderTableSLoadDwords(sload);
		int         offset_dw   = 0;
		if (dword_count == 0 || sload.src[0].register_id != 0 || !entry_values[index].test(0) || !entry_values[index].test(1) ||
		    !ShaderGetSmemConstantDwordOffset(sload, &offset_dw) || offset_dw < 0 ||
		    static_cast<uint32_t>(offset_dw) > kMaxSrtPointerTableDwords - static_cast<uint32_t>(dword_count))
		{
			continue;
		}
		ShaderPartitionGsUserDataLoad(code, cfg, index, sload, offset_dw, 0, dword_count,
		                              ShaderGsUserDataBufferPolicy::ReadOnlyDword, &loads);
	}
	return loads;
}

// Dwords an S_LOAD_DWORD* writes, or 0 for any other instruction.
static int ShaderScalarLoadDwords(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SLoadDwordx2: return 2;
		case ShaderInstructionType::SLoadDwordx4: return 4;
		case ShaderInstructionType::SLoadDwordx8: return 8;
		case ShaderInstructionType::SLoadDwordx16: return 16;
		default: return 0;
	}
}

// The scalar load at a constant offset whose destination holds the base pair
// of the table load at index and that is the only write of that base reaching
// it, or UINT32_MAX. The pointer is read from its load's dwords at the pair.
static uint32_t ShaderFindTablePointerLoad(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t index,
                                           int* pointer_offset_dw)
{
	const auto&    instructions = code.GetInstructions();
	const auto&    base         = instructions.At(index).src[0];
	const uint32_t block        = cfg.BlockAt(instructions.At(index).pc);
	if (block == kShaderCfgNone || base.size != 2)
	{
		return UINT32_MAX;
	}
	for (uint32_t candidate = 0; candidate < instructions.Size(); ++candidate)
	{
		const auto& inst      = instructions.At(candidate);
		const int   dwords    = ShaderScalarLoadDwords(inst.type);
		const int   pair_word = base.type == ShaderOperandType::Sgpr ? base.register_id - inst.dst.register_id : 0;
		int         load_dw   = 0;
		if (candidate == index || dwords == 0 || inst.dst.type != base.type || inst.dst.size != dwords || pair_word < 0 ||
		    pair_word > dwords - 2 || inst.src_num < 2 || inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].size != 2 ||
		    !ShaderGetSmemConstantDwordOffset(inst, &load_dw) || load_dw < 0 ||
		    static_cast<uint32_t>(load_dw + pair_word) > kMaxSrtPointerTableDwords - 2u)
		{
			continue;
		}
		*pointer_offset_dw = load_dw + pair_word;
		uint8_t holds = ShaderReachingWriteEntries(code, cfg, candidate, base)[block];
		for (uint32_t at = ShaderCfgBlockFirstIndex(cfg, block); at < index; ++at)
		{
			holds = ShaderReachTransfer(code, at, candidate, base, holds);
		}
		if (holds == kShaderReachLoad)
		{
			return candidate;
		}
	}
	return UINT32_MAX;
}

// Follows a scalar move of one SGPR back to the SGPR it copies.
static bool ShaderTraceScalarMove(const ShaderInstruction& inst, int* reg)
{
	const auto& src  = inst.src[0];
	const int   size = inst.type == ShaderInstructionType::SMovB32 ? 1 : inst.type == ShaderInstructionType::SMovB64 ? 2 : 0;
	if (size == 0 || inst.src_num != 1 || inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != size ||
	    src.type != ShaderOperandType::Sgpr || src.size != size)
	{
		return false;
	}
	*reg = src.register_id + (*reg - inst.dst.register_id);
	return true;
}

// Walks back, within the CFG block of an image consumer, through scalar moves
// to the loads that write each dword of its T#. They must read eight
// consecutive dwords of one table through one pointer load; a T# one load
// writes in place is left to the load partition.
static bool ShaderFindCopiedNestedTexture(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t consumer_index,
                                          ShaderNestedTableLoad* found)
{
	const auto&    instructions = code.GetInstructions();
	const auto&    consumer     = instructions.At(consumer_index);
	const uint32_t block        = cfg.BlockAt(consumer.pc);
	if (block == kShaderCfgNone || consumer.src_num < 2 || consumer.src[1].type != ShaderOperandType::Sgpr || consumer.src[1].size != 8)
	{
		return false;
	}
	std::array<int, 8>      reg {};
	std::array<uint32_t, 8> load {};
	std::array<int, 8>      load_word {};
	std::array<int, 8>      table_dw {};
	for (int word = 0; word < 8; ++word)
	{
		reg[word] = consumer.src[1].register_id + word;
	}
	int  open   = 8;
	bool copied = false;
	for (uint32_t cursor = consumer_index; open > 0 && cursor-- > ShaderCfgBlockFirstIndex(cfg, block);)
	{
		const auto& inst = instructions.At(cursor);
		for (int word = 0; word < 8; ++word)
		{
			if (reg[word] < 0 || !ShaderInstructionWritesSgprRange(inst, reg[word], 1))
			{
				continue;
			}
			if (ShaderTraceScalarMove(inst, &reg[word]))
			{
				copied = true;
				continue;
			}
			const int dwords    = ShaderScalarLoadDwords(inst.type);
			int       offset_dw = 0;
			if (dwords == 0 || inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != dwords || inst.src_num < 2 ||
			    !ShaderGetSmemConstantDwordOffset(inst, &offset_dw) || offset_dw < 0 ||
			    static_cast<uint32_t>(offset_dw) > kMaxSrtPointerTableDwords - static_cast<uint32_t>(dwords))
			{
				return false;
			}
			load[word]      = cursor;
			load_word[word] = reg[word] - inst.dst.register_id;
			table_dw[word]  = offset_dw + load_word[word];
			reg[word]      = -1;
			open--;
		}
	}
	if (open != 0 || (!copied && std::all_of(load.begin(), load.end(), [&](uint32_t index) { return index == load[0]; })))
	{
		return false;
	}
	int      pointer_offset_dw = 0;
	uint32_t pointer_index     = UINT32_MAX;
	ShaderNestedCopiedTexture texture {consumer_index, table_dw[0], {}};
	for (int word = 0; word < 8; ++word)
	{
		if (table_dw[word] != table_dw[0] + word)
		{
			return false;
		}
		if (word > 0 && load[word] == load[word - 1])
		{
			texture.pieces.back().block.dst.size++;
			continue;
		}
		int piece_pointer_offset_dw = 0;
		const uint32_t piece_pointer = ShaderFindTablePointerLoad(code, cfg, load[word], &piece_pointer_offset_dw);
		if (piece_pointer == UINT32_MAX || (word > 0 && (piece_pointer != pointer_index || piece_pointer_offset_dw != pointer_offset_dw)))
		{
			return false;
		}
		pointer_index     = piece_pointer;
		pointer_offset_dw = piece_pointer_offset_dw;
		ShaderNestedTexturePiece piece {instructions.At(load[word]), table_dw[word], word};
		piece.block.dst.register_id += load_word[word];
		piece.block.dst.size = 1;
		texture.pieces.push_back(piece);
	}
	found->pointer_index     = pointer_index;
	found->pointer_offset_dw = pointer_offset_dw;
	found->copied_textures   = {texture};
	return true;
}

// The V# operand of a MUBUF/MTBUF access, or nullptr.
static const ShaderOperand* ShaderVectorBufferDescriptor(const ShaderInstruction& inst)
{
	if (!ShaderInstructionIsVectorBufferAccess(inst.type) || inst.src_num < 2 ||
	    inst.src[1].type != ShaderOperandType::Sgpr || inst.src[1].size != 4)
	{
		return nullptr;
	}
	return &inst.src[1];
}

// The only write of an SGPR range or of VCC that reaches the instruction at
// index on every path, or UINT32_MAX.
static uint32_t ShaderUniqueReachingWrite(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t index,
                                          const ShaderOperand& range)
{
	const auto&    instructions = code.GetInstructions();
	const uint32_t block        = cfg.BlockAt(instructions.At(index).pc);
	if (block == kShaderCfgNone)
	{
		return UINT32_MAX;
	}
	for (uint32_t candidate = 0; candidate < instructions.Size(); ++candidate)
	{
		if (!ShaderInstructionWritesLoadBase(instructions.At(candidate), range))
		{
			continue;
		}
		uint8_t holds = ShaderReachingWriteEntries(code, cfg, candidate, range)[block];
		for (uint32_t at = ShaderCfgBlockFirstIndex(cfg, block); at < index; ++at)
		{
			holds = ShaderReachTransfer(code, at, candidate, range, holds);
		}
		if (holds == kShaderReachLoad)
		{
			return candidate;
		}
	}
	return UINT32_MAX;
}

static uint32_t ShaderUniqueReachingSgprWrite(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t index, int reg)
{
	ShaderOperand range;
	range.type        = ShaderOperandType::Sgpr;
	range.register_id = reg;
	range.size        = 1;
	return ShaderUniqueReachingWrite(code, cfg, index, range);
}

bool ShaderCodeResourceMemo::SgprWrittenOnEveryPath(const ShaderCode& code, uint32_t index, int reg) const
{
	std::shared_ptr<const ShaderControlFlowGraph> cfg;
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		const auto                          found = m_written_on_every_path.find({index, reg});
		if (found != m_written_on_every_path.end())
		{
			return found->second;
		}
		cfg = m_cfg;
	}
	if (cfg == nullptr)
	{
		cfg = std::make_shared<const ShaderControlFlowGraph>(ShaderBuildControlFlowGraph(code));
	}
	const bool                          written = ShaderUniqueReachingSgprWrite(code, *cfg, index, reg) != UINT32_MAX;
	std::unique_lock<std::shared_mutex> lock(m_mutex);
	if (m_cfg == nullptr)
	{
		m_cfg = cfg;
	}
	return m_written_on_every_path.emplace(std::make_pair(index, reg), written).first->second;
}

// The one instruction whose write of every SGPR in [first, first + count)
// reaches index on every path, or UINT32_MAX.
static uint32_t ShaderUniqueReachingRangeWrite(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t index, int first,
                                               int count)
{
	uint32_t writer = UINT32_MAX;
	for (int reg = first; reg < first + count; ++reg)
	{
		const uint32_t write = ShaderUniqueReachingSgprWrite(code, cfg, index, reg);
		if (write == UINT32_MAX || (reg != first && write != writer))
		{
			return UINT32_MAX;
		}
		writer = write;
	}
	return writer;
}

static bool ShaderIsScalarOffsetRegister(const ShaderOperand& operand)
{
	return (operand.type == ShaderOperandType::Sgpr && operand.size == 1) || operand.type == ShaderOperandType::VccLo;
}

static bool ShaderTableSamplerOffsetShift(const ShaderInstruction& shift, const ShaderOperand& offset)
{
	return shift.type == ShaderInstructionType::SLshlB32 && shift.src_num == 2 && shift.dst.type == offset.type &&
	       (offset.type != ShaderOperandType::Sgpr || (shift.dst.register_id == offset.register_id && shift.dst.size == 1)) &&
	       shift.src[0].type == ShaderOperandType::Sgpr && shift.src[0].size == 1 && ShaderOperandIsPlainConstant(shift.src[1]) &&
	       shift.src[1].constant.u < 32u;
}

std::vector<ShaderTableSamplerRead> ShaderFindTableSamplerReads(const ShaderCode& code)
{
	std::vector<ShaderTableSamplerRead> reads;
	const auto&                         instructions = code.GetInstructions();
	const auto                          indices      = ShaderIndexInstructions(code);
	if (indices.image_samplers.empty())
	{
		return reads;
	}
	const auto cfg = ShaderBuildControlFlowGraph(code);
	for (const uint32_t index: indices.image_samplers)
	{
		const auto& inst = instructions.At(index);
		if (inst.src_num < 3 || inst.src[2].type != ShaderOperandType::Sgpr || inst.src[2].size != 4)
		{
			continue;
		}
		const uint32_t record_load = ShaderUniqueReachingRangeWrite(code, cfg, index, inst.src[2].register_id, 4);
		if (record_load == UINT32_MAX)
		{
			continue;
		}
		const auto& load = instructions.At(record_load);
		if (load.type != ShaderInstructionType::SBufferLoadDwordx4 || load.dst.type != ShaderOperandType::Sgpr ||
		    load.dst.register_id != inst.src[2].register_id || load.src_num < 2 || load.src[0].type != ShaderOperandType::Sgpr ||
		    load.src[0].size != 4 || !ShaderIsScalarOffsetRegister(load.src[1]))
		{
			continue;
		}
		const uint32_t table_load = ShaderUniqueReachingRangeWrite(code, cfg, record_load, load.src[0].register_id, 4);
		const uint32_t shift      = ShaderUniqueReachingWrite(code, cfg, record_load, load.src[1]);
		if (table_load == UINT32_MAX || shift == UINT32_MAX || !ShaderTableSamplerOffsetShift(instructions.At(shift), load.src[1]))
		{
			continue;
		}
		const auto& vsharp       = instructions.At(table_load);
		const int   vsharp_dword = load.src[0].register_id - vsharp.dst.register_id;
		int         offset_dw    = 0;
		if (ShaderTableSLoadDwords(vsharp) == 0 || vsharp_dword < 0 || vsharp_dword + 4 > vsharp.dst.size ||
		    !ShaderGetSmemConstantDwordOffset(vsharp, &offset_dw) || offset_dw < 0)
		{
			continue;
		}
		reads.push_back(ShaderTableSamplerRead {index, record_load, table_load, vsharp_dword, shift});
	}
	return reads;
}

const std::vector<ShaderTableSamplerRead>& ShaderCodeResourceMemo::TableSamplerReads(const ShaderCode& code) const
{
	std::call_once(m_table_sampler_reads_once, [&] { m_table_sampler_reads = ShaderFindTableSamplerReads(code); });
	return m_table_sampler_reads;
}

// The S# record a table sampler read takes for this draw. The record offset is
// OFFSET + SOFFSET in bytes, dword-aligned, and must lie inside the V#.
static ShaderSamplerResource ShaderReadTableSampler(const ShaderCode& code, const ShaderTableSamplerRead& read,
                                                    const HW::UserSgprInfo& user_sgpr, int user_data_register_base)
{
	const auto& instructions = code.GetInstructions();
	const auto& table_load   = instructions.At(read.table_load);
	const auto& record_load  = instructions.At(read.record_load);
	const auto& shift        = instructions.At(read.offset_shift);
	int         offset_dw    = 0;
	EXIT_IF(!ShaderGetSmemConstantDwordOffset(table_load, &offset_dw));
	const int      pointer = table_load.src[0].register_id - user_data_register_base;
	const uint64_t table   = ((static_cast<uint64_t>(user_sgpr.value[pointer + 1]) & 0xffffu) << 32u) | user_sgpr.value[pointer];
	const uint64_t vsharp_address =
	    table + (static_cast<uint64_t>(offset_dw) + static_cast<uint64_t>(read.vsharp_dword)) * sizeof(uint32_t);
	ShaderBufferResource vsharp {};
	if (!ShaderSnapshotGuestDescriptorTable(vsharp_address, 4, vsharp.fields))
	{
		EXIT("unreadable sampler-table V#: pc=0x%08" PRIx32 " address=0x%016" PRIx64 "\n", table_load.pc, vsharp_address);
	}
	const uint32_t index  = user_sgpr.value[shift.src[0].register_id - user_data_register_base];
	const int64_t  offset = static_cast<int64_t>(index << shift.src[1].constant.u) + record_load.smem_imm_offset;
	const uint64_t bytes  = ShaderBufferByteSize(vsharp.Stride(), vsharp.NumRecords());
	if (offset < 0 || static_cast<uint64_t>(offset) + sizeof(ShaderSamplerResource::fields) > bytes)
	{
		EXIT("sampler-table record outside its V#: pc=0x%08" PRIx32 " offset=%" PRId64 " bytes=%" PRIu64 "\n", record_load.pc, offset,
		     bytes);
	}
	const uint64_t        address = (vsharp.Base48() + static_cast<uint64_t>(offset)) & ~uint64_t {3};
	ShaderSamplerResource resource {};
	if (!ShaderSnapshotGuestDescriptorTable(address, 4, resource.fields))
	{
		EXIT("unreadable sampler-table record: pc=0x%08" PRIx32 " address=0x%016" PRIx64 "\n", record_load.pc, address);
	}
	return resource;
}

void ShaderCollectTableSamplers(const ShaderCode& code, ShaderBindResources* bind, ShaderParsedUsage* info, const HW::UserSgprInfo& user_sgpr,
                                uint16_t srt_size_dw, int user_data_register_base, const ShaderScalarFlow& scalar_flow,
                                const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(bind == nullptr || info == nullptr);
	const auto  direct       = memo == nullptr ? ShaderFindTableSamplerReads(code) : std::vector<ShaderTableSamplerRead> {};
	const auto& reads        = memo != nullptr ? memo->TableSamplerReads(code) : direct;
	const auto& entry_values = scalar_flow.holding_entry_value;
	const auto& instructions = code.GetInstructions();
	for (const auto& read: reads)
	{
		const auto& inst        = instructions.At(read.instruction);
		const auto& shift       = instructions.At(read.offset_shift);
		const int   index_sgpr  = shift.src[0].register_id - user_data_register_base;
		const bool  entry_known = read.table_load < entry_values.size() && read.offset_shift < entry_values.size();
		// The pointer and the index must be the draw's user data where they are read.
		if (!entry_known || ShaderFindImageSamplerDescriptor(inst, *bind, user_data_register_base) >= 0 ||
		    !ShaderLoadsThroughSrtPointer(instructions.At(read.table_load), entry_values[read.table_load], srt_size_dw,
		                                  user_data_register_base, *bind) ||
		    index_sgpr < 0 || index_sgpr >= static_cast<int>(srt_size_dw) || index_sgpr >= HW::UserSgprInfo::SGPRS_MAX ||
		    !entry_values[read.offset_shift].test(static_cast<size_t>(shift.src[0].register_id)))
		{
			continue;
		}
		ShaderBindFixedSampler(bind, info, inst, ShaderReadTableSampler(code, read, user_sgpr, user_data_register_base),
		                       user_data_register_base);
	}
}

// Descriptor heap reads (ShaderHeapDescriptorRead).

constexpr int      kShaderScalarChainDepth = 12;
constexpr uint32_t kShaderHeapRecordsMax   = 32;

// The dword of dst that holds the one-dword operand, or -1.
static int ShaderDestinationWord(const ShaderOperand& dst, const ShaderOperand& operand)
{
	if (dst.type == ShaderOperandType::Sgpr)
	{
		const int word = operand.register_id - dst.register_id;
		return operand.type == ShaderOperandType::Sgpr && word >= 0 && word < dst.size ? word : -1;
	}
	if (dst.type == operand.type && (dst.type == ShaderOperandType::VccLo || dst.type == ShaderOperandType::VccHi))
	{
		return 0;
	}
	return dst.type == ShaderOperandType::VccLo && dst.size == 2 && operand.type == ShaderOperandType::VccHi ? 1 : -1;
}

// Dwords an S_LOAD or S_BUFFER_LOAD writes, or 0; buffer tells which of the two it is.
static int ShaderScalarMemoryLoadDwords(ShaderInstructionType type, bool* buffer)
{
	*buffer = false;
	switch (type)
	{
		case ShaderInstructionType::SLoadDword: return 1;
		case ShaderInstructionType::SLoadDwordx2: return 2;
		case ShaderInstructionType::SLoadDwordx4: return 4;
		case ShaderInstructionType::SLoadDwordx8: return 8;
		case ShaderInstructionType::SLoadDwordx16: return 16;
		default: break;
	}
	*buffer = true;
	switch (type)
	{
		case ShaderInstructionType::SBufferLoadDword: return 1;
		case ShaderInstructionType::SBufferLoadDwordx2: return 2;
		case ShaderInstructionType::SBufferLoadDwordx4: return 4;
		case ShaderInstructionType::SBufferLoadDwordx8: return 8;
		case ShaderInstructionType::SBufferLoadDwordx16: return 16;
		default: return 0;
	}
}

static bool ShaderIsOneDwordScalarRegister(const ShaderOperand& operand)
{
	return operand.size == 1 && (operand.type == ShaderOperandType::Sgpr || operand.type == ShaderOperandType::VccLo ||
	                             operand.type == ShaderOperandType::VccHi);
}

// Follows a scalar value back through the only write reaching each read of it,
// into the nodes ShaderEvalScalarChain evaluates for a draw.
class ShaderScalarChainBuilder
{
public:
	ShaderScalarChainBuilder(const ShaderCode& code, const ShaderControlFlowGraph& cfg): m_code(code), m_cfg(cfg) {}

	// Dwords [word, word + count) of what the S_LOAD or S_BUFFER_LOAD at index writes.
	int MemoryLoad(uint32_t index, int word, int count, int depth)
	{
		const auto& inst   = m_code.GetInstructions().At(index);
		bool        buffer = false;
		const int   dwords = ShaderScalarMemoryLoadDwords(inst.type, &buffer);
		if (depth > kShaderScalarChainDepth || dwords == 0 || inst.dst.size != dwords || word < 0 || word + count > dwords ||
		    inst.src_num < 2 || inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].size != (buffer ? 4 : 2))
		{
			return -1;
		}
		if (!buffer)
		{
			int       offset_dw = 0;
			const int pointer   = Words(index, inst.src[0].register_id, 2, depth + 1);
			if (pointer < 0 || !ShaderGetSmemConstantDwordOffset(inst, &offset_dw))
			{
				return -1;
			}
			return Add({.op = ShaderScalarChainOp::Load, .source = pointer, .imm = (offset_dw + word) * 4, .dwords = count});
		}
		const int vsharp = Words(index, inst.src[0].register_id, 4, depth + 1);
		const int offset = BufferOffset(index, inst.src[1], depth + 1);
		if (vsharp < 0 || offset < 0)
		{
			return -1;
		}
		const int node = Add({.op     = ShaderScalarChainOp::BufferLoad,
		                      .source = vsharp,
		                      .offset = offset,
		                      .imm    = inst.smem_imm_offset + word * 4,
		                      .dwords = count});
		if (nodes[offset].op == ShaderScalarChainOp::RecordIndex && domain < 0)
		{
			domain = node;
		}
		return node;
	}

	std::vector<ShaderScalarChainNode> nodes;
	uint32_t                           slot   = UINT32_MAX;
	int                                domain = -1;

private:
	int Add(const ShaderScalarChainNode& node)
	{
		nodes.push_back(node);
		return static_cast<int>(nodes.size()) - 1;
	}

	// The value of a one-dword operand where the instruction at index reads it.
	int Value(uint32_t index, const ShaderOperand& operand, int depth)
	{
		if (ShaderOperandIsPlainConstant(operand))
		{
			return Add({.op = ShaderScalarChainOp::Constant, .value = operand.constant.u});
		}
		if (depth > kShaderScalarChainDepth || !ShaderIsOneDwordScalarRegister(operand))
		{
			return -1;
		}
		const uint32_t writer = ShaderUniqueReachingWrite(m_code, m_cfg, index, operand);
		if (writer == UINT32_MAX)
		{
			return operand.type == ShaderOperandType::Sgpr
			           ? Add({.op = ShaderScalarChainOp::UserData, .value = static_cast<uint32_t>(operand.register_id), .at = index})
			           : -1;
		}
		const auto& inst = m_code.GetInstructions().At(writer);
		const int   word = ShaderDestinationWord(inst.dst, operand);
		if (word < 0)
		{
			return -1;
		}
		if (inst.type == ShaderInstructionType::SMovB32 && inst.src_num == 1)
		{
			return Value(writer, inst.src[0], depth + 1);
		}
		if (inst.type == ShaderInstructionType::SLshlB32 && inst.src_num == 2 && ShaderOperandIsPlainConstant(inst.src[1]) &&
		    inst.src[1].constant.u < 32u)
		{
			const int source = Value(writer, inst.src[0], depth + 1);
			return source < 0 ? -1 : Add({.op = ShaderScalarChainOp::ShiftLeft, .value = inst.src[1].constant.u, .source = source});
		}
		return MemoryLoad(writer, word, 1, depth + 1);
	}

	// SGPRs [first, first + count) where the instruction at index reads them.
	int Words(uint32_t index, int first, int count, int depth)
	{
		if (depth > kShaderScalarChainDepth || first < 0 || first + count > kShaderScalarLivenessSgprs)
		{
			return -1;
		}
		const uint32_t writer = ShaderUniqueReachingRangeWrite(m_code, m_cfg, index, first, count);
		if (writer == UINT32_MAX)
		{
			return Add({.op = ShaderScalarChainOp::UserData, .value = static_cast<uint32_t>(first), .at = index, .dwords = count});
		}
		const auto& inst = m_code.GetInstructions().At(writer);
		return inst.dst.type == ShaderOperandType::Sgpr ? MemoryLoad(writer, first - inst.dst.register_id, count, depth + 1) : -1;
	}

	// The SOFFSET of an S_BUFFER_LOAD. An S_MUL_I32 of an SGPR by a constant
	// stride makes it the record index the read walks; one read walks one index.
	int BufferOffset(uint32_t index, const ShaderOperand& operand, int depth)
	{
		const uint32_t writer =
		    ShaderIsOneDwordScalarRegister(operand) ? ShaderUniqueReachingWrite(m_code, m_cfg, index, operand) : UINT32_MAX;
		if (writer == UINT32_MAX || m_code.GetInstructions().At(writer).type != ShaderInstructionType::SMulI32)
		{
			return Value(index, operand, depth);
		}
		const auto& mul    = m_code.GetInstructions().At(writer);
		const int   stride = mul.src_num != 2 ? -1 : (ShaderOperandIsPlainConstant(mul.src[1]) ? 1 : (ShaderOperandIsPlainConstant(mul.src[0]) ? 0 : -1));
		if (stride < 0 || mul.src[stride].constant.u == 0 || !ShaderIsOneDwordScalarRegister(mul.src[1 - stride]) ||
		    ShaderDestinationWord(mul.dst, operand) != 0 || (slot != UINT32_MAX && slot != writer))
		{
			return -1;
		}
		slot = writer;
		return Add({.op = ShaderScalarChainOp::RecordIndex, .value = mul.src[stride].constant.u});
	}

	const ShaderCode&             m_code;
	const ShaderControlFlowGraph& m_cfg;
};

bool ShaderEvalScalarChain(const std::vector<ShaderScalarChainNode>& nodes, int node, uint32_t record, const HW::UserSgprInfo& user_sgpr,
                           uint16_t srt_size_dw, int user_data_register_base, const std::vector<std::bitset<106>>& entry_values,
                           uint32_t* words)
{
	EXIT_IF(node < 0 || node >= static_cast<int>(nodes.size()) || words == nullptr);
	const auto& n    = nodes[node];
	const auto  eval = [&](int child, uint32_t* out)
	{ return ShaderEvalScalarChain(nodes, child, record, user_sgpr, srt_size_dw, user_data_register_base, entry_values, out); };
	switch (n.op)
	{
		case ShaderScalarChainOp::Constant: words[0] = n.value; return true;
		case ShaderScalarChainOp::RecordIndex: words[0] = record * n.value; return true;
		case ShaderScalarChainOp::ShiftLeft:
			if (!eval(n.source, words))
			{
				return false;
			}
			words[0] <<= n.value;
			return true;
		case ShaderScalarChainOp::UserData:
			for (int word = 0; word < n.dwords; ++word)
			{
				const int reg  = static_cast<int>(n.value) + word;
				const int user = reg - user_data_register_base;
				if (n.at >= entry_values.size() || reg >= kShaderScalarLivenessSgprs || !entry_values[n.at].test(static_cast<size_t>(reg)) ||
				    user < 0 || user >= static_cast<int>(srt_size_dw) || user >= HW::UserSgprInfo::SGPRS_MAX)
				{
					return false;
				}
				words[word] = user_sgpr.value[user];
			}
			return true;
		case ShaderScalarChainOp::Load:
		{
			uint32_t pointer[2] = {};
			if (!eval(n.source, pointer))
			{
				return false;
			}
			const uint64_t base = ((static_cast<uint64_t>(pointer[1]) & 0xffffu) << 32u) | pointer[0];
			return ShaderSnapshotGuestDescriptorTable((base + static_cast<int64_t>(n.imm)) & ~uint64_t {3}, n.dwords, words);
		}
		case ShaderScalarChainOp::BufferLoad:
		{
			// Out of range the load writes nothing (the register keeps its value): no value for this draw.
			ShaderBufferResource vsharp {};
			uint32_t             offset = 0;
			if (!eval(n.source, vsharp.fields) || !eval(n.offset, &offset))
			{
				return false;
			}
			const int64_t  start = static_cast<int64_t>(offset) + n.imm;
			const uint64_t bytes = ShaderBufferByteSize(vsharp.Stride(), vsharp.NumRecords());
			if (start < 0 || static_cast<uint64_t>(start) + static_cast<uint64_t>(n.dwords) * sizeof(uint32_t) > bytes)
			{
				return false;
			}
			return ShaderSnapshotGuestDescriptorTable((vsharp.Base48() + static_cast<uint64_t>(start)) & ~uint64_t {3}, n.dwords, words);
		}
	}
	return false;
}

static void ShaderAddHeapDescriptorRead(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t index, bool sampler,
                                        std::vector<ShaderHeapDescriptorRead>* reads)
{
	const auto& inst   = code.GetInstructions().At(index);
	const int   src    = sampler ? 2 : 1;
	const int   dwords = sampler ? 4 : 8;
	if (inst.src_num <= src || inst.src[src].type != ShaderOperandType::Sgpr || inst.src[src].size != dwords)
	{
		return;
	}
	const uint32_t load = ShaderUniqueReachingRangeWrite(code, cfg, index, inst.src[src].register_id, dwords);
	if (load == UINT32_MAX)
	{
		return;
	}
	const auto& sload = code.GetInstructions().At(load);
	const auto  type  = sampler ? ShaderInstructionType::SBufferLoadDwordx4 : ShaderInstructionType::SBufferLoadDwordx8;
	if (sload.type != type || sload.dst.type != ShaderOperandType::Sgpr || sload.dst.register_id != inst.src[src].register_id)
	{
		return;
	}
	ShaderScalarChainBuilder builder(code, cfg);
	if (builder.MemoryLoad(load, 0, dwords, 0) < 0)
	{
		return;
	}
	reads->push_back(ShaderHeapDescriptorRead {index, load, sampler, builder.slot, builder.domain, std::move(builder.nodes)});
}

std::vector<ShaderHeapDescriptorRead> ShaderFindHeapDescriptorReads(const ShaderCode& code)
{
	std::vector<ShaderHeapDescriptorRead> reads;
	const auto&                           instructions = code.GetInstructions();
	if (!ShaderIndexInstructions(code).has_image_reads)
	{
		return reads;
	}
	const auto cfg = ShaderBuildControlFlowGraph(code);
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto type = instructions.At(index).type;
		if (!ShaderInstructionReadsImageResource(type))
		{
			continue;
		}
		ShaderAddHeapDescriptorRead(code, cfg, index, false, &reads);
		if (ShaderInstructionUsesImageSampler(type))
		{
			ShaderAddHeapDescriptorRead(code, cfg, index, true, &reads);
		}
	}
	return reads;
}

const std::vector<ShaderHeapDescriptorRead>& ShaderCodeResourceMemo::HeapDescriptorReads(const ShaderCode& code) const
{
	std::call_once(m_heap_descriptor_reads_once, [&] { m_heap_descriptor_reads = ShaderFindHeapDescriptorReads(code); });
	return m_heap_descriptor_reads;
}

// Records a heap read walks for this draw: the indexes whose stride multiple
// keeps its domain load inside the V#; 1 without a slot, 0 when unknown.
static uint32_t ShaderHeapRecordCount(const ShaderHeapDescriptorRead& read, const HW::UserSgprInfo& user_sgpr, uint16_t srt_size_dw,
                                      int user_data_register_base, const std::vector<std::bitset<106>>& entry_values)
{
	if (read.slot == UINT32_MAX)
	{
		return 1;
	}
	const auto&          load = read.nodes[read.domain];
	ShaderBufferResource vsharp {};
	if (!ShaderEvalScalarChain(read.nodes, load.source, 0, user_sgpr, srt_size_dw, user_data_register_base, entry_values, vsharp.fields))
	{
		return 0;
	}
	const uint64_t bytes  = ShaderBufferByteSize(vsharp.Stride(), vsharp.NumRecords());
	const uint64_t stride = read.nodes[load.offset].value;
	const int64_t  first  = static_cast<int64_t>(load.imm) + static_cast<int64_t>(load.dwords) * 4;
	if (load.imm < 0 || bytes < static_cast<uint64_t>(first))
	{
		return 0;
	}
	return static_cast<uint32_t>(std::min<uint64_t>((bytes - static_cast<uint64_t>(first)) / stride + 1u, UINT32_MAX));
}

// The S# of a heap read: the emitter bakes sampler state, so every record the
// read can reach must load the same words; false otherwise.
static bool ShaderHeapSampler(const ShaderHeapDescriptorRead& read, uint32_t count, const HW::UserSgprInfo& user_sgpr,
                              uint16_t srt_size_dw, int user_data_register_base, const std::vector<std::bitset<106>>& entry_values,
                              ShaderSamplerResource* sampler)
{
	const int root = static_cast<int>(read.nodes.size()) - 1;
	for (uint32_t record = 0; record < count; ++record)
	{
		std::array<uint32_t, 8> words {};
		if (!ShaderEvalScalarChain(read.nodes, root, record, user_sgpr, srt_size_dw, user_data_register_base, entry_values, words.data()))
		{
			return false;
		}
		ShaderSamplerResource resource {};
		std::copy_n(words.begin(), 4, resource.fields);
		if (record > 0 && !ShaderSamplerResourcesEqual(resource, *sampler))
		{
			return false;
		}
		*sampler = resource;
	}
	return true;
}

static State::ImageSampleOperation ShaderHeapSampleOperation(const ShaderInstruction& inst)
{
	return ShaderInstructionUsesImageSampler(inst.type) ? ShaderInstructionSamplerOperation(inst.type) : State::ImageSampleOperation::Regular;
}

// The T#s of a heap read, one per record. The image emitter bakes the shape,
// numeric class and sample operation of the one descriptor it resolves, so
// every record must agree with record 0 on them; false otherwise, or when a
// record holds no image.
static bool ShaderHeapTextures(const ShaderInstruction& inst, const ShaderHeapDescriptorRead& read, uint32_t count,
                               const HW::UserSgprInfo& user_sgpr, uint16_t srt_size_dw, int user_data_register_base,
                               const std::vector<std::bitset<106>>& entry_values, std::vector<ShaderTextureResource>* textures)
{
	ShaderGen5SampledTextureShape instruction_shape {};
	const bool shape_known   = ShaderGen5SampledTextureShapeForMimgDimension(inst.mimg_dimension, &instruction_shape);
	const auto texture_class = [&](const ShaderTextureResource& texture)
	{
		return std::tuple(shape_known ? instruction_shape : ShaderGen5SampledTextureShapeForType(texture.Type()),
		                  ShaderTextureSampleOperation(texture, ShaderHeapSampleOperation(inst)),
		                  VulkanGen5ImageNumericType(texture.Format()));
	};
	const int root = static_cast<int>(read.nodes.size()) - 1;
	for (uint32_t record = 0; record < count; ++record)
	{
		ShaderTextureResource texture {};
		if (!ShaderEvalScalarChain(read.nodes, root, record, user_sgpr, srt_size_dw, user_data_register_base, entry_values, texture.fields) ||
		    !ShaderGen5TextureTypeIsImage(texture.Type()) ||
		    (shape_known && !ShaderGen5InstructionShapeAppliesToType(texture.Type(), instruction_shape)) ||
		    (record > 0 && texture_class(texture) != texture_class(textures->front())))
		{
			return false;
		}
		textures->push_back(texture);
	}
	return true;
}

// One texture per record, never shared between records that load the same
// words: the module then depends on the record count, not on the draw's data.
static void ShaderBindHeapTextures(const ShaderCode& code, const ShaderHeapDescriptorRead& read,
                                   const std::vector<ShaderTextureResource>& textures, ShaderBindResources* bind, ShaderParsedUsage* info,
                                   const HW::UserSgprInfo& user_sgpr)
{
	const auto&       instructions = code.GetInstructions();
	const auto&       inst         = instructions.At(read.instruction);
	ShaderHeapTexture heap {.consumer_pc = inst.pc, .load_pc = instructions.At(read.load).pc};
	if (read.slot != UINT32_MAX)
	{
		// The record index is the S_MUL_I32 source that is not the stride.
		const auto& mul = instructions.At(read.slot);
		heap.slot_pc    = mul.pc;
		heap.slot_index = ShaderOperandIsPlainConstant(mul.src[1]) ? mul.src[0] : mul.src[1];
	}
	ShaderGen5SampledTextureShape shape {};
	const bool                    shape_known = ShaderGen5SampledTextureShapeForMimgDimension(inst.mimg_dimension, &shape);
	for (uint32_t record = 0; record < textures.size(); ++record)
	{
		const int index = bind->textures2D.textures_num;
		ShaderGetTextureBuffer(&bind->textures2D, nullptr, 16, 0, ShaderTextureUsage::ReadOnly, user_sgpr, textures[record].fields);
		bind->textures2D.desc[index].sample_operation = ShaderTextureSampleOperation(textures[record], ShaderHeapSampleOperation(inst));
		bind->textures2D.desc[index].dynamic_sload    = true;
		if (shape_known)
		{
			ShaderSetSampledTextureInstructionShape(&bind->textures2D, index, shape);
		}
		info->textures2D_readonly++;
		heap.record        = record;
		heap.texture_index = index;
		bind->heap_textures.Add(heap);
	}
}

void ShaderCollectHeapDescriptors(const ShaderCode& code, ShaderBindResources* bind, ShaderParsedUsage* info,
                                  const HW::UserSgprInfo& user_sgpr, uint16_t srt_size_dw, int user_data_register_base,
                                  const ShaderScalarFlow& scalar_flow, const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(bind == nullptr || info == nullptr);
	bind->heap_textures.Clear();
	const auto  direct       = memo == nullptr ? ShaderFindHeapDescriptorReads(code) : std::vector<ShaderHeapDescriptorRead> {};
	const auto& reads        = memo != nullptr ? memo->HeapDescriptorReads(code) : direct;
	const auto& instructions = code.GetInstructions();
	const auto& entry_values = scalar_flow.holding_entry_value;
	for (const auto& read: reads)
	{
		const auto& inst = instructions.At(read.instruction);
		if ((read.sampler ? ShaderFindImageSamplerDescriptor(inst, *bind, user_data_register_base)
		                  : ShaderFindImageSampledTextureDescriptor(inst, *bind, user_data_register_base)) >= 0)
		{
			continue;
		}
		const uint32_t                     count = ShaderHeapRecordCount(read, user_sgpr, srt_size_dw, user_data_register_base, entry_values);
		ShaderSamplerResource              sampler {};
		std::vector<ShaderTextureResource> textures;
		const bool                         bound =
		    count > 0 && count <= kShaderHeapRecordsMax &&
		    (read.sampler ? ShaderHeapSampler(read, count, user_sgpr, srt_size_dw, user_data_register_base, entry_values, &sampler)
		                  : ShaderHeapTextures(inst, read, count, user_sgpr, srt_size_dw, user_data_register_base, entry_values, &textures) &&
		                        bind->textures2D.textures_num + static_cast<int>(count) <= ShaderTextureResources::RES_MAX);
		if (!bound)
		{
			// The image emitter then stops on the unresolved descriptor.
			KYTY_LOG_LIMIT(Log::Level::Warn, 8,
			               "WARNING: heap descriptor read not bound: crc=%08" PRIx32 " pc=0x%08" PRIx32 " sampler=%d records=%u\n",
			               code.GetCrc32(), inst.pc, read.sampler ? 1 : 0, count);
			continue;
		}
		if (read.sampler)
		{
			ShaderBindFixedSampler(bind, info, inst, sampler, user_data_register_base);
		} else
		{
			ShaderBindHeapTextures(code, read, textures, bind, info, user_sgpr);
		}
	}
}

// Follows each V# dword of a buffer consumer back, through the only write
// reaching it on every path, across scalar moves to the load that writes it.
// The dwords must be four consecutive dwords of one table read through one
// pointer load; a V# one load writes in place is left to the load partition.
static bool ShaderFindCopiedNestedBuffer(const ShaderCode& code, const ShaderControlFlowGraph& cfg, uint32_t consumer_index,
                                         ShaderNestedTableLoad* found)
{
	constexpr int kMaxMoves    = 8;
	const auto&   instructions = code.GetInstructions();
	const auto*   descriptor   = ShaderVectorBufferDescriptor(instructions.At(consumer_index));
	if (descriptor == nullptr)
	{
		return false;
	}
	std::array<uint32_t, 4> load {};
	std::array<int, 4>      table_dw {};
	bool                    copied = false;
	for (int word = 0; word < 4; ++word)
	{
		int      reg   = descriptor->register_id + word;
		uint32_t at    = consumer_index;
		int      moves = 0;
		for (;;)
		{
			const uint32_t write = ShaderUniqueReachingSgprWrite(code, cfg, at, reg);
			if (write == UINT32_MAX || moves > kMaxMoves)
			{
				return false;
			}
			const auto& inst = instructions.At(write);
			if (ShaderTraceScalarMove(inst, &reg))
			{
				copied = true;
				at     = write;
				moves++;
				continue;
			}
			const int dwords    = ShaderScalarLoadDwords(inst.type);
			int       offset_dw = 0;
			if (dwords == 0 || inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != dwords || inst.src_num < 2 ||
			    !ShaderGetSmemConstantDwordOffset(inst, &offset_dw) || offset_dw < 0 ||
			    static_cast<uint32_t>(offset_dw) > kMaxSrtPointerTableDwords - static_cast<uint32_t>(dwords))
			{
				return false;
			}
			load[word]     = write;
			table_dw[word] = offset_dw + reg - inst.dst.register_id;
			break;
		}
	}
	if (!copied)
	{
		return false;
	}
	int      pointer_offset_dw = 0;
	uint32_t pointer_index     = UINT32_MAX;
	for (int word = 0; word < 4; ++word)
	{
		int            piece_pointer_offset_dw = 0;
		const uint32_t piece_pointer           = ShaderFindTablePointerLoad(code, cfg, load[word], &piece_pointer_offset_dw);
		if (table_dw[word] != table_dw[0] + word || piece_pointer == UINT32_MAX ||
		    (word > 0 && (piece_pointer != pointer_index || piece_pointer_offset_dw != pointer_offset_dw)))
		{
			return false;
		}
		pointer_index     = piece_pointer;
		pointer_offset_dw = piece_pointer_offset_dw;
	}
	found->pointer_index     = pointer_index;
	found->pointer_offset_dw = pointer_offset_dw;
	found->copied_buffers    = {{consumer_index, table_dw[0]}};
	return true;
}

std::vector<ShaderNestedTableLoad> ShaderFindNestedTableLoads(const ShaderCode& code)
{
	const auto&           instructions = code.GetInstructions();
	struct Candidate
	{
		uint32_t index;
		int      dwords;
		int      offset_dw;
	};
	std::vector<Candidate> candidates;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& sload = instructions.At(index);
		auto        shape = sload;
		if (shape.src_num >= 1 && shape.src[0].type == ShaderOperandType::VccLo)
		{
			shape.src[0].type = ShaderOperandType::Sgpr;
		}
		const int dword_count = ShaderTableSLoadDwords(shape);
		int       offset_dw   = 0;
		if (dword_count != 0 && ShaderGetSmemConstantDwordOffset(sload, &offset_dw) && offset_dw >= 0 &&
		    static_cast<uint32_t>(offset_dw) <= kMaxSrtPointerTableDwords - static_cast<uint32_t>(dword_count))
		{
			candidates.push_back({index, dword_count, offset_dw});
		}
	}
	std::vector<ShaderNestedTableLoad> nested;
	if (candidates.empty())
	{
		return nested;
	}
	const auto cfg = ShaderBuildControlFlowGraph(code);
	if (!cfg.Structurable())
	{
		return nested;
	}
	for (const auto& candidate: candidates)
	{
		ShaderNestedTableLoad load;
		load.pointer_index = ShaderFindTablePointerLoad(code, cfg, candidate.index, &load.pointer_offset_dw);
		if (load.pointer_index == UINT32_MAX)
		{
			continue;
		}
		ShaderPartitionGsUserDataLoad(code, cfg, candidate.index, instructions.At(candidate.index), candidate.offset_dw, 0,
		                              candidate.dwords, ShaderGsUserDataBufferPolicy::All, &load.loads);
		if (!load.loads.empty())
		{
			nested.push_back(std::move(load));
		}
	}
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto&           inst = instructions.At(index);
		ShaderNestedTableLoad copied;
		if ((ShaderInstructionReadsImageResource(inst.type) || ShaderInstructionWritesImageResource(inst.type)) &&
		    ShaderFindCopiedNestedTexture(code, cfg, index, &copied))
		{
			nested.push_back(std::move(copied));
		}
		if (ShaderInstructionIsVectorBufferAccess(inst.type) && ShaderFindCopiedNestedBuffer(code, cfg, index, &copied))
		{
			nested.push_back(std::move(copied));
		}
	}
	return nested;
}

void ShaderBindGsUserDataDescriptors(const ShaderCode& code, const std::vector<ShaderGsUserDataDescriptorLoad>& loads,
                                     uint64_t table_address, const HW::UserSgprInfo& user_sgpr, ShaderBindResources* bind,
                                     ShaderParsedUsage* info, const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(bind == nullptr || info == nullptr);
	if (loads.empty())
	{
		return;
	}
	uint32_t table_dwords = 0;
	for (const auto& load: loads)
	{
		table_dwords = std::max(table_dwords, static_cast<uint32_t>(load.offset_dw + load.block.dst.size));
	}
	std::array<uint32_t, 64> stack_table {};
	std::vector<uint32_t>    heap_table;
	uint32_t*                table_words = stack_table.data();
	if (table_dwords > stack_table.size())
	{
		heap_table.resize(table_dwords);
		table_words = heap_table.data();
	}
	if (table_address == 0 || !ShaderSnapshotGuestDescriptorTable(table_address, table_dwords, table_words))
	{
		EXIT("unreadable GS user-data table: address=0x%016" PRIx64 " dwords=%u\n", table_address, table_dwords);
	}
	const uint32_t first_record = bind->dynamic_sloads.records.Size();
	for (const auto pass: {ShaderDescriptorLoadPass::NonSampler, ShaderDescriptorLoadPass::Sampler})
	{
		for (const auto& load: loads)
		{
			if (!ShaderDescriptorLoadInPass(load.use, pass))
			{
				continue;
			}
			const bool is_storage       = load.use.kind == ShaderDynamicSLoadResourceKind::StorageBuffer;
			const int  prev_buffers_num = bind->storage_buffers.buffers_num;
			if (!ShaderAddDynamicScalarResource(code, bind, info, load.block, load.offset_dw, load.use, user_sgpr, table_words,
			                                    code.GetInstructions().Size(), memo))
			{
				if (is_storage)
				{
					// Leave overflow storage descriptors on the runtime guest-address path.
					continue;
				}
				EXIT("unable to materialize GS user-data descriptor: pc=0x%08" PRIx32 " offset_dw=%d dwords=%d kind=%u textures=%d "
				     "samplers=%d\n",
				     load.block.pc, load.offset_dw, load.block.dst.size, static_cast<unsigned>(load.use.kind),
				     bind->textures2D.textures_num, bind->samplers.samplers_num);
			}
			if (!is_storage)
			{
				continue;
			}
			int storage_index = -1;
			for (uint32_t mapping = bind->dynamic_sloads.records.Size(); mapping-- > first_record;)
			{
				const auto& rec = bind->dynamic_sloads.records.At(mapping);
				if (rec.kind == ShaderDynamicSLoadResourceKind::StorageBuffer && rec.instruction_pc == load.block.pc &&
				    rec.offset_dw == load.offset_dw)
				{
					storage_index = rec.resource_index;
					break;
				}
			}
			if (storage_index < 0 || storage_index >= bind->storage_buffers.buffers_num)
			{
				continue;
			}
			auto& s = bind->storage_buffers;
			if (bind->storage_buffers.buffers_num != prev_buffers_num && storage_index == prev_buffers_num)
			{
				s.accesses[storage_index]                = ShaderStorageAccess::Raw;
				s.unknown_reasons[storage_index]         = ShaderStorageUnknownReason::None;
				s.code_available[storage_index]          = true;
				s.exact_matches[storage_index]           = true;
				s.decoded_unknown[storage_index]         = false;
				s.indirect_descriptor_use[storage_index] = false;
				s.raw_vmem_oob_guarded[storage_index]    = load.use.raw_vmem_oob_guarded;
				s.raw_smem_use[storage_index]            = load.use.raw_smem_use;
				s.raw_smem_dynamic_offset[storage_index] = load.use.raw_smem_use;
				s.raw_tbuffer_use[storage_index]         = false;
				s.dword_loads_only[storage_index]        = load.use.dword_loads_only;
				s.format_loads_only[storage_index]       = false;
			} else
			{
				if (s.accesses[storage_index] == ShaderStorageAccess::Unknown)
				{
					s.accesses[storage_index] = ShaderStorageAccess::Raw;
				} else if (s.accesses[storage_index] == ShaderStorageAccess::Typed)
				{
					s.accesses[storage_index] = ShaderStorageAccess::Mixed;
				}
				s.raw_vmem_oob_guarded[storage_index]    = s.raw_vmem_oob_guarded[storage_index] || load.use.raw_vmem_oob_guarded;
				s.raw_smem_use[storage_index]            = s.raw_smem_use[storage_index] || load.use.raw_smem_use;
				s.raw_smem_dynamic_offset[storage_index] = s.raw_smem_dynamic_offset[storage_index] || load.use.raw_smem_use;
				s.dword_loads_only[storage_index]        = s.dword_loads_only[storage_index] && load.use.dword_loads_only;
				s.format_loads_only[storage_index]       = false;
			}
		}
	}
	for (uint32_t mapping = first_record; mapping < bind->dynamic_sloads.records.Size(); ++mapping)
	{
		bind->dynamic_sloads.records[mapping].guest_load = true;
	}
}

// A descriptor bound by metadata or as a direct resource at its own user-data
// position (dynamic bindings are placed per consumer and do not count).
static bool ShaderStorageBufferStaticallyBound(const ShaderStorageResources& resources, int api_register)
{
	for (int i = 0; i < resources.buffers_num; ++i)
	{
		if (!resources.dynamic_sload[i] && resources.start_register[i] == api_register)
		{
			return true;
		}
	}
	return false;
}

// The four words of a V# whose SGPRs each hold a user-data word on every path
// to the consumer, in an order other than the user data itself.
static bool ShaderAssembledDescriptorWords(const ShaderOperand& descriptor, const ShaderSgprEntrySources& sources,
                                           const HW::UserSgprInfo& user_sgpr, int user_sgpr_num, int user_data_register_base,
                                           std::array<uint32_t, 4>* words)
{
	bool identity = true;
	for (int word = 0; word < 4; ++word)
	{
		const int reg = descriptor.register_id + word;
		if (reg < 0 || reg >= kShaderScalarLivenessSgprs)
		{
			return false;
		}
		const int source    = sources[static_cast<size_t>(reg)];
		const int user_word = source - user_data_register_base;
		if (source == kShaderSgprNoEntrySource || user_word < 0 || user_word >= user_sgpr_num ||
		    user_word >= HW::UserSgprInfo::SGPRS_MAX)
		{
			return false;
		}
		identity          = identity && source == reg;
		(*words)[static_cast<size_t>(word)] = user_sgpr.value[user_word];
	}
	return !identity;
}

// One storage resource per distinct descriptor and V# register; a store or
// atomic through any consumer makes it writable.
static int ShaderAddAssembledStorageResource(ShaderStorageResources* resources, const std::array<uint32_t, 4>& words, int api_register,
                                             bool writes, bool typed)
{
	ShaderBufferResource resource {};
	for (int field = 0; field < 4; ++field)
	{
		resource.fields[field] = words[static_cast<size_t>(field)];
	}
	const auto access = typed ? ShaderStorageAccess::Typed : ShaderStorageAccess::Raw;
	for (int i = 0; i < resources->buffers_num; ++i)
	{
		if (resources->dynamic_sload[i] && resources->start_register[i] == api_register &&
		    ShaderStorageResourcesEqual(resources->buffers[i], resource))
		{
			if (writes)
			{
				resources->usages[i] = ShaderStorageUsage::ReadWrite;
			}
			if (resources->accesses[i] != access)
			{
				resources->accesses[i] = ShaderStorageAccess::Mixed;
			}
			return i;
		}
	}
	if (resources->buffers_num >= ShaderStorageResources::BUFFERS_MAX)
	{
		return -1;
	}
	const int index                   = resources->buffers_num++;
	resources->buffers[index]         = resource;
	resources->usages[index]          = writes ? ShaderStorageUsage::ReadWrite : ShaderStorageUsage::ReadOnly;
	resources->accesses[index]        = access;
	resources->sources[index]         = ShaderStorageBindingSource::DirectResource;
	resources->code_available[index]  = true;
	resources->exact_matches[index]   = true;
	resources->slots[index]           = api_register;
	resources->start_register[index]  = api_register;
	resources->extended[index]        = false;
	resources->dynamic_sload[index]   = true;
	return index;
}

// Binds the storage resource a consumer's V# words select and records that
// its V# registers take that resource's metadata right before it.
static void ShaderAddAssembledDescriptor(ShaderBindResources* bind, const ShaderInstruction& inst, const ShaderOperand& descriptor,
                                         const std::array<uint32_t, 4>& words, int user_data_register_base)
{
	const auto type   = inst.type;
	const bool writes = ShaderInstructionTypeStartsWith(type, "BufferStore") || ShaderInstructionTypeStartsWith(type, "TBufferStore") ||
	                    ShaderInstructionTypeStartsWith(type, "BufferAtomic");
	const bool typed  = ShaderInstructionTypeStartsWith(type, "TBuffer") ||
	                    ShaderInstructionTypeName(type).find("Format") != std::string_view::npos;
	const int  resource = ShaderAddAssembledStorageResource(&bind->storage_buffers, words,
	                                                        descriptor.register_id - user_data_register_base, writes, typed);
	if (resource < 0)
	{
		EXIT("assembled buffer descriptors exceed the storage binding capacity: pc=0x%08" PRIx32 " register=%d\n", inst.pc,
		     descriptor.register_id);
	}
	bind->assembled_descriptors.Add({inst.pc, descriptor.register_id, resource});
}

// A compiler can move a V# out of its user-data position or build several
// descriptors that share words (one at s1..s4 and one from s0 plus s2..s4).
// Such a descriptor is still known at draw time from the user data alone:
// each consumer gets the storage resource its words select, and the V#
// registers are written with that resource's metadata right before it.
void ShaderCollectAssembledBufferDescriptors(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                             int user_sgpr_num, int user_data_register_base, const ShaderScalarFlow& scalar_flow,
                                             const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(bind == nullptr);
	const auto& sources      = scalar_flow.entry_sources;
	const auto& instructions = code.GetInstructions();
	const ShaderInstructionIndices direct       = memo == nullptr ? ShaderIndexInstructions(code) : ShaderInstructionIndices {};
	const auto&                    indices      = memo != nullptr ? memo->InstructionIndices(code) : direct;
	for (const uint32_t index: indices.buffer_accesses)
	{
		if (index >= sources.size())
		{
			break;
		}
		const auto& inst       = instructions.At(index);
		const auto* descriptor = ShaderVectorBufferDescriptor(inst);
		std::array<uint32_t, 4> words {};
		if (descriptor == nullptr ||
		    ShaderStorageBufferStaticallyBound(bind->storage_buffers, descriptor->register_id - user_data_register_base) ||
		    !ShaderAssembledDescriptorWords(*descriptor, sources[index], user_sgpr, user_sgpr_num, user_data_register_base, &words))
		{
			continue;
		}
		ShaderAddAssembledDescriptor(bind, inst, *descriptor, words, user_data_register_base);
	}
}

void ShaderCollectNestedCopiedBufferDescriptors(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                                uint16_t srt_size_dw, int user_data_register_base, const ShaderScalarFlow& scalar_flow,
                                                const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(bind == nullptr);
	if (srt_size_dw < 2u)
	{
		return;
	}
	const auto  direct_nested = memo == nullptr ? ShaderFindNestedTableLoads(code) : std::vector<ShaderNestedTableLoad> {};
	const auto& nested        = memo != nullptr ? memo->NestedTableLoads(code) : direct_nested;
	const auto  tables = ShaderSnapshotNestedTables(code, nested, user_sgpr, srt_size_dw, user_data_register_base, scalar_flow, *bind, true);
	for (size_t n = 0; n < nested.size(); ++n)
	{
		for (const auto& buffer: nested[n].copied_buffers)
		{
			const auto& inst       = code.GetInstructions().At(buffer.consumer);
			const auto* descriptor = ShaderVectorBufferDescriptor(inst);
			if (tables[n].empty() || descriptor == nullptr ||
			    ShaderStorageBufferStaticallyBound(bind->storage_buffers, descriptor->register_id - user_data_register_base))
			{
				continue;
			}
			std::array<uint32_t, 4> words {};
			std::copy_n(tables[n].begin() + buffer.offset_dw, 4, words.begin());
			ShaderAddAssembledDescriptor(bind, inst, *descriptor, words, user_data_register_base);
		}
	}
}

// A V# register a bound descriptor names: the start register of a storage
// buffer (or, for scalar loads, of a proven-empty buffer).
static bool ShaderBufferRegisterNamesBinding(const ShaderBindResources& bind, const ShaderInstruction& inst,
                                             int user_data_register_base)
{
	if (ShaderInstructionIsScalarBufferLoad(inst))
	{
		const int reg = inst.src[0].register_id;
		for (int i = 0; i < bind.zero_sbuffer_resources.buffers_num; ++i)
		{
			if (bind.zero_sbuffer_resources.start_register[i] == reg) { return true; }
		}
		return ShaderStorageBufferResourceIsBound(bind, inst.src[0], user_data_register_base);
	}
	return ShaderStorageBufferResourceIsBound(bind, inst.src[1], user_data_register_base);
}

void ShaderCollectRedefinedBufferDescriptors(const ShaderCode& code, ShaderBindResources* bind, int user_data_register_base,
                                             const ShaderScalarFlow& scalar_flow, const ShaderCodeResourceMemo* memo,
                                             bool record_consumers)
{
	EXIT_IF(bind == nullptr);
	const auto& holding      = scalar_flow.holding_entry_value;
	const auto& instructions = code.GetInstructions();
	// An unresolved CFG reports no entry value anywhere and proves nothing.
	if (holding.empty() || holding.front().none())
	{
		return;
	}
	bind->descriptor_flow_proven = true;
	const ShaderInstructionIndices direct  = memo == nullptr ? ShaderIndexInstructions(code) : ShaderInstructionIndices {};
	const auto&                    indices = memo != nullptr ? memo->InstructionIndices(code) : direct;
	for (const uint32_t index: indices.buffer_accesses)
	{
		if (index >= holding.size())
		{
			break;
		}
		const auto& inst   = instructions.At(index);
		const bool  scalar = ShaderInstructionIsScalarBufferLoad(inst) && inst.src_num >= 1 &&
		                    inst.src[0].type == ShaderOperandType::Sgpr && inst.src[0].size == 4;
		const auto* vector = ShaderVectorBufferDescriptor(inst);
		if ((!scalar && vector == nullptr) || ShaderIsDynamicScalarStorageConsumer(*bind, inst) ||
		    ShaderIsDynamicVectorStorageConsumer(*bind, inst))
		{
			continue;
		}
		// The binding replaces the base dword with the descriptor slot; only a
		// base still holding it addresses the bound buffer.
		const int base = scalar ? inst.src[0].register_id : vector->register_id;
		if (base < 0 || base >= kShaderScalarLivenessSgprs)
		{
			continue;
		}
		if (!ShaderBufferRegisterNamesBinding(*bind, inst, user_data_register_base))
		{
			// A V# the program wrote on every path, over registers that name no binding, is a descriptor it built.
			if (record_consumers && !holding[index].test(static_cast<size_t>(base)) &&
			    (memo != nullptr ? memo->SgprWrittenOnEveryPath(code, index, base)
			                     : ShaderUniqueReachingSgprWrite(code, ShaderBuildControlFlowGraph(code), index, base) != UINT32_MAX))
			{
				bind->redefined_descriptor_consumers.Add(inst.pc);
			}
			continue;
		}
		if (!holding[index].test(static_cast<size_t>(base)) && record_consumers)
		{
			bind->redefined_descriptor_consumers.Add(inst.pc);
			continue;
		}
		if (!holding[index].test(static_cast<size_t>(base)))
		{
			// A slot-only consumer must never see the bound row, which may hold a
			// device address: a path that keeps it refuses the buffer instead.
			const bool written = memo != nullptr
			                         ? memo->SgprWrittenOnEveryPath(code, index, base)
			                         : ShaderUniqueReachingSgprWrite(code, ShaderBuildControlFlowGraph(code), index, base) != UINT32_MAX;
			const int  bound   = ShaderStorageBufferBoundIndex(*bind, scalar ? inst.src[0] : *vector, user_data_register_base);
			if (written)
			{
				bind->slot_only_consumers.Add(inst.pc);
			} else if (bound >= 0)
			{
				bind->storage_buffers.row_redefined[bound] = true;
			}
			continue;
		}
		// A read by address also takes words 1 (high address) and 2 (size) from the row.
		const int bound = ShaderStorageBufferBoundIndex(*bind, scalar ? inst.src[0] : *vector, user_data_register_base);
		if (bound >= 0 && (base + 2 >= kShaderScalarLivenessSgprs || !holding[index].test(static_cast<size_t>(base + 1)) ||
		                   !holding[index].test(static_cast<size_t>(base + 2))))
		{
			bind->storage_buffers.row_redefined[bound] = true;
		}
	}
}

bool ShaderIsDynamicScalarStorageConsumer(const ShaderBindResources& bind, const ShaderInstruction& inst)
{
	if (!ShaderInstructionIsScalarBufferLoad(inst) || inst.src_num == 0 || inst.src[0].type != ShaderOperandType::Sgpr ||
	    inst.src[0].size != 4)
	{
		return false;
	}
	for (uint32_t mapping = 0; mapping < bind.dynamic_sloads.records.Size(); ++mapping)
	{
		const auto& record = bind.dynamic_sloads.records.At(mapping);
		if (record.kind == ShaderDynamicSLoadResourceKind::StorageBuffer &&
		    record.destination_register == inst.src[0].register_id && inst.pc > record.instruction_pc &&
		    inst.pc <= record.last_consumer_pc)
		{
			return true;
		}
	}
	return false;
}

bool ShaderScalarBufferUsesRuntimeDescriptor(const ShaderBindResources& bind, const ShaderInstruction& inst, int user_data_register_base)
{
	if (!ShaderInstructionIsScalarBufferLoad(inst) || inst.src_num < 1 || inst.src[0].type != ShaderOperandType::Sgpr ||
	    inst.src[0].size != 4 || ShaderIsDynamicScalarStorageConsumer(bind, inst))
	{
		return false;
	}
	if (ShaderBufferConsumerRedefinesDescriptor(bind, inst))
	{
		return true;
	}
	if (ShaderStorageBufferResourceIsBound(bind, inst.src[0], user_data_register_base))
	{
		return false;
	}
	const int reg = inst.src[0].register_id;
	for (int i = 0; i < bind.zero_sbuffer_resources.buffers_num; ++i)
	{
		if (bind.zero_sbuffer_resources.start_register[i] == reg) { return false; }
	}
	return true;
}

bool ShaderStorageResourceHasDynamicSLoad(const ShaderBindResources& bind, int storage_index)
{
	for (uint32_t mapping = 0; mapping < bind.dynamic_sloads.records.Size(); ++mapping)
	{
		if (const auto& record = bind.dynamic_sloads.records.At(mapping);
		    record.kind == ShaderDynamicSLoadResourceKind::StorageBuffer && record.resource_index == storage_index)
		{
			return true;
		}
	}
	return false;
}

// Metadata-only V# entries are not physical resources. Pruning those before
// dynamic discovery preserves the fixed descriptor-table budget for proven
// S_LOAD consumers without relaxing any unknown or indirect descriptor use.
void ShaderPruneUnusedMetadataStorage(const ShaderCode& code, ShaderStorageResources* resources, int user_sgpr_num,
                                              int user_data_register_base, const ShaderCodeResourceMemo* memo)
{
	EXIT_IF(resources == nullptr);
	for (int index = 0; index < resources->buffers_num; ++index)
	{
		if (resources->sources[index] != ShaderStorageBindingSource::MetadataSharp)
		{
			continue;
		}
		const int binding_register = resources->extended[index]
		                                 ? ShaderGen5EudOffsetBase(user_sgpr_num) + (resources->start_register[index] - 16)
		                                 : resources->start_register[index];
		const auto exact = AnalyzeShaderStorageUse(code, binding_register + user_data_register_base, memo);
		ShaderStorageUseEvidence unbased {};
		if (user_data_register_base != 0)
		{
			unbased = AnalyzeShaderStorageUse(code, binding_register, memo);
		}
		resources->accesses[index] = ResolveShaderStorageAccessEvidence(true, resources->sources[index], exact.access, unbased.access,
		                                                                 exact.decoded_unknown, exact.indirect_descriptor_use)
		                                 .access;
	}
	ExcludeUnusedMetadataStorage(resources);
}

void ShaderAppendVertexStreamStorage(ShaderVertexInputInfo* info, bool streams_read)
{
	if (info == nullptr || !info->fetch_embedded || info->buffers_num <= 0)
	{
		return;
	}

	auto& storage = info->bind.storage_buffers;
	for (int i = 0; i < info->buffers_num; i++)
	{
		auto& stream = info->buffers[i];
		stream.storage_slot = -1;
		if (stream.addr == 0 || stream.stride == 0 || stream.num_records == 0)
		{
			continue;
		}
		bool already = false;
		for (int existing = 0; existing < storage.buffers_num; existing++)
		{
			// Reuse only a stream-style SSBO. Regular storage rewrites word0
			// to the slot index, so its vsharp base is no longer the guest V#.
			if (storage.buffers[existing].Base48() == stream.addr && storage.start_register[existing] < 0)
			{
				stream.storage_slot = existing;
				already             = true;
				break;
			}
		}
		if (already || storage.buffers_num >= ShaderStorageResources::BUFFERS_MAX)
		{
			continue;
		}

		const int index = storage.buffers_num;
		ShaderBufferResource resource {};
		resource.UpdateAddress48(stream.addr);
		resource.fields[1] = (resource.fields[1] & 0x0000ffffu) | (static_cast<uint32_t>(stream.stride & 0x3fffu) << 16u);
		resource.fields[2] = stream.num_records;

		storage.buffers[index]          = resource;
		storage.usages[index]           = ShaderStorageUsage::ReadOnly;
		storage.accesses[index]         = ShaderStorageAccess::Raw;
		storage.sources[index]          = ShaderStorageBindingSource::DirectResource;
		storage.raw_tbuffer_use[index]  = true;
		storage.slots[index]            = index;
		storage.start_register[index]   = -1;
		storage.unread_stream[index]    = !streams_read;
		storage.buffers_num++;
		stream.storage_slot = index;
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
