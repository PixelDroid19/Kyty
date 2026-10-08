#pragma once

#include "Emulator/Graphics/ShaderComputeWaveLayout.h"

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace Kyty::Libs::Graphics {

// Captures the distinction between device advertisement, queried support and
// the feature values explicitly enabled at vkCreateDevice time. Subgroup size
// control is core since Vulkan 1.3: the extension fields are diagnostics, and
// extension_enabled stays false when the core features are enabled instead.
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
	bool     fragment_required_size_supported = false;
	bool     vertex_required_size_supported = false;
	bool     quad_operations_in_all_stages = false;
	bool     subgroup_broadcast_dynamic_id_enabled = false;
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

struct ShaderSubgroupModuleCheck
{
	bool     supported = false;
	uint32_t required_operations = 0;
	size_t   word = 0;
	const char* reason = nullptr;
};

// Inspect the binary, not the IR or cache-miss path. Checks both declarations
// and instructions so a missing capability cannot hide an emitted operation.
[[nodiscard]] ShaderSubgroupModuleCheck ShaderCheckSubgroupModule(
	const uint32_t* words, size_t count, VkShaderStageFlagBits stage,
	uint32_t supported_stages, uint32_t supported_operations,
	const ShaderComputeWaveVulkanState& state, bool maximal_reconvergence) noexcept;

[[nodiscard]] bool ShaderRequiredSubgroupSizeSupported(
	const ShaderComputeWaveVulkanState& state, VkShaderStageFlagBits stage, uint32_t size) noexcept;

struct ShaderNativeSubgroupSelection
{
	bool supported = false;
	// When supported, zero means unspecified: every queried width is covered.
	// Otherwise this is an exact width, implicit or fixed by a required-size node.
	uint32_t size = 0;
	bool require_size = false;
};

// Same-guest-size mapping is always a width candidate. The neutral-region proof
// additionally permits fragment guest64 on host32. Lane/quad-local lowering
// permits smaller complete-quad subgroups, never a host wider than the guest mask.
// Varying stages require every power-of-two width in the queried range to be
// covered, or an attachable exact size. SPIR-V 1.6 permits variation even with
// no ALLOW_VARYING flag; callers must supply the actual module's policy.
// Bounds must be genuinely queried; proof booleans are mutually exclusive.
[[nodiscard]] ShaderNativeSubgroupSelection ShaderSelectNativeSubgroup(
	const ShaderComputeWaveVulkanState& state, VkShaderStageFlagBits stage,
	uint32_t default_size, uint32_t guest_size, bool lane_local, bool fragment_neutral32, bool varying_subgroups) noexcept;

// Preserve an existing pNext chain. A covered unspecified width adds no node.
// Failure leaves both output structures alone.
[[nodiscard]] bool ShaderAttachNativeSubgroup(
	const ShaderComputeWaveVulkanState& state, const ShaderNativeSubgroupSelection& selection,
	VkPipelineShaderStageCreateInfo* stage, VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT* required) noexcept;

// The host can force a fragment shader onto the exact subgroup size a wave32
// guest program needs for its lane indices to map 1:1 onto host lanes.
[[nodiscard]] bool ShaderFragmentRequiredSubgroupSizeSupported(
	const ShaderComputeWaveVulkanState& state, uint32_t required_size) noexcept;

// Legacy feature/width prerequisite, NOT a shader admission proof. Even an
// exact-size subgroup can have unavailable or all-helper source quads; callers
// also need the program's participation proof. The binary gate checks the actual
// emitted operations. A true result may require attaching an exact-size node;
// only a non-varying module or a queried singleton range fixes the implicit size.
[[nodiscard]] bool ShaderWave32FragmentNativeLaneExchangeSupported(
	uint32_t subgroup_stages, uint32_t subgroup_operations, bool maximal_reconvergence,
	uint32_t device_default_subgroup_size, const ShaderComputeWaveVulkanState& state, bool varying_subgroups) noexcept;

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
