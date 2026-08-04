#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/VulkanVertexInputLayout.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Profiler.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstdarg>
#include <set>
#include <string>
#include <vector>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// color helpers, RT dump, g_render_ctx / g_command_pool

bool RenderColorSlotConfigured(const RenderColorInfo& color, uint32_t slot)
{
	return slot < color.targets_num && color.attachment[slot].type != RenderColorType::NoColorOutput;
}

bool RenderColorSlotActive(const RenderColorInfo& color, uint32_t slot)
{
	return RenderColorSlotConfigured(color, slot) && color.attachment[slot].vulkan_buffer != nullptr;
}

bool RenderColorHasActiveTarget(const RenderColorInfo& color)
{
	for (uint32_t slot = 0; slot < color.targets_num; slot++)
	{
		if (RenderColorSlotConfigured(color, slot))
		{
			return true;
		}
	}
	return false;
}

const RenderColorAttachmentInfo* RenderColorFirstConfiguredAttachment(const RenderColorInfo& color)
{
	for (uint32_t slot = 0; slot < color.targets_num; slot++)
	{
		if (RenderColorSlotConfigured(color, slot))
		{
			return &color.attachment[slot];
		}
	}
	return nullptr;
}

VulkanImage* RenderColorFirstActiveImage(const RenderColorInfo& color)
{
	for (uint32_t slot = 0; slot < color.targets_num; slot++)
	{
		if (RenderColorSlotActive(color, slot))
		{
			return color.attachment[slot].vulkan_buffer;
		}
	}
	return nullptr;
}

VkSampleCountFlagBits decode_guest_sample_count(uint32_t encoded)
{
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	EXIT_NOT_IMPLEMENTED(encoded > 6u || !VulkanDecodeLog2SampleCount(static_cast<uint8_t>(encoded), &samples));
	return samples;
}

VkSampleCountFlagBits resolve_render_attachment_sample_count(const RenderColorInfo& color, const RenderDepthInfo& depth)
{
	const bool with_color = RenderColorHasActiveTarget(color);
	const bool with_depth = depth.format != VK_FORMAT_UNDEFINED;
	EXIT_NOT_IMPLEMENTED(!with_color && !with_depth);

	const auto* first_color = RenderColorFirstConfiguredAttachment(color);
	const auto  samples     = with_color ? first_color->samples : depth.samples;
	for (uint32_t slot = 0; slot < color.targets_num; slot++)
	{
		if (RenderColorSlotConfigured(color, slot))
		{
			EXIT_NOT_IMPLEMENTED(color.attachment[slot].samples != samples);
		}
	}
	if (with_color && with_depth && samples != depth.samples)
	{
		KYTY_LOG_DEBUG(
		             "KYTY_ATTACHMENT_SAMPLE_MISMATCH color_addr=0x%012" PRIx64 " color=%ux%u format=%u samples=%u "
		             "depth_addr=0x%012" PRIx64 " depth=%ux%u format=%u samples=%u depth_test=%u depth_write=%u\n",
		             first_color->base_addr, first_color->width, first_color->height,
		             static_cast<uint32_t>(first_color->render_texture_format), static_cast<uint32_t>(samples), depth.depth_buffer_vaddr,
		             depth.width, depth.height, static_cast<uint32_t>(depth.format), static_cast<uint32_t>(depth.samples),
		             depth.depth_test_enable ? 1u : 0u, depth.depth_write_enable ? 1u : 0u);
		EXIT_NOT_IMPLEMENTED(samples != depth.samples);
	}
	return samples;
}

VkExtent2D IntersectFramebufferAttachmentExtent(VkExtent2D current, const VulkanImage* attachment)
{
	EXIT_IF(attachment == nullptr);
	EXIT_NOT_IMPLEMENTED(attachment->extent.width == 0 || attachment->extent.height == 0);
	return {std::min(current.width, attachment->extent.width), std::min(current.height, attachment->extent.height)};
}

// Drop a depth/stencil surface that cannot form a legal framebuffer with the
// active color targets (guest depth size 0 → 1x1 while color is full-screen).
// Clears format so CreatePipeline does not enable depth test without a DS attachment.
void SanitizeRenderDepthAgainstColor(RenderColorInfo* color, RenderDepthInfo* depth)
{
	if (color == nullptr || depth == nullptr || depth->format == VK_FORMAT_UNDEFINED || depth->vulkan_buffer == nullptr)
	{
		return;
	}
	if (!RenderColorHasActiveTarget(*color))
	{
		return;
	}
	auto* color_img = RenderColorFirstActiveImage(*color);
	if (color_img == nullptr)
	{
		return;
	}
	const auto dw = depth->vulkan_buffer->extent.width;
	const auto dh = depth->vulkan_buffer->extent.height;
	const auto cw = color_img->extent.width;
	const auto ch = color_img->extent.height;
	if ((dw < cw || dh < ch) && (dw <= 1u || dh <= 1u))
	{
		KYTY_LOG_DEBUG(
		             "KYTY_GRAPHICS: sanitizing undersized depth %ux%u against color %ux%u (disabling depth attachment)\n", dw, dh, cw,
		             ch);
		depth->format             = VK_FORMAT_UNDEFINED;
		depth->vulkan_buffer      = nullptr;
		depth->depth_test_enable  = false;
		depth->depth_write_enable = false;
		depth->depth_clear_enable = false;
		depth->stencil_clear_enable = false;
		depth->stencil_test_enable  = false;
	}
}

// Latest 1280x720 color targets (for KYTY_DUMP_RT paired with VideoOut frame dumps).
VulkanImage*       g_dump_rt_images[k_dump_rt_slots] {};
uint32_t           g_dump_rt_count                = 0;
VulkanImage*       g_dump_bc3_image               = nullptr;
VulkanImage*       g_dump_bc3_compute_source      = nullptr;
VulkanImage*       g_dump_bc3_compute_destination = nullptr;

static void RememberDumpRt(VulkanImage* img)
{
	if (img == nullptr)
	{
		return;
	}
	for (uint32_t i = 0; i < g_dump_rt_count; i++)
	{
		if (g_dump_rt_images[i] == img)
		{
			return;
		}
	}
	if (g_dump_rt_count < k_dump_rt_slots)
	{
		g_dump_rt_images[g_dump_rt_count++] = img;
	} else
	{
		// Ring: keep most recent.
		for (uint32_t i = 1; i < k_dump_rt_slots; i++)
		{
			g_dump_rt_images[i - 1] = g_dump_rt_images[i];
		}
		g_dump_rt_images[k_dump_rt_slots - 1] = img;
	}
}

void GraphicsDumpRememberedRts(GraphicContext* ctx, const char* prefix)
{
	if (ctx == nullptr || prefix == nullptr)
	{
		return;
	}
	for (uint32_t i = 0; i < g_dump_rt_count; i++)
	{
		char tag[32];
		std::snprintf(tag, sizeof(tag), "rt%u", i);
		UtilDumpVulkanImageRgba8Png(ctx, g_dump_rt_images[i], prefix, tag);
	}
	if (g_dump_bc3_image != nullptr && g_dump_bc3_image->format == VK_FORMAT_BC3_UNORM_BLOCK)
	{
		const uint32_t       width  = g_dump_bc3_image->extent.width;
		const uint32_t       height = g_dump_bc3_image->extent.height;
		const uint64_t       bytes  = static_cast<uint64_t>((width + 3u) / 4u) * ((height + 3u) / 4u) * 16u;
		std::vector<uint8_t> data(static_cast<size_t>(bytes));
		UtilFillBuffer(ctx, data.data(), bytes, width, g_dump_bc3_image, static_cast<uint64_t>(g_dump_bc3_image->layout));
		char path[192];
		std::snprintf(path, sizeof(path), "%s-bc3-%ux%u.dds", prefix, width, height);
		if (FILE* file = std::fopen(path, "wb"); file != nullptr)
		{
			uint32_t dds[31] {};
			dds[0]  = 124u;
			dds[1]  = 0x00081007u;
			dds[2]  = height;
			dds[3]  = width;
			dds[4]  = static_cast<uint32_t>(bytes);
			dds[6]  = 1u;
			dds[18] = 32u;
			dds[19] = 4u;
			dds[20] = 0x35545844u;
			dds[26] = 0x1000u;
			std::fwrite("DDS ", 1, 4, file);
			std::fwrite(dds, sizeof(dds), 1, file);
			std::fwrite(data.data(), 1, data.size(), file);
			std::fclose(file);
		}
	}
	const auto dump_uint4_as_bc3 = [ctx, prefix](VulkanImage* image, const char* tag)
	{
		if (image == nullptr || image->format != VK_FORMAT_R32G32B32A32_UINT)
		{
			return;
		}
		const uint32_t       block_width  = image->extent.width;
		const uint32_t       block_height = image->extent.height;
		const uint32_t       width        = block_width * 4u;
		const uint32_t       height       = block_height * 4u;
		const uint64_t       bytes        = static_cast<uint64_t>(block_width) * block_height * 16u;
		std::vector<uint8_t> data(static_cast<size_t>(bytes));
		UtilFillBuffer(ctx, data.data(), bytes, block_width, image, static_cast<uint64_t>(image->layout));
		char path[192];
		std::snprintf(path, sizeof(path), "%s-%s-%ux%u.dds", prefix, tag, width, height);
		if (FILE* file = std::fopen(path, "wb"); file != nullptr)
		{
			uint32_t dds[31] {};
			dds[0]  = 124u;
			dds[1]  = 0x00081007u;
			dds[2]  = height;
			dds[3]  = width;
			dds[4]  = static_cast<uint32_t>(bytes);
			dds[6]  = 1u;
			dds[18] = 32u;
			dds[19] = 4u;
			dds[20] = 0x35545844u;
			dds[26] = 0x1000u;
			std::fwrite("DDS ", 1, 4, file);
			std::fwrite(dds, sizeof(dds), 1, file);
			std::fwrite(data.data(), 1, data.size(), file);
			std::fclose(file);
		}
	};
	dump_uint4_as_bc3(g_dump_bc3_compute_source, "bc3-compute-source");
	dump_uint4_as_bc3(g_dump_bc3_compute_destination, "bc3-compute-destination");
}

static void AppendFormatted(char* buffer, size_t buffer_size, size_t* length, const char* format, ...)
{
	EXIT_IF(buffer == nullptr || length == nullptr || buffer_size == 0);
	if (*length >= buffer_size - 1)
	{
		buffer[buffer_size - 1] = '\0';
		return;
	}

	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(buffer + *length, buffer_size - *length, format, args);
	va_end(args);
	if (written <= 0)
	{
		return;
	}
	const auto available = buffer_size - *length;
	if (static_cast<size_t>(written) >= available)
	{
		*length = buffer_size - 1;
		buffer[*length] = '\0';
		return;
	}
	*length += static_cast<size_t>(written);
}

void FormatTextureList(const ShaderTextureResources& textures, char* buffer, size_t buffer_size)
{
	EXIT_IF(buffer == nullptr || buffer_size == 0);
	buffer[0]      = '\0';
	size_t tex_len = 0;
	for (int ti = 0; ti < textures.textures_num; ti++)
	{
		const auto&    r  = textures.desc[ti].texture;
		const uint32_t tw = static_cast<uint32_t>(r.Width5()) + 1u;
		const uint32_t th = static_cast<uint32_t>(r.Height5()) + 1u;
		const uint32_t tf = r.Format();
		const uint32_t tt = r.TileMode();
		AppendFormatted(buffer, buffer_size, &tex_len,
		                "%s0x%" PRIx64 ":%ux%u:fmt%u:tile%u:type%u:depth%u:base%u", (tex_len ? "," : ""),
		                Config::IsNextGen() ? r.Base40() : r.Base38(), tw, th, tf, tt, static_cast<uint32_t>(r.Type()),
		                static_cast<uint32_t>(r.Depth()) + 1u, static_cast<uint32_t>(r.BaseArray5()));
		if (tex_len + 8 >= buffer_size)
		{
			break;
		}
	}
}

// KYTY_DUMP_DRAW_FRAME=min[-max] restricts draw dumps to a presented-frame
// window. Without it the bounded dump budget is spent on the first boot draws
// and never reaches a later phase of the title.
bool DumpDrawFrameSelected()
{
	static const char* spec = std::getenv("KYTY_DUMP_DRAW_FRAME");
	if (spec == nullptr || spec[0] == '\0')
	{
		return true;
	}
	static int  first  = 0;
	static int  last   = 0;
	static bool parsed = false;
	if (!parsed)
	{
		parsed = true;
		if (std::sscanf(spec, "%d-%d", &first, &last) != 2)
		{
			last = std::numeric_limits<int>::max();
		}
	}
	const int frame = WindowGetPresentedFrameNum();
	return frame >= first && frame <= last;
}

// Opt-in: KYTY_DUMP_DRAW=1 logs unique draws into 1280x720 color targets that
// sample a 980x347 texture. Captures VS fmt/stride, prim, viewport.
void MaybeDumpUiDraw(const RenderColorInfo& color, const ShaderVertexInputInfo& vs_input, const ShaderPixelInputInfo& ps_input,
                            const HW::Context& hw, const HW::UserConfig& ucfg, uint32_t index_count, uint32_t index_type_and_size,
                            bool indexed, uint32_t flags)
{
	static const char* enabled = std::getenv("KYTY_DUMP_DRAW");
	if (enabled == nullptr || enabled[0] == '\0' || !DumpDrawFrameSelected())
	{
		return;
	}
	const bool dump_all = std::strcmp(enabled, "all") == 0;
	bool       rt720    = false;
	uint32_t   rt_w     = 0;
	uint32_t   rt_h     = 0;
	for (uint32_t slot = 0; slot < color.targets_num; slot++)
	{
		VulkanImage* img = color.attachment[slot].vulkan_buffer;
		if (img != nullptr && (dump_all || (img->extent.width == 1280u && img->extent.height == 720u)))
		{
			rt720 = true;
			rt_w  = img->extent.width;
			rt_h  = img->extent.height;
			break;
		}
	}
	if (!rt720)
	{
		return;
	}

	bool        logo = false;
	char        tex_buf[256] {};
	const auto& textures = ps_input.bind.textures2D;
	FormatTextureList(textures, tex_buf, sizeof(tex_buf));
	for (int ti = 0; ti < textures.textures_num; ti++)
	{
		const auto&    r  = textures.desc[ti].texture;
		const uint32_t tw = static_cast<uint32_t>(r.Width5()) + 1u;
		const uint32_t th = static_cast<uint32_t>(r.Height5()) + 1u;
		if (tw == 980u && th == 347u)
		{
			logo = true;
		}
	}
	// Also log non-logo 720p draws once (wipe/bg); logo draws always preferred.
	if (!logo && textures.textures_num == 0)
	{
		// Still interesting for wipe geometry — allow a few.
	}

	char   vs_buf[384] {};
	size_t vs_len = 0;
	for (int bi = 0; bi < vs_input.buffers_num; bi++)
	{
		const auto& b = vs_input.buffers[bi];
		vs_len += static_cast<size_t>(std::snprintf(vs_buf + vs_len, sizeof(vs_buf) - vs_len, "%sstride=%u:recs=%u:attrs=%d",
		                                            (vs_len ? "|" : ""), b.stride, b.num_records, b.attr_num));
		for (int ai = 0; ai < b.attr_num && ai < 8; ai++)
		{
			const int     idx = b.attr_indices[ai];
			const uint8_t fmt = vs_input.resources[idx].Format();
			vs_len +=
			    static_cast<size_t>(std::snprintf(vs_buf + vs_len, sizeof(vs_buf) - vs_len, ":a%d@%u:fmt%u", idx, b.attr_offsets[ai], fmt));
		}
		if (vs_len + 16 >= sizeof(vs_buf))
		{
			break;
		}
	}

	const auto& vp            = hw.GetScreenViewport().viewports[0];
	const auto  xy            = State::ResolveViewportXy(vp.xscale, vp.xoffset, vp.yscale, vp.yoffset);
	const auto  sc            = State::ResolveScissor(hw.GetScreenViewport(), hw.GetScanModeControl(), 0);
	const auto& blend         = hw.GetBlendControl(0);
	const auto& mode          = hw.GetModeControl();
	const auto& depth_control = hw.GetDepthControl();

	char   ps_input_buf[256] {};
	size_t ps_input_len = 0;
	for (uint32_t i = 0; i < ps_input.input_num && ps_input_len + 16u < sizeof(ps_input_buf); ++i)
	{
		ps_input_len += static_cast<size_t>(std::snprintf(ps_input_buf + ps_input_len, sizeof(ps_input_buf) - ps_input_len, "%s%08x",
		                                                  (ps_input_len == 0 ? "" : ","), ps_input.interpolator_settings[i]));
	}

	char line[1024];
	std::snprintf(line, sizeof(line),
	              "rt=0x%012" PRIx64 ":%ux%u logo=%d prim=%u idx=%u itype=%u indexed=%d flags=0x%x vs_bufs=%d [%s] tex=[%s] "
	              "ps_inputs=%u:[%s] vp=%.1f,%.1f,%.1fx%.1f sc=%d,%d-%d,%d clear=%d:%08x:%08x "
	              "mask=%08x cull=%d:%d:%d depth=%d:%d:%u blend=%d:%u:%u:%u:%u:%u:%u\n",
	              static_cast<uint64_t>(color.attachment[0].base_addr), rt_w, rt_h, logo ? 1 : 0, ucfg.GetPrimType(), index_count, index_type_and_size,
	              indexed ? 1 : 0, flags, vs_input.buffers_num, vs_buf, tex_buf, ps_input.input_num, ps_input_buf, xy.x, xy.y, xy.width,
	              xy.height, sc.left, sc.top, sc.right, sc.bottom, color.attachment[0].cmask_fast_clear_enable ? 1 : 0,
	              color.attachment[0].clear_word0, color.attachment[0].clear_word1, hw.GetRenderTargetMask(), mode.cull_front ? 1 : 0,
	              mode.cull_back ? 1 : 0, mode.face ? 1 : 0,
	              depth_control.z_enable ? 1 : 0, depth_control.z_write_enable ? 1 : 0, depth_control.zfunc, blend.enable ? 1 : 0,
	              blend.color_srcblend, blend.color_comb_fcn, blend.color_destblend, blend.alpha_srcblend, blend.alpha_comb_fcn,
	              blend.alpha_destblend);

	static std::set<std::string> seen;
	static uint32_t              non_logo_left = 8;
	if (!logo && !dump_all)
	{
		if (non_logo_left == 0)
		{
			return;
		}
		--non_logo_left;
	}
	if (!seen.insert(line).second)
	{
		return;
	}

	KYTY_LOG_DEBUG( "KYTY_DUMP_DRAW %s", line);
	const auto& ps_buffers = ps_input.bind.storage_buffers;
	for (int bi = 0; bi < ps_buffers.buffers_num; ++bi)
	{
		const auto&    resource = ps_buffers.buffers[bi];
		const uint64_t address  = Config::IsNextGen() ? resource.Base48() : resource.Base44();
		const uint64_t size     = ShaderBufferByteSize(resource.Stride(), resource.NumRecords());
		const uint64_t readable = address != 0 ? GpuMemoryGetAllocatedRangePrefix(address, std::min<uint64_t>(size, 56u * sizeof(uint32_t))) : 0;
		const auto*    words    = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(address));
		KYTY_LOG_DEBUG( "KYTY_DUMP_DRAW_PS_BUFFER slot=%d reg=%d usage=%u addr=0x%012" PRIx64
		                     " stride=%u records=%u bytes=%" PRIu64 " readable=%" PRIu64,
		             ps_buffers.slots[bi], ps_buffers.start_register[bi], static_cast<unsigned>(ps_buffers.usages[bi]), address,
		             resource.Stride(), resource.NumRecords(), size, readable);
		if (readable >= 4u * sizeof(uint32_t))
		{
			KYTY_LOG_DEBUG( " words=%08x,%08x,%08x,%08x", words[0], words[1], words[2], words[3]);
			if (readable >= 5u * sizeof(uint32_t))
			{
				KYTY_LOG_DEBUG( ",%08x", words[4]);
			}
		}
		if (readable >= 56u * sizeof(uint32_t))
		{
			KYTY_LOG_DEBUG( " words48_55=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x", words[48], words[49], words[50],
			             words[51], words[52], words[53], words[54], words[55]);
		}
		KYTY_LOG_DEBUG( "\n");
	}

	// First vertex records as floats (cheap evidence for stride/fmt / shear).
	if ((logo || dump_all) && vs_input.buffers_num > 0)
	{
		const auto& b = vs_input.buffers[0];
		if (b.addr != 0 && b.stride >= 4 && b.stride <= 256)
		{
			const uint32_t floats_per = b.stride / 4u;
			const uint32_t requested_n = std::min(index_count == 0 ? 6u : index_count, 8u);
			const uint64_t requested_bytes = static_cast<uint64_t>(b.stride) * requested_n;
			const uint64_t readable_bytes = GpuMemoryGetAllocatedRangePrefix(b.addr, requested_bytes);
			const uint32_t vert_n = static_cast<uint32_t>(readable_bytes / b.stride);
			KYTY_LOG_DEBUG( "KYTY_DUMP_DRAW_VERT stride=%u floats=%u verts=%u", b.stride, floats_per, vert_n);
			for (int ai = 0; ai < b.attr_num && ai < 8; ai++)
			{
				const int     idx = b.attr_indices[ai];
				const uint8_t fmt = vs_input.resources[idx].Format();
				KYTY_LOG_DEBUG( " dst%d={reg=%d,n=%d,fmt=%u,off=%u}", ai, vs_input.resources_dst[idx].register_start,
				             vs_input.resources_dst[idx].registers_num, fmt, b.attr_offsets[ai]);
			}
			KYTY_LOG_DEBUG( "\n");
			const auto* base = reinterpret_cast<const float*>(static_cast<uintptr_t>(b.addr));
			for (uint32_t v = 0; v < vert_n; v++)
			{
				const float* f = base + static_cast<size_t>(v) * floats_per;
				KYTY_LOG_DEBUG( "KYTY_DUMP_DRAW_VERT[%u]", v);
				for (uint32_t i = 0; i < floats_per && i < 16u; i++)
				{
					KYTY_LOG_DEBUG( " %g", static_cast<double>(f[i]));
				}
				KYTY_LOG_DEBUG( "\n");
			}
		}
	}
}

// Opt-in: KYTY_DUMP_RT=WxH remembers matching color targets; paired VideoOut
// frame dumps call GraphicsDumpRememberedRts (avoid per-draw readback stalls).
void MaybeDumpColorTargets(GraphicContext* ctx, const RenderColorInfo& color)
{
	static const char* spec = std::getenv("KYTY_DUMP_RT");
	if (spec == nullptr || spec[0] == '\0' || ctx == nullptr)
	{
		return;
	}
	uint32_t want_w = 0;
	uint32_t want_h = 0;
	if (std::sscanf(spec, "%ux%u", &want_w, &want_h) != 2 || want_w == 0 || want_h == 0)
	{
		return;
	}
	for (uint32_t slot = 0; slot < color.targets_num; slot++)
	{
		VulkanImage* img = color.attachment[slot].vulkan_buffer;
		if (img == nullptr || img->extent.width != want_w || img->extent.height != want_h)
		{
			continue;
		}
		RememberDumpRt(img);
	}
}

RenderContext*           g_render_ctx = nullptr;
thread_local CommandPool g_command_pool;

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
