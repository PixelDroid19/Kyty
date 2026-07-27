#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREVOLUMELAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREVOLUMELAYOUT_H_

#include "Emulator/Graphics/Tile.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// The host upload for a Gen5 Color3D descriptor needs both the physical
// Standard4KB allocation and the linear staging footprint. Keep that contract
// in one place so sampled and storage textures cannot drift apart.
struct Gen5TextureVolumeLayout
{
	TileSizeAlign tiled {};
	uint64_t      linear_size       = 0;
	uint32_t      bytes_per_element = 0;
};

[[nodiscard]] bool Gen5GetStandard4KBVolumeTextureLayout(uint32_t format, uint32_t width, uint32_t height,
                                                          uint32_t depth, uint32_t pitch, uint32_t levels,
                                                          uint32_t tile, Gen5TextureVolumeLayout* layout);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREVOLUMELAYOUT_H_ */
