#include "Emulator/Graphics/Gen5TextureArrayLayout.h"

#include "Emulator/Graphics/Shader.h"

#include <cstring>
#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

bool FillSingleLevelSlice(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint32_t tile, uint32_t bytes_per_element,
                          TileSizeAlign* tiled_slice, uint32_t* host_pitch, uint64_t* linear_slice_size)
{
	if (tiled_slice == nullptr || host_pitch == nullptr || linear_slice_size == nullptr)
	{
		return false;
	}

	TileGetTextureSize2(format, width, height, pitch, 1u, tile, tiled_slice, nullptr, nullptr);
	if (tiled_slice->size == 0u || tiled_slice->align == 0u)
	{
		return false;
	}

	const uint32_t resolved_host_pitch = (tile == 9u || tile == 27u ? width : pitch);
	const uint64_t linear = static_cast<uint64_t>(resolved_host_pitch) * height * bytes_per_element;
	if (linear == 0u || linear > UINT32_MAX)
	{
		return false;
	}

	*host_pitch         = resolved_host_pitch;
	*linear_slice_size  = linear;
	return true;
}

bool FillStandard4KBMipSlice(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels,
                             Gen5TextureMipLayout* mip_layout, TileSizeAlign* tiled_slice, uint32_t* host_pitch,
                             uint64_t* linear_slice_size)
{
	if (mip_layout == nullptr || tiled_slice == nullptr || host_pitch == nullptr || linear_slice_size == nullptr)
	{
		return false;
	}
	if (!Gen5GetStandard4KBTextureMipLayout(format, width, height, pitch, levels, mip_layout))
	{
		return false;
	}
	if (mip_layout->tiled.size == 0u || mip_layout->linear_size == 0u || mip_layout->linear_size > UINT32_MAX)
	{
		return false;
	}

	*tiled_slice        = mip_layout->tiled;
	*host_pitch         = width;
	*linear_slice_size  = mip_layout->linear_size;
	return true;
}

bool DetileOneStandard4KBLayer(uint8_t* output_layer, const uint8_t* input_layer, const Gen5TextureArrayLayout& layout)
{
	if (layout.has_mip_layout)
	{
		return Gen5DetileStandard4KBTextureMipChain(output_layer, layout.linear_slice_size, input_layer, layout.tiled_slice.size,
		                                            layout.mip_layout);
	}
	TileConvertStandard4KBToLinear(output_layer, input_layer, layout.width, layout.height, layout.guest_pitch,
	                               layout.bytes_per_element);
	return true;
}

bool DetileOneLayer(uint8_t* output_layer, const uint8_t* input_layer, const Gen5TextureArrayLayout& layout)
{
	if (layout.tile == 0u)
	{
		std::memcpy(output_layer, input_layer, static_cast<size_t>(layout.linear_slice_size));
		return true;
	}
	if (layout.tile == 5u)
	{
		return DetileOneStandard4KBLayer(output_layer, input_layer, layout);
	}
	if (layout.tile == 9u && layout.host_pitch == layout.width)
	{
		TileConvertStandard64KBToLinear(output_layer, input_layer, layout.width, layout.height, layout.guest_pitch,
		                                layout.bytes_per_element);
		return true;
	}
	if (layout.tile == 27u && layout.host_pitch == layout.width)
	{
		TileConvertSw64kRxToLinear(output_layer, input_layer, layout.width, layout.height, layout.guest_pitch,
		                           layout.bytes_per_element);
		return true;
	}
	return false;
}

} // namespace

bool Gen5GetTextureArrayLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels, uint32_t tile,
                               uint32_t layers, Gen5TextureArrayLayout* layout)
{
	if (layout == nullptr)
	{
		return false;
	}
	*layout = {};

	if (width == 0u || height == 0u || pitch < width || levels == 0u || levels > 16u || layers == 0u)
	{
		return false;
	}

	const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(format);
	if (bytes_per_element == 0u)
	{
		return false;
	}

	const bool linear             = tile == 0u;
	const bool standard_4kb       = tile == 5u;
	const bool standard_64kb      = tile == 9u && TileGet64KBBlockWidth(bytes_per_element) != 0u &&
	                                !ShaderGen5TextureIsBlockCompressed(format);
	const bool render_target_64kb = tile == 27u && (bytes_per_element == 4u || bytes_per_element == 8u) &&
	                                !ShaderGen5TextureIsBlockCompressed(format);

	if (!linear && !standard_4kb && !standard_64kb && !render_target_64kb)
	{
		return false;
	}

	// Multi-mip arrays are only implemented for Standard4KB (including BC).
	if (levels > 1u && !standard_4kb)
	{
		return false;
	}

	TileSizeAlign tiled_slice {};
	uint32_t      host_pitch        = 0;
	uint64_t      linear_slice_size = 0;
	Gen5TextureMipLayout mip_layout {};
	bool                 has_mip_layout = false;

	// Use the shared element-aware layout even for one level. Block-compressed
	// arrays otherwise treat texel dimensions as compressed-block dimensions,
	// making their staging/detile range sixteen times too large.
	if (standard_4kb)
	{
		if (!FillStandard4KBMipSlice(format, width, height, pitch, levels, &mip_layout, &tiled_slice, &host_pitch, &linear_slice_size))
		{
			return false;
		}
		has_mip_layout = true;
	} else if (!FillSingleLevelSlice(format, width, height, pitch, tile, bytes_per_element, &tiled_slice, &host_pitch, &linear_slice_size))
	{
		return false;
	}

	const uint64_t tiled_size  = static_cast<uint64_t>(tiled_slice.size) * layers;
	const uint64_t linear_size = linear_slice_size * layers;
	if (tiled_size == 0u || tiled_size > UINT32_MAX || linear_size == 0u || linear_size > UINT32_MAX)
	{
		return false;
	}

	layout->tiled_slice       = tiled_slice;
	layout->layers            = layers;
	layout->levels            = levels;
	layout->width             = width;
	layout->height            = height;
	layout->guest_pitch       = pitch;
	layout->host_pitch        = host_pitch;
	layout->bytes_per_element = bytes_per_element;
	layout->tile              = tile;
	layout->tiled_size        = tiled_size;
	layout->linear_slice_size = linear_slice_size;
	layout->linear_size       = linear_size;
	layout->mip_layout        = mip_layout;
	layout->has_mip_layout    = has_mip_layout;
	return true;
}

bool Gen5ValidateTextureArrayUpload(const Gen5TextureArrayLayout& layout, uint32_t base_array, uint64_t guest_size)
{
	if (layout.layers == 0u)
	{
		return false;
	}
	if (base_array >= layout.layers)
	{
		return false;
	}
	return guest_size == layout.tiled_size;
}

bool Gen5DetileTextureArrayLayer(void* dst, uint64_t dst_size, const void* src, uint64_t src_size, const Gen5TextureArrayLayout& layout,
                                 uint32_t layer)
{
	if (dst == nullptr || src == nullptr)
	{
		return false;
	}
	if (layout.layers == 0u || layer >= layout.layers || layout.width == 0u || layout.height == 0u)
	{
		return false;
	}
	if (layout.guest_pitch < layout.width || layout.host_pitch == 0u)
	{
		return false;
	}
	if (layout.tiled_slice.size == 0u || layout.linear_slice_size == 0u)
	{
		return false;
	}
	if (dst_size < layout.linear_slice_size)
	{
		return false;
	}
	const uint64_t tiled_offset = static_cast<uint64_t>(layer) * layout.tiled_slice.size;
	if (src_size < tiled_offset || src_size - tiled_offset < layout.tiled_slice.size)
	{
		return false;
	}
	const auto* input_layer = static_cast<const uint8_t*>(src) + tiled_offset;
	return DetileOneLayer(static_cast<uint8_t*>(dst), input_layer, layout);
}

bool Gen5DetileTextureArray(void* dst, uint64_t dst_size, const void* src, uint64_t src_size, const Gen5TextureArrayLayout& layout)
{
	if (dst == nullptr || src == nullptr)
	{
		return false;
	}
	if (layout.layers == 0u || layout.linear_slice_size == 0u)
	{
		return false;
	}
	if (dst_size < layout.linear_size || src_size < layout.tiled_size)
	{
		return false;
	}

	auto* output = static_cast<uint8_t*>(dst);
	for (uint32_t layer = 0; layer < layout.layers; ++layer)
	{
		if (!Gen5DetileTextureArrayLayer(output + static_cast<uint64_t>(layer) * layout.linear_slice_size, layout.linear_slice_size, src,
		                                 src_size, layout, layer))
		{
			return false;
		}
	}
	return true;
}

bool Gen5FillTextureArrayUploadRegions(const Gen5TextureArrayLayout& layout, Gen5TextureArrayUploadRegion* regions,
                                       uint32_t region_capacity, uint32_t* region_count)
{
	if (region_count == nullptr || layout.layers == 0u || layout.levels == 0u || layout.levels > 16u)
	{
		return false;
	}
	const uint64_t needed = static_cast<uint64_t>(layout.layers) * layout.levels;
	if (needed == 0u || needed > UINT32_MAX)
	{
		return false;
	}
	*region_count = static_cast<uint32_t>(needed);
	if (regions == nullptr)
	{
		return true;
	}
	if (region_capacity < *region_count || layout.linear_slice_size == 0u)
	{
		return false;
	}

	uint32_t index = 0;
	for (uint32_t layer = 0; layer < layout.layers; ++layer)
	{
		const uint64_t layer_base = static_cast<uint64_t>(layer) * layout.linear_slice_size;
		if (layout.has_mip_layout)
		{
			if (layout.mip_layout.texels_per_element_x == 0u)
			{
				return false;
			}
			for (uint32_t level = 0; level < layout.levels; ++level)
			{
				const auto&    level_layout = layout.mip_layout.level[level];
				const uint64_t offset       = layer_base + level_layout.linear_offset;
				const uint64_t pitch_texels =
				    static_cast<uint64_t>(level_layout.element_width) * layout.mip_layout.texels_per_element_x;
				if (offset > UINT32_MAX || pitch_texels == 0u || pitch_texels > UINT32_MAX ||
				    level_layout.width == 0u || level_layout.height == 0u)
				{
					return false;
				}
				regions[index].offset          = offset;
				regions[index].pitch_texels    = static_cast<uint32_t>(pitch_texels);
				regions[index].width           = level_layout.width;
				regions[index].height          = level_layout.height;
				regions[index].dst_level       = level;
				regions[index].dst_array_layer = layer;
				++index;
			}
		}
		else
		{
			if (layer_base > UINT32_MAX || layout.host_pitch == 0u || layout.width == 0u || layout.height == 0u)
			{
				return false;
			}
			regions[index].offset          = layer_base;
			regions[index].pitch_texels    = layout.host_pitch;
			regions[index].width           = layout.width;
			regions[index].height          = layout.height;
			regions[index].dst_level       = 0;
			regions[index].dst_array_layer = layer;
			++index;
		}
	}
	return index == *region_count;
}

bool Gen5FillTextureArrayLayerUploadRegions(const Gen5TextureArrayLayout& layout, uint32_t layer,
                                            Gen5TextureArrayUploadRegion* regions, uint32_t region_capacity, uint32_t* region_count)
{
	if (region_count == nullptr || layer >= layout.layers || layout.levels == 0u || layout.levels > 16u)
	{
		return false;
	}
	const uint32_t needed = layout.has_mip_layout ? layout.levels : 1u;
	if (needed == 0u)
	{
		return false;
	}
	*region_count = needed;
	if (regions == nullptr)
	{
		return true;
	}
	if (region_capacity < needed || layout.linear_slice_size == 0u)
	{
		return false;
	}

	if (layout.has_mip_layout)
	{
		if (layout.mip_layout.texels_per_element_x == 0u)
		{
			return false;
		}
		for (uint32_t level = 0; level < layout.levels; ++level)
		{
			const auto&    level_layout = layout.mip_layout.level[level];
			const uint64_t offset       = level_layout.linear_offset;
			const uint64_t pitch_texels =
			    static_cast<uint64_t>(level_layout.element_width) * layout.mip_layout.texels_per_element_x;
			if (offset > UINT32_MAX || pitch_texels == 0u || pitch_texels > UINT32_MAX ||
			    level_layout.width == 0u || level_layout.height == 0u)
			{
				return false;
			}
			regions[level].offset          = offset;
			regions[level].pitch_texels    = static_cast<uint32_t>(pitch_texels);
			regions[level].width           = level_layout.width;
			regions[level].height          = level_layout.height;
			regions[level].dst_level       = level;
			regions[level].dst_array_layer = layer;
		}
		return true;
	}

	if (layout.host_pitch == 0u || layout.width == 0u || layout.height == 0u)
	{
		return false;
	}
	regions[0].offset          = 0;
	regions[0].pitch_texels    = layout.host_pitch;
	regions[0].width           = layout.width;
	regions[0].height          = layout.height;
	regions[0].dst_level       = 0;
	regions[0].dst_array_layer = layer;
	return true;
}

uint32_t Gen5Bc6hUfloatMode(const uint8_t* block, uint32_t byte_count)
{
	if (block == nullptr || byte_count < 16u)
	{
		return 0xFFu;
	}
	const uint32_t bits2 = static_cast<uint32_t>(block[0]) & 0x3u;
	if (bits2 < 2u)
	{
		return bits2;
	}
	const uint32_t bits5 = static_cast<uint32_t>(block[0]) & 0x1Fu;
	switch (bits5)
	{
		case 0x02u: return 2u;
		case 0x06u: return 3u;
		case 0x0Au: return 4u;
		case 0x0Eu: return 5u;
		case 0x12u: return 6u;
		case 0x16u: return 7u;
		case 0x1Au: return 8u;
		case 0x1Eu: return 9u;
		case 0x03u: return 10u;
		case 0x07u: return 11u;
		case 0x0Bu: return 12u;
		case 0x0Fu: return 13u;
		default: return 0xFFu;
	}
}

bool Gen5Bc6hUfloatModeIsDefined(uint32_t mode)
{
	return mode <= 13u;
}

uint32_t Gen5CountDefinedBc6hUfloatBlocks(const uint8_t* linear, uint64_t linear_size, uint64_t offset, uint32_t element_width,
                                          uint32_t element_height, uint32_t bytes_per_element)
{
	if (linear == nullptr || element_width == 0u || element_height == 0u || bytes_per_element != 16u)
	{
		return 0u;
	}
	const uint64_t row_bytes = static_cast<uint64_t>(element_width) * bytes_per_element;
	const uint64_t needed    = offset + row_bytes * element_height;
	if (needed > linear_size)
	{
		return 0u;
	}
	uint32_t defined = 0;
	for (uint32_t y = 0; y < element_height; ++y)
	{
		const uint8_t* row = linear + offset + static_cast<uint64_t>(y) * row_bytes;
		for (uint32_t x = 0; x < element_width; ++x)
		{
			defined += Gen5Bc6hUfloatModeIsDefined(Gen5Bc6hUfloatMode(row + static_cast<uint64_t>(x) * bytes_per_element, 16u)) ? 1u : 0u;
		}
	}
	return defined;
}

uint32_t Gen5Bc7Mode(const uint8_t* block, uint32_t byte_count)
{
	if (block == nullptr || byte_count < 16u)
	{
		return 0xFFu;
	}
	const uint32_t bits = static_cast<uint32_t>(block[0]);
	for (uint32_t mode = 0; mode < 8u; ++mode)
	{
		if ((bits & (1u << mode)) != 0u)
		{
			return mode;
		}
	}
	return 0xFFu;
}

bool Gen5Bc7ModeIsDefined(uint32_t mode)
{
	return mode <= 7u;
}

bool Gen5ClassifyDetiledCubeFace(const uint8_t* linear, uint64_t linear_size, const Gen5TextureArrayLayout& layout, uint32_t layer,
                                 uint32_t format, uint32_t max_blocks, Gen5DetiledCubeFaceStats* stats)
{
	if (stats == nullptr)
	{
		return false;
	}
	*stats = {};
	if (linear == nullptr || !layout.has_mip_layout || layer >= layout.layers || layout.bytes_per_element != 16u)
	{
		return false;
	}
	if (format != 179u && format != 181u)
	{
		return false;
	}
	const uint32_t sample_limit = (max_blocks == 0u ? 1u : (max_blocks > 32u ? 32u : max_blocks));
	const auto&    mip0         = layout.mip_layout.level[0];
	if (mip0.element_width == 0u || mip0.element_height == 0u)
	{
		return false;
	}
	const uint64_t face_offset = static_cast<uint64_t>(layer) * layout.linear_slice_size + mip0.linear_offset;
	const uint64_t row_bytes   = static_cast<uint64_t>(mip0.element_width) * layout.bytes_per_element;
	const uint64_t face_bytes  = row_bytes * mip0.element_height;
	if (face_offset > linear_size || face_bytes > linear_size - face_offset)
	{
		return false;
	}

	const uint32_t total_blocks = mip0.element_width * mip0.element_height;
	const uint32_t to_sample    = (total_blocks < sample_limit ? total_blocks : sample_limit);
	const bool     use_bc7      = format == 181u;
	for (uint32_t index = 0; index < to_sample; ++index)
	{
		const uint32_t y      = index / mip0.element_width;
		const uint32_t x      = index % mip0.element_width;
		const uint8_t* block  = linear + face_offset + static_cast<uint64_t>(y) * row_bytes +
		                       static_cast<uint64_t>(x) * layout.bytes_per_element;
		if (index == 0u)
		{
			std::memcpy(stats->first_block, block, 16u);
		}
		for (uint32_t byte = 0; byte < 16u; ++byte)
		{
			stats->nonzero_bytes += (block[byte] != 0u ? 1u : 0u);
		}
		const uint32_t mode = use_bc7 ? Gen5Bc7Mode(block, 16u) : Gen5Bc6hUfloatMode(block, 16u);
		if (index == 0u)
		{
			stats->first_mode = mode;
		}
		const bool defined = use_bc7 ? Gen5Bc7ModeIsDefined(mode) : Gen5Bc6hUfloatModeIsDefined(mode);
		stats->defined_modes += defined ? 1u : 0u;
		stats->reserved_modes += defined ? 0u : 1u;
		stats->sampled_blocks += 1u;
	}
	return stats->sampled_blocks > 0u;
}

} // namespace Kyty::Libs::Graphics

#endif
