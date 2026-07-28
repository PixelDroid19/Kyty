#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VULKANIMAGEBUILDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VULKANIMAGEBUILDER_H_

#include "Emulator/Graphics/GraphicContext.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Complete, explicit description of the VkImage fields shared by every GPU object.
// Callers provide object-specific dimensions, format, usage, and sample count only.
struct VulkanImageDescriptor
{
	VkImageCreateFlags    flags          = 0;
	VkImageType           image_type     = VK_IMAGE_TYPE_2D;
	VkExtent3D            extent         = {0, 0, 1};
	uint32_t              mip_levels     = 1;
	uint32_t              array_layers   = 1;
	VkFormat              format         = VK_FORMAT_UNDEFINED;
	VkImageTiling         tiling         = VK_IMAGE_TILING_OPTIMAL;
	VkImageLayout         initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageUsageFlags     usage          = 0;
	VkSampleCountFlagBits samples        = VK_SAMPLE_COUNT_1_BIT;
};

[[nodiscard]] VkImageCreateInfo VulkanBuildImageCreateInfo(const VulkanImageDescriptor& descriptor);

struct VulkanImageViewDescriptor
{
	VkImage            image            = nullptr;
	VkImageViewType    view_type        = VK_IMAGE_VIEW_TYPE_2D;
	VkFormat           format           = VK_FORMAT_UNDEFINED;
	VkComponentMapping components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                                       VK_COMPONENT_SWIZZLE_IDENTITY};
	VkImageAspectFlags aspect_mask      = VK_IMAGE_ASPECT_COLOR_BIT;
	uint32_t           base_mip_level   = 0;
	uint32_t           level_count      = 1;
	uint32_t           base_array_layer = 0;
	uint32_t           layer_count      = 1;
};

[[nodiscard]] VkImageViewCreateInfo VulkanBuildImageViewCreateInfo(const VulkanImageViewDescriptor& descriptor);

// Create one view and publish it only on success.
[[nodiscard]] bool VulkanCreateDeviceImageView(VkDevice device, const VulkanImageViewDescriptor& descriptor, VkImageView* view);

// Canonical color-image view set used by render targets and video buffers.
// Creation is atomic: a partial set is destroyed and cleared on failure.
[[nodiscard]] bool VulkanCreateStandardColorImageViews(GraphicContext* context, VulkanImage* image);

// Decode the four guest 3-bit selectors. Unknown selector values are rejected;
// they are never rewritten to IDENTITY.
[[nodiscard]] bool VulkanDecodeComponentMapping(uint32_t packed_selectors, VkComponentMapping* mapping);

// Vulkan storage views require identity mapping. The one representable BGRA
// case is expressed through the image format itself; all other mappings fail.
[[nodiscard]] bool VulkanNormalizeStorageComponentMapping(VkFormat* format, VkComponentMapping* mapping);

[[nodiscard]] bool VulkanImageFormatSupported(const GraphicContext* context, const VkImageCreateInfo& image_info);

// Create, allocate, bind, and publish one device-local image as an atomic operation.
// Returns false on a Vulkan creation/allocation failure and leaves image->image null.
[[nodiscard]] bool VulkanCreateDeviceImage(GraphicContext* context, const VkImageCreateInfo& image_info, VulkanImage* image,
                                           VulkanMemory* memory);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VULKANIMAGEBUILDER_H_
