#include "Emulator/Graphics/Objects/Texture.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#include "astcenc.h"

// IWYU pragma: no_forward_declare VkImageView_T

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool TextureSupportsGen5SampledFormat(uint16_t fmt)
{
	switch (fmt)
	{
		case 1:
		case 5:
		case 13:
		case 14:
		case 20:
		case 56:
		case 62:
		case 71:
		case 75:
		case 133:
		case 173: return true;
		default: return false;
	}
}

// Sampled texture formats only. Storage images and render-target aliases use
// their own object format resolvers because sRGB storage/image-write semantics
// are not interchangeable with sampled degamma.
VkFormat TextureResolveSampledVkFormat(uint8_t dfmt, uint8_t nfmt, uint16_t fmt, bool force_degamma)
{
	if (fmt == 0)
	{
		if (nfmt == 9 && dfmt == 10)
		{
			return VK_FORMAT_R8G8B8A8_SRGB;
		}
		if (nfmt == 0 && dfmt == 10)
		{
			return VK_FORMAT_R8G8B8A8_UNORM;
		}
		if (nfmt == 0 && dfmt == 1)
		{
			return VK_FORMAT_R8_UNORM;
		}
		if (nfmt == 0 && dfmt == 3)
		{
			return VK_FORMAT_R8G8_UNORM;
		}
		if (nfmt == 9 && dfmt == 37)
		{
			return VK_FORMAT_BC3_SRGB_BLOCK;
		}
		if (nfmt == 0 && dfmt == 37)
		{
			return VK_FORMAT_BC3_UNORM_BLOCK;
		}
		if (nfmt == 0 && dfmt == 36)
		{
			return VK_FORMAT_BC2_UNORM_BLOCK;
		}
		if (nfmt == 0 && dfmt == 35)
		{
			return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
		}
		EXIT("unknown format: nfmt = %u, dfmt = %u\n", nfmt, dfmt);
	} else
	{
		// Gen5 unified image formats (UfmtGFX10).
		if (fmt == 1)
		{
			return VK_FORMAT_R8_UNORM;
		}
		if (fmt == 5)
		{
			// UfmtGFX10 calls this 8_UINT, but the sampled-image recompiler
			// emits float image operations unless every image in the shader is a
			// 32-bit integer image. Use a normalized host view here so hardware
			// sampling has the same 0..1 result consumed by video shaders.
			return VK_FORMAT_R8_UNORM;
		}
		if (fmt == 13)
		{
			return VK_FORMAT_R16_SFLOAT;
		}
		if (fmt == 14)
		{
			return VK_FORMAT_R8G8_UNORM;
		}
		if (fmt == 20)
		{
			return VK_FORMAT_R32_UINT;
		}
		if (fmt == 56)
		{
			return (force_degamma ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
		}
		if (fmt == 62)
		{
			return VK_FORMAT_R32G32_UINT;
		}
		if (fmt == 71)
		{
			return VK_FORMAT_R16G16B16A16_SFLOAT;
		}
		if (fmt == 75)
		{
			return VK_FORMAT_R32G32B32A32_UINT;
		}
		if (fmt == 173)
		{
			return VK_FORMAT_BC3_UNORM_BLOCK;
		}
		if (fmt == 133)
		{
			return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
		}
		EXIT("unknown format: fmt = %u\n", fmt);
	}
	return VK_FORMAT_UNDEFINED;
}

static VkComponentSwizzle get_swizzle(uint8_t s)
{
	switch (s)
	{
		case 0: return VK_COMPONENT_SWIZZLE_ZERO; break;
		case 1: return VK_COMPONENT_SWIZZLE_ONE; break;
		case 4: return VK_COMPONENT_SWIZZLE_R; break;
		case 5: return VK_COMPONENT_SWIZZLE_G; break;
		case 6: return VK_COMPONENT_SWIZZLE_B; break;
		case 7: return VK_COMPONENT_SWIZZLE_A; break;
		case 2:
		case 3:
		default: EXIT("unknown swizzle: %d\n", static_cast<int>(s));
	}
	return VK_COMPONENT_SWIZZLE_IDENTITY;
}

static VkImageUsageFlags get_usage()
{
	VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	vk_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

	return vk_usage;
}

static bool CheckFormat(GraphicContext* ctx, VkImageCreateInfo* image_info)
{
	VkImageFormatProperties props {};
	if (vkGetPhysicalDeviceImageFormatProperties(ctx->physical_device, image_info->format, image_info->imageType, image_info->tiling,
	                                             image_info->usage, image_info->flags, &props) == VK_ERROR_FORMAT_NOT_SUPPORTED)
	{
		if (image_info->format == VK_FORMAT_R8G8B8A8_SRGB)
		{
			// TODO() convert SRGB -> LINEAR in shader
			image_info->format = VK_FORMAT_R8G8B8A8_UNORM;
			bool result        = CheckFormat(ctx, image_info);
			printf("replace VK_FORMAT_R8G8B8A8_SRGB => VK_FORMAT_R8G8B8A8_UNORM [%s]\n", (!result ? "FAIL" : "SUCCESS"));
			return result;
		}
		if (image_info->format == VK_FORMAT_B8G8R8A8_SRGB)
		{
			// TODO() convert SRGB -> LINEAR in shader
			image_info->format = VK_FORMAT_B8G8R8A8_UNORM;
			bool result        = CheckFormat(ctx, image_info);
			printf("replace VK_FORMAT_B8G8R8A8_SRGB => VK_FORMAT_B8G8R8A8_UNORM [%s]\n", (!result ? "FAIL" : "SUCCESS"));
			return result;
		}
		return false;
	}
	return true;
}

static bool CheckSwizzle(GraphicContext* /*ctx*/, VkImageCreateInfo* image_info, VkComponentMapping* components)
{
	if ((image_info->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0)
	{
		if (components->r == VK_COMPONENT_SWIZZLE_R && components->g == VK_COMPONENT_SWIZZLE_G && components->b == VK_COMPONENT_SWIZZLE_B &&
		    components->a == VK_COMPONENT_SWIZZLE_A)
		{
			return true;
		}

		if (components->r == VK_COMPONENT_SWIZZLE_B && components->g == VK_COMPONENT_SWIZZLE_G && components->b == VK_COMPONENT_SWIZZLE_R &&
		    components->a == VK_COMPONENT_SWIZZLE_A && image_info->format == VK_FORMAT_R8G8B8A8_SRGB)
		{
			printf("replace VK_FORMAT_R8G8B8A8_SRGB => VK_FORMAT_B8G8R8A8_SRGB\n");

			components->r      = VK_COMPONENT_SWIZZLE_R;
			components->g      = VK_COMPONENT_SWIZZLE_G;
			components->b      = VK_COMPONENT_SWIZZLE_B;
			components->a      = VK_COMPONENT_SWIZZLE_A;
			image_info->format = VK_FORMAT_B8G8R8A8_SRGB;
			return true;
		}

		// TODO() swizzle channels in shader

		return false;
	}
	return true;
}

namespace
{
astcenc_context* g_astc10x5_decoder_ctx = nullptr;
std::mutex       g_astc10x5_decoder_mutex;

bool EnsureAstc10x5Decoder()
{
	if (g_astc10x5_decoder_ctx != nullptr)
	{
		return true;
	}

	astcenc_config config {};
	if (astcenc_config_init(ASTCENC_PRF_LDR, 10, 5, 1, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &config) != ASTCENC_SUCCESS)
	{
		return false;
	}

	if (astcenc_context_alloc(&config, 1, &g_astc10x5_decoder_ctx, nullptr) != ASTCENC_SUCCESS)
	{
		return false;
	}

	return true;
}

bool DecodeAstc10x5ToRgba8(uint32_t width, uint32_t height, const uint8_t* source, size_t source_size, std::vector<uint8_t>& output)
{
	if (width == 0u || height == 0u || source == nullptr || source_size == 0u)
	{
		return false;
	}

	const auto blocks_x = (static_cast<size_t>(width) + 9u) / 10u;
	const auto blocks_y = (static_cast<size_t>(height) + 4u) / 5u;
	if (blocks_x > std::numeric_limits<size_t>::max() / blocks_y ||
	    blocks_x * blocks_y > std::numeric_limits<size_t>::max() / 16u ||
	    source_size < blocks_x * blocks_y * 16u)
	{
		return false;
	}

	std::lock_guard lock(g_astc10x5_decoder_mutex);
	if (!EnsureAstc10x5Decoder())
	{
		return false;
	}

	auto output_size = static_cast<size_t>(width);
	output_size *= static_cast<size_t>(height);
	output_size *= 4u;
	if (output_size == 0u)
	{
		return false;
	}

	output.resize(output_size);

	void* output_data[1] = {output.data()};
	astcenc_image image_out {};
	image_out.dim_x = width;
	image_out.dim_y = height;
	image_out.dim_z = 1u;
	image_out.data_type = ASTCENC_TYPE_U8;
	image_out.data = output_data;

	const astcenc_swizzle swizzle {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
	if (astcenc_decompress_image(g_astc10x5_decoder_ctx, source, source_size, &image_out, &swizzle, 0) != ASTCENC_SUCCESS)
	{
		return false;
	}

	return true;
}

} // namespace

static void update_func(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	KYTY_PROFILER_BLOCK("TextureObject::update_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = static_cast<TextureVulkanImage*>(obj);

	auto tile   = params[TextureObject::PARAM_TILE];
	auto fmt    = (params[TextureObject::PARAM_FORMAT] >> 16u) & 0xffffu;
	auto dfmt   = (params[TextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto nfmt   = (params[TextureObject::PARAM_FORMAT]) & 0xffu;
	auto width  = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto levels = params[TextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto pitch  = params[TextureObject::PARAM_PITCH];
	auto resource_info = params[TextureObject::PARAM_RESOURCE_INFO];
	auto resource_type = TextureObject::GetResourceType(resource_info);
	auto depth = TextureObject::GetResourceDepth(resource_info);
	auto base_array = TextureObject::GetResourceBaseArray(resource_info);
	bool neo    = Config::IsNeo();
	const bool skip_guest = params[TextureObject::PARAM_SKIP_GUEST_UPLOAD] != 0;
	const bool three_dimensional = resource_type == 10u;
	const bool arrayed_2d = resource_type == 13u;

	VkImageLayout vk_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	EXIT_NOT_IMPLEMENTED(levels >= 16);
	EXIT_NOT_IMPLEMENTED(three_dimensional && (fmt != 20u || tile != 5u || levels != 1u || depth == 0u));
	if (three_dimensional)
	{
		TileSizeAlign tiled_size {};
		TileGetStandard4KB32VolumeSize(width, height, static_cast<uint32_t>(depth), static_cast<uint32_t>(pitch), &tiled_size);
		EXIT_NOT_IMPLEMENTED(*size != tiled_size.size);
		const uint64_t linear_bytes = static_cast<uint64_t>(pitch) * height * depth * 4u;
		EXIT_NOT_IMPLEMENTED(linear_bytes == 0u || linear_bytes > *size);
		auto* temp_buf = new uint8_t[static_cast<size_t>(linear_bytes)];
		TileConvertStandard4KB32VolumeToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), static_cast<uint32_t>(width),
		                                      static_cast<uint32_t>(height), static_cast<uint32_t>(depth), static_cast<uint32_t>(pitch));
		Vector<BufferImageCopy> regions(1);
		regions[0].offset = 0;
		regions[0].pitch = static_cast<uint32_t>(pitch);
		regions[0].width = static_cast<uint32_t>(width);
		regions[0].height = static_cast<uint32_t>(height);
		regions[0].depth = static_cast<uint32_t>(depth);
		regions[0].dst_level = 0;
		regions[0].dst_x = 0;
		regions[0].dst_y = 0;
		regions[0].dst_z = 0;
		UtilFillImage(ctx, vk_obj, temp_buf, linear_bytes, regions, static_cast<uint64_t>(vk_layout));
		delete[] temp_buf;
		return;
	}
	if (arrayed_2d)
	{
		const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
		EXIT_NOT_IMPLEMENTED(bytes_per_element == 0u || tile != 5u || levels != 1u ||
		                     depth == 0u || base_array >= depth || depth >= 16u);
		TileSizeAlign slice_size {};
		TileGetTextureSize2(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
		                    static_cast<uint32_t>(pitch), 1u, static_cast<uint32_t>(tile), &slice_size, nullptr, nullptr);
		EXIT_NOT_IMPLEMENTED(*size != static_cast<uint64_t>(slice_size.size) * depth);
		const uint64_t linear_slice_bytes = static_cast<uint64_t>(pitch) * height * bytes_per_element;
		std::vector<uint8_t> linear(static_cast<size_t>(linear_slice_bytes * depth));
		Vector<BufferImageCopy> regions(static_cast<int>(depth));
			for (uint32_t layer = 0; layer < depth; ++layer)
		{
			TileConvertStandard4KBToLinear(linear.data() + layer * linear_slice_bytes,
			                              reinterpret_cast<const uint8_t*>(*vaddr) + layer * slice_size.size,
			                              static_cast<uint32_t>(width), static_cast<uint32_t>(height),
			                              static_cast<uint32_t>(pitch), bytes_per_element);
			regions[layer].offset = static_cast<uint32_t>(layer * linear_slice_bytes);
			regions[layer].pitch = static_cast<uint32_t>(pitch);
			regions[layer].width = static_cast<uint32_t>(width);
			regions[layer].height = static_cast<uint32_t>(height);
			regions[layer].dst_level = 0;
				regions[layer].dst_array_layer = layer;
			}
			UtilFillImage(ctx, vk_obj, linear.data(), linear.size(), regions, static_cast<uint64_t>(vk_layout));
			return;
	}

	// GPU-owned range under a live color surface that could not be bound as an
	// alias: never detile guest (period-16 bands). Transparent black clear.
	static const char* skipped_dump_spec = std::getenv("KYTY_DUMP_TILED_SAMPLE");
	uint32_t           skipped_dump_width = 0;
	uint32_t           skipped_dump_height = 0;
	const bool inspect_skipped_guest =
	    skipped_dump_spec != nullptr &&
	    std::sscanf(skipped_dump_spec, "%ux%u", &skipped_dump_width, &skipped_dump_height) == 2 &&
	    skipped_dump_width == width && skipped_dump_height == height;
	if (skip_guest && !inspect_skipped_guest)
	{
		const uint32_t bpp   = (fmt != 0u ? ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt)) : 4u);
		const uint64_t bytes = static_cast<uint64_t>(width) * height * bpp;
		EXIT_NOT_IMPLEMENTED(bytes == 0u);
		std::vector<uint8_t> zeros(static_cast<size_t>(bytes), 0);
		Vector<BufferImageCopy> clear_regions(1);
		clear_regions[0].offset    = 0;
		clear_regions[0].pitch     = static_cast<uint32_t>(width);
		clear_regions[0].width     = static_cast<uint32_t>(width);
		clear_regions[0].height    = static_cast<uint32_t>(height);
		clear_regions[0].dst_level = 0;
		clear_regions[0].dst_x     = 0;
		clear_regions[0].dst_y     = 0;
		UtilFillImage(ctx, vk_obj, zeros.data(), bytes, clear_regions, static_cast<uint64_t>(vk_layout));
		return;
	}

	TileSizeOffset level_sizes[16];

	if (fmt != 0)
	{
		// Gen5: tile 0 = linear; 5 = kStandard4KB; 27 = kRenderTarget;
		// 9 = kStandard64KB.
		// Other modes remain unsupported until their layout is evidenced.
		EXIT_NOT_IMPLEMENTED(tile != 0 && tile != 5 && tile != 27 && tile != 9);

		TileGetTextureSize2(fmt, width, height, pitch, levels, tile, nullptr, level_sizes, nullptr);
	} else
	{
		EXIT_NOT_IMPLEMENTED(tile != 8 && tile != 13 && tile != 10);

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

		regions[i].offset    = level_sizes[i].offset;
		regions[i].width     = mip_width;
		regions[i].height    = mip_height;
		regions[i].pitch     = mip_pitch;
		regions[i].dst_level = i;
		regions[i].dst_x     = 0;
		regions[i].dst_y     = 0;

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

	if (fmt == 0)
	{
		if (tile == 13)
		{
			// EXIT_NOT_IMPLEMENTED(pitch != width);
			EXIT_NOT_IMPLEMENTED(fmt != 0);
			auto* temp_buf = new uint8_t[*size];
			TileConvertTiledToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), TileMode::TextureTiled, dfmt, nfmt, width, height, pitch,
			                         levels, neo);
			UtilFillImage(ctx, vk_obj, temp_buf, *size, regions, static_cast<uint64_t>(vk_layout));
			delete[] temp_buf;
		} else if (tile == 10)
		{
			// Display_2dThin BGRA8 (SDL_GPU/Gnm UI and display surfaces). Do not treat
			// these as GPU-owned RenderTextures: that path skipped CPU upload and left
			// tiled guest bytes unread, which sampled as horizontally smeared UI.
			EXIT_NOT_IMPLEMENTED(!(dfmt == 10 && nfmt == 0));
			EXIT_NOT_IMPLEMENTED(levels != 1);
			const uint64_t linear_bytes = static_cast<uint64_t>(width) * height * 4u;
			EXIT_NOT_IMPLEMENTED(linear_bytes == 0);
			auto* temp_buf = new uint8_t[static_cast<size_t>(linear_bytes)];
			TileConvertDisplayThinBgraToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), width, height, pitch, neo);
			regions[0].pitch = static_cast<uint32_t>(width);
			UtilFillImage(ctx, vk_obj, temp_buf, linear_bytes, regions, static_cast<uint64_t>(vk_layout));
			delete[] temp_buf;
		} else if (tile == 8)
		{
			UtilFillImage(ctx, vk_obj, reinterpret_cast<void*>(*vaddr), *size, regions, static_cast<uint64_t>(vk_layout));
		}
	} else
	{
		if (tile == 0)
		{
			// Opt-in dump for linear Gen5 sample investigation (scratch only).
			// KYTY_DUMP_LINEAR_SAMPLE=WxH writes one RGBA8 BMP under /tmp.
			if (fmt == 56u && levels == 1u)
			{
				static const char* dump_spec = std::getenv("KYTY_DUMP_LINEAR_SAMPLE");
				if (dump_spec != nullptr && dump_spec[0] != '\0')
				{
					uint32_t dw = 0;
					uint32_t dh = 0;
					if (std::sscanf(dump_spec, "%ux%u", &dw, &dh) == 2 && dw == width && dh == height)
					{
						static std::set<uint64_t> dumped_sizes;
						const uint64_t            key = (static_cast<uint64_t>(width) << 32u) | height;
						if (dumped_sizes.insert(key).second)
						{
							char out_path[128];
							std::snprintf(out_path, sizeof(out_path), "/tmp/kyty-dump-linear-%ux%u.bmp",
							              static_cast<unsigned>(width), static_cast<unsigned>(height));
							char out_path_w[128];
							std::snprintf(out_path_w, sizeof(out_path_w), "/tmp/kyty-dump-linear-%ux%u-widthpitch.bmp",
							              static_cast<unsigned>(width), static_cast<unsigned>(height));
							auto write_bmp = [](const char* path, const uint8_t* src, uint32_t w, uint32_t h, uint32_t src_pitch) {
								FILE* f = std::fopen(path, "wb");
								if (f == nullptr)
								{
									return;
								}
								const uint32_t row_bytes = w * 4u;
								const uint32_t pad       = (4u - (row_bytes % 4u)) % 4u;
								const uint32_t dib       = 40u;
								const uint32_t off       = 14u + dib;
								const uint32_t size_img  = (row_bytes + pad) * h;
								const uint32_t file_size = off + size_img;
								uint8_t        hdr[54] {};
								hdr[0] = 'B';
								hdr[1] = 'M';
								std::memcpy(hdr + 2, &file_size, 4);
								std::memcpy(hdr + 10, &off, 4);
								std::memcpy(hdr + 14, &dib, 4);
								int32_t wi = static_cast<int32_t>(w);
								int32_t hi = -static_cast<int32_t>(h);
								std::memcpy(hdr + 18, &wi, 4);
								std::memcpy(hdr + 22, &hi, 4);
								uint16_t planes = 1;
								uint16_t bpp    = 32;
								std::memcpy(hdr + 26, &planes, 2);
								std::memcpy(hdr + 28, &bpp, 2);
								std::fwrite(hdr, 1, 54, f);
								std::vector<uint8_t> zero(pad, 0);
								for (uint32_t y = 0; y < h; y++)
								{
									const uint8_t* row = src + static_cast<uint64_t>(y) * src_pitch * 4u;
									std::fwrite(row, 1, row_bytes, f);
									if (pad != 0)
									{
										std::fwrite(zero.data(), 1, pad, f);
									}
								}
								std::fclose(f);
							};
							const auto* base = reinterpret_cast<const uint8_t*>(*vaddr);
							write_bmp(out_path, base, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
							          static_cast<uint32_t>(pitch));
							write_bmp(out_path_w, base, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
							          static_cast<uint32_t>(width));
							printf("KYTY_DUMP_LINEAR_SAMPLE wrote %ux%u pitch=%u -> %s\n", static_cast<unsigned>(width),
							       static_cast<unsigned>(height), static_cast<unsigned>(pitch), out_path);
						}
					}
				}
			}
			UtilFillImage(ctx, vk_obj, reinterpret_cast<void*>(*vaddr), *size, regions, static_cast<uint64_t>(vk_layout));
		} else if (tile == 5)
		{
			const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
			const bool bc3 = fmt == 173u;
			EXIT_NOT_IMPLEMENTED(bytes_per_element == 0u || levels != 1u);
			const uint32_t element_width = bc3 ? (width + 3u) / 4u : width;
			const uint32_t element_height = bc3 ? (height + 3u) / 4u : height;
			const uint32_t element_pitch = bc3 ? (pitch + 3u) / 4u : pitch;
			const uint64_t linear_bytes = static_cast<uint64_t>(element_pitch) * element_height * bytes_per_element;
			EXIT_NOT_IMPLEMENTED(linear_bytes == 0u || linear_bytes > *size);
			std::vector<uint8_t> temp_buf(static_cast<size_t>(linear_bytes));
			TileConvertStandard4KBToLinear(temp_buf.data(), reinterpret_cast<void*>(*vaddr), element_width, element_height,
		                              element_pitch, bytes_per_element);
			if (bc3 && std::getenv("KYTY_DUMP_ASTC_BLOCKS") != nullptr)
			{
				const auto* guest = reinterpret_cast<const uint8_t*>(*vaddr);
				uint64_t raw_nonzero = 0;
				uint64_t detiled_nonzero = 0;
				uint64_t raw_hash = 1469598103934665603ull;
				uint64_t detiled_hash = 1469598103934665603ull;
				for (uint64_t i = 0; i < linear_bytes; ++i)
				{
					raw_nonzero += (guest[i] != 0u ? 1u : 0u);
					detiled_nonzero += (temp_buf[i] != 0u ? 1u : 0u);
					raw_hash = (raw_hash ^ guest[i]) * 1099511628211ull;
					detiled_hash = (detiled_hash ^ temp_buf[i]) * 1099511628211ull;
				}
				std::fprintf(stderr, "KYTY_DUMP_ASTC_BLOCKS addr=0x%012" PRIx64 " size=%" PRIu64
				                     " elem=%ux%u pitch=%u raw_nonzero=%" PRIu64 " detiled_nonzero=%" PRIu64
				                     " raw_hash=%016" PRIx64 " detiled_hash=%016" PRIx64 " raw=",
				             *vaddr, linear_bytes, element_width, element_height, element_pitch, raw_nonzero, detiled_nonzero,
				             raw_hash, detiled_hash);
				for (uint32_t i = 0; i < 64u && i < linear_bytes; ++i)
				{
					std::fprintf(stderr, "%02x", guest[i]);
				}
				std::fprintf(stderr, " detiled=");
				for (uint32_t i = 0; i < 64u && i < linear_bytes; ++i)
				{
					std::fprintf(stderr, "%02x", temp_buf[i]);
				}
				std::fprintf(stderr, "\n");
			}
			regions[0].offset = 0;
			regions[0].width  = width;
			regions[0].height = height;
			if (vk_obj->astc10x5_cpu_fallback)
			{
				std::vector<uint8_t> decoded;
				EXIT_NOT_IMPLEMENTED(!DecodeAstc10x5ToRgba8(static_cast<uint32_t>(width), static_cast<uint32_t>(height), temp_buf.data(),
				                                          static_cast<size_t>(linear_bytes), decoded));
				regions[0].pitch = static_cast<uint32_t>(width);
				UtilFillImage(ctx, vk_obj, decoded.data(), decoded.size(), regions, static_cast<uint64_t>(vk_layout));
			}
			else
			{
				regions[0].pitch = pitch;
				UtilFillImage(ctx, vk_obj, temp_buf.data(), linear_bytes, regions, static_cast<uint64_t>(vk_layout));
			}
		} else if (tile == 27 || tile == 9)
		{
			// Tiled sample texture: detile into tightly packed linear rows then
			// upload. Render-target aliases still prefer FindRenderTexture
			// before create; this path covers pure CPU-backed sample textures.
			// tile 27 = kRenderTarget layout; tile 9 = kStandard64KB (RGBA8).
			// BC1 (fmt 133) detiles compressed 4x4 blocks as 8-byte elements on
			// tile 27 only.
			EXIT_NOT_IMPLEMENTED(tile == 9 && fmt != 56);
			EXIT_NOT_IMPLEMENTED(fmt != 56 && fmt != 133);
			EXIT_NOT_IMPLEMENTED(levels != 1);
			const bool bc1 = (fmt == 133u);
			EXIT_NOT_IMPLEMENTED(bc1 && tile != 27);
			const uint32_t bpp          = (bc1 ? 8u : 4u);
			const uint32_t copy_width   = bc1 ? std::max((static_cast<uint32_t>(width) + 3u) / 4u, 1u) : static_cast<uint32_t>(width);
			const uint32_t copy_height  = bc1 ? std::max((static_cast<uint32_t>(height) + 3u) / 4u, 1u) : static_cast<uint32_t>(height);
			const uint32_t pitch_texels = (pitch != 0u ? static_cast<uint32_t>(pitch) : static_cast<uint32_t>(width));
			const uint32_t pitch_elems  = bc1 ? std::max((pitch_texels + 3u) / 4u, 1u) : pitch_texels;
			// Pitch-strided linear rows (host tiler contract). Tight y*width packing
			// is only equivalent when pitch_elems == width.
			const uint64_t linear_bytes = static_cast<uint64_t>(pitch_elems) * copy_height * bpp;
			auto*          temp_buf     = new uint8_t[linear_bytes];
			std::memset(temp_buf, 0, static_cast<size_t>(linear_bytes));
			{
				const DebugStatsScopedWork detile_work(DebugStatsRecordDetile, static_cast<uint64_t>(copy_width) * copy_height * bpp);
				auto*                      d = temp_buf;
				const auto*                s = reinterpret_cast<const uint8_t*>(*vaddr);
				for (uint32_t y = 0; y < copy_height; y++)
				{
					for (uint32_t x = 0; x < copy_width; x++)
					{
						const uint64_t tiled =
						    (tile == 9) ? TileGetStandard64KB32Offset(x, y, pitch_elems) : TileGetSw64kRxOffset(x, y, pitch_elems, bpp);
						const uint64_t linear = (static_cast<uint64_t>(y) * pitch_elems + x) * bpp;
						std::memcpy(d + linear, s + tiled, bpp);
					}
				}
			}
			regions[0].offset = 0;
			// bufferRowLength is in texels (BC block width accounted by the caller).
			regions[0].pitch  = pitch_texels;
			regions[0].width  = static_cast<uint32_t>(width);
			regions[0].height = static_cast<uint32_t>(height);
			if (skip_guest && inspect_skipped_guest && bpp == 4u)
			{
				for (uint64_t pixel = 0; pixel < linear_bytes / 4u; ++pixel)
				{
					temp_buf[pixel * 4u + 0u] = 0u;
					temp_buf[pixel * 4u + 1u] = 255u;
					temp_buf[pixel * 4u + 2u] = 0u;
					temp_buf[pixel * 4u + 3u] = 255u;
				}
			}
			UtilFillImage(ctx, vk_obj, temp_buf, linear_bytes, regions, static_cast<uint64_t>(vk_layout));
			if (!bc1)
			{
				static const char* dump_spec = std::getenv("KYTY_DUMP_TILED_SAMPLE");
				uint32_t           dump_width = 0;
				uint32_t           dump_height = 0;
				if (dump_spec != nullptr && std::sscanf(dump_spec, "%ux%u", &dump_width, &dump_height) == 2 &&
				    dump_width == width && dump_height == height)
				{
					static std::set<uint64_t> dumped;
					const uint64_t key = (static_cast<uint64_t>(width) << 32u) | height;
					if (dumped.insert(key).second)
					{
						const auto* pixels = reinterpret_cast<const uint32_t*>(temp_buf);
						const size_t count = static_cast<size_t>(linear_bytes / sizeof(uint32_t));
						std::set<uint32_t> colors;
						for (size_t pixel = 0; pixel < count && colors.size() <= 256u; pixel++)
						{
							colors.insert(pixels[pixel]);
						}
						std::fprintf(stderr,
						             "KYTY_DUMP_TILED_SAMPLE guest=0x%016" PRIx64 " size=%ux%u pitch=%u first=0x%08x "
						             "colors=%zu%s raw0=0x%08x raw1=0x%08x\n",
						             static_cast<uint64_t>(*vaddr), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
						             pitch_elems, count > 0 ? pixels[0] : 0u, colors.size(), colors.size() > 256u ? "+" : "",
						             *size >= 4u ? *reinterpret_cast<const uint32_t*>(*vaddr) : 0u,
						             *size >= 8u ? *(reinterpret_cast<const uint32_t*>(*vaddr) + 1) : 0u);
						UtilDumpVulkanImageRgba8Bmp(ctx, vk_obj, "/tmp/kyty-dump-tiled-sample", "guest");
					}
				}
			}
			delete[] temp_buf;
		}
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void update2_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, void* obj, GpuMemoryScenario scenario,
                         const Vector<GpuMemoryObject>& objects)
{
	KYTY_PROFILER_BLOCK("TextureObject::update2_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(objects.IsEmpty());

	auto* vk_obj = static_cast<TextureVulkanImage*>(obj);

	auto width  = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto levels = params[TextureObject::PARAM_LEVELS] & 0xffffffffu;

	VkImageLayout vk_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	EXIT_NOT_IMPLEMENTED(levels >= 16);

	uint32_t mip_width  = width;
	uint32_t mip_height = height;

	Vector<ImageImageCopy> regions(levels);

	auto fmt = (params[TextureObject::PARAM_FORMAT] >> 16u) & 0xffffu;

	static const char* update2_dump_spec = std::getenv("KYTY_DUMP_TILED_SAMPLE");
	uint32_t           update2_dump_width = 0;
	uint32_t           update2_dump_height = 0;
	const bool dump_update2 =
	    update2_dump_spec != nullptr &&
	    std::sscanf(update2_dump_spec, "%ux%u", &update2_dump_width, &update2_dump_height) == 2 &&
	    update2_dump_width == width && update2_dump_height == height;
	if (dump_update2)
	{
		static std::atomic_uint dump_count {0};
		const auto              current = dump_count.fetch_add(1, std::memory_order_relaxed);
		if (current < 8u)
		{
			std::fprintf(stderr, "KYTY_DUMP_TILED_SAMPLE update2 size=%ux%u fmt=%u levels=%u scenario=%u parents=%u\n",
			             static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(fmt),
			             static_cast<unsigned>(levels), static_cast<unsigned>(scenario), static_cast<unsigned>(objects.Size()));
			for (uint32_t i = 0; i < objects.Size(); ++i)
			{
				const auto& parent = objects.At(i);
				const auto* image  = parent.type == GpuMemoryObjectType::RenderTexture
				                         ? static_cast<const VulkanImage*>(static_cast<const RenderTextureVulkanImage*>(parent.obj))
				                     : parent.type == GpuMemoryObjectType::StorageTexture
				                         ? static_cast<const VulkanImage*>(static_cast<const StorageTextureVulkanImage*>(parent.obj))
				                         : nullptr;
				const auto extent = image != nullptr ? image->GetGuestExtent() : VkExtent2D {};
				std::fprintf(stderr, "  parent[%u] type=%u obj=%p extent=%ux%u format=%u layout=%u guest_size=%" PRIu64 "\n",
				             static_cast<unsigned>(i), static_cast<unsigned>(parent.type), parent.obj,
				             static_cast<unsigned>(extent.width), static_cast<unsigned>(extent.height),
				             static_cast<unsigned>(image != nullptr ? image->format : VK_FORMAT_UNDEFINED),
				             static_cast<unsigned>(image != nullptr ? image->layout : VK_IMAGE_LAYOUT_UNDEFINED),
				             image != nullptr ? image->guest_size : 0u);
			}
			std::fflush(stderr);
		}
	}

	// Select a surface parent only when sample ufmt and VkFormat families match
	// and the parent extent equals this mip. Copying float lighting into RGBA8
	// reinterprets bits as cyan/hot garbage; copying a larger parent without a
	// crop view leaves horizontal bands on world tiles.
	const auto surface_parent_ok = [fmt](VulkanImage* img, uint32_t need_w, uint32_t need_h) -> bool {
		if (img == nullptr)
		{
			return false;
		}
		if (!img->MatchesGuestExtent(need_w, need_h))
		{
			return false;
		}
		if (fmt == 0u)
		{
			return true;
		}
		return Gen5SampleMayCopyFromSurfaceParent(static_cast<uint32_t>(fmt), img->format);
	};

	// Leave layout UNDEFINED when no valid surface parent. GpuMemory then
	// guest-uploads (package tiles) instead of leaving transparent-black
	// AABBs that only show god-ray bands through alpha.
	const auto skip_surface_copy = [&]() {
		(void)ctx;
		(void)vk_obj;
		(void)vk_layout;
	};

	if (objects.Size() == 1 && objects.At(0).type == GpuMemoryObjectType::StorageTexture && scenario == GpuMemoryScenario::Common)
	{
		auto* src_obj = static_cast<StorageTextureVulkanImage*>(objects.At(0).obj);
		const auto src_guest_extent = (src_obj != nullptr ? src_obj->GetGuestExtent() : VkExtent2D {});
		uint32_t   block_copy_width  = 0;
		uint32_t   block_copy_height = 0;
		const bool block_copy =
		    levels == 1u && src_obj != nullptr &&
		    Gen5BlockCompressedStorageCopyExtent(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width),
		                                         static_cast<uint32_t>(height), src_obj->format, src_guest_extent.width,
		                                         src_guest_extent.height, &block_copy_width, &block_copy_height);
		if (block_copy)
		{
			if (std::getenv("KYTY_DUMP_BLOCK_STORAGE") != nullptr)
			{
				const uint64_t bytes = static_cast<uint64_t>(block_copy_width) * block_copy_height * 16u;
				std::vector<uint8_t> source(static_cast<size_t>(bytes));
				UtilFillBuffer(ctx, source.data(), bytes, block_copy_width, src_obj, static_cast<uint64_t>(src_obj->layout));
				uint64_t nonzero = 0;
				uint64_t hash    = 1469598103934665603ull;
				for (uint8_t value: source)
				{
					nonzero += (value != 0u ? 1u : 0u);
					hash = (hash ^ value) * 1099511628211ull;
				}
				std::fprintf(stderr,
				             "KYTY_DUMP_BLOCK_STORAGE extent=%ux%u bytes=%" PRIu64 " nonzero=%" PRIu64 " hash=%016" PRIx64 "\n",
				             block_copy_width, block_copy_height, bytes, nonzero, hash);
				static std::atomic_bool dds_dumped {false};
				if (block_copy_width == 29u && block_copy_height == 30u && !dds_dumped.exchange(true))
				{
					uint32_t dds[31] {};
					dds[0]  = 124u;
					dds[1]  = 0x00081007u;
					dds[2]  = static_cast<uint32_t>(height);
					dds[3]  = static_cast<uint32_t>(width);
					dds[4]  = static_cast<uint32_t>(bytes);
					dds[6]  = 1u;
					dds[18] = 32u;
					dds[19] = 4u;
					dds[20] = 0x35545844u; // DXT5
					dds[26] = 0x1000u;
					if (FILE* file = std::fopen("/tmp/kyty-block-storage-116x120.dds", "wb"); file != nullptr)
					{
						std::fwrite("DDS ", 1, 4, file);
						std::fwrite(dds, sizeof(dds), 1, file);
						std::fwrite(source.data(), 1, source.size(), file);
						std::fclose(file);
					}
				}
			}
			// The source extent is expressed in uncompressed uint4 texels.
			// vkCmdCopyImage scales it to the BC3 destination's 4x4 block extent.
			regions[0].src_image = src_obj;
			regions[0].src_level = 0;
			regions[0].dst_level = 0;
			regions[0].width     = block_copy_width;
			regions[0].height    = block_copy_height;
			regions[0].src_x     = 0;
			regions[0].src_y     = 0;
			regions[0].dst_x     = 0;
			regions[0].dst_y     = 0;
		} else
		{
		// Single ST parent: exact sample extent, or a larger atlas that can host
		// mip offsets (legacy GenerateMips-style). Wrong-format parents rejected.
		const bool st_ok =
		    src_obj != nullptr && src_guest_extent.width >= width && src_guest_extent.height >= height &&
		    (fmt == 0u || Gen5SampleMayCopyFromSurfaceParent(static_cast<uint32_t>(fmt), src_obj->format));
		if (!st_ok)
		{
			skip_surface_copy();
			return;
		}

		for (uint32_t i = 0; i < levels; i++)
		{
			auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);

			regions[i].src_image = src_obj;
			regions[i].src_level = 0;
			regions[i].dst_level = i;
			regions[i].width     = mip_width;
			regions[i].height    = mip_height;
			regions[i].src_x     = mipmap_offset.first;
			regions[i].src_y     = mipmap_offset.second;
			regions[i].dst_x     = 0;
			regions[i].dst_y     = 0;

			if (mip_width > 1)
			{
				mip_width /= 2;
			}
			if (mip_height > 1)
			{
				mip_height /= 2;
			}
		}
		}
	} else if (levels == objects.Size() && scenario == GpuMemoryScenario::Common)
	{
		bool parents_ok = true;
		uint32_t check_w = static_cast<uint32_t>(width);
		uint32_t check_h = static_cast<uint32_t>(height);
		for (uint32_t i = 0; i < levels; i++)
		{
			const auto& object = objects.At(i);
			if (object.type != GpuMemoryObjectType::RenderTexture)
			{
				parents_ok = false;
				break;
			}
			auto* src_obj = static_cast<RenderTextureVulkanImage*>(object.obj);
			if (!surface_parent_ok(src_obj, check_w, check_h))
			{
				parents_ok = false;
				break;
			}
			if (check_w > 1)
			{
				check_w /= 2;
			}
			if (check_h > 1)
			{
				check_h /= 2;
			}
		}
		if (!parents_ok)
		{
			skip_surface_copy();
			return;
		}

		for (uint32_t i = 0; i < levels; i++)
		{
			const auto& object  = objects.At(i);
			auto*       src_obj = static_cast<RenderTextureVulkanImage*>(object.obj);

			regions[i].src_image = src_obj;
			regions[i].src_level = 0;
			regions[i].dst_level = i;
			regions[i].width     = mip_width;
			regions[i].height    = mip_height;
			regions[i].src_x     = 0;
			regions[i].src_y     = 0;
			regions[i].dst_x     = 0;
			regions[i].dst_y     = 0;

			if (mip_width > 1)
			{
				mip_width /= 2;
			}
			if (mip_height > 1)
			{
				mip_height /= 2;
			}
		}
		//	} else if (objects.Size() >= 2 && objects.At(0).type == GpuMemoryObjectType::StorageBuffer &&
		//	           objects.At(1).type == GpuMemoryObjectType::StorageTexture && scenario == GpuMemoryScenario::GenerateMips)
	} else if (objects.Size() >= 3 && objects.At(0).type == GpuMemoryObjectType::StorageBuffer &&
	           objects.At(1).type == GpuMemoryObjectType::Texture && objects.At(2).type == GpuMemoryObjectType::StorageTexture &&
	           scenario == GpuMemoryScenario::GenerateMips)
	{
		for (uint32_t i = 0; i < levels; i++)
		{
			VulkanImage* src_image = nullptr;
			bool         storage   = false;

			for (const auto& o: objects)
			{
				if (o.type == GpuMemoryObjectType::StorageTexture)
				{
					auto* src_obj = static_cast<StorageTextureVulkanImage*>(o.obj);
					if (src_obj->MatchesGuestExtent(mip_width, mip_height))
					{
						src_image = src_obj;
						storage   = true;
						break;
					}
				} else if (o.type == GpuMemoryObjectType::RenderTexture)
				{
					auto* src_obj = static_cast<RenderTextureVulkanImage*>(o.obj);
					if (src_obj->MatchesGuestExtent(mip_width, mip_height))
					{
						src_image = src_obj;
						storage   = false;
						break;
					}
				}
			}

			EXIT_NOT_IMPLEMENTED(src_image == nullptr);

			if (storage)
			{
				auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);

				regions[i].src_image = src_image;
				regions[i].src_level = 0;
				regions[i].dst_level = i;
				regions[i].width     = mip_width;
				regions[i].height    = mip_height;
				regions[i].src_x     = mipmap_offset.first;
				regions[i].src_y     = mipmap_offset.second;
				regions[i].dst_x     = 0;
				regions[i].dst_y     = 0;
			} else
			{
				regions[i].src_image = src_image;
				regions[i].src_level = 0;
				regions[i].dst_level = i;
				regions[i].width     = mip_width;
				regions[i].height    = mip_height;
				regions[i].src_x     = 0;
				regions[i].src_y     = 0;
				regions[i].dst_x     = 0;
				regions[i].dst_y     = 0;
			}

			if (mip_width > 1)
			{
				mip_width /= 2;
			}
			if (mip_height > 1)
			{
				mip_height /= 2;
			}
		}
	} else
	{
		// Mixed multi-parent graphs (RT/ST + StorageBuffer/VertexBuffer peers)
		// reach CreateFromObjects when a Texture sample range sits under a live
		// surface. Copy only exact-extent, format-compatible parents; ignore
		// peers, wrong-family RTs, and larger parents without crop views.
		for (uint32_t i = 0; i < levels; i++)
		{
			VulkanImage* src_image = nullptr;
			bool         storage   = false;

			for (const auto& o: objects)
			{
				if (o.type == GpuMemoryObjectType::StorageTexture)
				{
					auto* src_obj = static_cast<StorageTextureVulkanImage*>(o.obj);
					if (surface_parent_ok(src_obj, mip_width, mip_height))
					{
						src_image = src_obj;
						storage   = true;
						break;
					}
				} else if (o.type == GpuMemoryObjectType::RenderTexture)
				{
					auto* src_obj = static_cast<RenderTextureVulkanImage*>(o.obj);
					if (surface_parent_ok(src_obj, mip_width, mip_height))
					{
						src_image = src_obj;
						storage   = false;
						break;
					}
				}
			}

			if (src_image == nullptr)
			{
				// No exact-extent surface parent. Leave UNDEFINED so GpuMemory
				// can guest-upload package tiles (not a full-screen RT blit).
				skip_surface_copy();
				return;
			}

			if (storage)
			{
				auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);

				regions[i].src_image = src_image;
				regions[i].src_level = 0;
				regions[i].dst_level = i;
				regions[i].width     = mip_width;
				regions[i].height    = mip_height;
				regions[i].src_x     = mipmap_offset.first;
				regions[i].src_y     = mipmap_offset.second;
				regions[i].dst_x     = 0;
				regions[i].dst_y     = 0;
			} else
			{
				regions[i].src_image = src_image;
				regions[i].src_level = 0;
				regions[i].dst_level = i;
				regions[i].width     = mip_width;
				regions[i].height    = mip_height;
				regions[i].src_x     = 0;
				regions[i].src_y     = 0;
				regions[i].dst_x     = 0;
				regions[i].dst_y     = 0;
			}

			if (mip_width > 1)
			{
				mip_width /= 2;
			}
			if (mip_height > 1)
			{
				mip_height /= 2;
			}
		}
	}

	if (buffer == nullptr)
	{
		UtilFillImage(ctx, regions, vk_obj, static_cast<uint64_t>(vk_layout));
	} else
	{
		UtilImageToImage(buffer, regions, vk_obj, static_cast<uint64_t>(vk_layout));
	}
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("TextureObject::Create");

	EXIT_IF(size == nullptr || vaddr == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);

	auto fmt        = (params[TextureObject::PARAM_FORMAT] >> 16u) & 0xffffu;
	auto dfmt       = (params[TextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto nfmt       = (params[TextureObject::PARAM_FORMAT]) & 0xffu;
	auto width      = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height     = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto base_level = params[TextureObject::PARAM_LEVELS] >> 32u;
	auto levels     = params[TextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto swizzle    = params[TextureObject::PARAM_SWIZZLE];
	auto resource_info = params[TextureObject::PARAM_RESOURCE_INFO];
	auto resource_type = TextureObject::GetResourceType(resource_info);
	auto depth      = TextureObject::GetResourceDepth(resource_info);
	auto base_array = TextureObject::GetResourceBaseArray(resource_info);
	auto tile       = params[TextureObject::PARAM_TILE];
	auto force_degamma = params[TextureObject::PARAM_FORCE_DEGAMMA] != 0;
	const bool three_dimensional = resource_type == 10u;
	const bool arrayed_2d = resource_type == 13u;
	EXIT_NOT_IMPLEMENTED(resource_type != 8u && resource_type != 9u && resource_type != 13u && !three_dimensional);

	VkImageUsageFlags vk_usage = get_usage();

	VkComponentMapping components {};

	components.r = get_swizzle(GetDstSel(swizzle, 0));
	components.g = get_swizzle(GetDstSel(swizzle, 1));
	components.b = get_swizzle(GetDstSel(swizzle, 2));
	components.a = get_swizzle(GetDstSel(swizzle, 3));

	auto pixel_format = TextureResolveSampledVkFormat(dfmt, nfmt, fmt, force_degamma);

	EXIT_NOT_IMPLEMENTED(pixel_format == VK_FORMAT_UNDEFINED);
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);
	EXIT_NOT_IMPLEMENTED(three_dimensional && depth == 0u);

	auto* vk_obj = new TextureVulkanImage;

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = (three_dimensional ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D);
	image_info.extent.width  = width;
	image_info.extent.height = height;
	image_info.extent.depth  = (three_dimensional ? depth : 1u);
	image_info.mipLevels     = levels;
	image_info.arrayLayers   = (arrayed_2d ? depth : 1u);
	image_info.format        = pixel_format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	image_info.usage         = vk_usage;
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
	bool astc10x5_cpu_fallback = false;

	if (!CheckSwizzle(ctx, &image_info, &components))
	{
		EXIT("swizzle is not supported");
	}

	if (!CheckFormat(ctx, &image_info))
	{
		if (fmt == 173u && tile == 5u && !three_dimensional && !arrayed_2d)
			{
				image_info.format            = VK_FORMAT_R8G8B8A8_UNORM;
				astc10x5_cpu_fallback = CheckFormat(ctx, &image_info);
			}
			if (!astc10x5_cpu_fallback)
			{
				EXIT("format is not supported: fmt=%u vk=%u tile=%u size=%ux%u type=%u depth=%u\n", static_cast<unsigned>(fmt),
				     static_cast<unsigned>(image_info.format), static_cast<unsigned>(tile), static_cast<unsigned>(width),
				     static_cast<unsigned>(height), static_cast<unsigned>(resource_type), static_cast<unsigned>(depth));
			}
		}

	vk_obj->SetNativeExtent(width, height);
	vk_obj->format                = image_info.format;
	vk_obj->astc10x5_cpu_fallback = astc10x5_cpu_fallback;
	vk_obj->image         = nullptr;
	vk_obj->layout        = image_info.initialLayout;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);

	EXIT_NOT_IMPLEMENTED(vk_obj->image == nullptr);

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem->requirements);

	mem->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, mem);

	vk_obj->memory = *mem;

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = nullptr;
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = (three_dimensional ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D);
	create_info.format                          = vk_obj->format;
	create_info.components                      = components;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = base_level;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;

	const int view_index = (three_dimensional ? VulkanImage::VIEW_3D : VulkanImage::VIEW_DEFAULT);
	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[view_index]);
	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[view_index] == nullptr);
	if (!three_dimensional)
	{
		create_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		create_info.subresourceRange.baseArrayLayer = (arrayed_2d ? base_array : 0u);
		create_info.subresourceRange.layerCount = (arrayed_2d ? depth - base_array : 1u);
		vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_ARRAY]);
		EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_ARRAY] == nullptr);
	}

	return vk_obj;
}

static void* create2_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, GpuMemoryScenario scenario,
                          const Vector<GpuMemoryObject>& objects, VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("TextureObject::CreateFromObjects");

	EXIT_IF(objects.IsEmpty());
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);

	auto fmt        = (params[TextureObject::PARAM_FORMAT] >> 16u) & 0xffffu;
	auto dfmt       = (params[TextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto nfmt       = (params[TextureObject::PARAM_FORMAT]) & 0xffu;
	auto width      = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height     = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto base_level = params[TextureObject::PARAM_LEVELS] >> 32u;
	auto levels     = params[TextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto tile       = params[TextureObject::PARAM_TILE];
	auto swizzle    = params[TextureObject::PARAM_SWIZZLE];
	auto resource_info = params[TextureObject::PARAM_RESOURCE_INFO];
	auto resource_type = TextureObject::GetResourceType(resource_info);
	auto depth      = TextureObject::GetResourceDepth(resource_info);
	auto force_degamma = params[TextureObject::PARAM_FORCE_DEGAMMA] != 0;
	const bool three_dimensional = resource_type == 10u;
	EXIT_NOT_IMPLEMENTED(resource_type != 8u && resource_type != 9u && resource_type != 13u && !three_dimensional);

	VkImageUsageFlags vk_usage = get_usage();

	VkComponentMapping components {};

	components.r = get_swizzle(GetDstSel(swizzle, 0));
	components.g = get_swizzle(GetDstSel(swizzle, 1));
	components.b = get_swizzle(GetDstSel(swizzle, 2));
	components.a = get_swizzle(GetDstSel(swizzle, 3));

	auto pixel_format = TextureResolveSampledVkFormat(dfmt, nfmt, fmt, force_degamma);

	EXIT_NOT_IMPLEMENTED(pixel_format == VK_FORMAT_UNDEFINED);
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);
	EXIT_NOT_IMPLEMENTED(three_dimensional && depth == 0u);

	auto* vk_obj = new TextureVulkanImage;

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = (three_dimensional ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D);
	image_info.extent.width  = width;
	image_info.extent.height = height;
	image_info.extent.depth  = depth;
	image_info.mipLevels     = levels;
	image_info.arrayLayers   = 1;
	image_info.format        = pixel_format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	image_info.usage         = vk_usage;
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
	bool astc10x5_cpu_fallback = false;

	if (!CheckSwizzle(ctx, &image_info, &components))
	{
		EXIT("swizzle is not supported");
	}

	if (!CheckFormat(ctx, &image_info))
	{
		if (fmt == 173u && tile == 5u && !three_dimensional && resource_type != 13u)
		{
			image_info.format            = VK_FORMAT_R8G8B8A8_UNORM;
			astc10x5_cpu_fallback = CheckFormat(ctx, &image_info);
		}
		if (!astc10x5_cpu_fallback)
		{
			EXIT("format is not supported: fmt=%u dfmt=%u nfmt=%u size=%ux%u resource_type=%u\n", static_cast<unsigned>(fmt),
			     static_cast<unsigned>(dfmt), static_cast<unsigned>(nfmt), static_cast<unsigned>(width), static_cast<unsigned>(height),
			     static_cast<unsigned>(resource_type));
		}
	}

	vk_obj->SetNativeExtent(width, height);
	vk_obj->format                 = image_info.format;
	vk_obj->astc10x5_cpu_fallback = astc10x5_cpu_fallback;
	vk_obj->image                  = nullptr;
	vk_obj->layout                 = image_info.initialLayout;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);

	EXIT_NOT_IMPLEMENTED(vk_obj->image == nullptr);

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem->requirements);

	mem->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, mem);

	vk_obj->memory = *mem;

	update2_func(ctx, buffer, params, vk_obj, scenario, objects);

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = nullptr;
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = (three_dimensional ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D);
	create_info.format                          = vk_obj->format;
	create_info.components                      = components;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = base_level;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;

	const int view_index = (three_dimensional ? VulkanImage::VIEW_3D : VulkanImage::VIEW_DEFAULT);
	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[view_index]);
	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[view_index] == nullptr);
	if (!three_dimensional)
	{
		create_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_ARRAY]);
		EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_ARRAY] == nullptr);
	}

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("TextureObject::delete_func");

	auto* vk_obj = reinterpret_cast<TextureVulkanImage*>(obj);

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

bool TextureObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_PITCH] == other[PARAM_PITCH] &&
	        params[PARAM_WIDTH_HEIGHT] == other[PARAM_WIDTH_HEIGHT] && params[PARAM_LEVELS] == other[PARAM_LEVELS] &&
	        params[PARAM_TILE] == other[PARAM_TILE] && params[PARAM_NEO] == other[PARAM_NEO] &&
	        params[PARAM_SWIZZLE] == other[PARAM_SWIZZLE] && params[PARAM_FORCE_DEGAMMA] == other[PARAM_FORCE_DEGAMMA] &&
	        params[PARAM_SKIP_GUEST_UPLOAD] == other[PARAM_SKIP_GUEST_UPLOAD] &&
	        params[PARAM_RESOURCE_INFO] == other[PARAM_RESOURCE_INFO]);
}

GpuObject::create_func_t TextureObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::create_from_objects_func_t TextureObject::GetCreateFromObjectsFunc() const
{
	return create2_func;
}

GpuObject::delete_func_t TextureObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t TextureObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Kyty::Libs::Graphics

#endif
