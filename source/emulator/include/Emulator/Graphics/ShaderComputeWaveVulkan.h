#pragma once

#include "Emulator/Graphics/ShaderComputeWaveLayout.h"

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace Kyty::Libs::Graphics {

// Captures the distinction between device advertisement, queried support and
// the feature values explicitly enabled at vkCreateDevice time.
struct ShaderComputeWaveVulkanState
{
	bool     extension_advertised = false;
	uint32_t extension_revision = 0;
	bool     extension_enabled = false;
	bool     size_control_feature_supported = false;
	bool     full_subgroups_feature_supported = false;
	bool     size_control_feature_enabled = false;
	bool     full_subgroups_feature_enabled = false;
	bool     compute_required_size_supported = false;
	bool     compute_ballot_shuffle_supported = false;
	uint32_t min_subgroup_size = 0;
	uint32_t max_subgroup_size = 0;
	uint32_t max_local_size[3] {};
	uint32_t max_group_count[3] {};
	uint32_t max_invocations = 0;
	uint32_t max_subgroups = 0;
	uint32_t max_shared_bytes = 0;
};

[[nodiscard]] ShaderComputeWaveCapabilities ShaderComputeWaveVulkanBuildCapabilities(
	const ShaderComputeWaveVulkanState& state) noexcept;

// Integration storage buffers are deliberately capped to keep the probe
// bounded even when a generated fixture is malformed.
[[nodiscard]] bool ShaderComputeWaveVulkanProbeOutputWordCountValid(size_t words) noexcept;

// On success, `required` owns the required-size node that is prepended to the
// existing stage pNext chain. Both structures must remain alive through
// vkCreateComputePipelines.
[[nodiscard]] bool ShaderComputeWaveVulkanAttachRequiredSubgroupSize(
	const ShaderComputeWaveLayout& layout, const ShaderComputeWaveCapabilities& capabilities,
	VkPipelineShaderStageCreateInfo* stage,
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT* required) noexcept;

} // namespace Kyty::Libs::Graphics
