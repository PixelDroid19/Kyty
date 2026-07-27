#include "Emulator/Graphics/Gen5TextureVolumeLayout.h"

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool Gen5GetStandard4KBVolumeTextureLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t depth,
                                           uint32_t pitch, uint32_t levels, uint32_t tile,
                                           Gen5TextureVolumeLayout* layout)
{
	if (layout == nullptr)
	{
		return false;
	}
	*layout = {};

	const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(format);
	// TileConvertStandard4KB32VolumeToLinear describes the RDNA2 4-byte
	// element volume equation. Do not route another element size through it:
	// it would produce plausible but corrupt samples.
	if (bytes_per_element != 4u || width == 0u || height == 0u || depth == 0u || pitch < width ||
	    levels != 1u || tile != 5u)
	{
		return false;
	}

	TileSizeAlign tiled {};
	TileGetStandard4KB32VolumeSize(width, height, depth, pitch, &tiled);
	const uint64_t linear_size =
	    static_cast<uint64_t>(pitch) * static_cast<uint64_t>(height) * static_cast<uint64_t>(depth) * bytes_per_element;
	if (linear_size == 0u || linear_size > tiled.size)
	{
		return false;
	}

	layout->tiled             = tiled;
	layout->linear_size       = linear_size;
	layout->bytes_per_element = bytes_per_element;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
