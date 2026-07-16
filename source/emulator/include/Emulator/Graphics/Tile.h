#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

enum class TileMode
{
	VideoOutLinear,
	VideoOutTiled,
	TextureLinear,
	TextureTiled,
	// RenderTextureLinear,
	// RenderTextureTiled,
};

struct TileSizeAlign
{
	uint32_t size  = 0;
	uint32_t align = 0;
};

struct TileSizeOffset
{
	uint32_t size   = 0;
	uint32_t offset = 0;
};

struct TilePaddedSize
{
	uint32_t width  = 0;
	uint32_t height = 0;
};

void TileInit();
void TileConvertTiledToLinear(void* dst, const void* src, TileMode mode, uint32_t width, uint32_t height, bool neo);
void TileConvertTiledToLinear(void* dst, const void* src, TileMode mode, uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t levels, bool neo);

bool TileGetDepthSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t z_format, uint32_t stencil_format, bool htile, bool neo,
                      bool next_gen, TileSizeAlign* stencil_size, TileSizeAlign* htile_size, TileSizeAlign* depth_size);
void TileGetVideoOutSize(uint32_t width, uint32_t height, uint32_t pitch, bool tile, bool neo, TileSizeAlign* size);
// Computes the allocation for a Gen5 color target. Mode 0x1b is the 64 KiB
// rotated-X swizzle used by render targets; the result is padded to complete
// swizzle blocks rather than treated as a linear byte span.
void TileGetRenderTargetSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t tile_mode, uint32_t bytes_per_texel,
                             TileSizeAlign* size);
// Byte offset of texel (x,y) inside a Gen5 kRenderTarget (tile mode 27) surface.
// pitch_elems is the element pitch used for the block grid (0 → width).
// Supported: 4- and 8-byte elements.
uint64_t TileGetSw64kRxOffset(uint32_t x, uint32_t y, uint32_t pitch_elems, uint32_t bytes_per_element);
// Detile kRenderTarget into tightly packed linear rows of width*bytes_per_element.
void TileConvertSw64kRxToLinear(void* dst, const void* src, uint32_t width, uint32_t height, uint32_t pitch_elems,
                                uint32_t bytes_per_element);
// Gen5 kStandard64KB (tile mode 9) 32bpp sample atlases.
uint64_t TileGetStandard64KB32Offset(uint32_t x, uint32_t y, uint32_t pitch_elems);
void     TileConvertStandard64KB32ToLinear(void* dst, const void* src, uint32_t width, uint32_t height,
                                           uint32_t pitch_elems);
void TileGetTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels, uint32_t tile,
                        bool neo, TileSizeAlign* total_size, TileSizeOffset* level_sizes, TilePaddedSize* padded_size);
void TileGetTextureSize2(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels, uint32_t tile,
                         TileSizeAlign* total_size, TileSizeOffset* level_sizes, TilePaddedSize* padded_size);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_ */
