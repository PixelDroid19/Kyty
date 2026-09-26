#ifndef KYTY_INTEGRATION_TEST_SHADER_COMPUTE_VULKAN_COMPUTE_PROBE_H_
#define KYTY_INTEGRATION_TEST_SHADER_COMPUTE_VULKAN_COMPUTE_PROBE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Emulator/Graphics/ShaderComputeWaveLayout.h"
#include <vulkan/vulkan.h>

namespace Kyty::Libs::Graphics {

struct ShaderComputeInputInfo;
struct ShaderBindResources;

class VulkanComputeProbe
{
public:
	enum class Result
	{
		Success,
		Unavailable,
		Failure,
		InvalidArgument
	};

	VulkanComputeProbe() = default;
	VulkanComputeProbe(const VulkanComputeProbe&) = delete;
	VulkanComputeProbe& operator=(const VulkanComputeProbe&) = delete;
	~VulkanComputeProbe();

	[[nodiscard]] Result Initialize(std::string* message);
	// The optional paired-wave path uses its own Vulkan 1.2 instance/device
	// feature chain; scalar probes continue to use Initialize's Vulkan 1.0 path.
	[[nodiscard]] Result InitializeWave(std::string* message);
	[[nodiscard]] const ShaderComputeWaveCapabilities& WaveCapabilities() const noexcept { return wave_capabilities_; }
	[[nodiscard]] Result Dispatch(const uint32_t* spirv, size_t word_count, const std::array<uint32_t, 4>& initial_words,
	                              std::array<uint32_t, 4>* result_words, std::string* message) const;
	// Creates one pipeline and reuses it for two submissions with different
	// production program-base metadata. The shared output buffer is read after
	// each fence so the second dispatch cannot hide the first result.
	[[nodiscard]] Result DispatchProgramBasePair(const uint32_t* spirv, size_t word_count,
	                                             const std::array<uint32_t, 8>& initial_words,
	                                             const ShaderComputeInputInfo& input_info,
	                                             uint64_t first_program_base, uint64_t second_program_base,
	                                             std::array<uint32_t, 8>* first_result,
	                                             std::array<uint32_t, 8>* second_result,
	                                             std::string* message) const;
	[[nodiscard]] Result DispatchWave(const uint32_t* spirv, size_t word_count,
	                                  const ShaderComputeWaveLayout& layout,
	                                  const std::array<uint32_t, 3>& groups,
	                                  const std::vector<uint32_t>& initial_words,
	                                  std::vector<uint32_t>* result_words,
	                                  std::string* message) const;
	[[nodiscard]] Result DispatchWaveWithMetadata(const uint32_t* spirv, size_t word_count,
	                                              const ShaderComputeWaveLayout& layout,
	                                              const std::array<uint32_t, 3>& groups,
	                                              const std::vector<uint32_t>& initial_words,
	                                              const ShaderBindResources& bind,
	                                              const std::vector<uint32_t>& metadata,
	                                              uint32_t output_binding,
	                                              std::vector<uint32_t>* result_words,
	                                              std::string* message) const;

private:
	[[nodiscard]] Result InitializeInternal(std::string* message, bool request_wave_features);

	VkInstance       instance_          = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device_   = VK_NULL_HANDLE;
	VkDevice         device_            = VK_NULL_HANDLE;
	VkQueue          queue_             = VK_NULL_HANDLE;
	uint32_t         queue_family_index_ = 0;
	VkDeviceSize     max_storage_buffer_range_ = 0;
	ShaderComputeWaveCapabilities wave_capabilities_ {};
};

} // namespace Kyty::Libs::Graphics

#endif
