#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstring>
#include <utility>
#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty {
template <typename T>
class Vector;
} // namespace Kyty

namespace Kyty::Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct VulkanBuffer;
struct VulkanImage;
struct DepthStencilVulkanImage;
struct VulkanSwapchain;

VkImageLayout UtilGetImageUploadSourceLayout(const VulkanImage* image);

[[nodiscard]] inline bool DepthFormatHasStencil(VkFormat format)
{
	return format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

[[nodiscard]] inline VkImageAspectFlags DepthFormatAspectMask(VkFormat format)
{
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (DepthFormatHasStencil(format))
	{
		aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	return aspect;
}

// Combined D/S images cannot LOAD any aspect from UNDEFINED. Use UNDEFINED only when
// every used aspect CLEARs or is DONT_CARE; otherwise keep OPTIMAL and CLEAR depth only.
struct DepthAttachmentLoadOps
{
	VkAttachmentLoadOp depth_load     = VK_ATTACHMENT_LOAD_OP_LOAD;
	VkAttachmentLoadOp stencil_load   = VK_ATTACHMENT_LOAD_OP_LOAD;
	VkImageLayout      initial_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
};

// Guest HTILE/register clears map to attachment loadOp CLEAR. No invented color CLEAR0.
// When not clearing, LOAD OPTIMAL preserves prior DS contents. Depth-only CLEAR uses
// UNDEFINED init, so stencil cannot LOAD in that pass (DONT_CARE unless stencil clears).
[[nodiscard]] inline DepthAttachmentLoadOps ResolveDepthAttachmentLoadOps(VkFormat format, bool depth_clear, bool stencil_clear)
{
	DepthAttachmentLoadOps ops {};
	const bool             has_stencil = DepthFormatHasStencil(format);
	ops.depth_load                     = depth_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	if (!has_stencil)
	{
		ops.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	} else if (stencil_clear)
	{
		ops.stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
	} else if (depth_clear)
	{
		ops.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	} else
	{
		ops.stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
	}
	ops.initial_layout = (depth_clear || stencil_clear) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	return ops;
}

// Guest CB clear words map to attachment loadOp CLEAR on first GPU-owned use
// (tracked layout UNDEFINED). Subsequent passes LOAD OPTIMAL preserve contents.
struct ColorAttachmentLoadOps
{
	VkAttachmentLoadOp load_op        = VK_ATTACHMENT_LOAD_OP_LOAD;
	VkImageLayout      initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	float              clear_r        = 0.0f;
	float              clear_g        = 0.0f;
	float              clear_b        = 0.0f;
	float              clear_a        = 1.0f;
};

[[nodiscard]] inline VkClearColorValue DecodeGuestColorClearWords(uint32_t word0, uint32_t word1)
{
	VkClearColorValue value {};
	std::memcpy(&value.float32[0], &word0, sizeof(uint32_t));
	std::memcpy(&value.float32[1], &word1, sizeof(uint32_t));
	value.float32[2] = 0.0f;
	value.float32[3] = 1.0f;
	return value;
}

[[nodiscard]] inline ColorAttachmentLoadOps ResolveColorAttachmentLoadOps(VkImageLayout tracked_layout, bool guest_fast_clear,
                                                                          uint32_t clear_word0, uint32_t clear_word1)
{
	ColorAttachmentLoadOps ops {};
	if (tracked_layout == VK_IMAGE_LAYOUT_UNDEFINED)
	{
		ops.load_op        = VK_ATTACHMENT_LOAD_OP_CLEAR;
		ops.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (guest_fast_clear || clear_word0 != 0u || clear_word1 != 0u)
		{
			const auto clear = DecodeGuestColorClearWords(clear_word0, clear_word1);
			ops.clear_r      = clear.float32[0];
			ops.clear_g      = clear.float32[1];
			ops.clear_b      = clear.float32[2];
			ops.clear_a      = clear.float32[3];
		}
	} else
	{
		ops.load_op        = VK_ATTACHMENT_LOAD_OP_LOAD;
		ops.initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	return ops;
}

struct BufferImageCopy
{
	uint32_t offset;
	uint32_t pitch;
	uint32_t dst_level;
	uint32_t width;
	uint32_t height;
	int      dst_x;
	int      dst_y;
};

struct ImageImageCopy
{
	VulkanImage* src_image;
	uint32_t     src_level;
	uint32_t     dst_level;
	uint32_t     width;
	uint32_t     height;
	int          src_x;
	int          src_y;
	int          dst_x;
	int          dst_y;
};

void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, uint32_t src_pitch, VulkanImage* dst_image, uint64_t dst_layout);
void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, VulkanImage* dst_image, const Vector<BufferImageCopy>& regions,
                       uint64_t dst_layout);
void UtilImageToBuffer(CommandBuffer* buffer, VulkanImage* src_image, VulkanBuffer* dst_buffer, uint32_t dst_pitch, uint64_t src_layout);
void UtilImageToImage(CommandBuffer* buffer, const Vector<ImageImageCopy>& regions, VulkanImage* dst_image, uint64_t dst_layout);
void UtilBlitImage(CommandBuffer* buffer, VulkanImage* src_image, VulkanSwapchain* dst_swapchain);
void UtilFillImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size, uint32_t src_pitch,
                   uint64_t dst_layout);
void UtilFillImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size, const Vector<BufferImageCopy>& regions,
                   uint64_t dst_layout);
void UtilFillImage(GraphicContext* ctx, const Vector<ImageImageCopy>& regions, VulkanImage* dst_image, uint64_t dst_layout);
void UtilFillBuffer(GraphicContext* ctx, void* dst_data, uint64_t size, uint32_t dst_pitch, VulkanImage* src_image, uint64_t src_layout);
void UtilCopyBuffer(VulkanBuffer* src_buffer, VulkanBuffer* dst_buffer, uint64_t size);
void UtilSetDepthLayoutOptimal(DepthStencilVulkanImage* image);
void UtilSetImageLayoutOptimal(VulkanImage* image);

void VulkanCreateBuffer(GraphicContext* gctx, uint64_t size, VulkanBuffer* buffer);
void VulkanDeleteBuffer(GraphicContext* gctx, VulkanBuffer* buffer);

inline std::pair<int, int> UtilCalcMipmapOffset(uint32_t lod, uint32_t width, uint32_t height)
{
	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	int      mip_x      = 0;
	int      mip_y      = 0;

	for (uint32_t i = 0; i < 16; i++)
	{
		if (i == lod)
		{
			return {mip_x, mip_y};
		}

		bool odd = ((i & 1u) != 0);
		mip_x += static_cast<int>(odd ? mip_width : 0);
		mip_y += static_cast<int>(odd ? 0 : mip_height);

		mip_width >>= (mip_width > 1 ? 1u : 0u);
		mip_height >>= (mip_height > 1 ? 1u : 0u);
	}

	return {mip_x, mip_y};
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_ */
