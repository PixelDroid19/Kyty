#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREARRAYLAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREARRAYLAYOUT_H_

#include "Emulator/Graphics/Gen5TextureMipLayout.h"
#include "Emulator/Graphics/Tile.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// A Color2DArray resource has one complete tiled allocation per layer. The
// guest allocation and host staging layout intentionally remain separate:
// 64 KiB sample and render-target layouts use padded guest rows but upload
// compact host rows. Multi-mip Standard4KB arrays store one full mip chain
// per layer (including the shared 4 KiB mip tail).
struct Gen5TextureArrayLayout
{
	TileSizeAlign        tiled_slice {};
	uint32_t             layers             = 0;
	uint32_t             levels             = 1;
	uint32_t             width              = 0;
	uint32_t             height             = 0;
	uint32_t             guest_pitch        = 0;
	uint32_t             host_pitch         = 0;
	uint32_t             bytes_per_element  = 0;
	uint32_t             tile               = 0;
	uint64_t             tiled_size         = 0;
	uint64_t             linear_slice_size  = 0;
	uint64_t             linear_size        = 0;
	// Populated for every tile-5 slice so block-compressed element geometry is
	// shared by single- and multi-level arrays.
	Gen5TextureMipLayout mip_layout {};
	bool                 has_mip_layout = false;
};

[[nodiscard]] bool Gen5GetTextureArrayLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels,
                                             uint32_t tile, uint32_t layers, Gen5TextureArrayLayout* layout);
[[nodiscard]] bool Gen5ValidateTextureArrayUpload(const Gen5TextureArrayLayout& layout, uint32_t base_array, uint64_t guest_size);
[[nodiscard]] bool Gen5DetileTextureArray(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                          const Gen5TextureArrayLayout& layout);
// Detile one layer into a slice-sized staging buffer. Host uploads stream this
// path so a 6-face mip chain does not require a linear allocation of every layer.
[[nodiscard]] bool Gen5DetileTextureArrayLayer(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                               const Gen5TextureArrayLayout& layout, uint32_t layer);

// Vulkan buffer-image copies for a detiled array. pitch_texels is
// VkBufferImageCopy::bufferRowLength (texels, including BC block rounding).
// offset is the byte origin of that layer/level in the compact linear staging
// buffer and must fit a 32-bit copy offset.
struct Gen5TextureArrayUploadRegion
{
	uint64_t offset          = 0;
	uint32_t pitch_texels    = 0;
	uint32_t width           = 0;
	uint32_t height          = 0;
	uint32_t dst_level       = 0;
	uint32_t dst_array_layer = 0;
};

[[nodiscard]] bool Gen5FillTextureArrayUploadRegions(const Gen5TextureArrayLayout& layout, Gen5TextureArrayUploadRegion* regions,
                                                     uint32_t region_capacity, uint32_t* region_count);
// Regions for one layer. Offsets are relative to that layer's compact linear
// slice so a streamed upload can reuse a slice-sized staging buffer.
[[nodiscard]] bool Gen5FillTextureArrayLayerUploadRegions(const Gen5TextureArrayLayout& layout, uint32_t layer,
                                                          Gen5TextureArrayUploadRegion* regions, uint32_t region_capacity,
                                                          uint32_t* region_count);

// DXGI/Khronos BC6H_UFLOAT mode in the low bits of the first 16-byte block.
// Returns 0–13 for defined modes, 0xFFu for reserved or truncated input.
[[nodiscard]] uint32_t Gen5Bc6hUfloatMode(const uint8_t* block, uint32_t byte_count);
[[nodiscard]] bool     Gen5Bc6hUfloatModeIsDefined(uint32_t mode);
[[nodiscard]] uint32_t Gen5CountDefinedBc6hUfloatBlocks(const uint8_t* linear, uint64_t linear_size, uint64_t offset,
                                                        uint32_t element_width, uint32_t element_height, uint32_t bytes_per_element);

// DXGI/Khronos BC7: mode is the index of the first set bit in byte 0 (0–7).
// All-zero blocks have no mode bit and return 0xFFu.
[[nodiscard]] uint32_t Gen5Bc7Mode(const uint8_t* block, uint32_t byte_count);
[[nodiscard]] bool     Gen5Bc7ModeIsDefined(uint32_t mode);

struct Gen5DetiledCubeFaceStats
{
	uint32_t sampled_blocks = 0;
	uint32_t nonzero_bytes  = 0;
	uint32_t defined_modes  = 0;
	uint32_t reserved_modes = 0;
	uint32_t first_mode     = 0xFFu;
	uint8_t  first_block[16] {};
};

// Classify up to max_blocks (clamped to 1–32) of mip 0 on one cube face after
// CPU detile. format 179 uses BC6H_UFLOAT modes; format 181 uses BC7 modes.
[[nodiscard]] bool Gen5ClassifyDetiledCubeFace(const uint8_t* linear, uint64_t linear_size, const Gen5TextureArrayLayout& layout,
                                               uint32_t layer, uint32_t format, uint32_t max_blocks, Gen5DetiledCubeFaceStats* stats);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREARRAYLAYOUT_H_ */
