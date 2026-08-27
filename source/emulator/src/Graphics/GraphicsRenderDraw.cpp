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
#include "Emulator/Graphics/Objects/DepthMeta.h"
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

using DrawStageClock = std::chrono::steady_clock;

uint64_t DrawStageElapsedNs(DrawStageClock::time_point start)
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(DrawStageClock::now() - start).count());
}

struct VertexRecordRequirement
{
	uint32_t records = 0;
	bool     valid   = true;
};

VertexRecordRequirement ResolveLinearVertexRecords(int32_t first_vertex, uint32_t vertex_count)
{
	if (vertex_count == 0)
	{
		return {};
	}
	if (first_vertex < 0)
	{
		return {.valid = false};
	}
	const uint64_t records = static_cast<uint64_t>(first_vertex) + vertex_count;
	return records <= UINT32_MAX ? VertexRecordRequirement {.records = static_cast<uint32_t>(records)}
	                             : VertexRecordRequirement {.valid = false};
}

VertexRecordRequirement ResolveIndexedVertexRecords(const void* index_addr, uint32_t index_count, VkIndexType index_type,
	                                                  int32_t vertex_offset)
{
	if (index_addr == nullptr || index_count == 0)
	{
		return {};
	}

	const uint32_t element_size = index_type == VK_INDEX_TYPE_UINT16 ? 2u : (index_type == VK_INDEX_TYPE_UINT32 ? 4u : 0u);
	if (element_size == 0 || index_count > UINT64_MAX / element_size)
	{
		return {.valid = false};
	}
	const uint64_t byte_count = static_cast<uint64_t>(index_count) * element_size;
	constexpr uint64_t MaxIndexScanBytes = 16u * 1024u * 1024u;
	const uint64_t address = reinterpret_cast<uint64_t>(index_addr);
	if (byte_count > MaxIndexScanBytes || GpuMemoryGetAllocatedRangePrefix(address, byte_count) != byte_count)
	{
		return {};
	}

	const auto* bytes = static_cast<const uint8_t*>(index_addr);
	uint32_t    minimum = UINT32_MAX;
	uint32_t    maximum = 0;
	for (uint32_t index = 0; index < index_count; ++index)
	{
		uint32_t value = 0;
		std::memcpy(&value, bytes + static_cast<uint64_t>(index) * element_size, element_size);
		minimum = std::min(minimum, value);
		maximum = std::max(maximum, value);
	}

	const int64_t first = static_cast<int64_t>(minimum) + vertex_offset;
	const int64_t last  = static_cast<int64_t>(maximum) + vertex_offset;
	if (first < 0 || last < 0 || last >= UINT32_MAX)
	{
		return {.valid = false};
	}
	return {.records = static_cast<uint32_t>(last) + 1u};
}

uint32_t ResolveVertexClipProbeDescriptorSet(const ShaderVertexInputInfo& vs_input_info,
	                                           const ShaderPixelInputInfo& ps_input_info)
{
	uint32_t descriptor_set = 0;
	if (ShaderBindRequiresDescriptorSet(vs_input_info.bind))
	{
		EXIT_IF(vs_input_info.bind.descriptor_set_slot != descriptor_set);
		descriptor_set++;
	}
	if (ShaderBindRequiresDescriptorSet(ps_input_info.bind))
	{
		EXIT_IF(ps_input_info.bind.descriptor_set_slot != descriptor_set);
		descriptor_set++;
	}
	EXIT_IF(descriptor_set > 2u);
	return descriptor_set;
}

void ValidatePixelProbeSelection(const HW::PixelShaderInfo& pixel_shader_info, const HW::ShaderRegisters& shader_regs,
	                              ShaderVertexInputInfo* vs_input_info, ShaderPixelInputInfo* ps_input_info)
{
	EXIT_IF(vs_input_info == nullptr || ps_input_info == nullptr);
	const auto kind = ps_input_info->input0_probe.kind;
	if (!ps_input_info->input0_probe.enabled ||
	    (kind != ShaderPixelProbeKind::SampleResult && kind != ShaderPixelProbeKind::FinalMrtResult))
	{
		return;
	}
	const uint64_t present = static_cast<uint64_t>(std::max(0, WindowGetPresentedFrameNum()));
	if (!VertexClipProbeStagesCanReserveAtPresent(vs_input_info->clip_probe, ps_input_info->input0_probe, present))
	{
		return;
	}

	// Validate once at the candidate one-shot draw, before the host descriptor is
	// added to either stage. A bad ordinal leaves the ordinary PS untouched.
	const auto code = ShaderParsePS(&pixel_shader_info, &shader_regs);
	const bool valid_instruction = kind == ShaderPixelProbeKind::SampleResult
	                                   ? ShaderPixelSampleProbeMatchesInstruction(code, ps_input_info->input0_probe)
	                                   : ShaderPixelMrtProbeMatchesInstruction(code, *ps_input_info,
	                                                                          ps_input_info->input0_probe);
	if (!VertexClipProbeValidatePairedPixelInstruction(
	        valid_instruction, &vs_input_info->clip_probe, &ps_input_info->input0_probe))
	{
		vs_input_info->clip_probe_descriptor_set   = kVertexClipProbeInvalidDescriptorSet;
		ps_input_info->input0_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
	}
}

VertexClipProbeRenderer* ReserveVertexClipProbe(CommandBuffer* buffer, ShaderVertexInputInfo* vs_input_info,
	                                             ShaderPixelInputInfo* ps_input_info, uint64_t checksum, uint64_t pixel_checksum,
	                                             bool indexed, uint32_t guest_count, const RenderDepthInfo& depth_info,
	                                             const RenderColorInfo& color_info, const VulkanFramebuffer& framebuffer)
{
	EXIT_IF(buffer == nullptr || vs_input_info == nullptr || ps_input_info == nullptr || g_render_ctx == nullptr);
	const uint64_t present = static_cast<uint64_t>(std::max(0, WindowGetPresentedFrameNum()));
	const bool vertex_probe_selected = vs_input_info->clip_probe.enabled;
	const bool pixel_probe_selected  = ps_input_info->input0_probe.enabled;
	if (!VertexClipProbeStagesCanReserveAtPresent(vs_input_info->clip_probe, ps_input_info->input0_probe, present))
	{
		// Both stages share one process-wide lifecycle and raw buffer. Do not let
		// an earlier stage consume the one-shot before its selected peer reaches
		// its own threshold; the effective combined threshold is the maximum.
		vs_input_info->clip_probe                 = {};
		vs_input_info->clip_probe_descriptor_set  = kVertexClipProbeInvalidDescriptorSet;
		ps_input_info->input0_probe                = {};
		ps_input_info->input0_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
		return nullptr;
	}
	if (!vertex_probe_selected)
	{
		vs_input_info->clip_probe                = {};
		vs_input_info->clip_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
	}
	if (!pixel_probe_selected)
	{
		ps_input_info->input0_probe                = {};
		ps_input_info->input0_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
	}
	const bool vertex_probe_enabled = vs_input_info->clip_probe.enabled;
	const bool pixel_probe_enabled  = ps_input_info->input0_probe.enabled;
	EXIT_IF(pixel_probe_enabled && !ps_input_info->stage_enabled);
	if (!vertex_probe_enabled && !pixel_probe_enabled)
	{
		return nullptr;
	}

	const uint32_t descriptor_set = ResolveVertexClipProbeDescriptorSet(*vs_input_info, *ps_input_info);
	const uint64_t vertex_diagnostic_identity = VertexClipProbeDiagnosticIdentity(descriptor_set);
	const uint64_t pixel_diagnostic_identity =
	    ps_input_info->input0_probe.kind == ShaderPixelProbeKind::SampleResult
	        ? PixelSampleProbeDiagnosticIdentity(descriptor_set, ps_input_info->input0_probe.sample_ordinal,
	                                             ps_input_info->input0_probe.sparse_subgroup)
	        : ps_input_info->input0_probe.kind == ShaderPixelProbeKind::FinalMrtResult
	              ? PixelMrtProbeDiagnosticIdentity(descriptor_set, ps_input_info->input0_probe.mrt_target,
	                                                ps_input_info->input0_probe.export_ordinal)
	              : VertexClipProbeDiagnosticIdentity(descriptor_set);
	EXIT_IF(vertex_diagnostic_identity == 0 || pixel_diagnostic_identity == 0 ||
	        (vertex_probe_enabled && !vs_input_info->clip_probe.draw_scoped) ||
	        (pixel_probe_enabled && (!ps_input_info->input0_probe.draw_scoped ||
	                                 ps_input_info->input0_probe.kind == ShaderPixelProbeKind::None)));
	if (vertex_probe_enabled)
	{
		vs_input_info->clip_probe_descriptor_set      = descriptor_set;
		vs_input_info->clip_probe.diagnostic_identity = vertex_diagnostic_identity;
	}
	if (pixel_probe_enabled)
	{
		ps_input_info->input0_probe_descriptor_set      = descriptor_set;
		ps_input_info->input0_probe.diagnostic_identity = pixel_diagnostic_identity;
	}

	auto* probe_renderer = g_render_ctx->GetVertexClipProbeRenderer();
	EXIT_IF(probe_renderer == nullptr);
	const uint32_t pixel_probe_ordinal = ps_input_info->input0_probe.kind == ShaderPixelProbeKind::FinalMrtResult
	                                         ? ps_input_info->input0_probe.export_ordinal
	                                         : ps_input_info->input0_probe.sample_ordinal;
	const uint32_t attachment_target = ps_input_info->input0_probe.mrt_target;
	const VkAttachmentLoadOp attachment_load_op = attachment_target < VulkanFramebuffer::TARGETS_MAX
	                                                  ? framebuffer.color_load_op[attachment_target]
	                                                  : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	const VkImageLayout attachment_initial_layout = attachment_target < VulkanFramebuffer::TARGETS_MAX
	                                                    ? framebuffer.color_initial_layout[attachment_target]
	                                                    : VK_IMAGE_LAYOUT_UNDEFINED;
	if (!probe_renderer->Reserve(g_render_ctx->GetGraphicCtx(), buffer, checksum, indexed, guest_count, descriptor_set,
	                            pixel_checksum, vertex_probe_enabled, pixel_probe_enabled, true,
	                            depth_info.depth_test_enable, depth_info.stencil_test_enable,
	                            depth_info.depth_bounds_test_enable, ps_input_info->input0_probe.kind,
	                            pixel_probe_ordinal, ps_input_info->input0_probe.match_ordinal,
	                            ps_input_info->input0_probe.mrt_target, ps_input_info->input0_probe.sparse_subgroup,
	                            ps_input_info->input0_probe.attachment_readback,
	                            ps_input_info->input0_probe.attachment_min_invocations, &color_info, attachment_load_op,
	                            attachment_initial_layout))
	{
		// A completed or already-reserved process-wide diagnostic must remain an
		// ordinary draw before any shader/pipeline identity is observed.
		vs_input_info->clip_probe                = {};
		vs_input_info->clip_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
		ps_input_info->input0_probe                = {};
		ps_input_info->input0_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
		return nullptr;
	}
	if (pixel_probe_enabled && ps_input_info->input0_probe.kind == ShaderPixelProbeKind::FinalMrtResult)
	{
		TraceRenderTargetLifetimeSelectProbeColor(color_info, attachment_target, pixel_probe_ordinal);
	}
	return probe_renderer;
}

bool DispatchTextureExtentMatches(const ShaderComputeInputInfo& input, const char* specification)
{
	if (specification == nullptr || specification[0] == '\0')
	{
		return true;
	}

	uint32_t width  = 0;
	uint32_t height = 0;
	if (std::sscanf(specification, "%ux%u", &width, &height) != 2 || width == 0u || height == 0u)
	{
		return false;
	}

	for (int index = 0; index < input.bind.textures2D.textures_num; ++index)
	{
		const auto& texture = input.bind.textures2D.desc[index].texture;
		if (texture.Width5() + 1u == width && texture.Height5() + 1u == height)
		{
			return true;
		}
	}
	return false;
}

} // namespace

// DrawIndex, DrawIndexAuto, DispatchDirect, depth-stencil copy

void GraphicsRenderDepthStencilCopy(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg,
                                           HW::Shader* sh_ctx, uint32_t index_count, uint32_t index_type_and_size,
	                                       const void* index_addr, uint32_t instance_count, int32_t vertex_offset_add,
	                                       uint32_t first_instance);
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

		const auto shape = ShaderResolvedSampledTextureShape(descriptor);
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
			KYTY_LOG_DEBUG( "KYTY_STORAGE_SEED_SKIP mask=0x%08" PRIx32 " global=%" PRIu64 "x%" PRIu64 "x%" PRIu64 "\n",
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
	KYTY_LOG_DEBUG( "KYTY_DUMP_DRAW_SKIP_AUTO reason=%s count=%u modifier=0x%016" PRIx64 "\n", reason, index_count,
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
	KYTY_LOG_DEBUG( "KYTY_DUMP_DRAW_SKIP_INDEX reason=%s count=%u modifier=0x%016" PRIx64 " type=%u\n", reason, index_count,
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
		KYTY_LOG_INFO(
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

	KYTY_LOG_DEBUG(
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
		KYTY_LOG_DEBUG(
		             "KYTY_DUMP_PS_BUFFER index=%d slot=%d reg=%d usage=%u access=%u source=%u desc=%08x,%08x,%08x,%08x"
		             " addr=0x%012" PRIx64 " stride=%u records=%u bytes=%" PRIu64 " readable=%" PRIu64 " words=",
		             i, buffers.slots[i], buffers.start_register[i], static_cast<unsigned>(buffers.usages[i]),
		             static_cast<unsigned>(buffers.accesses[i]), static_cast<unsigned>(buffers.sources[i]), resource.fields[0],
		             resource.fields[1], resource.fields[2], resource.fields[3], address, resource.Stride(), resource.NumRecords(),
		             declared, readable);
		const auto* words = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(address));
		for (uint64_t word = 0; word < readable / sizeof(uint32_t); ++word)
		{
			KYTY_LOG_DEBUG( "%s%08x", word == 0 ? "" : ",", words[word]);
		}
		KYTY_LOG_DEBUG( "\n");
	}

	const auto& textures = input.bind.textures2D;
	for (int i = 0; i < textures.textures_num; ++i)
	{
		const auto& descriptor = textures.desc[i];
		const auto& resource   = descriptor.texture;
		KYTY_LOG_DEBUG(
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
		KYTY_LOG_DEBUG(
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
	KYTY_LOG_DEBUG(
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
	KYTY_LOG_DEBUG(
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
	KYTY_LOG_DEBUG(
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

	KYTY_LOG_DEBUG(
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
			KYTY_LOG_INFO(
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
                             uint32_t type, uint32_t instance_count, int32_t vertex_offset_add, uint32_t first_instance)
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

	const auto render_lock_start = DrawStageClock::now();
	Core::LockGuard lock(g_render_ctx->GetMutex());
	DebugStatsRecordDrawRenderLockWait(DrawStageElapsedNs(render_lock_start));
	const auto state_setup_start = DrawStageClock::now();
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
			KYTY_LOG_DEBUG( "KYTY_AB_SKIP_TINY_DEPTH skip depth=%ux%u fmt=%u\n", probe.width, probe.height,
			             static_cast<uint32_t>(probe.format));
			return;
		}
	}
	// Diagnostic A/B: skip embedded-VS draws (vs data_addr == 0).
	if (const char* ab = std::getenv("KYTY_AB_SKIP_EMBEDDED_VS"); ab != nullptr && ab[0] != '\0')
	{
		const auto& vs = sh_ctx->GetVs();
		if (vs.vs_embedded || vs.vs_regs.data_addr == 0)
		{
			KYTY_LOG_DEBUG( "KYTY_AB_SKIP_EMBEDDED_VS skip vs_embedded=%d vs_addr=0x%012" PRIx64 "\n", vs.vs_embedded ? 1 : 0,
			             vs.vs_regs.data_addr);
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
			KYTY_LOG_DEBUG( "KYTY_AB_SKIP_PS_ADDR skip ps=0x%012" PRIx64 "\n", sh_ctx->GetPs().ps_regs.data_addr);
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
			KYTY_LOG_DEBUG(
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

		KYTY_LOG_DEBUG("GraphicsRenderDrawIndex():Parameters:\n");
		KYTY_LOG_DEBUG("\t index_type_and_size = 0x%08" PRIx32 "\n", index_type_and_size);
		KYTY_LOG_DEBUG("\t index_count         = 0x%08" PRIx32 "\n", index_count);
		KYTY_LOG_DEBUG("\t index_addr          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
		KYTY_LOG_DEBUG("\t draw_modifier       = 0x%016" PRIx64 "\n", draw_modifier);
		KYTY_LOG_DEBUG("\t type                = 0x%08" PRIx32 "\n", type);

		if (!AutoDrawModifierSupported(draw_modifier)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !AutoDrawModifierSupported(draw_modifier) condition ignored (continuing)\n"); }
		GraphicsRenderDepthStencilCopy(submit_id, buffer, ctx, ucfg, sh_ctx, index_count, index_type_and_size, index_addr,
		                                      instance_count, vertex_offset_add, first_instance);
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

	if (ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000 condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("GraphicsRenderDrawIndex():Parameters:\n");
	KYTY_LOG_DEBUG("\t index_type_and_size = 0x%08" PRIx32 "\n", index_type_and_size);
	KYTY_LOG_DEBUG("\t index_count         = 0x%08" PRIx32 "\n", index_count);
	KYTY_LOG_DEBUG("\t index_addr          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
	KYTY_LOG_DEBUG("\t draw_modifier       = 0x%016" PRIx64 "\n", draw_modifier);
	KYTY_LOG_DEBUG("\t type                = 0x%08" PRIx32 "\n", type);
	KYTY_LOG_DEBUG("\t instance_count      = 0x%08" PRIx32 "\n", instance_count);
	KYTY_LOG_DEBUG("\t vertex_offset_add   = 0x%08" PRIx32 "\n", static_cast<uint32_t>(vertex_offset_add));
	KYTY_LOG_DEBUG("\t first_instance      = 0x%08" PRIx32 "\n", first_instance);

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
		default: if (index_type_and_size != 0 && index_type_and_size != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: index_type_and_size != 0 && index_type_and_size != 1 condition ignored (continuing)\n"); }
	}

	if (!AutoDrawModifierSupported(draw_modifier)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !AutoDrawModifierSupported(draw_modifier) condition ignored (continuing)\n"); }
	if (type != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: type != 1 condition ignored (continuing)\n"); }

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
	if (ShaderVertexClipProbeEligible(Config::IsNextGen(), sh_ctx->GetVs().vs_embedded))
	{
		vs_input_info.clip_probe = ShaderResolveVertexClipProbeConfig(sh_ctx->GetVs().gs_regs.chksum, true, index_count);
	}
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
			KYTY_LOG_DEBUG( "WARNING: skipping unsupported indexed primitive: type=%u count=%u vertex_buffers=%d\n",
			             ucfg->GetPrimType(), index_count, vs_input_info.buffers_num);
		}
		MaybeDumpIndexDrawSkip("unsupported-primitive", index_count, draw_modifier, type);
		return;
	}
	MaybeDumpPrimitiveDrawPlan("indexed", ucfg->GetPrimType(), index_count, vs_input_info.buffers_num, true, primitive_plan);

	ShaderPixelInputInfo ps_input_info;
	const bool ps_required = State::PixelShaderStageRequired(ctx->GetRenderTargetMask(), ctx->GetShaderRegisters(), ctx->GetDepthControl());
	ShaderGetInputInfoPS(&sh_ctx->GetPs(), &ctx->GetShaderRegisters(), &vs_input_info, &ps_input_info, !ps_required);
	ps_input_info.fragment_tap = ShaderResolveFragmentTapConfig(sh_ctx->GetPs().ps_regs.chksum, true, index_count);
	if (ShaderPixelInput0ProbeEligible(Config::IsNextGen(), sh_ctx->GetPs().ps_embedded))
	{
		ps_input_info.input0_probe =
		    ShaderResolvePixelProbeConfig(sh_ctx->GetPs().ps_regs.chksum, true, index_count, ps_input_info.stage_enabled);
	}
	ValidatePixelProbeSelection(sh_ctx->GetPs(), ctx->GetShaderRegisters(), &vs_input_info, &ps_input_info);
	const auto resolution = PrepareDisplayResolutionCohort(buffer, &color_info, depth_info, &ps_input_info);
	RequireSupportedRenderResolutionPlan(resolution);
	DebugStatsRecordDrawStateSetup(DrawStageElapsedNs(state_setup_start));
	const auto materialization_start = DrawStageClock::now();
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
	DebugStatsRecordDrawMaterialization(DrawStageElapsedNs(materialization_start));

	const auto pipeline_setup_start = DrawStageClock::now();
	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&color_info, &depth_info);

	if (framebuffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: framebuffer == nullptr condition ignored (continuing)\n"); }
	if (framebuffer->render_pass == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: framebuffer->render_pass == nullptr condition ignored (continuing)\n"); }

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	int32_t vertex_offset = 0;
	if (!ShaderResolveVertexOffset(ucfg->GetIndexOffset(), vs_input_info, &vertex_offset, vertex_offset_add))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: indexed vertex offset is outside int32 range; draw skipped\n");
		return;
	}
	const auto vertex_requirement = ResolveIndexedVertexRecords(index_addr, index_count, index_type, vertex_offset);
	if (!vertex_requirement.valid)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: indexed vertex range is invalid; draw skipped\n");
		return;
	}
	const uint32_t vertex_records = vertex_requirement.records;

	MaybeDumpIndexDrawReady(color_info, depth_info, *ctx, *sh_ctx, vs_input_info, ps_input_info, index_count, index_type_and_size,
	                        draw_modifier, type, ucfg->GetPrimType());
	MaybeDumpUiDraw(color_info, vs_input_info, ps_input_info, *ctx, *ucfg, index_count, index_type_and_size, true,
	                static_cast<uint32_t>(draw_modifier));

	auto* vertex_clip_probe = ReserveVertexClipProbe(buffer, &vs_input_info, &ps_input_info, sh_ctx->GetVs().gs_regs.chksum,
	                                                sh_ctx->GetPs().ps_regs.chksum, true, index_count, depth_info, color_info,
	                                                *framebuffer);
	if (vertex_clip_probe != nullptr && ps_input_info.input0_probe.kind == ShaderPixelProbeKind::FinalMrtResult)
	{
		TraceRenderTargetLifetimeProbeDepthAttempt(
		    submit_id, sh_ctx->GetPs().ps_regs.chksum, sh_ctx->GetVs().gs_regs.chksum, true, index_count,
		    ps_input_info.input0_probe.match_ordinal, ps_input_info.input0_probe.mrt_target,
		    ps_input_info.input0_probe.export_ordinal, depth_info, *framebuffer, *ctx, sample_locations);
	}
	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(framebuffer, &color_info, &depth_info, &vs_input_info, ctx, sh_ctx,
	                                                                  &ps_input_info, primitive_plan.topology, sample_locations);
	DebugStatsRecordDrawPipelineSetup(DrawStageElapsedNs(pipeline_setup_start));

	// EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);

	const auto resource_binding_start = DrawStageClock::now();
	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	const auto     vertex_buffer_binding_start = DrawStageClock::now();
	BindVertexBuffers(submit_id, buffer, vk_buffer, vs_input_info, vertex_records);
	DebugStatsRecordDrawVertexBufferBinding(DrawStageElapsedNs(vertex_buffer_binding_start));

	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, vs_input_info.bind,
	                VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);

	uint32_t declared_vertex_records = 0;
	for (int buffer_index = 0; buffer_index < vs_input_info.buffers_num; ++buffer_index)
	{
		declared_vertex_records = std::max(declared_vertex_records, vs_input_info.buffers[buffer_index].num_records);
	}
	const auto guest_vp_xy = State::ResolveViewportXy(ctx->GetScreenViewport().viewports[0].xscale,
	                                                  ctx->GetScreenViewport().viewports[0].xoffset,
	                                                  ctx->GetScreenViewport().viewports[0].yscale,
	                                                  ctx->GetScreenViewport().viewports[0].yoffset);
	const auto guest_vp_z = State::ResolveViewportDepth(
	    ctx->GetScreenViewport().viewports[0].zscale, ctx->GetScreenViewport().viewports[0].zoffset,
	    ctx->GetClipControl().dx_clip_space, g_render_ctx->GetGraphicCtx()->depth_range_unrestricted_supported,
	    ctx->GetScreenViewport().viewports[0].zmin, ctx->GetScreenViewport().viewports[0].zmax);
	const DrawMaterialTraceContext material_trace {.ps_checksum = sh_ctx->GetPs().ps_regs.chksum,
	                                               .ps_addr = sh_ctx->GetPs().ps_regs.data_addr,
	                                               .ps_in_control = ctx->GetShaderRegisters().ps_in_control,
	                                               .ps_required_subgroup = ps_input_info.required_subgroup_size,
	                                               .vs_checksum = sh_ctx->GetVs().gs_regs.chksum,
	                                               .vs_addr = sh_ctx->GetVs().vs_regs.data_addr != 0
	                                                              ? sh_ctx->GetVs().vs_regs.data_addr
	                                                              : sh_ctx->GetVs().es_regs.data_addr,
	                                               .vs_export_count = vs_input_info.export_count,
	                                               .vertex_float_mode = vs_input_info.gs_prolog
	                                                                        ? sh_ctx->GetVs().gs_regs.rsrc1.float_mode
	                                                                        : sh_ctx->GetVs().vs_regs.rsrc1.float_mode,
	                                               .vertex_dx10_clamp = vs_input_info.gs_prolog
	                                                                         ? sh_ctx->GetVs().gs_regs.rsrc1.dx10_clamp
	                                                                         : sh_ctx->GetVs().vs_regs.rsrc1.dx10_clamp,
	                                               .vertex_ieee_mode = vs_input_info.gs_prolog
	                                                                      ? sh_ctx->GetVs().gs_regs.rsrc1.ieee_mode
	                                                                      : sh_ctx->GetVs().vs_regs.rsrc1.ieee_mode,
	                                               .indexed = true,
	                                               .primitive_type = ucfg->GetPrimType(),
	                                               .guest_count = index_count,
	                                               .index_type = index_type_and_size,
	                                               .index_addr = reinterpret_cast<uint64_t>(index_addr),
	                                               .index_size = index_size,
	                                               .vertex_offset = vertex_offset,
	                                               .instance_count = instance_count,
	                                               .first_instance = first_instance,
	                                               .required_vertex_records = vertex_records,
	                                               .vertex_input = &vs_input_info,
	                                               .color = &color_info,
	                                               .depth = &depth_info,
	                                               .hardware = ctx,
	                                               .framebuffer = framebuffer,
	                                               .declared_vertex_records = declared_vertex_records,
	                                               .target_mask = ctx->GetRenderTargetMask(),
	                                               .blend_enable = ctx->GetBlendControl(0).enable ? 1u : 0u,
	                                               .blend_src = ctx->GetBlendControl(0).color_srcblend,
	                                               .blend_op = ctx->GetBlendControl(0).color_comb_fcn,
	                                               .blend_dst = ctx->GetBlendControl(0).color_destblend,
	                                               .blend_bypass = ctx->GetRenderTarget(0).info.blend_bypass ? 1u : 0u,
	                                               .color_mask = State::ResolveColorWriteAgainstDepth(
	                                                   State::ResolveColorWriteMask(ctx->GetRenderTargetMask(),
	                                                                                ctx->GetShaderRegisters().m_cbShaderMask, 0),
	                                                   ctx->GetDepthControl().color_writes_on_depth_pass_disable,
	                                                   ctx->GetDepthControl().color_writes_on_depth_fail_enable),
	                                               .color_on_depth_fail =
	                                                   ctx->GetDepthControl().color_writes_on_depth_fail_enable ? 1u : 0u,
	                                               .color_off_depth_pass =
	                                                   ctx->GetDepthControl().color_writes_on_depth_pass_disable ? 1u : 0u,
	                                               .cull_front = ctx->GetModeControl().cull_front ? 1u : 0u,
	                                               .cull_back = ctx->GetModeControl().cull_back ? 1u : 0u,
	                                               .face = ctx->GetModeControl().face ? 1u : 0u,
	                                               .vp_x = guest_vp_xy.x,
	                                               .vp_y = guest_vp_xy.y,
	                                               .vp_width = guest_vp_xy.width,
	                                               .vp_height = guest_vp_xy.height,
	                                               .vp_zmin = guest_vp_z.min_depth,
	                                               .vp_zmax = guest_vp_z.max_depth,
	                                               .dx_clip = ctx->GetClipControl().dx_clip_space ? 1u : 0u,
	                                               .ps_input_num = ps_input_info.input_num,
	                                               .interpolators = {ps_input_info.interpolator_settings[0],
	                                                                 ps_input_info.interpolator_settings[1],
	                                                                 ps_input_info.interpolator_settings[2],
	                                                                 ps_input_info.interpolator_settings[3],
	                                                                 ps_input_info.interpolator_settings[4],
	                                                                 ps_input_info.interpolator_settings[5],
	                                                                 ps_input_info.interpolator_settings[6],
	                                                                 ps_input_info.interpolator_settings[7]}};
	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, ps_input_info.bind,
	                VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel, 0, &material_trace);
	TraceRenderTargetLifetimeDraw(submit_id, material_trace);

	const uint64_t index_addr_u64 = reinterpret_cast<uint64_t>(index_addr);
	const auto     index_buffer_binding_start = DrawStageClock::now();
	VulkanBuffer*  indices = TryUploadTransientReadOnlyBuffer(buffer, index_addr_u64, index_size, true, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	if (indices == nullptr)
	{
		indices = static_cast<VulkanBuffer*>(
		    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, index_addr_u64, index_size, IndexBufferGpuObject()));
	}
	if (indices == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: indices == nullptr condition ignored (continuing)\n"); }

	vkCmdBindIndexBuffer(vk_buffer, indices->buffer, 0, index_type);
	DebugStatsRecordDrawIndexBufferBinding(DrawStageElapsedNs(index_buffer_binding_start));
	DebugStatsRecordDrawResourceBinding(DrawStageElapsedNs(resource_binding_start));

	const auto command_emission_start = DrawStageClock::now();
	if (vertex_clip_probe != nullptr)
	{
		vertex_clip_probe->Arm(buffer, pipeline->pipeline_layout);
		vertex_clip_probe->CaptureAttachmentBeforePass(buffer);
	}
	buffer->BeginRenderPass(framebuffer, &color_info, &depth_info, &sample_locations);
	if (vertex_clip_probe != nullptr)
	{
		vertex_clip_probe->BeginDepthPassQuery(buffer);
	}
	if (primitive_plan.chunked)
	{
		for (uint32_t i = 0; i < index_count; i += primitive_plan.chunk_count)
		{
			vkCmdDrawIndexed(vk_buffer, primitive_plan.chunk_count, instance_count, i, vertex_offset, first_instance);
			DebugStatsRecordDraw();
		}
	} else
	{
		vkCmdDrawIndexed(vk_buffer, primitive_plan.draw_count, instance_count, 0, vertex_offset, first_instance);
		DebugStatsRecordDraw();
	}
	if (vertex_clip_probe != nullptr)
	{
		vertex_clip_probe->EndDepthPassQuery(buffer);
	}

	buffer->EndRenderPass();
	if (vertex_clip_probe != nullptr)
	{
		vertex_clip_probe->Finish(buffer);
	}

	// Explicit attachment-write → later shader/attachment-read dependency across the
	// render-pass boundary. Without this, hosts can hang when the next draw samples
	// a color target written earlier in the same command buffer.
	if (vk_buffer != nullptr)
	{
		VkMemoryBarrier memory_barrier {};
		memory_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		                     0, 1, &memory_barrier, 0, nullptr, 0, nullptr);
	}

	MaybeDumpColorTargets(g_render_ctx->GetGraphicCtx(), color_info);

	InvalidateMemoryObject(color_info);
	InvalidateMemoryObject(depth_info);
	DebugStatsRecordDrawCommandEmission(DrawStageElapsedNs(command_emission_start));
}

static bool GraphicsRenderDepthStencilCopyClearSource(uint64_t submit_id, CommandBuffer* buffer, RenderDepthInfo* source,
                                                       const VulkanSampleLocationState& sample_locations)
{
	EXIT_IF(buffer == nullptr || source == nullptr);

	if (!source->depth_clear_enable && !source->stencil_clear_enable)
	{
		return false;
	}

	RenderColorInfo no_color;
	auto* source_framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&no_color, source);
	if (source_framebuffer == nullptr || source_framebuffer->render_pass == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source_framebuffer == nullptr || source_framebuffer->render_pass == nullptr condition ignored (continuing)\n"); }
	TraceRenderTargetLifetimeDepthClearPass(submit_id, *source, *source_framebuffer);
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
	if (CreateRenderResolutionTransform(guest_resolution, host_resolution, &transform) !=
	                     RenderResolutionTransformStatus::Success) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: CreateRenderResolutionTransform(guest_resolution, host_resolution, &transform) ! condition ignored (continuing)\n"); }

	const ResolutionViewport guest_viewport {guest_xy.x, guest_xy.y, guest_xy.width, guest_xy.height, guest_depth.min_depth,
	                                         guest_depth.max_depth};
	ResolutionViewport host_viewport {};
	if (MapRenderResolutionViewport(transform, guest_viewport, &host_viewport) != RenderResolutionTransformStatus::Success) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: MapRenderResolutionViewport(transform, guest_viewport, &host_viewport) != RenderResolutionTransformStatus::Success condition ignored (continuing)\n"); }

	const ResolutionScissorRect guest_scissor_rect {guest_scissor.left, guest_scissor.top, guest_scissor.right, guest_scissor.bottom};
	ResolutionScissorRect host_scissor {};
	if (MapRenderResolutionScissor(transform, guest_scissor_rect, &host_scissor) != RenderResolutionTransformStatus::Success) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: MapRenderResolutionScissor(transform, guest_scissor_rect, &host_scissor) != RenderResolutionTransformStatus::Success condition ignored (continuing)\n"); }
	if (host_scissor.left < 0 || host_scissor.top < 0 || host_scissor.right < host_scissor.left ||
	                     host_scissor.bottom < host_scissor.top || static_cast<uint64_t>(host_scissor.right) > host_extent.width ||
	                     static_cast<uint64_t>(host_scissor.bottom) > host_extent.height) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: host_scissor.left < 0 || host_scissor.top < 0 || host_scissor.right < host_sciss condition ignored (continuing)\n"); }

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
	                                                   VulkanBuffer* index_buffer, VkIndexType index_type, int32_t vertex_offset,
	                                                   uint32_t vertex_records, uint32_t instance_count, uint32_t first_instance)
{
	EXIT_IF(buffer == nullptr || framebuffer == nullptr || color == nullptr || depth == nullptr);
	EXIT_IF(guest_geometry && guest_vertex_input == nullptr);
	EXIT_IF(index_buffer != nullptr && !guest_geometry);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	auto* depth_stencil_copy_renderer = g_render_ctx->GetDepthStencilCopyRenderer();
	const auto draw = depth_stencil_copy_renderer->PrepareDraw(g_render_ctx->GetGraphicCtx(), request);
	depth_stencil_copy_renderer->BindPreparedDraw(vk_buffer, draw);
	if (guest_geometry)
	{
		BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline_layout, guest_vertex_input->bind,
		                VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);
		BindVertexBuffers(submit_id, buffer, vk_buffer, *guest_vertex_input, vertex_records);
		if (index_buffer != nullptr)
		{
			vkCmdBindIndexBuffer(vk_buffer, index_buffer->buffer, 0, index_type);
		}
	}
	// Guest descriptor preparation can materialize sampled resources with
	// transfer/compute commands. Finish it before entering render-pass scope.
	TraceRenderTargetLifetimeDepthClearPass(submit_id, *depth, *framebuffer);
	buffer->BeginRenderPass(framebuffer, color, depth, &request.sample_locations);
	if (guest_geometry)
	{
		if (index_buffer != nullptr)
		{
			vkCmdDrawIndexed(vk_buffer, index_count, instance_count, 0, vertex_offset, first_instance);
		} else
		{
			const uint32_t first_vertex = static_cast<uint32_t>(vertex_offset);
			vkCmdDraw(vk_buffer, index_count, instance_count, first_vertex, first_instance);
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
	int32_t vertex_offset, uint32_t vertex_records, uint32_t instance_count, uint32_t first_instance)
{
	EXIT_IF(buffer == nullptr || source_info == nullptr);
	EXIT_IF(guest_geometry && guest_vertex_input == nullptr);

	auto* source = source_info->vulkan_buffer;
	if (source == nullptr || source->samples != sample_locations.sample_count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source == nullptr || source->samples != sample_locations.sample_count condition ignored (continuing)\n"); }

	RenderDepthInfo draw_depth = *source_info;
	if (apply_clear)
	{
		GraphicsRenderDepthStencilCopyClearSource(submit_id, buffer, &draw_depth, sample_locations);
	}
	draw_depth.depth_clear_enable   = false;
	draw_depth.stencil_clear_enable = false;

	RenderColorInfo no_color;
	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&no_color, &draw_depth);
	if (framebuffer == nullptr || framebuffer->render_pass == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: framebuffer == nullptr || framebuffer->render_pass == nullptr condition ignored (continuing)\n"); }

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
	                                        guest_vertex_input, index_count, index_buffer, index_type, vertex_offset, vertex_records,
	                                        instance_count, first_instance);

	InvalidateMemoryObject(draw_depth);
}

void GraphicsRenderDepthStencilCopy(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg,
                                           HW::Shader* sh_ctx, uint32_t index_count, uint32_t index_type_and_size,
	                                       const void* index_addr, uint32_t instance_count, int32_t vertex_offset_add,
	                                       uint32_t first_instance)
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
	if (!render_control.depth_copy || !render_control.stencil_copy) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !render_control.depth_copy || !render_control.stencil_copy condition ignored (continuing)\n"); }
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
		if (GraphicsRenderDepthStencilCopyClearSource(submit_id, buffer, &source_setup, sample_locations))
		{
			InvalidateMemoryObject(source_setup);
		}
		return;
	}
	if (!static_rect_list && !guest_geometry)
	{
		KYTY_LOG_DEBUG( "KYTY_GRAPHICS: unsupported depth-stencil-copy primitive=%u count=%u\n", ucfg->GetPrimType(),
		             index_count);
		if (!static_rect_list && !guest_geometry) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !static_rect_list && !guest_geometry condition ignored (continuing)\n"); }
	}
	ShaderVertexInputInfo        guest_vertex_input {};
	ShaderId                     guest_vertex_id {};
	ShaderTranslationCacheResult guest_vertex_translation {};
	DepthStencilCopyVertexStage  guest_vertex_stage {};
	const DepthStencilCopyVertexStage* request_vertex_stage = nullptr;
	VulkanBuffer*                indices               = nullptr;
	VkIndexType                  index_type            = VK_INDEX_TYPE_UINT16;
	int32_t                      vertex_offset         = 0;
	uint32_t                     vertex_records        = 0;
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
				if (index_type_and_size != 0 && index_type_and_size != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: index_type_and_size != 0 && index_type_and_size != 1 condition ignored (continuing)\n"); }
		}
		const uint64_t index_addr_u64 = reinterpret_cast<uint64_t>(index_addr);
		indices = TryUploadTransientReadOnlyBuffer(buffer, index_addr_u64, index_size, true, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
		if (indices == nullptr)
		{
			indices = static_cast<VulkanBuffer*>(
			    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, index_addr_u64, index_size, IndexBufferGpuObject()));
		}
		if (indices == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: indices == nullptr condition ignored (continuing)\n"); }
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
		if (guest_vertex_translation.binary.IsEmpty()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: guest_vertex_translation.binary.IsEmpty() condition ignored (continuing)\n"); }

		if (ShaderBindRequiresDescriptorSet(guest_vertex_input.bind))
		{
			if (guest_vertex_input.bind.descriptor_set_slot != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: guest_vertex_input.bind.descriptor_set_slot != 0 condition ignored (continuing)\n"); }
			guest_vertex_stage.descriptor_set_layout =
			    g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(DescriptorCache::Stage::Vertex, guest_vertex_input.bind);
			if (guest_vertex_stage.descriptor_set_layout == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: guest_vertex_stage.descriptor_set_layout == nullptr condition ignored (continuing)\n"); }
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
		if (!ShaderResolveVertexOffset(indexed_draw ? ucfg->GetIndexOffset() : 0, guest_vertex_input, &vertex_offset,
		                               vertex_offset_add))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: depth/stencil copy vertex offset is outside int32 range; draw skipped\n");
			return;
		}
		const auto vertex_requirement = indexed_draw
		                                    ? ResolveIndexedVertexRecords(index_addr, index_count, index_type, vertex_offset)
		                                    : ResolveLinearVertexRecords(vertex_offset, index_count);
		if (!vertex_requirement.valid)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: depth/stencil copy vertex range is invalid; draw skipped\n");
			return;
		}
		vertex_records = vertex_requirement.records;
	}

	if (!color_expansion_enabled)
	{
		MaterializeRenderDepthInfo(submit_id, buffer, &depth_info, 0, 0, &sample_locations);
		GraphicsRenderDepthStencilCopyWriteDepthStencil(
		    submit_id, buffer, *ctx, &depth_info, sample_locations, true, effective_depth_write, guest_geometry, static_rect_list,
		    request_vertex_stage,
		    (guest_geometry ? &guest_vertex_input : nullptr), index_count, indices, index_type, vertex_offset, vertex_records,
		    instance_count, first_instance);
		return;
	}

	RenderColorInfo color_info;
	HW::Context     color_context = *ctx;
	color_context.SetRenderTargetMask(color_write_mask);
	DescribeRenderColorInfo(buffer, color_context, &color_info);
	ShaderPixelInputInfo copy_shader_usage {};
	const auto resolution = PrepareDisplayResolutionCohort(buffer, &color_info, depth_info, &copy_shader_usage);
	RequireSupportedRenderResolutionPlan(resolution);

	if (depth_info.format != VK_FORMAT_D32_SFLOAT_S8_UINT) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: depth_info.format != VK_FORMAT_D32_SFLOAT_S8_UINT condition ignored (continuing)\n"); }
	if (color_info.targets_num != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: color_info.targets_num != 1 condition ignored (continuing)\n"); }
	if (depth_info.samples != color_info.attachment[0].samples || sample_locations.sample_count != depth_info.samples) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: depth_info.samples != color_info.attachment[0].samples || sample_locations.sample_count != depth_info.samples condition ignored (continuing)\n"); }

	MaterializeRenderDepthInfo(submit_id, buffer, &depth_info,
	                           resolution.classification == ResolutionClassification::Scaled ? resolution.host_extent.width : 0,
	                           resolution.classification == ResolutionClassification::Scaled ? resolution.host_extent.height : 0,
	                           &sample_locations);
	MaterializeRenderColorInfo(submit_id, buffer, &color_info);
	CommitMaterializedRenderResolutionPlan(resolution, color_info, depth_info);

	auto* source = depth_info.vulkan_buffer;
	auto* target = color_info.attachment[0].vulkan_buffer;
	if (source == nullptr || target == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source == nullptr || target == nullptr condition ignored (continuing)\n"); }
	if (source->format != VK_FORMAT_D32_SFLOAT_S8_UINT) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source->format != VK_FORMAT_D32_SFLOAT_S8_UINT condition ignored (continuing)\n"); }
	const bool supported_target_format = target->format == VK_FORMAT_R8G8B8A8_UNORM || target->format == VK_FORMAT_B8G8R8A8_UNORM ||
	                                     target->format == VK_FORMAT_R8G8B8A8_SRGB || target->format == VK_FORMAT_B8G8R8A8_SRGB;
	if (!supported_target_format)
	{
		KYTY_LOG_DEBUG( "KYTY_GRAPHICS: unsupported depth-stencil-copy target format=%d render-format=%u width=%u height=%u\n",
		             static_cast<int>(target->format), static_cast<unsigned>(color_info.attachment[0].render_texture_format), target->extent.width,
		             target->extent.height);
		if (!supported_target_format) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !supported_target_format condition ignored (continuing)\n"); }
	}
	if (source->samples != target->samples || source->samples != sample_locations.sample_count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source->samples != target->samples || source->samples != sample_locations.sample_count condition ignored (continuing)\n"); }
	const auto source_guest = source->GetGuestExtent();
	const auto target_guest = target->GetGuestExtent();
	if (source_guest.width != target_guest.width || source_guest.height != target_guest.height) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source_guest.width != target_guest.width || source_guest.height != target_guest.height condition ignored (continuing)\n"); }
	if (source->memory.unique_id == target->memory.unique_id) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source->memory.unique_id == target->memory.unique_id condition ignored (continuing)\n"); }

	RenderDepthInfo source_setup = depth_info;
	// A deferred depth/stencil clear initializes the complete sampled source
	// plane independently of the geometry used for the color expansion.
	const bool source_modified = GraphicsRenderDepthStencilCopyClearSource(submit_id, buffer, &source_setup, sample_locations);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	GraphicsRenderDepthStencilBarrier(vk_buffer, source);
	if (source->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL condition ignored (continuing)\n"); }

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
		if (source->extent.width != target->extent.width || source->extent.height != target->extent.height) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source->extent.width != target->extent.width || source->extent.height != target->extent.height condition ignored (continuing)\n"); }
		copy_depth.depth_clear_enable   = false;
		copy_depth.stencil_clear_enable = false;
		copy_depth.depth_write_enable   = false;
		copy_depth.suppress_depth_write = true;
		depth_attachment                = &copy_depth;
	}
	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(
	    &color_info, depth_attachment, (depth_attachment == &copy_depth ? DepthStencilAttachmentAccess::ReadOnly
	                                                                       : DepthStencilAttachmentAccess::Writable));
	if (framebuffer == nullptr || framebuffer->render_pass == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: framebuffer == nullptr || framebuffer->render_pass == nullptr condition ignored (continuing)\n"); }

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
	                                        index_type, vertex_offset, vertex_records, instance_count, first_instance);

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
		    (guest_geometry ? &guest_vertex_input : nullptr), index_count, indices, index_type, vertex_offset, vertex_records,
		    instance_count, first_instance);
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
	                             uint32_t index_count, uint64_t draw_modifier, uint32_t instance_count)
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

	const auto render_lock_start = DrawStageClock::now();
	Core::LockGuard lock(g_render_ctx->GetMutex());
	DebugStatsRecordDrawRenderLockWait(DrawStageElapsedNs(render_lock_start));
	const auto state_setup_start = DrawStageClock::now();
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
			KYTY_LOG_DEBUG( "KYTY_AB_SKIP_TINY_DEPTH skip depth=%ux%u fmt=%u\n", probe.width, probe.height,
			             static_cast<uint32_t>(probe.format));
			return;
		}
	}
	if (const char* ab = std::getenv("KYTY_AB_SKIP_EMBEDDED_VS"); ab != nullptr && ab[0] != '\0')
	{
		const auto& vs = sh_ctx->GetVs();
		if (vs.vs_embedded || vs.vs_regs.data_addr == 0)
		{
			KYTY_LOG_DEBUG( "KYTY_AB_SKIP_EMBEDDED_VS skip vs_embedded=%d vs_addr=0x%012" PRIx64 "\n", vs.vs_embedded ? 1 : 0,
			             vs.vs_regs.data_addr);
			return;
		}
	}
	if (const char* ab_ps = std::getenv("KYTY_AB_SKIP_PS_ADDR"); ab_ps != nullptr && ab_ps[0] != '\0')
	{
		char*      end     = nullptr;
		const auto skip_ps = std::strtoull(ab_ps, &end, 0);
		if (end != ab_ps && skip_ps == sh_ctx->GetPs().ps_regs.data_addr)
		{
			KYTY_LOG_DEBUG( "KYTY_AB_SKIP_PS_ADDR skip ps=0x%012" PRIx64 "\n", sh_ctx->GetPs().ps_regs.data_addr);
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

		KYTY_LOG_DEBUG("GraphicsRenderDrawIndex():Parameters:\n");
		KYTY_LOG_DEBUG("\t index_count         = 0x%08" PRIx32 "\n", index_count);
		KYTY_LOG_DEBUG("\t draw_modifier       = 0x%016" PRIx64 "\n", draw_modifier);

		if (!AutoDrawModifierSupported(draw_modifier)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !AutoDrawModifierSupported(draw_modifier) condition ignored (continuing)\n"); }
		if (ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000 condition ignored (continuing)\n"); }

		GraphicsRenderDepthStencilCopy(submit_id, buffer, ctx, ucfg, sh_ctx, index_count, UINT32_MAX, nullptr, instance_count, 0, 0);
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

	KYTY_LOG_DEBUG("GraphicsRenderDrawIndex():Parameters:\n");
	KYTY_LOG_DEBUG("\t index_count         = 0x%08" PRIx32 "\n", index_count);
	KYTY_LOG_DEBUG("\t draw_modifier       = 0x%016" PRIx64 "\n", draw_modifier);

	if (!AutoDrawModifierSupported(draw_modifier)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !AutoDrawModifierSupported(draw_modifier) condition ignored (continuing)\n"); }
	if (ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000 condition ignored (continuing)\n"); }

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
	if (ShaderVertexClipProbeEligible(Config::IsNextGen(), vertex_shader_info.vs_embedded))
	{
		vs_input_info.clip_probe = ShaderResolveVertexClipProbeConfig(vertex_shader_info.gs_regs.chksum, false, index_count);
	}
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
			KYTY_LOG_DEBUG( "WARNING: skipping unsupported auto-draw primitive: type=%u count=%u vertex_buffers=%d\n",
			             ucfg->GetPrimType(), index_count, vs_input_info.buffers_num);
		}
		MaybeDumpAutoDrawSkip("unsupported-primitive", index_count, draw_modifier);
		return;
	}
	MaybeDumpPrimitiveDrawPlan("auto", ucfg->GetPrimType(), index_count, vs_input_info.buffers_num, false, primitive_plan);

	ShaderPixelInputInfo ps_input_info;
	const bool ps_required = State::PixelShaderStageRequired(ctx->GetRenderTargetMask(), shader_regs, ctx->GetDepthControl());
	ShaderGetInputInfoPS(&pixel_shader_info, &shader_regs, &vs_input_info, &ps_input_info, !ps_required);
	ps_input_info.fragment_tap = ShaderResolveFragmentTapConfig(pixel_shader_info.ps_regs.chksum, false, index_count);
	if (ShaderPixelInput0ProbeEligible(Config::IsNextGen(), pixel_shader_info.ps_embedded))
	{
		ps_input_info.input0_probe = ShaderResolvePixelProbeConfig(pixel_shader_info.ps_regs.chksum, false, index_count,
		                                                          ps_input_info.stage_enabled);
	}
	ValidatePixelProbeSelection(pixel_shader_info, shader_regs, &vs_input_info, &ps_input_info);
	const auto resolution = PrepareDisplayResolutionCohort(buffer, &color_info, depth_info, &ps_input_info);
	RequireSupportedRenderResolutionPlan(resolution);
	DebugStatsRecordDrawStateSetup(DrawStageElapsedNs(state_setup_start));
	const auto materialization_start = DrawStageClock::now();
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
	DebugStatsRecordDrawMaterialization(DrawStageElapsedNs(materialization_start));

	const auto pipeline_setup_start = DrawStageClock::now();
	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&color_info, &depth_info);

	if (framebuffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: framebuffer == nullptr condition ignored (continuing)\n"); }
	if (framebuffer->render_pass == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: framebuffer->render_pass == nullptr condition ignored (continuing)\n"); }

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	int32_t resolved_first_vertex = 0;
	if (!ShaderResolveVertexOffset(0, vs_input_info, &resolved_first_vertex))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: auto-draw vertex offset is outside int32 range; draw skipped\n");
		return;
	}
	const uint32_t actual_vertex_count = primitive_plan.chunked ? index_count : primitive_plan.draw_count;
	const auto     vertex_requirement  = ResolveLinearVertexRecords(resolved_first_vertex, actual_vertex_count);
	if (!vertex_requirement.valid)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: auto-draw vertex range is invalid; draw skipped\n");
		return;
	}
	const uint32_t vertex_records = vertex_requirement.records;

	bool clear_only = false;
	if (const char* ab = std::getenv("KYTY_AB_CLEAR_ONLY_AUTO_RT_ADDR"); ab != nullptr && ab[0] != '\0')
	{
		char*      end        = nullptr;
		const auto clear_addr = std::strtoull(ab, &end, 0);
		const auto* target    = RenderColorFirstConfiguredAttachment(color_info);
		clear_only            = end != ab && target != nullptr && clear_addr == target->base_addr;
	}
	if ((vs_input_info.clip_probe.enabled || ps_input_info.input0_probe.enabled) && clear_only)
	{
		// Preserve the existing clear-only render-pass path, but never let its
		// no-draw branch consume the process-wide selected probe reservation.
		vs_input_info.clip_probe                = {};
		vs_input_info.clip_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
		ps_input_info.input0_probe                = {};
		ps_input_info.input0_probe_descriptor_set = kVertexClipProbeInvalidDescriptorSet;
	}

	MaybeDumpAutoDrawReady(color_info, depth_info, *ctx, *sh_ctx, vs_input_info, ps_input_info, index_count, ucfg->GetPrimType());
	MaybeDumpUiDraw(color_info, vs_input_info, ps_input_info, *ctx, *ucfg, index_count, 0xffffffffu, false,
	                static_cast<uint32_t>(draw_modifier));

	auto* vertex_clip_probe = ReserveVertexClipProbe(buffer, &vs_input_info, &ps_input_info, vertex_shader_info.gs_regs.chksum,
	                                                pixel_shader_info.ps_regs.chksum, false, index_count, depth_info, color_info,
	                                                *framebuffer);
	if (vertex_clip_probe != nullptr && ps_input_info.input0_probe.kind == ShaderPixelProbeKind::FinalMrtResult)
	{
		TraceRenderTargetLifetimeProbeDepthAttempt(
		    submit_id, pixel_shader_info.ps_regs.chksum, vertex_shader_info.gs_regs.chksum, false, index_count,
		    ps_input_info.input0_probe.match_ordinal, ps_input_info.input0_probe.mrt_target,
		    ps_input_info.input0_probe.export_ordinal, depth_info, *framebuffer, *ctx, sample_locations);
	}
	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(framebuffer, &color_info, &depth_info, &vs_input_info, ctx, sh_ctx,
	                                                                  &ps_input_info, primitive_plan.topology, sample_locations);
	DebugStatsRecordDrawPipelineSetup(DrawStageElapsedNs(pipeline_setup_start));

	// EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);

	const auto resource_binding_start = DrawStageClock::now();
	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	const auto     vertex_buffer_binding_start = DrawStageClock::now();
	BindVertexBuffers(submit_id, buffer, vk_buffer, vs_input_info, vertex_records);
	DebugStatsRecordDrawVertexBufferBinding(DrawStageElapsedNs(vertex_buffer_binding_start));

	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, vs_input_info.bind,
	                VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);

	uint32_t declared_vertex_records = 0;
	for (int buffer_index = 0; buffer_index < vs_input_info.buffers_num; ++buffer_index)
	{
		declared_vertex_records = std::max(declared_vertex_records, vs_input_info.buffers[buffer_index].num_records);
	}
	const auto guest_vp_xy = State::ResolveViewportXy(ctx->GetScreenViewport().viewports[0].xscale,
	                                                  ctx->GetScreenViewport().viewports[0].xoffset,
	                                                  ctx->GetScreenViewport().viewports[0].yscale,
	                                                  ctx->GetScreenViewport().viewports[0].yoffset);
	const auto guest_vp_z = State::ResolveViewportDepth(
	    ctx->GetScreenViewport().viewports[0].zscale, ctx->GetScreenViewport().viewports[0].zoffset,
	    ctx->GetClipControl().dx_clip_space, g_render_ctx->GetGraphicCtx()->depth_range_unrestricted_supported,
	    ctx->GetScreenViewport().viewports[0].zmin, ctx->GetScreenViewport().viewports[0].zmax);
	const DrawMaterialTraceContext material_trace {.ps_checksum = sh_ctx->GetPs().ps_regs.chksum,
	                                               .ps_addr = sh_ctx->GetPs().ps_regs.data_addr,
	                                               .ps_in_control = ctx->GetShaderRegisters().ps_in_control,
	                                               .ps_required_subgroup = ps_input_info.required_subgroup_size,
	                                               .vs_checksum = sh_ctx->GetVs().gs_regs.chksum,
	                                               .vs_addr = sh_ctx->GetVs().vs_regs.data_addr != 0
	                                                              ? sh_ctx->GetVs().vs_regs.data_addr
	                                                              : sh_ctx->GetVs().es_regs.data_addr,
	                                               .vs_export_count = vs_input_info.export_count,
	                                               .vertex_float_mode = vs_input_info.gs_prolog
	                                                                        ? sh_ctx->GetVs().gs_regs.rsrc1.float_mode
	                                                                        : sh_ctx->GetVs().vs_regs.rsrc1.float_mode,
	                                               .vertex_dx10_clamp = vs_input_info.gs_prolog
	                                                                         ? sh_ctx->GetVs().gs_regs.rsrc1.dx10_clamp
	                                                                         : sh_ctx->GetVs().vs_regs.rsrc1.dx10_clamp,
	                                               .vertex_ieee_mode = vs_input_info.gs_prolog
	                                                                      ? sh_ctx->GetVs().gs_regs.rsrc1.ieee_mode
	                                                                      : sh_ctx->GetVs().vs_regs.rsrc1.ieee_mode,
	                                               .indexed = false,
	                                               .primitive_type = ucfg->GetPrimType(),
	                                               .guest_count = index_count,
	                                               .index_type = 0,
	                                               .index_addr = 0,
	                                               .index_size = 0,
	                                               .vertex_offset = resolved_first_vertex,
	                                               .instance_count = instance_count,
	                                               .first_instance = 0,
	                                               .required_vertex_records = vertex_records,
	                                               .vertex_input = &vs_input_info,
	                                               .color = &color_info,
	                                               .depth = &depth_info,
	                                               .hardware = ctx,
	                                               .framebuffer = framebuffer,
	                                               .declared_vertex_records = declared_vertex_records,
	                                               .target_mask = ctx->GetRenderTargetMask(),
	                                               .blend_enable = ctx->GetBlendControl(0).enable ? 1u : 0u,
	                                               .blend_src = ctx->GetBlendControl(0).color_srcblend,
	                                               .blend_op = ctx->GetBlendControl(0).color_comb_fcn,
	                                               .blend_dst = ctx->GetBlendControl(0).color_destblend,
	                                               .blend_bypass = ctx->GetRenderTarget(0).info.blend_bypass ? 1u : 0u,
	                                               .color_mask = State::ResolveColorWriteAgainstDepth(
	                                                   State::ResolveColorWriteMask(ctx->GetRenderTargetMask(),
	                                                                                ctx->GetShaderRegisters().m_cbShaderMask, 0),
	                                                   ctx->GetDepthControl().color_writes_on_depth_pass_disable,
	                                                   ctx->GetDepthControl().color_writes_on_depth_fail_enable),
	                                               .color_on_depth_fail =
	                                                   ctx->GetDepthControl().color_writes_on_depth_fail_enable ? 1u : 0u,
	                                               .color_off_depth_pass =
	                                                   ctx->GetDepthControl().color_writes_on_depth_pass_disable ? 1u : 0u,
	                                               .cull_front = ctx->GetModeControl().cull_front ? 1u : 0u,
	                                               .cull_back = ctx->GetModeControl().cull_back ? 1u : 0u,
	                                               .face = ctx->GetModeControl().face ? 1u : 0u,
	                                               .vp_x = guest_vp_xy.x,
	                                               .vp_y = guest_vp_xy.y,
	                                               .vp_width = guest_vp_xy.width,
	                                               .vp_height = guest_vp_xy.height,
	                                               .vp_zmin = guest_vp_z.min_depth,
	                                               .vp_zmax = guest_vp_z.max_depth,
	                                               .dx_clip = ctx->GetClipControl().dx_clip_space ? 1u : 0u,
	                                               .ps_input_num = ps_input_info.input_num,
	                                               .interpolators = {ps_input_info.interpolator_settings[0],
	                                                                 ps_input_info.interpolator_settings[1],
	                                                                 ps_input_info.interpolator_settings[2],
	                                                                 ps_input_info.interpolator_settings[3],
	                                                                 ps_input_info.interpolator_settings[4],
	                                                                 ps_input_info.interpolator_settings[5],
	                                                                 ps_input_info.interpolator_settings[6],
	                                                                 ps_input_info.interpolator_settings[7]}};
	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, ps_input_info.bind,
	                VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel, 0, &material_trace);
	TraceRenderTargetLifetimeDraw(submit_id, material_trace);
	DebugStatsRecordDrawResourceBinding(DrawStageElapsedNs(resource_binding_start));

	const auto command_emission_start = DrawStageClock::now();
	if (vertex_clip_probe != nullptr)
	{
		vertex_clip_probe->Arm(buffer, pipeline->pipeline_layout);
		vertex_clip_probe->CaptureAttachmentBeforePass(buffer);
	}
	buffer->BeginRenderPass(framebuffer, &color_info, &depth_info, &sample_locations);
	if (vertex_clip_probe != nullptr)
	{
		vertex_clip_probe->BeginDepthPassQuery(buffer);
	}
	const uint32_t first_vertex = static_cast<uint32_t>(resolved_first_vertex);

	if (!clear_only && primitive_plan.chunked)
	{
		for (uint32_t i = 0; i < index_count; i += primitive_plan.chunk_count)
		{
			vkCmdDraw(vk_buffer, primitive_plan.chunk_count, instance_count, first_vertex + i, 0);
			DebugStatsRecordDraw();
		}
	} else if (!clear_only)
	{
		vkCmdDraw(vk_buffer, primitive_plan.draw_count, instance_count, first_vertex, 0);
		DebugStatsRecordDraw();
	}
	if (vertex_clip_probe != nullptr)
	{
		vertex_clip_probe->EndDepthPassQuery(buffer);
	}

	buffer->EndRenderPass();
	if (vertex_clip_probe != nullptr)
	{
		vertex_clip_probe->Finish(buffer);
	}

	if (vk_buffer != nullptr)
	{
		VkMemoryBarrier memory_barrier {};
		memory_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		                     0, 1, &memory_barrier, 0, nullptr, 0, nullptr);
	}

	MaybeDumpColorTargets(g_render_ctx->GetGraphicCtx(), color_info);

	InvalidateMemoryObject(color_info);
	InvalidateMemoryObject(depth_info);
	DebugStatsRecordDrawCommandEmission(DrawStageElapsedNs(command_emission_start));
}

static bool TryPublishComputeDepthMetaFill(uint64_t submit_id, const ShaderComputeInputInfo& input, uint32_t thread_group_x,
                                           uint32_t thread_group_y, uint32_t thread_group_z)
{
	const auto& evidence = input.meta_fill;
	const auto& buffers  = input.bind.storage_buffers;
	if (!evidence.valid || buffers.buffers_num != 3 || input.bind.textures2D.textures_num != 0 ||
	    input.bind.samplers.samplers_num != 0 || input.bind.gds_pointers.pointers_num != 0 ||
	    evidence.workgroup_register != input.workgroup_register || evidence.workgroup_shift != 6u ||
	    input.threads_num[0] != 64u || input.threads_num[1] != 1u || input.threads_num[2] != 1u ||
	    !input.group_id[0] || input.group_id[1] || input.group_id[2] || input.thread_ids_num != 1 ||
	    thread_group_x == 0u || thread_group_y != 1u || thread_group_z != 1u)
	{
		return false;
	}

	const auto find_binding = [&](int start_register)
	{
		int match = -1;
		for (int i = 0; i < buffers.buffers_num; ++i)
		{
			if (buffers.start_register[i] != start_register)
			{
				continue;
			}
			if (match != -1)
			{
				return -1;
			}
			match = i;
		}
		return match;
	};
	const int source_index      = find_binding(evidence.source_start_register);
	const int destination_index = find_binding(evidence.destination_start_register);
	const int parameter_index   = find_binding(evidence.parameter_start_register);
	if (source_index < 0 || destination_index < 0 || parameter_index < 0 || source_index == destination_index ||
	    source_index == parameter_index || destination_index == parameter_index)
	{
		return false;
	}

	const auto& source      = buffers.buffers[source_index];
	const auto& destination = buffers.buffers[destination_index];
	const auto& parameters  = buffers.buffers[parameter_index];
	const auto binding_is_exact = [&](int index, ShaderStorageAccess access, bool read_only)
	{
		return buffers.accesses[index] == access && buffers.sources[index] == ShaderStorageBindingSource::MetadataSharp &&
		       buffers.code_available[index] && buffers.exact_matches[index] && !buffers.unbased_matches[index] &&
		       !buffers.decoded_unknown[index] && !buffers.indirect_descriptor_use[index] &&
		       ShaderStorageUsageIsReadOnly(buffers.usages[index]) == read_only;
	};
	if (!binding_is_exact(source_index, ShaderStorageAccess::Typed, true) ||
	    !binding_is_exact(destination_index, ShaderStorageAccess::Typed, false) ||
	    !binding_is_exact(parameter_index, ShaderStorageAccess::Raw, true) || source.Base48() == 0u ||
	    destination.Base48() == 0u || parameters.Base48() == 0u || source.Stride() != sizeof(uint32_t) ||
	    source.NumRecords() != 1u || destination.Stride() != sizeof(uint32_t) || destination.NumRecords() == 0u ||
	    parameters.Stride() != 4u * sizeof(uint32_t) || parameters.NumRecords() != 1u ||
	    source.Format() != destination.Format() || source.DstSelXYZW() != destination.DstSelXYZW() ||
	    source.SwizzleEnabled() || destination.SwizzleEnabled() || source.IndexStride() != 0u ||
	    destination.IndexStride() != 0u || source.AddTid() || destination.AddTid())
	{
		return false;
	}

	const uint64_t invocation_count = static_cast<uint64_t>(thread_group_x) * input.threads_num[0];
	if (invocation_count != destination.NumRecords())
	{
		return false;
	}

	uint32_t source_word = 0;
	uint32_t parameter_words[4] {};
	if (!GpuMemoryCaptureSnapshotReadOnlyBuffer(source.Base48(), sizeof(source_word), &source_word) ||
	    !GpuMemoryCaptureSnapshotReadOnlyBuffer(parameters.Base48(), sizeof(parameter_words), parameter_words) ||
	    parameter_words[0] != destination.NumRecords() || parameter_words[1] != 0u)
	{
		return false;
	}

	const uint64_t destination_size = ShaderBufferByteSize(destination.Stride(), destination.NumRecords());
	GpuMemoryRangeProvenance provenance {};
	if (destination_size == 0u || !GpuMemoryQueryRangeProvenance(destination.Base48(), destination_size, &provenance) ||
	    provenance.truncated)
	{
		return false;
	}
	const GpuMemoryRangeProvenanceEntry* storage = nullptr;
	for (uint32_t i = 0; i < provenance.entry_count; ++i)
	{
		const auto& entry = provenance.entries[i];
		if (entry.type != GpuMemoryObjectType::StorageBuffer || entry.relation != GpuMemoryOverlapType::Equals || entry.read_only)
		{
			continue;
		}
		if (storage != nullptr)
		{
			return false;
		}
		storage = &entry;
	}
	if (storage == nullptr || !storage->in_use || !storage->write_back_capable || storage->logical_generation == 0u ||
	    storage->backing_generation == 0u || storage->submit_id > submit_id)
	{
		return false;
	}

	DepthMetaStorageIdentity identity {};
	identity.address                     = destination.Base48();
	identity.size                        = destination_size;
	identity.logical_generation          = storage->logical_generation;
	identity.backing_generation          = storage->backing_generation;
	identity.producer_or_consumer_submit = submit_id;
	return DepthMetaPublishComputeFill(identity, source_word);
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

	if ((mode & ~DISPATCH_KNOWN_BITS) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (mode & ~DISPATCH_KNOWN_BITS) != 0 condition ignored (continuing)\n"); }

	const auto& cs_regs = sh_ctx->GetCs();
	const auto& sh_regs = ctx->GetShaderRegisters();

	if ((mode & DISPATCH_USE_THREAD_DIMENSIONS) != 0)
	{
		const uint32_t lx = cs_regs.cs_regs.num_thread_x;
		const uint32_t ly = cs_regs.cs_regs.num_thread_y;
		const uint32_t lz = cs_regs.cs_regs.num_thread_z;
		if (lx == 0 || ly == 0 || lz == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: lx == 0 || ly == 0 || lz == 0 condition ignored (continuing)\n"); }
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
		KYTY_LOG_DEBUG( "KYTY_AB_SKIP_ALL_CS skip shader=0x%012" PRIx64 "\n", cs_regs.cs_regs.data_addr);
		return;
	}
	if (const char* ab_skip = std::getenv("KYTY_AB_SKIP_TEX_CS");
	    ab_skip != nullptr && ab_skip[0] != '\0' && input_info.bind.textures2D.textures_num > 0)
	{
		KYTY_LOG_DEBUG( "KYTY_AB_SKIP_TEX_CS skip shader=0x%012" PRIx64 " textures=%d\n", cs_regs.cs_regs.data_addr,
		             input_info.bind.textures2D.textures_num);
		return;
	}
	if (const char* ab_addr = std::getenv("KYTY_AB_SKIP_CS_ADDR"); ab_addr != nullptr && ab_addr[0] != '\0')
	{
		char*      end      = nullptr;
		const auto skip_addr = std::strtoull(ab_addr, &end, 0);
		if (end != ab_addr && skip_addr == cs_regs.cs_regs.data_addr)
		{
			KYTY_LOG_DEBUG( "KYTY_AB_SKIP_CS_ADDR skip shader=0x%012" PRIx64 "\n", cs_regs.cs_regs.data_addr);
			return;
		}
	}
	static const char* dump_dispatch = std::getenv("KYTY_DUMP_DISPATCH");
	const char*        dump_cs_id    = std::getenv("KYTY_DUMP_CS_ID");
	const char*        dump_texture  = std::getenv("KYTY_DUMP_CS_TEXTURE");
	bool               selected_cs   = true;
	bool               selector_used = false;
	if (dump_cs_id != nullptr && dump_cs_id[0] != '\0')
	{
		selector_used       = true;
		char*      end      = nullptr;
		const auto selected = std::strtoull(dump_cs_id, &end, 16);
		selected_cs         = end != dump_cs_id && *end == '\0' && selected == cs_regs.cs_regs.chksum;
	}
	if (dump_texture != nullptr && dump_texture[0] != '\0')
	{
		selector_used = true;
		selected_cs   = selected_cs && DispatchTextureExtentMatches(input_info, dump_texture);
	}
	selected_cs = selector_used && selected_cs;
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
		KYTY_LOG_WARN(
		             "KYTY_DUMP_DISPATCH frame=%d shader=0x%012" PRIx64 " groups=%ux%ux%u local=%ux%ux%u mode=0x%x "
		             "storage=%d textures=%d direct=%d\n",
		             GraphicsRunGetFrameNum(), cs_regs.cs_regs.data_addr, thread_group_x, thread_group_y, thread_group_z,
		             input_info.threads_num[0], input_info.threads_num[1], input_info.threads_num[2], mode,
		             input_info.bind.storage_buffers.buffers_num, input_info.bind.textures2D.textures_num,
		             input_info.bind.direct_sgprs.sgprs_num);
		for (int i = 0; i < input_info.bind.storage_buffers.buffers_num; ++i)
		{
			const auto& resource = input_info.bind.storage_buffers.buffers[i];
			KYTY_LOG_WARN(
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
				KYTY_LOG_WARN("    words=");
				for (uint32_t word = 0; word < resource.NumRecords() * 4u; ++word)
				{
					KYTY_LOG_WARN("%s%08x", word == 0u ? "" : ",", words[word]);
				}
				KYTY_LOG_WARN("\n");
			}
		}
		for (int i = 0; i < input_info.bind.textures2D.textures_num; ++i)
		{
			const auto& texture = input_info.bind.textures2D.desc[i].texture;
			KYTY_LOG_WARN(
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
			KYTY_LOG_WARN("  direct[%d] reg=%d value=0x%08x\n", i, input_info.bind.direct_sgprs.start_register[i],
			             input_info.bind.direct_sgprs.sgprs[i].field);
		}
	}

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(&input_info, &sh_ctx->GetCs(), &ctx->GetShaderRegisters());

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	const uint32_t storage_seed_skip_mask = ResolveStorageSeedSkipMask(input_info, thread_group_x, thread_group_y, thread_group_z);
	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline_layout, input_info.bind,
	                VK_SHADER_STAGE_COMPUTE_BIT, DescriptorCache::Stage::Compute, storage_seed_skip_mask, nullptr);
	(void)TryPublishComputeDepthMetaFill(submit_id, input_info, thread_group_x, thread_group_y, thread_group_z);

	vkCmdDispatch(vk_buffer, thread_group_x, thread_group_y, thread_group_z);
	DebugStatsRecordDispatch();
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
