#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdlib>
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

// Byte offset of the 64-bit address field inside a WaitRegMem-family packet,
// or 0 when the header is not a recognized wait. Used by WaitRegMemPatchAddress.
[[nodiscard]] inline uint32_t GraphicsWaitRegMemAddressByteOffset(uint32_t header)
{
	if ((header >> 30u) != 3u)
	{
		return 0;
	}
	const uint32_t op = (header >> 8u) & 0xffu;
	const uint32_t r  = (header >> 2u) & 0x3fu;
	if (op == 0x3cu) // IT_WAIT_REG_MEM
	{
		return 8u;
	}
	// Custom waits: IT_NOP + R_WAIT_MEM_32 (0x0A) / R_WAIT_MEM_64 (0x16).
	if (op == 0x10u && (r == 0x0au || r == 0x16u))
	{
		return 4u;
	}
	return 0;
}

// True when header is custom IT_NOP/R_DMA_DATA (8 dwords, dst at +16).
[[nodiscard]] inline bool GraphicsIsCustomDmaDataPacket(uint32_t header)
{
	if ((header >> 30u) != 3u)
	{
		return false;
	}
	const uint32_t op  = (header >> 8u) & 0xffu;
	const uint32_t r   = (header >> 2u) & 0x3fu;
	const uint32_t len = ((header >> 16u) & 0x3fffu) + 2u;
	return op == 0x10u && r == 0x19u && len >= 8u;
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
	float              clear_a        = 0.0f;
};

[[nodiscard]] inline float Float16BitsToFloat32(uint16_t bits)
{
	const uint32_t sign = (static_cast<uint32_t>(bits & 0x8000u) << 16);
	const uint32_t exp  = (bits >> 10) & 0x1fu;
	const uint32_t mant = bits & 0x3ffu;
	uint32_t       out  = 0;
	if (exp == 0)
	{
		if (mant == 0)
		{
			out = sign;
		} else
		{
			uint32_t m = mant;
			uint32_t e = 127 - 15 + 1;
			while ((m & 0x400u) == 0u)
			{
				m <<= 1u;
				--e;
			}
			m &= 0x3ffu;
			out = sign | (e << 23) | (m << 13);
		}
	} else if (exp == 0x1fu)
	{
		out = sign | 0x7f800000u | (mant << 13);
	} else
	{
		out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
	}
	float result = 0.0f;
	std::memcpy(&result, &out, sizeof(result));
	return result;
}

[[nodiscard]] inline bool ColorClearWordsHaveKnownPacking(VkFormat format)
{
	return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
	       format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

[[nodiscard]] inline VkClearColorValue DecodeGuestColorClearWords(uint32_t word0, uint32_t word1, VkFormat format)
{
	VkClearColorValue value {};
	if (format == VK_FORMAT_R16G16B16A16_SFLOAT)
	{
		value.float32[0] = Float16BitsToFloat32(static_cast<uint16_t>(word0 & 0xffffu));
		value.float32[1] = Float16BitsToFloat32(static_cast<uint16_t>((word0 >> 16) & 0xffffu));
		value.float32[2] = Float16BitsToFloat32(static_cast<uint16_t>(word1 & 0xffffu));
		value.float32[3] = Float16BitsToFloat32(static_cast<uint16_t>((word1 >> 16) & 0xffffu));
		return value;
	}
	if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB)
	{
		value.float32[0] = static_cast<float>((word0 >> 0) & 0xffu) / 255.0f;
		value.float32[1] = static_cast<float>((word0 >> 8) & 0xffu) / 255.0f;
		value.float32[2] = static_cast<float>((word0 >> 16) & 0xffu) / 255.0f;
		value.float32[3] = static_cast<float>((word0 >> 24) & 0xffu) / 255.0f;
		return value;
	}
	if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB)
	{
		value.float32[0] = static_cast<float>((word0 >> 16) & 0xffu) / 255.0f;
		value.float32[1] = static_cast<float>((word0 >> 8) & 0xffu) / 255.0f;
		value.float32[2] = static_cast<float>((word0 >> 0) & 0xffu) / 255.0f;
		value.float32[3] = static_cast<float>((word0 >> 24) & 0xffu) / 255.0f;
		return value;
	}
	value.float32[0] = 0.0f;
	value.float32[1] = 0.0f;
	value.float32[2] = 0.0f;
	value.float32[3] = 0.0f;
	return value;
}

[[nodiscard]] inline ColorAttachmentLoadOps ResolveColorAttachmentLoadOps(VkImageLayout tracked_layout, bool guest_fast_clear,
                                                                          uint32_t clear_word0, uint32_t clear_word1, VkFormat format)
{
	ColorAttachmentLoadOps ops {};
	const bool             clear_on_bind =
	    tracked_layout == VK_IMAGE_LAYOUT_UNDEFINED || tracked_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (clear_on_bind)
	{
		ops.load_op        = VK_ATTACHMENT_LOAD_OP_CLEAR;
		ops.initial_layout = (tracked_layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_IMAGE_LAYOUT_UNDEFINED
		                                                                  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		const bool decode_words = guest_fast_clear || clear_word0 != 0u || clear_word1 != 0u || ColorClearWordsHaveKnownPacking(format);
		if (decode_words)
		{
			const auto clear = DecodeGuestColorClearWords(clear_word0, clear_word1, format);
			ops.clear_r      = clear.float32[0];
			ops.clear_g      = clear.float32[1];
			ops.clear_b      = clear.float32[2];
			ops.clear_a      = clear.float32[3];
		}
		// Diagnostic only: prove whether draws write over a non-black clear.
		// If the screen stays magenta, draws are not reaching the display buffer.
		// If content appears over magenta, the black was empty/alpha-0 guest output.
		if (std::getenv("KYTY_DEBUG_CLEAR_MAGENTA") != nullptr)
		{
			ops.clear_r = 1.0f;
			ops.clear_g = 0.0f;
			ops.clear_b = 1.0f;
			ops.clear_a = 1.0f;
		}
	} else
	{
		ops.load_op        = VK_ATTACHMENT_LOAD_OP_LOAD;
		ops.initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	return ops;
}

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
