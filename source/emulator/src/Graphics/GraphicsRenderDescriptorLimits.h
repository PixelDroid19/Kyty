#pragma once

#include "Emulator/Graphics/ShaderDescriptorLimits.h"

#include <vulkan/vulkan_core.h>

namespace Kyty::Libs::Graphics {

[[nodiscard]] inline ShaderDescriptorLimits::DeviceLimits
ShaderDescriptorLimitsFromVulkan(const VkPhysicalDeviceLimits& limits) noexcept
{
	ShaderDescriptorLimits::DeviceLimits result {};
	result.max_per_stage_sampled_images  = limits.maxPerStageDescriptorSampledImages;
	result.max_per_stage_storage_images  = limits.maxPerStageDescriptorStorageImages;
	result.max_per_stage_samplers        = limits.maxPerStageDescriptorSamplers;
	result.max_per_stage_storage_buffers = limits.maxPerStageDescriptorStorageBuffers;
	result.max_per_stage_uniform_buffers = limits.maxPerStageDescriptorUniformBuffers;
	result.max_per_stage_resources       = limits.maxPerStageResources;
	result.max_pipeline_sampled_images   = limits.maxDescriptorSetSampledImages;
	result.max_pipeline_storage_images   = limits.maxDescriptorSetStorageImages;
	result.max_pipeline_samplers         = limits.maxDescriptorSetSamplers;
	result.max_pipeline_storage_buffers  = limits.maxDescriptorSetStorageBuffers;
	result.max_pipeline_uniform_buffers  = limits.maxDescriptorSetUniformBuffers;
	result.max_fragment_combined_output_resources = limits.maxFragmentCombinedOutputResources;
	return result;
}

} // namespace Kyty::Libs::Graphics
