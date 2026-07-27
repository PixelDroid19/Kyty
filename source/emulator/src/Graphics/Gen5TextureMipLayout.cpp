#include "Emulator/Graphics/Gen5TextureMipLayout.h"

#include "Emulator/Graphics/Shader.h"

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

[[nodiscard]] constexpr bool is_block_compressed(uint32_t format)
{
	return format == 133u || format == 173u;
}

} // namespace

bool Gen5GetStandard4KBTextureMipLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
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
	uint32_t       bytes_log2        = 0;
	if (!get_standard_4kb_block_dimensions(bytes_per_element, &block_width, &block_height, &bytes_log2))
	{
		return false;
	}

	const uint32_t texels_per_element = (is_block_compressed(format) ? 4u : 1u);
	uint32_t       base_element_width = 0;
	uint32_t       base_element_height = 0;
	uint32_t       base_element_pitch = 0;
	if (!ceil_div(width, texels_per_element, &base_element_width) ||
	    !ceil_div(height, texels_per_element, &base_element_height) ||
	    !ceil_div(pitch, texels_per_element, &base_element_pitch) || base_element_pitch != base_element_width)
	{
		// Standard4KB resources in this path use their logical width as pitch.
		// A descriptor with a distinct pitch needs independently evidenced layout
		// rules rather than silently applying the compact chain below.
		return false;
	}

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
		if (entry.element_width <= tail_width && entry.element_height <= tail_height && levels - level <= 8u)
		{
			result.first_tail_level = level;
			break;
		}
	}

	uint64_t tiled_offset = (result.first_tail_level < levels ? 4096u : 0u);
	for (int level = static_cast<int>(result.first_tail_level) - 1; level >= 0; level--)
	{
		auto& entry = result.level[static_cast<uint32_t>(level)];
		uint32_t padded_width  = 0;
		uint32_t padded_height = 0;
		if (!align_up(entry.element_width, block_width, &padded_width) ||
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
		if (tail_index >= 8u)
		{
			return false;
		}
		entry.tiled_pitch = block_width;
		entry.tiled_offset = 0u;
		entry.tiled_size = 4096u;
		entry.tail_x = k_standard_4kb_mip_tail[bytes_log2][tail_index].x;
		entry.tail_y = k_standard_4kb_mip_tail[bytes_log2][tail_index].y;
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
	result.tiled.align = 4096u;
	result.linear_size = linear_offset;
	*layout            = result;
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

} // namespace Kyty::Libs::Graphics

#endif
