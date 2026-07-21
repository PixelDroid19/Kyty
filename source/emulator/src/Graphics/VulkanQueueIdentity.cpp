#include "Emulator/Graphics/VulkanQueueIdentity.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VulkanQueueLockAssignmentStatus VulkanAssignQueueLockIndices(const VulkanQueueIdentity* identities, uint32_t count,
	                                                          uint32_t* lock_indices, uint32_t* lock_count)
{
	if (identities == nullptr || lock_indices == nullptr || lock_count == nullptr || count == 0)
	{
		return VulkanQueueLockAssignmentStatus::InvalidArgument;
	}

	*lock_count = 0;
	for (uint32_t current = 0; current < count; current++)
	{
		const auto& identity = identities[current];
		if (identity.family == UINT32_MAX || identity.index == UINT32_MAX || identity.vk_queue == nullptr)
		{
			return VulkanQueueLockAssignmentStatus::InvalidIdentity;
		}

		bool assigned = false;
		for (uint32_t previous = 0; previous < current; previous++)
		{
			const auto& candidate            = identities[previous];
			const bool  same_coordinates     = candidate.family == identity.family && candidate.index == identity.index;
			const bool  same_vulkan_queue     = candidate.vk_queue == identity.vk_queue;
			if (same_coordinates != same_vulkan_queue)
			{
				return VulkanQueueLockAssignmentStatus::ConflictingIdentity;
			}
			if (same_coordinates)
			{
				lock_indices[current] = lock_indices[previous];
				assigned              = true;
				break;
			}
		}

		if (!assigned)
		{
			lock_indices[current] = (*lock_count)++;
		}
	}

	return VulkanQueueLockAssignmentStatus::Success;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
