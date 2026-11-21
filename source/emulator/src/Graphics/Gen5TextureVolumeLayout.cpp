#include "Emulator/Graphics/Gen5TextureVolumeLayout.h"

#include "Emulator/Graphics/Shader.h"

#include <algorithm>
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

// A thin volume stores depth slice z of every level in array layer z; each layer holds the
// whole mip chain, as a 2D-array layer does.
bool ThinRenderTargetLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t depth, uint32_t pitch, uint32_t levels,
                            Gen5TextureVolumeLayout* layout)
{
	Gen5TextureArrayLayout slices {};
	if (!Gen5GetTextureArrayLayout(format, width, height, pitch, levels, kSwModeRender64K, depth, &slices) ||
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

// Inverse of the tail's interleave: mipX takes offset bits 9, 11, 13, ... and mipY bits 8, 10, 12, ...
uint32_t GatherTailBits(uint32_t offset, uint32_t first_bit)
{
	uint32_t value = 0;
	for (uint32_t bit = 0; bit < 6u; ++bit) { value |= ((offset >> (first_bit + 2u * bit)) & 1u) << bit; }
	return value;
}

// A SW_64KB_S volume with a mip chain (see Gen5TextureVolumeLayout). Only 4-byte elements: their 64 KiB block is
// 32x32x16 and their 256-byte block 4x4x4, the case whose tail origins the title confirms.
bool ThickMipChainLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t depth, uint32_t levels,
                         Gen5TextureVolumeLayout* layout)
{
	constexpr uint32_t kBlockBytes      = 65536u;
	constexpr uint32_t kMaxTailLevels   = 10u; // 64 KiB thick blocks
	constexpr uint32_t kTail256Width    = 4u;  // 256-byte 3D block of 4-byte elements
	constexpr uint32_t kTail256Height   = 4u;
	if (ShaderGen5TextureIsBlockCompressed(format) || ShaderGen5TextureBytesPerElement(format) != 4u ||
	    levels > layout->level.size())
	{
		return false;
	}
	uint32_t block_width = 0, block_height = 0, block_depth = 0;
	if (!TileGetStandardVolumeBlock(4u, kBlockBytes, &block_width, &block_height, &block_depth)) { return false; }
	// A level joins the tail when it fits half a block's width and the tail has room for it and every smaller level.
	uint32_t first_tail = levels;
	for (uint32_t index = 0; index < levels; ++index)
	{
		auto& level  = layout->level[index];
		level.width  = std::max(1u, width >> index);
		level.height = std::max(1u, height >> index);
		level.depth  = std::max(1u, depth >> index);
		if (first_tail == levels && level.width <= block_width / 2u && level.height <= block_height && levels - index <= kMaxTailLevels)
		{
			first_tail = index;
		}
	}
	uint64_t slab_offset = first_tail < levels ? kBlockBytes : 0u;
	for (uint32_t index = first_tail; index-- > 0u;)
	{
		auto& level         = layout->level[index];
		level.pitch         = (level.width + block_width - 1u) / block_width * block_width;
		level.padded_height = (level.height + block_height - 1u) / block_height * block_height;
		level.slab_offset   = slab_offset;
		slab_offset += static_cast<uint64_t>(level.pitch) * level.padded_height * block_depth * 4u;
	}
	for (uint32_t index = first_tail; index < levels; ++index)
	{
		auto&          level  = layout->level[index];
		const uint32_t m      = kMaxTailLevels - 1u - (index - first_tail);
		const uint32_t offset = m > 6u ? 16u << m : m << 8u;
		level.pitch           = block_width;
		level.padded_height   = block_height;
		level.tail_x          = GatherTailBits(offset, 9u) * kTail256Width;
		level.tail_y          = GatherTailBits(offset, 8u) * kTail256Height;
		if (level.tail_x + level.width > block_width || level.tail_y + level.height > block_height) { return false; }
	}
	const uint64_t slabs = (depth + block_depth - 1u) / block_depth;
	const uint64_t total = slab_offset * slabs;
	if (slab_offset == 0u || total > UINT32_MAX) { return false; }
	uint64_t linear = 0;
	for (uint32_t index = 0; index < levels; ++index)
	{
		auto& level         = layout->level[index];
		level.linear_offset = linear;
		linear += static_cast<uint64_t>(level.width) * level.height * level.depth * 4u;
	}
	layout->tiled             = {static_cast<uint32_t>(total), kBlockBytes};
	layout->linear_size       = linear;
	layout->bytes_per_element = 4u;
	layout->pitch             = layout->level[0].width;
	layout->block_bytes       = kBlockBytes;
	layout->block_depth       = block_depth;
	layout->slab_bytes        = slab_offset;
	layout->levels            = levels;
	layout->mip_chain         = true;
	return true;
}

void DetileVolumeLevel(uint8_t* destination, const uint8_t* source, const Gen5TextureVolumeLayout& layout,
                       const Gen5TextureVolumeLevel& level)
{
	uint64_t out = level.linear_offset;
	for (uint32_t z = 0; z < level.depth; ++z)
	{
		const uint64_t slab = static_cast<uint64_t>(z / layout.block_depth) * layout.slab_bytes + level.slab_offset;
		for (uint32_t y = 0; y < level.height; ++y)
		{
			for (uint32_t x = 0; x < level.width; ++x, out += 4u)
			{
				const uint64_t tiled = slab + TileGetStandardVolumeOffset(x + level.tail_x, y + level.tail_y, z % layout.block_depth,
				                                                          level.pitch, level.padded_height, 4u, layout.block_bytes);
				std::memcpy(destination + out, source + tiled, 4u);
			}
		}
	}
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
	if (width == 0u || height == 0u || depth == 0u || pitch < width || levels == 0u ||
	    (levels != 1u && tile != kSwModeRender64K && tile != kSwModeStandard64K))
	{
		return false;
	}
	Gen5TextureVolumeLayout result {};
	bool                    supported = false;
	if (tile == kSwModeStandard4K) { supported = StandardLayout(format, width, height, depth, 4096u, &result); }
	if (tile == kSwModeStandard64K)
	{
		supported = levels == 1u ? StandardLayout(format, width, height, depth, 65536u, &result)
		                         : ThickMipChainLayout(format, width, height, depth, levels, &result);
	}
	if (tile == kSwModeRender64K) { supported = ThinRenderTargetLayout(format, width, height, depth, pitch, levels, &result); }
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
	if (layout.mip_chain)
	{
		const uint64_t slabs = (layout.depth + layout.block_depth - 1u) / layout.block_depth;
		return layout.slab_bytes * slabs == layout.tiled.size && source_size >= layout.tiled.size;
	}
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
	// A mip chain has one linear range per level (Gen5DetileTextureVolumeLevels).
	if (layout.mip_chain) { return false; }
	if (layout.thin)
	{
		// A mip chain has one linear range per level (see the sampled volume upload).
		if (layout.slices.levels != 1u) { return false; }
		return Gen5DetileTextureArray(destination, destination_size, source, source_size, layout.slices);
	}
	TileConvertStandardVolumeToLinear(destination, source, layout.width, layout.height, layout.depth, layout.pitch,
	                                  layout.bytes_per_element, layout.block_bytes);
	return true;
}

bool Gen5DetileTextureVolumeLevels(void* destination, uint64_t destination_size, const void* source, uint64_t source_size,
                                   const Gen5TextureVolumeLayout& layout)
{
	if (destination == nullptr || source == nullptr || !layout.mip_chain || !Gen5ValidateTextureVolumeUpload(layout, source_size) ||
	    destination_size < layout.linear_size)
	{
		return false;
	}
	for (uint32_t index = 0; index < layout.levels; ++index)
	{
		DetileVolumeLevel(static_cast<uint8_t*>(destination), static_cast<const uint8_t*>(source), layout, layout.level[index]);
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
