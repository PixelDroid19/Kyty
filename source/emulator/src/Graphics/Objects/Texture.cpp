#include "Emulator/Graphics/Objects/Texture.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/Gen5TextureArrayLayout.h"
#include "Emulator/Graphics/Gen5TextureMipLayout.h"
#include "Emulator/Graphics/Gen5TextureVolumeLayout.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GuestTextureLayout.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"
#include "Emulator/Log.h"

// IWYU pragma: no_forward_declare VkImageView_T

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static VkImageUsageFlags get_usage()
{
	VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	vk_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

	return vk_usage;
}

static void update_func(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	KYTY_PROFILER_BLOCK("TextureObject::update_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = static_cast<TextureVulkanImage*>(obj);

	auto       tile              = params[TextureObject::PARAM_TILE];
	auto       fmt               = (params[TextureObject::PARAM_FORMAT] >> 16u) & 0xffffu;
	auto       dfmt              = (params[TextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto       nfmt              = (params[TextureObject::PARAM_FORMAT]) & 0xffu;
	auto       width             = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto       height            = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto       levels            = params[TextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto       pitch             = params[TextureObject::PARAM_PITCH];
	auto       resource_info     = params[TextureObject::PARAM_RESOURCE_INFO];
	auto       resource_type     = TextureObject::GetResourceType(resource_info);
	auto       depth             = TextureObject::GetResourceDepth(resource_info);
	auto       base_array        = TextureObject::GetResourceBaseArray(resource_info);
	bool       neo               = Config::IsNeo();
	const bool skip_guest        = params[TextureObject::PARAM_SKIP_GUEST_UPLOAD] != 0;
	const bool three_dimensional = resource_type == 10u;
	const bool arrayed_2d        = resource_type == 13u;

	VkImageLayout vk_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	// SKIPPED: levels >= 16
	if (levels >= 16)
	{
		KYTY_LOG_DEBUG("WARNING: skipped check: levels >= 16\n");
	}
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
	if (arrayed_2d && tile != 24u)
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
	if (fmt != 0u && tile == 5u && levels > 1u)
	{
		Gen5TextureMipLayout mip_layout {};
		EXIT_NOT_IMPLEMENTED(!Gen5GetStandard4KBTextureMipLayout(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width),
		                                                         static_cast<uint32_t>(height), static_cast<uint32_t>(pitch),
		                                                         static_cast<uint32_t>(levels), &mip_layout));
		EXIT_NOT_IMPLEMENTED(*size != mip_layout.tiled.size);

		std::vector<uint8_t> linear(static_cast<size_t>(mip_layout.linear_size));
		EXIT_NOT_IMPLEMENTED(
		    !Gen5DetileStandard4KBTextureMipChain(linear.data(), linear.size(), reinterpret_cast<const void*>(*vaddr), *size, mip_layout));

		const char* block_dump_spec   = std::getenv("KYTY_DUMP_TILED_BLOCKS");
		uint32_t    block_dump_width  = 0;
		uint32_t    block_dump_height = 0;
		const bool  block_dump_matches = ShaderGen5TextureIsBlockCompressed(static_cast<uint32_t>(fmt)) &&
		                                 block_dump_spec != nullptr &&
		                                 (std::sscanf(block_dump_spec, "%ux%u", &block_dump_width, &block_dump_height) != 2 ||
		                                  (block_dump_width == width && block_dump_height == height));
		if (block_dump_matches)
		{
			const auto& level = mip_layout.level[0];
			EXIT_NOT_IMPLEMENTED(static_cast<uint64_t>(level.linear_offset) + level.linear_size > linear.size());
			const auto* bytes = linear.data() + level.linear_offset;
			uint64_t    hash  = 1469598103934665603ull;
			for (uint32_t i = 0; i < level.linear_size; ++i)
			{
				hash = (hash ^ bytes[i]) * 1099511628211ull;
			}
			static std::set<uint64_t> dumped_mip_blocks;
			const uint64_t dump_key = *vaddr ^ (static_cast<uint64_t>(width) << 32u) ^ height ^ hash;
			if (dumped_mip_blocks.size() < 32u && dumped_mip_blocks.insert(dump_key).second)
			{
				char path[192];
				std::snprintf(path, sizeof(path), "/tmp/kyty-bc-mip0-%ux%u-%012" PRIx64 "-%016" PRIx64 ".bin",
				              static_cast<uint32_t>(width), static_cast<uint32_t>(height), *vaddr, hash);
				if (FILE* file = std::fopen(path, "wb"); file != nullptr)
				{
					const size_t written = std::fwrite(bytes, 1, level.linear_size, file);
					std::fclose(file);
					KYTY_LOG_DEBUG(
					             "KYTY_DUMP_TILED_BLOCKS_FILE path=%s bytes=%zu complete=%u level=0 elem=%ux%u hash=%016" PRIx64 "\n",
					             path, written, written == level.linear_size ? 1u : 0u, level.element_width, level.element_height, hash);
				}
			}
		}

		Vector<BufferImageCopy> regions(static_cast<int>(levels));
		for (uint32_t level = 0; level < levels; level++)
		{
			const auto&    level_layout = mip_layout.level[level];
			const uint64_t row_pitch    = static_cast<uint64_t>(level_layout.element_width) * mip_layout.texels_per_element_x;
			EXIT_NOT_IMPLEMENTED(row_pitch == 0u || row_pitch > UINT32_MAX);
			regions[level].offset    = level_layout.linear_offset;
			regions[level].pitch     = static_cast<uint32_t>(row_pitch);
			regions[level].width     = level_layout.width;
			regions[level].height    = level_layout.height;
			regions[level].dst_level = level;
			regions[level].dst_x     = 0;
			regions[level].dst_y     = 0;
		}

		UtilFillImage(ctx, vk_obj, linear.data(), linear.size(), regions, static_cast<uint64_t>(vk_layout));
		return;
	}

	// GPU-owned range under a live color surface that could not be bound as an
	// alias: never detile guest (period-16 bands). Transparent black clear.
	static const char* skipped_dump_spec     = std::getenv("KYTY_DUMP_TILED_SAMPLE");
	uint32_t           skipped_dump_width    = 0;
	uint32_t           skipped_dump_height   = 0;
	const bool         inspect_skipped_guest = skipped_dump_spec != nullptr &&
	                                           std::sscanf(skipped_dump_spec, "%ux%u", &skipped_dump_width, &skipped_dump_height) == 2 &&
	                                           skipped_dump_width == width && skipped_dump_height == height;
	if (skip_guest && !inspect_skipped_guest)
	{
		const uint32_t bpp   = (fmt != 0u ? ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt)) : 4u);
		const uint64_t bytes = static_cast<uint64_t>(width) * height * bpp;
		// SKIPPED: bytes == 0u
		if (bytes == 0u)
		{
			KYTY_LOG_DEBUG("WARNING: skipped check: bytes == 0u\n");
		}
		std::vector<uint8_t>    zeros(static_cast<size_t>(bytes), 0);
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
		// Gen5: tile 0 = linear; 5 = kStandard4KB; 9 = kStandard64KB;
		// 24 = depth; 27 = render target.
		// Other modes remain unsupported until their layout is evidenced.
		if (tile != 0 && tile != 5 && tile != 9 && tile != 24 && tile != 27)
		{
			KYTY_LOG_DEBUG("WARNING: skipped check: tile != 0 && tile != 5 && tile != 27 && tile != 9\n");
		}

		TileGetTextureSize2(fmt, width, height, pitch, levels, tile, nullptr, level_sizes, nullptr);
	} else
	{
		// SKIPPED: tile != 8 && tile != 13 && tile != 10
		if (tile != 8 && tile != 13 && tile != 10)
		{
			KYTY_LOG_DEBUG("WARNING: skipped check: tile != 8 && tile != 13 && tile != 10\n");
		}

		TileGetTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, nullptr, level_sizes, nullptr);
	}

	// dbg_test_mipmaps(ctx, VK_FORMAT_BC3_SRGB_BLOCK, 512, 512);

	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	uint32_t mip_pitch  = pitch;

	Vector<BufferImageCopy> regions(levels);
	for (uint32_t i = 0; i < levels; i++)
	{
		// SKIPPED: level_sizes[i].size == 0
		if (level_sizes[i].size == 0)
		{
			KYTY_LOG_DEBUG("WARNING: skipped check: level_sizes[i].size == 0\n");
		}

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

	// Gen5 linear resources are already in the format consumed by Vulkan. The
	// video path uses this mode for both the luma (R8) and interleaved chroma
	// (R8G8) planes; leaving it without an upload creates a valid image backed
	// only by its allocation clear value, which appears as a solid blue frame.
	if (fmt != 0u && tile == 0u && levels == 1u)
	{
		if (!skip_guest)
		{
			const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
			uint32_t       upload_pitch      = static_cast<uint32_t>(pitch);
			if (bytes_per_element != 0u && width <= std::numeric_limits<uint32_t>::max() / bytes_per_element)
			{
				const uint32_t row_bytes = static_cast<uint32_t>(width) * bytes_per_element;
				if (const uint32_t registered_pitch = GuestTextureLayoutGetLinearRowPitch(*vaddr, row_bytes);
				    registered_pitch != 0u && registered_pitch % bytes_per_element == 0u)
				{
					upload_pitch = registered_pitch / bytes_per_element;
				}
			}
			regions[0].offset = 0;
			regions[0].width  = static_cast<uint32_t>(width);
			regions[0].height = static_cast<uint32_t>(height);
			regions[0].pitch  = upload_pitch;
			if (std::getenv("KYTY_DUMP_VIDEO_UPLOAD") != nullptr)
			{
				static std::atomic_uint dump_count {0};
				const auto           index = dump_count.fetch_add(1, std::memory_order_relaxed);
				if (index < 32u)
				{
					const auto* bytes = reinterpret_cast<const uint8_t*>(*vaddr);
					KYTY_LOG_DEBUG( "KYTY_DUMP_VIDEO_UPLOAD index=%u fmt=%u size=%ux%u pitch=%u upload_pitch=%u addr=0x%012" PRIx64
					             " bytes=%" PRIu64 " first=%02x%02x%02x%02x\n",
					             index, static_cast<unsigned>(fmt), static_cast<unsigned>(width), static_cast<unsigned>(height),
					             static_cast<unsigned>(pitch), static_cast<unsigned>(upload_pitch), static_cast<uint64_t>(*vaddr),
					             static_cast<uint64_t>(*size), bytes[0], bytes[1], bytes[2], bytes[3]);
				}
			}
			UtilFillImage(ctx, vk_obj, reinterpret_cast<const void*>(*vaddr), *size, regions, static_cast<uint64_t>(vk_layout));
			if (std::getenv("KYTY_DUMP_VIDEO_READBACK") != nullptr)
			{
				static std::atomic_uint readback_count {0};
				const auto             index = readback_count.fetch_add(1, std::memory_order_relaxed);
				if (index < 4u)
				{
					std::vector<uint8_t> readback(static_cast<size_t>(*size));
					UtilFillBuffer(ctx, readback.data(), *size, static_cast<uint32_t>(width), vk_obj, static_cast<uint64_t>(vk_layout));
					uint64_t hash = 1469598103934665603ull;
					uint8_t  minimum = 255u;
					uint8_t  maximum = 0u;
					for (uint8_t value: readback)
					{
						hash = (hash ^ value) * 1099511628211ull;
						minimum = std::min(minimum, value);
						maximum = std::max(maximum, value);
					}
					KYTY_LOG_DEBUG( "KYTY_DUMP_VIDEO_READBACK index=%u fmt=%u bytes=%zu first=%02x%02x%02x%02x min=%u max=%u hash=%016" PRIx64 "\n",
					             index, static_cast<unsigned>(fmt), readback.size(), readback.size() > 0 ? readback[0] : 0u,
					             readback.size() > 1 ? readback[1] : 0u, readback.size() > 2 ? readback[2] : 0u,
					             readback.size() > 3 ? readback[3] : 0u, static_cast<unsigned>(minimum), static_cast<unsigned>(maximum), hash);
				}
			}
		}
		return;
	}

	if (fmt == 0)
	{
		if (tile == 13)
		{
			// SKIPPED: pitch != width
			if (pitch != width)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: pitch != width\n");
			}
			// SKIPPED: fmt != 0
			if (fmt != 0)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: fmt != 0\n");
			}
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
			// SKIPPED: !(dfmt == 10 && nfmt == 0)
			if (!(dfmt == 10 && nfmt == 0))
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: !(dfmt == 10 && nfmt == 0)\n");
			}
			// SKIPPED: levels != 1
			if (levels != 1)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: levels != 1\n");
			}
			const uint64_t linear_bytes = static_cast<uint64_t>(width) * height * 4u;
			// SKIPPED: linear_bytes == 0
			if (linear_bytes == 0)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: linear_bytes == 0\n");
			}
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
			const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
			if (bytes_per_element != 0u && width <= std::numeric_limits<uint32_t>::max() / bytes_per_element)
			{
				const uint32_t row_bytes = static_cast<uint32_t>(width) * bytes_per_element;
				if (const uint32_t registered_pitch = GuestTextureLayoutGetLinearRowPitch(*vaddr, row_bytes);
				    registered_pitch != 0u && registered_pitch % bytes_per_element == 0u)
				{
					regions[0].pitch = registered_pitch / bytes_per_element;
				}
			}
			// Opt-in dump for linear Gen5 sample investigation (scratch only).
			// KYTY_DUMP_LINEAR_SAMPLE=WxH writes one RGBA8 PNG under /tmp.
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
							std::snprintf(out_path, sizeof(out_path), "/tmp/kyty-dump-linear-%ux%u.png", static_cast<unsigned>(width),
							              static_cast<unsigned>(height));
							char out_path_w[128];
							std::snprintf(out_path_w, sizeof(out_path_w), "/tmp/kyty-dump-linear-%ux%u-widthpitch.png",
							              static_cast<unsigned>(width), static_cast<unsigned>(height));
							const auto* base = reinterpret_cast<const uint8_t*>(*vaddr);
							UtilWriteRgba8Png(out_path, base, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
							                  static_cast<uint32_t>(pitch));
							UtilWriteRgba8Png(out_path_w, base, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
							                  static_cast<uint32_t>(width));
							KYTY_LOG_DEBUG("KYTY_DUMP_LINEAR_SAMPLE wrote %ux%u pitch=%u -> %s\n", static_cast<unsigned>(width),
							       static_cast<unsigned>(height), static_cast<unsigned>(pitch), out_path);
						}
					}
				}
			}
			UtilFillImage(ctx, vk_obj, reinterpret_cast<void*>(*vaddr), *size, regions, static_cast<uint64_t>(vk_layout));
		} else if (tile == 5)
		{
			const uint32_t bytes_per_element = ShaderGen5TextureBytesPerElement(static_cast<uint32_t>(fmt));
			const bool     block_compressed  = ShaderGen5TextureIsBlockCompressed(static_cast<uint32_t>(fmt));
			// SKIPPED: bytes_per_element == 0u || levels != 1u
			if (bytes_per_element == 0u || levels != 1u)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: bytes_per_element == 0u || levels != 1u\n");
			}
			const uint32_t element_width  = block_compressed ? (width + 3u) / 4u : width;
			const uint32_t element_height = block_compressed ? (height + 3u) / 4u : height;
			const uint32_t element_pitch  = block_compressed ? (pitch + 3u) / 4u : pitch;
			const uint64_t linear_bytes   = static_cast<uint64_t>(element_pitch) * element_height * bytes_per_element;
			// SKIPPED: linear_bytes == 0u || linear_bytes > *size
			if (linear_bytes == 0u || linear_bytes > *size)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: linear_bytes == 0u || linear_bytes > *size\n");
			}
			std::vector<uint8_t> temp_buf(static_cast<size_t>(linear_bytes));
			TileConvertStandard4KBToLinear(temp_buf.data(), reinterpret_cast<void*>(*vaddr), element_width, element_height, element_pitch,
			                               bytes_per_element);
			const char* block_dump_spec = std::getenv("KYTY_DUMP_TILED_BLOCKS");
			uint32_t    block_dump_width = 0;
			uint32_t    block_dump_height = 0;
			const bool  block_dump_matches =
			    block_compressed && block_dump_spec != nullptr &&
			    (std::sscanf(block_dump_spec, "%ux%u", &block_dump_width, &block_dump_height) != 2 ||
			     (block_dump_width == width && block_dump_height == height));
			if (block_dump_matches)
			{
				const auto* guest           = reinterpret_cast<const uint8_t*>(*vaddr);
				uint64_t    raw_nonzero     = 0;
				uint64_t    detiled_nonzero = 0;
				uint64_t    raw_hash        = 1469598103934665603ull;
				uint64_t    detiled_hash    = 1469598103934665603ull;
				for (uint64_t i = 0; i < linear_bytes; ++i)
				{
					raw_nonzero += (guest[i] != 0u ? 1u : 0u);
					detiled_nonzero += (temp_buf[i] != 0u ? 1u : 0u);
					raw_hash     = (raw_hash ^ guest[i]) * 1099511628211ull;
					detiled_hash = (detiled_hash ^ temp_buf[i]) * 1099511628211ull;
				}
				KYTY_LOG_DEBUG(
				             "KYTY_DUMP_TILED_BLOCKS addr=0x%012" PRIx64 " size=%" PRIu64 " elem=%ux%u pitch=%u raw_nonzero=%" PRIu64
				             " detiled_nonzero=%" PRIu64 " raw_hash=%016" PRIx64 " detiled_hash=%016" PRIx64 " raw=",
				             *vaddr, linear_bytes, element_width, element_height, element_pitch, raw_nonzero, detiled_nonzero, raw_hash,
				             detiled_hash);
				for (uint32_t i = 0; i < 64u && i < linear_bytes; ++i)
				{
					KYTY_LOG_DEBUG( "%02x", guest[i]);
				}
				KYTY_LOG_DEBUG( " detiled=");
				for (uint32_t i = 0; i < 64u && i < linear_bytes; ++i)
				{
					KYTY_LOG_DEBUG( "%02x", temp_buf[i]);
				}
				KYTY_LOG_DEBUG( "\n");

				static std::set<uint64_t> dumped_blocks;
				const uint64_t dump_key = *vaddr ^ (static_cast<uint64_t>(width) << 32u) ^ height ^ detiled_hash;
				if (dumped_blocks.size() < 32u && dumped_blocks.insert(dump_key).second)
				{
					char path[192];
					std::snprintf(path, sizeof(path), "/tmp/kyty-bc-blocks-%ux%u-%012" PRIx64 "-%016" PRIx64 ".bin",
					              static_cast<uint32_t>(width), static_cast<uint32_t>(height), *vaddr, detiled_hash);
					if (FILE* file = std::fopen(path, "wb"); file != nullptr)
					{
						const size_t written = std::fwrite(temp_buf.data(), 1, static_cast<size_t>(linear_bytes), file);
						std::fclose(file);
						KYTY_LOG_DEBUG( "KYTY_DUMP_TILED_BLOCKS_FILE path=%s bytes=%zu complete=%u\n", path, written,
						             written == linear_bytes ? 1u : 0u);
					}
				}
			}
			regions[0].offset = 0;
			regions[0].width  = width;
			regions[0].height = height;
			regions[0].pitch  = pitch;
			UtilFillImage(ctx, vk_obj, temp_buf.data(), linear_bytes, regions, static_cast<uint64_t>(vk_layout));
		} else if (tile == 24)
		{
			EXIT_NOT_IMPLEMENTED(fmt != 22u || levels != 1u);
			const uint64_t linear_bytes = static_cast<uint64_t>(width) * height * 4u;
			EXIT_NOT_IMPLEMENTED(linear_bytes == 0u || linear_bytes > *size);
			std::vector<uint8_t> linear(static_cast<size_t>(linear_bytes));
			TileConvertDepth64KB32ToLinear(linear.data(), reinterpret_cast<const void*>(*vaddr), static_cast<uint32_t>(width),
			                               static_cast<uint32_t>(height), static_cast<uint32_t>(pitch));
			regions[0].offset = 0;
			regions[0].width  = static_cast<uint32_t>(width);
			regions[0].height = static_cast<uint32_t>(height);
			regions[0].pitch  = static_cast<uint32_t>(width);
			UtilFillImage(ctx, vk_obj, linear.data(), linear.size(), regions, static_cast<uint64_t>(vk_layout));
		} else if (tile == 27 || tile == 9)
		{
			// Tiled sample texture: detile into tightly packed linear rows then
			// upload. Render-target aliases still prefer FindRenderTexture
			// before create; this path covers pure CPU-backed sample textures.
			// tile 27 = kRenderTarget layout; tile 9 = kStandard64KB (RGBA8).
			// BC1 (fmt 133) detiles compressed 4x4 blocks as 8-byte elements on
			// tile 27 only.
			// SKIPPED: tile == 9 && fmt != 56
			if (tile == 9 && fmt != 56)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: tile == 9 && fmt != 56\n");
			}
			// SKIPPED: fmt != 56 && fmt != 133
			if (fmt != 56 && fmt != 133)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: fmt != 56 && fmt != 133\n");
			}
			// SKIPPED: levels != 1
			if (levels != 1)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: levels != 1\n");
			}
			const bool bc1 = (fmt == 133u);
			// SKIPPED: bc1 && tile != 27
			if (bc1 && tile != 27)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: bc1 && tile != 27\n");
			}
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
						const uint64_t tiled = (tile == 9) ? TileGetStandard64KBOffset(x, y, pitch_elems, bpp) :
						                                         TileGetSw64kRxOffset(x, y, pitch_elems, bpp);
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
				static const char* dump_spec   = std::getenv("KYTY_DUMP_TILED_SAMPLE");
				uint32_t           dump_width  = 0;
				uint32_t           dump_height = 0;
				if (dump_spec != nullptr && std::sscanf(dump_spec, "%ux%u", &dump_width, &dump_height) == 2 && dump_width == width &&
				    dump_height == height)
				{
					static std::set<uint64_t> dumped;
					const uint64_t            key = (static_cast<uint64_t>(width) << 32u) | height;
					if (dumped.insert(key).second)
					{
						const auto*        pixels = reinterpret_cast<const uint32_t*>(temp_buf);
						const size_t       count  = static_cast<size_t>(linear_bytes / sizeof(uint32_t));
						std::set<uint32_t> colors;
						for (size_t pixel = 0; pixel < count && colors.size() <= 256u; pixel++)
						{
							colors.insert(pixels[pixel]);
						}
						KYTY_LOG_DEBUG(
						             "KYTY_DUMP_TILED_SAMPLE guest=0x%016" PRIx64 " size=%ux%u pitch=%u first=0x%08x "
						             "colors=%zu%s raw0=0x%08x raw1=0x%08x\n",
						             static_cast<uint64_t>(*vaddr), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
						             pitch_elems, count > 0 ? pixels[0] : 0u, colors.size(), colors.size() > 256u ? "+" : "",
						             *size >= 4u ? *reinterpret_cast<const uint32_t*>(*vaddr) : 0u,
						             *size >= 8u ? *(reinterpret_cast<const uint32_t*>(*vaddr) + 1) : 0u);
						UtilDumpVulkanImageRgba8Png(ctx, vk_obj, "/tmp/kyty-dump-tiled-sample", "guest");
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

	// SKIPPED: levels >= 16
	if (levels >= 16)
	{
		KYTY_LOG_DEBUG("WARNING: skipped check: levels >= 16\n");
	}

	uint32_t mip_width  = width;
	uint32_t mip_height = height;

	Vector<ImageImageCopy> regions(levels);

	auto fmt = (params[TextureObject::PARAM_FORMAT] >> 16u) & 0xffffu;

	static const char* update2_dump_spec   = std::getenv("KYTY_DUMP_TILED_SAMPLE");
	uint32_t           update2_dump_width  = 0;
	uint32_t           update2_dump_height = 0;
	const bool         dump_update2        = update2_dump_spec != nullptr &&
	                                         std::sscanf(update2_dump_spec, "%ux%u", &update2_dump_width, &update2_dump_height) == 2 &&
	                                         update2_dump_width == width && update2_dump_height == height;
	if (dump_update2)
	{
		static std::atomic_uint dump_count {0};
		const auto              current = dump_count.fetch_add(1, std::memory_order_relaxed);
		if (current < 8u)
		{
			KYTY_LOG_DEBUG( "KYTY_DUMP_TILED_SAMPLE update2 size=%ux%u fmt=%u levels=%u scenario=%u parents=%u\n",
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
				const auto  extent = image != nullptr ? image->GetGuestExtent() : VkExtent2D {};
				KYTY_LOG_DEBUG( "  parent[%u] type=%u obj=%p extent=%ux%u format=%u layout=%u guest_size=%" PRIu64 "\n",
				             static_cast<unsigned>(i), static_cast<unsigned>(parent.type), parent.obj, static_cast<unsigned>(extent.width),
				             static_cast<unsigned>(extent.height),
				             static_cast<unsigned>(image != nullptr ? image->format : VK_FORMAT_UNDEFINED),
				             static_cast<unsigned>(image != nullptr ? image->layout : VK_IMAGE_LAYOUT_UNDEFINED),
				             image != nullptr ? image->guest_size : 0u);
			}
		}
	}

	// Select a surface parent only when sample ufmt and VkFormat families match
	// and the parent extent equals this mip. Copying float lighting into RGBA8
	// reinterprets bits as cyan/hot garbage; copying a larger parent without a
	// crop view leaves horizontal bands on world tiles.
	const auto surface_parent_ok = [fmt](VulkanImage* img, uint32_t need_w, uint32_t need_h) -> bool
	{
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
	const auto skip_surface_copy = [&]()
	{
		(void)ctx;
		(void)vk_obj;
		(void)vk_layout;
	};

	if (objects.Size() == 1 && objects.At(0).type == GpuMemoryObjectType::StorageTexture && scenario == GpuMemoryScenario::Common)
	{
		auto*      src_obj           = static_cast<StorageTextureVulkanImage*>(objects.At(0).obj);
		const auto src_guest_extent  = (src_obj != nullptr ? src_obj->GetGuestExtent() : VkExtent2D {});
		uint32_t   block_copy_width  = 0;
		uint32_t   block_copy_height = 0;
		const bool block_copy = levels == 1u && src_obj != nullptr &&
		                        Gen5BlockCompressedStorageCopyExtent(static_cast<uint32_t>(fmt), static_cast<uint32_t>(width),
		                                                             static_cast<uint32_t>(height), src_obj->format, src_guest_extent.width,
		                                                             src_guest_extent.height, &block_copy_width, &block_copy_height);
		if (block_copy)
		{
			if (std::getenv("KYTY_DUMP_BLOCK_STORAGE") != nullptr)
			{
				const uint64_t       bytes = static_cast<uint64_t>(block_copy_width) * block_copy_height * 16u;
				std::vector<uint8_t> source(static_cast<size_t>(bytes));
				UtilFillBuffer(ctx, source.data(), bytes, block_copy_width, src_obj, static_cast<uint64_t>(src_obj->layout));
				uint64_t nonzero = 0;
				uint64_t hash    = 1469598103934665603ull;
				for (uint8_t value: source)
				{
					nonzero += (value != 0u ? 1u : 0u);
					hash = (hash ^ value) * 1099511628211ull;
				}
				KYTY_LOG_DEBUG( "KYTY_DUMP_BLOCK_STORAGE extent=%ux%u bytes=%" PRIu64 " nonzero=%" PRIu64 " hash=%016" PRIx64 "\n",
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
			const bool st_ok = src_obj != nullptr && src_guest_extent.width >= width && src_guest_extent.height >= height &&
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
		bool     parents_ok = true;
		uint32_t check_w    = static_cast<uint32_t>(width);
		uint32_t check_h    = static_cast<uint32_t>(height);
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

			// SKIPPED: src_image == nullptr
			if (src_image == nullptr)
			{
				KYTY_LOG_DEBUG("WARNING: skipped check: src_image == nullptr\n");
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

struct TextureImageViewConfiguration
{
	VkComponentMapping components {};
	uint32_t           base_level        = 0;
	uint32_t           base_array        = 0;
	uint32_t           depth             = 1;
	bool               three_dimensional = false;
	bool               arrayed_2d        = false;
};

static TextureVulkanImage* create_texture_image(GraphicContext* ctx, const uint64_t* params, VulkanMemory* mem,
                                                TextureImageViewConfiguration* view_config)
{
	EXIT_IF(ctx == nullptr || params == nullptr || mem == nullptr || view_config == nullptr);

	const auto fmt           = static_cast<uint16_t>((params[TextureObject::PARAM_FORMAT] >> 16u) & 0xffffu);
	const auto dfmt          = static_cast<uint8_t>((params[TextureObject::PARAM_FORMAT] >> 8u) & 0xffu);
	const auto nfmt          = static_cast<uint8_t>(params[TextureObject::PARAM_FORMAT] & 0xffu);
	const auto width         = static_cast<uint32_t>(params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u);
	const auto height        = static_cast<uint32_t>(params[TextureObject::PARAM_WIDTH_HEIGHT]);
	const auto levels        = static_cast<uint32_t>(params[TextureObject::PARAM_LEVELS]);
	const auto resource_info = params[TextureObject::PARAM_RESOURCE_INFO];
	const auto resource_type = TextureObject::GetResourceType(resource_info);
	const auto force_degamma = params[TextureObject::PARAM_FORCE_DEGAMMA] != 0;

	view_config->base_level        = static_cast<uint32_t>(params[TextureObject::PARAM_LEVELS] >> 32u);
	view_config->depth             = TextureObject::GetResourceDepth(resource_info);
	view_config->base_array        = TextureObject::GetResourceBaseArray(resource_info);
	view_config->three_dimensional = resource_type == 10u;
	view_config->arrayed_2d        = resource_type == 13u;

	EXIT_NOT_IMPLEMENTED(resource_type != 8u && resource_type != 9u && !view_config->arrayed_2d && !view_config->three_dimensional);
	EXIT_NOT_IMPLEMENTED(width == 0 || height == 0 || levels == 0);
	EXIT_NOT_IMPLEMENTED((view_config->three_dimensional || view_config->arrayed_2d) && view_config->depth == 0u);
	EXIT_NOT_IMPLEMENTED(view_config->arrayed_2d && view_config->base_array >= view_config->depth);
	EXIT_NOT_IMPLEMENTED(
	    !VulkanDecodeComponentMapping(static_cast<uint32_t>(params[TextureObject::PARAM_SWIZZLE]), &view_config->components));

	const auto pixel_format = VulkanResolveGuestImageFormat(GuestImageUsage::Sampled, dfmt, nfmt, fmt, force_degamma);
	EXIT_NOT_IMPLEMENTED(pixel_format == VK_FORMAT_UNDEFINED);

	VulkanImageDescriptor image_descriptor {};
	image_descriptor.image_type   = view_config->three_dimensional ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	image_descriptor.extent       = {width, height, view_config->three_dimensional ? view_config->depth : 1u};
	image_descriptor.mip_levels   = levels;
	image_descriptor.array_layers = view_config->arrayed_2d ? view_config->depth : 1u;
	image_descriptor.format       = pixel_format;
	image_descriptor.usage        = get_usage();
	auto image_info               = VulkanBuildImageCreateInfo(image_descriptor);

	if (!VulkanImageFormatSupported(ctx, image_info))
	{
		EXIT("texture format is not supported\n");
	}

	auto* vk_obj = new TextureVulkanImage;
	vk_obj->SetNativeExtent(width, height);
	vk_obj->format = image_info.format;
	vk_obj->image  = nullptr;
	vk_obj->layout = image_info.initialLayout;
	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}
	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImage(ctx, image_info, vk_obj, mem));
	return vk_obj;
}

static void create_texture_image_views(GraphicContext* ctx, TextureVulkanImage* vk_obj, const TextureImageViewConfiguration& config)
{
	VulkanImageViewDescriptor descriptor {};
	descriptor.image          = vk_obj->image;
	descriptor.view_type      = config.three_dimensional ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	descriptor.format         = vk_obj->format;
	descriptor.components     = config.components;
	descriptor.base_mip_level = config.base_level;
	descriptor.level_count    = VK_REMAINING_MIP_LEVELS;

	const int view_index = config.three_dimensional ? VulkanImage::VIEW_3D : VulkanImage::VIEW_DEFAULT;
	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImageView(ctx->device, descriptor, &vk_obj->image_view[view_index]));
	if (!config.three_dimensional)
	{
		descriptor.view_type        = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		descriptor.base_array_layer = config.arrayed_2d ? config.base_array : 0u;
		descriptor.layer_count      = config.arrayed_2d ? config.depth - config.base_array : 1u;
		EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImageView(ctx->device, descriptor, &vk_obj->image_view[VulkanImage::VIEW_ARRAY]));
	}
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("TextureObject::Create");
	EXIT_IF(size == nullptr || vaddr == nullptr);

	TextureImageViewConfiguration view_config {};
	auto*                         vk_obj = create_texture_image(ctx, params, mem, &view_config);
	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);
	create_texture_image_views(ctx, vk_obj, view_config);
	return vk_obj;
}

static void* create2_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, GpuMemoryScenario scenario,
                          const Vector<GpuMemoryObject>& objects, VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("TextureObject::CreateFromObjects");
	EXIT_IF(objects.IsEmpty());

	TextureImageViewConfiguration view_config {};
	auto*                         vk_obj = create_texture_image(ctx, params, mem, &view_config);
	update2_func(ctx, buffer, params, vk_obj, scenario, objects);
	create_texture_image_views(ctx, vk_obj, view_config);
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
	        params[PARAM_SKIP_GUEST_UPLOAD] == other[PARAM_SKIP_GUEST_UPLOAD] && params[PARAM_RESOURCE_INFO] == other[PARAM_RESOURCE_INFO]);
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
