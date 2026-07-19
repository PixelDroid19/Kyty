#include "Emulator/Graphics/Objects/VideoOutBuffer.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <functional>
#include <vector>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

const char* VideoOutHostExtentSetSelectionStatusName(VideoOutHostExtentSetSelectionStatus status)
{
	switch (status)
	{
		case VideoOutHostExtentSetSelectionStatus::Selected: return "selected";
		case VideoOutHostExtentSetSelectionStatus::StickyMatch: return "sticky_match";
		case VideoOutHostExtentSetSelectionStatus::StickyMismatch: return "sticky_mismatch";
		case VideoOutHostExtentSetSelectionStatus::InvalidArgument: return "invalid_argument";
		case VideoOutHostExtentSetSelectionStatus::Empty: return "empty";
	}
	return "unknown";
}

namespace {

class VideoOutImageLockSet
{
public:
	VideoOutImageLockSet(VideoOutVulkanImage* const* images, uint32_t image_count)
	{
		if (images == nullptr || image_count == 0)
		{
			return;
		}

		m_images.assign(images, images + image_count);
		if (std::any_of(m_images.cbegin(), m_images.cend(), [](const auto* image) { return image == nullptr; }))
		{
			m_images.clear();
			return;
		}

		std::sort(m_images.begin(), m_images.end(), std::less<VideoOutVulkanImage*> {});
		if (std::adjacent_find(m_images.cbegin(), m_images.cend()) != m_images.cend())
		{
			m_images.clear();
			return;
		}

		for (auto* image: m_images)
		{
			image->materialize_mutex.Lock();
		}
		m_locked = true;
	}

	~VideoOutImageLockSet()
	{
		if (m_locked)
		{
			for (auto image = m_images.rbegin(); image != m_images.rend(); ++image)
			{
				(*image)->materialize_mutex.Unlock();
			}
		}
	}

	KYTY_CLASS_NO_COPY(VideoOutImageLockSet);

	[[nodiscard]] bool IsLocked() const { return m_locked; }

private:
	std::vector<VideoOutVulkanImage*> m_images;
	bool                              m_locked = false;
};

VideoOutHostExtentState GetHostExtentStateLocked(const VideoOutVulkanImage* image)
{
	return {image->extent.width, image->extent.height, image->host_extent_selected, image->image != nullptr};
}

VideoOutHostExtentStatus SelectHostExtentLocked(VideoOutVulkanImage* image, uint32_t width, uint32_t height, VideoOutHostExtentState* state)
{
	if (image->image != nullptr)
	{
		image->host_extent_selected = true;
		*state                      = GetHostExtentStateLocked(image);
		return image->extent.width == width && image->extent.height == height ? VideoOutHostExtentStatus::StickyMatch
		                                                                      : VideoOutHostExtentStatus::StickyMismatch;
	}
	if (!image->host_extent_selected)
	{
		image->SetHostExtent(width, height);
		image->host_extent_selected = true;
		*state                      = GetHostExtentStateLocked(image);
		return VideoOutHostExtentStatus::Selected;
	}

	*state = GetHostExtentStateLocked(image);
	return image->extent.width == width && image->extent.height == height ? VideoOutHostExtentStatus::StickyMatch
	                                                                      : VideoOutHostExtentStatus::StickyMismatch;
}

} // namespace

static bool buffer_is_tiled(uint64_t vaddr, uint64_t size)
{
	if ((size & 0x7u) == 0)
	{
		const auto* ptr     = reinterpret_cast<const uint64_t*>(vaddr);
		const auto* ptr_end = reinterpret_cast<const uint64_t*>(vaddr + size / 8);
		for (uint64_t element = *ptr; ptr < ptr_end; ptr++)
		{
			if (element != *ptr)
			{
				return true;
			}
		}
		return false;
	}
	return true;
}

static void upload_guest_contents(GraphicContext* ctx, VideoOutVulkanImage* vk_obj)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(vk_obj->image == nullptr);

	if (!VideoOutBufferShouldCpuUploadOnUpdate(vk_obj->tiled))
	{
		return;
	}

	vk_obj->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (vk_obj->tiled && buffer_is_tiled(vk_obj->guest_vaddr, vk_obj->guest_size))
	{
		EXIT_NOT_IMPLEMENTED(vk_obj->guest_extent.width != vk_obj->guest_pitch);
		auto* temp_buf = new uint8_t[vk_obj->guest_size];
		TileConvertTiledToLinear(temp_buf, reinterpret_cast<void*>(vk_obj->guest_vaddr), TileMode::VideoOutTiled,
		                         vk_obj->guest_extent.width, vk_obj->guest_extent.height, vk_obj->neo);
		UtilFillImage(ctx, vk_obj, temp_buf, vk_obj->guest_size, vk_obj->guest_pitch,
		              static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
		delete[] temp_buf;
	} else
	{
		UtilFillImage(ctx, vk_obj, reinterpret_cast<void*>(vk_obj->guest_vaddr), vk_obj->guest_size, vk_obj->guest_pitch,
		              static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
	}
}

bool VideoOutBufferNeedsMaterialization(const VideoOutVulkanImage* image)
{
	return image != nullptr && image->image == nullptr;
}

bool VideoOutBufferGetHostExtentState(VideoOutVulkanImage* image, VideoOutHostExtentState* state)
{
	if (image == nullptr || state == nullptr)
	{
		return false;
	}
	Core::LockGuard lock(image->materialize_mutex);
	*state = GetHostExtentStateLocked(image);
	return true;
}

VideoOutHostExtentStatus VideoOutBufferSelectHostExtent(VideoOutVulkanImage* image, uint32_t width, uint32_t height,
                                                        VideoOutHostExtentState* state)
{
	if (image == nullptr || state == nullptr || width == 0 || height == 0)
	{
		return VideoOutHostExtentStatus::InvalidArgument;
	}

	Core::LockGuard lock(image->materialize_mutex);
	return SelectHostExtentLocked(image, width, height, state);
}

VideoOutHostExtentSetSelectionStatus VideoOutBufferSelectHostExtentSet(VideoOutVulkanImage* const* images, uint32_t image_count,
                                                                       uint32_t width, uint32_t height, VideoOutHostExtentSetState* state)
{
	if (images == nullptr || state == nullptr || width == 0 || height == 0)
	{
		return VideoOutHostExtentSetSelectionStatus::InvalidArgument;
	}
	if (image_count == 0)
	{
		return VideoOutHostExtentSetSelectionStatus::Empty;
	}

	VideoOutImageLockSet lock(images, image_count);
	if (!lock.IsLocked())
	{
		return VideoOutHostExtentSetSelectionStatus::InvalidArgument;
	}

	bool needs_selection = false;
	for (uint32_t index = 0; index < image_count; index++)
	{
		const auto image_state = GetHostExtentStateLocked(images[index]);
		if ((image_state.selected || image_state.materialized) && (image_state.width != width || image_state.height != height))
		{
			*state = {image_state.width, image_state.height, image_count};
			return VideoOutHostExtentSetSelectionStatus::StickyMismatch;
		}
		needs_selection = needs_selection || !image_state.selected;
	}

	for (uint32_t index = 0; index < image_count; index++)
	{
		VideoOutHostExtentState image_state;
		const auto              status = SelectHostExtentLocked(images[index], width, height, &image_state);
		if (status == VideoOutHostExtentStatus::StickyMismatch)
		{
			*state = {image_state.width, image_state.height, image_count};
			return VideoOutHostExtentSetSelectionStatus::StickyMismatch;
		}
	}

	*state = {width, height, image_count};
	return needs_selection ? VideoOutHostExtentSetSelectionStatus::Selected : VideoOutHostExtentSetSelectionStatus::StickyMatch;
}

VideoOutHostExtentSetInspectionStatus VideoOutBufferInspectHostExtentSet(VideoOutVulkanImage* const* images, uint32_t image_count,
                                                                         VideoOutHostExtentSetState* state)
{
	if (images == nullptr || state == nullptr)
	{
		return VideoOutHostExtentSetInspectionStatus::InvalidArgument;
	}
	if (image_count == 0)
	{
		return VideoOutHostExtentSetInspectionStatus::Empty;
	}

	VideoOutImageLockSet lock(images, image_count);
	if (!lock.IsLocked())
	{
		return VideoOutHostExtentSetInspectionStatus::InvalidArgument;
	}

	const auto first = GetHostExtentStateLocked(images[0]);
	if (!first.selected)
	{
		return VideoOutHostExtentSetInspectionStatus::Unselected;
	}

	for (uint32_t index = 1; index < image_count; index++)
	{
		const auto current = GetHostExtentStateLocked(images[index]);
		if (!current.selected)
		{
			return VideoOutHostExtentSetInspectionStatus::Unselected;
		}
		if (current.width != first.width || current.height != first.height)
		{
			*state = {first.width, first.height, image_count};
			return VideoOutHostExtentSetInspectionStatus::NonUniform;
		}
	}

	*state = {first.width, first.height, image_count};
	return VideoOutHostExtentSetInspectionStatus::Uniform;
}

void VideoOutBufferEnsureMaterialized(GraphicContext* ctx, VideoOutVulkanImage* vk_obj)
{
	KYTY_PROFILER_BLOCK("VideoOutBufferObject::Materialize");
	EXIT_IF(ctx == nullptr);
	EXIT_IF(vk_obj == nullptr);

	Core::LockGuard lock(vk_obj->materialize_mutex);
	if (vk_obj->image != nullptr)
	{
		return;
	}
	vk_obj->host_extent_selected = true;

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent.width  = vk_obj->extent.width;
	image_info.extent.height = vk_obj->extent.height;
	image_info.extent.depth  = 1;
	image_info.mipLevels     = 1;
	image_info.arrayLayers   = 1;
	image_info.format        = vk_obj->format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = vk_obj->layout;
	image_info.usage         = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;

	vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);
	EXIT_NOT_IMPLEMENTED(vk_obj->image == nullptr);

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &vk_obj->memory.requirements);
	vk_obj->memory.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	EXIT_NOT_IMPLEMENTED(!VulkanAllocate(ctx, &vk_obj->memory));
	VulkanBindImageMemory(ctx, vk_obj, &vk_obj->memory);

	printf("VideoOutBufferObject::Materialize()\n");
	printf("\t memory size = %" PRIu64 "\n", vk_obj->memory.requirements.size);
	printf("\t width       = %" PRIu32 "\n", vk_obj->extent.width);
	printf("\t height      = %" PRIu32 "\n", vk_obj->extent.height);
	printf("\t guest size  = %" PRIu64 "\n", vk_obj->guest_size);

	upload_guest_contents(ctx, vk_obj);

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = nullptr;
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = vk_obj->format;
	create_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = 0;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = 1;

	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]);

	create_info.components.r = VK_COMPONENT_SWIZZLE_B;
	create_info.components.g = VK_COMPONENT_SWIZZLE_G;
	create_info.components.b = VK_COMPONENT_SWIZZLE_R;
	create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_BGRA]);

	create_info.components.r = VK_COMPONENT_SWIZZLE_A;
	create_info.components.g = VK_COMPONENT_SWIZZLE_B;
	create_info.components.b = VK_COMPONENT_SWIZZLE_G;
	create_info.components.a = VK_COMPONENT_SWIZZLE_R;
	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_ABGR]);

	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);
	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_BGRA] == nullptr);
	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_ABGR] == nullptr);
}

static void update_func(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	KYTY_PROFILER_BLOCK("VideoOutBufferObject::update_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto*           vk_obj = static_cast<VideoOutVulkanImage*>(obj);
	Core::LockGuard lock(vk_obj->materialize_mutex);
	vk_obj->guest_vaddr = *vaddr;
	vk_obj->guest_size  = *size;
	if (vk_obj->image != nullptr)
	{
		upload_guest_contents(ctx, vk_obj);
	}
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("VideoOutBufferObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto pixel_format = params[VideoOutBufferObject::PARAM_FORMAT];
	auto width        = params[VideoOutBufferObject::PARAM_WIDTH];
	auto height       = params[VideoOutBufferObject::PARAM_HEIGHT];

	// EXIT_NOT_IMPLEMENTED(pixel_format != 0x80000000);
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);

	auto* vk_obj = new VideoOutVulkanImage;

	VkFormat vk_format = VK_FORMAT_UNDEFINED;

	switch (pixel_format)
	{
		case static_cast<uint64_t>(VideoOutBufferFormat::R8G8B8A8Srgb): vk_format = VK_FORMAT_R8G8B8A8_SRGB; break;
		case static_cast<uint64_t>(VideoOutBufferFormat::B8G8R8A8Srgb): vk_format = VK_FORMAT_B8G8R8A8_SRGB; break;
		case static_cast<uint64_t>(VideoOutBufferFormat::R10G10B10A2Unorm): vk_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
		case static_cast<uint64_t>(VideoOutBufferFormat::B10G10R10A2Unorm): vk_format = VK_FORMAT_A2R10G10B10_UNORM_PACK32; break;
		default: EXIT("unknown format: %" PRIu64 "\n", pixel_format);
	}

	vk_obj->SetNativeExtent(width, height);
	vk_obj->format      = vk_format;
	vk_obj->layout      = VK_IMAGE_LAYOUT_UNDEFINED;
	vk_obj->guest_vaddr = *vaddr;
	vk_obj->guest_size  = *size;
	vk_obj->guest_pitch = params[VideoOutBufferObject::PARAM_PITCH];
	vk_obj->tiled       = params[VideoOutBufferObject::PARAM_TILED] != 0;
	vk_obj->neo         = params[VideoOutBufferObject::PARAM_NEO] != 0;

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* /*mem*/)
{
	KYTY_PROFILER_BLOCK("VideoOutBufferObject::delete_func");

	auto* vk_obj = reinterpret_cast<VideoOutVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);
	{
		Core::LockGuard lock(vk_obj->materialize_mutex);
		if (vk_obj->image != nullptr)
		{
			DeleteFramebuffer(vk_obj);

			for (auto view: vk_obj->image_view)
			{
				if (view != nullptr)
				{
					vkDestroyImageView(ctx->device, view, nullptr);
				}
			}

			vkDestroyImage(ctx->device, vk_obj->image, nullptr);
			VulkanFree(ctx, &vk_obj->memory);
		}
	}

	delete vk_obj;
}

bool VideoOutBufferObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_WIDTH] == other[PARAM_WIDTH] &&
	        params[PARAM_HEIGHT] == other[PARAM_HEIGHT] && params[PARAM_TILED] == other[PARAM_TILED] &&
	        params[PARAM_PITCH] == other[PARAM_PITCH] && params[PARAM_NEO] == other[PARAM_NEO]);
}

GpuObject::create_func_t VideoOutBufferObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::delete_func_t VideoOutBufferObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t VideoOutBufferObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
