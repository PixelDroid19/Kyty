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
// Display_2dThin (tile mode 10) BGRA8: 128x128 macro tiles; padded pitch/height from
// TileGetTextureSize's dynamic Display Thin rule. Dest rows are tightly packed (width).
void TileConvertDisplayThinBgraToLinear(void* dst, const void* src, uint32_t width, uint32_t height, uint32_t pitch, bool neo);

bool TileGetDepthSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t z_format, uint32_t stencil_format, bool htile, bool neo,
                      bool next_gen, TileSizeAlign* stencil_size, TileSizeAlign* htile_size, TileSizeAlign* depth_size);
void TileGetVideoOutSize(uint32_t width, uint32_t height, uint32_t pitch, bool tile, bool neo, TileSizeAlign* size);
// Computes the allocation for a Gen5 color target. Mode 0x1b is the 64 KiB
// rotated-X swizzle used by render targets; the result is padded to complete
// swizzle blocks rather than treated as a linear byte span.
void TileGetRenderTargetSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t tile_mode, uint32_t bytes_per_texel,
                             TileSizeAlign* size);
// Width in elements of a 64 KiB swizzle block for an exact power-of-two BPE.
// Returns zero for unsupported element sizes.
[[nodiscard]] uint32_t TileGet64KBBlockWidth(uint32_t bytes_per_element);
[[nodiscard]] uint32_t TileAlign64KBPitch(uint32_t width, uint32_t bytes_per_element);
// Byte offset of texel (x,y) inside a Gen5 kRenderTarget (tile mode 27) surface.
// pitch_elems is the element pitch used for the block grid (0 → width).
// Supported: 4- and 8-byte elements.
uint64_t TileGetSw64kRxOffset(uint32_t x, uint32_t y, uint32_t pitch_elems, uint32_t bytes_per_element);
// Detile kRenderTarget into tightly packed linear rows of width*bytes_per_element.
void TileConvertSw64kRxToLinear(void* dst, const void* src, uint32_t width, uint32_t height, uint32_t pitch_elems,
                                uint32_t bytes_per_element);
// Gen5 kStandard64KB (tile mode 9) sample atlas layout. The element size
// determines the 64 KiB block geometry and the within-block swizzle.
uint64_t TileGetStandard64KBOffset(uint32_t x, uint32_t y, uint32_t pitch_elems, uint32_t bytes_per_element);
void     TileConvertStandard64KBToLinear(void* dst, const void* src, uint32_t width, uint32_t height, uint32_t pitch_elems,
                                         uint32_t bytes_per_element);
// Gen5 depth tile mode 24 stores 32-bit depth samples in 64 KiB Z-order blocks.
// This is distinct from render-target tile mode 27 despite identical block size.
uint64_t TileGetDepth64KB32Offset(uint32_t x, uint32_t y, uint32_t pitch_elems);
void     TileConvertDepth64KB32ToLinear(void* dst, const void* src, uint32_t width, uint32_t height, uint32_t pitch_elems);
// Gen5 kStandard4KB (tile mode 5) 32bpp surfaces. The layout is used by
// integer image load/store resources as well as sampled textures.
uint64_t TileGetStandard4KB32Offset(uint32_t x, uint32_t y, uint32_t pitch_elems);
void     TileConvertStandard4KB32ToLinear(void* dst, const void* src, uint32_t width, uint32_t height, uint32_t pitch_elems);
uint64_t TileGetStandard4KBOffset(uint32_t x, uint32_t y, uint32_t pitch_elems, uint32_t bytes_per_element);
[[nodiscard]] uint32_t TileGetStandard4KBContiguousElements(uint32_t x, uint32_t bytes_per_element);
void     TileConvertStandard4KBToLinear(void* dst, const void* src, uint32_t width, uint32_t height, uint32_t pitch_elems,
                                        uint32_t bytes_per_element);
// Gen5 kStandard4KB volumetric R32_UINT resources use 8x16x8 swizzle blocks.
// These helpers are intentionally separate from the 2D layout above: a 3D
// descriptor interleaves Z inside each 4 KiB block and therefore cannot be
// represented as independently tiled 2D slices.
uint64_t TileGetStandard4KB32VolumeOffset(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch_elems, uint32_t height);
void     TileConvertStandard4KB32VolumeToLinear(void* dst, const void* src, uint32_t width, uint32_t height, uint32_t depth,
                                                uint32_t pitch_elems);
void     TileGetStandard4KB32VolumeSize(uint32_t width, uint32_t height, uint32_t depth, uint32_t pitch_elems, TileSizeAlign* size);
void     TileGetTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels, uint32_t tile,
                            bool neo, TileSizeAlign* total_size, TileSizeOffset* level_sizes, TilePaddedSize* padded_size);
void     TileGetTextureSize2(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels, uint32_t tile,
                             TileSizeAlign* total_size, TileSizeOffset* level_sizes, TilePaddedSize* padded_size);

// Unified Gen5 detile request. Offsets come only from the existing layout
// helpers (TileGet*Offset); no alternate address math is allowed.
enum class TileDetileLayout : uint32_t
{
	Sw64kRx,      // tile mode 27 / kRenderTarget (4 or 8 BPE)
	Standard64KB, // tile mode 9  (1/2/4/8/16 BPE)
	Standard4KB,  // tile mode 5  (power-of-two BPE <= 16)
	Depth64KB32,  // tile mode 24 (fixed 4 BPE)
};

struct TileDetileRequest
{
	void*            dst               = nullptr;
	const void*      src               = nullptr;
	uint32_t         width             = 0; // elements (or BC blocks)
	uint32_t         height            = 0;
	uint32_t         pitch_elems       = 0; // guest tiled pitch in elements
	uint32_t         dst_pitch_elems   = 0; // linear row pitch in elements
	uint32_t         bytes_per_element = 0;
	TileDetileLayout layout            = TileDetileLayout::Sw64kRx;
	// Guest tiled allocation size. A nonzero value is a hard source-read limit.
	// Zero is retained only for legacy host wrappers whose containing allocation
	// was validated by the caller.
	uint64_t         src_bytes         = 0;
};

// True when layout, dimensions, arithmetic, and any supplied source range are valid.
[[nodiscard]] bool TileDetileIsSupported(const TileDetileRequest& request);
// Scalar golden path: one element at a time via TileGet*Offset. Tests only.
[[nodiscard]] bool TileDetileReference(const TileDetileRequest& request);
// Production host path. Must match TileDetileReference byte-for-byte.
[[nodiscard]] bool TileDetile(const TileDetileRequest& request);
// Workgroup-structured host path (same math as the GPU compute kernel).
// Used for golden equality against reference without requiring a Vulkan device.
[[nodiscard]] bool TileDetileComputeStyle(const TileDetileRequest& request);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_ */
