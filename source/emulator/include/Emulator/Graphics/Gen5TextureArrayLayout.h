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
	// Populated for tile-5 multi-mip slices; unused for single-level layouts.
	Gen5TextureMipLayout mip_layout {};
	bool                 has_mip_layout = false;
};

[[nodiscard]] bool Gen5GetTextureArrayLayout(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels,
                                             uint32_t tile, uint32_t layers, Gen5TextureArrayLayout* layout);
[[nodiscard]] bool Gen5ValidateTextureArrayUpload(const Gen5TextureArrayLayout& layout, uint32_t base_array, uint64_t guest_size);
[[nodiscard]] bool Gen5DetileTextureArray(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                          const Gen5TextureArrayLayout& layout);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREARRAYLAYOUT_H_ */
