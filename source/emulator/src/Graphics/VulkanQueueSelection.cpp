#include "Emulator/Graphics/VulkanQueueSelection.h"

#include <algorithm>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VulkanQueueSelectionResult VulkanSelectCommonRenderQueueFamily(const VulkanQueueFamilyCapabilities* families, uint32_t count,
                                                               VulkanQueueFamilySelection* selection)
{
	if (families == nullptr || count == 0 || selection == nullptr)
	{
		return VulkanQueueSelectionResult::InvalidArgument;
	}

	*selection               = {};
	bool found_common_family = false;
	for (uint32_t family = 0; family < count; family++)
	{
		const auto& capabilities = families[family];
		if (capabilities.queue_count == 0 || !capabilities.present)
		{
			continue;
		}

		const bool graphics = (capabilities.queue_flags & VK_QUEUE_GRAPHICS_BIT) != 0;
		const bool compute  = (capabilities.queue_flags & VK_QUEUE_COMPUTE_BIT) != 0;
		const bool transfer = (capabilities.queue_flags & VK_QUEUE_TRANSFER_BIT) != 0 || graphics || compute;
		if (!graphics || !compute || !transfer)
		{
			continue;
		}

		if (!found_common_family || capabilities.queue_count > families[selection->family].queue_count)
		{
			selection->family   = family;
			selection->index    = 0;
			found_common_family = true;
		}
	}

	return found_common_family ? VulkanQueueSelectionResult::Success : VulkanQueueSelectionResult::NoCommonFamily;
}

VulkanQueueSelectionResult VulkanAllocateSharedQueueRoles(uint32_t queue_count, uint32_t max_compute_count,
                                                          VulkanQueueRoleAllocation* allocation)
{
	if (queue_count == 0 || max_compute_count == 0 || allocation == nullptr)
	{
		return VulkanQueueSelectionResult::InvalidArgument;
	}

	*allocation                = {};
	allocation->graphics_index = 0;

	// Keep one queue for graphics, upload, and present when the family is
	// constrained. Once those roles can be separated, use the remaining family
	// capacity for compute queues up to the renderer's existing limit.
	const uint32_t reserved_role_count = 3;
	allocation->compute_count          = queue_count > reserved_role_count ? queue_count - reserved_role_count : 1;
	allocation->compute_count          = std::min(allocation->compute_count, max_compute_count);
	allocation->compute_first_index    = queue_count > 1 ? 1 : 0;

	const uint32_t next_index = allocation->compute_first_index + allocation->compute_count;
	if (queue_count >= 3 && next_index < queue_count)
	{
		allocation->transfer_index = next_index;
	} else
	{
		allocation->transfer_index = allocation->graphics_index;
	}

	if (queue_count >= 4 && allocation->transfer_index + 1 < queue_count)
	{
		allocation->present_index = allocation->transfer_index + 1;
	} else
	{
		allocation->present_index = allocation->graphics_index;
	}

	allocation->family_used = std::max(allocation->graphics_index, allocation->compute_first_index + allocation->compute_count - 1) + 1;
	allocation->family_used = std::max(allocation->family_used, allocation->transfer_index + 1);
	allocation->family_used = std::max(allocation->family_used, allocation->present_index + 1);

	return VulkanQueueSelectionResult::Success;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
