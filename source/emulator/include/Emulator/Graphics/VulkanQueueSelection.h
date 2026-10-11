#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANQUEUESELECTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANQUEUESELECTION_H_

#include "Emulator/Common.h"

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct VulkanQueueFamilyCapabilities
{
	uint32_t     queue_count = 0;
	VkQueueFlags queue_flags = 0;
	bool         present     = false;
};

struct VulkanQueueFamilySelection
{
	uint32_t family = UINT32_MAX;
	uint32_t index  = UINT32_MAX;
};

struct VulkanQueueRoleAllocation
{
	uint32_t graphics_index      = UINT32_MAX;
	uint32_t compute_first_index = UINT32_MAX;
	uint32_t compute_count       = 0;
	uint32_t transfer_index      = UINT32_MAX;
	uint32_t present_index       = UINT32_MAX;
	uint32_t family_used         = 0;
};

enum class VulkanQueueSelectionResult : uint8_t
{
	Success,
	InvalidArgument,
	NoCommonFamily,
};

// Kyty currently requires one family for graphics, compute, upload, and
// presentation work because the renderer has no exclusive-family transfer
// contract. Distinct queues within that family remain usable.
[[nodiscard]] VulkanQueueSelectionResult VulkanSelectCommonRenderQueueFamily(const VulkanQueueFamilyCapabilities* families, uint32_t count,
                                                                             VulkanQueueFamilySelection* selection);

[[nodiscard]] VulkanQueueSelectionResult VulkanAllocateSharedQueueRoles(uint32_t queue_count, uint32_t max_compute_count,
                                                                        VulkanQueueRoleAllocation* allocation);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANQUEUESELECTION_H_ */
