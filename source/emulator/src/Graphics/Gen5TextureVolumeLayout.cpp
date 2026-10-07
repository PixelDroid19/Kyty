#include "Emulator/Graphics/Gen5TextureVolumeLayout.h"

#include "Emulator/Graphics/Shader.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint32_t kSwModeLinear     = 0u;
constexpr uint32_t kSwModeStandard4K  = 5u;
constexpr uint32_t kSwModeStandard64K = 9u;
constexpr uint32_t kSwModeRender64K   = 27u;
constexpr uint64_t kLinearAlign      = 256u; // T# base address granularity

// A swizzled surface has no descriptor pitch (word4 belongs to linear resources): rows are the width rounded up
// to the block width, which is also the staging row length of the upload.
bool StandardLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t depth, uint32_t block_bytes,
                    Gen5TextureVolumeLayout* layout)
{
	// Block-compressed volumes address 4x4 blocks, not texels: not modelled.
	if (ShaderGen5TextureIsBlockCompressed(format)) { return false; }
	const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(format);
	uint32_t       block_width = 0, block_height = 0, block_depth = 0;
	if (!TileGetStandardVolumeBlock(bytes_per_element, block_bytes, &block_width, &block_height, &block_depth)) { return false; }
	const uint32_t pitch = (width + block_width - 1u) / block_width * block_width;
	TileSizeAlign  tiled {};
	if (pitch < width || !TileTryGetStandardVolumeSize(width, height, depth, pitch, bytes_per_element, block_bytes, &tiled))
	{
		return false;
	}
	const uint64_t linear_size =
	    static_cast<uint64_t>(pitch) * static_cast<uint64_t>(height) * static_cast<uint64_t>(depth) * bytes_per_element;
	if (linear_size == 0u || linear_size > tiled.size) { return false; }
	layout->tiled             = tiled;
	layout->linear_size       = linear_size;
	layout->bytes_per_element = bytes_per_element;
	layout->pitch             = pitch;
	layout->block_bytes       = block_bytes;
	layout->linear            = false;
	return true;
}

// Exact byte count of a linear volume, or 0 when it is not representable.
uint64_t LinearVolumeBytes(uint32_t bytes_per_element, uint32_t height, uint32_t depth, uint32_t pitch)
{
	const uint64_t slice = static_cast<uint64_t>(pitch) * height;
	if (bytes_per_element == 0u || slice == 0u || depth == 0u || slice > UINT32_MAX / bytes_per_element) { return 0u; }
	const uint64_t bytes = slice * bytes_per_element * depth;
	return bytes <= UINT32_MAX - kLinearAlign ? bytes : 0u;
}

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1u) & ~(alignment - 1u);
}

bool LinearLayout(uint32_t format, uint32_t height, uint32_t depth, uint32_t pitch, Gen5TextureVolumeLayout* layout)
{
	// Block-compressed volumes address blocks, not texels: not modelled.
	if (ShaderGen5TextureIsBlockCompressed(format)) { return false; }
	const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(format);
	const uint64_t linear_size       = LinearVolumeBytes(bytes_per_element, height, depth, pitch);
	if (linear_size == 0u) { return false; }
	layout->tiled             = {static_cast<uint32_t>(AlignUp(linear_size, kLinearAlign)), static_cast<uint32_t>(kLinearAlign)};
	layout->linear_size       = linear_size;
	layout->bytes_per_element = bytes_per_element;
	layout->linear            = true;
	return true;
}

bool ThinRenderTargetLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t depth, uint32_t pitch,
                            Gen5TextureVolumeLayout* layout)
{
	Gen5TextureArrayLayout slices {};
	if (!Gen5GetTextureArrayLayout(format, width, height, pitch, 1u, kSwModeRender64K, depth, &slices) ||
	    slices.tiled_size > UINT32_MAX)
	{
		return false;
	}
	layout->tiled             = {static_cast<uint32_t>(slices.tiled_size), slices.tiled_slice.align};
	layout->linear_size       = slices.linear_size;
	layout->bytes_per_element = slices.bytes_per_element;
	layout->pitch             = slices.host_pitch;
	layout->thin              = true;
	layout->slices            = slices;
	return true;
}

bool ValidateLinearUpload(const Gen5TextureVolumeLayout& layout, uint64_t source_size)
{
	const uint64_t linear_size = LinearVolumeBytes(layout.bytes_per_element, layout.height, layout.depth, layout.pitch);
	return linear_size != 0u && layout.pitch >= layout.width && layout.linear_size == linear_size &&
	       layout.tiled.size == AlignUp(linear_size, kLinearAlign) && layout.tiled.align == kLinearAlign && source_size >= linear_size;
}

} // namespace

bool Gen5GetVolumeTextureLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t depth, uint32_t pitch, uint32_t levels,
                                uint32_t tile, Gen5TextureVolumeLayout* layout)
{
	if (layout == nullptr) { return false; }
	*layout = {};
	if (width == 0u || height == 0u || depth == 0u || pitch < width || levels != 1u) { return false; }
	Gen5TextureVolumeLayout result {};
	bool                    supported = false;
	if (tile == kSwModeStandard4K) { supported = StandardLayout(format, width, height, depth, 4096u, &result); }
	if (tile == kSwModeStandard64K) { supported = StandardLayout(format, width, height, depth, 65536u, &result); }
	if (tile == kSwModeRender64K) { supported = ThinRenderTargetLayout(format, width, height, depth, pitch, &result); }
	if (tile == kSwModeLinear)
	{
		supported    = LinearLayout(format, height, depth, pitch, &result);
		result.pitch = pitch;
	}
	// Element (0,0,0) is at byte 0 in every swizzle mode, so a one-element volume
	// is laid out exactly as a linear one.
	if (!supported && width == 1u && height == 1u && depth == 1u)
	{
		supported    = LinearLayout(format, 1u, 1u, 1u, &result);
		result.pitch = 1u;
	}
	if (!supported) { return false; }
	result.width  = width;
	result.height = height;
	result.depth  = depth;
	*layout       = result;
	return true;
}

bool Gen5ValidateTextureVolumeUpload(const Gen5TextureVolumeLayout& layout, uint64_t source_size)
{
	if (layout.linear) { return ValidateLinearUpload(layout, source_size); }
	if (layout.thin) { return Gen5ValidateTextureArrayUpload(layout.slices, 0u, source_size); }
	TileSizeAlign tiled {};
	if (!TileTryGetStandardVolumeSize(layout.width, layout.height, layout.depth, layout.pitch, layout.bytes_per_element,
	                                  layout.block_bytes, &tiled))
	{
		return false;
	}
	return layout.tiled.size == tiled.size && layout.tiled.align == tiled.align && source_size >= tiled.size &&
	       layout.linear_size == static_cast<uint64_t>(layout.pitch) * layout.height * layout.depth * layout.bytes_per_element;
}

bool Gen5DetileTextureVolume(void* destination, uint64_t destination_size, const void* source, uint64_t source_size,
                            const Gen5TextureVolumeLayout& layout)
{
	if (destination == nullptr || source == nullptr || !Gen5ValidateTextureVolumeUpload(layout, source_size) ||
	    destination_size < layout.linear_size)
	{
		return false;
	}
	if (layout.linear)
	{
		std::memcpy(destination, source, static_cast<size_t>(layout.linear_size));
		return true;
	}
	if (layout.thin)
	{
		return Gen5DetileTextureArray(destination, destination_size, source, source_size, layout.slices);
	}
	TileConvertStandardVolumeToLinear(destination, source, layout.width, layout.height, layout.depth, layout.pitch,
	                                  layout.bytes_per_element, layout.block_bytes);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
