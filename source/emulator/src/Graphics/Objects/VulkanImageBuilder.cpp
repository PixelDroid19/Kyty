#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"

#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VkImageCreateInfo VulkanBuildImageCreateInfo(const VulkanImageDescriptor& descriptor)
{
	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = descriptor.flags;
	image_info.imageType     = descriptor.image_type;
	image_info.extent        = descriptor.extent;
	image_info.mipLevels     = descriptor.mip_levels;
	image_info.arrayLayers   = descriptor.array_layers;
	image_info.format        = descriptor.format;
	image_info.tiling        = descriptor.tiling;
	image_info.initialLayout = descriptor.initial_layout;
	image_info.usage         = descriptor.usage;
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = descriptor.samples;
	return image_info;
}

VkImageViewCreateInfo VulkanBuildImageViewCreateInfo(const VulkanImageViewDescriptor& descriptor)
{
	VkImageViewCreateInfo view_info {};
	view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.pNext                           = nullptr;
	view_info.flags                           = 0;
	view_info.image                           = descriptor.image;
	view_info.viewType                        = descriptor.view_type;
	view_info.format                          = descriptor.format;
	view_info.components                      = descriptor.components;
	view_info.subresourceRange.aspectMask     = descriptor.aspect_mask;
	view_info.subresourceRange.baseMipLevel   = descriptor.base_mip_level;
	view_info.subresourceRange.levelCount     = descriptor.level_count;
	view_info.subresourceRange.baseArrayLayer = descriptor.base_array_layer;
	view_info.subresourceRange.layerCount     = descriptor.layer_count;
	return view_info;
}

bool VulkanCreateDeviceImageView(VkDevice device, const VulkanImageViewDescriptor& descriptor, VkImageView* view)
{
	EXIT_IF(device == nullptr || view == nullptr);
	*view                = nullptr;
	const auto view_info = VulkanBuildImageViewCreateInfo(descriptor);
	return vkCreateImageView(device, &view_info, nullptr, view) == VK_SUCCESS && *view != nullptr;
}

bool VulkanCreateStandardColorImageViews(GraphicContext* context, VulkanImage* image)
{
	EXIT_IF(context == nullptr || image == nullptr);

	VulkanImageViewDescriptor descriptor {};
	descriptor.image  = image->image;
	descriptor.format = image->format;

	auto create = [&](int index) { return VulkanCreateDeviceImageView(context->device, descriptor, &image->image_view[index]); };
	if (!create(VulkanImage::VIEW_DEFAULT))
	{
		return false;
	}
	descriptor.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	if (!create(VulkanImage::VIEW_ARRAY))
	{
		goto fail;
	}
	descriptor.view_type  = VK_IMAGE_VIEW_TYPE_2D;
	descriptor.components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_IDENTITY};
	if (!create(VulkanImage::VIEW_BGRA))
	{
		goto fail;
	}
	descriptor.components = {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R};
	if (!create(VulkanImage::VIEW_ABGR))
	{
		goto fail;
	}
	return true;

fail:
	for (auto& view: image->image_view)
	{
		if (view != nullptr)
		{
			vkDestroyImageView(context->device, view, nullptr);
			view = nullptr;
		}
	}
	return false;
}

namespace {

bool DecodeComponentSwizzle(uint8_t selector, VkComponentSwizzle* swizzle)
{
	EXIT_IF(swizzle == nullptr);
	switch (selector)
	{
		case 0: *swizzle = VK_COMPONENT_SWIZZLE_ZERO; return true;
		case 1: *swizzle = VK_COMPONENT_SWIZZLE_ONE; return true;
		case 4: *swizzle = VK_COMPONENT_SWIZZLE_R; return true;
		case 5: *swizzle = VK_COMPONENT_SWIZZLE_G; return true;
		case 6: *swizzle = VK_COMPONENT_SWIZZLE_B; return true;
		case 7: *swizzle = VK_COMPONENT_SWIZZLE_A; return true;
		case 2:
		case 3: return false;
	}
	return false;
}

bool IsIdentityMapping(const VkComponentMapping& mapping)
{
	return mapping.r == VK_COMPONENT_SWIZZLE_R && mapping.g == VK_COMPONENT_SWIZZLE_G && mapping.b == VK_COMPONENT_SWIZZLE_B &&
	       mapping.a == VK_COMPONENT_SWIZZLE_A;
}

} // namespace

bool VulkanDecodeComponentMapping(uint32_t packed_selectors, VkComponentMapping* mapping)
{
	EXIT_IF(mapping == nullptr);
	VkComponentMapping decoded {};
	if (!DecodeComponentSwizzle(static_cast<uint8_t>((packed_selectors >> 0u) & 0x7u), &decoded.r) ||
	    !DecodeComponentSwizzle(static_cast<uint8_t>((packed_selectors >> 3u) & 0x7u), &decoded.g) ||
	    !DecodeComponentSwizzle(static_cast<uint8_t>((packed_selectors >> 6u) & 0x7u), &decoded.b) ||
	    !DecodeComponentSwizzle(static_cast<uint8_t>((packed_selectors >> 9u) & 0x7u), &decoded.a))
	{
		return false;
	}
	*mapping = decoded;
	return true;
}

bool VulkanNormalizeStorageComponentMapping(VkFormat* format, VkComponentMapping* mapping)
{
	EXIT_IF(format == nullptr || mapping == nullptr);
	if (IsIdentityMapping(*mapping))
	{
		return true;
	}
	if (mapping->r == VK_COMPONENT_SWIZZLE_B && mapping->g == VK_COMPONENT_SWIZZLE_G && mapping->b == VK_COMPONENT_SWIZZLE_R &&
	    mapping->a == VK_COMPONENT_SWIZZLE_A && *format == VK_FORMAT_R8G8B8A8_SRGB)
	{
		*format  = VK_FORMAT_B8G8R8A8_SRGB;
		*mapping = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
		return true;
	}
	return false;
}

bool VulkanImageFormatSupported(const GraphicContext* context, const VkImageCreateInfo& image_info)
{
	EXIT_IF(context == nullptr);
	VkImageFormatProperties properties {};
	return vkGetPhysicalDeviceImageFormatProperties(context->physical_device, image_info.format, image_info.imageType, image_info.tiling,
	                                                image_info.usage, image_info.flags, &properties) == VK_SUCCESS;
}

bool VulkanCreateDeviceImage(GraphicContext* context, const VkImageCreateInfo& image_info, VulkanImage* image, VulkanMemory* memory)
{
	EXIT_IF(context == nullptr || image == nullptr || memory == nullptr);
	EXIT_IF(image->image != nullptr);

	if (vkCreateImage(context->device, &image_info, nullptr, &image->image) != VK_SUCCESS || image->image == nullptr)
	{
		image->image = nullptr;
		return false;
	}
	vkGetImageMemoryRequirements(context->device, image->image, &memory->requirements);
	memory->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (!VulkanAllocate(context, memory))
	{
		vkDestroyImage(context->device, image->image, nullptr);
		image->image = nullptr;
		return false;
	}
	VulkanBindImageMemory(context, image, memory);
	image->memory = *memory;
	image->usage  = image_info.usage;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
