#include "Emulator/Graphics/Shader.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Log.h"

#include "ShaderStorageAnalysis.h"

#include <cstdint>
#include <climits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {



bool Gen5HasEudPointer(const ShaderUserData* user_data)
{
	return user_data != nullptr && user_data->eud_size_dw != 0 && user_data->srt_size_dw == 0 &&
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
                                         uint32_t last_consumer_pc, bool raw_vmem_oob_guarded)
{
	EXIT_IF(mappings == nullptr);
	if (sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != dword_count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != dword_count condition ignored (continuing)\n"); }
	for (int mapping = 0; mapping < mappings->mappings_num; ++mapping)
	{
		if (mappings->instruction_pc[mapping] == sload.pc)
		{
			const bool same = mappings->kind[mapping] == kind && mappings->resource_index[mapping] == resource_index &&
			                  mappings->offset_dw[mapping] == offset_dw && mappings->dword_count[mapping] == dword_count &&
			                  mappings->resource_field_offset[mapping] == resource_field_offset &&
			                  mappings->raw_vmem_oob_guarded[mapping] == raw_vmem_oob_guarded;
			if (same && last_consumer_pc > mappings->last_consumer_pc[mapping])
			{
				mappings->last_consumer_pc[mapping] = last_consumer_pc;
			}
			return same;
		}
	}
	if (mappings->mappings_num >= ShaderDynamicSLoadMappings::MAPPINGS_MAX)
	{
		return false;
	}

	const int mapping                        = mappings->mappings_num++;
	mappings->kind[mapping]                  = kind;
	mappings->resource_index[mapping]        = resource_index;
	mappings->destination_register[mapping]  = sload.dst.register_id;
	mappings->instruction_pc[mapping]        = sload.pc;
	mappings->offset_dw[mapping]             = offset_dw;
	mappings->dword_count[mapping]           = dword_count;
	mappings->resource_field_offset[mapping] = resource_field_offset;
	mappings->last_consumer_pc[mapping]      = last_consumer_pc;
	mappings->raw_vmem_oob_guarded[mapping]  = raw_vmem_oob_guarded;
	return true;
}

static bool ShaderAddDynamicScalarStorageResource(ShaderBindResources* bind, const ShaderInstruction& sload, int offset_dw,
	                                               uint32_t last_consumer_pc, bool raw_vmem_oob_guarded,
	                                               const uint32_t* extended_buffer, bool* added_resource)
{
	EXIT_IF(bind == nullptr || extended_buffer == nullptr || added_resource == nullptr);
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
		resources.usages[storage_index]         = ShaderStorageUsage::ReadOnly;
		resources.sources[storage_index]        = ShaderStorageBindingSource::DynamicScalarLoad;
		resources.slots[storage_index]          = offset_dw;
		resources.start_register[storage_index] = sload.dst.register_id;
		resources.extended[storage_index]       = false;
		resources.dynamic_sload[storage_index]  = true;
		*added_resource                         = true;
	}

	return ShaderAddDynamicSLoadMapping(&bind->dynamic_sloads, ShaderDynamicSLoadResourceKind::StorageBuffer, storage_index, sload,
	                                    offset_dw, 4, 0, last_consumer_pc, raw_vmem_oob_guarded);
}

static bool ShaderAddDynamicTextureResource(ShaderBindResources* bind, const ShaderInstruction& sload, int offset_dw,
	                                         uint32_t last_consumer_pc, ShaderTextureUsage usage,
	                                         State::ImageSampleOperation operation,
	                                         ShaderGen5SampledTextureShape sampled_shape, bool sampled_shape_known,
	                                         const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer,
	                                         bool* added_resource)
{
	EXIT_IF(bind == nullptr || extended_buffer == nullptr || added_resource == nullptr);
	if (sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != 8 || usage == ShaderTextureUsage::Unknown) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != 8 || usage == ShaderTextureUsage::Unknown condition ignored (continuing)\n"); }
	*added_resource = false;

	ShaderTextureResource resource {};
	for (int field = 0; field < 8; ++field)
	{
		resource.fields[field] = extended_buffer[offset_dw + field];
	}

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
	                                    0, last_consumer_pc, false);
}

static bool ShaderAddDynamicSamplerResource(ShaderBindResources* bind, const ShaderInstruction& sload, int offset_dw,
	                                         uint32_t last_consumer_pc, State::ImageSampleOperation operation,
	                                         const HW::UserSgprInfo& user_sgpr,
	                                         const uint32_t* extended_buffer, bool* added_resource)
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
	                                    0, last_consumer_pc, false);
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
	if (!ShaderTryGetDwordOffset(load.src[1], offset_dw) || *offset_dw < 0)
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
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
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
		const bool                    shape_known = reads && ShaderGen5SampledTextureShapeForMimgDimension(consumer.mimg_dimension, &shape);
		bool                          added_resource = false;
		const int resource_index = ShaderAddSplitTextureResource(bind, resource, consumer.src[1].register_id, low.offset_dw, usage,
		                                                         operation, shape, shape_known, &added_resource);
		if (resource_index < 0)
		{
			continue;
		}
		const bool low_added  = ShaderAddDynamicSLoadMapping(&bind->dynamic_sloads, ShaderDynamicSLoadResourceKind::Texture, resource_index,
		                                                     *low.instruction, low.offset_dw, 4, 0, consumer.pc, false);
		const bool high_added = ShaderAddDynamicSLoadMapping(&bind->dynamic_sloads, ShaderDynamicSLoadResourceKind::Texture, resource_index,
		                                                     *high.instruction, high.offset_dw, 4, 4, consumer.pc, false);
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
	ShaderTextureUsage             texture_usage     = ShaderTextureUsage::Unknown;
	State::ImageSampleOperation sampler_operation = State::ImageSampleOperation::Regular;
	ShaderGen5SampledTextureShape sampled_shape = ShaderGen5SampledTextureShape::TwoDimensional;
	uint32_t                    last_consumer_pc   = 0;
	bool                        raw_vmem_oob_guarded = false;
	bool                        sampled_shape_known = false;
	bool                        found              = false;
	bool                        valid              = true;
};

static bool ShaderInstructionUsesVectorBufferDescriptor(ShaderInstructionType type)
{
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
		case ShaderInstructionType::BufferStoreDword:
		case ShaderInstructionType::BufferStoreDwordx2:
		case ShaderInstructionType::BufferStoreDwordx3:
		case ShaderInstructionType::BufferStoreDwordx4:
		case ShaderInstructionType::BufferStoreFormatX:
		case ShaderInstructionType::BufferStoreFormatXy:
		case ShaderInstructionType::BufferStoreFormatXyzw:
		case ShaderInstructionType::BufferAtomicAdd:
		case ShaderInstructionType::TBufferLoadFormatX:
		case ShaderInstructionType::TBufferLoadFormatXy:
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
	ShaderCollectSplitTextureResources(code, bind, info, extended_buffer, eud_size_dw);

	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto& sload = code.GetInstructions().At(index);
		const int dword_count = (sload.type == ShaderInstructionType::SLoadDwordx4 ? 4 :
		                         (sload.type == ShaderInstructionType::SLoadDwordx8 ? 8 : 0));
		if (dword_count == 0 || sload.dst.type != ShaderOperandType::Sgpr || sload.dst.size != dword_count || sload.src_num < 2 ||
		    sload.src[0].type != ShaderOperandType::Sgpr || sload.src[0].register_id != bind->extended.start_register ||
		    sload.src[0].size != 2)
		{
			continue;
		}

		int offset_dw = 0;
		if (!ShaderTryGetDwordOffset(sload.src[1], &offset_dw) || offset_dw < 0)
		{
			continue;
		}
		// offset_dw is the EUD table index (byte_offset/4). Metadata eud_size_dw is a
		// lower bound — same overrun policy as ShaderGen5EudSpanAllowed (api = 16+idx).
		if (!ShaderGen5EudSpanAllowed(16 + offset_dw, dword_count, eud_size_dw))
		{
			continue;
		}

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

		if (!use.valid || !use.found)
		{
			continue;
		}

		bool added_resource = false;
		bool added_mapping = false;
		switch (use.kind)
		{
			case ShaderDynamicSLoadResourceKind::StorageBuffer:
				added_mapping = ShaderAddDynamicScalarStorageResource(bind, sload, offset_dw, use.last_consumer_pc,
				                                                      use.raw_vmem_oob_guarded, extended_buffer,
				                                                      &added_resource);
				if (added_resource)
				{
					info->storage_buffers_readonly++;
				}
				break;
			case ShaderDynamicSLoadResourceKind::Texture:
				added_mapping = ShaderAddDynamicTextureResource(bind, sload, offset_dw, use.last_consumer_pc, use.texture_usage,
				                                                use.sampler_operation, use.sampled_shape, use.sampled_shape_known,
				                                                 user_sgpr, extended_buffer, &added_resource);
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
				                                                 extended_buffer, &added_resource);
				if (added_resource)
				{
					info->samplers++;
				}
				break;
		}
		if (!added_mapping)
		{
			EXIT("unable to materialize dynamic descriptor: pc=0x%08" PRIx32 " offset_dw=%d dwords=%d kind=%u "
			     "storage=%d textures=%d samplers=%d mappings=%d eud_dw=%u\n",
			     sload.pc, offset_dw, dword_count, static_cast<unsigned>(use.kind), bind->storage_buffers.buffers_num,
			     bind->textures2D.textures_num, bind->samplers.samplers_num, bind->dynamic_sloads.mappings_num,
			     static_cast<unsigned>(eud_size_dw));
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
	for (int mapping = 0; mapping < bind.dynamic_sloads.mappings_num; ++mapping)
	{
		if (bind.dynamic_sloads.kind[mapping] == ShaderDynamicSLoadResourceKind::StorageBuffer &&
		    bind.dynamic_sloads.destination_register[mapping] == inst.src[0].register_id &&
		    inst.pc > bind.dynamic_sloads.instruction_pc[mapping] && inst.pc <= bind.dynamic_sloads.last_consumer_pc[mapping])
		{
			return true;
		}
	}
	return false;
}

bool ShaderStorageResourceHasDynamicSLoad(const ShaderBindResources& bind, int storage_index)
{
	for (int mapping = 0; mapping < bind.dynamic_sloads.mappings_num; ++mapping)
	{
		if (bind.dynamic_sloads.kind[mapping] == ShaderDynamicSLoadResourceKind::StorageBuffer &&
		    bind.dynamic_sloads.resource_index[mapping] == storage_index)
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

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
