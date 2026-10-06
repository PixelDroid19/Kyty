#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"

#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"

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
	*view          = nullptr;
	auto view_info = VulkanBuildImageViewCreateInfo(descriptor);
	VkImageViewUsageCreateInfo usage_info {};
	if (descriptor.usage != 0u)
	{
		usage_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
		usage_info.usage = descriptor.usage;
		view_info.pNext  = &usage_info;
	}
	return vkCreateImageView(device, &view_info, nullptr, view) == VK_SUCCESS && *view != nullptr;
}

bool VulkanPlanSampledImageView(const VulkanImage& image, VkImageViewType view_type, VkImageAspectFlags aspect,
                                uint32_t base_mip, uint32_t mip_count, uint32_t base_layer, uint32_t layer_count,
                                uint32_t selectors, VulkanImageViewDescriptor* descriptor)
{
	if (descriptor == nullptr)
	{
		return false;
	}
	*descriptor = {};
	VkComponentMapping components {};
	if (image.image == VK_NULL_HANDLE || image.format == VK_FORMAT_UNDEFINED ||
	    (image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0u ||
	    !VulkanDecodeComponentMapping(selectors, &components) ||
	    base_mip >= image.mip_levels || mip_count == 0u || mip_count > image.mip_levels - base_mip ||
	    base_layer >= image.array_layers || layer_count == 0u || layer_count > image.array_layers - base_layer)
	{
		return false;
	}
	const bool volume = view_type == VK_IMAGE_VIEW_TYPE_3D;
	if ((view_type != VK_IMAGE_VIEW_TYPE_2D && view_type != VK_IMAGE_VIEW_TYPE_2D_ARRAY && !volume) ||
	    (volume ? image.image_type != VK_IMAGE_TYPE_3D || base_layer != 0u || layer_count != 1u
	            : image.image_type != VK_IMAGE_TYPE_2D) ||
	    (view_type == VK_IMAGE_VIEW_TYPE_2D && layer_count != 1u))
	{
		return false;
	}
	const bool depth_format = image.format == VK_FORMAT_D16_UNORM || image.format == VK_FORMAT_D32_SFLOAT ||
	                          image.format == VK_FORMAT_D16_UNORM_S8_UINT || image.format == VK_FORMAT_D24_UNORM_S8_UINT ||
	                          image.format == VK_FORMAT_D32_SFLOAT_S8_UINT || image.format == VK_FORMAT_X8_D24_UNORM_PACK32;
	const bool stencil_format = image.format == VK_FORMAT_S8_UINT || image.format == VK_FORMAT_D16_UNORM_S8_UINT ||
	                            image.format == VK_FORMAT_D24_UNORM_S8_UINT || image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
	if ((aspect == VK_IMAGE_ASPECT_COLOR_BIT && (depth_format || stencil_format)) ||
	    (aspect == VK_IMAGE_ASPECT_DEPTH_BIT && !depth_format) ||
	    (aspect == VK_IMAGE_ASPECT_STENCIL_BIT && !stencil_format) ||
	    (aspect != VK_IMAGE_ASPECT_COLOR_BIT && aspect != VK_IMAGE_ASPECT_DEPTH_BIT && aspect != VK_IMAGE_ASPECT_STENCIL_BIT))
	{
		return false;
	}
	*descriptor = {image.image, view_type, image.format, components, aspect, base_mip, mip_count, base_layer, layer_count};
	return true;
}

bool VulkanImageViewDescriptorsEqual(const VulkanImageViewDescriptor& a, const VulkanImageViewDescriptor& b)
{
	return a.image == b.image && a.view_type == b.view_type && a.format == b.format && a.usage == b.usage && a.aspect_mask == b.aspect_mask &&
	       a.components.r == b.components.r && a.components.g == b.components.g && a.components.b == b.components.b &&
	       a.components.a == b.components.a && a.base_mip_level == b.base_mip_level && a.level_count == b.level_count &&
	       a.base_array_layer == b.base_array_layer && a.layer_count == b.layer_count;
}

int VulkanGetOrCreateSampledImageView(VkDevice device, VulkanImage* image, const VulkanImageViewDescriptor& descriptor,
                                     VulkanImageViewCreator create)
{
	if (image == nullptr || create == nullptr || descriptor.image != image->image || (image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0u)
	{
		return -1;
	}
	// Another format reads the same texels only on a mutable image, through a
	// view limited to sampling.
	if (descriptor.format != image->format &&
	    (!image->mutable_format || descriptor.usage != VK_IMAGE_USAGE_SAMPLED_BIT ||
	     !VulkanColorFormatsShareTexels(image->format, descriptor.format)))
	{
		return -1;
	}
	for (size_t i = 0; i < image->sampled_view_descriptors.size(); ++i)
	{
		if (VulkanImageViewDescriptorsEqual(image->sampled_view_descriptors[i], descriptor))
		{
			return VulkanImage::VIEW_MAX + static_cast<int>(i);
		}
	}
	if (image->image_view.size() >= VulkanImage::VIEW_CACHE_LIMIT)
	{
		return -1;
	}
	VkImageView view = VK_NULL_HANDLE;
	if (!create(device, descriptor, &view) || view == VK_NULL_HANDLE)
	{
		return -1;
	}
	const int index = static_cast<int>(image->image_view.size());
	image->image_view.push_back(view);
	image->sampled_view_descriptors.push_back(descriptor);
	return index;
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
	descriptor.layer_count = image->array_layers;
	if (!create(VulkanImage::VIEW_ARRAY))
	{
		goto fail;
	}
	descriptor.view_type  = VK_IMAGE_VIEW_TYPE_2D;
	descriptor.layer_count = 1;
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

int VulkanResolveColorAttachmentView(VkFormat image_format, VkFormat attachment_format)
{
	if (image_format == attachment_format)
	{
		return VulkanImage::VIEW_DEFAULT;
	}
	if ((image_format == VK_FORMAT_R8G8B8A8_SRGB && attachment_format == VK_FORMAT_R8G8B8A8_UNORM) ||
	    (image_format == VK_FORMAT_B8G8R8A8_SRGB && attachment_format == VK_FORMAT_B8G8R8A8_UNORM))
	{
		return VulkanImage::VIEW_COLOR_UNORM;
	}
	if ((image_format == VK_FORMAT_R8G8B8A8_UNORM && attachment_format == VK_FORMAT_R8G8B8A8_SRGB) ||
	    (image_format == VK_FORMAT_B8G8R8A8_UNORM && attachment_format == VK_FORMAT_B8G8R8A8_SRGB))
	{
		return VulkanImage::VIEW_COLOR_SRGB;
	}
	return -1;
}

bool VulkanCreateCompatibleColorAttachmentViews(GraphicContext* context, VulkanImage* image)
{
	EXIT_IF(context == nullptr || image == nullptr || image->image == nullptr);

	VkFormat alternate = VK_FORMAT_UNDEFINED;
	int      index     = -1;
	switch (image->format)
	{
		case VK_FORMAT_R8G8B8A8_SRGB:
			alternate = VK_FORMAT_R8G8B8A8_UNORM;
			index     = VulkanImage::VIEW_COLOR_UNORM;
			break;
		case VK_FORMAT_B8G8R8A8_SRGB:
			alternate = VK_FORMAT_B8G8R8A8_UNORM;
			index     = VulkanImage::VIEW_COLOR_UNORM;
			break;
		case VK_FORMAT_R8G8B8A8_UNORM:
			alternate = VK_FORMAT_R8G8B8A8_SRGB;
			index     = VulkanImage::VIEW_COLOR_SRGB;
			break;
		case VK_FORMAT_B8G8R8A8_UNORM:
			alternate = VK_FORMAT_B8G8R8A8_SRGB;
			index     = VulkanImage::VIEW_COLOR_SRGB;
			break;
		default: return true;
	}

	VulkanImageViewDescriptor descriptor {};
	descriptor.image  = image->image;
	descriptor.format = alternate;
	return VulkanCreateDeviceImageView(context->device, descriptor, &image->image_view[index]);
}

bool VulkanResolveStorageImageView(const VulkanImage* image, bool three_dimensional, bool arrayed_2d, int* view_index,
                                   uint32_t base_mip_level)
{
	if (image == nullptr || view_index == nullptr || (image->usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0u)
	{
		return false;
	}
	if (three_dimensional)
	{
		*view_index = VulkanImage::VIEW_3D;
	} else if (arrayed_2d)
	{
		*view_index = image->type == VulkanImageType::RenderTexture ? VulkanImage::VIEW_ARRAY : VulkanImage::VIEW_STORAGE_ARRAY;
	} else
	{
		if (image->type == VulkanImageType::StorageTexture && image->mip_levels > 1u)
		{
			if (base_mip_level >= image->mip_levels || base_mip_level >= VulkanImage::VIEW_STORAGE_MIP_COUNT)
			{
				return false;
			}
			*view_index = VulkanImage::VIEW_STORAGE_MIP_BASE + static_cast<int>(base_mip_level);
		} else
		{
			*view_index = VulkanImage::VIEW_DEFAULT;
		}
	}
	return image->image_view[*view_index] != nullptr;
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
	// A BGRA selection of an RGBA8 image is the same bytes as a BGRA8 image read with identity.
	const bool bgra = mapping->r == VK_COMPONENT_SWIZZLE_B && mapping->g == VK_COMPONENT_SWIZZLE_G &&
	                  mapping->b == VK_COMPONENT_SWIZZLE_R && mapping->a == VK_COMPONENT_SWIZZLE_A;
	if (!bgra || (*format != VK_FORMAT_R8G8B8A8_SRGB && *format != VK_FORMAT_R8G8B8A8_UNORM))
	{
		return false;
	}
	*format  = *format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
	*mapping = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
	return true;
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
	if (!VulkanAllocate(context, memory,
	                    image_info.tiling == VK_IMAGE_TILING_OPTIMAL ? VulkanMemoryResource::Optimal : VulkanMemoryResource::Linear))
	{
		vkDestroyImage(context->device, image->image, nullptr);
		image->image = nullptr;
		return false;
	}
	VulkanBindImageMemory(context, image, memory);
	image->memory          = *memory;
	image->usage           = image_info.usage;
	image->physical_extent = image_info.extent;
	image->mip_levels      = image_info.mipLevels;
	image->array_layers    = image_info.arrayLayers;
	image->image_type      = image_info.imageType;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
