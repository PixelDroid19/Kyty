#include "Emulator/Graphics/Tile.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/DebugStats.h"

#include <cstring>
#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

using TileOffsetFn = uint64_t (*)(uint32_t x, uint32_t y, uint32_t pitch_elems, uint32_t bytes_per_element);

constexpr uint64_t k_4kb_block_bytes  = 4096u;
constexpr uint64_t k_64kb_block_bytes = 65536u;

bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t* result)
{
	if (result == nullptr || (left != 0u && right > std::numeric_limits<uint64_t>::max() / left))
	{
		return false;
	}
	*result = left * right;
	return true;
}

bool DivideRoundUp(uint64_t value, uint64_t divisor, uint64_t* result)
{
	if (result == nullptr || divisor == 0u)
	{
		return false;
	}
	*result = value / divisor + (value % divisor == 0u ? 0u : 1u);
	return true;
}

uint32_t DivideRoundUp(uint32_t value, uint32_t divisor)
{
	return value / divisor + (value % divisor == 0u ? 0u : 1u);
}

bool CanRoundUpU32(uint32_t value, uint32_t alignment)
{
	return alignment != 0u && value <= std::numeric_limits<uint32_t>::max() - (alignment - 1u);
}

bool CalculateBlockGridBytes(uint32_t pitch_elems, uint32_t height, uint32_t block_width, uint32_t block_height,
	                         uint64_t block_bytes, uint64_t* bytes)
{
	uint64_t blocks_x = 0;
	uint64_t blocks_y = 0;
	uint64_t blocks   = 0;
	return DivideRoundUp(pitch_elems, block_width, &blocks_x) && DivideRoundUp(height, block_height, &blocks_y) &&
	       CheckedMultiply(blocks_x, blocks_y, &blocks) && CheckedMultiply(blocks, block_bytes, bytes);
}

uint64_t OffsetSw64kRx(uint32_t x, uint32_t y, uint32_t pitch_elems, uint32_t bytes_per_element)
{
	return TileGetSw64kRxOffset(x, y, pitch_elems, bytes_per_element);
}

uint64_t OffsetStandard64KB(uint32_t x, uint32_t y, uint32_t pitch_elems, uint32_t bytes_per_element)
{
	return TileGetStandard64KBOffset(x, y, pitch_elems, bytes_per_element);
}

uint64_t OffsetStandard4KB(uint32_t x, uint32_t y, uint32_t pitch_elems, uint32_t bytes_per_element)
{
	return TileGetStandard4KBOffset(x, y, pitch_elems, bytes_per_element);
}

uint64_t OffsetDepth64KB32(uint32_t x, uint32_t y, uint32_t pitch_elems, uint32_t /*bytes_per_element*/)
{
	return TileGetDepth64KB32Offset(x, y, pitch_elems);
}

bool LayoutBpeSupported(TileDetileLayout layout, uint32_t bytes_per_element)
{
	if (layout == TileDetileLayout::Sw64kRx)
	{
		return bytes_per_element == 4u || bytes_per_element == 8u;
	}
	if (layout == TileDetileLayout::Standard64KB)
	{
		return TileGet64KBBlockWidth(bytes_per_element) != 0u;
	}
	if (layout == TileDetileLayout::Standard4KB)
	{
		const bool power_of_two = (bytes_per_element & (bytes_per_element - 1u)) == 0u;
		return bytes_per_element >= 1u && bytes_per_element <= 16u && power_of_two;
	}
	if (layout == TileDetileLayout::Depth64KB32)
	{
		return bytes_per_element == 4u;
	}
	return false;
}

TileOffsetFn ResolveOffsetFn(TileDetileLayout layout)
{
	if (layout == TileDetileLayout::Sw64kRx)
	{
		return OffsetSw64kRx;
	}
	if (layout == TileDetileLayout::Standard64KB)
	{
		return OffsetStandard64KB;
	}
	if (layout == TileDetileLayout::Standard4KB)
	{
		return OffsetStandard4KB;
	}
	if (layout == TileDetileLayout::Depth64KB32)
	{
		return OffsetDepth64KB32;
	}
	return nullptr;
}

uint32_t ResolvePitch(uint32_t pitch_elems, uint32_t width)
{
	if (pitch_elems != 0u)
	{
		return pitch_elems;
	}
	return width;
}

bool CalculateRequiredSourceBytes(const TileDetileRequest& request, uint64_t* bytes)
{
	if (bytes == nullptr)
	{
		return false;
	}

	const uint32_t pitch = ResolvePitch(request.pitch_elems, request.width);
	if (request.layout == TileDetileLayout::Sw64kRx)
	{
		const uint32_t block_width  = TileGet64KBBlockWidth(request.bytes_per_element);
		const uint32_t block_height = (request.bytes_per_element == 8u ? 64u : 128u);
		return CanRoundUpU32(pitch, block_width) &&
		       CalculateBlockGridBytes(pitch, request.height, block_width, block_height, k_64kb_block_bytes, bytes);
	}
	if (request.layout == TileDetileLayout::Standard64KB)
	{
		const uint32_t block_width  = TileGet64KBBlockWidth(request.bytes_per_element);
		const uint32_t block_height = static_cast<uint32_t>(k_64kb_block_bytes / (block_width * request.bytes_per_element));
		return CanRoundUpU32(pitch, block_width) &&
		       CalculateBlockGridBytes(pitch, request.height, block_width, block_height, k_64kb_block_bytes, bytes);
	}
	if (request.layout == TileDetileLayout::Standard4KB)
	{
		const uint32_t block_width =
		    (request.bytes_per_element <= 2u ? 64u : (request.bytes_per_element <= 8u ? 32u : 16u));
		const uint32_t block_height =
		    (request.bytes_per_element == 1u ? 64u : (request.bytes_per_element <= 4u ? 32u : 16u));
		return CanRoundUpU32(pitch, block_width) &&
		       CalculateBlockGridBytes(pitch, request.height, block_width, block_height, k_4kb_block_bytes, bytes);
	}
	if (request.layout == TileDetileLayout::Depth64KB32)
	{
		if ((pitch % 128u) != 0u)
		{
			return false;
		}
		uint64_t blocks_y = 0;
		uint64_t blocks   = 0;
		return DivideRoundUp(request.height, 128u, &blocks_y) && CheckedMultiply(pitch / 128u, blocks_y, &blocks) &&
		       CheckedMultiply(blocks, k_64kb_block_bytes, bytes);
	}
	return false;
}

bool CalculateLinearBytes(const TileDetileRequest& request, uint64_t* bytes)
{
	uint64_t rows = 0;
	return CheckedMultiply(ResolvePitch(request.dst_pitch_elems, request.width), request.height, &rows) &&
	       CheckedMultiply(rows, request.bytes_per_element, bytes);
}

bool ValidateDetileRequest(const TileDetileRequest& request)
{
	if (request.src == nullptr || request.width == 0u || request.height == 0u || request.bytes_per_element == 0u)
	{
		return false;
	}
	if (!LayoutBpeSupported(request.layout, request.bytes_per_element) || ResolveOffsetFn(request.layout) == nullptr)
	{
		return false;
	}

	const uint32_t src_pitch = ResolvePitch(request.pitch_elems, request.width);
	const uint32_t dst_pitch = ResolvePitch(request.dst_pitch_elems, request.width);
	if (src_pitch < request.width || dst_pitch < request.width)
	{
		return false;
	}

	uint64_t required_source_bytes = 0;
	uint64_t linear_bytes          = 0;
	if (!CalculateRequiredSourceBytes(request, &required_source_bytes) || !CalculateLinearBytes(request, &linear_bytes) ||
	    required_source_bytes == 0u || linear_bytes == 0u || required_source_bytes > std::numeric_limits<size_t>::max() ||
	    linear_bytes > std::numeric_limits<size_t>::max())
	{
		return false;
	}

	// Legacy conversion wrappers predate capacity-bearing requests. New guest-facing
	// callers must supply src_bytes; when supplied, it is a hard read boundary.
	return request.src_bytes == 0u || request.src_bytes >= required_source_bytes;
}

void CopyOneElement(uint8_t* dst, const uint8_t* src, uint64_t linear, uint64_t tiled, uint32_t bytes_per_element)
{
	// Both host buffers may be byte-addressed guest allocations. memcpy keeps the
	// copy valid for unaligned addresses and avoids strict-aliasing violations.
	std::memcpy(dst + linear, src + tiled, bytes_per_element);
}

void DetileScalarRange(uint8_t* dst, const uint8_t* src, uint32_t y0, uint32_t y1, uint32_t width, uint32_t pitch,
                       uint32_t dst_pitch, uint32_t bytes_per_element, TileOffsetFn offset_fn)
{
	for (uint32_t y = y0; y < y1; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const uint64_t tiled  = offset_fn(x, y, pitch, bytes_per_element);
			const uint64_t linear = (static_cast<uint64_t>(y) * dst_pitch + x) * bytes_per_element;
			CopyOneElement(dst, src, linear, tiled, bytes_per_element);
		}
	}
}

// Compute-style path processes fixed 8x8 workgroups (matches GPU local_size).
// Select OffsetFn once before entering this hot loop so every element uses a
// direct layout helper call instead of an indirect function-pointer dispatch.
template <TileOffsetFn OffsetFn>
void DetileWorkgroupRange(uint8_t* dst, const uint8_t* src, uint32_t y0, uint32_t y1, uint32_t width, uint32_t pitch,
                          uint32_t dst_pitch, uint32_t bytes_per_element)
{
	static constexpr uint32_t k_group = 8u;
	const uint32_t            groups_x = DivideRoundUp(width, k_group);
	const uint32_t            groups_y = DivideRoundUp(y1 - y0, k_group);

	for (uint32_t gy = 0; gy < groups_y; ++gy)
	{
		const uint32_t base_y = y0 + gy * k_group;
		for (uint32_t gx = 0; gx < groups_x; ++gx)
		{
			const uint32_t base_x = gx * k_group;
			for (uint32_t ly = 0; ly < k_group; ++ly)
			{
				const uint32_t y = base_y + ly;
				if (y >= y1)
				{
					break;
				}
				for (uint32_t lx = 0; lx < k_group; ++lx)
				{
					const uint32_t x = base_x + lx;
					if (x >= width)
					{
						break;
					}
					const uint64_t tiled  = OffsetFn(x, y, pitch, bytes_per_element);
					const uint64_t linear = (static_cast<uint64_t>(y) * dst_pitch + x) * bytes_per_element;
					CopyOneElement(dst, src, linear, tiled, bytes_per_element);
				}
			}
		}
	}
}

// Standard4KB has short contiguous swizzle runs. Keep this production path
// separate from the generic workgroup dispatch so texture uploads retain the
// established memcpy specialization while the compute-style path remains
// available for cross-checking the common layout dispatcher.
void DetileStandard4KBContiguousRange(uint8_t* dst, const uint8_t* src, uint32_t width, uint32_t height, uint32_t pitch,
                                      uint32_t dst_pitch, uint32_t bytes_per_element)
{
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width;)
		{
			const uint32_t available = TileGetStandard4KBContiguousElements(x, bytes_per_element);
			const uint32_t remaining = width - x;
			const uint32_t run       = available < remaining ? available : remaining;
			const uint64_t tiled     = TileGetStandard4KBOffset(x, y, pitch, bytes_per_element);
			const uint64_t linear    = (static_cast<uint64_t>(y) * dst_pitch + x) * bytes_per_element;
			std::memcpy(dst + linear, src + tiled, static_cast<size_t>(run) * bytes_per_element);
			x += run;
		}
	}
}

TileDetileProductionPath GetProductionPathUnchecked(const TileDetileRequest& request)
{
	return request.layout == TileDetileLayout::Standard4KB ? TileDetileProductionPath::Standard4KBContiguous :
	                                                        TileDetileProductionPath::ComputeStyle;
}

bool RunDetile(const TileDetileRequest& request, bool reference, bool compute_style, bool standard4kb_contiguous)
{
	if (!TileDetileIsSupported(request))
	{
		return false;
	}
	if (request.dst == nullptr)
	{
		return false;
	}

	const uint32_t     pitch     = ResolvePitch(request.pitch_elems, request.width);
	const uint32_t     dst_pitch = ResolvePitch(request.dst_pitch_elems, request.width);
	const TileOffsetFn offset_fn = ResolveOffsetFn(request.layout);
	auto*              dst       = static_cast<uint8_t*>(request.dst);
	const auto*        src       = static_cast<const uint8_t*>(request.src);

	uint64_t active_elements = 0;
	uint64_t active_bytes    = 0;
	if (!CheckedMultiply(request.width, request.height, &active_elements) ||
	    !CheckedMultiply(active_elements, request.bytes_per_element, &active_bytes))
	{
		return false;
	}
	const DebugStatsScopedWork detile_work(DebugStatsRecordDetile, active_bytes);

	if (reference)
	{
		DetileScalarRange(dst, src, 0, request.height, request.width, pitch, dst_pitch, request.bytes_per_element, offset_fn);
		return true;
	}
	if (standard4kb_contiguous && GetProductionPathUnchecked(request) == TileDetileProductionPath::Standard4KBContiguous)
	{
		DetileStandard4KBContiguousRange(dst, src, request.width, request.height, pitch, dst_pitch, request.bytes_per_element);
		return true;
	}
	if (compute_style)
	{
		switch (request.layout)
		{
			case TileDetileLayout::Sw64kRx:
				DetileWorkgroupRange<OffsetSw64kRx>(dst, src, 0, request.height, request.width, pitch, dst_pitch,
				                                         request.bytes_per_element);
				break;
			case TileDetileLayout::Standard64KB:
				DetileWorkgroupRange<OffsetStandard64KB>(dst, src, 0, request.height, request.width, pitch, dst_pitch,
				                                             request.bytes_per_element);
				break;
			case TileDetileLayout::Standard4KB:
				DetileWorkgroupRange<OffsetStandard4KB>(dst, src, 0, request.height, request.width, pitch, dst_pitch,
				                                            request.bytes_per_element);
				break;
			case TileDetileLayout::Depth64KB32:
				DetileWorkgroupRange<OffsetDepth64KB32>(dst, src, 0, request.height, request.width, pitch, dst_pitch,
				                                           request.bytes_per_element);
				break;
			default: return false;
		}
		return true;
	}
	DetileScalarRange(dst, src, 0, request.height, request.width, pitch, dst_pitch, request.bytes_per_element, offset_fn);
	return true;
}

} // namespace

bool TileDetileIsSupported(const TileDetileRequest& request)
{
	return ValidateDetileRequest(request);
}

bool TileDetileReference(const TileDetileRequest& request)
{
	return RunDetile(request, /*reference=*/true, /*compute_style=*/false, /*standard4kb_contiguous=*/false);
}

bool TileDetile(const TileDetileRequest& request)
{
	// Production retains the Standard4KB contiguous-run specialization; every
	// other layout follows the validated compute-style dispatcher.
	return RunDetile(request, /*reference=*/false, /*compute_style=*/true, /*standard4kb_contiguous=*/true);
}

bool TileDetileComputeStyle(const TileDetileRequest& request)
{
	return RunDetile(request, /*reference=*/false, /*compute_style=*/true, /*standard4kb_contiguous=*/false);
}

TileDetileProductionPath TileDetileGetProductionPathForTesting(const TileDetileRequest& request)
{
	if (!TileDetileIsSupported(request) || request.dst == nullptr)
	{
		return TileDetileProductionPath::Unsupported;
	}
	return GetProductionPathUnchecked(request);
}

} // namespace Kyty::Libs::Graphics

#endif
