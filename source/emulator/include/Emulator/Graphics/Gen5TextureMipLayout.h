#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREMIPLAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREMIPLAYOUT_H_

#include "Emulator/Graphics/Tile.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// One guest level may live in the shared 4 KiB mip tail. The physical source
// range and the compact linear upload range deliberately remain separate.
struct Gen5TextureMipLevelLayout
{
	uint32_t width          = 0;
	uint32_t height         = 0;
	uint32_t element_width  = 0;
	uint32_t element_height = 0;
	uint32_t tiled_pitch    = 0;
	uint32_t tiled_offset   = 0;
	uint32_t tiled_size     = 0;
	uint32_t linear_offset  = 0;
	uint32_t linear_size    = 0;
	uint32_t tail_x         = 0;
	uint32_t tail_y         = 0;
	bool     in_mip_tail    = false;
};

// Layout and CPU detiler for GFX10 kStandard4KB 2D resources. The helper
// handles ordinary levels and the shared tail used by smaller mip levels.
struct Gen5TextureMipLayout
{
	TileSizeAlign             tiled {};
	uint32_t                  bytes_per_element      = 0;
	uint32_t                  texels_per_element_x  = 1;
	uint32_t                  texels_per_element_y  = 1;
	uint32_t                  levels                 = 0;
	uint32_t                  first_tail_level       = 16;
	uint64_t                  linear_size            = 0;
	Gen5TextureMipLevelLayout level[16] {};
};

// Block-compressed Vulkan copies require a 4×4 texel block. Host images
// therefore stop at the last mip whose both dimensions are still at least 4;
// smaller guest levels clamp there instead of emitting 1×1/2×2 copies.
[[nodiscard]] constexpr uint32_t Gen5CompressedHostMipCount(uint32_t width, uint32_t height, uint32_t guest_levels)
{
	if (guest_levels == 0u)
	{
		return 0u;
	}
	uint32_t count = 1u;
	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	while (count < guest_levels && mip_width >= 8u && mip_height >= 8u)
	{
		mip_width  >>= 1u;
		mip_height >>= 1u;
		++count;
	}
	return count;
}

[[nodiscard]] bool Gen5GetStandard4KBTextureMipLayout(uint32_t format, uint32_t width, uint32_t height,
                                                       uint32_t pitch, uint32_t levels,
                                                       Gen5TextureMipLayout* layout);

[[nodiscard]] bool Gen5DetileStandard4KBTextureMipChain(void* dst, uint64_t dst_size, const void* src,
                                                         uint64_t src_size, const Gen5TextureMipLayout& layout);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GEN5TEXTUREMIPLAYOUT_H_ */
