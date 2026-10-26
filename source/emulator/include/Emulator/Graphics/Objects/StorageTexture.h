#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGETEXTURE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGETEXTURE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Utils.h"

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class StorageTextureObject: public GpuObject
{
public:
	static constexpr int PARAM_FORMAT        = 0;
	static constexpr int PARAM_PITCH         = 1;
	static constexpr int PARAM_WIDTH_HEIGHT  = 2;
	static constexpr int PARAM_LEVELS        = 3;
	static constexpr int PARAM_TILE          = 4;
	static constexpr int PARAM_NEO           = 5;
	static constexpr int PARAM_SWIZZLE       = 6;
	static constexpr int PARAM_RESOURCE_TYPE = 7;
	static constexpr int PARAM_DEPTH         = 8;
	static constexpr int PARAM_BASE_ARRAY    = 9;
	static constexpr int PARAM_SKIP_SEED     = 10;

	StorageTextureObject(uint8_t dfmt, uint8_t nfmt, uint16_t fmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t base_level,
	                     uint32_t levels, uint32_t tile, bool neo, uint32_t swizzle, uint8_t resource_type = 9u, uint32_t depth = 1u,
	                     uint32_t base_array = 0u, bool skip_seed = false)
	{
		params[PARAM_FORMAT]        = (static_cast<uint64_t>(fmt) << 16u) | (static_cast<uint64_t>(dfmt) << 8u) | nfmt;
		params[PARAM_PITCH]         = pitch;
		params[PARAM_WIDTH_HEIGHT]  = (static_cast<uint64_t>(width) << 32u) | height;
		params[PARAM_LEVELS]        = (static_cast<uint64_t>(base_level) << 32u) | levels;
		params[PARAM_TILE]          = tile;
		params[PARAM_NEO]           = neo ? 1 : 0;
		params[PARAM_SWIZZLE]       = swizzle;
		params[PARAM_RESOURCE_TYPE] = resource_type;
		params[PARAM_DEPTH]         = depth;
		params[PARAM_BASE_ARRAY]    = base_array;
		params[PARAM_SKIP_SEED]     = skip_seed ? 1u : 0u;
		check_hash                  = true;
		type                        = Graphics::GpuMemoryObjectType::StorageTexture;
	}

	bool Equal(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override;
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override { return nullptr; };
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
};

[[nodiscard]] bool StorageTextureCanCopyGrowingBacking(const uint64_t* existing, const uint64_t* incoming);
// An array descriptor at the address of a live array of the same slice layout names some of its
// leading slices (captured: one slice at level 1 of a six-slice, eight-level 128x128 array). It is a
// view of that backing, never a separate image: the two would diverge on the next write.
[[nodiscard]] bool StorageTextureCanReuseLargerBacking(const uint64_t* existing, const uint64_t* incoming);
// A single-level 2D Gen5 storage view in the depth layout (mode 24) with a 32-bit texel and a 32-bit depth target
// without HTILE or stencil, bound over the same range, address the same texels, so either view reads the raw bytes
// the other last held. The 4-byte render-target layout (mode 27) orders the 8x8 micro tile differently, so a mode 27
// view does not share texels with a depth target. Storage images are never resolution-scaled, so the depth must not
// be either.
[[nodiscard]] bool StorageTextureSharesDepthTexels(const uint64_t* storage, const uint64_t* depth, bool next_gen);
// A storage view that re-describes memory of another view with a different
// format, or the exact range with a different extent: a recycled allocation
// whose texels mean nothing in the other layout.
[[nodiscard]] bool StorageTextureRedescribesRange(const uint64_t* existing, const uint64_t* incoming, bool exact_range);
// A one-level view of exactly the mip tail block of a 64 KiB-swizzled storage chain with the same element format.
[[nodiscard]] bool StorageTextureViewsMipTail(const uint64_t* chain, uint64_t chain_address, const uint64_t* view, uint64_t view_address,
                                              uint64_t view_size);
// The same over a render target: a different texel size, or the exact range
// with a different extent.
[[nodiscard]] bool StorageTextureRedescribesRenderTarget(const uint64_t* render, const uint64_t* storage, bool exact_range);
// The same over a Gen5 depth target: a storage view outside the depth layout (mode 24)
// orders the texels differently, so it never reads what the depth target wrote.
[[nodiscard]] bool StorageTextureRedescribesDepthTarget(const uint64_t* storage, bool next_gen);
// A 2D single-layer image with several levels is one mipmapped backing: each level is written through
// its own single-level view (the descriptor's BASE_LEVEL picks it) and the whole chain is sampled
// through another. Depth-tiled R32 chains established this contract; Gen5 colour chains in the
// render-target (27) and Standard64KB (9) tilings follow it. Tile 9 is a different layout on Gen4, so
// the colour case needs `gen5`. Every other layout keeps the single-level atlas.
[[nodiscard]] bool StorageTextureUsesMipBacking(const uint64_t* params, bool gen5);
// The guest platform decides the Gen5 layout contracts. Object identity is
// evaluated before a platform is chosen (tools, tests): an unknown platform is
// not Gen5.
[[nodiscard]] bool StorageTextureGuestIsGen5();
// Levels of the host image behind a mip backing (0 when there is none). A depth chain has exactly the
// descriptor's levels. A colour chain carries every level its extent allows: captured descriptors
// address levels past MAX_MIP (a downsample loop stores to level 6 of a MAX_MIP = 5 resource, into the
// slack after the allocation), and those stores must land in the image, not fail or clamp onto a real
// level. The result never exceeds the per-level view table.
[[nodiscard]] uint32_t StorageTextureMipBackingLevels(const uint64_t* params, bool gen5);
[[nodiscard]] VkImageUsageFlags StorageTextureGetImageUsage();

struct StorageTextureRenderAliasCopy
{
	uint32_t source_x      = 0;
	uint32_t source_y      = 0;
	uint32_t destination_x = 0;
	uint32_t destination_y = 0;
	uint32_t width         = 0;
	uint32_t height        = 0;
};

[[nodiscard]] bool StorageTexturePlanRenderAlias(const uint64_t* render_params, uint64_t render_address,
	                                             uint64_t render_size, const uint64_t* storage_params,
	                                             uint64_t storage_address, uint64_t storage_size,
	                                             Vector<StorageTextureRenderAliasCopy>* copies);
void StorageTextureCopyRenderAlias(CommandBuffer* buffer, VulkanImage* source, VulkanImage* destination,
	                              const Vector<StorageTextureRenderAliasCopy>& copies);

struct StorageTextureRawRenderAliasPlan
{
	uint32_t source_first_block      = 0;
	uint32_t destination_first_block = 0;
	uint32_t block_count             = 0;
	uint32_t source_blocks_x         = 0;
	uint32_t destination_blocks_x    = 0;
};

[[nodiscard]] bool StorageTexturePlanRawRenderAlias(const uint64_t* render_params, uint64_t render_address,
	                                                 uint64_t render_size, const uint64_t* storage_params,
	                                                 uint64_t storage_address, uint64_t storage_size,
	                                                 StorageTextureRawRenderAliasPlan* plan);
[[nodiscard]] bool StorageTextureCopyRawRenderAlias(GraphicContext* ctx, CommandBuffer* buffer, VulkanImage* source,
	                                                VulkanImage* destination, const StorageTextureRawRenderAliasPlan& plan);

struct StorageTextureRawRenderSource
{
	VulkanImage* image         = nullptr;
	uint64_t     guest_address = 0;
	uint64_t     guest_size    = 0;
	uint32_t     width         = 0;
	uint32_t     height        = 0;
	uint32_t     pitch         = 0;
	uint32_t     bytes_per_pixel = 0;
};

[[nodiscard]] bool StorageTextureDescribeRawRenderSource(const uint64_t* render_params, uint64_t address,
	                                                       uint64_t size, StorageTextureRawRenderSource* source);
// A single-level 2D storage image in the 64 KB R_X layout holds its guest bytes
// in the same block order as a tiled render target.
[[nodiscard]] bool StorageTextureDescribeRawStorageSource(const uint64_t* storage_params, uint64_t address,
	                                                        uint64_t size, StorageTextureRawRenderSource* source);
// The source image can be copied raw into the composite: an unscaled single
// sample, level and layer image of the source's texel size.
[[nodiscard]] bool StorageTextureRawRenderSourceImageMatches(const StorageTextureRawRenderSource& source);
[[nodiscard]] bool StorageTextureRawRenderSourceCovers(const StorageTextureRawRenderSource& source,
	                                                    uint64_t address, uint64_t size);
[[nodiscard]] bool StorageTextureCanCompositeRawRenderDestination(const uint64_t* storage_params,
	                                                               uint64_t address, uint64_t size);
// A linear (tile 0) view with several levels is one stacked image: guest
// memory stores the levels as a GFX10 linear mip chain and the host image
// keeps them as row rectangles at per-level atlas offsets. Plan the upload
// regions and the stacked extent from the descriptor alone.
[[nodiscard]] bool StorageTexturePlanLinearMipUpload(const uint64_t* storage_params, uint64_t size, bool mip_backing,
	                                                  Vector<BufferImageCopy>* regions, uint32_t* image_width,
	                                                  uint32_t* image_height, uint32_t* bytes_per_element);
[[nodiscard]] bool StorageTextureCanCompositeLinearMipRawRenderDestination(const uint64_t* storage_params,
	                                                                        uint64_t address, uint64_t size);
[[nodiscard]] bool StorageTextureCompositeRawRenderAliases(GraphicContext* ctx, CommandBuffer* buffer,
	                                                         const Vector<StorageTextureRawRenderSource>& sources,
	                                                         VulkanImage* destination,
	                                                         const uint64_t* destination_params, uint64_t address,
	                                                         uint64_t size);

struct StorageTextureArrayViewRange
{
	uint32_t base_array_layer = 0;
	uint32_t layer_count      = 0;
};

[[nodiscard]] bool StorageTextureGetArrayViewRanges(uint32_t depth, uint32_t base_array,
                                                    StorageTextureArrayViewRange* sampled,
                                                    StorageTextureArrayViewRange* storage);

// The storage view of an array descriptor: its layer window at its base level, one level, cached on
// the image. Returns the view index, or -1 when the window or level lies outside the image.
[[nodiscard]] int StorageTextureResolveArrayStorageView(VkDevice device, StorageTextureVulkanImage* image, uint32_t depth,
                                                        uint32_t base_array, uint32_t base_level,
                                                        VulkanImageViewCreator create = VulkanCreateDeviceImageView);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_STORAGETEXTURE_H_ */
