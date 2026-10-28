#include "Emulator/Graphics/Objects/StorageTexture.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Gen5TextureArrayLayout.h"
#include "Emulator/Graphics/Gen5TextureMipLayout.h"
#include "Emulator/Graphics/Gen5TextureVolumeLayout.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/RenderTexture.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

// IWYU pragma: no_forward_declare VkImageView_T

#include <algorithm>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool StorageTextureGetArrayViewRanges(uint32_t depth, uint32_t base_array, StorageTextureArrayViewRange* sampled,
                                      StorageTextureArrayViewRange* storage)
{
	if (sampled == nullptr || storage == nullptr)
	{
		return false;
	}
	if (depth == 0u || base_array >= depth)
	{
		return false;
	}

	*sampled = {0u, depth};
	*storage = {base_array, depth - base_array};
	return true;
}

VkImageUsageFlags StorageTextureGetImageUsage()
{
	VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	vk_usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	return vk_usage;
}

// Object identity is also evaluated before a guest platform has been chosen (tools, tests): an
// unknown platform is not Gen5.
static bool GuestIsGen5()
{
	return Config::IsInitialized() && Config::GetGuestPlatform() == GuestPlatform::Ps5;
}

static bool IsR32SingleComponentStorageFormat(uint32_t fmt)
{
	return fmt == 20u || fmt == 22u;
}

// One- and two-component typed formats whose storage view always uses the
// identity mapping, whatever their guest selector.
static bool IsIdentityViewStorageFormat(uint32_t fmt)
{
	return fmt == 5u || fmt == 7u || fmt == 11u || fmt == 13u || fmt == 14u || fmt == 18u || fmt == 62u;
}

// `format` is the packed PARAM_FORMAT (fmt << 16 | dfmt << 8 | nfmt).
static uint32_t NormalizeStorageTextureSwizzle(uint64_t format, uint32_t swizzle)
{
	const auto fmt  = static_cast<uint32_t>((format >> 16u) & 0xffffu);
	const auto dfmt = static_cast<uint8_t>(format >> 8u);
	const auto nfmt = static_cast<uint8_t>(format);
	// Storage image views for these typed formats use identity component
	// mapping. Reuse must follow the effective host view contract rather than
	// the raw guest selector bits, otherwise equivalent bindings churn a fresh
	// GpuMemory object every frame.
	if (IsIdentityViewStorageFormat(fmt))
	{
		return DstSel(4, 5, 6, 7);
	}
	if (fmt == 1u && swizzle == DstSel(4, 0, 0, 1))
	{
		return DstSel(4, 5, 6, 7);
	}
	if (IsR32SingleComponentStorageFormat(fmt) && (swizzle == DstSel(4, 0, 0, 1) || swizzle == DstSel(4, 0, 0, 0)))
	{
		return DstSel(4, 5, 6, 7);
	}
	if (fmt == 36u && swizzle == DstSel(4, 5, 6, 1))
	{
		// The packed three-component format has no physical alpha channel. Its
		// guest selector supplies the architectural default one, whereas storage
		// image views must keep an identity component mapping.
		return DstSel(4, 5, 6, 7);
	}
	if ((fmt == 29u || fmt == 64u) && swizzle == DstSel(4, 5, 0, 1))
	{
		// The two-component format has no physical blue/alpha channels. Its
		// guest selector supplies the architectural defaults (0, 1), which is
		// exactly how a two-component Vulkan format expands, whereas storage
		// image views must keep an identity component mapping.
		return DstSel(4, 5, 6, 7);
	}
	if (ShaderStorageImageSwizzleInShader(swizzle, VulkanStorageHasRedBlueView(dfmt, nfmt, static_cast<uint16_t>(fmt)),
	                                      VulkanStorageHasSingleChannel(dfmt, nfmt, static_cast<uint16_t>(fmt))))
	{
		// The image-store emitter places each component in its selected channel.
		return DstSel(4, 5, 6, 7);
	}
	return swizzle;
}

static void update_func(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	KYTY_PROFILER_BLOCK("StorageTextureObject::update_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = static_cast<StorageTextureVulkanImage*>(obj);

	auto tile   = params[StorageTextureObject::PARAM_TILE];
	auto fmt    = (params[StorageTextureObject::PARAM_FORMAT] >> 16u) & 0xffffu;
	auto dfmt   = (params[StorageTextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto nfmt   = (params[StorageTextureObject::PARAM_FORMAT]) & 0xffu;
	auto width  = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto       levels            = params[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto       pitch             = params[StorageTextureObject::PARAM_PITCH];
	auto       resource_type     = params[StorageTextureObject::PARAM_RESOURCE_TYPE];
	auto       depth             = params[StorageTextureObject::PARAM_DEPTH];
	auto       base_array        = params[StorageTextureObject::PARAM_BASE_ARRAY];
	bool       neo               = Config::IsNeo();
	const bool three_dimensional = resource_type == 10u;
	const bool arrayed_2d        = resource_type == 13u || resource_type == 11u;
	const bool depth64kb32       = fmt == 22u && tile == 24u;
	const bool skip_seed         = params[StorageTextureObject::PARAM_SKIP_SEED] != 0;
	const bool mip_backing       = StorageTextureUsesMipBacking(params, GuestIsGen5());

	// A write-only, fully covered dispatch does not observe the guest backing.
	// Leave the image undefined; the compute descriptor bind transitions it
	// directly to GENERAL before the first shader write.
	if (skip_seed)
	{
		return;
	}

	VkImageLayout vk_layout = VK_IMAGE_LAYOUT_GENERAL;

	if (levels >= 16)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: levels >= 16 condition ignored (continuing)\n");
	}
	if (three_dimensional)
	{
		Gen5TextureVolumeLayout volume_layout {};
		const bool              layout_valid = Gen5GetVolumeTextureLayout(
		    static_cast<uint32_t>(fmt), static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth),
		    static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels), static_cast<uint32_t>(tile), &volume_layout);
		if (!layout_valid || !Gen5ValidateTextureVolumeUpload(volume_layout, *size))
		{
			EXIT("unsupported Gen5 storage volume upload: format=%u %ux%ux%u pitch=%u levels=%u tile=%u size=%" PRIu64 "\n",
			     static_cast<uint32_t>(fmt), static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth),
			     static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels), static_cast<uint32_t>(tile), *size);
		}
		std::vector<uint8_t> linear(static_cast<size_t>(volume_layout.linear_size));
		EXIT_IF(!Gen5DetileTextureVolume(linear.data(), linear.size(), reinterpret_cast<const void*>(*vaddr), *size, volume_layout));
		Vector<BufferImageCopy> regions(1);
		regions[0].offset    = 0;
		regions[0].pitch     = volume_layout.pitch;
		regions[0].width     = volume_layout.width;
		regions[0].height    = volume_layout.height;
		regions[0].depth     = volume_layout.depth;
		regions[0].dst_level = 0;
		regions[0].dst_x     = 0;
		regions[0].dst_y     = 0;
		regions[0].dst_z     = 0;
		if (!linear.empty())
		{
			UtilFillImage(ctx, vk_obj, linear.data(), volume_layout.linear_size, regions, static_cast<uint64_t>(vk_layout));
		}
		return;
	}
	if (depth64kb32)
	{
		const bool one_2d_layer =
		    (resource_type == 9u && depth == 1u && base_array == 0u) || (arrayed_2d && depth == 1u && base_array == 0u);
		if (levels > 1u)
		{
			Gen5TextureMipLayout mip_layout {};
			if (resource_type != 9u || depth != 1u || base_array != 0u ||
			    !Gen5GetDepth64KBTextureMipLayout(static_cast<uint32_t>(fmt),
			                                                         static_cast<uint32_t>(width), static_cast<uint32_t>(height),
			                                                         static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels),
			                                                         &mip_layout) || *size != mip_layout.tiled.size)
			{
				EXIT("unsupported Gen5 depth storage mip backing: format=%u levels=%u size=0x%" PRIx64 "\n",
				     static_cast<unsigned>(fmt), static_cast<unsigned>(levels), *size);
			}
			std::vector<uint8_t> linear(static_cast<size_t>(mip_layout.linear_size));
			if (!Gen5DetileDepth64KBTextureMipChain(linear.data(), linear.size(), reinterpret_cast<const void*>(*vaddr),
			                                        *size, mip_layout))
			{
				EXIT("Gen5 depth storage mip detile failed: levels=%u\n", static_cast<unsigned>(levels));
			}
			Vector<BufferImageCopy> regions(static_cast<int>(levels));
			for (uint32_t level = 0u; level < levels; level++)
			{
				const auto& mip        = mip_layout.level[level];
				regions[level].offset    = mip.linear_offset;
				regions[level].pitch     = mip.width;
				regions[level].width     = mip.width;
				regions[level].height    = mip.height;
				regions[level].dst_level = level;
			}
			UtilFillImage(ctx, vk_obj, linear.data(), linear.size(), regions, static_cast<uint64_t>(vk_layout));
			return;
		}
		if (!one_2d_layer || levels != 1u || pitch < width)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !one_2d_layer || levels != 1u || pitch < width condition ignored (continuing)\n");
		}

		TileSizeAlign tiled_size {};
		TileGetTextureSize2(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
		                    static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels), static_cast<uint32_t>(tile), &tiled_size, nullptr,
		                    nullptr);
		const uint64_t linear_bytes = width * height * 4u;
		if (tiled_size.size != *size || linear_bytes == 0u || linear_bytes > *size)
		{
			KYTY_LOG_LIMIT(
			    Log::Level::Warn, 8,
			    "WARNING: tiled_size.size != *size || linear_bytes == 0u || linear_bytes > *size condition ignored (continuing)\n");
		}

		std::vector<uint8_t> linear(static_cast<size_t>(linear_bytes));
		TileConvertDepth64KB32ToLinear(linear.data(), reinterpret_cast<const void*>(*vaddr), static_cast<uint32_t>(width),
		                               static_cast<uint32_t>(height), static_cast<uint32_t>(pitch));

		Vector<BufferImageCopy> regions(1);
		regions[0].offset    = 0;
		regions[0].pitch     = static_cast<uint32_t>(width);
		regions[0].width     = static_cast<uint32_t>(width);
		regions[0].height    = static_cast<uint32_t>(height);
		regions[0].dst_level = 0;
		UtilFillImage(ctx, vk_obj, linear.data(), linear.size(), regions, static_cast<uint64_t>(vk_layout));
		return;
	}
	if (arrayed_2d)
	{
		Gen5TextureArrayLayout array_layout {};
		if (!Gen5GetTextureArrayLayout(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
		                               static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels), static_cast<uint32_t>(tile),
		                               static_cast<uint32_t>(depth), &array_layout))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8,
			               "WARNING: !Gen5GetTextureArrayLayout(static_cast<uint32_t>(fmt), static_cast<uint32_t>(wid condition ignored "
			               "(continuing)\n");
		}
		if (!Gen5ValidateTextureArrayUpload(array_layout, static_cast<uint32_t>(base_array), *size))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8,
			               "WARNING: !Gen5ValidateTextureArrayUpload(array_layout, static_cast<uint32_t>(base_array), *size) condition "
			               "ignored (continuing)\n");
		}

		// A slice holds its whole chain; the image carries as many of those levels as it has.
		const uint32_t upload_levels = std::min(array_layout.levels, vk_obj->mip_levels);
		std::vector<uint8_t> slice(static_cast<size_t>(array_layout.linear_slice_size));
		uint32_t             layer_region_count = 0;
		if (!Gen5FillTextureArrayLayerUploadRegionsForLevels(array_layout, 0u, upload_levels, nullptr, 0u, &layer_region_count) ||
		    layer_region_count == 0u)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8,
			               "WARNING: !Gen5FillTextureArrayLayerUploadRegions(array_layout, 0u, nullptr, 0u, &layer_region_count) "
			               "condition ignored (continuing)\n");
		}
		std::vector<Gen5TextureArrayUploadRegion> upload_regions(static_cast<size_t>(layer_region_count));
		Vector<BufferImageCopy>                   regions(static_cast<int>(layer_region_count));
		for (uint32_t layer = 0; layer < array_layout.layers; ++layer)
		{
			if (!Gen5DetileTextureArrayLayer(slice.data(), slice.size(), reinterpret_cast<const void*>(*vaddr), *size, array_layout,
			                                 layer))
			{
				KYTY_LOG_LIMIT(Log::Level::Warn, 8,
				               "WARNING: !Gen5DetileTextureArrayLayer(...) condition ignored (continuing)\n");
			}
			uint32_t filled = layer_region_count;
			if (!Gen5FillTextureArrayLayerUploadRegionsForLevels(array_layout, layer, upload_levels, upload_regions.data(),
			                                                     layer_region_count, &filled))
			{
				KYTY_LOG_LIMIT(Log::Level::Warn, 8,
				               "WARNING: !Gen5FillTextureArrayLayerUploadRegions(...) condition ignored (continuing)\n");
			}
			for (uint32_t region_index = 0; region_index < layer_region_count; ++region_index)
			{
				const auto& upload                    = upload_regions[region_index];
				regions[region_index].offset          = static_cast<uint32_t>(upload.offset);
				regions[region_index].pitch           = upload.pitch_texels;
				regions[region_index].width           = upload.width;
				regions[region_index].height          = upload.height;
				regions[region_index].dst_level       = upload.dst_level;
				regions[region_index].dst_array_layer = upload.dst_array_layer;
			}
			UtilFillImage(ctx, vk_obj, slice.data(), slice.size(), regions, static_cast<uint64_t>(vk_layout));
		}
		return;
	}

	// Gen5 storage images use the same 2D tile contracts as sampled textures:
	// 0/8 linear, 5 Standard4KB, 9 Standard64KB, 13 TextureTiled (legacy),
	// 27 kRenderTarget SW64k. Reject anything outside that set with values.
	if (tile != 0 && tile != 5 && tile != 8 && tile != 9 && tile != 13 && tile != 27)
	{
		EXIT("unsupported storage texture tile: tile=%" PRIu64 " format=%" PRIu64 " dfmt=%" PRIu64 " nfmt=%" PRIu64 " width=%" PRIu64
		     " height=%" PRIu64 " pitch=%" PRIu64 " levels=%" PRIu64 " type=%" PRIu64 " size=%" PRIu64 "\n",
		     tile, fmt, dfmt, nfmt, width, height, pitch, levels, resource_type, *size);
	}

	TileSizeOffset level_sizes[16];
	TilePaddedSize padded_sizes[16];

	if (fmt != 0)
	{
		TileGetTextureSize2(fmt, width, height, pitch, levels, tile, nullptr, level_sizes, padded_sizes);
	} else
	{
		TileGetTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, nullptr, level_sizes, nullptr);
	}

	// dbg_test_mipmaps(ctx, VK_FORMAT_BC3_SRGB_BLOCK, 512, 512);

	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	uint32_t mip_pitch  = pitch;
	// A GFX10 linear mip chain stores every level with its row pitch aligned to
	// 256 bytes. The halved pitch of the other layouts would read the rows of
	// such a level from the wrong offsets.
	const uint32_t format_bpp = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
	const bool     linear_chain = tile == 0 && levels > 1u && format_bpp != 0u && 256u % format_bpp == 0u;

	// A mip backing has real levels and nothing is packed into an atlas. Linear guest memory holds
	// every level; the tiled layouts have no level layout here, so guest memory seeds level 0 only and
	// the higher levels are produced on the GPU. Other layouts pack every level into one image.
	const uint32_t region_count = (mip_backing && tile != 0) ? 1u : static_cast<uint32_t>(levels);
	Vector<BufferImageCopy> regions(region_count);
	for (uint32_t i = 0; i < region_count; i++)
	{
		if (level_sizes[i].size == 0)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: level_sizes[i].size == 0 condition ignored (continuing)\n");
		}

		auto mipmap_offset = mip_backing ? std::pair<int, int> {0, 0} : UtilCalcMipmapOffset(i, width, height);

		regions[i].offset    = level_sizes[i].offset;
		regions[i].width     = mip_width;
		regions[i].height    = mip_height;
		regions[i].pitch     = linear_chain ? padded_sizes[i].width : mip_pitch;
		regions[i].dst_level = mip_backing ? i : 0;
		regions[i].dst_x     = mipmap_offset.first;
		regions[i].dst_y     = mipmap_offset.second;

		if (mip_width > 1)
		{
			mip_width /= 2;
		}
		if (mip_height > 1)
		{
			mip_height /= 2;
		}
		if (mip_pitch > 1)
		{
			mip_pitch /= 2;
		}
	}

	if (tile == 13)
	{
		// EXIT_NOT_IMPLEMENTED(pitch != width);
		if (fmt != 0)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: fmt != 0 condition ignored (continuing)\n");
		}
		auto* temp_buf = new uint8_t[*size];
		TileConvertTiledToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), TileMode::TextureTiled, dfmt, nfmt, width, height, pitch,
		                         levels, neo);
		UtilFillImage(ctx, vk_obj, temp_buf, *size, regions, static_cast<uint64_t>(vk_layout));
		delete[] temp_buf;
	} else if (tile == 5)
	{
		const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
		if (bytes_per_element == 0u || (levels != 1u && !mip_backing))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: bytes_per_element == 0u || levels != 1u condition ignored (continuing)\n");
		}
		const uint64_t linear_bytes = static_cast<uint64_t>(pitch) * height * bytes_per_element;
		if (linear_bytes == 0u || linear_bytes > *size)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: linear_bytes == 0u || linear_bytes > *size condition ignored (continuing)\n");
		}
		auto* temp_buf = new uint8_t[static_cast<size_t>(linear_bytes)];
		TileConvertStandard4KBToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), width, height, pitch, bytes_per_element);
		regions[0].offset = 0;
		regions[0].pitch  = pitch;
		regions[0].width  = width;
		regions[0].height = height;
		UtilFillImage(ctx, vk_obj, temp_buf, linear_bytes, regions, static_cast<uint64_t>(vk_layout));
		delete[] temp_buf;
	} else if (tile == 9 || tile == 27)
	{
		// Detile Standard64KB / kRenderTarget into pitch-strided linear rows for
		// the host storage image. Multi-mip and block-compressed storage writes
		// are not part of this path yet.
		const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
		if (bytes_per_element == 0u || (levels != 1u && !mip_backing))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: bytes_per_element == 0u || levels != 1u condition ignored (continuing)\n");
		}
		const uint32_t pitch_elems  = (pitch != 0u ? static_cast<uint32_t>(pitch) : static_cast<uint32_t>(width));
		const uint64_t linear_bytes = static_cast<uint64_t>(pitch_elems) * height * bytes_per_element;
		if (linear_bytes == 0u || linear_bytes > *size)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: linear_bytes == 0u || linear_bytes > *size condition ignored (continuing)\n");
		}
		auto* temp_buf = new uint8_t[static_cast<size_t>(linear_bytes)];
		std::memset(temp_buf, 0, static_cast<size_t>(linear_bytes));
		const auto* src = reinterpret_cast<const uint8_t*>(*vaddr);
		for (uint32_t y = 0; y < static_cast<uint32_t>(height); y++)
		{
			for (uint32_t x = 0; x < static_cast<uint32_t>(width); x++)
			{
				const uint64_t tiled  = (tile == 9) ? TileGetStandard64KBOffset(x, y, pitch_elems, bytes_per_element)
				                                    : TileGetSw64kRxOffset(x, y, pitch_elems, bytes_per_element);
				const uint64_t linear = (static_cast<uint64_t>(y) * pitch_elems + x) * bytes_per_element;
				std::memcpy(temp_buf + linear, src + tiled, bytes_per_element);
			}
		}
		regions[0].offset = 0;
		regions[0].pitch  = pitch_elems;
		regions[0].width  = static_cast<uint32_t>(width);
		regions[0].height = static_cast<uint32_t>(height);
		UtilFillImage(ctx, vk_obj, temp_buf, linear_bytes, regions, static_cast<uint64_t>(vk_layout));
		delete[] temp_buf;
	} else if (tile == 0 || tile == 8)
	{
		// Linear general (Gen5 tile 0) and legacy linear (tile 8): guest memory is
		// already pitch-strided host-order rows.
		UtilFillImage(ctx, vk_obj, reinterpret_cast<void*>(*vaddr), *size, regions, static_cast<uint64_t>(vk_layout));
	}
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("StorageTextureObject::Create");

	EXIT_IF(size == nullptr || vaddr == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr_num != 1);

	auto       fmt               = (params[StorageTextureObject::PARAM_FORMAT] >> 16u) & 0xffffu;
	auto       dfmt              = (params[StorageTextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto       nfmt              = (params[StorageTextureObject::PARAM_FORMAT]) & 0xffu;
	auto       width             = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto       height            = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto       base_level        = params[StorageTextureObject::PARAM_LEVELS] >> 32u;
	auto       levels            = params[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto       swizzle           = NormalizeStorageTextureSwizzle(params[StorageTextureObject::PARAM_FORMAT], params[StorageTextureObject::PARAM_SWIZZLE]);
	auto       resource_type     = params[StorageTextureObject::PARAM_RESOURCE_TYPE];
	auto       depth             = params[StorageTextureObject::PARAM_DEPTH];
	auto       base_array        = params[StorageTextureObject::PARAM_BASE_ARRAY];
	auto       pitch             = params[StorageTextureObject::PARAM_PITCH];
	auto       tile              = params[StorageTextureObject::PARAM_TILE];
	const bool three_dimensional = resource_type == 10u;
	const bool arrayed_2d        = resource_type == 13u || resource_type == 11u;
	const uint32_t host_levels    = StorageTextureMipBackingLevels(params, GuestIsGen5());
	const bool     mip_backing    = host_levels != 0u;
	if (three_dimensional)
	{
		Gen5TextureVolumeLayout volume_layout {};
		if (!Gen5GetVolumeTextureLayout(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width),
		                                          static_cast<uint32_t>(height), static_cast<uint32_t>(depth),
		                                          static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels),
		                                          static_cast<uint32_t>(tile), &volume_layout) ||
		    base_level != 0u || !Gen5ValidateTextureVolumeUpload(volume_layout, *size))
		{
			EXIT("unsupported Gen5 storage volume image: format=%u %ux%ux%u pitch=%u levels=%u tile=%u size=%" PRIu64 "\n",
			     static_cast<uint32_t>(fmt), static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth),
			     static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels), static_cast<uint32_t>(tile), *size);
		}
	}
	if (resource_type != 8u && resource_type != 9u && !arrayed_2d && !three_dimensional)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unsupported storage texture resource type (continuing)\n");
	}

	if (base_level != 0u && !mip_backing)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: base_level != 0 condition ignored (continuing)\n");
	}

	VkImageUsageFlags vk_usage = StorageTextureGetImageUsage();

	VkComponentMapping components {};
	EXIT_IF(!VulkanDecodeComponentMapping(static_cast<uint32_t>(swizzle), &components));

	auto pixel_format = VulkanResolveGuestImageFormat(GuestImageUsage::Storage, static_cast<uint8_t>(dfmt), static_cast<uint8_t>(nfmt),
	                                                  static_cast<uint16_t>(fmt));

	if (pixel_format == VK_FORMAT_UNDEFINED)
	{
		EXIT("unsupported storage texture format: format=%" PRIu64 " dfmt=%" PRIu64 " nfmt=%" PRIu64 " tile=%" PRIu64 " width=%" PRIu64
		     " height=%" PRIu64 " pitch=%" PRIu64 " levels=%" PRIu64 " type=%" PRIu64 " depth=%" PRIu64 " base_array=%" PRIu64
		     " swizzle=0x%03" PRIx64 " base=0x%012" PRIx64 " size=%" PRIu64 "\n",
		     fmt, dfmt, nfmt, tile, width, height, pitch, levels, resource_type, depth, base_array, static_cast<uint64_t>(swizzle), *vaddr,
		     *size);
	}
	if (width == 0)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: width == 0 condition ignored (continuing)\n");
	}
	if (height == 0)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: height == 0 condition ignored (continuing)\n");
	}
	if (three_dimensional && depth == 0u)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: three_dimensional && depth == 0u condition ignored (continuing)\n");
	}
	if (arrayed_2d && (depth == 0u || base_array >= depth))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: arrayed_2d && (depth == 0u || base_array >= depth) condition ignored (continuing)\n");
	}

	if (mip_backing && (base_level >= host_levels || levels > VulkanImage::VIEW_STORAGE_MIP_COUNT))
	{
		EXIT("unsupported storage mip view: base=%" PRIu64 " levels=%" PRIu64 " host_levels=%u\n", base_level, levels, host_levels);
	}
	auto real_height = ((levels > 1u && !mip_backing) ? height + (height > 1u ? height / 2u : 1u) : height);

	auto* vk_obj = new StorageTextureVulkanImage;

	VulkanImageDescriptor image_descriptor {};
	image_descriptor.image_type   = three_dimensional ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	image_descriptor.extent       = {static_cast<uint32_t>(width), static_cast<uint32_t>(real_height),
	                                 static_cast<uint32_t>(three_dimensional ? depth : 1u)};
	image_descriptor.array_layers = static_cast<uint32_t>(arrayed_2d ? depth : 1u);
	image_descriptor.mip_levels   = mip_backing ? host_levels : 1u;
	image_descriptor.format       = pixel_format;
	image_descriptor.usage        = vk_usage;
	auto image_info               = VulkanBuildImageCreateInfo(image_descriptor);

	// These canonical views are the storage-write interface. Sampled bindings
	// create/cache separate views from the current T# selectors in PrepareTextures;
	// normalization here must never substitute for that descriptor's read mapping.
	// Storage image views use identity component mapping. Single-component R32
	// resources encode their read result as R,0,0,1 while writes address R only;
	// Normalize that view contract before Vulkan validation.
	// Single-component R16/R16F (formats 7 and 13) share that contract.
	if (IsR32SingleComponentStorageFormat(static_cast<uint32_t>(fmt)) || IsIdentityViewStorageFormat(static_cast<uint32_t>(fmt)))
	{
		components.r = VK_COMPONENT_SWIZZLE_R;
		components.g = VK_COMPONENT_SWIZZLE_G;
		components.b = VK_COMPONENT_SWIZZLE_B;
		components.a = VK_COMPONENT_SWIZZLE_A;
	}

	if (!VulkanNormalizeStorageComponentMapping(&image_info.format, &components))
	{
		EXIT("swizzle is not supported: format=%" PRIu64 " swizzle=0x%03" PRIx64 " decoded=(%d,%d,%d,%d) vkformat=%d\n",
		     fmt, static_cast<uint64_t>(swizzle), static_cast<int>(components.r), static_cast<int>(components.g), static_cast<int>(components.b),
		     static_cast<int>(components.a), static_cast<int>(image_info.format));
	}

	if (!VulkanImageFormatSupported(ctx, image_info))
	{
		EXIT("format is not supported");
	}

	// Guest memory is untyped: a sampled descriptor may read these texels
	// through another format of the same size. Create the image mutable over
	// those formats when the device supports it, so such a sample views the
	// live image instead of guest bytes the device never wrote back.
	VkFormat                    view_formats[24] = {};
	VkImageFormatListCreateInfo format_list {};
	format_list.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
	format_list.viewFormatCount = VulkanColorTexelFormatList(image_info.format, view_formats, 24);
	format_list.pViewFormats    = view_formats;
	bool mutable_format         = false;
	if (format_list.viewFormatCount > 1u && image_info.pNext == nullptr)
	{
		auto mutable_info = image_info;
		mutable_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
		mutable_info.pNext = &format_list;
		if (VulkanImageFormatSupported(ctx, mutable_info))
		{
			image_info     = mutable_info;
			mutable_format = true;
		}
	}

	vk_obj->SetNativeExtent(width, height);
	vk_obj->format     = image_info.format;
	vk_obj->image      = nullptr;
	vk_obj->layout     = image_info.initialLayout;
	vk_obj->array_layers = image_descriptor.array_layers;
	vk_obj->guest_vaddr = *vaddr;
	vk_obj->guest_size = *size;
	vk_obj->mutable_format = mutable_format;
	vk_obj->storage_components = components;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	if (!VulkanCreateDeviceImage(ctx, image_info, vk_obj, mem))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8,
		               "WARNING: !VulkanCreateDeviceImage(ctx, image_info, vk_obj, mem) condition ignored (continuing)\n");
	}

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	VulkanImageViewDescriptor view_descriptor {};
	view_descriptor.image       = vk_obj->image;
	view_descriptor.view_type   = three_dimensional ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	view_descriptor.format      = vk_obj->format;
	view_descriptor.components  = components;
	view_descriptor.level_count = VK_REMAINING_MIP_LEVELS;
	const int view_index        = (three_dimensional ? VulkanImage::VIEW_3D : VulkanImage::VIEW_DEFAULT);
	if (!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[view_index]))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8,
		               "WARNING: !VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[view_index]) condition "
		               "ignored (continuing)\n");
	}
	if (mip_backing && !arrayed_2d)
	{
		view_descriptor.level_count    = 1u;
		for (uint32_t level = 0u; level < host_levels; level++)
		{
			view_descriptor.base_mip_level = level;
			if (!VulkanCreateDeviceImageView(ctx->device, view_descriptor,
			                                 &vk_obj->image_view[VulkanImage::VIEW_STORAGE_MIP_BASE + level]))
			{
				EXIT("failed to create storage mip view: level=%u\n", level);
			}
		}
		view_descriptor.base_mip_level = 0u;
		view_descriptor.level_count    = VK_REMAINING_MIP_LEVELS;
	}
	if (!three_dimensional)
	{
		view_descriptor.view_type        = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		view_descriptor.base_array_layer = 0u;
		view_descriptor.layer_count      = arrayed_2d ? static_cast<uint32_t>(depth) : 1u;
		if (!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_ARRAY]))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8,
			               "WARNING: !VulkanCreateDeviceImageView(ctx->device, view_descriptor, "
				               "&vk_obj->image_view[VulkanImage::VIEW_ARRAY]) condition ignored (continuing)\n");
		}
	}

	return vk_obj;
}

static StorageTextureVulkanImage* FindStorageTextureGrowthSource(const Vector<GpuMemoryObject>& objects,
                                                                 const Gen5TextureArrayLayout&  layout)
{
	StorageTextureVulkanImage* source = nullptr;
	for (const auto& object: objects)
	{
		if (object.type != GpuMemoryObjectType::StorageTexture || object.obj == nullptr)
		{
			continue;
		}
		auto* candidate = static_cast<StorageTextureVulkanImage*>(object.obj);
		if (!candidate->MatchesGuestExtent(layout.width, layout.height) || candidate->guest_size == 0u ||
		    candidate->guest_size >= layout.tiled_size || candidate->guest_size % layout.tiled_slice.size != 0u)
		{
			continue;
		}
		if (source == nullptr || candidate->guest_size > source->guest_size)
		{
			source = candidate;
		}
	}
	return source;
}

static void* create_from_objects_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, GpuMemoryScenario scenario,
                                      const Vector<GpuMemoryObject>& objects, VulkanMemory* mem)
{
	EXIT_IF(ctx == nullptr || buffer == nullptr || params == nullptr || mem == nullptr);
	EXIT_IF(scenario != GpuMemoryScenario::Common);
	const auto format = static_cast<uint32_t>((params[StorageTextureObject::PARAM_FORMAT] >> 16u) & 0xffffu);
	const auto width  = static_cast<uint32_t>(params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u);
	const auto height = static_cast<uint32_t>(params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu);
	const auto pitch  = static_cast<uint32_t>(params[StorageTextureObject::PARAM_PITCH]);
	const auto levels = static_cast<uint32_t>(params[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu);
	const auto tile   = static_cast<uint32_t>(params[StorageTextureObject::PARAM_TILE]);
	const auto layers = static_cast<uint32_t>(params[StorageTextureObject::PARAM_DEPTH]);

	Gen5TextureArrayLayout layout {};
	EXIT_IF(!Gen5GetTextureArrayLayout(format, width, height, pitch, levels, tile, layers, &layout));
	auto* source = FindStorageTextureGrowthSource(objects, layout);
	EXIT_IF(source == nullptr);

	uint64_t guest_address = source->guest_vaddr;
	uint64_t guest_size    = layout.tiled_size;
	auto*    destination   = static_cast<StorageTextureVulkanImage*>(create_func(ctx, params, &guest_address, &guest_size, 1, mem));
	EXIT_IF(destination == nullptr || destination->format != source->format);

	const auto prefix_layers = static_cast<uint32_t>(source->guest_size / layout.tiled_slice.size);
	EXIT_IF(prefix_layers == 0u || prefix_layers >= layout.layers || source->mip_levels != destination->mip_levels);
	const bool volume = params[StorageTextureObject::PARAM_RESOURCE_TYPE] == 10u;

	// Each guest slice holds its whole mip chain, so the prefix layers carry every level.
	Vector<ImageImageCopy> regions(source->mip_levels);
	for (uint32_t level = 0; level < source->mip_levels; ++level)
	{
		auto& region       = regions[level];
		region.src_image   = source;
		region.src_level   = level;
		region.dst_level   = level;
		region.width       = std::max(layout.width >> level, 1u);
		region.height      = std::max(layout.height >> level, 1u);
		region.src_x       = 0;
		region.src_y       = 0;
		region.dst_x       = 0;
		region.dst_y       = 0;
		region.layer_count = volume ? 1u : prefix_layers;
		region.depth       = volume ? std::max(prefix_layers >> level, 1u) : 1u;
	}
	UtilImageToImage(buffer, regions, destination, static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
	return destination;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("StorageTextureObject::delete_func");

	auto* vk_obj = reinterpret_cast<StorageTextureVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);

	DeleteDescriptor(vk_obj);

	for (auto view: vk_obj->image_view)
	{
		if (view != nullptr)
		{
			vkDestroyImageView(ctx->device, view, nullptr);
		}
	}

	vkDestroyImage(ctx->device, vk_obj->image, nullptr);

	VulkanFree(ctx, mem);

	delete vk_obj;
}

bool StorageTextureObject::Equal(const uint64_t* other) const
{
	if (other == nullptr)
	{
		return false;
	}

	// Descriptors that pick different levels of one chain are views of one backing.
	const bool gen5               = GuestIsGen5();
	const bool same_mip_backing   = StorageTextureUsesMipBacking(params, gen5) && StorageTextureUsesMipBacking(other, gen5) &&
	                              (params[PARAM_LEVELS] & 0xffffffffu) == (other[PARAM_LEVELS] & 0xffffffffu);
	const bool same_levels = params[PARAM_LEVELS] == other[PARAM_LEVELS] || same_mip_backing;
	// So do descriptors that pick different layers of one array.
	const bool arrayed    = params[PARAM_RESOURCE_TYPE] == 11u || params[PARAM_RESOURCE_TYPE] == 13u;
	const bool same_layer = params[PARAM_BASE_ARRAY] == other[PARAM_BASE_ARRAY] || arrayed;
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_PITCH] == other[PARAM_PITCH] &&
	        params[PARAM_WIDTH_HEIGHT] == other[PARAM_WIDTH_HEIGHT] && same_levels &&
	        params[PARAM_TILE] == other[PARAM_TILE] && params[PARAM_NEO] == other[PARAM_NEO] &&
	        NormalizeStorageTextureSwizzle(params[PARAM_FORMAT], params[PARAM_SWIZZLE]) ==
	            NormalizeStorageTextureSwizzle(other[PARAM_FORMAT], other[PARAM_SWIZZLE]) &&
	        params[PARAM_RESOURCE_TYPE] == other[PARAM_RESOURCE_TYPE] && params[PARAM_DEPTH] == other[PARAM_DEPTH] &&
	        same_layer && params[PARAM_SKIP_SEED] == other[PARAM_SKIP_SEED]);
}

bool StorageTextureGuestIsGen5()
{
	return GuestIsGen5();
}

bool StorageTextureUsesMipBacking(const uint64_t* params, bool gen5)
{
	if (params == nullptr)
	{
		return false;
	}
	const auto fmt    = static_cast<uint32_t>((params[StorageTextureObject::PARAM_FORMAT] >> 16u) & 0xffffu);
	const auto tile   = params[StorageTextureObject::PARAM_TILE];
	const auto levels = params[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu;
	const auto type   = params[StorageTextureObject::PARAM_RESOURCE_TYPE];
	const bool single = type == 9u && params[StorageTextureObject::PARAM_DEPTH] == 1u &&
	                    params[StorageTextureObject::PARAM_BASE_ARRAY] == 0u;
	const bool arrayed = type == 11u || type == 13u;
	if (levels <= 1u || (!single && !arrayed))
	{
		return false;
	}
	if (arrayed)
	{
		// Each array slice holds its whole chain in the Standard4KB, Standard64KB and
		// render-target tilings (Gen5GetTextureArrayLayout); a linear array has no chain layout.
		return gen5 && (tile == 5u || tile == 9u || tile == 27u);
	}
	const bool depth_chain  = fmt == 22u && tile == 24u;
	// Gen5 colour tilings: linear (0), Standard4KB (5), Standard64KB (9) and render target (27).
	const bool colour_chain = gen5 && (tile == 0u || tile == 5u || tile == 9u || tile == 27u);
	return depth_chain || colour_chain;
}

uint32_t StorageTextureMipBackingLevels(const uint64_t* params, bool gen5)
{
	if (!StorageTextureUsesMipBacking(params, gen5))
	{
		return 0u;
	}
	const auto levels = static_cast<uint32_t>(params[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu);
	if (((params[StorageTextureObject::PARAM_FORMAT] >> 16u) & 0xffffu) == 22u && params[StorageTextureObject::PARAM_TILE] == 24u)
	{
		return levels;
	}
	const auto width  = static_cast<uint32_t>(params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u);
	const auto height = static_cast<uint32_t>(params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu);
	uint32_t   longest = std::max(width, height);
	uint32_t   full    = 1u;
	while (longest > 1u)
	{
		longest >>= 1u;
		++full;
	}
	return std::min(full, static_cast<uint32_t>(VulkanImage::VIEW_STORAGE_MIP_COUNT));
}

bool StorageTextureRedescribesRange(const uint64_t* existing, const uint64_t* incoming, bool compare_layout)
{
	return existing[StorageTextureObject::PARAM_FORMAT] != incoming[StorageTextureObject::PARAM_FORMAT] ||
	       existing[StorageTextureObject::PARAM_TILE] != incoming[StorageTextureObject::PARAM_TILE] ||
	       (compare_layout && (existing[StorageTextureObject::PARAM_WIDTH_HEIGHT] != incoming[StorageTextureObject::PARAM_WIDTH_HEIGHT] ||
	                           existing[StorageTextureObject::PARAM_PITCH] != incoming[StorageTextureObject::PARAM_PITCH]));
}

bool StorageTextureViewsMipTail(const uint64_t* chain, uint64_t chain_address, const uint64_t* view, uint64_t view_address,
                                uint64_t view_size)
{
	const auto chain_levels = static_cast<uint32_t>(chain[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu);
	if (chain[StorageTextureObject::PARAM_TILE] != 9u || view[StorageTextureObject::PARAM_TILE] != 9u ||
	    chain[StorageTextureObject::PARAM_FORMAT] != view[StorageTextureObject::PARAM_FORMAT] ||
	    (view[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu) != 1u || view[StorageTextureObject::PARAM_RESOURCE_TYPE] != 9u ||
	    view[StorageTextureObject::PARAM_DEPTH] != 1u || !StorageTextureUsesMipBacking(chain, true))
	{
		return false;
	}
	Gen5TextureMipLayout layout {};
	const auto           fmt = static_cast<uint32_t>(chain[StorageTextureObject::PARAM_FORMAT] >> 16u);
	if (!Gen5GetStandard64KBTextureMipLayout(fmt, static_cast<uint32_t>(chain[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u),
	                                         static_cast<uint32_t>(chain[StorageTextureObject::PARAM_WIDTH_HEIGHT]),
	                                         static_cast<uint32_t>(chain[StorageTextureObject::PARAM_PITCH]), chain_levels, &layout) ||
	    layout.first_tail_level >= layout.levels)
	{
		return false;
	}
	const auto& tail = layout.level[layout.first_tail_level];
	return view_address == chain_address + tail.tiled_offset && view_size == tail.tiled_size;
}

bool StorageTextureRedescribesRenderTarget(const uint64_t* render, const uint64_t* storage, bool compare_layout)
{
	const auto render_format = static_cast<VkFormat>(
	    VulkanResolveRenderTextureFormat(static_cast<RenderTextureFormat>(render[RenderTextureObject::PARAM_FORMAT])));
	const uint32_t storage_bytes =
	    ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(storage[StorageTextureObject::PARAM_FORMAT] >> 16u));
	const bool render_tiled  = render[RenderTextureObject::PARAM_TILED] != 0u;
	const bool storage_tiled = storage[StorageTextureObject::PARAM_TILE] != 0u;
	// Gen5 colour targets bind only the 64 KiB R_X swizzle when tiled, so a storage view in
	// any other swizzle (a depth-layout view, say) addresses the bytes differently.
	const bool swizzle_differs = Config::IsNextGen() && render_tiled && storage[StorageTextureObject::PARAM_TILE] != 27u;
	return VulkanColorTexelBytes(render_format) != storage_bytes || render_tiled != storage_tiled || swizzle_differs ||
	       (compare_layout && (render[RenderTextureObject::PARAM_WIDTH] != (storage[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u) ||
	                           render[RenderTextureObject::PARAM_HEIGHT] != (storage[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu) ||
	                           render[RenderTextureObject::PARAM_PITCH] != storage[StorageTextureObject::PARAM_PITCH]));
}

bool StorageTextureRedescribesDepthTarget(const uint64_t* storage, bool next_gen)
{
	return next_gen && storage != nullptr && storage[StorageTextureObject::PARAM_TILE] != 24u;
}

bool StorageTextureSharesDepthTexels(const uint64_t* storage, const uint64_t* depth, bool next_gen)
{
	if (!next_gen || storage == nullptr || depth == nullptr)
	{
		return false;
	}
	const auto width  = depth[DepthStencilBufferObject::PARAM_GUEST_WIDTH];
	const auto height = depth[DepthStencilBufferObject::PARAM_GUEST_HEIGHT];
	if (depth[DepthStencilBufferObject::PARAM_FORMAT] != VK_FORMAT_D32_SFLOAT || depth[DepthStencilBufferObject::PARAM_HTILE] != 0u ||
	    depth[DepthStencilBufferObject::PARAM_SAMPLES] != 1u || width == 0u || width > UINT32_MAX ||
	    depth[DepthStencilBufferObject::PARAM_HOST_WIDTH] != width || depth[DepthStencilBufferObject::PARAM_HOST_HEIGHT] != height)
	{
		return false;
	}
	const auto packed = storage[StorageTextureObject::PARAM_FORMAT];
	const auto tile   = storage[StorageTextureObject::PARAM_TILE];
	const auto format = VulkanResolveGuestImageFormat(GuestImageUsage::Storage, static_cast<uint8_t>(packed >> 8u),
	                                                  static_cast<uint8_t>(packed), static_cast<uint16_t>(packed >> 16u));
	return (format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) &&
	       tile == 24u && storage[StorageTextureObject::PARAM_LEVELS] == 1u &&
	       storage[StorageTextureObject::PARAM_RESOURCE_TYPE] == 9u && storage[StorageTextureObject::PARAM_DEPTH] == 1u &&
	       storage[StorageTextureObject::PARAM_BASE_ARRAY] == 0u &&
	       storage[StorageTextureObject::PARAM_WIDTH_HEIGHT] == ((width << 32u) | height) &&
	       storage[StorageTextureObject::PARAM_PITCH] == TileAlign64KBPitch(static_cast<uint32_t>(width), 4u);
}

bool StorageTextureCanCopyGrowingBacking(const uint64_t* existing, const uint64_t* incoming)
{
	if (existing == nullptr || incoming == nullptr)
	{
		return false;
	}
	const auto existing_type = existing[StorageTextureObject::PARAM_RESOURCE_TYPE];
	const auto incoming_type = incoming[StorageTextureObject::PARAM_RESOURCE_TYPE];
	const bool arrayed_2d    = incoming_type == 11u || incoming_type == 13u;
	const bool volume        = incoming_type == 10u;
	if ((!arrayed_2d && !volume) || existing_type != incoming_type)
	{
		return false;
	}

	const auto existing_depth = existing[StorageTextureObject::PARAM_DEPTH];
	const auto incoming_depth = incoming[StorageTextureObject::PARAM_DEPTH];
	const auto incoming_base  = incoming[StorageTextureObject::PARAM_BASE_ARRAY];
	if (existing_depth == 0u || incoming_depth <= existing_depth ||
	    (volume ? incoming_base != 0u : incoming_base > existing_depth))
	{
		return false;
	}
	if (volume && incoming[StorageTextureObject::PARAM_LEVELS] != 1u)
	{
		return false;
	}

	if (existing[StorageTextureObject::PARAM_FORMAT] != incoming[StorageTextureObject::PARAM_FORMAT] ||
	    existing[StorageTextureObject::PARAM_PITCH] != incoming[StorageTextureObject::PARAM_PITCH] ||
	    existing[StorageTextureObject::PARAM_WIDTH_HEIGHT] != incoming[StorageTextureObject::PARAM_WIDTH_HEIGHT] ||
	    existing[StorageTextureObject::PARAM_LEVELS] != incoming[StorageTextureObject::PARAM_LEVELS] ||
	    existing[StorageTextureObject::PARAM_TILE] != incoming[StorageTextureObject::PARAM_TILE] ||
	    existing[StorageTextureObject::PARAM_NEO] != incoming[StorageTextureObject::PARAM_NEO])
	{
		return false;
	}
	return NormalizeStorageTextureSwizzle(existing[StorageTextureObject::PARAM_FORMAT], existing[StorageTextureObject::PARAM_SWIZZLE]) ==
	       NormalizeStorageTextureSwizzle(incoming[StorageTextureObject::PARAM_FORMAT], incoming[StorageTextureObject::PARAM_SWIZZLE]);
}

bool StorageTextureCanReuseLargerBacking(const uint64_t* existing, const uint64_t* incoming)
{
	if (existing == nullptr || incoming == nullptr)
	{
		return false;
	}
	const auto type = incoming[StorageTextureObject::PARAM_RESOURCE_TYPE];
	if ((type != 11u && type != 13u) || existing[StorageTextureObject::PARAM_RESOURCE_TYPE] != type)
	{
		return false;
	}
	const auto existing_depth = existing[StorageTextureObject::PARAM_DEPTH];
	const auto incoming_depth = incoming[StorageTextureObject::PARAM_DEPTH];
	if (incoming_depth == 0u || incoming_depth > existing_depth ||
	    incoming[StorageTextureObject::PARAM_BASE_ARRAY] >= incoming_depth)
	{
		return false;
	}
	// The same chain in every slice: base_level only selects a view.
	if (existing[StorageTextureObject::PARAM_FORMAT] != incoming[StorageTextureObject::PARAM_FORMAT] ||
	    existing[StorageTextureObject::PARAM_PITCH] != incoming[StorageTextureObject::PARAM_PITCH] ||
	    existing[StorageTextureObject::PARAM_WIDTH_HEIGHT] != incoming[StorageTextureObject::PARAM_WIDTH_HEIGHT] ||
	    (existing[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu) != (incoming[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu) ||
	    existing[StorageTextureObject::PARAM_TILE] != incoming[StorageTextureObject::PARAM_TILE] ||
	    existing[StorageTextureObject::PARAM_NEO] != incoming[StorageTextureObject::PARAM_NEO])
	{
		return false;
	}
	return NormalizeStorageTextureSwizzle(existing[StorageTextureObject::PARAM_FORMAT], existing[StorageTextureObject::PARAM_SWIZZLE]) ==
	       NormalizeStorageTextureSwizzle(incoming[StorageTextureObject::PARAM_FORMAT], incoming[StorageTextureObject::PARAM_SWIZZLE]);
}

int StorageTextureResolveArrayStorageView(VkDevice device, StorageTextureVulkanImage* image, uint32_t depth, uint32_t base_array,
                                          uint32_t base_level, VulkanImageViewCreator create)
{
	StorageTextureArrayViewRange sampled;
	StorageTextureArrayViewRange storage;
	if (image == nullptr || !StorageTextureGetArrayViewRanges(depth, base_array, &sampled, &storage) ||
	    base_level >= image->mip_levels || storage.base_array_layer >= image->array_layers ||
	    storage.layer_count > image->array_layers - storage.base_array_layer)
	{
		return -1;
	}
	VulkanImageViewDescriptor descriptor {};
	descriptor.image            = image->image;
	descriptor.view_type        = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	descriptor.format           = image->format;
	descriptor.components       = image->storage_components;
	descriptor.base_mip_level   = base_level;
	descriptor.level_count      = 1u;
	descriptor.base_array_layer = storage.base_array_layer;
	descriptor.layer_count      = storage.layer_count;
	return VulkanGetOrCreateCachedImageView(device, image, descriptor, create);
}

// A render-target alias copies texel bytes unchanged. Equal formats alias, and so
// do the 8-bit four-channel formats, whose texels store the guest bytes in order
// whatever their channel order or sRGB encoding.
static bool RenderAliasFormatsMatch(VkFormat render_format, VkFormat storage_format)
{
	const auto rgba8 = [](VkFormat format)
	{
		return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM ||
		       format == VK_FORMAT_B8G8R8A8_SRGB;
	};
	return render_format != VK_FORMAT_UNDEFINED &&
	       (render_format == storage_format || (rgba8(render_format) && rgba8(storage_format)));
}

static uint32_t RenderAliasBytesPerElement(const uint64_t* render_params, const uint64_t* storage_params)
{
	if (render_params == nullptr || storage_params == nullptr)
	{
		return 0u;
	}
	if (render_params[RenderTextureObject::PARAM_TILED] != 1u ||
	    render_params[RenderTextureObject::PARAM_WRITE_BACK] != 0u ||
	    render_params[RenderTextureObject::PARAM_SAMPLES] != 1u ||
	    render_params[RenderTextureObject::PARAM_ARRAY_LAYERS] != 1u ||
	    render_params[RenderTextureObject::PARAM_NEO] != storage_params[StorageTextureObject::PARAM_NEO] ||
	    storage_params[StorageTextureObject::PARAM_TILE] != 27u ||
	    storage_params[StorageTextureObject::PARAM_LEVELS] != 1u ||
	    storage_params[StorageTextureObject::PARAM_DEPTH] != 1u ||
	    storage_params[StorageTextureObject::PARAM_BASE_ARRAY] != 0u ||
	    storage_params[StorageTextureObject::PARAM_SKIP_SEED] != 0u ||
	    (storage_params[StorageTextureObject::PARAM_RESOURCE_TYPE] != 8u &&
	     storage_params[StorageTextureObject::PARAM_RESOURCE_TYPE] != 9u))
	{
		return 0u;
	}

	const uint32_t guest_format = static_cast<uint32_t>(storage_params[StorageTextureObject::PARAM_FORMAT] >> 16u);
	const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(guest_format);
	if (bytes_per_element != 4u && bytes_per_element != 8u)
	{
		return 0u;
	}
	// The storage image format as created: a BGRA selection becomes a BGRA8 image.
	auto storage_format = VulkanResolveGuestImageFormat(
	    GuestImageUsage::Storage, static_cast<uint8_t>(storage_params[StorageTextureObject::PARAM_FORMAT] >> 8u),
	    static_cast<uint8_t>(storage_params[StorageTextureObject::PARAM_FORMAT]), static_cast<uint16_t>(guest_format));
	VkComponentMapping components {};
	if (storage_format == VK_FORMAT_UNDEFINED ||
	    !VulkanDecodeComponentMapping(NormalizeStorageTextureSwizzle(storage_params[StorageTextureObject::PARAM_FORMAT],
	                                                                 storage_params[StorageTextureObject::PARAM_SWIZZLE]),
	                                  &components) ||
	    !VulkanNormalizeStorageComponentMapping(&storage_format, &components))
	{
		return 0u;
	}
	const auto render_format = static_cast<VkFormat>(VulkanResolveRenderTextureFormat(
	    static_cast<RenderTextureFormat>(render_params[RenderTextureObject::PARAM_FORMAT])));
	return RenderAliasFormatsMatch(render_format, storage_format) ? bytes_per_element : 0u;
}

struct RenderAliasLayout
{
	uint64_t address  = 0;
	uint64_t size     = 0;
	uint64_t width    = 0;
	uint64_t height   = 0;
	uint64_t blocks_x = 0;
};

static bool DescribeRenderAliasLayout(uint64_t address, uint64_t size, uint64_t width, uint64_t height, uint64_t pitch,
                                      uint32_t bytes_per_element, RenderAliasLayout* layout)
{
	constexpr uint64_t block_bytes = 65536u;
	const uint64_t block_width = TileGet64KBBlockWidth(bytes_per_element);
	const uint64_t block_height = block_bytes / (block_width * bytes_per_element);
	if (layout == nullptr || width == 0u || height == 0u || width > UINT32_MAX || height > UINT32_MAX || pitch > UINT32_MAX ||
	    pitch != TileAlign64KBPitch(static_cast<uint32_t>(width), bytes_per_element))
	{
		return false;
	}
	const uint64_t blocks_x = pitch / block_width;
	const uint64_t blocks_y = (height + block_height - 1u) / block_height;
	if (blocks_x > UINT64_MAX / blocks_y / block_bytes || address % block_bytes != 0u || size > UINT64_MAX - address ||
	    size != blocks_x * blocks_y * block_bytes)
	{
		return false;
	}
	*layout = {address, size, width, height, blocks_x};
	return true;
}

// A copy that continues the previous one along the same rows extends it.
static void AppendRenderAliasCopy(Vector<StorageTextureRenderAliasCopy>* plan, const StorageTextureRenderAliasCopy& copy)
{
	if (!plan->IsEmpty())
	{
		auto& last = (*plan)[plan->Size() - 1];
		if (last.source_y == copy.source_y && last.destination_y == copy.destination_y && last.source_x + last.width == copy.source_x &&
		    last.destination_x + last.width == copy.destination_x)
		{
			last.width += copy.width;
			return;
		}
	}
	plan->Add(copy);
}

bool StorageTexturePlanRenderAlias(const uint64_t* render_params, uint64_t render_address, uint64_t render_size,
                                   const uint64_t* storage_params, uint64_t storage_address, uint64_t storage_size,
                                   Vector<StorageTextureRenderAliasCopy>* copies)
{
	if (copies == nullptr)
	{
		return false;
	}
	copies->Clear();
	const uint32_t bytes_per_element = RenderAliasBytesPerElement(render_params, storage_params);
	if (bytes_per_element == 0u)
	{
		return false;
	}
	const uint64_t block_bytes = 65536u;
	const uint64_t block_width = TileGet64KBBlockWidth(bytes_per_element);
	const uint64_t block_height = block_bytes / (block_width * bytes_per_element);
	RenderAliasLayout render {};
	RenderAliasLayout storage {};
	if (!DescribeRenderAliasLayout(render_address, render_size, render_params[RenderTextureObject::PARAM_WIDTH],
	                               render_params[RenderTextureObject::PARAM_HEIGHT], render_params[RenderTextureObject::PARAM_PITCH],
	                               bytes_per_element, &render) ||
	    !DescribeRenderAliasLayout(storage_address, storage_size,
	                               storage_params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u,
	                               storage_params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu,
	                               storage_params[StorageTextureObject::PARAM_PITCH], bytes_per_element, &storage))
	{
		return false;
	}

	const uint64_t overlap_start = std::max(render.address, storage.address);
	const uint64_t overlap_end = std::min(render.address + render.size, storage.address + storage.size);
	if (overlap_start >= overlap_end || (overlap_end - overlap_start) % block_bytes != 0u)
	{
		return false;
	}
	Vector<StorageTextureRenderAliasCopy> plan;
	for (uint64_t address = overlap_start; address < overlap_end; address += block_bytes)
	{
		const uint64_t render_index = (address - render.address) / block_bytes;
		const uint64_t storage_index = (address - storage.address) / block_bytes;
		const uint64_t source_x = (render_index % render.blocks_x) * block_width;
		const uint64_t source_y = (render_index / render.blocks_x) * block_height;
		const uint64_t destination_x = (storage_index % storage.blocks_x) * block_width;
		const uint64_t destination_y = (storage_index / storage.blocks_x) * block_height;
		const uint64_t height = std::min({block_height, render.height - std::min(source_y, render.height),
		                                  storage.height - std::min(destination_y, storage.height)});
		// The 8-byte pattern folds y6, above its 64-row block, into bit 11 with x5: a block
		// that sits on rows of opposite parity in the two images holds its 32-column strips swapped.
		const bool     swap_strips = bytes_per_element == 8u && ((source_y ^ destination_y) & block_height) != 0u;
		const uint64_t strip_width = swap_strips ? 32u : block_width;
		for (uint64_t strip = 0; strip < block_width; strip += strip_width)
		{
			const uint64_t strip_source_x      = source_x + strip;
			const uint64_t strip_destination_x = destination_x + (swap_strips ? (strip ^ 32u) : strip);
			// An edge block holds texels past an image's extent; copy only those both images cover.
			const uint64_t width = std::min({strip_width, render.width - std::min(strip_source_x, render.width),
			                                 storage.width - std::min(strip_destination_x, storage.width)});
			if (width == 0u || height == 0u)
			{
				continue;
			}
			AppendRenderAliasCopy(&plan, {static_cast<uint32_t>(strip_source_x), static_cast<uint32_t>(source_y),
			                              static_cast<uint32_t>(strip_destination_x), static_cast<uint32_t>(destination_y),
			                              static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
		}
	}
	for (const auto& copy: plan)
	{
		copies->Add(copy);
	}
	return !copies->IsEmpty();
}

bool StorageTexturePlanRawRenderAlias(const uint64_t* render_params, uint64_t render_address, uint64_t render_size,
                                      const uint64_t* storage_params, uint64_t storage_address, uint64_t storage_size,
                                      StorageTextureRawRenderAliasPlan* plan)
{
	if (render_params == nullptr || storage_params == nullptr || plan == nullptr)
	{
		return false;
	}
	*plan = {};
	const uint32_t storage_format = static_cast<uint32_t>(storage_params[StorageTextureObject::PARAM_FORMAT] >> 16u);
	if (static_cast<RenderTextureFormat>(render_params[RenderTextureObject::PARAM_FORMAT]) !=
	        RenderTextureFormat::B10G11R11Ufloat ||
	    VulkanResolveGuestImageFormat(GuestImageUsage::Storage,
	                                  static_cast<uint8_t>(storage_params[StorageTextureObject::PARAM_FORMAT] >> 8u),
	                                  static_cast<uint8_t>(storage_params[StorageTextureObject::PARAM_FORMAT]),
	                                  static_cast<uint16_t>(storage_format)) != VK_FORMAT_R16_SFLOAT ||
	    render_params[RenderTextureObject::PARAM_TILED] != 1u ||
	    render_params[RenderTextureObject::PARAM_WRITE_BACK] != 0u ||
	    render_params[RenderTextureObject::PARAM_SAMPLES] != 1u ||
	    render_params[RenderTextureObject::PARAM_ARRAY_LAYERS] != 1u ||
	    render_params[RenderTextureObject::PARAM_NEO] != storage_params[StorageTextureObject::PARAM_NEO] ||
	    storage_params[StorageTextureObject::PARAM_TILE] != 27u ||
	    storage_params[StorageTextureObject::PARAM_LEVELS] != 1u ||
	    storage_params[StorageTextureObject::PARAM_DEPTH] != 1u ||
	    storage_params[StorageTextureObject::PARAM_BASE_ARRAY] != 0u ||
	    storage_params[StorageTextureObject::PARAM_SKIP_SEED] != 0u ||
	    (storage_params[StorageTextureObject::PARAM_RESOURCE_TYPE] != 8u &&
	     storage_params[StorageTextureObject::PARAM_RESOURCE_TYPE] != 9u) ||
	    NormalizeStorageTextureSwizzle(storage_params[StorageTextureObject::PARAM_FORMAT], storage_params[StorageTextureObject::PARAM_SWIZZLE]) !=
	        DstSel(4, 5, 6, 7))
	{
		return false;
	}
	RenderAliasLayout render {};
	RenderAliasLayout storage {};
	if (!DescribeRenderAliasLayout(render_address, render_size, render_params[RenderTextureObject::PARAM_WIDTH],
	                               render_params[RenderTextureObject::PARAM_HEIGHT],
	                               render_params[RenderTextureObject::PARAM_PITCH], 4u, &render) ||
	    !DescribeRenderAliasLayout(storage_address, storage_size,
	                               storage_params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u,
	                               storage_params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu,
	                               storage_params[StorageTextureObject::PARAM_PITCH], 2u, &storage))
	{
		return false;
	}
	constexpr uint64_t block_bytes = 65536u;
	const uint64_t overlap_start = std::max(render.address, storage.address);
	const uint64_t overlap_end = std::min(render.address + render.size, storage.address + storage.size);
	if (overlap_start >= overlap_end || render.blocks_x > UINT32_MAX || storage.blocks_x > UINT32_MAX)
	{
		return false;
	}
	const uint64_t source_first = (overlap_start - render.address) / block_bytes;
	const uint64_t destination_first = (overlap_start - storage.address) / block_bytes;
	const uint64_t count = (overlap_end - overlap_start) / block_bytes;
	if (source_first > UINT32_MAX || destination_first > UINT32_MAX || count > UINT32_MAX)
	{
		return false;
	}
	for (uint64_t index = source_first; index < source_first + count; ++index)
	{
		const uint64_t x = (index % render.blocks_x) * 128u;
		const uint64_t y = (index / render.blocks_x) * 128u;
		if (x + 128u > render.width || y + 128u > render.height)
		{
			return false;
		}
	}
	*plan = {static_cast<uint32_t>(source_first), static_cast<uint32_t>(destination_first),
	         static_cast<uint32_t>(count), static_cast<uint32_t>(render.blocks_x),
	         static_cast<uint32_t>(storage.blocks_x)};
	return count != 0u;
}

// One 64 KB R_X surface, single level and layer, whose blocks exactly fill the
// guest range.
static bool DescribeRaw64KBSource(uint64_t width, uint64_t height, uint64_t pitch, uint32_t bytes_per_pixel, uint64_t address,
                                  uint64_t size, StorageTextureRawRenderSource* source)
{
	const uint64_t block_width = TileGet64KBBlockWidth(bytes_per_pixel);
	const uint64_t block_height = bytes_per_pixel == 8u ? 64u : 128u;
	if (width == 0u || height == 0u || width > UINT32_MAX || height > UINT32_MAX ||
	    pitch != TileAlign64KBPitch(static_cast<uint32_t>(width), bytes_per_pixel) ||
	    pitch > UINT32_MAX || address > UINT64_MAX - size)
	{
		return false;
	}
	const uint64_t blocks_x = pitch / block_width;
	const uint64_t blocks_y = (height + block_height - 1u) / block_height;
	if (blocks_x == 0u || blocks_y == 0u || blocks_x > UINT32_MAX || blocks_y > UINT32_MAX ||
	    blocks_x > UINT64_MAX / blocks_y / 65536u || blocks_x * blocks_y * 65536u != size)
	{
		return false;
	}
	*source = {nullptr, address, size, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
	           static_cast<uint32_t>(pitch), bytes_per_pixel};
	return true;
}

bool StorageTextureDescribeRawRenderSource(const uint64_t* params, uint64_t address, uint64_t size,
                                           StorageTextureRawRenderSource* source)
{
	if (params == nullptr || source == nullptr || address % 65536u != 0u || size == 0u || size > INT32_MAX)
	{
		return false;
	}
	*source = {};
	uint32_t bytes_per_pixel = 0u;
	switch (static_cast<RenderTextureFormat>(params[RenderTextureObject::PARAM_FORMAT]))
	{
		case RenderTextureFormat::R8G8Unorm: bytes_per_pixel = 2u; break;
		case RenderTextureFormat::R8G8B8A8Unorm: bytes_per_pixel = 4u; break;
		case RenderTextureFormat::R16G16B16A16Sfloat: bytes_per_pixel = 8u; break;
		default: return false;
	}
	if (params[RenderTextureObject::PARAM_TILED] != 1u || params[RenderTextureObject::PARAM_WRITE_BACK] != 0u ||
	    params[RenderTextureObject::PARAM_SAMPLES] != 1u || params[RenderTextureObject::PARAM_ARRAY_LAYERS] != 1u)
	{
		return false;
	}
	return DescribeRaw64KBSource(params[RenderTextureObject::PARAM_WIDTH], params[RenderTextureObject::PARAM_HEIGHT],
	                             params[RenderTextureObject::PARAM_PITCH], bytes_per_pixel, address, size, source);
}

bool StorageTextureDescribeRawStorageSource(const uint64_t* params, uint64_t address, uint64_t size,
                                            StorageTextureRawRenderSource* source)
{
	if (params == nullptr || source == nullptr || address % 65536u != 0u || size == 0u || size > INT32_MAX)
	{
		return false;
	}
	*source = {};
	uint32_t bytes_per_pixel = 0u;
	switch (VulkanResolveGuestImageFormat(GuestImageUsage::Storage,
	                                      static_cast<uint8_t>(params[StorageTextureObject::PARAM_FORMAT] >> 8u),
	                                      static_cast<uint8_t>(params[StorageTextureObject::PARAM_FORMAT]),
	                                      static_cast<uint16_t>(params[StorageTextureObject::PARAM_FORMAT] >> 16u)))
	{
		case VK_FORMAT_R8G8_UNORM: bytes_per_pixel = 2u; break;
		case VK_FORMAT_R8G8B8A8_UNORM: bytes_per_pixel = 4u; break;
		case VK_FORMAT_R16G16B16A16_SFLOAT: bytes_per_pixel = 8u; break;
		default: return false;
	}
	if (params[StorageTextureObject::PARAM_TILE] != 27u || params[StorageTextureObject::PARAM_LEVELS] != 1u ||
	    params[StorageTextureObject::PARAM_RESOURCE_TYPE] != 9u || params[StorageTextureObject::PARAM_DEPTH] != 1u ||
	    params[StorageTextureObject::PARAM_BASE_ARRAY] != 0u ||
	    NormalizeStorageTextureSwizzle(params[StorageTextureObject::PARAM_FORMAT], params[StorageTextureObject::PARAM_SWIZZLE]) !=
	        DstSel(4, 5, 6, 7))
	{
		return false;
	}
	return DescribeRaw64KBSource(params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u,
	                             params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu,
	                             params[StorageTextureObject::PARAM_PITCH], bytes_per_pixel, address, size, source);
}

bool StorageTextureRawRenderSourceCovers(const StorageTextureRawRenderSource& source, uint64_t address, uint64_t size)
{
	if (size == 0u || address < source.guest_address || source.guest_address > UINT64_MAX - source.guest_size ||
	    address > UINT64_MAX - size || address + size > source.guest_address + source.guest_size ||
	    (source.bytes_per_pixel != 2u && source.bytes_per_pixel != 4u && source.bytes_per_pixel != 8u) || source.pitch == 0u)
	{
		return false;
	}
	const uint64_t block_width  = TileGet64KBBlockWidth(source.bytes_per_pixel);
	const uint64_t block_height = source.bytes_per_pixel == 8u ? 64u : 128u;
	if (block_width == 0u || source.pitch < block_width || source.pitch % block_width != 0u)
	{
		return false;
	}
	const uint64_t blocks_x     = source.pitch / block_width;
	const uint64_t first_block  = (address - source.guest_address) / 65536u;
	const uint64_t last_block   = (address + size - 1u - source.guest_address) / 65536u;
	for (uint64_t block = first_block; block <= last_block; ++block)
	{
		const uint64_t x = (block % blocks_x) * block_width;
		const uint64_t y = (block / blocks_x) * block_height;
		if (x + block_width > source.width || y + block_height > source.height)
		{
			return false;
		}
	}
	return true;
}

// A linear single-level view is a raw byte window over the guest range: the
// copy reproduces the writer's bytes at the same addresses, and the image
// reads them row by row. The view names the row stride (its pitch must be the
// width), the texel size and the identity swizzle; the range may start inside
// a block and may extend past the image.
bool StorageTextureCanCompositeRawRenderDestination(const uint64_t* params, uint64_t address, uint64_t size)
{
	if (params == nullptr || size == 0u || size > INT32_MAX ||
	    params[StorageTextureObject::PARAM_TILE] != 0u || params[StorageTextureObject::PARAM_LEVELS] != 1u ||
	    params[StorageTextureObject::PARAM_RESOURCE_TYPE] != 9u || params[StorageTextureObject::PARAM_DEPTH] != 1u ||
	    params[StorageTextureObject::PARAM_BASE_ARRAY] != 0u || params[StorageTextureObject::PARAM_SKIP_SEED] != 0u)
	{
		return false;
	}
	const uint64_t width  = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	const uint64_t height = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	const uint32_t bpp    = ShaderGen5TextureBytesPerElement(
        static_cast<uint32_t>((params[StorageTextureObject::PARAM_FORMAT] >> 16u) & 0xffffu));
	if (width == 0u || height == 0u || bpp == 0u || bpp > 8u || width > UINT32_MAX / bpp ||
	    height > UINT32_MAX / (width * bpp) || params[StorageTextureObject::PARAM_PITCH] != width ||
	    size < width * height * bpp ||
	    NormalizeStorageTextureSwizzle(params[StorageTextureObject::PARAM_FORMAT], params[StorageTextureObject::PARAM_SWIZZLE]) !=
	        DstSel(4, 5, 6, 7))
	{
		return false;
	}
	return true;
}

bool StorageTexturePlanLinearMipUpload(const uint64_t* params, uint64_t size, bool mip_backing,
                                       Vector<BufferImageCopy>* regions, uint32_t* image_width,
                                       uint32_t* image_height, uint32_t* bytes_per_element)
{
	if (params == nullptr || regions == nullptr || image_width == nullptr || image_height == nullptr ||
	    bytes_per_element == nullptr)
	{
		return false;
	}
	regions->Clear();
	*image_width       = 0;
	*image_height      = 0;
	*bytes_per_element = 0;
	const uint64_t fmt    = (params[StorageTextureObject::PARAM_FORMAT] >> 16u) & 0xffffu;
	const uint64_t width  = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	const uint64_t height = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	const uint64_t levels = params[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu;
	const uint64_t pitch  = params[StorageTextureObject::PARAM_PITCH];
	// The linear mip chain contract only covers explicit Gen5 formats with a
	// byte pitch that aligns a level row, and a view whose rows are exactly as
	// wide as the descriptor.
	if (fmt == 0u || ShaderGen5TextureIsBlockCompressed(static_cast<uint32_t>(fmt)) || width == 0u || height == 0u ||
	    width > UINT32_MAX || height > UINT32_MAX || levels < 2u || levels > 16u || pitch != width)
	{
		return false;
	}
	const uint32_t bpp = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
	if (bpp == 0u || 256u % bpp != 0u || size == 0u || size > INT32_MAX)
	{
		return false;
	}
	// GFX10 linear mip chains (addrlib HwlComputeSurfaceInfoLinear): each
	// level's element pitch is aligned to 256 bytes and the levels are stored
	// from the smallest to level 0. TileGetTextureSize2 must report exactly
	// that chain; anything else (a table entry or another layout) is not this
	// destination shape.
	TileSizeOffset chain_sizes[16] {};
	TileSizeAlign  reported {};
	TileGetTextureSize2(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
	                    static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels), 0u, &reported, chain_sizes, nullptr);
	const uint64_t pitch_align = 256u / bpp;
	uint64_t       chain_total = 0;
	uint64_t       offsets[16] {};
	uint64_t       row_pitches[16] {};
	uint64_t       level_sizes[16] {};
	for (uint32_t level = levels; level-- > 0u;)
	{
		const uint64_t mip_width  = std::max<uint64_t>(width >> level, 1u);
		const uint64_t mip_height = std::max<uint64_t>(height >> level, 1u);
		const uint64_t row_pitch  = (mip_width + pitch_align - 1u) & ~(pitch_align - 1u);
		const uint64_t level_size = row_pitch * mip_height * bpp;
		offsets[level]     = chain_total;
		row_pitches[level] = row_pitch;
		level_sizes[level] = level_size;
		chain_total += level_size;
	}
	if (reported.size == 0u || reported.size != ((chain_total + 255u) & ~uint64_t {255}) || reported.size > size)
	{
		return false;
	}
	for (uint32_t level = 0u; level < levels; level++)
	{
		if (chain_sizes[level].offset != offsets[level] || chain_sizes[level].size != level_sizes[level] ||
		    offsets[level] % 4u != 0u)
		{
			return false;
		}
	}
	// A mip backing keeps every level in its own image mip; the stacked atlas
	// packs them into one image at per-level offsets.
	const uint64_t real_height = mip_backing ? height : height + height / 2u;
	if (real_height > UINT32_MAX)
	{
		return false;
	}
	Vector<BufferImageCopy> planned;
	for (uint32_t level = 0u; level < levels; level++)
	{
		const uint64_t mip_width  = std::max<uint64_t>(width >> level, 1u);
		const uint64_t mip_height = std::max<uint64_t>(height >> level, 1u);
		const uint64_t row_bytes  = row_pitches[level] * bpp;
		if (offsets[level] > size || mip_height > (size - offsets[level]) / row_bytes ||
		    mip_width * bpp > size - offsets[level] - (mip_height - 1u) * row_bytes)
		{
			return false;
		}
		BufferImageCopy region {};
		region.offset    = static_cast<uint32_t>(offsets[level]);
		region.pitch     = static_cast<uint32_t>(row_pitches[level]);
		region.width     = static_cast<uint32_t>(mip_width);
		region.height    = static_cast<uint32_t>(mip_height);
		if (mip_backing)
		{
			region.dst_level = level;
		} else
		{
			const auto atlas = UtilCalcMipmapOffset(level, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
			if (atlas.first < 0 || atlas.second < 0 || static_cast<uint64_t>(atlas.first) + mip_width > width ||
			    static_cast<uint64_t>(atlas.second) + mip_height > real_height)
			{
				return false;
			}
			region.dst_level = 0u;
			region.dst_x     = atlas.first;
			region.dst_y     = atlas.second;
		}
		planned.Add(region);
	}
	for (const auto& region: planned)
	{
		regions->Add(region);
	}
	*image_width       = static_cast<uint32_t>(width);
	*image_height      = static_cast<uint32_t>(real_height);
	*bytes_per_element = bpp;
	return true;
}

bool StorageTextureCanCompositeLinearMipRawRenderDestination(const uint64_t* params, uint64_t address, uint64_t size)
{
	if (params == nullptr || address % 65536u != 0u || size == 0u || size > INT32_MAX ||
	    params[StorageTextureObject::PARAM_TILE] != 0u || params[StorageTextureObject::PARAM_RESOURCE_TYPE] != 9u ||
	    params[StorageTextureObject::PARAM_DEPTH] != 1u || params[StorageTextureObject::PARAM_BASE_ARRAY] != 0u ||
	    params[StorageTextureObject::PARAM_SKIP_SEED] != 0u)
	{
		return false;
	}
	Vector<BufferImageCopy> regions;
	uint32_t                image_width       = 0;
	uint32_t                image_height      = 0;
	uint32_t                bytes_per_element = 0;
	return StorageTexturePlanLinearMipUpload(params, size, StorageTextureUsesMipBacking(params, GuestIsGen5()), &regions,
	                                          &image_width, &image_height, &bytes_per_element);
}

void StorageTextureCopyRenderAlias(CommandBuffer* buffer, VulkanImage* source, VulkanImage* destination,
                                   const Vector<StorageTextureRenderAliasCopy>& copies)
{
	EXIT_IF(buffer == nullptr || source == nullptr || destination == nullptr || copies.IsEmpty());
	EXIT_IF(!RenderAliasFormatsMatch(source->format, destination->format) || source->samples != VK_SAMPLE_COUNT_1_BIT ||
	        destination->samples != VK_SAMPLE_COUNT_1_BIT ||
	        source->extent.width != source->guest_extent.width || source->extent.height != source->guest_extent.height ||
	        destination->extent.width != destination->guest_extent.width ||
	        destination->extent.height != destination->guest_extent.height);
	Vector<ImageImageCopy> regions;
	for (const auto& copy: copies)
	{
		ImageImageCopy region {};
		region.src_image = source;
		region.src_x = static_cast<int>(copy.source_x);
		region.src_y = static_cast<int>(copy.source_y);
		region.dst_x = static_cast<int>(copy.destination_x);
		region.dst_y = static_cast<int>(copy.destination_y);
		region.width = copy.width;
		region.height = copy.height;
		regions.Add(region);
	}
	UtilImageToImage(buffer, regions, destination, static_cast<uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
}

GpuObject::create_func_t StorageTextureObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::create_from_objects_func_t StorageTextureObject::GetCreateFromObjectsFunc() const
{
	return create_from_objects_func;
}

GpuObject::delete_func_t StorageTextureObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t StorageTextureObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Kyty::Libs::Graphics

#endif
