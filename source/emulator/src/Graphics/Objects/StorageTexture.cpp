#include "Emulator/Graphics/Objects/StorageTexture.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Gen5TextureArrayLayout.h"
#include "Emulator/Graphics/Gen5TextureVolumeLayout.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

// IWYU pragma: no_forward_declare VkImageView_T

#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static VkImageUsageFlags get_usage()
{
	VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	vk_usage |= VK_IMAGE_USAGE_STORAGE_BIT;

	return vk_usage;
}

static bool IsR32SingleComponentStorageFormat(uint32_t fmt)
{
	return fmt == 20u || fmt == 22u;
}

static uint32_t NormalizeStorageTextureSwizzle(uint32_t fmt, uint32_t swizzle)
{
	// Storage image views for these typed formats use identity component
	// mapping. Reuse must follow the effective host view contract rather than
	// the raw guest selector bits, otherwise equivalent bindings churn a fresh
	// GpuMemory object every frame.
	if (fmt == 5u || fmt == 14u || fmt == 62u)
	{
		return DstSel(4, 5, 6, 7);
	}
	if (IsR32SingleComponentStorageFormat(fmt) &&
	    (swizzle == DstSel(4, 0, 0, 1) || swizzle == DstSel(4, 0, 0, 0)))
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
	// auto base_level = params[StorageTextureObject::PARAM_LEVELS] >> 32u;
	auto       levels            = params[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto       pitch             = params[StorageTextureObject::PARAM_PITCH];
	auto       resource_type     = params[StorageTextureObject::PARAM_RESOURCE_TYPE];
	auto       depth             = params[StorageTextureObject::PARAM_DEPTH];
	auto       base_array        = params[StorageTextureObject::PARAM_BASE_ARRAY];
	bool       neo               = Config::IsNeo();
	const bool three_dimensional = resource_type == 10u;
	const bool arrayed_2d        = resource_type == 13u;
	const bool depth64kb32       = fmt == 22u && tile == 24u;

	VkImageLayout vk_layout = VK_IMAGE_LAYOUT_GENERAL;

	EXIT_NOT_IMPLEMENTED(levels >= 16);
	if (three_dimensional)
	{
		Gen5TextureVolumeLayout volume_layout {};
		const bool              is_standard = Gen5GetStandard4KBVolumeTextureLayout(
		    static_cast<uint32_t>(fmt), static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(depth),
		    static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels), static_cast<uint32_t>(tile), &volume_layout);
		if (!is_standard)
		{
			const uint32_t bpe        = std::max(1u, ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt)));
			volume_layout.linear_size = static_cast<uint64_t>(pitch) * height * depth * bpe;
			volume_layout.tiled.size  = std::max(4096u, static_cast<uint32_t>(volume_layout.linear_size));
			volume_layout.tiled.align = 4096;
		}
		std::vector<uint8_t> linear(static_cast<size_t>(volume_layout.linear_size));
		if (is_standard && !linear.empty())
		{
			TileConvertStandard4KB32VolumeToLinear(linear.data(), reinterpret_cast<void*>(*vaddr), static_cast<uint32_t>(width),
			                                       static_cast<uint32_t>(height), static_cast<uint32_t>(depth),
			                                       static_cast<uint32_t>(pitch));
		} else if (!linear.empty())
		{
			std::memcpy(linear.data(), reinterpret_cast<void*>(*vaddr), linear.size());
		}
		Vector<BufferImageCopy> regions(1);
		regions[0].offset    = 0;
		regions[0].pitch     = static_cast<uint32_t>(pitch);
		regions[0].width     = static_cast<uint32_t>(width);
		regions[0].height    = static_cast<uint32_t>(height);
		regions[0].depth     = static_cast<uint32_t>(depth);
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
		const bool one_2d_layer = (resource_type == 9u && depth == 0u && base_array == 0u) ||
		                          (arrayed_2d && depth == 1u && base_array == 0u);
		EXIT_NOT_IMPLEMENTED(!one_2d_layer || levels != 1u || pitch < width);

		TileSizeAlign tiled_size {};
		TileGetTextureSize2(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
		                    static_cast<uint32_t>(pitch), static_cast<uint32_t>(levels), static_cast<uint32_t>(tile), &tiled_size,
		                    nullptr, nullptr);
		const uint64_t linear_bytes = width * height * 4u;
		EXIT_NOT_IMPLEMENTED(tiled_size.size != *size || linear_bytes == 0u || linear_bytes > *size);

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
		EXIT_NOT_IMPLEMENTED(!Gen5GetTextureArrayLayout(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width),
		                                                static_cast<uint32_t>(height), static_cast<uint32_t>(pitch),
		                                                static_cast<uint32_t>(levels), static_cast<uint32_t>(tile),
		                                                static_cast<uint32_t>(depth), &array_layout));
		EXIT_NOT_IMPLEMENTED(!Gen5ValidateTextureArrayUpload(array_layout, static_cast<uint32_t>(base_array), *size));

		std::vector<uint8_t> linear(static_cast<size_t>(array_layout.linear_size));
		EXIT_NOT_IMPLEMENTED(!Gen5DetileTextureArray(linear.data(), linear.size(), reinterpret_cast<const void*>(*vaddr), *size,
		                                             array_layout));

		Vector<BufferImageCopy> regions(static_cast<int>(array_layout.layers));
		for (uint32_t layer = 0; layer < array_layout.layers; ++layer)
		{
			regions[layer].offset          = static_cast<uint32_t>(layer * array_layout.linear_slice_size);
			regions[layer].pitch           = array_layout.host_pitch;
			regions[layer].width           = array_layout.width;
			regions[layer].height          = array_layout.height;
			regions[layer].dst_level       = 0;
			regions[layer].dst_array_layer = layer;
		}
		UtilFillImage(ctx, vk_obj, linear.data(), linear.size(), regions, static_cast<uint64_t>(vk_layout));
		return;
	}

	// Gen5 storage images use the same 2D tile contracts as sampled textures:
	// 0/8 linear, 5 Standard4KB, 9 Standard64KB, 13 TextureTiled (legacy),
	// 27 kRenderTarget SW64k. Reject anything outside that set with values.
	if (tile != 0 && tile != 5 && tile != 8 && tile != 9 && tile != 13 && tile != 27)
	{
		EXIT("unsupported storage texture tile: tile=%" PRIu64 " format=%" PRIu64 " dfmt=%" PRIu64 " nfmt=%" PRIu64
		     " width=%" PRIu64 " height=%" PRIu64 " pitch=%" PRIu64 " levels=%" PRIu64 " type=%" PRIu64
		     " size=%" PRIu64 "\n",
		     tile, fmt, dfmt, nfmt, width, height, pitch, levels, resource_type, *size);
	}

	TileSizeOffset level_sizes[16];

	if (fmt != 0)
	{
		TileGetTextureSize2(fmt, width, height, pitch, levels, tile, nullptr, level_sizes, nullptr);
	} else
	{
		TileGetTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, nullptr, level_sizes, nullptr);
	}

	// dbg_test_mipmaps(ctx, VK_FORMAT_BC3_SRGB_BLOCK, 512, 512);

	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	uint32_t mip_pitch  = pitch;

	Vector<BufferImageCopy> regions(levels);
	for (uint32_t i = 0; i < levels; i++)
	{
		EXIT_NOT_IMPLEMENTED(level_sizes[i].size == 0);

		auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);

		regions[i].offset    = level_sizes[i].offset;
		regions[i].width     = mip_width;
		regions[i].height    = mip_height;
		regions[i].pitch     = mip_pitch;
		regions[i].dst_level = 0;
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
		EXIT_NOT_IMPLEMENTED(fmt != 0);
		auto* temp_buf = new uint8_t[*size];
		TileConvertTiledToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), TileMode::TextureTiled, dfmt, nfmt, width, height, pitch,
		                         levels, neo);
		UtilFillImage(ctx, vk_obj, temp_buf, *size, regions, static_cast<uint64_t>(vk_layout));
		delete[] temp_buf;
	} else if (tile == 5)
	{
		const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
		EXIT_NOT_IMPLEMENTED(bytes_per_element == 0u || levels != 1u);
		const uint64_t linear_bytes = static_cast<uint64_t>(pitch) * height * bytes_per_element;
		EXIT_NOT_IMPLEMENTED(linear_bytes == 0u || linear_bytes > *size);
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
		EXIT_NOT_IMPLEMENTED(bytes_per_element == 0u || levels != 1u);
		const uint32_t pitch_elems  = (pitch != 0u ? static_cast<uint32_t>(pitch) : static_cast<uint32_t>(width));
		const uint64_t linear_bytes = static_cast<uint64_t>(pitch_elems) * height * bytes_per_element;
		EXIT_NOT_IMPLEMENTED(linear_bytes == 0u || linear_bytes > *size);
		auto*       temp_buf = new uint8_t[static_cast<size_t>(linear_bytes)];
		std::memset(temp_buf, 0, static_cast<size_t>(linear_bytes));
		const auto* src = reinterpret_cast<const uint8_t*>(*vaddr);
		for (uint32_t y = 0; y < static_cast<uint32_t>(height); y++)
		{
			for (uint32_t x = 0; x < static_cast<uint32_t>(width); x++)
			{
				const uint64_t tiled =
				    (tile == 9) ? TileGetStandard64KBOffset(x, y, pitch_elems, bytes_per_element) :
				                  TileGetSw64kRxOffset(x, y, pitch_elems, bytes_per_element);
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
	auto       swizzle           = NormalizeStorageTextureSwizzle(fmt, params[StorageTextureObject::PARAM_SWIZZLE]);
	auto       resource_type     = params[StorageTextureObject::PARAM_RESOURCE_TYPE];
	auto       depth             = params[StorageTextureObject::PARAM_DEPTH];
	auto       base_array        = params[StorageTextureObject::PARAM_BASE_ARRAY];
	auto       pitch             = params[StorageTextureObject::PARAM_PITCH];
	auto       tile              = params[StorageTextureObject::PARAM_TILE];
	const bool three_dimensional = resource_type == 10u;
	const bool arrayed_2d        = resource_type == 13u;
	EXIT_NOT_IMPLEMENTED(resource_type != 8u && resource_type != 9u && resource_type != 13u && !three_dimensional);

	EXIT_NOT_IMPLEMENTED(base_level != 0);

	VkImageUsageFlags vk_usage = get_usage();

	VkComponentMapping components {};
	EXIT_IF(!VulkanDecodeComponentMapping(static_cast<uint32_t>(swizzle), &components));

	auto pixel_format = VulkanResolveGuestImageFormat(GuestImageUsage::Storage, static_cast<uint8_t>(dfmt), static_cast<uint8_t>(nfmt),
	                                                  static_cast<uint16_t>(fmt));

	if (pixel_format == VK_FORMAT_UNDEFINED)
	{
		EXIT("unsupported storage texture format: format=%" PRIu64 " dfmt=%" PRIu64 " nfmt=%" PRIu64 " tile=%" PRIu64
		     " width=%" PRIu64 " height=%" PRIu64 " pitch=%" PRIu64 " levels=%" PRIu64
		     " type=%" PRIu64 " depth=%" PRIu64 " base_array=%" PRIu64 " swizzle=0x%03" PRIx64
		     " base=0x%012" PRIx64 " size=%" PRIu64 "\n",
		     fmt, dfmt, nfmt, tile, width, height, pitch, levels, resource_type, depth, base_array,
		     static_cast<uint64_t>(swizzle), *vaddr, *size);
	}
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);
	EXIT_NOT_IMPLEMENTED(three_dimensional && depth == 0u);
	EXIT_NOT_IMPLEMENTED(arrayed_2d && (depth == 0u || base_array >= depth));

	auto real_height = ((levels > 1) ? height + (height > 1 ? height / 2 : 1) : height);

	auto* vk_obj = new StorageTextureVulkanImage;

	VulkanImageDescriptor image_descriptor {};
	image_descriptor.image_type   = three_dimensional ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	image_descriptor.extent       = {static_cast<uint32_t>(width), static_cast<uint32_t>(real_height),
	                                 static_cast<uint32_t>(three_dimensional ? depth : 1u)};
	image_descriptor.array_layers = static_cast<uint32_t>(arrayed_2d ? depth : 1u);
	image_descriptor.format       = pixel_format;
	image_descriptor.usage        = vk_usage;
	auto image_info               = VulkanBuildImageCreateInfo(image_descriptor);

	// Storage image views use identity component mapping. Single-component R32
	// resources encode their read result as R,0,0,1 while writes address R only;
	// Normalize that view contract before Vulkan validation.
	if (IsR32SingleComponentStorageFormat(static_cast<uint32_t>(fmt)) || fmt == 5u || fmt == 14u || fmt == 62u)
	{
		components.r = VK_COMPONENT_SWIZZLE_R;
		components.g = VK_COMPONENT_SWIZZLE_G;
		components.b = VK_COMPONENT_SWIZZLE_B;
		components.a = VK_COMPONENT_SWIZZLE_A;
	}

	if (!VulkanNormalizeStorageComponentMapping(&image_info.format, &components))
	{
		EXIT("swizzle is not supported");
	}

	if (!VulkanImageFormatSupported(ctx, image_info))
	{
		EXIT("format is not supported");
	}

	vk_obj->SetNativeExtent(width, height);
	vk_obj->format     = image_info.format;
	vk_obj->image      = nullptr;
	vk_obj->layout     = image_info.initialLayout;
	vk_obj->guest_size = *size;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImage(ctx, image_info, vk_obj, mem));

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	VulkanImageViewDescriptor view_descriptor {};
	view_descriptor.image       = vk_obj->image;
	view_descriptor.view_type   = three_dimensional ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	view_descriptor.format      = vk_obj->format;
	view_descriptor.components  = components;
	view_descriptor.level_count = VK_REMAINING_MIP_LEVELS;
	const int view_index        = (three_dimensional ? VulkanImage::VIEW_3D : VulkanImage::VIEW_DEFAULT);
	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[view_index]));
	if (!three_dimensional)
	{
		view_descriptor.view_type        = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		view_descriptor.base_array_layer = arrayed_2d ? static_cast<uint32_t>(base_array) : 0u;
		view_descriptor.layer_count      = arrayed_2d ? static_cast<uint32_t>(depth - base_array) : 1u;
		EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_ARRAY]));
	}

	return vk_obj;
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

	const auto fmt       = static_cast<uint32_t>((params[PARAM_FORMAT] >> 16u) & 0xffffu);
	const auto other_fmt = static_cast<uint32_t>((other[PARAM_FORMAT] >> 16u) & 0xffffu);
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_PITCH] == other[PARAM_PITCH] &&
	        params[PARAM_WIDTH_HEIGHT] == other[PARAM_WIDTH_HEIGHT] && params[PARAM_LEVELS] == other[PARAM_LEVELS] &&
	        params[PARAM_TILE] == other[PARAM_TILE] && params[PARAM_NEO] == other[PARAM_NEO] &&
	        NormalizeStorageTextureSwizzle(fmt, params[PARAM_SWIZZLE]) == NormalizeStorageTextureSwizzle(other_fmt, other[PARAM_SWIZZLE]) &&
	        params[PARAM_RESOURCE_TYPE] == other[PARAM_RESOURCE_TYPE] && params[PARAM_DEPTH] == other[PARAM_DEPTH] &&
	        params[PARAM_BASE_ARRAY] == other[PARAM_BASE_ARRAY]);
}

GpuObject::create_func_t StorageTextureObject::GetCreateFunc() const
{
	return create_func;
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
