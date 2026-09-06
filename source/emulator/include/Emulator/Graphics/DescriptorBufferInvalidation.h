#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DESCRIPTORBUFFERINVALIDATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DESCRIPTORBUFFERINVALIDATION_H_

#include <vulkan/vulkan_core.h>

namespace Kyty::Libs::Graphics {

[[nodiscard]] inline bool VulkanBufferNeedsDescriptorInvalidation(VkBufferUsageFlags usage)
{
	// Transfer, vertex, index and indirect-only allocations cannot occur in a
	// valid buffer descriptor. Retain the scan for zero or unfamiliar usage,
	// and for any combination containing shader-visible descriptor usage.
	constexpr VkBufferUsageFlags non_descriptor = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
	                                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
	                                              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
	return usage == 0u || (usage & ~non_descriptor) != 0u;
}

} // namespace Kyty::Libs::Graphics

#endif
