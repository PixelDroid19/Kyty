#ifndef KYTY_INTEGRATION_TEST_SHADER_COMPUTE_VULKAN_WAVE_PROBE_H_
#define KYTY_INTEGRATION_TEST_SHADER_COMPUTE_VULKAN_WAVE_PROBE_H_

#include "VulkanComputeProbe.h"

#include <vector>

namespace Kyty::Libs::Graphics::VulkanWaveProbeInternal {

[[nodiscard]] VulkanComputeProbe::Result Dispatch(
	VkPhysicalDevice physical_device, VkDevice device, VkQueue queue, uint32_t queue_family_index,
	VkDeviceSize max_storage_buffer_range,
	const ShaderComputeWaveCapabilities& capabilities, const uint32_t* spirv, size_t word_count,
	const ShaderComputeWaveLayout& layout, const std::array<uint32_t, 3>& groups,
	const std::vector<uint32_t>& initial_words, std::vector<uint32_t>* result_words, std::string* message);

[[nodiscard]] VulkanComputeProbe::Result DispatchWithMetadata(
	VkPhysicalDevice physical_device, VkDevice device, VkQueue queue, uint32_t queue_family_index,
	VkDeviceSize max_storage_buffer_range,
	const ShaderComputeWaveCapabilities& capabilities, const uint32_t* spirv, size_t word_count,
	const ShaderComputeWaveLayout& layout, const std::array<uint32_t, 3>& groups,
	const std::vector<uint32_t>& initial_words, const ShaderBindResources& bind,
	const std::vector<uint32_t>& metadata, uint32_t output_binding,
	std::vector<uint32_t>* result_words, std::string* message);

} // namespace Kyty::Libs::Graphics::VulkanWaveProbeInternal

#endif
