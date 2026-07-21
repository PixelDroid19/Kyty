#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANQUEUEIDENTITY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANQUEUEIDENTITY_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct VulkanQueueIdentity
{
	uint32_t family   = UINT32_MAX;
	uint32_t index    = UINT32_MAX;
	VkQueue  vk_queue = nullptr;
};

enum class VulkanQueueLockAssignmentStatus
{
	Success,
	InvalidArgument,
	InvalidIdentity,
	ConflictingIdentity,
};

[[nodiscard]] VulkanQueueLockAssignmentStatus VulkanAssignQueueLockIndices(const VulkanQueueIdentity* identities,
	                                                                        uint32_t count, uint32_t* lock_indices,
	                                                                        uint32_t* lock_count);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANQUEUEIDENTITY_H_ */
