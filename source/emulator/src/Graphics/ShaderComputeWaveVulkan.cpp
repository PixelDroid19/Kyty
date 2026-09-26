#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"

#include <limits>

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint32_t kGuestWaveSize = 64;
constexpr uint32_t kNativeSubgroupSize = 32;

bool MultiplyU64(uint64_t a, uint64_t b, uint64_t* product) noexcept
{
	if (product == nullptr || (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)) { return false; }
	*product = a * b;
	return true;
}

} // namespace

ShaderComputeWaveCapabilities ShaderComputeWaveVulkanBuildCapabilities(const ShaderComputeWaveVulkanState& state) noexcept
{
	ShaderComputeWaveCapabilities capabilities {};
	const bool extension_v2_enabled = state.extension_advertised && state.extension_revision >= 2 && state.extension_enabled;
	capabilities.size_control_enabled = extension_v2_enabled && state.size_control_feature_supported && state.size_control_feature_enabled;
	capabilities.full_subgroups_enabled =
	    extension_v2_enabled && state.full_subgroups_feature_supported && state.full_subgroups_feature_enabled;
	capabilities.compute_required_size_supported = extension_v2_enabled && state.compute_required_size_supported;
	capabilities.compute_ballot_shuffle_supported = state.compute_ballot_shuffle_supported;
	capabilities.min_subgroup_size = state.min_subgroup_size;
	capabilities.max_subgroup_size = state.max_subgroup_size;
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		capabilities.max_local_size[axis] = state.max_local_size[axis];
		capabilities.max_group_count[axis] = state.max_group_count[axis];
	}
	capabilities.max_invocations = state.max_invocations;
	capabilities.max_subgroups = state.max_subgroups;
	capabilities.max_shared_bytes = state.max_shared_bytes;
	return capabilities;
}

bool ShaderComputeWaveVulkanProbeOutputWordCountValid(size_t words) noexcept
{
	return words > 0 && words <= 4096;
}

bool ShaderComputeWaveVulkanAttachRequiredSubgroupSize(
	const ShaderComputeWaveLayout& layout, const ShaderComputeWaveCapabilities& capabilities,
	VkPipelineShaderStageCreateInfo* stage,
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT* required) noexcept
{
	if (stage == nullptr || required == nullptr || stage->stage != VK_SHADER_STAGE_COMPUTE_BIT ||
	    layout.strategy != ShaderComputeWaveStrategy::Paired64On32 || layout.guest_wave_size != kGuestWaveSize ||
	    layout.native_subgroup_size != kNativeSubgroupSize || layout.banks != 2 || layout.waves == 0 ||
	    !capabilities.size_control_enabled || !capabilities.full_subgroups_enabled ||
	    !capabilities.compute_required_size_supported || !capabilities.compute_ballot_shuffle_supported ||
	    capabilities.min_subgroup_size > kNativeSubgroupSize || capabilities.max_subgroup_size < kNativeSubgroupSize)
	{
		return false;
	}

	uint64_t guest_invocations = 1;
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		if (layout.guest_local[axis] == 0 ||
		    !MultiplyU64(guest_invocations, layout.guest_local[axis], &guest_invocations))
		{
			return false;
		}
	}
	uint64_t expected_physical_x = 0;
	uint64_t lds_bytes = 0;
	// The trailing guest wave may be partial.
	if ((guest_invocations + kGuestWaveSize - 1u) / kGuestWaveSize != layout.waves ||
	    !MultiplyU64(layout.waves, kNativeSubgroupSize, &expected_physical_x) ||
	    !MultiplyU64(layout.lds_dwords, sizeof(uint32_t), &lds_bytes))
	{
		return false;
	}

	if (expected_physical_x != layout.physical_local[0] || layout.physical_local[0] == 0 ||
	    (layout.physical_local[0] % kNativeSubgroupSize) != 0 || layout.physical_local[1] != 1 ||
	    layout.physical_local[2] != 1 || layout.physical_local[0] > capabilities.max_local_size[0] ||
	    layout.physical_local[1] > capabilities.max_local_size[1] ||
	    layout.physical_local[2] > capabilities.max_local_size[2] ||
	    expected_physical_x > capabilities.max_invocations || layout.waves > capabilities.max_subgroups ||
	    lds_bytes > capabilities.max_shared_bytes)
	{
		return false;
	}

	for (const auto* chain = static_cast<const VkBaseInStructure*>(stage->pNext); chain != nullptr; chain = chain->pNext)
	{
		if (chain->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT) { return false; }
	}

	// Attach our node at the head rather than replacing an existing stage chain.
	required->sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
	required->pNext               = const_cast<void*>(stage->pNext);
	required->requiredSubgroupSize = kNativeSubgroupSize;
	stage->pNext                  = required;
	stage->flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
	stage->flags &= ~VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT;
	return true;
}

} // namespace Kyty::Libs::Graphics
