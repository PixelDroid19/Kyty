#include "VulkanWaveProbe.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"
#include "VulkanComputeProbeInternal.h"

#include <limits>

namespace Kyty::Libs::Graphics::VulkanWaveProbeInternal {

namespace {

VulkanComputeProbe::Result DispatchInternal(VkPhysicalDevice physical_device,
	                                           VkDevice device,
	                                           VkQueue queue,
	                                           uint32_t queue_family_index,
	                                           VkDeviceSize max_storage_buffer_range,
	                                           const ShaderComputeWaveCapabilities& capabilities,
	                                           const uint32_t* spirv, size_t word_count,
	                                           const ShaderComputeWaveLayout& layout,
	                                           const std::array<uint32_t, 3>& groups,
	                                           const std::vector<uint32_t>& initial_words,
	                                           const ShaderBindResources* bind,
	                                           const std::vector<uint32_t>* metadata,
	                                           int output_binding,
	                                           std::vector<uint32_t>* result_words,
	                                           std::string* message)
{
	if (message == nullptr) { return VulkanComputeProbe::Result::Failure; }
	*message = "";
	if (result_words == nullptr || spirv == nullptr || word_count == 0 ||
	    word_count > std::numeric_limits<size_t>::max() / sizeof(uint32_t) ||
	    !ShaderComputeWaveVulkanProbeOutputWordCountValid(initial_words.size()) ||
	    !ShaderComputeWaveVulkanProbeOutputWordCountValid(result_words->size()) ||
	    initial_words.size() != result_words->size())
	{
		*message = "wave probe requires matching nonempty buffers of at most 4096 words and valid SPIR-V";
		return VulkanComputeProbe::Result::InvalidArgument;
	}
	const bool metadata_path = bind != nullptr || metadata != nullptr || output_binding >= 0;
	if (metadata_path)
	{
		if (bind == nullptr || metadata == nullptr || output_binding < 0 ||
		    bind->descriptor_set_slot != 0 || bind->vsharp_uniform_buffer || bind->vsharp_binding_index >= 0 ||
		    bind->storage_buffers.buffers_num != 1 || bind->storage_buffers.binding_index < 0 ||
		    !bind->storage_buffers.dynamic_sload[0] ||
		    bind->storage_buffers.sources[0] != ShaderStorageBindingSource::DynamicScalarLoad ||
		    !bind->extended.used || bind->dynamic_sloads.records.Size() == 0 ||
		    bind->textures2D.textures_num != 0 || bind->samplers.samplers_num != 0 || bind->gds_pointers.pointers_num != 0 ||
		    bind->push_constant_size == 0 ||
		    bind->push_constant_size > ShaderBindResources::PORTABLE_PUSH_CONSTANT_BYTES ||
		    (bind->push_constant_size & 15u) != 0u || (bind->push_constant_offset & 3u) != 0u ||
		    bind->push_constant_offset > ShaderBindResources::PORTABLE_PUSH_CONSTANT_BYTES - bind->push_constant_size ||
		    metadata->size() != bind->push_constant_size / sizeof(uint32_t) ||
		    static_cast<uint32_t>(output_binding) == static_cast<uint32_t>(bind->storage_buffers.binding_index))
		{
			*message = "wave metadata probe requires one dynamic storage resource, portable push metadata, and distinct set-zero bindings";
			return VulkanComputeProbe::Result::InvalidArgument;
		}
		for (const auto& record: bind->dynamic_sloads.records)
		{
			if (record.kind != ShaderDynamicSLoadResourceKind::StorageBuffer || record.resource_index != 0)
			{
				*message = "wave metadata probe does not support non-storage or additional dynamic resource mappings";
				return VulkanComputeProbe::Result::InvalidArgument;
			}
		}
	}

	if (layout.strategy != ShaderComputeWaveStrategy::Paired64On32 || layout.guest_wave_size != 64 ||
	    layout.native_subgroup_size != 32 || layout.banks != 2 || layout.waves == 0 ||
	    layout.physical_local[0] == 0 || layout.physical_local[1] != 1 || layout.physical_local[2] != 1)
	{
		*message = "wave probe received an invalid paired compute-wave layout";
		return VulkanComputeProbe::Result::InvalidArgument;
	}
	if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0)
	{
		*result_words = initial_words;
		return VulkanComputeProbe::Result::Success;
	}
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		if (groups[axis] > capabilities.max_group_count[axis])
		{
			*message = "wave probe dispatch group count exceeds a Vulkan device limit";
			return VulkanComputeProbe::Result::Unavailable;
		}
	}

	uint64_t logical_lane_count = 1;
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		if (layout.guest_local[axis] == 0 ||
		    logical_lane_count > std::numeric_limits<uint64_t>::max() / layout.guest_local[axis])
		{
			*message = "wave probe guest local dimensions are invalid or overflow";
			return VulkanComputeProbe::Result::InvalidArgument;
		}
		logical_lane_count *= layout.guest_local[axis];
		if (logical_lane_count > std::numeric_limits<uint64_t>::max() / groups[axis])
		{
			*message = "wave probe logical dispatch size overflows";
			return VulkanComputeProbe::Result::InvalidArgument;
		}
		logical_lane_count *= groups[axis];
	}
	if (logical_lane_count > result_words->size())
	{
		*message = "wave probe output buffer has fewer words than logical guest lanes";
		return VulkanComputeProbe::Result::InvalidArgument;
	}
	const VkDeviceSize buffer_bytes = static_cast<VkDeviceSize>(result_words->size()) * sizeof(uint32_t);
	if (buffer_bytes > max_storage_buffer_range)
	{
		*message = "wave probe storage buffer exceeds maxStorageBufferRange";
		return VulkanComputeProbe::Result::Unavailable;
	}
	if (device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE)
	{
		*message = "wave probe device is not initialized";
		return VulkanComputeProbe::Result::Unavailable;
	}

	VkPipelineShaderStageCreateInfo preflight_stage {};
	preflight_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	preflight_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT preflight_required {};
	if (!ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &preflight_stage, &preflight_required))
	{
		*message = "wave probe layout is unsupported by enabled subgroup features or host limits";
		return VulkanComputeProbe::Result::Unavailable;
	}

	VulkanComputeProbeInternal::DispatchObjects objects {};
	objects.device = device;
	auto result = VulkanComputeProbeInternal::CreateProbeBuffer(device, physical_device, initial_words.data(), buffer_bytes,
	                                                          &objects, message);
	if (result != VulkanComputeProbe::Result::Success) { return result; }
	result = VulkanComputeProbeInternal::CreateProbePipeline(device, spirv, word_count, buffer_bytes, bind, &objects,
	                                                          message, &layout, &capabilities, output_binding);
	if (result != VulkanComputeProbe::Result::Success) { return result; }
	return VulkanComputeProbeInternal::SubmitAndRead(device, queue, queue_family_index, &objects, bind, metadata,
	                                                 result_words->size(), result_words->data(), message, groups);
}

} // namespace

VulkanComputeProbe::Result Dispatch(VkPhysicalDevice physical_device,
	                                VkDevice device,
	                                VkQueue queue,
	                                uint32_t queue_family_index,
	                                VkDeviceSize max_storage_buffer_range,
	                                const ShaderComputeWaveCapabilities& capabilities,
	                                const uint32_t* spirv, size_t word_count,
	                                const ShaderComputeWaveLayout& layout,
	                                const std::array<uint32_t, 3>& groups,
	                                const std::vector<uint32_t>& initial_words,
	                                std::vector<uint32_t>* result_words,
	                                std::string* message)
{
	return DispatchInternal(physical_device, device, queue, queue_family_index, max_storage_buffer_range, capabilities,
	                        spirv, word_count, layout, groups, initial_words, nullptr, nullptr, -1, result_words, message);
}

VulkanComputeProbe::Result DispatchWithMetadata(VkPhysicalDevice physical_device,
	                                            VkDevice device,
	                                            VkQueue queue,
	                                            uint32_t queue_family_index,
	                                            VkDeviceSize max_storage_buffer_range,
	                                            const ShaderComputeWaveCapabilities& capabilities,
	                                            const uint32_t* spirv, size_t word_count,
	                                            const ShaderComputeWaveLayout& layout,
	                                            const std::array<uint32_t, 3>& groups,
	                                            const std::vector<uint32_t>& initial_words,
	                                            const ShaderBindResources& bind,
	                                            const std::vector<uint32_t>& metadata,
	                                            uint32_t output_binding,
	                                            std::vector<uint32_t>* result_words,
	                                            std::string* message)
{
	if (output_binding > static_cast<uint32_t>(std::numeric_limits<int>::max()))
	{
		if (message != nullptr) { *message = "wave metadata probe output binding is out of range"; }
		return VulkanComputeProbe::Result::InvalidArgument;
	}
	return DispatchInternal(physical_device, device, queue, queue_family_index, max_storage_buffer_range, capabilities,
	                        spirv, word_count, layout, groups, initial_words, &bind, &metadata,
	                        static_cast<int>(output_binding), result_words, message);
}

} // namespace Kyty::Libs::Graphics::VulkanWaveProbeInternal

namespace Kyty::Libs::Graphics {

VulkanComputeProbe::Result VulkanComputeProbe::DispatchWave(
	const uint32_t* spirv, size_t word_count, const ShaderComputeWaveLayout& layout,
	const std::array<uint32_t, 3>& groups, const std::vector<uint32_t>& initial_words,
	std::vector<uint32_t>* result_words, std::string* message) const
{
	return VulkanWaveProbeInternal::Dispatch(physical_device_, device_, queue_, queue_family_index_,
	                                        max_storage_buffer_range_, wave_capabilities_, spirv,
	                                        word_count, layout, groups, initial_words, result_words, message);
}

VulkanComputeProbe::Result VulkanComputeProbe::DispatchWaveWithMetadata(
	const uint32_t* spirv, size_t word_count, const ShaderComputeWaveLayout& layout,
	const std::array<uint32_t, 3>& groups, const std::vector<uint32_t>& initial_words,
	const ShaderBindResources& bind, const std::vector<uint32_t>& metadata, uint32_t output_binding,
	std::vector<uint32_t>* result_words, std::string* message) const
{
	return VulkanWaveProbeInternal::DispatchWithMetadata(
	    physical_device_, device_, queue_, queue_family_index_, max_storage_buffer_range_, wave_capabilities_, spirv,
	    word_count, layout, groups, initial_words, bind, metadata, output_binding, result_words, message);
}

} // namespace Kyty::Libs::Graphics
