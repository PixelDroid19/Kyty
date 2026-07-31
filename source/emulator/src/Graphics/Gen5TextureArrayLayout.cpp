#include "Emulator/Graphics/Gen5TextureArrayLayout.h"

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool Gen5GetTextureArrayLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels,
	                           uint32_t tile, uint32_t layers, Gen5TextureArrayLayout* layout)
{
	if (layout == nullptr)
	{
		return false;
	}
	*layout = {};

	const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(format);
	const bool     standard_64kb     = tile == 9u && TileGet64KBBlockWidth(bytes_per_element) != 0u &&
	                                  !ShaderGen5TextureIsBlockCompressed(format);
	const bool     render_target_64kb = tile == 27u && (bytes_per_element == 4u || bytes_per_element == 8u) &&
	                                    !ShaderGen5TextureIsBlockCompressed(format);
	if (width == 0u || height == 0u || pitch < width || levels != 1u || layers == 0u ||
	    bytes_per_element == 0u || (tile != 5u && tile != 9u && tile != 27u) ||
	    (tile == 9u && !standard_64kb) || (tile == 27u && !render_target_64kb))
	{
		return false;
	}

	TileSizeAlign tiled_slice {};
	TileGetTextureSize2(format, width, height, pitch, levels, tile, &tiled_slice, nullptr, nullptr);
	if (tiled_slice.size == 0u || tiled_slice.align == 0u)
	{
		return false;
	}

	const uint32_t host_pitch = (tile == 9u || tile == 27u ? width : pitch);
	const uint64_t tiled_size = static_cast<uint64_t>(tiled_slice.size) * layers;
	const uint64_t linear_slice_size = static_cast<uint64_t>(host_pitch) * height * bytes_per_element;
	const uint64_t linear_size = linear_slice_size * layers;
	if (tiled_size == 0u || tiled_size > UINT32_MAX || linear_slice_size == 0u || linear_slice_size > UINT32_MAX ||
	    linear_size > UINT32_MAX)
	{
		return false;
	}

	layout->tiled_slice       = tiled_slice;
	layout->layers            = layers;
	layout->width             = width;
	layout->height            = height;
	layout->guest_pitch       = pitch;
	layout->host_pitch        = host_pitch;
	layout->bytes_per_element = bytes_per_element;
	layout->tile              = tile;
	layout->tiled_size        = tiled_size;
	layout->linear_slice_size = linear_slice_size;
	layout->linear_size       = linear_size;
	return true;
}

bool Gen5ValidateTextureArrayUpload(const Gen5TextureArrayLayout& layout, uint32_t base_array, uint64_t guest_size)
{
	return layout.layers != 0u && base_array < layout.layers && guest_size == layout.tiled_size;
}

bool Gen5DetileTextureArray(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
	                         const Gen5TextureArrayLayout& layout)
{
	if (dst == nullptr || src == nullptr || layout.layers == 0u || layout.width == 0u || layout.height == 0u ||
	    layout.guest_pitch < layout.width || layout.host_pitch < layout.width || layout.tiled_slice.size == 0u ||
	    layout.linear_slice_size == 0u || dst_size < layout.linear_size || src_size < layout.tiled_size)
	{
		return false;
	}

	auto*       output = static_cast<uint8_t*>(dst);
	const auto* input  = static_cast<const uint8_t*>(src);
	for (uint32_t layer = 0; layer < layout.layers; ++layer)
	{
		auto*       output_layer = output + static_cast<uint64_t>(layer) * layout.linear_slice_size;
		const auto* input_layer = input + static_cast<uint64_t>(layer) * layout.tiled_slice.size;
		if (layout.tile == 5u)
		{
			TileConvertStandard4KBToLinear(output_layer, input_layer, layout.width, layout.height, layout.guest_pitch,
			                               layout.bytes_per_element);
		} else if (layout.tile == 9u && layout.host_pitch == layout.width)
		{
			TileConvertStandard64KBToLinear(output_layer, input_layer, layout.width, layout.height, layout.guest_pitch,
		                                 layout.bytes_per_element);
		} else if (layout.tile == 27u && layout.host_pitch == layout.width)
		{
			TileConvertSw64kRxToLinear(output_layer, input_layer, layout.width, layout.height, layout.guest_pitch,
		                              layout.bytes_per_element);
		} else
		{
			return false;
		}
	}

	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
