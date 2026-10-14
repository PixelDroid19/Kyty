#ifndef KYTY_INTEGRATION_TEST_SHADER_COMPUTE_VULKAN_COMPUTE_PROBE_INTERNAL_H_
#define KYTY_INTEGRATION_TEST_SHADER_COMPUTE_VULKAN_COMPUTE_PROBE_INTERNAL_H_

#include "VulkanComputeProbe.h"

#include <array>
#include <vector>

namespace Kyty::Libs::Graphics {

struct ShaderBindResources;

namespace VulkanComputeProbeInternal {

inline constexpr VkDeviceSize kBasicProbeBufferSize       = 4u * sizeof(uint32_t);
inline constexpr VkDeviceSize kProgramBaseProbeBufferSize = 8u * sizeof(uint32_t);

void SetVkError(std::string* message, const char* operation, VkResult result);

struct DispatchObjects
{
	VkDevice                  device          = VK_NULL_HANDLE;
	VkDeviceSize              buffer_size     = 0;
	VkBuffer                  buffer          = VK_NULL_HANDLE;
	VkDeviceMemory            memory          = VK_NULL_HANDLE;
	VkBuffer                  metadata_buffer = VK_NULL_HANDLE;
	VkDeviceMemory            metadata_memory = VK_NULL_HANDLE;
	VkShaderModule            shader          = VK_NULL_HANDLE;
	VkDescriptorSetLayout     set_layout      = VK_NULL_HANDLE;
	VkPipelineLayout          pipeline_layout = VK_NULL_HANDLE;
	VkDescriptorPool          descriptor_pool = VK_NULL_HANDLE;
	VkDescriptorSet           descriptor_set  = VK_NULL_HANDLE;
	VkPipeline                pipeline        = VK_NULL_HANDLE;
	VkCommandPool             command_pool    = VK_NULL_HANDLE;
	VkFence                   fence           = VK_NULL_HANDLE;
	void*                     mapped          = nullptr;
	void*                     metadata_mapped = nullptr;
	// False when the selected host-visible type is not HOST_COHERENT; such memory needs explicit flush and invalidate.
	bool                      memory_coherent   = true;
	bool                      metadata_coherent = true;
	// The output seed is host-written once at creation and must be flushed before the first submission.
	bool                      output_host_dirty = false;

	~DispatchObjects();
};

[[nodiscard]] VulkanComputeProbe::Result CreateProbeBuffer(VkDevice device, VkPhysicalDevice physical_device,
	                                                          const void* initial_data, VkDeviceSize buffer_size,
	                                                          DispatchObjects* objects, std::string* message);
[[nodiscard]] VulkanComputeProbe::Result CreateMetadataBuffer(VkDevice device, VkPhysicalDevice physical_device,
	                                                             VkDeviceSize size, DispatchObjects* objects,
	                                                             std::string* message);
[[nodiscard]] VulkanComputeProbe::Result CreateProbePipeline(VkDevice device, const uint32_t* spirv, size_t word_count,
	                                                          VkDeviceSize storage_buffer_size,
	                                                          const ShaderBindResources* bind,
	                                                          DispatchObjects* objects, std::string* message,
	                                                          const ShaderComputeWaveLayout* wave_layout = nullptr,
	                                                          const ShaderComputeWaveCapabilities* wave_capabilities = nullptr,
	                                                          int output_binding = -1);
[[nodiscard]] VulkanComputeProbe::Result SubmitAndRead(VkDevice device, VkQueue queue, uint32_t queue_family,
	                                                     DispatchObjects* objects, const ShaderBindResources* bind,
	                                                     const std::vector<uint32_t>* metadata,
	                                                     size_t output_word_count, void* result_words,
	                                                     std::string* message,
	                                                     const std::array<uint32_t, 3>& groups = std::array<uint32_t, 3> {1, 1, 1});

} // namespace VulkanComputeProbeInternal

} // namespace Kyty::Libs::Graphics

#endif
