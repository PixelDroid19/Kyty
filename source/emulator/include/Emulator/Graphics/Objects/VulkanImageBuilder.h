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

[[nodiscard]] VkImageViewCreateInfo VulkanBuildImageViewCreateInfo(const VulkanImageViewDescriptor& descriptor);

// Create one view and publish it only on success.
[[nodiscard]] bool VulkanCreateDeviceImageView(VkDevice device, const VulkanImageViewDescriptor& descriptor, VkImageView* view);

// Sampled views are descriptor-specific, even when their live backing belongs
// to an attachment or a storage descriptor with identity write components.
[[nodiscard]] bool VulkanPlanSampledImageView(const VulkanImage& image, VkImageViewType view_type,
                                             VkImageAspectFlags aspect, uint32_t base_mip, uint32_t mip_count,
                                             uint32_t base_layer, uint32_t layer_count, uint32_t selectors,
                                             VulkanImageViewDescriptor* descriptor);
[[nodiscard]] bool VulkanImageViewDescriptorsEqual(const VulkanImageViewDescriptor& a, const VulkanImageViewDescriptor& b);

using VulkanImageViewCreator = bool (*)(VkDevice, const VulkanImageViewDescriptor&, VkImageView*);
// Called under the renderer's resource lock. Injectable creation permits a CPU
// contract test of cache identity/lifetime without a Vulkan device.
[[nodiscard]] int VulkanGetOrCreateSampledImageView(VkDevice device, VulkanImage* image,
                                                   const VulkanImageViewDescriptor& descriptor,
                                                   VulkanImageViewCreator create = VulkanCreateDeviceImageView);

// Canonical color-image view set used by render targets and video buffers.
// Creation is atomic: a partial set is destroyed and cleared on failure.
[[nodiscard]] bool VulkanCreateStandardColorImageViews(GraphicContext* context, VulkanImage* image);

// Create the alternate UNORM/sRGB attachment view for a mutable color image.
[[nodiscard]] bool VulkanCreateCompatibleColorAttachmentViews(GraphicContext* context, VulkanImage* image);

// Resolve the view that preserves the guest color-attachment transfer domain.
// Returns -1 when the image and attachment formats are not view-compatible.
[[nodiscard]] int VulkanResolveColorAttachmentView(VkFormat image_format, VkFormat attachment_format);

// Resolve the descriptor view for a storage-image bind. Render-target arrays
// reuse their canonical identity array view; storage textures keep their
// dedicated normalized storage view.
[[nodiscard]] bool VulkanResolveStorageImageView(const VulkanImage* image, bool three_dimensional, bool arrayed_2d, int* view_index,
                                                 uint32_t base_mip_level = 0u);

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
