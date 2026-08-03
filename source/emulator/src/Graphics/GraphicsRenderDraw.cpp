#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/DepthStencilCopy.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Gen5TextureMipLayout.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/RenderResolutionPolicy.h"
#include "Emulator/Graphics/RenderResolutionTransform.h"
#include "Emulator/Graphics/SampleLocations.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"
#include "Emulator/Graphics/SpirvBinaryCacheStore.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/VulkanVertexInputLayout.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// DrawIndex, DrawIndexAuto, DispatchDirect, depth-stencil copy

void GraphicsRenderDepthStencilCopy(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg,
                                           HW::Shader* sh_ctx, uint32_t index_count, uint32_t index_type_and_size,
                                           const void* index_addr);
static bool vertex_shader_is_disabled(HW::Shader* sh_ctx)
{
	if (const auto& vs = sh_ctx->GetVs();
	    !vs.vs_embedded && ((vs.vs_regs.data_addr != 0 && ShaderIsDisabled(vs.vs_regs.data_addr)) ||
	                        (vs.vs_regs.data_addr == 0 && vs.gs_regs.data_addr == 0 && vs.es_regs.data_addr != 0 &&
	                         vs.gs_regs.chksum != 0 && ShaderIsDisabled2(vs.es_regs.data_addr, vs.gs_regs.chksum))))
	{
		return true;
	}
	return false;
}

static const char* shader_disable_reason(HW::Shader* sh_ctx)
{
	if (vertex_shader_is_disabled(sh_ctx))
	{
		return "vs";
	}

	if (const auto& ps = sh_ctx->GetPs();
	    !ps.ps_embedded && ((ps.ps_regs.chksum == 0 && ShaderIsDisabled(ps.ps_regs.data_addr)) ||
	                        (ps.ps_regs.chksum != 0 && ShaderIsDisabled2(ps.ps_regs.data_addr, ps.ps_regs.chksum))))
	{
		return "ps";
	}

	return nullptr;
}

static uint32_t ResolveStorageSeedSkipMask(const ShaderComputeInputInfo& input_info, uint32_t groups_x, uint32_t groups_y,
                                           uint32_t groups_z)
{
	if (!Config::IsNextGen() || input_info.storage_image_write_only_mask == 0u)
	{
		return 0u;
	}

	const uint64_t global_x = static_cast<uint64_t>(groups_x) * input_info.threads_num[0];
	const uint64_t global_y = static_cast<uint64_t>(groups_y) * input_info.threads_num[1];
	const uint64_t global_z = static_cast<uint64_t>(groups_z) * input_info.threads_num[2];
	uint32_t       result   = 0u;
	for (int i = 0; i < input_info.bind.textures2D.textures_num; ++i)
	{
		const auto& descriptor = input_info.bind.textures2D.desc[i];
		if ((input_info.storage_image_write_only_mask & (1u << static_cast<uint32_t>(i))) == 0u ||
		    !descriptor.textures2d_without_sampler)
		{
			continue;
		}

		const auto shape = ShaderGen5SampledTextureShapeForType(descriptor.texture.Type());
		const uint64_t width  = static_cast<uint64_t>(descriptor.texture.Width5()) + 1u;
		const uint64_t height = static_cast<uint64_t>(descriptor.texture.Height5()) + 1u;
		const uint64_t depth  = shape == ShaderGen5SampledTextureShape::TwoDimensional ? 1u :
		                       static_cast<uint64_t>(descriptor.texture.Depth()) + 1u;
		if (global_x >= width && global_y >= height && global_z >= depth)
		{
			result |= 1u << static_cast<uint32_t>(i);
		}
	}
	if (result != 0u)
	{
		static const bool dump = std::getenv("KYTY_DUMP_STORAGE_SEED_SKIP") != nullptr;
		static std::atomic_uint32_t logged {0};
		if (dump && logged.fetch_add(1, std::memory_order_relaxed) < 32u)
		{
			std::fprintf(stderr, "KYTY_STORAGE_SEED_SKIP mask=0x%08" PRIx32 " global=%" PRIu64 "x%" PRIu64 "x%" PRIu64 "\n",
			             result, global_x, global_y, global_z);
		}
	}
	return result;
}

static void MaybeDumpAutoDrawSkip(const char* reason, uint32_t index_count, uint64_t draw_modifier)
{
	if (std::getenv("KYTY_DUMP_DRAW") == nullptr || !DumpDrawFrameSelected())
	{
		return;
	}
	static uint32_t logs = 0;
	if (logs >= 32u)
	{
		return;
	}
	++logs;
	std::fprintf(stderr, "KYTY_DUMP_DRAW_SKIP_AUTO reason=%s count=%u modifier=0x%016" PRIx64 "\n", reason, index_count,
	             draw_modifier);
}

static void MaybeDumpIndexDrawSkip(const char* reason, uint32_t index_count, uint64_t draw_modifier, uint32_t type)
{
	if (std::getenv("KYTY_DUMP_DRAW") == nullptr || !DumpDrawFrameSelected())
	{
		return;
	}
	static uint32_t logs = 0;
	if (logs >= 32u)
	{
		return;
	}
	++logs;
	std::fprintf(stderr, "KYTY_DUMP_DRAW_SKIP_INDEX reason=%s count=%u modifier=0x%016" PRIx64 " type=%u\n", reason, index_count,
	             draw_modifier, type);
}

static bool PixelShaderSnapshotSelected(uint64_t shader_id)
{
	static const char* selector = std::getenv("KYTY_DUMP_PS_ID");
	static uint64_t    selected = 0;
	static bool        parsed   = false;
	if (!parsed)
	{
		parsed = true;
		if (selector != nullptr && selector[0] != '\0')
		{
			char* end = nullptr;
			selected  = std::strtoull(selector, &end, 16);
			if (end == selector || *end != '\0')
			{
				selected = 0;
			}
		}
	}
	return selected != 0 && shader_id == selected && DumpDrawFrameSelected();
}

static uint64_t PixelShaderSnapshotHash(const ShaderPixelInputInfo& input)
{
	uint64_t hash = 1469598103934665603ull;
	const auto append = [&hash](const void* data, size_t size) {
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
	};

	const auto& buffers = input.bind.storage_buffers;
	append(&buffers.buffers_num, sizeof(buffers.buffers_num));
	for (int i = 0; i < buffers.buffers_num; ++i)
	{
		const auto& resource = buffers.buffers[i];
		append(resource.fields, sizeof(resource.fields));
		append(&buffers.slots[i], sizeof(buffers.slots[i]));
		append(&buffers.start_register[i], sizeof(buffers.start_register[i]));
		const uint64_t address   = Config::IsNextGen() ? resource.Base48() : resource.Base44();
		const uint64_t declared  = ShaderBufferByteSize(resource.Stride(), resource.NumRecords());
		const uint64_t requested = std::min<uint64_t>(declared, 64u);
		const uint64_t readable  = address != 0 && requested != 0 ? GpuMemoryGetAllocatedRangePrefix(address, requested) : 0;
		if (readable != 0)
		{
			append(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), static_cast<size_t>(readable));
		}
	}

	const auto& textures = input.bind.textures2D;
	append(&textures.textures_num, sizeof(textures.textures_num));
	for (int i = 0; i < textures.textures_num; ++i)
	{
		append(textures.desc[i].texture.fields, sizeof(textures.desc[i].texture.fields));
	}
	const auto& samplers = input.bind.samplers;
	append(&samplers.samplers_num, sizeof(samplers.samplers_num));
	for (int i = 0; i < samplers.samplers_num; ++i)
	{
		append(samplers.samplers[i].fields, sizeof(samplers.samplers[i].fields));
	}
	return hash;
}

static void MaybeDumpPixelShaderTextureBytes(uint64_t shader_id, int index, const ShaderTextureResource& resource)
{
	if (std::getenv("KYTY_DUMP_PS_BYTES") == nullptr || !Config::IsNextGen() || resource.TileMode() != 5u)
	{
		return;
	}

	const uint32_t width  = static_cast<uint32_t>(resource.Width5()) + 1u;
	const uint32_t height = static_cast<uint32_t>(resource.Height5()) + 1u;
	const uint32_t levels = static_cast<uint32_t>(resource.MaxMip()) + 1u;
	Gen5TextureMipLayout layout {};
	if (!Gen5GetStandard4KBTextureMipLayout(resource.Format(), width, height, width, levels, &layout))
	{
		return;
	}
	const uint32_t base_level = resource.BaseLevel();
	if (base_level >= layout.levels)
	{
		return;
	}
	const auto&    level       = layout.level[base_level];
	const uint64_t address     = resource.Base40();
	const uint64_t source_addr = address + level.tiled_offset;
	const uint64_t readable    = GpuMemoryGetAllocatedRangePrefix(source_addr, level.tiled_size);
	if (readable != level.tiled_size)
	{
		return;
	}

	static std::set<uint64_t> dumped;
	const uint64_t key = shader_id ^ (static_cast<uint64_t>(index) << 56u) ^ source_addr;
	if (dumped.size() >= 16u || !dumped.insert(key).second)
	{
		return;
	}

	char file_path[192];
	std::snprintf(file_path, sizeof(file_path), "/tmp/kyty-ps-bytes-%d-%ux%u-%012" PRIx64 ".bin", index, width, height,
	              source_addr);
	if (FILE* file = std::fopen(file_path, "wb"); file != nullptr)
	{
		const size_t written = std::fwrite(reinterpret_cast<const void*>(static_cast<uintptr_t>(source_addr)), 1,
		                                   static_cast<size_t>(level.tiled_size), file);
		std::fclose(file);
		std::fprintf(stderr,
		             "KYTY_DUMP_PS_BYTES index=%d path=%s allocation=0x%012" PRIx64 " level=%u offset=%u bytes=%u complete=%u\n",
		             index, file_path, address, base_level, level.tiled_offset, level.tiled_size,
		             written == level.tiled_size ? 1u : 0u);
	}
}

static void MaybeDumpPixelShaderSnapshot(const char* path, const HW::Shader& sh, const ShaderPixelInputInfo& input,
	                                      uint32_t count, uint32_t primitive_type)
{
	const auto& ps = sh.GetPs();
	if (!PixelShaderSnapshotSelected(ps.ps_regs.chksum))
	{
		return;
	}

	static std::set<uint64_t> seen;
	static uint32_t           logs  = 0;
	uint32_t                  limit = 128u;
	if (const char* env_limit = std::getenv("KYTY_DUMP_PS_LIMIT"); env_limit != nullptr && env_limit[0] != '\0')
	{
		const auto parsed = std::strtoul(env_limit, nullptr, 10);
		if (parsed > 0u && parsed <= 4096u)
		{
			limit = static_cast<uint32_t>(parsed);
		}
	}
	const uint64_t snapshot_hash = PixelShaderSnapshotHash(input);
	if (logs >= limit || !seen.insert(snapshot_hash).second)
	{
		return;
	}
	++logs;

	std::fprintf(stderr,
	             "KYTY_DUMP_PS_SNAPSHOT path=%s ordinal=%u hash=0x%016" PRIx64 " count=%u prim=%u addr=0x%012" PRIx64
	             " id=0x%016" PRIx64 " buffers=%d textures=%d\n",
	             path, logs, snapshot_hash, count, primitive_type, ps.ps_regs.data_addr, ps.ps_regs.chksum,
	             input.bind.storage_buffers.buffers_num, input.bind.textures2D.textures_num);

	const auto& buffers = input.bind.storage_buffers;
	for (int i = 0; i < buffers.buffers_num; ++i)
	{
		const auto& resource  = buffers.buffers[i];
		const uint64_t address = Config::IsNextGen() ? resource.Base48() : resource.Base44();
		const uint64_t declared = ShaderBufferByteSize(resource.Stride(), resource.NumRecords());
		const uint64_t requested = std::min<uint64_t>(declared, 64u);
		const uint64_t readable = address != 0 && requested != 0 ? GpuMemoryGetAllocatedRangePrefix(address, requested) : 0;
		std::fprintf(stderr,
		             "KYTY_DUMP_PS_BUFFER index=%d slot=%d reg=%d usage=%u access=%u source=%u desc=%08x,%08x,%08x,%08x"
		             " addr=0x%012" PRIx64 " stride=%u records=%u bytes=%" PRIu64 " readable=%" PRIu64 " words=",
		             i, buffers.slots[i], buffers.start_register[i], static_cast<unsigned>(buffers.usages[i]),
		             static_cast<unsigned>(buffers.accesses[i]), static_cast<unsigned>(buffers.sources[i]), resource.fields[0],
		             resource.fields[1], resource.fields[2], resource.fields[3], address, resource.Stride(), resource.NumRecords(),
		             declared, readable);
		const auto* words = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(address));
		for (uint64_t word = 0; word < readable / sizeof(uint32_t); ++word)
		{
			std::fprintf(stderr, "%s%08x", word == 0 ? "" : ",", words[word]);
		}
		std::fprintf(stderr, "\n");
	}

	const auto& textures = input.bind.textures2D;
	for (int i = 0; i < textures.textures_num; ++i)
	{
		const auto& descriptor = textures.desc[i];
		const auto& resource   = descriptor.texture;
		std::fprintf(stderr,
		             "KYTY_DUMP_PS_TEXTURE index=%d slot=%d reg=%d usage=%u desc=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x"
		             " addr=0x%012" PRIx64 " size=%ux%u format=%u type=%u tile=%u\n",
		             i, descriptor.slot, descriptor.start_register, static_cast<unsigned>(descriptor.usage), resource.fields[0],
		             resource.fields[1], resource.fields[2], resource.fields[3], resource.fields[4], resource.fields[5],
		             resource.fields[6], resource.fields[7], resource.Base40(), static_cast<uint32_t>(resource.Width5()) + 1u,
		             static_cast<uint32_t>(resource.Height5()) + 1u, resource.Format(), resource.Type(), resource.TileMode());
		MaybeDumpPixelShaderTextureBytes(ps.ps_regs.chksum, i, resource);
	}

	const auto& samplers = input.bind.samplers;
	for (int i = 0; i < samplers.samplers_num; ++i)
	{
		const auto& resource = samplers.samplers[i];
		std::fprintf(stderr,
		             "KYTY_DUMP_PS_SAMPLER index=%d slot=%d reg=%d desc=%08x,%08x,%08x,%08x force_degamma=%u skip_degamma=%u"
		             " min=%u max=%u lod_bias=%u mag=%u min_filter=%u mip=%u\n",
		             i, samplers.slots[i], samplers.start_register[i], resource.fields[0], resource.fields[1], resource.fields[2],
		             resource.fields[3], resource.ForceDegamma() ? 1u : 0u, resource.SkipDegamma() ? 1u : 0u, resource.MinLod(),
		             resource.MaxLod(), resource.LodBias(), resource.XyMagFilter(), resource.XyMinFilter(), resource.MipFilter());
	}
}

static void MaybeDumpVideoDrawReady(const char* path, const HW::Shader& sh, const ShaderPixelInputInfo& ps_input,
                                    const RenderColorInfo& color, uint32_t count, uint32_t primitive_type)
{
	if (std::getenv("KYTY_DUMP_VIDEO_DRAW") == nullptr || !Config::IsNextGen())
	{
		return;
	}

	const auto& textures = ps_input.bind.textures2D;
	bool       has_luma   = false;
	bool       has_chroma = false;
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& resource = textures.desc[index].texture;
		const uint32_t width  = static_cast<uint32_t>(resource.Width5()) + 1u;
		const uint32_t height = static_cast<uint32_t>(resource.Height5()) + 1u;
		if (resource.TileMode() != 0u)
		{
			continue;
		}
		if (resource.Format() == 1u && width == 1920u && height == 1080u)
		{
			has_luma = true;
		}
		if (resource.Format() == 14u && width == 960u && height == 540u)
		{
			has_chroma = true;
		}
	}
	if (!has_luma || !has_chroma)
	{
		return;
	}

	static std::atomic<uint32_t> logs {0};
	const uint32_t ordinal = logs.fetch_add(1, std::memory_order_relaxed);
	if (ordinal >= 128u)
	{
		return;
	}
	const auto& ps = sh.GetPs();
	const auto& vs = sh.GetVs();
	const auto* target = RenderColorFirstConfiguredAttachment(color);
	std::fprintf(stderr,
	             "KYTY_DUMP_VIDEO_DRAW path=%s ordinal=%u frame=%d count=%u prim=%u target=0x%012" PRIx64
	             " vs_id=0x%016" PRIx64 " ps_id=0x%016" PRIx64 " ps_addr=0x%012" PRIx64 " textures=%d\n",
	             path, ordinal, WindowGetPresentedFrameNum(), count, primitive_type, target != nullptr ? target->base_addr : 0u,
	             vs.gs_regs.chksum, ps.ps_regs.chksum, ps.ps_regs.data_addr, textures.textures_num);
}

static void MaybeDumpIndexDrawReady(const RenderColorInfo& color, const RenderDepthInfo& depth, const HW::Context& hw,
                                    const HW::Shader& sh,
                                    const ShaderVertexInputInfo& vs_input, const ShaderPixelInputInfo& ps_input, uint32_t index_count,
                                    uint32_t index_type_and_size, uint64_t draw_modifier, uint32_t packet_type,
                                    uint32_t primitive_type)
{
	MaybeDumpPixelShaderSnapshot("index", sh, ps_input, index_count, primitive_type);
	MaybeDumpVideoDrawReady("index", sh, ps_input, color, index_count, primitive_type);
	if (std::getenv("KYTY_DUMP_DRAW2") != nullptr)
	{
		static uint32_t draw2_logs = 0;
		if (draw2_logs < 5000)
		{
			draw2_logs++;
			if (FILE* f = fopen("/tmp/kyty_draws2.log", "a"))
			{
				const auto& vp = hw.GetScreenViewport().viewports[0];
				const auto  sc = State::ResolveScissor(hw.GetScreenViewport(), hw.GetScanModeControl(), 0);
				fprintf(f,
				        "DRAW2: cb=0x%012llx db=0x%012llx idx=%u ps=%08x vs=%08x vp=%.1f,%.1f,%.1fx%.1f "
				        "sc=%d,%d-%d,%d targets=%u\n",
				        (unsigned long long)color.attachment[0].base_addr, (unsigned long long)depth.depth_buffer_vaddr,
				        index_count, (unsigned)(sh.GetPs().ps_regs.chksum & 0xffffffffu),
				        (unsigned)(sh.GetVs().gs_regs.chksum & 0xffffffffu), vp.xscale, vp.xoffset, vp.yscale, vp.yoffset,
				        sc.left, sc.top, sc.right, sc.bottom, color.targets_num);
				fclose(f);
			}
		}
	}
	if (std::getenv("KYTY_DUMP_DRAW") == nullptr || !DumpDrawFrameSelected())
	{
		return;
	}
	static uint32_t logs = 0;
	uint32_t limit = 48u;
	if (const char* env_limit = std::getenv("KYTY_DUMP_DRAW_LIMIT"); env_limit != nullptr && env_limit[0] != '\0')
	{
		const auto parsed = std::strtoul(env_limit, nullptr, 10);
		if (parsed > 0u && parsed <= 100000u)
		{
			limit = static_cast<uint32_t>(parsed);
		}
	}
	if (logs >= limit)
	{
		return;
	}
	++logs;

	uint32_t active_slots = 0;
	uint64_t first_addr   = 0;
	uint32_t first_width  = 0;
	uint32_t first_height = 0;
	uint32_t first_format = 0;
	uint32_t first_samples = 0;
	for (uint32_t slot = 0; slot < color.targets_num; ++slot)
	{
		const auto& attachment = color.attachment[slot];
		if (attachment.vulkan_buffer == nullptr)
		{
			continue;
		}
		active_slots |= 1u << slot;
		if (first_addr == 0)
		{
			first_addr    = attachment.base_addr;
			first_width   = attachment.vulkan_buffer->extent.width;
			first_height  = attachment.vulkan_buffer->extent.height;
			first_format  = static_cast<uint32_t>(attachment.vulkan_buffer->format);
			first_samples = static_cast<uint32_t>(attachment.vulkan_buffer->samples);
		}
	}

	const auto& vp = hw.GetScreenViewport().viewports[0];
	const auto  xy = State::ResolveViewportXy(vp.xscale, vp.xoffset, vp.yscale, vp.yoffset);
	const auto  sc = State::ResolveScissor(hw.GetScreenViewport(), hw.GetScanModeControl(), 0);
	const auto& rt0 = hw.GetRenderTarget(0).info;
	const auto& cc  = hw.GetColorControl();
	const auto& bc  = hw.GetBlendControl(0);
	const auto& vs  = sh.GetVs();
	const auto& ps  = sh.GetPs();
	char        tex_buf[512] {};
	FormatTextureList(ps_input.bind.textures2D, tex_buf, sizeof(tex_buf));
	std::fprintf(stderr,
	             "KYTY_DUMP_DRAW_READY_INDEX count=%u modifier=0x%016" PRIx64 " packet_type=%u prim=%u index_type=%u targets=%u active=0x%02" PRIx32
	             " rt=0x%012" PRIx64 ":%ux%u:s%u:f%u depth=%u:%ux%u target_mask=0x%08" PRIx32
	             " shader_mask=0x%08" PRIx32 " color_mode=%u rop3=0x%02x blend=%u:%u:%u:%u:%u:%u:%u"
	             " vs_addr=0x%012" PRIx64 " vs_id=0x%016" PRIx64 " ps_addr=0x%012" PRIx64 " ps_id=0x%016" PRIx64
	             " cbfmt=0x%x cbtype=0x%x cborder=0x%x vs_bufs=%d ps_tex=%d tex=[%s] ps_buf=%d"
	             " vp=%.1f,%.1f,%.1fx%.1f sc=%d,%d-%d,%d\n",
	             index_count, draw_modifier, packet_type, primitive_type, index_type_and_size, color.targets_num, active_slots, first_addr, first_width,
	             first_height, first_samples, first_format, static_cast<uint32_t>(depth.format), depth.width, depth.height,
	             hw.GetRenderTargetMask(), hw.GetShaderRegisters().m_cbShaderMask, cc.mode, cc.op, bc.enable ? 1u : 0u, bc.color_srcblend, bc.color_comb_fcn,
	             bc.color_destblend, bc.alpha_srcblend, bc.alpha_comb_fcn, bc.alpha_destblend, vs.vs_regs.data_addr, vs.gs_regs.chksum,
	             ps.ps_regs.data_addr, ps.ps_regs.chksum, rt0.format, rt0.channel_type, rt0.channel_order,
	             vs_input.buffers_num, ps_input.bind.textures2D.textures_num, tex_buf,
	             ps_input.bind.storage_buffers.buffers_num, xy.x, xy.y, xy.width, xy.height, sc.left, sc.top, sc.right, sc.bottom);
}

static void MaybeDumpAutoDrawReady(const RenderColorInfo& color, const RenderDepthInfo& depth, const HW::Context& hw,
                                   const HW::Shader& sh, const ShaderVertexInputInfo& vs_input,
                                   const ShaderPixelInputInfo& ps_input, uint32_t index_count, uint32_t primitive_type)
{
	MaybeDumpPixelShaderSnapshot("auto", sh, ps_input, index_count, primitive_type);
	MaybeDumpVideoDrawReady("auto", sh, ps_input, color, index_count, primitive_type);
	if (std::getenv("KYTY_DUMP_DRAW") == nullptr || !DumpDrawFrameSelected())
	{
		return;
	}
	static uint32_t logs = 0;
	if (logs >= 64u)
	{
		return;
	}
	++logs;

	auto* color_image = RenderColorFirstActiveImage(color);
	const auto& vs    = sh.GetVs();
	const auto& ps    = sh.GetPs();
	std::fprintf(stderr,
	             "KYTY_DUMP_DRAW_READY_AUTO count=%u prim=%u rt=0x%012" PRIx64 ":%ux%u:f%u depth=%u:%ux%u "
	             "stages=0x%08" PRIx32 " es=0x%012" PRIx64 " vs_id=0x%016" PRIx64 " ps=0x%012" PRIx64
	             " ps_id=0x%016" PRIx64 " vs_bufs=%d ps_tex=%d ps_buf=%d\n",
	             index_count, primitive_type, color_image != nullptr ? color.attachment[0].base_addr : 0u,
	             color_image != nullptr ? color_image->extent.width : 0u, color_image != nullptr ? color_image->extent.height : 0u,
	             color_image != nullptr ? static_cast<uint32_t>(color_image->format) : 0u, static_cast<uint32_t>(depth.format),
	             depth.vulkan_buffer != nullptr ? depth.vulkan_buffer->extent.width : 0u,
	             depth.vulkan_buffer != nullptr ? depth.vulkan_buffer->extent.height : 0u, hw.GetShaderStages(), vs.es_regs.data_addr,
	             vs.gs_regs.chksum, ps.ps_regs.data_addr, ps.ps_regs.chksum, vs_input.buffers_num,
	             ps_input.bind.textures2D.textures_num, ps_input.bind.storage_buffers.buffers_num);
}

bool GraphicsResolvePrimitiveDrawPlan(uint32_t primitive_type, uint32_t guest_count, int vertex_buffers_num, bool indexed,
                                             PrimitiveDrawPlan* plan)
{
	if (plan == nullptr)
	{
		return false;
	}

	PrimitiveDrawPlan resolved {};
	resolved.draw_count = guest_count;

	switch (primitive_type)
	{
		case 1: resolved.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
		case 2: resolved.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
		case 3: resolved.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
		case 4: resolved.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
		case 5: resolved.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
		case 6: resolved.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
		case 7:
		case 17:
			resolved.topology      = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			resolved.requires_rect = !indexed;
			if (!indexed && guest_count == 3 && vertex_buffers_num == 0)
			{
				resolved.draw_count = 4;
			}
			// RectList normally arrives as a procedural three-vertex draw, but
			// titles may bind vertex buffers while retaining the same primitive
			// type.  Vulkan has no RectList topology, so preserve the submitted
			// vertex count and render it as a regular triangle strip when the
			// fourth corner cannot be synthesized safely.
			break;
		case 19:
			resolved.topology    = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
			resolved.chunked     = true;
			resolved.chunk_count = 4;
			if ((guest_count & 0x3u) != 0)
			{
				return false;
			}
			break;
		default: return false;
	}

	*plan = resolved;
	return true;
}

static bool PrimitiveDumpMatches(uint32_t primitive_type)
{
	const char* selector = std::getenv("KYTY_DUMP_PRIMITIVE");
	if (selector == nullptr || selector[0] == '\0')
	{
		return false;
	}

	if (std::strcmp(selector, "rect") == 0)
	{
		return primitive_type == 7u || primitive_type == 17u;
	}

	char* end = nullptr;
	const auto parsed = std::strtoul(selector, &end, 0);
	return end != selector && *end == '\0' && parsed == primitive_type;
}

void MaybeDumpPrimitiveDrawPlan(const char* path, uint32_t primitive_type, uint32_t guest_count, int vertex_buffers_num,
                                       bool indexed, const PrimitiveDrawPlan& plan)
{
	if (!PrimitiveDumpMatches(primitive_type))
	{
		return;
	}

	static uint32_t logs = 0;
	uint32_t        limit = 128u;
	if (const char* env_limit = std::getenv("KYTY_DUMP_PRIMITIVE_LIMIT"); env_limit != nullptr && env_limit[0] != '\0')
	{
		const auto parsed = std::strtoul(env_limit, nullptr, 10);
		if (parsed > 0u && parsed <= 100000u)
		{
			limit = static_cast<uint32_t>(parsed);
		}
	}
	if (logs >= limit)
	{
		return;
	}
	++logs;

	std::fprintf(stderr,
	             "KYTY_DUMP_PRIMITIVE path=%s prim=%u indexed=%u guest_count=%u draw_count=%u chunked=%u chunk_count=%u vertex_buffers=%d "
	             "topology=%u rect=%u\n",
	             path, primitive_type, indexed ? 1u : 0u, guest_count, plan.draw_count, plan.chunked ? 1u : 0u, plan.chunk_count,
	             vertex_buffers_num, static_cast<uint32_t>(plan.topology), plan.requires_rect ? 1u : 0u);
}


static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx)
{
	const auto& vs = sh_ctx.GetVs();
	if (vs.vs_embedded)
	{
		return true;
	}
	// Gen5 NGG: ES address + GS checksum identify a usable vertex program.
	if (Config::IsNextGen())
	{
		return vs.es_regs.data_addr != 0 && vs.gs_regs.chksum != 0;
	}
	return vs.vs_regs.data_addr != 0;
}

static bool ShouldSkipUnsupportedGeShader(const HW::Context& ctx, const HW::UserConfig& ucfg, const HW::Shader& sh_ctx)
{
	if (!Config::IsNextGen())
	{
		return false;
	}
	const auto& vertex_info = sh_ctx.GetVs();
	const auto& sh_regs     = ctx.GetShaderRegisters();
	const auto& ge_cntl     = ucfg.GetGeControl();
	const auto  stages      = ctx.GetShaderStages();

	const auto is_known_gs_out_prim_type = [](uint32_t value) {
		switch (value)
		{
			case 0x0u: // points
			case 0x1u: // lines
			case 0x2u: // triangles
			case 0x3u: // 2d rectangle
			case 0x4u: // rect list
				return true;
			default: return false;
		}
	};

	// Supported NGG vertex path: passthrough stages mask with ES program, GS
	// checksum, zero max-vert-out (hardware NGG passthrough), and a known
	// GS output primitive type. Wider GE/NGG configurations remain unmodeled.
	const bool ngg_vertex_path = stages == 0x02002000u && vertex_info.es_regs.data_addr != 0 &&
	                             vertex_info.gs_regs.chksum != 0 && sh_regs.m_vgtGsMaxVertOut == 0x00000000u &&
	                             is_known_gs_out_prim_type(sh_regs.m_vgtGsOutPrimType);

	const bool unsupported_stage_mask = (stages != 0 && stages != 0x02002000u);
	const bool unsupported_gs_stage =
	    (vertex_info.es_regs.data_addr != 0 && vertex_info.gs_regs.data_addr != 0 && !ngg_vertex_path);
	const bool ge_group_size = ge_cntl.primitive_group_size > 0x0040 || ge_cntl.vertex_group_size > 0x0040;
	const bool ge_shader_regs =
	    (sh_regs.m_geNggSubgrpCntl != 0x00000000 && sh_regs.m_geNggSubgrpCntl != 0x00000001) ||
	    sh_regs.m_vgtGsMaxVertOut != 0x00000000u || !is_known_gs_out_prim_type(sh_regs.m_vgtGsOutPrimType) ||
	    sh_regs.m_geMaxOutputPerSubgroup > 0x00000040u;

	if (unsupported_stage_mask || unsupported_gs_stage || ge_group_size || ge_shader_regs)
	{
		static uint32_t logs = 0;
		if (logs < 16u)
		{
			++logs;
			std::fprintf(stderr,
			             "KYTY_GRAPHICS: skip unsupported GE draw stages=0x%08" PRIx32 " es=0x%012" PRIx64
			             " gs=0x%012" PRIx64 " max_vert=0x%08" PRIx32 " out_prim=0x%08" PRIx32
			             " ge_ngg=0x%08" PRIx32 " max_out=0x%08" PRIx32 "\n",
			             stages, vertex_info.es_regs.data_addr, vertex_info.gs_regs.data_addr, sh_regs.m_vgtGsMaxVertOut,
			             sh_regs.m_vgtGsOutPrimType, sh_regs.m_geNggSubgrpCntl, sh_regs.m_geMaxOutputPerSubgroup);
		}
		return true;
	}
	return false;
}

void GraphicsRenderDrawIndex(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                             uint32_t index_type_and_size, uint32_t index_count, const void* index_addr, uint64_t draw_modifier,
                             uint32_t type)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	// Diagnostic A/B: KYTY_AB_SKIP_ALL_DRAWS=1 skips graphics draws (DEVICE_LOST triage).
	if (const char* ab = std::getenv("KYTY_AB_SKIP_ALL_DRAWS"); ab != nullptr && ab[0] != '\0')
	{
		return;
	}
	if (const char* ab = std::getenv("KYTY_AB_SKIP_INDEXED_DRAWS"); ab != nullptr && ab[0] != '\0')
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());
	if (!DrawHasValidVertexShader(*sh_ctx) || ShouldSkipUnsupportedGeShader(*ctx, *ucfg, *sh_ctx))
	{
		MaybeDumpIndexDrawSkip("invalid-vs-or-ge", index_count, draw_modifier, type);
		return;
	}
	// Diagnostic A/B: skip draws whose depth target is a 1x1 D/S surface while color is larger.
	if (const char* ab = std::getenv("KYTY_AB_SKIP_TINY_DEPTH"); ab != nullptr && ab[0] != '\0')
	{
		RenderDepthInfo probe {};
		DescribeRenderDepthInfo(*ctx, &probe);
		if (probe.format != VK_FORMAT_UNDEFINED && probe.width <= 1u && probe.height <= 1u)
		{
			std::fprintf(stderr, "KYTY_AB_SKIP_TINY_DEPTH skip depth=%ux%u fmt=%u\n", probe.width, probe.height,
			             static_cast<uint32_t>(probe.format));
			std::fflush(stderr);
			return;
		}
	}
	// Diagnostic A/B: skip embedded-VS draws (vs data_addr == 0).
	if (const char* ab = std::getenv("KYTY_AB_SKIP_EMBEDDED_VS"); ab != nullptr && ab[0] != '\0')
	{
		const auto& vs = sh_ctx->GetVs();
		if (vs.vs_embedded || vs.vs_regs.data_addr == 0)
		{
			std::fprintf(stderr, "KYTY_AB_SKIP_EMBEDDED_VS skip vs_embedded=%d vs_addr=0x%012" PRIx64 "\n", vs.vs_embedded ? 1 : 0,
			             vs.vs_regs.data_addr);
			std::fflush(stderr);
			return;
		}
	}
	// Diagnostic A/B: skip draws whose PS data_addr matches KYTY_AB_SKIP_PS_ADDR (hex).
	if (const char* ab_ps = std::getenv("KYTY_AB_SKIP_PS_ADDR"); ab_ps != nullptr && ab_ps[0] != '\0')
	{
		char*      end     = nullptr;
		const auto skip_ps = std::strtoull(ab_ps, &end, 0);
		if (end != ab_ps && skip_ps == sh_ctx->GetPs().ps_regs.data_addr)
		{
			std::fprintf(stderr, "KYTY_AB_SKIP_PS_ADDR skip ps=0x%012" PRIx64 "\n", sh_ctx->GetPs().ps_regs.data_addr);
			std::fflush(stderr);
			return;
		}
	}
	if (std::getenv("KYTY_DUMP_DRAW") != nullptr)
	{
		static uint32_t logs = 0;
		if (logs < 48u)
		{
			++logs;
			const auto& sh_regs = ctx->GetShaderRegisters();
			std::fprintf(stderr,
			             "KYTY_DUMP_DRAW_ENTER_INDEX count=%u modifier=0x%016" PRIx64
			             " type=%u color_mode=%u color_op=0x%x stages=0x%08" PRIx32
			             " es=0x%012" PRIx64 " max_vert=0x%08" PRIx32 " out_prim=0x%08" PRIx32 "\n",
			             index_count, draw_modifier, type, static_cast<uint32_t>(ctx->GetColorControl().mode), ctx->GetColorControl().op,
			             ctx->GetShaderStages(), sh_ctx->GetVs().es_regs.data_addr, sh_regs.m_vgtGsMaxVertOut,
			             sh_regs.m_vgtGsOutPrimType);
		}
	}

	if (GraphicsRenderColorResolve(submit_id, buffer, *ctx))
	{
		MaybeDumpIndexDrawSkip("color-resolve", index_count, draw_modifier, type);
		return;
	}
	const bool depth_stencil_copy = ctx->GetRenderControl().depth_copy || ctx->GetRenderControl().stencil_copy;
	if (depth_stencil_copy)
	{
		MaybeDumpIndexDrawSkip("depth-stencil-copy", index_count, draw_modifier, type);
		uc_print("GraphicsRenderDrawIndex():UserConfig:", *ucfg);
		uc_check(*ucfg);

		hw_print(*ctx);
		hw_check(*ctx, true);

		printf("GraphicsRenderDrawIndex():Parameters:\n");
		printf("\t index_type_and_size = 0x%08" PRIx32 "\n", index_type_and_size);
		printf("\t index_count         = 0x%08" PRIx32 "\n", index_count);
		printf("\t index_addr          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
		printf("\t draw_modifier       = 0x%016" PRIx64 "\n", draw_modifier);
		printf("\t type                = 0x%08" PRIx32 "\n", type);

		EXIT_NOT_IMPLEMENTED(!AutoDrawModifierSupported(draw_modifier));
		GraphicsRenderDepthStencilCopy(submit_id, buffer, ctx, ucfg, sh_ctx, index_count, index_type_and_size, index_addr);
		return;
	}

	if (const char* reason = shader_disable_reason(sh_ctx); reason != nullptr)
	{
		MaybeDumpIndexDrawSkip(reason, index_count, draw_modifier, type);
		return;
	}

	sh_print("GraphicsRenderDrawIndex():Shader:", *sh_ctx);
	sh_check(*sh_ctx);

	uc_print("GraphicsRenderDrawIndex():UserConfig:", *ucfg);
	uc_check(*ucfg);

	hw_print(*ctx);
	hw_check(*ctx);

	EXIT_NOT_IMPLEMENTED(ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000);

	printf("GraphicsRenderDrawIndex():Parameters:\n");
	printf("\t index_type_and_size = 0x%08" PRIx32 "\n", index_type_and_size);
	printf("\t index_count         = 0x%08" PRIx32 "\n", index_count);
	printf("\t index_addr          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
	printf("\t draw_modifier       = 0x%016" PRIx64 "\n", draw_modifier);
	printf("\t type                = 0x%08" PRIx32 "\n", type);

	VkIndexType index_type = VK_INDEX_TYPE_UINT16;
	uint64_t    index_size = 0;

	switch (index_type_and_size)
	{
		case 0:
			index_type = VK_INDEX_TYPE_UINT16;
			index_size = 2 * static_cast<uint64_t>(index_count);
			break;
		case 1:
			index_type = VK_INDEX_TYPE_UINT32;
			index_size = 4 * static_cast<uint64_t>(index_count);
			break;
		default: EXIT_NOT_IMPLEMENTED(index_type_and_size != 0 && index_type_and_size != 1);
	}

	EXIT_NOT_IMPLEMENTED(!AutoDrawModifierSupported(draw_modifier));
	EXIT_NOT_IMPLEMENTED(type != 1);

	RenderDepthInfo depth_info;
	RenderColorInfo color_info;
	DescribeRenderDepthInfo(*ctx, &depth_info);
	DescribeRenderColorInfo(buffer, *ctx, &color_info);
	if (!RenderColorHasActiveTarget(color_info) && depth_info.format == VK_FORMAT_UNDEFINED)
	{
		// A zero target mask with depth disabled is a valid no-output draw.
		MaybeDumpIndexDrawSkip("no-output", index_count, draw_modifier, type);
		return;
	}
	VulkanSampleLocationState sample_locations {};
	aa_check_for_attachment_samples(*ctx, resolve_render_attachment_sample_count(color_info, depth_info), &sample_locations);
	const auto depth_only_resolution = PrepareDepthOnlyDisplayResolutionCohort(buffer, color_info, depth_info);
	RequireSupportedRenderResolutionPlan(depth_only_resolution);

	ShaderVertexInputInfo vs_input_info;
	ShaderGetInputInfoVS(&sh_ctx->GetVs(), &ctx->GetShaderRegisters(), &vs_input_info);
	if (!vs_input_info.input_resources_valid)
	{
		MaybeDumpIndexDrawSkip("invalid-vs-resources", index_count, draw_modifier, type);
		return;
	}

	PrimitiveDrawPlan primitive_plan {};
	if (!GraphicsResolvePrimitiveDrawPlan(ucfg->GetPrimType(), index_count, vs_input_info.buffers_num, true, &primitive_plan))
	{
		static std::atomic_uint32_t unsupported_indexed_draws {0};
		if (unsupported_indexed_draws.fetch_add(1, std::memory_order_relaxed) < 16u)
		{
			std::fprintf(stderr, "WARNING: skipping unsupported indexed primitive: type=%u count=%u vertex_buffers=%d\n",
			             ucfg->GetPrimType(), index_count, vs_input_info.buffers_num);
		}
		MaybeDumpIndexDrawSkip("unsupported-primitive", index_count, draw_modifier, type);
		return;
	}
	MaybeDumpPrimitiveDrawPlan("indexed", ucfg->GetPrimType(), index_count, vs_input_info.buffers_num, true, primitive_plan);

	ShaderPixelInputInfo ps_input_info;
	ShaderGetInputInfoPS(&sh_ctx->GetPs(), &ctx->GetShaderRegisters(), &vs_input_info, &ps_input_info);
	const auto resolution = PrepareDisplayResolutionCohort(buffer, &color_info, depth_info, &ps_input_info);
	RequireSupportedRenderResolutionPlan(resolution);
	const auto& materialization_resolution = !RenderColorHasActiveTarget(color_info) ? depth_only_resolution : resolution;
	MaterializeRenderDepthInfo(
	    submit_id, buffer, &depth_info,
	    materialization_resolution.classification == ResolutionClassification::Scaled ? materialization_resolution.host_extent.width : 0,
	    materialization_resolution.classification == ResolutionClassification::Scaled ? materialization_resolution.host_extent.height : 0,
	    &sample_locations);
	MaterializeRenderColorInfo(submit_id, buffer, &color_info);
	CommitMaterializedRenderResolutionPlan(materialization_resolution, color_info, depth_info);
	// Guest depth size 0 can materialize as 1x1 while color is full-screen; drop it
	// before framebuffer/pipeline creation so Xe does not hang on illegal FB extent.
	SanitizeRenderDepthAgainstColor(&color_info, &depth_info);

	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&color_info, &depth_info);

	EXIT_NOT_IMPLEMENTED(framebuffer == nullptr);
	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	MaybeDumpIndexDrawReady(color_info, depth_info, *ctx, *sh_ctx, vs_input_info, ps_input_info, index_count, index_type_and_size,
	                        draw_modifier, type, ucfg->GetPrimType());
	MaybeDumpUiDraw(color_info, vs_input_info, ps_input_info, *ctx, *ucfg, index_count, index_type_and_size, true,
	                static_cast<uint32_t>(draw_modifier));

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(framebuffer, &color_info, &depth_info, &vs_input_info, ctx, sh_ctx,
	                                                                  &ps_input_info, primitive_plan.topology, sample_locations);

	// EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	BindVertexBuffers(submit_id, buffer, vk_buffer, vs_input_info);

	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, vs_input_info.bind,
	                VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);

	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, ps_input_info.bind,
	                VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);

	const uint64_t index_addr_u64 = reinterpret_cast<uint64_t>(index_addr);
	VulkanBuffer*  indices = TryUploadTransientReadOnlyBuffer(buffer, index_addr_u64, index_size, true, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	if (indices == nullptr)
	{
		indices = static_cast<VulkanBuffer*>(
		    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, index_addr_u64, index_size, IndexBufferGpuObject()));
	}
	EXIT_NOT_IMPLEMENTED(indices == nullptr);

	vkCmdBindIndexBuffer(vk_buffer, indices->buffer, 0, index_type);

	buffer->BeginRenderPass(framebuffer, &color_info, &depth_info, &sample_locations);
	const int32_t vertex_offset = ShaderResolveVertexOffset(ucfg->GetIndexOffset(), vs_input_info);

	if (primitive_plan.chunked)
	{
		for (uint32_t i = 0; i < index_count; i += primitive_plan.chunk_count)
		{
			vkCmdDrawIndexed(vk_buffer, primitive_plan.chunk_count, 1, i, vertex_offset, 0);
			DebugStatsRecordDraw();
		}
	} else
	{
		vkCmdDrawIndexed(vk_buffer, primitive_plan.draw_count, 1, 0, vertex_offset, 0);
		DebugStatsRecordDraw();
	}

	buffer->EndRenderPass();

	// Explicit attachment-write → later shader/attachment-read dependency across the
	// render-pass boundary. Without this, hosts can hang when the next draw samples
	// a color target written earlier in the same command buffer.
	if (vk_buffer != nullptr)
	{
		VkMemoryBarrier memory_barrier {};
		memory_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		                     0, 1, &memory_barrier, 0, nullptr, 0, nullptr);
	}

	MaybeDumpColorTargets(g_render_ctx->GetGraphicCtx(), color_info);

	InvalidateMemoryObject(color_info);
	InvalidateMemoryObject(depth_info);
}

static bool GraphicsRenderDepthStencilCopyClearSource(CommandBuffer* buffer, RenderDepthInfo* source,
                                                       const VulkanSampleLocationState& sample_locations)
{
	EXIT_IF(buffer == nullptr || source == nullptr);

	if (!source->depth_clear_enable && !source->stencil_clear_enable)
	{
		return false;
	}

	RenderColorInfo no_color;
	auto* source_framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&no_color, source);
	EXIT_NOT_IMPLEMENTED(source_framebuffer == nullptr || source_framebuffer->render_pass == nullptr);
	buffer->BeginRenderPass(source_framebuffer, &no_color, source, &sample_locations);
	buffer->EndRenderPass();
	return true;
}

static bool GraphicsRenderDepthStencilCopyStencilTestWrites(const RenderDepthInfo& depth)
{
	if (!depth.stencil_test_enable)
	{
		return false;
	}

	const auto face_writes = [](const PipelineStencilStaticState& face, const PipelineStencilDynamicState& dynamic)
	{
		return dynamic.writeMask != 0 &&
		       (face.failOp != VK_STENCIL_OP_KEEP || face.passOp != VK_STENCIL_OP_KEEP || face.depthFailOp != VK_STENCIL_OP_KEEP);
	};
	return face_writes(depth.stencil_static_front, depth.stencil_dynamic_front) ||
	       face_writes(depth.stencil_static_back, depth.stencil_dynamic_back);
}

static bool GraphicsRenderDepthStencilCopyStencilTestAffectsCoverage(const RenderDepthInfo& depth)
{
	if (!depth.stencil_test_enable)
	{
		return false;
	}
	return depth.stencil_static_front.compareOp != VK_COMPARE_OP_ALWAYS ||
	       depth.stencil_static_back.compareOp != VK_COMPARE_OP_ALWAYS;
}

void GraphicsRenderDepthStencilCopySetStencilTest(DepthStencilCopyStencilTest* target, const RenderDepthInfo& source,
	                                                      bool writable)
{
	EXIT_IF(target == nullptr);
	target->enabled = source.stencil_test_enable;
	if (!target->enabled)
	{
		return;
	}

	const auto copy_face = [writable](DepthStencilCopyStencilFace* destination, const PipelineStencilStaticState& static_state,
	                                  const PipelineStencilDynamicState& dynamic_state)
	{
		destination->fail_op       = (writable ? static_state.failOp : VK_STENCIL_OP_KEEP);
		destination->pass_op       = (writable ? static_state.passOp : VK_STENCIL_OP_KEEP);
		destination->depth_fail_op = (writable ? static_state.depthFailOp : VK_STENCIL_OP_KEEP);
		destination->compare_op    = static_state.compareOp;
		destination->compare_mask  = dynamic_state.compareMask;
		destination->write_mask    = (writable ? dynamic_state.writeMask : 0);
		destination->reference     = dynamic_state.reference;
	};
	copy_face(&target->front, source.stencil_static_front, source.stencil_dynamic_front);
	copy_face(&target->back, source.stencil_static_back, source.stencil_dynamic_back);
}

void GraphicsRenderDepthStencilCopySetDrawArea(const HW::Context& context, bool guest_triangle_strip,
	                                                    const VkExtent2D& guest_extent, const VkExtent2D& host_extent,
	                                                    DepthStencilCopyRequest* request)
{
	EXIT_IF(request == nullptr);

	if (!guest_triangle_strip)
	{
		request->viewport.x        = 0.0f;
		request->viewport.y        = 0.0f;
		request->viewport.width    = static_cast<float>(host_extent.width);
		request->viewport.height   = static_cast<float>(host_extent.height);
		request->viewport.minDepth = 0.0f;
		request->viewport.maxDepth = 1.0f;
		request->scissor.offset    = {0, 0};
		request->scissor.extent    = host_extent;
		return;
	}

	const auto& screen_viewport = context.GetScreenViewport();
	const auto  guest_xy = State::ResolveViewportXy(screen_viewport.viewports[0].xscale, screen_viewport.viewports[0].xoffset,
	                                                 screen_viewport.viewports[0].yscale, screen_viewport.viewports[0].yoffset);
	const auto guest_depth = State::ResolveViewportDepth(
	    screen_viewport.viewports[0].zscale, screen_viewport.viewports[0].zoffset, context.GetClipControl().dx_clip_space,
	    g_render_ctx->GetGraphicCtx()->depth_range_unrestricted_supported, screen_viewport.viewports[0].zmin,
	    screen_viewport.viewports[0].zmax);
	const auto guest_scissor = State::ResolveScissor(screen_viewport, context.GetScanModeControl(), 0);

	RenderResolutionTransform transform {};
	const ResolutionExtent         guest_resolution {guest_extent.width, guest_extent.height};
	const ResolutionExtent         host_resolution {host_extent.width, host_extent.height};
	EXIT_NOT_IMPLEMENTED(CreateRenderResolutionTransform(guest_resolution, host_resolution, &transform) !=
	                     RenderResolutionTransformStatus::Success);

	const ResolutionViewport guest_viewport {guest_xy.x, guest_xy.y, guest_xy.width, guest_xy.height, guest_depth.min_depth,
	                                         guest_depth.max_depth};
	ResolutionViewport host_viewport {};
	EXIT_NOT_IMPLEMENTED(MapRenderResolutionViewport(transform, guest_viewport, &host_viewport) != RenderResolutionTransformStatus::Success);

	const ResolutionScissorRect guest_scissor_rect {guest_scissor.left, guest_scissor.top, guest_scissor.right, guest_scissor.bottom};
	ResolutionScissorRect host_scissor {};
	EXIT_NOT_IMPLEMENTED(MapRenderResolutionScissor(transform, guest_scissor_rect, &host_scissor) != RenderResolutionTransformStatus::Success);
	EXIT_NOT_IMPLEMENTED(host_scissor.left < 0 || host_scissor.top < 0 || host_scissor.right < host_scissor.left ||
	                     host_scissor.bottom < host_scissor.top || static_cast<uint64_t>(host_scissor.right) > host_extent.width ||
	                     static_cast<uint64_t>(host_scissor.bottom) > host_extent.height);

	request->viewport.x        = static_cast<float>(host_viewport.x);
	request->viewport.y        = static_cast<float>(host_viewport.y);
	request->viewport.width    = static_cast<float>(host_viewport.width);
	request->viewport.height   = static_cast<float>(host_viewport.height);
	request->viewport.minDepth = static_cast<float>(host_viewport.min_depth);
	request->viewport.maxDepth = static_cast<float>(host_viewport.max_depth);
	request->scissor.offset    = {static_cast<int32_t>(host_scissor.left), static_cast<int32_t>(host_scissor.top)};
	request->scissor.extent    = {static_cast<uint32_t>(host_scissor.right - host_scissor.left),
	                              static_cast<uint32_t>(host_scissor.bottom - host_scissor.top)};
}

void GraphicsRenderDepthStencilCopyIssueDraw(uint64_t submit_id, CommandBuffer* buffer, VulkanFramebuffer* framebuffer,
	                                                   RenderColorInfo* color, RenderDepthInfo* depth,
	                                                   const DepthStencilCopyRequest& request, bool guest_geometry, bool static_rect_list,
	                                                   const ShaderVertexInputInfo* guest_vertex_input, uint32_t index_count,
	                                                   VulkanBuffer* index_buffer, VkIndexType index_type, int32_t vertex_offset)
{
	EXIT_IF(buffer == nullptr || framebuffer == nullptr || color == nullptr || depth == nullptr);
	EXIT_IF(guest_geometry && guest_vertex_input == nullptr);
	EXIT_IF(index_buffer != nullptr && !guest_geometry);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	buffer->BeginRenderPass(framebuffer, color, depth, &request.sample_locations);
	auto* depth_stencil_copy_renderer = g_render_ctx->GetDepthStencilCopyRenderer();
	const auto draw = depth_stencil_copy_renderer->PrepareDraw(g_render_ctx->GetGraphicCtx(), request);
	depth_stencil_copy_renderer->BindPreparedDraw(vk_buffer, draw);
	if (guest_geometry)
	{
		BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline_layout, guest_vertex_input->bind,
		                VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);
		BindVertexBuffers(submit_id, buffer, vk_buffer, *guest_vertex_input);
		if (index_buffer != nullptr)
		{
			vkCmdBindIndexBuffer(vk_buffer, index_buffer->buffer, 0, index_type);
			vkCmdDrawIndexed(vk_buffer, index_count, 1, 0, vertex_offset, 0);
		} else
		{
			const uint32_t first_vertex = static_cast<uint32_t>(vertex_offset);
			vkCmdDraw(vk_buffer, index_count, 1, first_vertex, 0);
		}
	} else
	{
		const uint32_t vertex_count = static_rect_list ? 4u : 3u;
		vkCmdDraw(vk_buffer, vertex_count, 1, 0, 0);
	}
	DebugStatsRecordDraw();
	buffer->EndRenderPass();
}

void GraphicsRenderDepthStencilCopyWriteDepthStencil(
	uint64_t submit_id, CommandBuffer* buffer, const HW::Context& context, RenderDepthInfo* source_info,
	const VulkanSampleLocationState& sample_locations, bool apply_clear,
	bool effective_depth_write, bool guest_geometry, bool static_rect_list, const DepthStencilCopyVertexStage* vertex_stage,
	const ShaderVertexInputInfo* guest_vertex_input, uint32_t index_count, VulkanBuffer* index_buffer, VkIndexType index_type,
	int32_t vertex_offset)
{
	EXIT_IF(buffer == nullptr || source_info == nullptr);
	EXIT_IF(guest_geometry && guest_vertex_input == nullptr);

	auto* source = source_info->vulkan_buffer;
	EXIT_NOT_IMPLEMENTED(source == nullptr || source->samples != sample_locations.sample_count);

	RenderDepthInfo draw_depth = *source_info;
	if (apply_clear)
	{
		GraphicsRenderDepthStencilCopyClearSource(buffer, &draw_depth, sample_locations);
	}
	draw_depth.depth_clear_enable   = false;
	draw_depth.stencil_clear_enable = false;

	RenderColorInfo no_color;
	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&no_color, &draw_depth);
	EXIT_NOT_IMPLEMENTED(framebuffer == nullptr || framebuffer->render_pass == nullptr);

	DepthStencilCopyRequest request {};
	request.mode                    = DepthStencilCopyMode::DepthStencilOnly;
	request.render_pass             = framebuffer->render_pass;
	request.render_pass_id          = framebuffer->render_pass_id;
	request.extent                  = source->extent;
	request.sample_locations        = sample_locations;
	request.depth_test.enabled      = draw_depth.depth_test_enable;
	request.depth_test.write_enable = effective_depth_write;
	request.depth_test.compare_op   = draw_depth.depth_compare_op;
	GraphicsRenderDepthStencilCopySetStencilTest(&request.stencil_test, draw_depth, true);
	request.vertex_stage = vertex_stage;
	const auto source_guest = source->GetGuestExtent();
	GraphicsRenderDepthStencilCopySetDrawArea(context, guest_geometry, source_guest, source->extent, &request);
	GraphicsRenderDepthStencilCopyIssueDraw(submit_id, buffer, framebuffer, &no_color, &draw_depth, request, guest_geometry, static_rect_list,
	                                        guest_vertex_input, index_count, index_buffer, index_type, vertex_offset);

	InvalidateMemoryObject(draw_depth);
}

void GraphicsRenderDepthStencilCopy(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg,
                                           HW::Shader* sh_ctx, uint32_t index_count, uint32_t index_type_and_size,
                                           const void* index_addr)
{
	EXIT_IF(buffer == nullptr || ctx == nullptr || ucfg == nullptr || sh_ctx == nullptr);

	const auto& render_control = ctx->GetRenderControl();
	RenderDepthInfo depth_info;
	DescribeRenderDepthInfo(*ctx, &depth_info);
	if (depth_info.format == VK_FORMAT_UNDEFINED)
	{
		return;
	}
	const bool effective_depth_write = depth_info.depth_write_enable && !depth_info.suppress_depth_write;
	const uint8_t color_write_mask =
	    State::ResolveColorWriteMask(ctx->GetRenderTargetMask(), ctx->GetShaderRegisters().m_cbShaderMask, 0);
	const bool color_expansion_enabled = (color_write_mask != 0);

	// Rect-list copies synthesize full-target geometry. Guest draws retain the
	// vertex stage so the expansion covers exactly the guest pixels.
	EXIT_NOT_IMPLEMENTED(!render_control.depth_copy || !render_control.stencil_copy);
	VulkanSampleLocationState sample_locations {};
	aa_check_for_attachment_samples(*ctx, depth_info.samples, &sample_locations);
	const bool stencil_test_required = depth_info.stencil_test_enable;
	const bool depth_stencil_write = effective_depth_write || GraphicsRenderDepthStencilCopyStencilTestWrites(depth_info);
	const bool indexed_draw        = (index_addr != nullptr);
	PrimitiveDrawPlan copy_primitive_plan {};
	const bool supported_copy_primitive =
	    GraphicsResolvePrimitiveDrawPlan(ucfg->GetPrimType(), index_count, 0, indexed_draw, &copy_primitive_plan);
	if (supported_copy_primitive)
	{
		MaybeDumpPrimitiveDrawPlan("depth-stencil-copy", ucfg->GetPrimType(), index_count, 0, indexed_draw, copy_primitive_plan);
	}
	const bool static_rect_list = supported_copy_primitive && copy_primitive_plan.requires_rect && copy_primitive_plan.draw_count == 4;
	const bool guest_triangle_strip = (!indexed_draw && ucfg->GetPrimType() == 6 && index_count == 3);
	const bool guest_triangle_list  = (indexed_draw && ucfg->GetPrimType() == 4 && index_count >= 3 && (index_count % 3) == 0);
	const bool guest_geometry       = guest_triangle_strip || guest_triangle_list;
	if (!color_expansion_enabled && !depth_stencil_write)
	{
		// DB validation disables the color expansion when its effective component
		// mask is empty. A clear is the only remaining observable effect.
		MaterializeRenderDepthInfo(submit_id, buffer, &depth_info, 0, 0, &sample_locations);

		RenderDepthInfo source_setup = depth_info;
		if (GraphicsRenderDepthStencilCopyClearSource(buffer, &source_setup, sample_locations))
		{
			InvalidateMemoryObject(source_setup);
		}
		return;
	}
	if (!static_rect_list && !guest_geometry)
	{
		std::fprintf(stderr, "KYTY_GRAPHICS: unsupported depth-stencil-copy primitive=%u count=%u\n", ucfg->GetPrimType(),
		             index_count);
		EXIT_NOT_IMPLEMENTED(!static_rect_list && !guest_geometry);
	}
	ShaderVertexInputInfo        guest_vertex_input {};
	ShaderId                     guest_vertex_id {};
	ShaderTranslationCacheResult guest_vertex_translation {};
	DepthStencilCopyVertexStage  guest_vertex_stage {};
	const DepthStencilCopyVertexStage* request_vertex_stage = nullptr;
	VulkanBuffer*                indices               = nullptr;
	VkIndexType                  index_type            = VK_INDEX_TYPE_UINT16;
	int32_t                      vertex_offset         = 0;
	if (indexed_draw)
	{
		uint64_t index_size = 0;
		switch (index_type_and_size)
		{
			case 0:
				index_type = VK_INDEX_TYPE_UINT16;
				index_size = 2 * static_cast<uint64_t>(index_count);
				break;
			case 1:
				index_type = VK_INDEX_TYPE_UINT32;
				index_size = 4 * static_cast<uint64_t>(index_count);
				break;
			default:
				EXIT_NOT_IMPLEMENTED(index_type_and_size != 0 && index_type_and_size != 1);
		}
		const uint64_t index_addr_u64 = reinterpret_cast<uint64_t>(index_addr);
		indices = TryUploadTransientReadOnlyBuffer(buffer, index_addr_u64, index_size, true, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
		if (indices == nullptr)
		{
			indices = static_cast<VulkanBuffer*>(
			    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, index_addr_u64, index_size, IndexBufferGpuObject()));
		}
		EXIT_NOT_IMPLEMENTED(indices == nullptr);
	}
	if (guest_geometry)
	{
		if (vertex_shader_is_disabled(sh_ctx))
		{
			return;
		}

		const auto& vertex_shader_info = sh_ctx->GetVs();
		const auto& shader_registers   = ctx->GetShaderRegisters();
		ShaderGetInputInfoVS(&vertex_shader_info, &shader_registers, &guest_vertex_input);
		if (!guest_vertex_input.input_resources_valid)
		{
			return;
		}
		guest_vertex_id = ShaderGetIdVS(&vertex_shader_info, &guest_vertex_input);

		auto* translation_cache = g_render_ctx->GetShaderTranslationCache();
		EXIT_IF(translation_cache == nullptr);
		guest_vertex_translation = translation_cache->GetOrCompile(
		    ShaderModuleKey::Create(guest_vertex_id, ShaderModuleStage::Vertex, Config::GetShaderOptimizationType(),
		                            Config::IsNextGen(), Config::SpirvDebugPrintfEnabled()),
		    [&]
		    {
			    auto vertex_code = ShaderParseVS(&vertex_shader_info, &shader_registers);
			    return ShaderRecompileVS(vertex_code, &guest_vertex_input);
		    });
		DebugStatsRecordShaderTranslationCache(guest_vertex_translation.hit, guest_vertex_translation.evicted);
		EXIT_NOT_IMPLEMENTED(guest_vertex_translation.binary.IsEmpty());

		if (ShaderBindRequiresDescriptorSet(guest_vertex_input.bind))
		{
			EXIT_NOT_IMPLEMENTED(guest_vertex_input.bind.descriptor_set_slot != 0);
			guest_vertex_stage.descriptor_set_layout =
			    g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(DescriptorCache::Stage::Vertex, guest_vertex_input.bind);
			EXIT_NOT_IMPLEMENTED(guest_vertex_stage.descriptor_set_layout == nullptr);
		}

		const auto& mode = ctx->GetModeControl();
		guest_vertex_stage.input            = &guest_vertex_input;
		guest_vertex_stage.bind             = &guest_vertex_input.bind;
		guest_vertex_stage.shader_id        = &guest_vertex_id;
		guest_vertex_stage.shader_words     = guest_vertex_translation.binary.GetDataConst();
		guest_vertex_stage.shader_word_count = guest_vertex_translation.binary.Size();
		guest_vertex_stage.topology         = guest_triangle_list ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
		                                                          : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		guest_vertex_stage.cull_front       = mode.cull_front;
		guest_vertex_stage.cull_back        = mode.cull_back;
		guest_vertex_stage.face             = mode.face;
		guest_vertex_stage.dx_clip_space    = ctx->GetClipControl().dx_clip_space;
		request_vertex_stage                 = &guest_vertex_stage;
		vertex_offset                        = ShaderResolveVertexOffset(indexed_draw ? ucfg->GetIndexOffset() : 0, guest_vertex_input);
	}

	if (!color_expansion_enabled)
	{
		MaterializeRenderDepthInfo(submit_id, buffer, &depth_info, 0, 0, &sample_locations);
		GraphicsRenderDepthStencilCopyWriteDepthStencil(
		    submit_id, buffer, *ctx, &depth_info, sample_locations, true, effective_depth_write, guest_geometry, static_rect_list,
		    request_vertex_stage,
		    (guest_geometry ? &guest_vertex_input : nullptr), index_count, indices, index_type, vertex_offset);
		return;
	}

	RenderColorInfo color_info;
	HW::Context     color_context = *ctx;
	color_context.SetRenderTargetMask(color_write_mask);
	DescribeRenderColorInfo(buffer, color_context, &color_info);
	ShaderPixelInputInfo copy_shader_usage {};
	const auto resolution = PrepareDisplayResolutionCohort(buffer, &color_info, depth_info, &copy_shader_usage);
	RequireSupportedRenderResolutionPlan(resolution);

	EXIT_NOT_IMPLEMENTED(depth_info.format != VK_FORMAT_D32_SFLOAT_S8_UINT);
	EXIT_NOT_IMPLEMENTED(color_info.targets_num != 1);
	EXIT_NOT_IMPLEMENTED(depth_info.samples != color_info.attachment[0].samples || sample_locations.sample_count != depth_info.samples);

	MaterializeRenderDepthInfo(submit_id, buffer, &depth_info,
	                           resolution.classification == ResolutionClassification::Scaled ? resolution.host_extent.width : 0,
	                           resolution.classification == ResolutionClassification::Scaled ? resolution.host_extent.height : 0,
	                           &sample_locations);
	MaterializeRenderColorInfo(submit_id, buffer, &color_info);
	CommitMaterializedRenderResolutionPlan(resolution, color_info, depth_info);

	auto* source = depth_info.vulkan_buffer;
	auto* target = color_info.attachment[0].vulkan_buffer;
	EXIT_NOT_IMPLEMENTED(source == nullptr || target == nullptr);
	EXIT_NOT_IMPLEMENTED(source->format != VK_FORMAT_D32_SFLOAT_S8_UINT);
	const bool supported_target_format = target->format == VK_FORMAT_R8G8B8A8_UNORM || target->format == VK_FORMAT_B8G8R8A8_UNORM ||
	                                     target->format == VK_FORMAT_R8G8B8A8_SRGB || target->format == VK_FORMAT_B8G8R8A8_SRGB;
	if (!supported_target_format)
	{
		std::fprintf(stderr, "KYTY_GRAPHICS: unsupported depth-stencil-copy target format=%d render-format=%u width=%u height=%u\n",
		             static_cast<int>(target->format), static_cast<unsigned>(color_info.attachment[0].render_texture_format), target->extent.width,
		             target->extent.height);
		EXIT_NOT_IMPLEMENTED(!supported_target_format);
	}
	EXIT_NOT_IMPLEMENTED(source->samples != target->samples || source->samples != sample_locations.sample_count);
	const auto source_guest = source->GetGuestExtent();
	const auto target_guest = target->GetGuestExtent();
	EXIT_NOT_IMPLEMENTED(source_guest.width != target_guest.width || source_guest.height != target_guest.height);
	EXIT_NOT_IMPLEMENTED(source->memory.unique_id == target->memory.unique_id);

	RenderDepthInfo source_setup = depth_info;
	// A deferred depth/stencil clear initializes the complete sampled source
	// plane independently of the geometry used for the color expansion.
	const bool source_modified = GraphicsRenderDepthStencilCopyClearSource(buffer, &source_setup, sample_locations);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	GraphicsRenderDepthStencilBarrier(vk_buffer, source);
	EXIT_NOT_IMPLEMENTED(source->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

	RenderDepthInfo no_depth;
	RenderDepthInfo copy_depth = source_setup;
	copy_depth.stencil_test_enable = stencil_test_required;
	RenderDepthInfo* depth_attachment = &no_depth;
	const bool copy_stencil_runtime_affects_coverage = GraphicsRenderDepthStencilCopyStencilTestAffectsCoverage(copy_depth);
	if (!copy_stencil_runtime_affects_coverage)
	{
		copy_depth.stencil_test_enable = false;
	}
	const bool copy_requires_depth_stencil_attachment = copy_depth.depth_test_enable || copy_stencil_runtime_affects_coverage;
	if (copy_requires_depth_stencil_attachment)
	{
		// Vulkan can sample an attached depth plane only while that attachment is
		// read-only. A preceding clear has already materialized the source, so the
		// copy phase preserves the guest comparison without another depth write.
		EXIT_NOT_IMPLEMENTED(source->extent.width != target->extent.width || source->extent.height != target->extent.height);
		copy_depth.depth_clear_enable   = false;
		copy_depth.stencil_clear_enable = false;
		copy_depth.depth_write_enable   = false;
		copy_depth.suppress_depth_write = true;
		depth_attachment                = &copy_depth;
	}
	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(
	    &color_info, depth_attachment, (depth_attachment == &copy_depth ? DepthStencilAttachmentAccess::ReadOnly
	                                                                       : DepthStencilAttachmentAccess::Writable));
	EXIT_NOT_IMPLEMENTED(framebuffer == nullptr || framebuffer->render_pass == nullptr);

	DepthStencilCopyRequest request {};
	request.mode             = DepthStencilCopyMode::ExpandToColor;
	request.source           = source;
	request.render_pass      = framebuffer->render_pass;
	request.render_pass_id   = framebuffer->render_pass_id;
	request.extent           = target->extent;
	request.sample_locations = sample_locations;
	request.copy_sample      = render_control.copy_sample;
	request.copy_centroid    = render_control.copy_centroid;
	request.color_write_mask = color_write_mask;
	request.depth_test.enabled    = copy_depth.depth_test_enable;
	request.depth_test.write_enable = false;
	request.depth_test.compare_op = copy_depth.depth_compare_op;
	GraphicsRenderDepthStencilCopySetStencilTest(&request.stencil_test, copy_depth, false);
	request.vertex_stage = request_vertex_stage;
	GraphicsRenderDepthStencilCopySetDrawArea(*ctx, guest_geometry, target_guest, target->extent, &request);
	GraphicsRenderDepthStencilCopyIssueDraw(submit_id, buffer, framebuffer, &color_info, depth_attachment, request,
	                                        guest_geometry, static_rect_list, (guest_geometry ? &guest_vertex_input : nullptr), index_count, indices,
	                                        index_type, vertex_offset);

	MaybeDumpColorTargets(g_render_ctx->GetGraphicCtx(), color_info);
	InvalidateMemoryObject(color_info);
	if (depth_stencil_write)
	{
		// The expansion samples the source in a read-only layout. Commit matching
		// depth/stencil side effects only after that sampled pass has completed.
		GraphicsRenderDepthStencilCopyWriteDepthStencil(
		    submit_id, buffer, *ctx, &source_setup, sample_locations, false, effective_depth_write, guest_geometry,
		    static_rect_list,
		    request_vertex_stage,
		    (guest_geometry ? &guest_vertex_input : nullptr), index_count, indices, index_type, vertex_offset);
	} else if (source_modified)
	{
		InvalidateMemoryObject(source_setup);
	}
}

bool AutoDrawModifierSupported(uint64_t draw_modifier)
{
	constexpr uint64_t modifier_bits =
	    (1ull << 0u) | (1ull << 1u) | (1ull << 2u) | (1ull << 3u) | (1ull << 4u) | (1ull << 5u) | (1ull << 30u) | (1ull << 31u) |
	    (1ull << 32u);
	return (draw_modifier & ~modifier_bits) == 0;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void GraphicsRenderDrawIndexAuto(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                                 uint32_t index_count, uint64_t draw_modifier)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr || ucfg == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr || buffer->IsInvalid());

	// Diagnostic A/B: KYTY_AB_SKIP_ALL_DRAWS=1 skips graphics draws (DEVICE_LOST triage).
	if (const char* ab = std::getenv("KYTY_AB_SKIP_ALL_DRAWS"); ab != nullptr && ab[0] != '\0')
	{
		return;
	}
	if (const char* ab = std::getenv("KYTY_AB_SKIP_AUTO_DRAWS"); ab != nullptr && ab[0] != '\0')
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());
	if (!DrawHasValidVertexShader(*sh_ctx) || ShouldSkipUnsupportedGeShader(*ctx, *ucfg, *sh_ctx))
	{
		return;
	}
	if (const char* ab = std::getenv("KYTY_AB_SKIP_TINY_DEPTH"); ab != nullptr && ab[0] != '\0')
	{
		RenderDepthInfo probe {};
		DescribeRenderDepthInfo(*ctx, &probe);
		if (probe.format != VK_FORMAT_UNDEFINED && probe.width <= 1u && probe.height <= 1u)
		{
			std::fprintf(stderr, "KYTY_AB_SKIP_TINY_DEPTH skip depth=%ux%u fmt=%u\n", probe.width, probe.height,
			             static_cast<uint32_t>(probe.format));
			std::fflush(stderr);
			return;
		}
	}
	if (const char* ab = std::getenv("KYTY_AB_SKIP_EMBEDDED_VS"); ab != nullptr && ab[0] != '\0')
	{
		const auto& vs = sh_ctx->GetVs();
		if (vs.vs_embedded || vs.vs_regs.data_addr == 0)
		{
			std::fprintf(stderr, "KYTY_AB_SKIP_EMBEDDED_VS skip vs_embedded=%d vs_addr=0x%012" PRIx64 "\n", vs.vs_embedded ? 1 : 0,
			             vs.vs_regs.data_addr);
			std::fflush(stderr);
			return;
		}
	}
	if (const char* ab_ps = std::getenv("KYTY_AB_SKIP_PS_ADDR"); ab_ps != nullptr && ab_ps[0] != '\0')
	{
		char*      end     = nullptr;
		const auto skip_ps = std::strtoull(ab_ps, &end, 0);
		if (end != ab_ps && skip_ps == sh_ctx->GetPs().ps_regs.data_addr)
		{
			std::fprintf(stderr, "KYTY_AB_SKIP_PS_ADDR skip ps=0x%012" PRIx64 "\n", sh_ctx->GetPs().ps_regs.data_addr);
			std::fflush(stderr);
			return;
		}
	}
	if (GraphicsRenderColorResolve(submit_id, buffer, *ctx))
	{
		MaybeDumpAutoDrawSkip("color-resolve", index_count, draw_modifier);
		return;
	}
	const bool       depth_stencil_copy = ctx->GetRenderControl().depth_copy || ctx->GetRenderControl().stencil_copy;

	if (depth_stencil_copy)
	{
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", *ucfg);
		uc_check(*ucfg);

		hw_print(*ctx);
		hw_check(*ctx, true);

		printf("GraphicsRenderDrawIndex():Parameters:\n");
		printf("\t index_count         = 0x%08" PRIx32 "\n", index_count);
		printf("\t draw_modifier       = 0x%016" PRIx64 "\n", draw_modifier);

		EXIT_NOT_IMPLEMENTED(!AutoDrawModifierSupported(draw_modifier));
		EXIT_NOT_IMPLEMENTED(ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000);

		GraphicsRenderDepthStencilCopy(submit_id, buffer, ctx, ucfg, sh_ctx, index_count, UINT32_MAX, nullptr);
		return;
	}

	if (const char* reason = shader_disable_reason(sh_ctx); reason != nullptr)
	{
		MaybeDumpAutoDrawSkip(reason, index_count, draw_modifier);
		return;
	}

	sh_print("GraphicsRenderDrawIndexAuto():Shader:", *sh_ctx);
	sh_check(*sh_ctx);

	uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", *ucfg);
	uc_check(*ucfg);

	hw_print(*ctx);
	hw_check(*ctx);

	printf("GraphicsRenderDrawIndex():Parameters:\n");
	printf("\t index_count         = 0x%08" PRIx32 "\n", index_count);
	printf("\t draw_modifier       = 0x%016" PRIx64 "\n", draw_modifier);

	EXIT_NOT_IMPLEMENTED(!AutoDrawModifierSupported(draw_modifier));
	EXIT_NOT_IMPLEMENTED(ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000);

	RenderDepthInfo depth_info;
	RenderColorInfo color_info;
	DescribeRenderDepthInfo(*ctx, &depth_info);
	DescribeRenderColorInfo(buffer, *ctx, &color_info);
	if (!RenderColorHasActiveTarget(color_info) && depth_info.format == VK_FORMAT_UNDEFINED)
	{
		// A zero target mask with depth disabled is a valid no-output draw.
		return;
	}
	VulkanSampleLocationState sample_locations {};
	aa_check_for_attachment_samples(*ctx, resolve_render_attachment_sample_count(color_info, depth_info), &sample_locations);
	const auto depth_only_resolution = PrepareDepthOnlyDisplayResolutionCohort(buffer, color_info, depth_info);
	RequireSupportedRenderResolutionPlan(depth_only_resolution);

	const auto& vertex_shader_info = sh_ctx->GetVs();
	const auto& pixel_shader_info  = sh_ctx->GetPs();
	const auto& shader_regs        = ctx->GetShaderRegisters();
	if (const auto* target = RenderColorFirstConfiguredAttachment(color_info); target != nullptr)
	{
		if (const char* ab = std::getenv("KYTY_AB_SKIP_AUTO_RT_ADDR"); ab != nullptr && ab[0] != '\0')
		{
			char*      end       = nullptr;
			const auto skip_addr = std::strtoull(ab, &end, 0);
			if (end != ab && skip_addr == target->base_addr)
			{
				return;
			}
		}
		if (const char* ab = std::getenv("KYTY_AB_ONLY_AUTO_RT_ADDR"); ab != nullptr && ab[0] != '\0')
		{
			char*      end       = nullptr;
			const auto only_addr = std::strtoull(ab, &end, 0);
			if (end != ab && only_addr != target->base_addr)
			{
				return;
			}
		}
	}

	ShaderVertexInputInfo vs_input_info;
	ShaderGetInputInfoVS(&vertex_shader_info, &shader_regs, &vs_input_info);
	if (!vs_input_info.input_resources_valid)
	{
		MaybeDumpAutoDrawSkip("invalid-vs-resources", index_count, draw_modifier);
		return;
	}

	PrimitiveDrawPlan primitive_plan {};
	if (!GraphicsResolvePrimitiveDrawPlan(ucfg->GetPrimType(), index_count, vs_input_info.buffers_num, false, &primitive_plan))
	{
		static std::atomic_uint32_t unsupported_auto_draws {0};
		if (unsupported_auto_draws.fetch_add(1, std::memory_order_relaxed) < 16u)
		{
			std::fprintf(stderr, "WARNING: skipping unsupported auto-draw primitive: type=%u count=%u vertex_buffers=%d\n",
			             ucfg->GetPrimType(), index_count, vs_input_info.buffers_num);
		}
		MaybeDumpAutoDrawSkip("unsupported-primitive", index_count, draw_modifier);
		return;
	}
	MaybeDumpPrimitiveDrawPlan("auto", ucfg->GetPrimType(), index_count, vs_input_info.buffers_num, false, primitive_plan);

	ShaderPixelInputInfo ps_input_info;
	ShaderGetInputInfoPS(&pixel_shader_info, &shader_regs, &vs_input_info, &ps_input_info);
	const auto resolution = PrepareDisplayResolutionCohort(buffer, &color_info, depth_info, &ps_input_info);
	RequireSupportedRenderResolutionPlan(resolution);
	const auto& materialization_resolution = !RenderColorHasActiveTarget(color_info) ? depth_only_resolution : resolution;
	MaterializeRenderDepthInfo(
	    submit_id, buffer, &depth_info,
	    materialization_resolution.classification == ResolutionClassification::Scaled ? materialization_resolution.host_extent.width : 0,
	    materialization_resolution.classification == ResolutionClassification::Scaled ? materialization_resolution.host_extent.height : 0,
	    &sample_locations);
	MaterializeRenderColorInfo(submit_id, buffer, &color_info);
	CommitMaterializedRenderResolutionPlan(materialization_resolution, color_info, depth_info);
	// Guest depth size 0 can materialize as 1x1 while color is full-screen; drop it
	// before framebuffer/pipeline creation so Xe does not hang on illegal FB extent.
	SanitizeRenderDepthAgainstColor(&color_info, &depth_info);

	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&color_info, &depth_info);

	EXIT_NOT_IMPLEMENTED(framebuffer == nullptr);
	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	MaybeDumpAutoDrawReady(color_info, depth_info, *ctx, *sh_ctx, vs_input_info, ps_input_info, index_count, ucfg->GetPrimType());
	MaybeDumpUiDraw(color_info, vs_input_info, ps_input_info, *ctx, *ucfg, index_count, 0xffffffffu, false,
	                static_cast<uint32_t>(draw_modifier));

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(framebuffer, &color_info, &depth_info, &vs_input_info, ctx, sh_ctx,
	                                                                  &ps_input_info, primitive_plan.topology, sample_locations);

	// EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	BindVertexBuffers(submit_id, buffer, vk_buffer, vs_input_info);

	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, vs_input_info.bind,
	                VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);

	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, ps_input_info.bind,
	                VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);

	buffer->BeginRenderPass(framebuffer, &color_info, &depth_info, &sample_locations);
	const uint32_t first_vertex = static_cast<uint32_t>(ShaderResolveVertexOffset(0, vs_input_info));
	bool           clear_only   = false;
	if (const char* ab = std::getenv("KYTY_AB_CLEAR_ONLY_AUTO_RT_ADDR"); ab != nullptr && ab[0] != '\0')
	{
		char*      end        = nullptr;
		const auto clear_addr = std::strtoull(ab, &end, 0);
		const auto* target    = RenderColorFirstConfiguredAttachment(color_info);
		clear_only            = end != ab && target != nullptr && clear_addr == target->base_addr;
	}

	if (!clear_only && primitive_plan.chunked)
	{
		for (uint32_t i = 0; i < index_count; i += primitive_plan.chunk_count)
		{
			vkCmdDraw(vk_buffer, primitive_plan.chunk_count, 1, first_vertex + i, 0);
			DebugStatsRecordDraw();
		}
	} else if (!clear_only)
	{
		vkCmdDraw(vk_buffer, primitive_plan.draw_count, 1, first_vertex, 0);
		DebugStatsRecordDraw();
	}

	buffer->EndRenderPass();

	if (vk_buffer != nullptr)
	{
		VkMemoryBarrier memory_barrier {};
		memory_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		                     0, 1, &memory_barrier, 0, nullptr, 0, nullptr);
	}

	MaybeDumpColorTargets(g_render_ctx->GetGraphicCtx(), color_info);

	InvalidateMemoryObject(color_info);
	InvalidateMemoryObject(depth_info);
}

void GraphicsRenderDispatchDirect(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::Shader* sh_ctx, uint32_t thread_group_x,
                                  uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	const auto& cs_disable_regs = sh_ctx->GetCs().cs_regs;
	const bool  cs_disabled     = (cs_disable_regs.chksum != 0 ? ShaderIsDisabled2(cs_disable_regs.data_addr, cs_disable_regs.chksum)
	                                                           : ShaderIsDisabled(cs_disable_regs.data_addr));
	if (cs_disabled)
	{
		return;
	}

	// COMPUTE_DISPATCH_INITIATOR bits. Direct-dispatch packets already select the
	// compute queue; USE_THREAD_DIMENSIONS means the packet carries thread counts instead of
	// group counts, so they are divided by the shader's threadgroup size. The
	// remaining bits observed (FORCE_START_AT_000, ORDER_MODE, wave ordering) are
	// hardware scheduling hints that do not change the dispatched grid.
	constexpr uint32_t DISPATCH_COMPUTE_SHADER_EN     = 0x01u;
	constexpr uint32_t DISPATCH_PARTIAL_TG_EN         = 0x02u;
	constexpr uint32_t DISPATCH_FORCE_START_AT_000    = 0x04u;
	constexpr uint32_t DISPATCH_USE_THREAD_DIMENSIONS = 0x20u;
	constexpr uint32_t DISPATCH_ORDER_MODE            = 0x40u;
	constexpr uint32_t DISPATCH_KNOWN_BITS            = DISPATCH_COMPUTE_SHADER_EN | DISPATCH_PARTIAL_TG_EN | DISPATCH_FORCE_START_AT_000 |
	                                                    DISPATCH_USE_THREAD_DIMENSIONS | DISPATCH_ORDER_MODE;

	EXIT_NOT_IMPLEMENTED((mode & ~DISPATCH_KNOWN_BITS) != 0);

	const auto& cs_regs = sh_ctx->GetCs();
	const auto& sh_regs = ctx->GetShaderRegisters();

	if ((mode & DISPATCH_USE_THREAD_DIMENSIONS) != 0)
	{
		const uint32_t lx = cs_regs.cs_regs.num_thread_x;
		const uint32_t ly = cs_regs.cs_regs.num_thread_y;
		const uint32_t lz = cs_regs.cs_regs.num_thread_z;
		EXIT_NOT_IMPLEMENTED(lx == 0 || ly == 0 || lz == 0);
		thread_group_x = (thread_group_x + lx - 1) / lx;
		thread_group_y = (thread_group_y + ly - 1) / ly;
		thread_group_z = (thread_group_z + lz - 1) / lz;
	}

	ShaderComputeInputInfo input_info;
	ShaderGetInputInfoCS(&cs_regs, &sh_regs, &input_info);
	// Diagnostic A/B only (not a product fix):
	//   KYTY_AB_SKIP_ALL_CS=1 — skip every compute dispatch
	//   KYTY_AB_SKIP_TEX_CS=1 — skip compute that binds textures
	//   KYTY_AB_SKIP_CS_ADDR=0x... — skip one guest CS data address (hex)
	if (const char* ab_all = std::getenv("KYTY_AB_SKIP_ALL_CS"); ab_all != nullptr && ab_all[0] != '\0')
	{
		std::fprintf(stderr, "KYTY_AB_SKIP_ALL_CS skip shader=0x%012" PRIx64 "\n", cs_regs.cs_regs.data_addr);
		std::fflush(stderr);
		return;
	}
	if (const char* ab_skip = std::getenv("KYTY_AB_SKIP_TEX_CS");
	    ab_skip != nullptr && ab_skip[0] != '\0' && input_info.bind.textures2D.textures_num > 0)
	{
		std::fprintf(stderr, "KYTY_AB_SKIP_TEX_CS skip shader=0x%012" PRIx64 " textures=%d\n", cs_regs.cs_regs.data_addr,
		             input_info.bind.textures2D.textures_num);
		std::fflush(stderr);
		return;
	}
	if (const char* ab_addr = std::getenv("KYTY_AB_SKIP_CS_ADDR"); ab_addr != nullptr && ab_addr[0] != '\0')
	{
		char*      end      = nullptr;
		const auto skip_addr = std::strtoull(ab_addr, &end, 0);
		if (end != ab_addr && skip_addr == cs_regs.cs_regs.data_addr)
		{
			std::fprintf(stderr, "KYTY_AB_SKIP_CS_ADDR skip shader=0x%012" PRIx64 "\n", cs_regs.cs_regs.data_addr);
			std::fflush(stderr);
			return;
		}
	}
	static const char* dump_dispatch = std::getenv("KYTY_DUMP_DISPATCH");
	const char*        dump_cs_addr  = std::getenv("KYTY_DUMP_CS_ADDR");
	bool               selected_cs   = false;
	if (dump_cs_addr != nullptr && dump_cs_addr[0] != '\0')
	{
		char*      end      = nullptr;
		const auto selected = std::strtoull(dump_cs_addr, &end, 0);
		selected_cs         = end != dump_cs_addr && *end == '\0' && selected == cs_regs.cs_regs.data_addr;
	}
	static uint32_t    dispatch_logs = 0;
	uint32_t           dispatch_limit = 256u;
	if (const char* env_limit = std::getenv("KYTY_DUMP_DISPATCH_LIMIT"); env_limit != nullptr && env_limit[0] != '\0')
	{
		const auto parsed = std::strtoul(env_limit, nullptr, 10);
		if (parsed > 0u && parsed <= 100000u)
		{
			dispatch_limit = static_cast<uint32_t>(parsed);
		}
	}
	if (((dump_dispatch != nullptr && dump_dispatch[0] != '\0') || selected_cs) && dispatch_logs < dispatch_limit &&
	    (selected_cs || GraphicsRunGetFrameNum() <= 5 || std::strcmp(dump_dispatch, "all") == 0))
	{
		++dispatch_logs;
		std::fprintf(stderr,
		             "KYTY_DUMP_DISPATCH frame=%d shader=0x%012" PRIx64 " groups=%ux%ux%u local=%ux%ux%u mode=0x%x "
		             "storage=%d textures=%d direct=%d\n",
		             GraphicsRunGetFrameNum(), cs_regs.cs_regs.data_addr, thread_group_x, thread_group_y, thread_group_z,
		             input_info.threads_num[0], input_info.threads_num[1], input_info.threads_num[2], mode,
		             input_info.bind.storage_buffers.buffers_num, input_info.bind.textures2D.textures_num,
		             input_info.bind.direct_sgprs.sgprs_num);
		for (int i = 0; i < input_info.bind.storage_buffers.buffers_num; ++i)
		{
			const auto& resource = input_info.bind.storage_buffers.buffers[i];
			std::fprintf(stderr,
			             "  storage[%d] reg=%d slot=%d usage=%u access=%u addr=0x%012" PRIx64
			             " stride=%u records=%u fmt=%u dstsel=0x%03" PRIx32 " add_tid=%u fields=%08x,%08x,%08x,%08x\n",
			             i, input_info.bind.storage_buffers.start_register[i], input_info.bind.storage_buffers.slots[i],
			             static_cast<unsigned>(input_info.bind.storage_buffers.usages[i]),
			             static_cast<unsigned>(input_info.bind.storage_buffers.accesses[i]), resource.Base48(), resource.Stride(),
			             resource.NumRecords(), resource.Format(), resource.DstSelXYZW(), resource.AddTid() ? 1u : 0u, resource.fields[0],
			             resource.fields[1], resource.fields[2], resource.fields[3]);
			if (resource.Base48() != 0u && resource.Stride() == 16u && resource.NumRecords() <= 16u)
			{
				const auto* words = reinterpret_cast<const uint32_t*>(resource.Base48());
				std::fprintf(stderr, "    words=");
				for (uint32_t word = 0; word < resource.NumRecords() * 4u; ++word)
				{
					std::fprintf(stderr, "%s%08x", word == 0u ? "" : ",", words[word]);
				}
				std::fprintf(stderr, "\n");
			}
		}
		for (int i = 0; i < input_info.bind.textures2D.textures_num; ++i)
		{
			const auto& texture = input_info.bind.textures2D.desc[i].texture;
			std::fprintf(stderr,
			             "  texture[%d] reg=%d slot=%d usage=%u addr=0x%012" PRIx64
			             " fmt=%u tile=%u size=%ux%u type=%u fields=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
			             i, input_info.bind.textures2D.desc[i].start_register, input_info.bind.textures2D.desc[i].slot,
			             static_cast<unsigned>(input_info.bind.textures2D.desc[i].usage), texture.Base40(), texture.Format(),
			             texture.TileMode(), static_cast<unsigned>(texture.Width5()) + 1u, static_cast<unsigned>(texture.Height5()) + 1u,
			             texture.Type(), texture.fields[0], texture.fields[1], texture.fields[2], texture.fields[3], texture.fields[4],
			             texture.fields[5], texture.fields[6], texture.fields[7]);
		}
		for (int i = 0; i < input_info.bind.direct_sgprs.sgprs_num; ++i)
		{
			std::fprintf(stderr, "  direct[%d] reg=%d value=0x%08x\n", i, input_info.bind.direct_sgprs.start_register[i],
			             input_info.bind.direct_sgprs.sgprs[i].field);
		}
	}

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(&input_info, &sh_ctx->GetCs(), &ctx->GetShaderRegisters());

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	const uint32_t storage_seed_skip_mask = ResolveStorageSeedSkipMask(input_info, thread_group_x, thread_group_y, thread_group_z);
	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline_layout, input_info.bind,
	                VK_SHADER_STAGE_COMPUTE_BIT, DescriptorCache::Stage::Compute, storage_seed_skip_mask);

	vkCmdDispatch(vk_buffer, thread_group_x, thread_group_y, thread_group_z);
	DebugStatsRecordDispatch();
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
