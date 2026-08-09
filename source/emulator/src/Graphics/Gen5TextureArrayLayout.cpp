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

	if (standard_4kb && levels > 1u)
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

bool Gen5DetileTextureArray(void* dst, uint64_t dst_size, const void* src, uint64_t src_size, const Gen5TextureArrayLayout& layout)
{
	if (dst == nullptr || src == nullptr)
	{
		return false;
	}
	if (layout.layers == 0u || layout.width == 0u || layout.height == 0u)
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
	if (dst_size < layout.linear_size || src_size < layout.tiled_size)
	{
		return false;
	}

	auto*       output = static_cast<uint8_t*>(dst);
	const auto* input  = static_cast<const uint8_t*>(src);
	for (uint32_t layer = 0; layer < layout.layers; ++layer)
	{
		auto*       output_layer = output + static_cast<uint64_t>(layer) * layout.linear_slice_size;
		const auto* input_layer  = input + static_cast<uint64_t>(layer) * layout.tiled_slice.size;
		if (!DetileOneLayer(output_layer, input_layer, layout))
		{
			return false;
		}
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
