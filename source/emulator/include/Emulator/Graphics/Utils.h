#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
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

// Host presentation only. Guest intermediate float RTs (COLOR_16_16_16_16 + FLOAT /
// ufmt 71) are game-driven contracts and are unrelated to this host color-space policy.
// Default swapchain selection must stay ordinary LDR sRGB — not HDR10 / HLG / Dolby /
// BT.2020, which few displays use and which can produce wrong presentation.
[[nodiscard]] inline bool VulkanColorSpaceIsHostHdr(VkColorSpaceKHR color_space)
{
	switch (static_cast<int>(color_space))
	{
		case static_cast<int>(VK_COLOR_SPACE_HDR10_ST2084_EXT):
		case static_cast<int>(VK_COLOR_SPACE_HDR10_HLG_EXT):
		case static_cast<int>(VK_COLOR_SPACE_DOLBYVISION_EXT):
		case static_cast<int>(VK_COLOR_SPACE_BT2020_LINEAR_EXT): return true;
		default: return false;
	}
}

// Prefer B8G8R8A8 UNORM/SRGB + SRGB_NONLINEAR. Never pick a host-HDR color space when
// any LDR SRGB_NONLINEAR candidate exists. Last resort: first non-HDR entry, else [0].
[[nodiscard]] inline VkSurfaceFormatKHR SelectDefaultSwapchainSurfaceFormat(const VkSurfaceFormatKHR* formats, uint32_t count)
{
	VkSurfaceFormatKHR fallback {};
	fallback.format     = VK_FORMAT_B8G8R8A8_UNORM;
	fallback.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	if (formats == nullptr || count == 0u)
	{
		return fallback;
	}

	const VkSurfaceFormatKHR* unorm_srgb = nullptr;
	const VkSurfaceFormatKHR* srgb_srgb  = nullptr;
	const VkSurfaceFormatKHR* any_srgb   = nullptr;
	const VkSurfaceFormatKHR* any_ldr    = nullptr;

	for (uint32_t i = 0; i < count; i++)
	{
		const auto& f = formats[i];
		if (VulkanColorSpaceIsHostHdr(f.colorSpace))
		{
			continue;
		}
		if (any_ldr == nullptr)
		{
			any_ldr = &f;
		}
		if (f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			if (any_srgb == nullptr)
			{
				any_srgb = &f;
			}
			if (f.format == VK_FORMAT_B8G8R8A8_UNORM && unorm_srgb == nullptr)
			{
				unorm_srgb = &f;
			}
			if (f.format == VK_FORMAT_B8G8R8A8_SRGB && srgb_srgb == nullptr)
			{
				srgb_srgb = &f;
			}
		}
	}

	if (unorm_srgb != nullptr)
	{
		return *unorm_srgb;
	}
	if (srgb_srgb != nullptr)
	{
		return *srgb_srgb;
	}
	if (any_srgb != nullptr)
	{
		return *any_srgb;
	}
	if (any_ldr != nullptr)
	{
		return *any_ldr;
	}
	// Only HDR (or empty-after-filter) surfaces: still avoid inventing a format the
	// driver did not list; caller must cope. Prefer not silently "enabling HDR".
	return formats[0];
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

// IEEE754 binary16 → binary32. Used only to unpack CB CLEAR_WORD bit patterns.
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
			// Renormalize subnormal.
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
	float value = 0.0f;
	std::memcpy(&value, &out, sizeof(value));
	return value;
}

// AMD CB_COLOR*_CLEAR_WORD0/1 hold the raw clear pixel (two dwords).
// Mesa/RADV packing (evidenced):
//   R16G16B16A16_SFLOAT: WORD0 = f16(R)|(f16(G)<<16), WORD1 = f16(B)|(f16(A)<<16)
//   R8G8B8A8_*:          WORD0 = R|(G<<8)|(B<<16)|(A<<24), WORD1 = 0
// Do not invent B=0/A=1 by bitcasting WORD0/1 as float32 R/G.
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
	// Unsupported clear packing for this attachment format: keep structured fail
	// path callers from inventing channels. Black+opaque matches default load ops.
	value.float32[0] = 0.0f;
	value.float32[1] = 0.0f;
	value.float32[2] = 0.0f;
	value.float32[3] = 1.0f;
	return value;
}

[[nodiscard]] inline ColorAttachmentLoadOps ResolveColorAttachmentLoadOps(VkImageLayout tracked_layout, bool guest_fast_clear,
                                                                          uint32_t clear_word0, uint32_t clear_word1, VkFormat format)
{
	ColorAttachmentLoadOps ops {};
	// First bind (UNDEFINED) and rebind after sampling (SHADER_READ_ONLY) must CLEAR.
	// Captured Dead Cells lighting: RGBA16F + SRC_ALPHA,ONE with no guest fast-clear;
	// LOAD after sample keeps prior-frame light and accumulates into hot yellow/red slabs.
	// Within-frame light draws stay COLOR_ATTACHMENT_OPTIMAL and still LOAD.
	// BeginRenderPass barriers non-COLOR layouts to COLOR before vkCmdBeginRenderPass,
	// so SHADER_READ rebinds use COLOR as the render-pass initial layout.
	const bool clear_on_bind =
	    tracked_layout == VK_IMAGE_LAYOUT_UNDEFINED || tracked_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (clear_on_bind)
	{
		ops.load_op        = VK_ATTACHMENT_LOAD_OP_CLEAR;
		ops.initial_layout = (tracked_layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_IMAGE_LAYOUT_UNDEFINED
		                                                                  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		const bool decode_words = guest_fast_clear || clear_word0 != 0u || clear_word1 != 0u ||
		                          format == VK_FORMAT_R16G16B16A16_SFLOAT;
		if (decode_words)
		{
			// Always decode for float RTs: CLEAR_WORD=0 packs to A=0 (captured lighting
			// targets with blend SRC_ALPHA,ONE). Inventing opaque A=1 diverges from
			// Mesa/RADV f16 packing. Other formats keep legacy opaque-black when words
			// are zero and fast-clear is off until those paths are re-characterized.
			const auto clear = DecodeGuestColorClearWords(clear_word0, clear_word1, format);
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

// GPU→CPU buffer write-back with absolute holes [hole_begin[i], hole_end[i]).
// Bytes in holes keep the existing dst contents (EOP fences, guest resets).
inline void MemcpySkipAbsoluteRanges(void* dst, const void* src, uint64_t size, const uint64_t* hole_begin, const uint64_t* hole_end,
                                     int hole_count)
{
	if (dst == nullptr || src == nullptr || size == 0)
	{
		return;
	}
	if (hole_count <= 0 || hole_begin == nullptr || hole_end == nullptr)
	{
		std::memcpy(dst, src, static_cast<size_t>(size));
		return;
	}

	const uint64_t base = reinterpret_cast<uint64_t>(dst);
	const uint64_t end  = base + size;

	struct Range
	{
		uint64_t begin = 0;
		uint64_t end   = 0;
	};

	std::vector<Range> ranges;
	ranges.reserve(static_cast<size_t>(hole_count));
	for (int i = 0; i < hole_count; i++)
	{
		uint64_t b = hole_begin[i];
		uint64_t e = hole_end[i];
		if (e <= b)
		{
			continue;
		}
		if (e <= base || b >= end)
		{
			continue;
		}
		if (b < base)
		{
			b = base;
		}
		if (e > end)
		{
			e = end;
		}
		ranges.push_back({b, e});
	}

	if (ranges.empty())
	{
		std::memcpy(dst, src, static_cast<size_t>(size));
		return;
	}

	std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });

	std::vector<Range> merged;
	merged.reserve(ranges.size());
	merged.push_back(ranges[0]);
	for (size_t i = 1; i < ranges.size(); i++)
	{
		Range& last = merged.back();
		if (ranges[i].begin <= last.end)
		{
			if (ranges[i].end > last.end)
			{
				last.end = ranges[i].end;
			}
		} else
		{
			merged.push_back(ranges[i]);
		}
	}

	uint64_t cursor = base;
	auto*    dbytes = static_cast<uint8_t*>(dst);
	auto*    sbytes = static_cast<const uint8_t*>(src);
	for (const auto& hole: merged)
	{
		if (hole.begin > cursor)
		{
			const uint64_t off = cursor - base;
			const uint64_t len = hole.begin - cursor;
			std::memcpy(dbytes + off, sbytes + off, static_cast<size_t>(len));
		}
		cursor = hole.end > cursor ? hole.end : cursor;
	}
	if (cursor < end)
	{
		const uint64_t off = cursor - base;
		const uint64_t len = end - cursor;
		std::memcpy(dbytes + off, sbytes + off, static_cast<size_t>(len));
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_ */
