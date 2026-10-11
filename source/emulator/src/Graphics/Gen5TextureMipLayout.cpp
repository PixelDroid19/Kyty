#include "Emulator/Graphics/Gen5TextureMipLayout.h"

#include "Emulator/Graphics/Shader.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

struct MipTailLocation
{
	uint32_t x;
	uint32_t y;
};

// GFX10 kStandard4KB thin-resource mip tails, in element coordinates. Rows
// select bytes-per-element 1, 2, 4, 8 and 16 respectively.
constexpr MipTailLocation k_standard_4kb_mip_tail[5][8] = {
	{{32u, 0u}, {16u, 32u}, {0u, 48u}, {0u, 32u}, {16u, 16u}, {16u, 0u}, {0u, 16u}, {0u, 0u}},
	{{32u, 0u}, {16u, 16u}, {0u, 24u}, {0u, 16u}, {16u, 8u}, {16u, 0u}, {0u, 8u}, {0u, 0u}},
	{{16u, 0u}, {8u, 16u}, {0u, 24u}, {0u, 16u}, {8u, 8u}, {8u, 0u}, {0u, 8u}, {0u, 0u}},
	{{16u, 0u}, {8u, 8u}, {0u, 12u}, {0u, 8u}, {8u, 4u}, {8u, 0u}, {0u, 4u}, {0u, 0u}},
	{{8u, 0u}, {4u, 8u}, {0u, 12u}, {0u, 8u}, {4u, 4u}, {4u, 0u}, {0u, 4u}, {0u, 0u}},
};

// GFX10 depth-64KB thin tails in texel coordinates for 16- and 32-bit depth.
// The 16-bit depth tail enters at a narrower width than the 32-bit tail.
constexpr MipTailLocation k_depth_64kb_mip_tail[2][12] = {
	{{128u, 0u}, {0u, 64u}, {64u, 0u}, {0u, 32u}, {32u, 0u}, {16u, 16u},
	 {0u, 24u}, {0u, 16u}, {16u, 8u}, {16u, 0u}, {0u, 8u}, {0u, 0u}},
	{{64u, 0u}, {0u, 64u}, {32u, 0u}, {0u, 32u}, {16u, 0u}, {8u, 16u},
	 {0u, 24u}, {0u, 16u}, {8u, 8u}, {8u, 0u}, {0u, 8u}, {0u, 0u}},
};

[[nodiscard]] constexpr uint32_t max_one(uint32_t value)
{
	return (value == 0u ? 1u : value);
}

[[nodiscard]] bool ceil_div(uint32_t value, uint32_t divisor, uint32_t* result)
{
	if (result == nullptr || divisor == 0u)
	{
		return false;
	}
	const uint64_t divided = (static_cast<uint64_t>(value) + divisor - 1u) / divisor;
	if (divided == 0u || divided > UINT32_MAX)
	{
		return false;
	}
	*result = static_cast<uint32_t>(divided);
	return true;
}

[[nodiscard]] bool shift_ceil(uint32_t value, uint32_t shift, uint32_t* result)
{
	if (result == nullptr || shift >= 32u)
	{
		return false;
	}
	const uint64_t divisor = 1ull << shift;
	const uint64_t divided = (static_cast<uint64_t>(value) + divisor - 1u) / divisor;
	if (divided == 0u || divided > UINT32_MAX)
	{
		return false;
	}
	*result = static_cast<uint32_t>(divided);
	return true;
}

[[nodiscard]] bool align_up(uint32_t value, uint32_t alignment, uint32_t* result)
{
	if (result == nullptr || alignment == 0u)
	{
		return false;
	}
	const uint64_t aligned = (static_cast<uint64_t>(value) + alignment - 1u) & ~static_cast<uint64_t>(alignment - 1u);
	if (aligned == 0u || aligned > UINT32_MAX)
	{
		return false;
	}
	*result = static_cast<uint32_t>(aligned);
	return true;
}

[[nodiscard]] bool get_standard_4kb_block_dimensions(uint32_t bytes_per_element, uint32_t* width, uint32_t* height,
	                                                   uint32_t* bytes_log2)
{
	if (width == nullptr || height == nullptr || bytes_log2 == nullptr)
	{
		return false;
	}
	switch (bytes_per_element)
	{
		case 1u: *width = 64u; *height = 64u; *bytes_log2 = 0u; return true;
		case 2u: *width = 64u; *height = 32u; *bytes_log2 = 1u; return true;
		case 4u: *width = 32u; *height = 32u; *bytes_log2 = 2u; return true;
		case 8u: *width = 32u; *height = 16u; *bytes_log2 = 3u; return true;
		case 16u: *width = 16u; *height = 16u; *bytes_log2 = 4u; return true;
		default: return false;
	}
}

[[nodiscard]] MipTailLocation get_standard_mip_tail_location(uint32_t bytes_log2, uint32_t index,
                                                             uint32_t block_bytes, uint32_t block_width,
                                                             uint32_t block_height)
{
	if (block_bytes == 4096u)
	{
		return k_standard_4kb_mip_tail[bytes_log2][index];
	}
	if (index >= 4u)
	{
		return k_standard_4kb_mip_tail[bytes_log2][index - 4u];
	}
	// The four larger thin-tail levels precede the shared 4 KiB tail pattern.
	const uint32_t divisor = index < 2u ? 2u : 4u;
	return (index & 1u) == 0u ? MipTailLocation {block_width / divisor, 0u}
	                         : MipTailLocation {0u, block_height / divisor};
}

[[nodiscard]] bool get_standard_texture_mip_layout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                                                    uint32_t levels, uint32_t block_bytes, Gen5TextureMipLayout* layout)
{
	if (layout == nullptr)
	{
		return false;
	}
	*layout = {};

	if (width == 0u || height == 0u || pitch < width || levels == 0u || levels > 16u)
	{
		return false;
	}

	const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(format);
	uint32_t       block_width       = 0;
	uint32_t       block_height      = 0;
	uint32_t       bytes_log2        = 0;
	if (!get_standard_4kb_block_dimensions(bytes_per_element, &block_width, &block_height, &bytes_log2))
	{
		return false;
	}
	const uint32_t block_scale = block_bytes == 65536u ? 4u : 1u;
	const uint32_t tail_levels = block_bytes == 65536u ? 12u : 8u;
	block_width *= block_scale;
	block_height *= block_scale;

	const uint32_t texels_per_element = (ShaderGen5TextureIsBlockCompressed(format) ? 4u : 1u);
	uint32_t       base_element_width = 0;
	uint32_t       base_element_height = 0;
	uint32_t       base_element_pitch = 0;
	if (!ceil_div(width, texels_per_element, &base_element_width) ||
	    !ceil_div(height, texels_per_element, &base_element_height) ||
	    !ceil_div(pitch, texels_per_element, &base_element_pitch) || base_element_pitch < base_element_width)
	{
		return false;
	}
	const bool has_distinct_pitch = (base_element_pitch != base_element_width);

	uint32_t maximum_levels = 1u;
	for (uint32_t dimension = (width > height ? width : height); dimension > 1u; dimension >>= 1u)
	{
		maximum_levels++;
	}
	if (levels > maximum_levels)
	{
		return false;
	}

	Gen5TextureMipLayout result {};
	result.bytes_per_element     = bytes_per_element;
	result.texels_per_element_x = texels_per_element;
	result.texels_per_element_y = texels_per_element;
	result.levels                = levels;
	result.first_tail_level      = levels;

	for (uint32_t level = 0; level < levels; level++)
	{
		auto& entry = result.level[level];
		entry.width  = max_one(width >> level);
		entry.height = max_one(height >> level);
		if (!shift_ceil(base_element_width, level, &entry.element_width) ||
		    !shift_ceil(base_element_height, level, &entry.element_height))
		{
			return false;
		}
	}

	const uint32_t tail_width  = block_width >> 1u;
	const uint32_t tail_height = block_height;
	for (uint32_t level = 0; levels > 1u && level < levels; level++)
	{
		const auto& entry = result.level[level];
		if (entry.element_width <= tail_width && entry.element_height <= tail_height && levels - level <= tail_levels)
		{
			result.first_tail_level = level;
			break;
		}
	}

	uint64_t tiled_offset = (result.first_tail_level < levels ? block_bytes : 0u);
	for (int level = static_cast<int>(result.first_tail_level) - 1; level >= 0; level--)
	{
		auto& entry = result.level[static_cast<uint32_t>(level)];
		uint32_t effective_width = entry.element_width;
		if (has_distinct_pitch && level == 0)
		{
			effective_width = base_element_pitch;
		}
		uint32_t padded_width  = 0;
		uint32_t padded_height = 0;
		if (!align_up(effective_width, block_width, &padded_width) ||
		    !align_up(entry.element_height, block_height, &padded_height))
		{
			return false;
		}
		const uint64_t tiled_size = static_cast<uint64_t>(padded_width) * padded_height * bytes_per_element;
		if (tiled_size == 0u || tiled_size > UINT32_MAX || tiled_offset > UINT32_MAX ||
		    tiled_size > UINT32_MAX - tiled_offset)
		{
			return false;
		}
		entry.tiled_pitch  = padded_width;
		entry.tiled_offset = static_cast<uint32_t>(tiled_offset);
		entry.tiled_size   = static_cast<uint32_t>(tiled_size);
		tiled_offset += tiled_size;
	}

	for (uint32_t level = result.first_tail_level; level < levels; level++)
	{
		auto& entry = result.level[level];
		const auto  tail_index = level - result.first_tail_level;
		if (tail_index >= tail_levels)
		{
			return false;
		}
		entry.tiled_pitch = block_width;
		entry.tiled_offset = 0u;
		entry.tiled_size = block_bytes;
		const auto tail = get_standard_mip_tail_location(bytes_log2, tail_index, block_bytes, block_width, block_height);
		entry.tail_x = tail.x;
		entry.tail_y = tail.y;
		entry.in_mip_tail = true;
		if (entry.tail_x + entry.element_width > block_width || entry.tail_y + entry.element_height > block_height)
		{
			return false;
		}
	}

	uint64_t linear_offset = 0u;
	for (uint32_t level = 0; level < levels; level++)
	{
		auto& entry = result.level[level];
		linear_offset = (linear_offset + 3u) & ~uint64_t {3u};
		const uint64_t linear_size = static_cast<uint64_t>(entry.element_width) * entry.element_height * bytes_per_element;
		if (linear_size == 0u || linear_size > UINT32_MAX || linear_offset > UINT32_MAX ||
		    linear_size > UINT32_MAX - linear_offset)
		{
			return false;
		}
		entry.linear_offset = static_cast<uint32_t>(linear_offset);
		entry.linear_size   = static_cast<uint32_t>(linear_size);
		linear_offset += linear_size;
	}

	if (tiled_offset == 0u || tiled_offset > UINT32_MAX)
	{
		return false;
	}
	result.tiled.size  = static_cast<uint32_t>(tiled_offset);
	result.tiled.align = block_bytes;
	result.linear_size = linear_offset;
	*layout            = result;
	return true;
}

} // namespace

bool Gen5GetStandard4KBTextureMipLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                                         uint32_t levels, Gen5TextureMipLayout* layout)
{
	return get_standard_texture_mip_layout(format, width, height, pitch, levels, 4096u, layout);
}

bool Gen5GetStandard64KBTextureMipLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                                          uint32_t levels, Gen5TextureMipLayout* layout)
{
	return get_standard_texture_mip_layout(format, width, height, pitch, levels, 65536u, layout);
}

bool Gen5GetStandard256BTextureMipLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                                          uint32_t levels, Gen5TextureMipLayout* layout)
{
	if (layout == nullptr)
	{
		return false;
	}
	*layout = {};
	if (width == 0u || height == 0u || pitch < width || levels == 0u || levels > 16u)
	{
		return false;
	}
	const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(format);
	uint32_t       block_width       = 0;
	uint32_t       block_height      = 0;
	if (bytes_per_element == 0u || !TileGetStandard256BBlock(bytes_per_element, &block_width, &block_height))
	{
		return false;
	}
	uint32_t maximum_levels = 1u;
	for (uint32_t dimension = std::max(width, height); dimension > 1u; dimension >>= 1u)
	{
		maximum_levels++;
	}
	const uint32_t texels_per_element  = (ShaderGen5TextureIsBlockCompressed(format) ? 4u : 1u);
	uint32_t       base_element_width  = 0;
	uint32_t       base_element_height = 0;
	uint32_t       base_element_pitch  = 0;
	if (levels > maximum_levels || !ceil_div(width, texels_per_element, &base_element_width) ||
	    !ceil_div(height, texels_per_element, &base_element_height) || !ceil_div(pitch, texels_per_element, &base_element_pitch))
	{
		return false;
	}

	Gen5TextureMipLayout result {};
	result.bytes_per_element    = bytes_per_element;
	result.texels_per_element_x = texels_per_element;
	result.texels_per_element_y = texels_per_element;
	result.levels               = levels;
	result.first_tail_level     = levels;

	uint64_t tiled_offset = 0u;
	for (uint32_t level = levels; level-- > 0u;)
	{
		auto& entry  = result.level[level];
		entry.width  = max_one(width >> level);
		entry.height = max_one(height >> level);
		uint32_t padded_width  = 0;
		uint32_t padded_height = 0;
		if (!shift_ceil(base_element_width, level, &entry.element_width) ||
		    !shift_ceil(base_element_height, level, &entry.element_height) ||
		    !align_up(level == 0u ? base_element_pitch : entry.element_width, block_width, &padded_width) ||
		    !align_up(entry.element_height, block_height, &padded_height))
		{
			return false;
		}
		const uint64_t tiled_size = static_cast<uint64_t>(padded_width) * padded_height * bytes_per_element;
		if (tiled_size > UINT32_MAX - tiled_offset)
		{
			return false;
		}
		entry.tiled_pitch  = padded_width;
		entry.tiled_offset = static_cast<uint32_t>(tiled_offset);
		entry.tiled_size   = static_cast<uint32_t>(tiled_size);
		tiled_offset += tiled_size;
	}

	uint64_t linear_offset = 0u;
	for (uint32_t level = 0; level < levels; level++)
	{
		auto& entry = result.level[level];
		linear_offset = (linear_offset + 3u) & ~uint64_t {3u};
		const uint64_t linear_size = static_cast<uint64_t>(entry.element_width) * entry.element_height * bytes_per_element;
		if (linear_size > UINT32_MAX || linear_offset > UINT32_MAX - linear_size)
		{
			return false;
		}
		entry.linear_offset = static_cast<uint32_t>(linear_offset);
		entry.linear_size   = static_cast<uint32_t>(linear_size);
		linear_offset += linear_size;
	}

	result.tiled.size  = static_cast<uint32_t>(tiled_offset);
	result.tiled.align = 256u;
	result.linear_size = linear_offset;
	*layout            = result;
	return true;
}

bool Gen5DetileStandard256BTextureMipChain(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                           const Gen5TextureMipLayout& layout)
{
	if (dst == nullptr || src == nullptr || layout.levels == 0u || layout.levels > 16u || layout.bytes_per_element == 0u ||
	    src_size < layout.tiled.size || dst_size < layout.linear_size)
	{
		return false;
	}
	for (uint32_t level = 0; level < layout.levels; level++)
	{
		const auto& entry = layout.level[level];
		if (entry.in_mip_tail || entry.element_width == 0u || entry.element_height == 0u ||
		    entry.tiled_pitch < entry.element_width || static_cast<uint64_t>(entry.linear_offset) + entry.linear_size > dst_size ||
		    static_cast<uint64_t>(entry.tiled_offset) + entry.tiled_size > src_size)
		{
			return false;
		}
		TileDetileRequest request {};
		request.dst               = static_cast<uint8_t*>(dst) + entry.linear_offset;
		request.src               = static_cast<const uint8_t*>(src) + entry.tiled_offset;
		request.width             = entry.element_width;
		request.height            = entry.element_height;
		request.pitch_elems       = entry.tiled_pitch;
		request.dst_pitch_elems   = entry.element_width;
		request.bytes_per_element = layout.bytes_per_element;
		request.layout            = TileDetileLayout::Standard256B;
		request.src_bytes         = entry.tiled_size;
		if (!TileDetile(request))
		{
			return false;
		}
	}
	return true;
}

bool Gen5DetileStandard4KBTextureMipChain(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
	                                       const Gen5TextureMipLayout& layout)
{
	if (dst == nullptr || src == nullptr || layout.levels == 0u || layout.levels > 16u ||
	    layout.bytes_per_element == 0u || src_size < layout.tiled.size || dst_size < layout.linear_size)
	{
		return false;
	}

	uint32_t block_width  = 0;
	uint32_t block_height = 0;
	uint32_t bytes_log2   = 0;
	if (!get_standard_4kb_block_dimensions(layout.bytes_per_element, &block_width, &block_height, &bytes_log2))
	{
		return false;
	}
	(void) bytes_log2;

	auto*       output = static_cast<uint8_t*>(dst);
	const auto* input  = static_cast<const uint8_t*>(src);
	for (uint32_t level = 0; level < layout.levels; level++)
	{
		const auto& entry = layout.level[level];
		if (entry.element_width == 0u || entry.element_height == 0u || entry.tiled_pitch < entry.element_width ||
		    entry.tiled_size == 0u || entry.linear_size == 0u ||
		    static_cast<uint64_t>(entry.linear_offset) + entry.linear_size > dst_size ||
		    static_cast<uint64_t>(entry.tiled_offset) + entry.tiled_size > src_size)
		{
			return false;
		}

		const size_t row_bytes = static_cast<size_t>(entry.element_width) * layout.bytes_per_element;
		if (row_bytes == 0u)
		{
			return false;
		}
		auto* level_output = output + entry.linear_offset;

		if (entry.in_mip_tail)
		{
			if (entry.tiled_pitch != block_width || entry.tiled_size != 4096u ||
			    entry.tail_x + entry.element_width > block_width || entry.tail_y + entry.element_height > block_height)
			{
				return false;
			}
			std::vector<uint8_t> block_linear(4096u);
			TileConvertStandard4KBToLinear(block_linear.data(), input + entry.tiled_offset, block_width, block_height,
			                              block_width, layout.bytes_per_element);
			for (uint32_t y = 0; y < entry.element_height; y++)
			{
				const auto* row = block_linear.data() +
				                  (static_cast<size_t>(entry.tail_y + y) * block_width + entry.tail_x) * layout.bytes_per_element;
				std::memcpy(level_output + static_cast<size_t>(y) * row_bytes, row, row_bytes);
			}
		} else if (entry.tiled_pitch == entry.element_width)
		{
			// Compact host rows already match the tiled pitch; detile in place.
			TileConvertStandard4KBToLinear(level_output, input + entry.tiled_offset, entry.element_width, entry.element_height,
			                              entry.tiled_pitch, layout.bytes_per_element);
		} else
		{
			const uint64_t temporary_size = static_cast<uint64_t>(entry.tiled_pitch) * entry.element_height * layout.bytes_per_element;
			if (temporary_size == 0u || temporary_size > std::numeric_limits<size_t>::max())
			{
				return false;
			}
			std::vector<uint8_t> padded_linear(static_cast<size_t>(temporary_size));
			TileConvertStandard4KBToLinear(padded_linear.data(), input + entry.tiled_offset, entry.element_width,
			                              entry.element_height, entry.tiled_pitch, layout.bytes_per_element);
			for (uint32_t y = 0; y < entry.element_height; y++)
			{
				std::memcpy(level_output + static_cast<size_t>(y) * row_bytes,
				            padded_linear.data() + static_cast<size_t>(y) * entry.tiled_pitch * layout.bytes_per_element, row_bytes);
			}
		}
	}

	return true;
}

bool Gen5DetileStandard64KBTextureMipChain(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                             const Gen5TextureMipLayout& layout)
{
	const uint32_t block_width = TileGet64KBBlockWidth(layout.bytes_per_element);
	if (dst == nullptr || src == nullptr || block_width == 0u || layout.levels == 0u || layout.levels > 16u ||
	    layout.tiled.align != 65536u || src_size < layout.tiled.size || dst_size < layout.linear_size)
	{
		return false;
	}
	const uint32_t block_height = 65536u / (block_width * layout.bytes_per_element);
	auto* output = static_cast<uint8_t*>(dst);
	const auto* input = static_cast<const uint8_t*>(src);
	std::vector<uint8_t> tail;
	for (uint32_t level = 0; level < layout.levels; ++level)
	{
		const auto& entry = layout.level[level];
		const uint64_t row_bytes = static_cast<uint64_t>(entry.element_width) * layout.bytes_per_element;
		if (entry.element_width == 0u || entry.element_height == 0u || entry.tiled_pitch < entry.element_width ||
		    row_bytes > entry.linear_size || row_bytes * entry.element_height != entry.linear_size || entry.tiled_size == 0u ||
		    static_cast<uint64_t>(entry.linear_offset) + entry.linear_size > dst_size ||
		    static_cast<uint64_t>(entry.tiled_offset) + entry.tiled_size > src_size)
		{
			return false;
		}
		TileDetileRequest request {};
		request.src = input + entry.tiled_offset;
		request.src_bytes = entry.tiled_size;
		request.layout = TileDetileLayout::Standard64KB;
		request.bytes_per_element = layout.bytes_per_element;
		request.pitch_elems = entry.tiled_pitch;
		request.width = entry.element_width;
		request.height = entry.element_height;
		request.dst_pitch_elems = entry.element_width;
		request.dst = output + entry.linear_offset;
		if (!entry.in_mip_tail)
		{
			if (!TileDetile(request)) { return false; }
			continue;
		}
		if (entry.tiled_pitch != block_width || entry.tiled_size != 65536u || entry.tiled_offset != 0u ||
		    entry.tail_x >= block_width || entry.element_width > block_width - entry.tail_x ||
		    entry.tail_y >= block_height || entry.element_height > block_height - entry.tail_y)
		{
			return false;
		}
		if (tail.empty())
		{
			tail.resize(65536u);
			request.dst = tail.data();
			request.width = request.dst_pitch_elems = block_width;
			request.height = block_height;
			if (!TileDetile(request)) { return false; }
		}
		for (uint32_t y = 0; y < entry.element_height; ++y)
		{
			const uint64_t origin = (static_cast<uint64_t>(entry.tail_y + y) * block_width + entry.tail_x) *
			                        layout.bytes_per_element;
			std::memcpy(output + entry.linear_offset + y * row_bytes, tail.data() + origin, static_cast<size_t>(row_bytes));
		}
	}
	return true;
}

bool Gen5GetDepth64KBTextureMipLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
	                                   uint32_t levels, Gen5TextureMipLayout* layout)
{
	if (layout == nullptr)
	{
		return false;
	}
	*layout = {};
	const uint32_t bytes_per_element = format == 7u ? 2u : format == 22u ? 4u : 0u;
	const uint32_t block_width       = TileGet64KBBlockWidth(bytes_per_element);
	constexpr uint32_t block_height = 128u;
	constexpr uint32_t block_bytes  = 65536u;
	if (bytes_per_element == 0u || width == 0u || height == 0u || pitch != TileAlign64KBPitch(width, bytes_per_element) ||
	    levels == 0u || levels > 16u)
	{
		return false;
	}
	uint32_t maximum_levels = 1u;
	for (uint32_t dimension = std::max(width, height); dimension > 1u; dimension >>= 1u)
	{
		maximum_levels++;
	}
	if (levels > maximum_levels)
	{
		return false;
	}

	Gen5TextureMipLayout result {};
	result.bytes_per_element = bytes_per_element;
	result.levels            = levels;
	result.first_tail_level  = levels;
	for (uint32_t level = 0u; level < levels; level++)
	{
		auto& mip            = result.level[level];
		mip.width            = max_one(width >> level);
		mip.height           = max_one(height >> level);
		mip.element_width    = mip.width;
		mip.element_height   = mip.height;
	}
	const uint32_t tail_width = bytes_per_element == 2u ? 64u : block_width / 2u;
	for (uint32_t level = 0u; levels > 1u && level < levels; level++)
	{
		const auto& mip = result.level[level];
		if (mip.width <= tail_width && mip.height <= block_height && levels - level <= 12u)
		{
			result.first_tail_level = level;
			break;
		}
	}

	uint64_t tiled_offset = result.first_tail_level < levels ? block_bytes : 0u;
	for (int level = static_cast<int>(result.first_tail_level) - 1; level >= 0; level--)
	{
		auto&    mip = result.level[static_cast<uint32_t>(level)];
		uint32_t padded_width = 0u;
		uint32_t padded_height = 0u;
		if (!align_up(mip.width, block_width, &padded_width) || !align_up(mip.height, block_height, &padded_height))
		{
			return false;
		}
		const uint64_t tiled_size = static_cast<uint64_t>(padded_width) * padded_height * bytes_per_element;
		if (tiled_size == 0u || tiled_offset > UINT32_MAX || tiled_size > UINT32_MAX - tiled_offset)
		{
			return false;
		}
		mip.tiled_pitch  = padded_width;
		mip.tiled_offset = static_cast<uint32_t>(tiled_offset);
		mip.tiled_size   = static_cast<uint32_t>(tiled_size);
		tiled_offset += tiled_size;
	}
	for (uint32_t level = result.first_tail_level; level < levels; level++)
	{
		auto& mip = result.level[level];
		const auto tail_index = level - result.first_tail_level;
		if (tail_index >= 12u)
		{
			return false;
		}
		const auto location = k_depth_64kb_mip_tail[bytes_per_element == 2u ? 0u : 1u][tail_index];
		mip.tiled_pitch = block_width;
		mip.tiled_size = block_bytes;
		mip.tail_x = location.x;
		mip.tail_y = location.y;
		mip.in_mip_tail = true;
		if (mip.tail_x + mip.width > block_width || mip.tail_y + mip.height > block_height)
		{
			return false;
		}
	}
	uint64_t linear_offset = 0u;
	for (uint32_t level = 0u; level < levels; level++)
	{
		auto& mip = result.level[level];
		linear_offset = (linear_offset + 3u) & ~uint64_t {3u};
		const uint64_t linear_size = static_cast<uint64_t>(mip.width) * mip.height * bytes_per_element;
		if (linear_size == 0u || linear_size > UINT32_MAX || linear_offset > UINT32_MAX ||
		    linear_size > UINT32_MAX - linear_offset)
		{
			return false;
		}
		mip.linear_offset = static_cast<uint32_t>(linear_offset);
		mip.linear_size = static_cast<uint32_t>(linear_size);
		linear_offset += linear_size;
	}
	if (tiled_offset == 0u || tiled_offset > UINT32_MAX)
	{
		return false;
	}
	result.tiled.size  = static_cast<uint32_t>(tiled_offset);
	result.tiled.align = block_bytes;
	result.linear_size = linear_offset;
	*layout            = result;
	return true;
}

bool Gen5DetileDepth64KBTextureMipChain(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
	                                     const Gen5TextureMipLayout& layout)
{
	if (dst == nullptr || src == nullptr || layout.levels == 0u || layout.levels > 16u ||
	    (layout.bytes_per_element != 2u && layout.bytes_per_element != 4u) ||
	    src_size < layout.tiled.size || dst_size < layout.linear_size)
	{
		return false;
	}
	const uint32_t block_width = TileGet64KBBlockWidth(layout.bytes_per_element);
	const auto*    input       = static_cast<const uint8_t*>(src);
	auto*          output      = static_cast<uint8_t*>(dst);
	for (uint32_t level = 0u; level < layout.levels; level++)
	{
		const auto& mip = layout.level[level];
		if (mip.width == 0u || mip.height == 0u || mip.tiled_pitch < mip.width ||
		    (mip.tiled_pitch % block_width) != 0u || mip.tiled_size == 0u ||
		    static_cast<uint64_t>(mip.tiled_offset) + mip.tiled_size > src_size ||
		    static_cast<uint64_t>(mip.linear_offset) + mip.linear_size > dst_size ||
		    static_cast<uint64_t>(mip.width) * mip.height * layout.bytes_per_element != mip.linear_size ||
		    (mip.in_mip_tail && (mip.tiled_pitch != block_width || mip.tiled_size != 65536u ||
		                         mip.tail_x + mip.width > block_width || mip.tail_y + mip.height > 128u)))
		{
			return false;
		}
		for (uint32_t y = 0u; y < mip.height; y++)
		{
			for (uint32_t x = 0u; x < mip.width; x++)
			{
				const uint64_t offset = TileGetDepth64KBOffset(x + mip.tail_x, y + mip.tail_y, mip.tiled_pitch,
				                                                layout.bytes_per_element);
				if (offset + layout.bytes_per_element > mip.tiled_size)
				{
					return false;
				}
				const uint64_t dest = static_cast<uint64_t>(mip.linear_offset) +
				                      (static_cast<uint64_t>(y) * mip.width + x) * layout.bytes_per_element;
				std::memcpy(output + dest, input + mip.tiled_offset + offset, layout.bytes_per_element);
			}
		}
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
