#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Log.h"

#include "Emulator/Graphics/ShaderScalarLiveness.h"
#include "ShaderStorageAnalysis.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <climits>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

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
	return -1;
}

void ShaderAssociateSampledTextureSamplers(const ShaderCode& code, ShaderBindResources* bind, int user_data_register_base)
{
	EXIT_IF(bind == nullptr);
	for (int index = 0; index < bind->textures2D.textures_num; ++index)
	{
		bind->textures2D.desc[index].sampler_indices_mask = 0u;
	}
	for (const auto& inst: code.GetInstructions())
	{
		if (!ShaderInstructionUsesImageSampler(inst.type))
		{
			continue;
		}
		const int texture_index = ShaderFindImageSampledTextureDescriptor(inst, *bind, user_data_register_base);
		const int sampler_index = ShaderFindImageSamplerDescriptor(inst, *bind, user_data_register_base);
		if (texture_index >= 0 && sampler_index >= 0 && sampler_index < 16)
		{
			bind->textures2D.desc[texture_index].sampler_indices_mask |= static_cast<uint16_t>(1u << sampler_index);
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

// Gen5 texture type nibble: 8 = 1D, 9 = 2D, 10 = 3D, 13 = 2D array. SizeFlag clear
// selects an 8-dword T#; other type values in this path are 4-dword V#.
static bool Gen5SharpIsImageDescriptor(int offset_dw, int user_sgpr_num, const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer)
{
	const uint32_t word3 = Gen5SharpUserSgprDword(offset_dw + 3, user_sgpr_num, user_sgpr, extended_buffer);
	const uint8_t  type  = static_cast<uint8_t>((word3 >> 28u) & 0xFu);
	// 8/9/10 = 1D/2D/3D; 11 = cube (faces as layers); 13 = 2D array.
	return type == 8u || type == 9u || type == 10u || type == 11u || type == 13u;
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

static bool ShaderGen5EudAddRequiredSpan(uint32_t offset_dw, uint32_t dwords, uint32_t* required_end_dw)
{
	if (required_end_dw == nullptr || dwords == 0u || offset_dw > UINT32_MAX - dwords)
	{
		return false;
	}
	const uint32_t end_dw = offset_dw + dwords;
	if (end_dw > SHADER_GEN5_EUD_MAX_DWORDS)
	{
		return false;
	}
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

bool ShaderGen5EudRequiredEndDwords(const ShaderUserData* user_data, int user_sgpr_num, int eud_pointer_register,
                                    const ShaderCode* code, int user_data_register_base, uint32_t* required_end_dw)
{
	(void)user_data_register_base;
	if (user_data == nullptr || required_end_dw == nullptr || user_sgpr_num < 0 ||
	    user_sgpr_num > HW::UserSgprInfo::SGPRS_MAX || eud_pointer_register < 0 ||
	    eud_pointer_register > user_sgpr_num - 2 || user_data->eud_size_dw == 0u ||
	    user_data->eud_size_dw > SHADER_GEN5_EUD_MAX_DWORDS)
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
		for (const auto& inst: code->GetInstructions())
		{
			const int dwords = inst.type == ShaderInstructionType::SLoadDwordx4 ? 4 :
			                   (inst.type == ShaderInstructionType::SLoadDwordx8 ? 8 : 0);
			if (dwords == 0 || inst.dst.type != ShaderOperandType::Sgpr || inst.dst.size != dwords || inst.src_num < 2 ||
			    inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].register_id != eud_pointer_register ||
			    inst.src[0].size != 2)
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
			const bool is_image = type == 8u || type == 9u || type == 10u || type == 11u || type == 13u;
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

	sampled_shape_known = sampled_shape_known && ShaderGen5InstructionShapeAppliesToType(resource.Type(), sampled_shape);
	int texture_index = -1;
	for (int index = 0; index < bind->textures2D.textures_num; ++index)
	{
		const auto& descriptor = bind->textures2D.desc[index];
		const bool  shape_matches = usage != ShaderTextureUsage::ReadOnly || !sampled_shape_known ||
		                            ShaderResolvedSampledTextureShape(descriptor) == sampled_shape;
		if (shape_matches && descriptor.usage == usage && descriptor.sample_operation == operation &&
		    ShaderTextureResourcesEqual(descriptor.texture, resource))
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
			bind->textures2D.desc[texture_index].sampled_shape                  = sampled_shape;
			bind->textures2D.desc[texture_index].sampled_shape_from_instruction = true;
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
                                           uint16_t eud_size_dw, int* offset_dw)
{
	if (offset_dw == nullptr || load.dst.type != ShaderOperandType::Sgpr || load.dst.size != dword_count || load.src_num < 2)
	{
		return false;
	}
	if (load.src[0].type != ShaderOperandType::Sgpr || load.src[0].register_id != bind.extended.start_register || load.src[0].size != 2)
	{
		return false;
	}
	if (!ShaderGetSmemConstantDwordOffset(load, offset_dw) || *offset_dw < 0)
	{
		return false;
	}
	return ShaderGen5EudSpanAllowed(16 + *offset_dw, dword_count, eud_size_dw);
}

static bool ShaderFindSplitTextureLoads(const ShaderCode& code, uint32_t consumer_index, const ShaderBindResources& bind,
                                        uint16_t eud_size_dw, ShaderSplitTextureLoad* low, ShaderSplitTextureLoad* high)
{
	if (low == nullptr || high == nullptr || consumer_index >= code.GetInstructions().Size())
	{
		return false;
	}
	const auto& consumer = code.GetInstructions().At(consumer_index);
	if (consumer.src_num < 2 || consumer.src[1].type != ShaderOperandType::Sgpr || consumer.src[1].size != 8)
	{
		return false;
	}
	const int descriptor_register = consumer.src[1].register_id;
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
		if (candidate.type != ShaderInstructionType::SLoadDwordx4)
		{
			return false;
		}
		ShaderSplitTextureLoad* half = nullptr;
		if (candidate.dst.register_id == descriptor_register)
		{
			half = low;
		} else if (candidate.dst.register_id == descriptor_register + 4)
		{
			half = high;
		}
		if (half == nullptr || half->instruction != nullptr ||
		    !ShaderTryGetExtendedLoadOffset(candidate, bind, 4, eud_size_dw, &half->offset_dw))
		{
			return false;
		}
		half->instruction = &candidate;
		if (low->instruction != nullptr && high->instruction != nullptr)
		{
			return true;
		}
	}
	return false;
}

static int ShaderFindTextureResource(const ShaderBindResources& bind, const ShaderTextureResource& resource, ShaderTextureUsage usage,
                                     State::ImageSampleOperation operation, ShaderGen5SampledTextureShape shape, bool shape_known)
{
	for (int index = 0; index < bind.textures2D.textures_num; ++index)
	{
		const auto& descriptor = bind.textures2D.desc[index];
		const bool  shape_matches =
		    usage != ShaderTextureUsage::ReadOnly || !shape_known || ShaderResolvedSampledTextureShape(descriptor) == shape;
		if (shape_matches && descriptor.usage == usage && descriptor.sample_operation == operation &&
		    ShaderTextureResourcesEqual(descriptor.texture, resource))
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
	const int existing = ShaderFindTextureResource(*bind, resource, usage, operation, shape, shape_known);
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
                                               const uint32_t* extended_buffer, uint16_t eud_size_dw)
{
	EXIT_IF(bind == nullptr || info == nullptr || extended_buffer == nullptr);
	const uint32_t instruction_count = code.GetInstructions().Size();
	for (uint32_t index = 0; index < instruction_count; ++index)
	{
		const auto& consumer = code.GetInstructions().At(index);
		const bool  reads    = ShaderInstructionReadsImageResource(consumer.type);
		const bool  writes   = ShaderInstructionWritesImageResource(consumer.type);
		if (!reads && !writes)
		{
			continue;
		}
		ShaderSplitTextureLoad low {};
		ShaderSplitTextureLoad high {};
		if (!ShaderFindSplitTextureLoads(code, index, *bind, eud_size_dw, &low, &high))
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

struct ShaderDynamicSLoadUse
{
	ShaderDynamicSLoadResourceKind kind              = ShaderDynamicSLoadResourceKind::StorageBuffer;
	ShaderStorageUsage             storage_usage     = ShaderStorageUsage::ReadOnly;
	ShaderTextureUsage             texture_usage     = ShaderTextureUsage::Unknown;
	State::ImageSampleOperation sampler_operation = State::ImageSampleOperation::Regular;
	ShaderGen5SampledTextureShape sampled_shape = ShaderGen5SampledTextureShape::TwoDimensional;
	uint32_t                    last_consumer_pc   = 0;
	bool                        raw_vmem_oob_guarded = false;
	bool                        sampled_shape_known = false;
	bool                        found              = false;
	bool                        valid              = true;
};

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
		case ShaderInstructionType::BufferAtomicXor: return true;
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
			use->kind          = ShaderDynamicSLoadResourceKind::StorageBuffer;
			use->texture_usage = ShaderTextureUsage::Unknown;
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
			use->kind                   = ShaderDynamicSLoadResourceKind::StorageBuffer;
			use->storage_usage = ShaderInstructionWritesVectorBufferDescriptor(inst.type) ? ShaderStorageUsage::ReadWrite
			                                                                              : ShaderStorageUsage::ReadOnly;
			use->texture_usage          = ShaderTextureUsage::Unknown;
			use->raw_vmem_oob_guarded = true;
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
			use->kind              = ShaderDynamicSLoadResourceKind::Texture;
			use->texture_usage     = ShaderTextureUsage::ReadOnly;
			use->sampler_operation = ShaderInstructionUsesImageSampler(inst.type)
			                             ? ShaderInstructionSamplerOperation(inst.type)
			                             : State::ImageSampleOperation::Regular;
			use->sampled_shape_known =
			    ShaderGen5SampledTextureShapeForMimgDimension(inst.mimg_dimension, &use->sampled_shape);
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
static ShaderDynamicSLoadUse ShaderFindDynamicSLoadUse(const ShaderCode& code, uint32_t index, const ShaderInstruction& sload,
                                                       bool follow_conditional_branches)
{
	const int dword_count = sload.dst.size;
	ShaderDynamicSLoadUse use {};
	for (uint32_t next_index = index + 1; next_index < code.GetInstructions().Size(); ++next_index)
	{
		const auto& next = code.GetInstructions().At(next_index);
		const bool branch = ShaderInstructionHasStaticBranchTarget(next.type);
		if (next.type == ShaderInstructionType::Unknown || next.type == ShaderInstructionType::SEndpgm ||
		    next.type == ShaderInstructionType::SSetpcB64 ||
		    (branch && (!follow_conditional_branches || next.type == ShaderInstructionType::SBranch)))
		{
			break;
		}
		if (branch)
		{
			continue;
		}

		ShaderDynamicSLoadUse next_use {};
		if (ShaderDynamicSLoadMatchesConsumer(next, sload, &next_use))
		{
			if (use.found && (use.kind != next_use.kind || use.texture_usage != next_use.texture_usage))
			{
				use.valid = false;
				break;
			}
			if (use.found && use.sampled_shape_known && next_use.sampled_shape_known &&
			    use.sampled_shape != next_use.sampled_shape)
			{
				use.valid = false;
				break;
			}
			const bool operation_sensitive =
			    use.kind == ShaderDynamicSLoadResourceKind::Sampler ||
			    (use.kind == ShaderDynamicSLoadResourceKind::Texture && use.texture_usage == ShaderTextureUsage::ReadOnly);
			const auto sampler_operation =
			    use.found && operation_sensitive &&
			            use.sampler_operation != next_use.sampler_operation
			        ? State::ImageSampleOperation::Mixed
			        : next_use.sampler_operation;
			use.kind              = next_use.kind;
			if (next_use.storage_usage == ShaderStorageUsage::ReadWrite)
			{
				use.storage_usage = ShaderStorageUsage::ReadWrite;
			}
			use.texture_usage     = next_use.texture_usage;
			use.sampler_operation = sampler_operation;
			if (next_use.sampled_shape_known)
			{
				use.sampled_shape       = next_use.sampled_shape;
				use.sampled_shape_known = true;
			}
			use.last_consumer_pc   = next.pc;
			use.raw_vmem_oob_guarded = use.raw_vmem_oob_guarded || next_use.raw_vmem_oob_guarded;
			use.found              = true;
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

	return use;
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

// Registers the resource a descriptor S_LOAD materializes and its PC-keyed
// mapping. table holds the loaded words at offset_dw.
static bool ShaderAddDynamicScalarResource(ShaderBindResources* bind, ShaderParsedUsage* info, const ShaderInstruction& sload,
                                           int offset_dw, const ShaderDynamicSLoadUse& use, const HW::UserSgprInfo& user_sgpr,
                                           const uint32_t* table, uint32_t instruction_count)
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
	                                             const uint32_t* extended_buffer, uint16_t eud_size_dw)
{
	EXIT_IF(bind == nullptr || info == nullptr);
	if (!bind->extended.used || extended_buffer == nullptr)
	{
		return;
	}
	const uint32_t instruction_count = code.GetInstructions().Size();
	ShaderCollectSplitTextureResources(code, bind, info, extended_buffer, eud_size_dw);

	for (uint32_t index = 0; index < instruction_count; ++index)
	{
		const auto& sload       = code.GetInstructions().At(index);
		const int   dword_count = ShaderDescriptorSLoadDwords(sload);
		if (dword_count == 0 || sload.src[0].register_id != bind->extended.start_register)
		{
			continue;
		}

		int offset_dw = 0;
		if (!ShaderGetSmemConstantDwordOffset(sload, &offset_dw) || offset_dw < 0)
		{
			continue;
		}
		// offset_dw is the EUD table index (byte_offset/4). Metadata eud_size_dw is a
		// lower bound — same overrun policy as ShaderGen5EudSpanAllowed (api = 16+idx).
		if (!ShaderGen5EudSpanAllowed(16 + offset_dw, dword_count, eud_size_dw))
		{
			continue;
		}

		const auto use = ShaderFindDynamicSLoadUse(code, index, sload, false);
		if (!use.valid || !use.found)
		{
			continue;
		}

		if (!ShaderAddDynamicScalarResource(bind, info, sload, offset_dw, use, user_sgpr, extended_buffer, instruction_count))
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

struct ShaderDescriptorBlock
{
	ShaderInstruction     load;
	int                   first_dword = 0;
	ShaderDynamicSLoadUse use;
};

// Splits the destination of a pointer-table load into descriptors with
// consumers: a block used as one T# or S# stays whole, an 8-dword block may
// also hold two S#, and a 16-dword load is two 8-dword halves. Each block is
// analysed as its own load of the same PC.
static bool ShaderPartitionDescriptorLoad(const ShaderCode& code, uint32_t index, const ShaderInstruction& sload, int first_dword,
                                          int dwords, std::vector<ShaderDescriptorBlock>* blocks)
{
	if (dwords == 16)
	{
		return ShaderPartitionDescriptorLoad(code, index, sload, first_dword, 8, blocks) &&
		       ShaderPartitionDescriptorLoad(code, index, sload, first_dword + 8, 8, blocks);
	}
	ShaderInstruction block = sload;
	block.type              = dwords == 4 ? ShaderInstructionType::SLoadDwordx4 : ShaderInstructionType::SLoadDwordx8;
	block.dst.register_id   = sload.dst.register_id + first_dword;
	block.dst.size          = dwords;
	const auto use          = ShaderFindDynamicSLoadUse(code, index, block, true);
	if (use.valid && use.found && use.kind != ShaderDynamicSLoadResourceKind::StorageBuffer)
	{
		blocks->push_back({block, first_dword, use});
		return true;
	}
	return dwords == 8 && ShaderPartitionDescriptorLoad(code, index, sload, first_dword, 4, blocks) &&
	       ShaderPartitionDescriptorLoad(code, index, sload, first_dword + 4, 4, blocks);
}

// An SRT can hold 64-bit pointers to descriptor tables. A T# or S# read through
// such a pointer at a constant offset comes from the table the pointer selects
// at draw time, so it is materialized like an EUD descriptor load; every
// descriptor of a multi-descriptor load must have consumers. Buffer
// descriptors keep their existing paths.
void ShaderCollectPointerTableResources(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                        ShaderParsedUsage* info, uint16_t srt_size_dw, int user_data_register_base)
{
	EXIT_IF(bind == nullptr || info == nullptr);
	if (srt_size_dw < 2u)
	{
		return;
	}
	const auto     flow              = ShaderScalarFlowOf(code);
	const auto&    entry_values      = flow->holding_entry_value;
	const uint32_t instruction_count = code.GetInstructions().Size();
	for (uint32_t index = 0; index < instruction_count && index < entry_values.size(); ++index)
	{
		const auto& sload       = code.GetInstructions().At(index);
		const int   dword_count = sload.type == ShaderInstructionType::SLoadDwordx16 && sload.dst.type == ShaderOperandType::Sgpr &&
		                                    sload.dst.size == 16 && sload.src_num >= 2 && sload.src[0].type == ShaderOperandType::Sgpr &&
		                                    sload.src[0].size == 2
		                              ? 16
		                              : ShaderDescriptorSLoadDwords(sload);
		int         offset_dw   = 0;
		std::vector<ShaderDescriptorBlock> blocks;
		if (dword_count == 0 || !ShaderLoadsThroughSrtPointer(sload, entry_values[index], srt_size_dw, user_data_register_base, *bind) ||
		    !ShaderGetSmemConstantDwordOffset(sload, &offset_dw) || offset_dw < 0 ||
		    offset_dw > SHADER_GEN5_EUD_MAX_DWORDS - dword_count ||
		    !ShaderPartitionDescriptorLoad(code, index, sload, 0, dword_count, &blocks))
		{
			continue;
		}

		const int      pointer = sload.src[0].register_id - user_data_register_base;
		const uint64_t address = ((static_cast<uint64_t>(user_sgpr.value[pointer + 1]) & 0xffffu) << 32u) | user_sgpr.value[pointer];
		std::array<uint32_t, SHADER_GEN5_EUD_MAX_DWORDS> table {};
		if (!ShaderSnapshotGuestDescriptorTable(address, static_cast<uint32_t>(offset_dw + dword_count), &table))
		{
			EXIT("unreadable descriptor table behind an SRT pointer: pc=0x%08" PRIx32 " sgpr=%d address=0x%016" PRIx64
			     " dwords=%d\n",
			     sload.pc, pointer, address, offset_dw + dword_count);
		}
		for (const auto& block: blocks)
		{
			if (!ShaderAddDynamicScalarResource(bind, info, block.load, offset_dw + block.first_dword, block.use, user_sgpr, table.data(),
			                                    instruction_count))
			{
				EXIT("unable to materialize SRT pointer descriptor: pc=0x%08" PRIx32 " offset_dw=%d dwords=%d kind=%u textures=%d "
				     "samplers=%d\n",
				     sload.pc, offset_dw + block.first_dword, block.load.dst.size, static_cast<unsigned>(block.use.kind),
				     bind->textures2D.textures_num, bind->samplers.samplers_num);
			}
		}
	}
}

// The V# operand of a MUBUF/MTBUF access, or nullptr.
static const ShaderOperand* ShaderVectorBufferDescriptor(const ShaderInstruction& inst)
{
	if ((!ShaderInstructionTypeStartsWith(inst.type, "Buffer") && !ShaderInstructionTypeStartsWith(inst.type, "TBuffer")) ||
	    inst.src_num < 2 ||
	    inst.src[1].type != ShaderOperandType::Sgpr || inst.src[1].size != 4)
	{
		return nullptr;
	}
	return &inst.src[1];
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

// A compiler can move a V# out of its user-data position or build several
// descriptors that share words (one at s1..s4 and one from s0 plus s2..s4).
// Such a descriptor is still known at draw time from the user data alone:
// each consumer gets the storage resource its words select, and the V#
// registers are written with that resource's metadata right before it.
void ShaderCollectAssembledBufferDescriptors(const ShaderCode& code, ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                             int user_sgpr_num, int user_data_register_base)
{
	EXIT_IF(bind == nullptr);
	const auto  flow         = ShaderScalarFlowOf(code);
	const auto& sources      = flow->entry_sources;
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = 0; index < instructions.Size() && index < sources.size(); ++index)
	{
		const auto& inst       = instructions.At(index);
		const auto* descriptor = ShaderVectorBufferDescriptor(inst);
		std::array<uint32_t, 4> words {};
		if (descriptor == nullptr ||
		    ShaderStorageBufferStaticallyBound(bind->storage_buffers, descriptor->register_id - user_data_register_base) ||
		    !ShaderAssembledDescriptorWords(*descriptor, sources[index], user_sgpr, user_sgpr_num, user_data_register_base, &words))
		{
			continue;
		}
		const auto type   = inst.type;
		const bool writes = ShaderInstructionTypeStartsWith(type, "BufferStore") || ShaderInstructionTypeStartsWith(type, "TBufferStore") ||
		                    ShaderInstructionTypeStartsWith(type, "BufferAtomic");
		const bool typed  = ShaderInstructionTypeStartsWith(type, "TBuffer") ||
		                    ShaderInstructionTypeName(type).find("Format") != std::string_view::npos;
		const int  resource = ShaderAddAssembledStorageResource(&bind->storage_buffers, words,
		                                                        descriptor->register_id - user_data_register_base, writes, typed);
		if (resource < 0)
		{
			EXIT("assembled buffer descriptors exceed the storage binding capacity: pc=0x%08" PRIx32 " register=%d\n", inst.pc,
			     descriptor->register_id);
		}
		bind->assembled_descriptors.Add({inst.pc, descriptor->register_id, resource});
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

bool ShaderScalarBufferUsesRuntimeDescriptor(const ShaderBindResources& bind, const ShaderInstruction& inst)
{
	if (!ShaderInstructionIsScalarBufferLoad(inst) || inst.src_num < 1 || inst.src[0].type != ShaderOperandType::Sgpr ||
	    inst.src[0].size != 4 || ShaderIsDynamicScalarStorageConsumer(bind, inst))
	{
		return false;
	}
	const int reg = inst.src[0].register_id;
	for (int i = 0; i < bind.storage_buffers.buffers_num; ++i)
	{
		if (bind.storage_buffers.start_register[i] == reg) { return false; }
	}
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
                                              int user_data_register_base)
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
		const auto exact = AnalyzeShaderStorageUse(code, binding_register + user_data_register_base);
		ShaderStorageUseEvidence unbased {};
		if (user_data_register_base != 0)
		{
			unbased = AnalyzeShaderStorageUse(code, binding_register);
		}
		resources->accesses[index] = ResolveShaderStorageAccessEvidence(true, resources->sources[index], exact.access, unbased.access,
		                                                                 exact.decoded_unknown, exact.indirect_descriptor_use)
		                                 .access;
	}
	ExcludeUnusedMetadataStorage(resources);
}

void ShaderAppendVertexStreamStorage(ShaderVertexInputInfo* info)
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
		storage.buffers_num++;
		stream.storage_slot = index;
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
