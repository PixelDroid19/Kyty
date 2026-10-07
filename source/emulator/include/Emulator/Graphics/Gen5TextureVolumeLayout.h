#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREVOLUMELAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREVOLUMELAYOUT_H_

#include "Emulator/Graphics/Gen5TextureArrayLayout.h"
#include "Emulator/Graphics/Tile.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// The host upload for a Gen5 Color3D descriptor needs both the physical
// allocation and the linear staging footprint. Keep that contract in one place so
// sampled and storage textures cannot drift apart.
//
// Supported swizzle modes (T# SW mode, single mip only):
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
//    render-target slice, laid out like the layers of a 2D array.
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
};

[[nodiscard]] bool Gen5GetVolumeTextureLayout(uint32_t format, uint32_t width, uint32_t height,
                                              uint32_t depth, uint32_t pitch, uint32_t levels,
                                              uint32_t tile, Gen5TextureVolumeLayout* layout);

[[nodiscard]] bool Gen5ValidateTextureVolumeUpload(const Gen5TextureVolumeLayout& layout, uint64_t source_size);
[[nodiscard]] bool Gen5DetileTextureVolume(void* destination, uint64_t destination_size, const void* source,
                                          uint64_t source_size, const Gen5TextureVolumeLayout& layout);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREVOLUMELAYOUT_H_ */
