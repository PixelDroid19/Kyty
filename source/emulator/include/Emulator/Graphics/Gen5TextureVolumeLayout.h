#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREVOLUMELAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREVOLUMELAYOUT_H_

#include "Emulator/Graphics/Gen5TextureArrayLayout.h"
#include "Emulator/Graphics/Tile.h"

#include <array>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// The host upload for a Gen5 Color3D descriptor needs both the physical
// allocation and the linear staging footprint. Keep that contract in one place so
// sampled and storage textures cannot drift apart.
//
// Supported swizzle modes (T# SW mode; one mip level except in SW mode 27):
//  - 5, Standard 4KB, and 9, Standard 64KB: 1, 2, 4, 8 or 16-byte elements through the RDNA2 SW_4KB_S /
//    SW_64KB_S volume patterns; the descriptor pitch is ignored and rows are the width rounded up to the block
//    width;
//  - 0, linear: rows of `pitch` elements, `height` rows per slice, slices back to
//    back (slice stride = pitch * height * element bytes), the allocation rounded
//    up to 256 bytes like the 2D linear estimate. The T# stores no slice stride for
//    a linear surface, so this is the tight convention, not a measured padding;
//  - 27, render target 64KB (SW_64KB_R_X): the addressing library keeps 3D thick
//    blocks for the Z and standard swizzles only (titles carry its 64 KiB 3D block
//    table), so a render-target volume is thin: every depth slice is a complete 2D
//    render-target slice, laid out like the layers of a 2D array, with the slice
//    index permuting the within-block pipe bits. Each layer holds the whole mip chain, so
//    slice z of level L is layer z of that chain; level L has max(1, depth >> L) slices.
//  - 9 with a mip chain (4-byte elements only): 3D thick blocks of 32x32x16 elements. The volume is a stack of
//    slabs one block deep and every slab holds the whole chain: the mip tail block first, then the larger levels
//    from the smallest to level 0, each padded to whole blocks. Levels that fit in half a block's width join the
//    tail (at most 10), each at its own origin inside the tail block (GFX10 address library thick mip tail).
struct Gen5TextureVolumeLevel
{
	uint32_t width         = 0; // texels
	uint32_t height        = 0;
	uint32_t depth         = 0;
	uint32_t pitch         = 0; // elements of the level's rows in a slab (the block width in the tail)
	uint32_t padded_height = 0;
	uint64_t slab_offset   = 0; // byte offset of the level inside every slab
	uint32_t tail_x        = 0; // origin inside the tail block
	uint32_t tail_y        = 0;
	uint64_t linear_offset = 0; // byte offset of the level in the packed linear staging
};

struct Gen5TextureVolumeLayout
{
	TileSizeAlign tiled {};
	uint64_t      linear_size       = 0;
	uint32_t      bytes_per_element = 0;
	uint32_t      width             = 0;
	uint32_t      height            = 0;
	uint32_t      depth             = 0;
	uint32_t      pitch             = 0;
	uint32_t      block_bytes       = 0;     // 4096 or 65536 for the standard swizzles
	bool          linear            = false; // SW mode 0: upload is a plain copy
	bool          thin              = false; // SW mode 27: depth slices as 2D-array layers
	Gen5TextureArrayLayout slices {};
	// SW mode 9 with more than one level: the slab chain (see above).
	bool                                    mip_chain   = false;
	uint32_t                                levels      = 1;
	uint32_t                                block_depth = 0;
	uint64_t                                slab_bytes  = 0;
	std::array<Gen5TextureVolumeLevel, 16> level {};
};

[[nodiscard]] bool Gen5GetVolumeTextureLayout(uint32_t format, uint32_t width, uint32_t height,
                                              uint32_t depth, uint32_t pitch, uint32_t levels,
                                              uint32_t tile, Gen5TextureVolumeLayout* layout);

[[nodiscard]] bool Gen5ValidateTextureVolumeUpload(const Gen5TextureVolumeLayout& layout, uint64_t source_size);
[[nodiscard]] bool Gen5DetileTextureVolume(void* destination, uint64_t destination_size, const void* source,
                                          uint64_t source_size, const Gen5TextureVolumeLayout& layout);
// Detiles every level of a mip-chained volume into the packed linear staging (level L at level[L].linear_offset).
[[nodiscard]] bool Gen5DetileTextureVolumeLevels(void* destination, uint64_t destination_size, const void* source,
                                                uint64_t source_size, const Gen5TextureVolumeLayout& layout);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREVOLUMELAYOUT_H_ */
