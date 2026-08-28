#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/RenderResolutionCoordinator.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/RenderTexture.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/RenderResolutionAlias.h"
#include "Emulator/Graphics/RenderResolutionImageCapability.h"
#include "Emulator/Graphics/RenderResolutionPlanner.h"
#include "Emulator/Graphics/RenderResolutionPolicy.h"
#include "Emulator/Graphics/SampleLocations.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/RenderResolutionShaderScale.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// barriers, describe/materialize color/depth, resolution cohort

static bool ResolveDepthMetaStorageIdentity(uint64_t submit_id, const RenderDepthInfo& depth,
                                            DepthMetaStorageIdentity* identity)
{
	if (identity == nullptr || depth.htile_buffer_vaddr == 0u || depth.htile_buffer_size == 0u)
	{
		return false;
	}
	*identity = {};
	GpuMemoryRangeProvenance provenance {};
	if (!GpuMemoryQueryRangeProvenance(depth.htile_buffer_vaddr, depth.htile_buffer_size, &provenance) ||
	    provenance.truncated)
	{
		return false;
	}

	const GpuMemoryRangeProvenanceEntry* selected = nullptr;
	for (uint32_t i = 0; i < provenance.entry_count; ++i)
	{
		const auto& entry = provenance.entries[i];
		if (entry.type != GpuMemoryObjectType::StorageBuffer || entry.relation != GpuMemoryOverlapType::Equals ||
		    entry.read_only)
		{
			continue;
		}
		if (selected != nullptr)
		{
			return false;
		}
		selected = &entry;
	}
	if (selected == nullptr)
	{
		return false;
	}

	identity->address                     = depth.htile_buffer_vaddr;
	identity->size                        = depth.htile_buffer_size;
	identity->logical_generation          = selected->logical_generation;
	identity->backing_generation          = selected->backing_generation;
	identity->producer_or_consumer_submit = submit_id;
	return true;
}

static bool ConsumeDepthMetaClear(uint64_t submit_id, const RenderDepthInfo& depth, DepthMetaClearEvent* event)
{
	DepthMetaTraceSnapshot snapshot {};
	if (!DepthMetaQueryTraceState(depth.htile_buffer_vaddr, &snapshot) || !snapshot.pending)
	{
		return false;
	}
	if (snapshot.pending_event.source != DepthMetaClearSource::ComputeMetadataFill)
	{
		return DepthMetaConsumeClear(depth.htile_buffer_vaddr, event);
	}

	DepthMetaStorageIdentity identity {};
	if (!ResolveDepthMetaStorageIdentity(submit_id, depth, &identity))
	{
		return false;
	}
	// Clearing either aspect of a combined D32S8 image starts the render pass
	// from UNDEFINED. Only translate the captured zero-fill family when the
	// guest also initializes stencil in the same first use; otherwise Vulkan
	// could discard a stencil plane whose metadata semantics are not modeled.
	if (depth.format != VK_FORMAT_D32_SFLOAT_S8_UINT || depth.samples != VK_SAMPLE_COUNT_1_BIT ||
	    snapshot.pending_event.pattern.kind != DepthMetaPatternKind::UniformZero ||
	    snapshot.pending_event.pattern.first_word != 0u || depth.depth_clear_value != 0.0f ||
	    std::signbit(depth.depth_clear_value) || !depth.stencil_clear_enable || depth.stencil_clear_value != 0u)
	{
		(void)DepthMetaDiscardComputeFill(identity, snapshot.pending_event.sequence);
		return false;
	}

	if (!DepthMetaConsumeClear(identity, event))
	{
		return false;
	}
	return event != nullptr;
}

void GraphicsRenderMemoryBarrier(CommandBuffer* buffer)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	VkMemoryBarrier mem_barrier {};
	mem_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mem_barrier.pNext         = nullptr;
	mem_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	mem_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mem_barrier, 0, nullptr,
	                     0, nullptr);
}

void GraphicsRenderRenderTextureBarrier(VkCommandBuffer vk_buffer, VulkanImage* image)
{
	EXIT_IF(image == nullptr);

	// Sample bind may alias a live color RT or a storage image written by
	// compute; both need SHADER_READ_ONLY before the draw samples them.
	if (image->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || image->layout == VK_IMAGE_LAYOUT_GENERAL ||
	    image->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || image->layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext = nullptr;
		image_memory_barrier.srcAccessMask =
		    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
		image_memory_barrier.oldLayout                       = image->layout;
		image_memory_barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = image->image;
		image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

		vkCmdPipelineBarrier(vk_buffer,
		                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
		                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
		                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}
}

void GraphicsRenderDepthStencilBarrier(VkCommandBuffer vk_buffer, VulkanImage* image)
{
	EXIT_IF(image == nullptr);

	if (image->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		auto* depth_image = static_cast<DepthStencilVulkanImage*>(image);
		const bool custom_sample_locations = depth_image->sample_locations_compatible;
		VkSampleLocationEXT sample_location_values[kVulkanSampleLocationMaxCount] = {};
		VkSampleLocationsInfoEXT sample_location_info {};
		if (custom_sample_locations)
		{
			if (!VulkanSampleLocationsEnabled(depth_image->last_sample_locations)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanSampleLocationsEnabled(depth_image->last_sample_locations) condition ignored (continuing)\n"); }
			if (
			    !VulkanSampleLocationsPopulateInfo(depth_image->last_sample_locations, sample_location_values, &sample_location_info)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanSampleLocationsPopulateInfo(depth_image->last_sample_locations, sample_lo condition ignored (continuing)\n"); }
		}

		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext                           = (custom_sample_locations ? &sample_location_info : nullptr);
		image_memory_barrier.srcAccessMask                   = VK_ACCESS_MEMORY_WRITE_BIT;
		image_memory_barrier.dstAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.oldLayout                       = image->layout;
		image_memory_barrier.newLayout                       = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = image->image;
		image_memory_barrier.subresourceRange.aspectMask     = DepthFormatAspectMask(image->format);
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

		vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}
}

void GraphicsRenderStorageImageBarrier(VkCommandBuffer vk_buffer, VulkanImage* image)
{
	EXIT_IF(image == nullptr);

	VkPipelineStageFlags src_stage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkAccessFlags        src_access = 0;
	switch (image->layout)
	{
		case VK_IMAGE_LAYOUT_UNDEFINED: break;
		case VK_IMAGE_LAYOUT_PREINITIALIZED:
			src_stage  = VK_PIPELINE_STAGE_HOST_BIT;
			src_access = VK_ACCESS_HOST_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			src_stage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			src_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			src_stage  = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
			             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			src_access = VK_ACCESS_SHADER_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			src_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
			src_access = VK_ACCESS_TRANSFER_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			src_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
			src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_GENERAL:
			src_stage  = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
			             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			src_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			break;
		default:
			src_stage  = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			src_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			break;
	}

	VkImageMemoryBarrier image_memory_barrier {};
	image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	image_memory_barrier.pNext                           = nullptr;
	image_memory_barrier.srcAccessMask                   = src_access;
	image_memory_barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	image_memory_barrier.oldLayout                       = image->layout;
	image_memory_barrier.newLayout                       = VK_IMAGE_LAYOUT_GENERAL;
	image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.image                           = image->image;
	image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	image_memory_barrier.subresourceRange.baseMipLevel   = 0;
	image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	image_memory_barrier.subresourceRange.baseArrayLayer = 0;
	image_memory_barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

	const VkPipelineStageFlags dst_stage =
	    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	vkCmdPipelineBarrier(vk_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &image_memory_barrier);
	image->layout = image_memory_barrier.newLayout;
}

void GraphicsRenderRenderTextureBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	auto images = FindRenderTexture(buffer, vaddr, size, false);

	for (auto* image: images)
	{
		GraphicsRenderRenderTextureBarrier(vk_buffer, image);
	}
}

void GraphicsRenderDepthStencilBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	auto images = FindDepthStencil(buffer, vaddr, size, false);

	for (auto* image: images)
	{
		GraphicsRenderDepthStencilBarrier(vk_buffer, image);
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void DescribeRenderDepthInfo(const HW::Context& hw, RenderDepthInfo* r)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(r == nullptr);

	const auto  z  = State::ResolveDepthStencilBasePairs(hw.GetDepthRenderTarget());
	const auto& rc = hw.GetRenderControl();
	const auto& dc = hw.GetDepthControl();
	const auto& sc = hw.GetStencilControl();
	const auto& sm = hw.GetStencilMask();
	const auto& cc = hw.GetColorControl();

	TileSizeAlign stencil_size {};
	TileSizeAlign htile_size {};
	TileSizeAlign depth_size {};

	bool           neo               = Config::IsNeo();
	bool           ps5               = Config::IsNextGen();
	bool           htile             = z.z_info.tile_surface_enable;
	bool           decompress        = htile && (rc.depth_compress_disable || rc.stencil_compress_disable);
	const auto     usage             = State::ResolveDepthStencilUsage(z, rc, dc);
	const uint32_t effective_stencil = State::ResolveEffectiveStencilFormat(z);
	const auto     depth_extent      = State::ResolveDepthTargetExtent(z, ps5);

	// DB_DEPTH_SIZE_XY and z_enable may remain programmed after planes are
	// unbound. A zero size encoding is the legal 1x1 value; do not invent an
	// attachment from enables alone when no plane base is bound. Also reject
	// the degenerate 1x1 extent that comes from an unset SIZE_XY paired with a
	// still-programmed format (collapses full-screen color FBs on host GPUs).
	const bool depth_plane_bound =
	    z.z_read_base_addr != 0 || z.z_write_base_addr != 0 || z.stencil_read_base_addr != 0 || z.stencil_write_base_addr != 0 ||
	    z.htile_data_base_addr != 0;
	const bool depth_format_declared = z.z_info.format != 0 || effective_stencil != 0;
	const bool depth_extent_usable =
	    depth_extent.valid && depth_extent.width > 1u && depth_extent.height > 1u;

	if (usage.target_active && depth_plane_bound && depth_format_declared && (!ps5 || depth_extent_usable))
	{
		switch (z.z_info.format * 2 + effective_stencil)
		{
			case 0: r->format = VK_FORMAT_UNDEFINED; break;
			case 1:
				KYTY_LOG_DEBUG("WARNING: VK_FORMAT_S8_UINT not supported, using D32_SFLOAT\n");
				r->format = VK_FORMAT_D32_SFLOAT;
				break;
			case 2: r->format = VK_FORMAT_D16_UNORM; break;
			case 3: r->format = VK_FORMAT_D24_UNORM_S8_UINT; break;
			case 6: r->format = VK_FORMAT_D32_SFLOAT; break;
			case 7: r->format = VK_FORMAT_D32_SFLOAT_S8_UINT; break;
			default:
				KYTY_LOG_DEBUG("WARNING: unknown z/stencil format, using D32_SFLOAT\n");
				r->format = VK_FORMAT_D32_SFLOAT;
				break;
		}
	}

	if (r->format != VK_FORMAT_UNDEFINED)
	{
		r->samples = decode_guest_sample_count(z.z_info.num_samples);
		if (ps5)
		{
			if (!depth_extent.valid) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !depth_extent.valid condition ignored (continuing)\n"); }
			bool size_found = TileGetDepthSize(depth_extent.width, depth_extent.height, 0, z.z_info.format, effective_stencil, htile, true, true,
			                                   &stencil_size, &htile_size, &depth_size);
			if (!size_found) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !size_found condition ignored (continuing)\n"); }
		} else
		{
			uint32_t size  = 0;
			uint32_t pitch = (z.pitch_div8_minus1 + 1) * 8;

			if (z.z_info.format == 3)
			{
				size = (z.slice_div64_minus1 + 1) * 64 * 4;
			} else if (z.z_info.format == 1)
			{
				size = (z.slice_div64_minus1 + 1) * 64 * 2;
			}

			if (!TileGetDepthSize(z.width, z.height, pitch, z.z_info.format, effective_stencil, htile, neo, false, &stencil_size,
			                      &htile_size, &depth_size))
			{
				depth_size.size  = size;
				depth_size.align = neo ? 65536 : 32768;
			} else
			{
				if (depth_size.size != size) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: depth_size.size != size condition ignored (continuing)\n"); }
			}
		}
	}

	auto stencil_addr_mask = static_cast<uint64_t>(stencil_size.align) - 1;
	auto depth_addr_mask   = static_cast<uint64_t>(depth_size.align) - 1;
	auto htile_addr_mask   = static_cast<uint64_t>(htile_size.align) - 1;

	r->htile                = htile;
	r->neo                  = neo;
	r->depth_buffer_size    = depth_size.size;
	r->depth_buffer_vaddr   = z.z_read_base_addr & ~depth_addr_mask;
	r->depth_tile_swizzle   = z.z_read_base_addr & depth_addr_mask;
	r->stencil_buffer_size  = stencil_size.size;
	r->stencil_buffer_vaddr = z.stencil_read_base_addr & ~stencil_addr_mask;
	r->stencil_tile_swizzle = z.stencil_read_base_addr & stencil_addr_mask;
	r->htile_buffer_size    = htile_size.size;
	r->htile_buffer_vaddr   = z.htile_data_base_addr & ~htile_addr_mask;
	r->htile_tile_swizzle   = z.htile_data_base_addr & htile_addr_mask;
	r->width                = (ps5 ? depth_extent.width : z.width);
	r->height               = (ps5 ? depth_extent.height : z.height);
	r->depth_clear_enable   = rc.depth_clear_enable;
	r->suppress_depth_write = rc.depth_clear_enable;
	r->depth_clear_value    = hw.GetDepthClearValue();
	r->depth_test_enable    = dc.z_enable;
	r->depth_write_enable   = usage.depth_write_enable;
	r->depth_compare_op     = static_cast<VkCompareOp>(dc.zfunc);

	r->depth_bounds_test_enable = dc.depth_bounds_enable;
	r->depth_min_bounds         = 0.0f;
	r->depth_max_bounds         = 0.0f;
	if (r->depth_bounds_test_enable) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r->depth_bounds_test_enable condition ignored (continuing)\n"); }

	r->stencil_clear_enable = rc.stencil_clear_enable;
	r->stencil_clear_value  = hw.GetStencilClearValue();
	// EXIT_NOT_IMPLEMENTED(r->stencil_clear_enable);

	r->stencil_test_enable = dc.stencil_enable;

	// No attachment ⇒ no depth/stencil side effects, even if guest left enables set.
	if (r->format == VK_FORMAT_UNDEFINED)
	{
		r->depth_test_enable    = false;
		r->depth_write_enable   = false;
		r->depth_clear_enable   = false;
		r->suppress_depth_write = false;
		r->stencil_test_enable  = false;
		r->stencil_clear_enable = false;
		r->width                = 0;
		r->height               = 0;
	}
	if (r->stencil_test_enable)
	{
		get_stencil_state(&r->stencil_static_front, &r->stencil_dynamic_front, dc.stencilfunc, sc.stencil_fail, sc.stencil_zpass,
		                  sc.stencil_zfail, sm.stencil_testval, sm.stencil_mask, sm.stencil_writemask, sm.stencil_opval);
		if (dc.backface_enable)
		{
			get_stencil_state(&r->stencil_static_back, &r->stencil_dynamic_back, dc.stencilfunc_bf, sc.stencil_fail_bf, sc.stencil_zpass_bf,
			                  sc.stencil_zfail_bf, sm.stencil_testval_bf, sm.stencil_mask_bf, sm.stencil_writemask_bf, sm.stencil_opval_bf);
		} else
		{
			r->stencil_static_back  = r->stencil_static_front;
			r->stencil_dynamic_back = r->stencil_dynamic_front;
		}
	} else
	{
		r->stencil_static_front  = {};
		r->stencil_static_back   = {};
		r->stencil_dynamic_front = {};
		r->stencil_dynamic_back  = {};
	}
	// EXIT_NOT_IMPLEMENTED(r->stencil_test_enable);

	if (r->format != VK_FORMAT_UNDEFINED)
	{
		// Depth/stencil objects keep a sampled representation from their first
		// materialization. Later fixed-function expansion and shader sampling
		// must observe the same image allocation and its accumulated contents.
		r->sampled = true;

		if (z.z_info.tile_mode_index != 0 && r->depth_tile_swizzle != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: z.z_info.tile_mode_index != 0 && r->depth_tile_swizzle != 0 condition ignored (continuing)\n"); }
		if (r->stencil_tile_swizzle != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r->stencil_tile_swizzle != 0 condition ignored (continuing)\n"); }
		if (r->htile_tile_swizzle != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r->htile_tile_swizzle != 0 condition ignored (continuing)\n"); }

		r->vaddr_num = 0;

		if (r->depth_buffer_size > 0)
		{
			r->vaddr[r->vaddr_num] = r->depth_buffer_vaddr;
			r->size[r->vaddr_num]  = r->depth_buffer_size;
			r->vaddr_num++;
		}

		if (r->stencil_buffer_size > 0)
		{
			r->vaddr[r->vaddr_num] = r->stencil_buffer_vaddr;
			r->size[r->vaddr_num]  = r->stencil_buffer_size;
			r->vaddr_num++;
		}

		if (r->htile_buffer_size > 0)
		{
			r->vaddr[r->vaddr_num] = r->htile_buffer_vaddr;
			r->size[r->vaddr_num]  = r->htile_buffer_size;
			r->vaddr_num++;
		}

		if (r->vaddr_num == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r->vaddr_num == 0 condition ignored (continuing)\n"); }
		r->update_compression_state = ((cc.mode == 0 && cc.op == 0xCC) || (dc.z_enable || dc.z_write_enable));
		r->compressed_after_draw    = htile && !decompress;
	}
}

void MaterializeRenderDepthInfo(uint64_t submit_id, CommandBuffer* buffer, RenderDepthInfo* r, uint32_t host_width,
                                       uint32_t host_height,
                                       const VulkanSampleLocationState* sample_locations)
{
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(r == nullptr);

	if (r->format == VK_FORMAT_UNDEFINED)
	{
		return;
	}
	host_width  = host_width == 0 ? r->width : host_width;
	host_height = host_height == 0 ? r->height : host_height;
	const bool sample_locations_compatible =
	    (sample_locations != nullptr && VulkanSampleLocationsEnabled(*sample_locations));
	DepthStencilBufferObject vulkan_buffer_info(r->format, r->width, r->height, host_width, host_height, r->htile, r->neo, r->sampled,
	                                            r->htile_buffer_vaddr, r->htile_buffer_size, static_cast<uint32_t>(r->samples),
	                                            sample_locations_compatible);
	r->vulkan_buffer = static_cast<DepthStencilVulkanImage*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, r->vaddr, r->size, r->vaddr_num, vulkan_buffer_info));
	if (r->vulkan_buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r->vulkan_buffer == nullptr condition ignored (continuing)\n"); }
	if (sample_locations_compatible && !r->vulkan_buffer->sample_locations_compatible)
	{
		KYTY_LOG_DEBUG( "KYTY_GRAPHICS: depth image was created without custom sample-location compatibility\n");
		if (!r->vulkan_buffer->sample_locations_compatible) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !r->vulkan_buffer->sample_locations_compatible condition ignored (continuing)\n"); }
	}

	DepthMetaClearEvent meta_clear_event {};
	if (r->htile && ConsumeDepthMetaClear(submit_id, *r, &meta_clear_event))
	{
		const auto clear_actions = State::ResolveDepthClearActions(r->depth_clear_enable, true);
		r->depth_clear_enable    = clear_actions.vulkan_clear;
		r->suppress_depth_write  = clear_actions.suppress_depth_write;
	}
	if (r->update_compression_state)
	{
		r->vulkan_buffer->compressed = r->compressed_after_draw;
	}
}

DepthStencilAttachmentAccess ResolveDepthStencilAttachmentAccess(const RenderDepthInfo& depth, bool sampled_in_same_draw,
                                                                 bool load_store_op_none_supported)
{
	if (depth.format == VK_FORMAT_UNDEFINED || !sampled_in_same_draw)
	{
		return DepthStencilAttachmentAccess::Writable;
	}

	const bool depth_writes = depth.depth_write_enable && !depth.suppress_depth_write;
	const auto stencil_face_writes = [](const PipelineStencilStaticState& face, const PipelineStencilDynamicState& dynamic)
	{
		return dynamic.writeMask != 0u &&
		       (face.failOp != VK_STENCIL_OP_KEEP || face.passOp != VK_STENCIL_OP_KEEP || face.depthFailOp != VK_STENCIL_OP_KEEP);
	};
	const bool stencil_writes = depth.stencil_test_enable &&
	                            (stencil_face_writes(depth.stencil_static_front, depth.stencil_dynamic_front) ||
	                             stencil_face_writes(depth.stencil_static_back, depth.stencil_dynamic_back));

	if (depth.depth_clear_enable || depth.stencil_clear_enable || depth_writes || stencil_writes || !load_store_op_none_supported)
	{
		return DepthStencilAttachmentAccess::Unsupported;
	}
	return DepthStencilAttachmentAccess::ReadOnly;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static bool DescribeRenderColorSlotInfo(CommandBuffer* buffer, const HW::RenderTarget& rt, RenderColorInfo* r)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer == nullptr || r == nullptr);

	*r = {};

	if (rt.base.addr == 0)
	{
		return false;
	}
	r->targets_num = 1;
	auto& attachment = r->attachment[0];
	attachment.samples = decode_guest_sample_count(rt.attrib.num_samples);
	const auto render_format = ResolveRenderTextureFormat(rt.info.format, rt.info.channel_type, rt.info.channel_order);
	if (render_format.format == RenderTextureFormat::Unknown || render_format.bytes_per_element == 0)
	{
		EXIT("unsupported render-target format: format=0x%" PRIx32 " type=0x%" PRIx32 " order=0x%" PRIx32
		     " samples=%u\n",
		     rt.info.format, rt.info.channel_type, rt.info.channel_order, static_cast<unsigned>(attachment.samples));
	}

	bool ps5 = Config::IsNextGen();

	if (ps5)
	{
		switch (rt.attrib3.tile_mode)
		{
			case 0x1b:
				attachment.tile       = true;
				attachment.write_back = false;
				break;
			default: EXIT("unsupported render-target tile mode: %u\n", rt.attrib3.tile_mode);
		}

		attachment.width  = rt.attrib2.width + 1;
		attachment.height = rt.attrib2.height + 1;

		const uint32_t rt_bpp = render_format.bytes_per_element;

		// Element pitch: hardware PITCH when programmed, otherwise align width
		// to the 64 KiB block width for this BPE (same rule as sample tile 27).
		if (rt.pitch.pitch_div8_minus1 != 0)
		{
			attachment.pitch = (rt.pitch.pitch_div8_minus1 + 1u) << 3u;
		} else
		{
			attachment.pitch = TileAlign64KBPitch(attachment.width, rt_bpp);
			if (attachment.pitch == 0u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: attachment.pitch == 0u condition ignored (continuing)\n"); }
		}

		Graphics::TileSizeAlign size32 {};
		Graphics::TileGetRenderTargetSize(attachment.width, attachment.height, attachment.pitch, rt.attrib3.tile_mode, rt_bpp, &size32);

		RenderTextureArrayView array_view {};
		if (!ResolveRenderTextureArrayView(size32.size, rt.view.base_array_slice_index, rt.view.last_array_slice_index, &array_view))
		{
			EXIT("invalid layered render-target layout: width=%" PRIu32 " height=%" PRIu32 " pitch=%" PRIu32
			     " tile=%u layer=%" PRIu32 "..%" PRIu32 "\n",
			     attachment.width, attachment.height, attachment.pitch, rt.attrib3.tile_mode, rt.view.base_array_slice_index,
			     rt.view.last_array_slice_index);
		}
		attachment.size             = array_view.full_backing_size;
		attachment.image_layers     = array_view.image_layers;
		attachment.base_array_layer = array_view.base_layer;
		attachment.layer_count      = array_view.layer_count;
		if (attachment.image_layers > 1u &&
		    GpuMemoryValidateAllocatedRange(rt.base.addr, attachment.size) != GpuMemoryRangeValidationStatus::Valid)
		{
			EXIT("layered render-target backing is not fully allocated: size=%" PRIu64 " layers=%" PRIu32 "\n", attachment.size,
			     attachment.image_layers);
		}
	} else
	{
		if (attachment.image_layers != 1u || attachment.base_array_layer != 0u || attachment.layer_count != 1u)
		{
			EXIT("display buffer cannot use an array-slice view\n");
		}
		switch (rt.attrib.tile_mode)
		{
			case 0x8:
				attachment.tile       = false;
				attachment.write_back = false;
				break;

			case 0x1f:
				attachment.tile       = false;
				attachment.write_back = true;
				break;

			case 0xa:
			case 0xd:
			case 0xe:
				attachment.tile       = true;
				attachment.write_back = false;
				break;

			default: EXIT("unsupported render-target tile mode: %u\n", rt.attrib.tile_mode);
		}

		attachment.width  = rt.size.width;
		attachment.height = rt.size.height;
		attachment.pitch  = (rt.pitch.pitch_div8_minus1 + 1) * 8;
		attachment.size   = (rt.slice.slice_div64_minus1 + 1) * 64 * 4;
	}
	attachment.neo = rt.info.neo_mode;

	auto video_image       = VideoOut::VideoOutGetImageMetadataForSubmission(rt.base.addr, buffer);
	bool render_to_texture = (video_image.image == nullptr);

	if (render_to_texture)
	{
		attachment.render_texture_format = render_format.format;
		attachment.type                  = RenderColorType::RenderTexture;
		attachment.base_addr             = rt.base.addr;
	} else
	{
		// Display buffers can also be HDR 16:16:16:16 float (0xC, UE4 titles).
		if (!((rt.info.format == 0xa && (rt.info.channel_type == 0x6 || rt.info.channel_type == 0x0) &&
		                        (rt.info.channel_order == 0x0 || rt.info.channel_order == 0x1)) ||
		                       (rt.info.format == 0xc && rt.info.channel_type == 0x7 &&
		                        (rt.info.channel_order == 0x0 || rt.info.channel_order == 0x1 || rt.info.channel_order == 0x2)))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !((rt.info.format == 0xa && (rt.info.channel_type == 0x6 || rt.info.channel_type condition ignored (continuing)\n"); }

		// Display buffer (single swapchain target only).
		if (r->targets_num != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r->targets_num != 1 condition ignored (continuing)\n"); }
		if (attachment.samples != VK_SAMPLE_COUNT_1_BIT) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: attachment.samples != VK_SAMPLE_COUNT_1_BIT condition ignored (continuing)\n"); }
		// HDR display: the render-target tiling (tile 0x1b) and the display
		// tiling (doubled 4bpp table) sizes legitimately differ; the display
		// buffer is the authoritative backing, so only the pitch must match.
		const bool hdr_display = (render_format.format == RenderTextureFormat::R16G16B16A16Sfloat);
		if (!hdr_display && video_image.buffer_size != attachment.size) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !hdr_display && video_image.buffer_size != attachment.size condition ignored (continuing)\n"); }
		if (video_image.buffer_pitch != attachment.pitch) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: video_image.buffer_pitch != attachment.pitch condition ignored (continuing)\n"); }
		attachment.type                 = RenderColorType::DisplayBuffer;
		attachment.base_addr            = rt.base.addr;
		attachment.existing_video_image = video_image.image;
	}

	attachment.cmask_fast_clear_enable = rt.info.cmask_fast_clear_enable;
	attachment.clear_word0             = rt.clear_word0.word0;
	attachment.clear_word1             = rt.clear_word1.word1;
	return true;
}

static void CopyRenderColorSlot(RenderColorInfo* dst, uint32_t dst_slot, const RenderColorInfo& src)
{
	EXIT_IF(dst == nullptr);
	if (dst_slot >= RenderColorInfo::TARGETS_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_slot >= RenderColorInfo::TARGETS_MAX condition ignored (continuing)\n"); }
	if (!RenderColorSlotConfigured(src, 0)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !RenderColorSlotConfigured(src, 0) condition ignored (continuing)\n"); }

	dst->attachment[dst_slot] = src.attachment[0];
	dst->targets_num          = std::max(dst->targets_num, dst_slot + 1);
}

void NormalizeRenderColorArrayBackings(RenderColorInfo* color)
{
	EXIT_IF(color == nullptr);
	for (uint32_t first = 0; first < color->targets_num; first++)
	{
		auto& a = color->attachment[first];
		if (a.type != RenderColorType::RenderTexture || a.image_layers == 0u || a.size % a.image_layers != 0u)
		{
			continue;
		}
		const uint64_t bytes_per_layer = a.size / a.image_layers;
		for (uint32_t second = first + 1u; second < color->targets_num; second++)
		{
			auto& b = color->attachment[second];
			const bool compatible = b.type == RenderColorType::RenderTexture && a.base_addr == b.base_addr &&
			                        a.render_texture_format == b.render_texture_format && a.width == b.width && a.height == b.height &&
			                        a.samples == b.samples && a.pitch == b.pitch && a.tile == b.tile && a.neo == b.neo &&
			                        a.write_back == b.write_back;
			if (!compatible)
			{
				continue;
			}
			if (b.image_layers == 0u || b.size % b.image_layers != 0u || b.size / b.image_layers != bytes_per_layer)
			{
				EXIT("incompatible layered render-target backing sizes\n");
			}
			const uint32_t shared_layers = std::max(a.image_layers, b.image_layers);
			if (bytes_per_layer > UINT64_MAX / shared_layers)
			{
				EXIT("layered render-target backing size overflow\n");
			}
			a.image_layers = shared_layers;
			b.image_layers = shared_layers;
			a.size         = bytes_per_layer * shared_layers;
			b.size         = a.size;
		}
	}
}

void DescribeRenderColorInfo(CommandBuffer* buffer, const HW::Context& hw, RenderColorInfo* r)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer == nullptr || r == nullptr);

	auto mask = hw.GetRenderTargetMask();
	*r        = {};

	if (mask == 0)
	{
		// No color output
		return;
	}

	for (uint32_t slot = 0; slot < RenderColorInfo::TARGETS_MAX; slot++)
	{
		const auto slot_mask = (mask >> (slot * 4u)) & 0xfu;
		if (slot_mask == 0)
		{
			continue;
		}

		RenderColorInfo current {};
		if (!DescribeRenderColorSlotInfo(buffer, hw.GetRenderTarget(slot), &current))
		{
			continue;
		}
		if (current.attachment[0].type == RenderColorType::DisplayBuffer)
		{
			if (RenderColorHasActiveTarget(*r) || slot != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: RenderColorHasActiveTarget(*r) || slot != 0 condition ignored (continuing)\n"); }
		} else
		{
			if (current.attachment[0].type != RenderColorType::RenderTexture) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: current.attachment[0].type != RenderColorType::RenderTexture condition ignored (continuing)\n"); }
		}
		CopyRenderColorSlot(r, slot, current);
	}
	NormalizeRenderColorArrayBackings(r);

	if (r->attachment[0].type == RenderColorType::DisplayBuffer)
	{
		if (r->targets_num != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: r->targets_num != 1 condition ignored (continuing)\n"); }
	}
}

void MaterializeRenderColorInfo(uint64_t submit_id, CommandBuffer* buffer, RenderColorInfo* r)
{
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(r == nullptr);

	if (r->targets_num == 0)
	{
		return;
	}
	for (uint32_t slot = 0; slot < r->targets_num; slot++)
	{
		if (!RenderColorSlotConfigured(*r, slot))
		{
			continue;
		}
		auto& attachment = r->attachment[slot];
		if (attachment.type == RenderColorType::DisplayBuffer)
		{
			const auto video_image = VideoOut::VideoOutGetImageForSubmission(attachment.base_addr, buffer);
			if (video_image.image != attachment.existing_video_image) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: video_image.image != attachment.existing_video_image condition ignored (continuing)\n"); }
			attachment.vulkan_buffer = video_image.image;
			if (attachment.vulkan_buffer == nullptr || attachment.vulkan_buffer->samples != attachment.samples) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: attachment.vulkan_buffer == nullptr || attachment.vulkan_buffer->samples != attachment.samples condition ignored (continuing)\n"); }
			continue;
		}

		if (attachment.type != RenderColorType::RenderTexture) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: attachment.type != RenderColorType::RenderTexture condition ignored (continuing)\n"); }
		RenderTextureObject vulkan_buffer_info(attachment.render_texture_format, attachment.width, attachment.height, attachment.tile,
		                                      attachment.neo, attachment.pitch, attachment.write_back,
		                                      static_cast<uint32_t>(attachment.samples), attachment.image_layers);
		auto* buffer_vulkan = static_cast<Graphics::RenderTextureVulkanImage*>(Graphics::GpuMemoryCreateObject(
		    submit_id, g_render_ctx->GetGraphicCtx(), buffer, attachment.base_addr, attachment.size, vulkan_buffer_info));
		if (buffer_vulkan == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: buffer_vulkan == nullptr condition ignored (continuing)\n"); }
		attachment.vulkan_buffer = buffer_vulkan;
	}
}

void InvalidateMemoryObject(const RenderColorInfo& r);

static void TransitionColorImage(VkCommandBuffer vk_buffer, VulkanImage* image, VkImageLayout new_layout, VkAccessFlags dst_access,
                                 VkPipelineStageFlags dst_stage)
{
	EXIT_IF(image == nullptr);

	if (image->layout == new_layout)
	{
		return;
	}

	VkAccessFlags        src_access = 0;
	VkPipelineStageFlags src_stage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	if (image->layout != VK_IMAGE_LAYOUT_UNDEFINED)
	{
		src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
		             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
		            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	}

	VkImageMemoryBarrier image_memory_barrier {};
	image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	image_memory_barrier.pNext                           = nullptr;
	image_memory_barrier.srcAccessMask                   = src_access;
	image_memory_barrier.dstAccessMask                   = dst_access;
	image_memory_barrier.oldLayout                       = image->layout;
	image_memory_barrier.newLayout                       = new_layout;
	image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.image                           = image->image;
	image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	image_memory_barrier.subresourceRange.baseMipLevel   = 0;
	image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	image_memory_barrier.subresourceRange.baseArrayLayer = 0;
	image_memory_barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

	vkCmdPipelineBarrier(vk_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &image_memory_barrier);
	image->layout = new_layout;
}

bool GraphicsRenderColorResolve(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw)
{
	if (hw.GetColorControl().mode != 3)
	{
		return false;
	}

	EXIT_IF(buffer == nullptr || buffer->IsInvalid());
	if (hw.GetColorControl().op != 0xCC) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: hw.GetColorControl().op != 0xCC condition ignored (continuing)\n"); }

	RenderColorInfo source {};
	RenderColorInfo destination {};
	const auto& source_rt      = hw.GetRenderTarget(0);
	const auto& destination_rt = hw.GetRenderTarget(1);
	// MODE=RESOLVE is a fixed-function color0->color1 operation. The guest can
	// leave one side unbound while reusing the same control state for a later
	// pass; hardware treats that packet as having no color work. Do not turn the
	// missing destination into a host-fatal assertion before the normal draw
	// stream has a chance to continue.
	if (source_rt.base.addr == 0 || destination_rt.base.addr == 0)
	{
		if (std::getenv("KYTY_DUMP_COLOR_RESOLVE") != nullptr)
		{
			static uint32_t skipped_logs = 0;
			if (skipped_logs < 32u)
			{
				++skipped_logs;
				KYTY_LOG_DEBUG(
				             "KYTY_COLOR_RESOLVE_SKIP source=0x%012" PRIx64 " destination=0x%012" PRIx64
				             " mode=%u op=0x%x\n",
				             source_rt.base.addr, destination_rt.base.addr, static_cast<uint32_t>(hw.GetColorControl().mode),
				             hw.GetColorControl().op);
			}
		}
		return true;
	}
	if (!DescribeRenderColorSlotInfo(buffer, source_rt, &source)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !DescribeRenderColorSlotInfo(buffer, source_rt, &source) condition ignored (continuing)\n"); }
	if (!DescribeRenderColorSlotInfo(buffer, destination_rt, &destination)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !DescribeRenderColorSlotInfo(buffer, destination_rt, &destination) condition ignored (continuing)\n"); }
	if (std::getenv("KYTY_DUMP_COLOR_RESOLVE") != nullptr)
	{
		static uint32_t describe_logs = 0;
		if (describe_logs < 32u)
		{
			++describe_logs;
			const auto& src = source.attachment[0];
			const auto& dst = destination.attachment[0];
			KYTY_LOG_DEBUG(
			             "KYTY_COLOR_RESOLVE_DESC src_type=%u src=0x%012" PRIx64 ":%ux%u:p%u:s%u:f%u "
			             "dst_type=%u dst=0x%012" PRIx64 ":%ux%u:p%u:s%u:f%u\n",
			             static_cast<uint32_t>(src.type), src.base_addr, src.width, src.height, src.pitch,
			             static_cast<uint32_t>(src.samples), static_cast<uint32_t>(src.render_texture_format),
			             static_cast<uint32_t>(dst.type), dst.base_addr, dst.width, dst.height, dst.pitch,
			             static_cast<uint32_t>(dst.samples), static_cast<uint32_t>(dst.render_texture_format));
		}
	}
	if (source.attachment[0].type != RenderColorType::RenderTexture ||
	                     destination.attachment[0].type != RenderColorType::RenderTexture) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: source.attachment[0].type != RenderColorType::RenderTexture || condition ignored (continuing)\n"); }

	MaterializeRenderColorInfo(submit_id, buffer, &source);
	MaterializeRenderColorInfo(submit_id, buffer, &destination);

	auto* src = source.attachment[0].vulkan_buffer;
	auto* dst = destination.attachment[0].vulkan_buffer;
	if (src == nullptr || dst == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src == nullptr || dst == nullptr condition ignored (continuing)\n"); }
	if (std::getenv("KYTY_DUMP_DRAW") != nullptr)
	{
		static uint32_t logs = 0;
		if (logs < 32u)
		{
			++logs;
			KYTY_LOG_DEBUG(
			             "KYTY_DUMP_COLOR_RESOLVE src_id=%" PRIu64 " dst_id=%" PRIu64
			             " src=0x%012" PRIx64 ":%ux%u/%ux%u:s%u:f%u dst=0x%012" PRIx64 ":%ux%u/%ux%u:s%u:f%u op=0x%x\n",
			             src->memory.unique_id, dst->memory.unique_id, source.attachment[0].base_addr, src->GetGuestExtent().width,
			             src->GetGuestExtent().height, src->extent.width, src->extent.height, static_cast<uint32_t>(src->samples),
			             static_cast<uint32_t>(src->format), destination.attachment[0].base_addr, dst->GetGuestExtent().width,
			             dst->GetGuestExtent().height, dst->extent.width, dst->extent.height, static_cast<uint32_t>(dst->samples),
			             static_cast<uint32_t>(dst->format), hw.GetColorControl().op);
		}
	}
	if (src->memory.unique_id == dst->memory.unique_id)
	{
		return true;
	}

	if (src->samples == VK_SAMPLE_COUNT_1_BIT) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src->samples == VK_SAMPLE_COUNT_1_BIT condition ignored (continuing)\n"); }
	if (dst->samples != VK_SAMPLE_COUNT_1_BIT) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst->samples != VK_SAMPLE_COUNT_1_BIT condition ignored (continuing)\n"); }
	if (src->format != dst->format) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src->format != dst->format condition ignored (continuing)\n"); }
	if (source.attachment[0].layer_count != destination.attachment[0].layer_count)
	{
		EXIT("color resolve layer-count mismatch: source=%" PRIu32 " destination=%" PRIu32 "\n",
		     source.attachment[0].layer_count, destination.attachment[0].layer_count);
	}
	if (src->GetGuestExtent().width != dst->GetGuestExtent().width ||
	                     src->GetGuestExtent().height != dst->GetGuestExtent().height) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src->GetGuestExtent().width != dst->GetGuestExtent().width || condition ignored (continuing)\n"); }
	if (src->extent.width != dst->extent.width || src->extent.height != dst->extent.height) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src->extent.width != dst->extent.width || src->extent.height != dst->extent.height condition ignored (continuing)\n"); }

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	TransitionColorImage(vk_buffer, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT);
	TransitionColorImage(vk_buffer, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageResolve region {};
	region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.mipLevel       = 0;
	region.srcSubresource.baseArrayLayer = source.attachment[0].base_array_layer;
	region.srcSubresource.layerCount     = source.attachment[0].layer_count;
	region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.dstSubresource.mipLevel       = 0;
	region.dstSubresource.baseArrayLayer = destination.attachment[0].base_array_layer;
	region.dstSubresource.layerCount     = destination.attachment[0].layer_count;
	region.srcOffset                     = {0, 0, 0};
	region.dstOffset                     = {0, 0, 0};
	region.extent                        = {src->extent.width, src->extent.height, 1};

	TraceRenderTargetLifetimeResolve(submit_id, source, destination);
	vkCmdResolveImage(vk_buffer, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
	                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	MaybeDumpColorTargets(g_render_ctx->GetGraphicCtx(), destination);
	InvalidateMemoryObject(destination);
	return true;
}

RenderResolutionPlan PrepareDepthOnlyDisplayResolutionCohort(CommandBuffer* buffer, const RenderColorInfo& color,
                                                                        const RenderDepthInfo& depth)
{
	EXIT_IF(buffer == nullptr);
	RenderResolutionPlan native;
	native.classification   = ResolutionClassification::Native;
	native.reason           = RenderResolutionPlanReason::None;
	native.guest_extent     = {depth.width, depth.height};
	native.host_extent      = native.guest_extent;
	native.attachment_count = depth.format == VK_FORMAT_UNDEFINED ? 0u : 1u;
	if (RenderColorHasActiveTarget(color) || depth.format == VK_FORMAT_UNDEFINED)
	{
		return native;
	}
	const auto             snapshot = RenderResolutionGetSnapshot();
	const ResolutionExtent depth_extent {depth.width, depth.height};
	if (!snapshot.guest_registered || depth_extent != snapshot.guest_display_extent)
	{
		return native;
	}

	RenderResolutionAttachment attachment;
	attachment.guest_extent        = depth_extent;
	attachment.resource.kind       = ResolutionResourceKind::DepthStencilAttachment;
	attachment.resource.compressed = depth.htile;
	GpuMemoryOverlapSnapshot overlap {};
	const bool               overlap_available = GpuMemoryQueryOverlaps(depth.vaddr, depth.size, depth.vaddr_num, &overlap);
	const bool               alias_safe =
	    overlap_available && RenderResolutionAliasAllowsSnapshot(overlap, GpuMemoryObjectType::DepthStencilBuffer, true);
	attachment.resource.ambiguous_alias = !alias_safe;
	RenderResolutionPlanInput input;
	input.attachments      = &attachment;
	input.attachment_count = 1;
	input.expected_count   = 1;
	const auto candidate   = RenderResolutionEvaluatePlan(input);
	const auto requested   = candidate.classification == ResolutionClassification::Scaled ? candidate.host_extent : depth_extent;

	uint32_t   registered_width  = 0;
	uint32_t   registered_height = 0;
	const auto registered_status =
	    VideoOut::VideoOutGetRegisteredHostExtent(buffer, depth_extent.width, depth_extent.height, &registered_width, &registered_height);
	const bool             registered_selected = registered_status == VideoOut::VideoOutRegisteredHostExtentStatus::Uniform;
	const bool             registered_pending  = registered_status == VideoOut::VideoOutRegisteredHostExtentStatus::Unselected;
	const ResolutionExtent registered_extent {registered_selected ? registered_width : requested.width,
	                                          registered_selected ? registered_height : requested.height};
	if (!registered_selected && !registered_pending)
	{
		native.classification = ResolutionClassification::Unsupported;
		native.reason         = RenderResolutionPlanReason::MismatchedHostExtent;
		return native;
	}
	if (registered_selected && registered_extent != requested)
	{
		native.classification = ResolutionClassification::Unsupported;
		native.reason         = RenderResolutionPlanReason::MismatchedHostExtent;
		native.host_extent    = registered_extent;
		return native;
	}

	auto       decision    = EvaluateDepthOnlyRenderExtentCompatibility(depth_extent, registered_extent, candidate);
	if (decision.classification != ResolutionClassification::Scaled)
	{
		const auto video_selection =
		    VideoOut::VideoOutSelectRegisteredHostExtent(buffer, depth_extent.width, depth_extent.height, depth_extent.width,
		                                                 depth_extent.height);
		if (video_selection != VideoOut::VideoOutRegisteredHostExtentStatus::Uniform)
		{
			native.classification = ResolutionClassification::Unsupported;
			native.reason         = RenderResolutionPlanReason::MismatchedHostExtent;
			return native;
		}
		return native;
	}
	if (decision.classification == ResolutionClassification::Scaled)
	{
		VulkanResolutionAttachmentRequest request;
		request.extent = decision.host_extent;
		request.format = depth.format;
		request.sample_count = depth.samples;
		request.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		                 (depth.sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
		const auto capability = EvaluateVulkanResolutionAttachment(g_render_ctx->GetGraphicCtx(), request);
		if (capability.status != VulkanRenderResolutionCapabilityStatus::Success ||
		    capability.decision.status != RenderResolutionImageCapabilityStatus::Supported)
		{
			decision.classification = ResolutionClassification::Unsupported;
			decision.reason         = RenderResolutionPlanReason::DepthCapabilityUnsupported;
			return decision;
		}
	}

	ResolutionExtent selected      = requested;
	const auto*      authorization = decision.classification == ResolutionClassification::Scaled ? &decision : nullptr;
	const auto       status = RenderResolutionSelectDisplayHostExtent(depth_extent, requested, authorization, &selected);
	if (status != RenderDisplaySelectionStatus::Selected)
	{
		decision.classification = ResolutionClassification::Unsupported;
		decision.reason         = RenderResolutionPlanReason::MismatchedHostExtent;
		decision.host_extent    = selected;
		return decision;
	}
	const auto video_selection =
	    VideoOut::VideoOutSelectRegisteredHostExtent(buffer, depth_extent.width, depth_extent.height, selected.width, selected.height);
	if (video_selection != VideoOut::VideoOutRegisteredHostExtentStatus::Uniform)
	{
		decision.classification = ResolutionClassification::Unsupported;
		decision.reason         = RenderResolutionPlanReason::MismatchedHostExtent;
		decision.host_extent    = selected;
		return decision;
	}

	const auto existing_depth = GpuMemoryFindObjectsForSubmission(buffer, depth.vaddr, depth.size, depth.vaddr_num,
	                                                              GpuMemoryObjectType::DepthStencilBuffer, true, false);
	if (existing_depth.Size() > 1)
	{
		KYTY_LOG_DEBUG( "Depth-only display selection found %u exact depth objects\n", existing_depth.Size());
		decision.classification = ResolutionClassification::Unsupported;
		decision.reason         = RenderResolutionPlanReason::MismatchedHostExtent;
		return decision;
	}
	if (!existing_depth.IsEmpty())
	{
		const auto* image = static_cast<const DepthStencilVulkanImage*>(existing_depth[0].obj);
		if (image != nullptr)
		{
			selected.width       = image->extent.width;
			selected.height      = image->extent.height;
			decision.host_extent = selected;
		}
	}
	return decision;
}

RenderResolutionPlan PrepareDisplayResolutionCohort(CommandBuffer* buffer, RenderColorInfo* color, const RenderDepthInfo& depth,
                                                           ShaderPixelInputInfo* ps)
{
	EXIT_IF(buffer == nullptr);
	RenderResolutionPlan native;
	native.classification = ResolutionClassification::Native;
	const auto* first_attachment = color != nullptr ? RenderColorFirstConfiguredAttachment(*color) : nullptr;
	native.guest_extent   = {first_attachment != nullptr ? first_attachment->width : 0,
	                       first_attachment != nullptr ? first_attachment->height : 0};
	native.host_extent    = native.guest_extent;
	if (color == nullptr || ps == nullptr || color->targets_num != 1 || color->attachment[0].type != RenderColorType::DisplayBuffer ||
	    color->attachment[0].existing_video_image == nullptr)
	{
		return native;
	}

	const auto&            display_attachment = color->attachment[0];
	const ResolutionExtent guest {display_attachment.width, display_attachment.height};
	const bool             has_depth = depth.format != VK_FORMAT_UNDEFINED;

	const bool color_alias_safe =
	    RenderResolutionAliasAllowsRanges(&display_attachment.base_addr, &display_attachment.size, 1, GpuMemoryObjectType::VideoOutBuffer, false);
	const bool depth_alias_safe = !has_depth || RenderResolutionAliasAllowsRanges(depth.vaddr, depth.size, depth.vaddr_num,
	                                                                              GpuMemoryObjectType::DepthStencilBuffer, true);

	RenderResolutionAttachment attachments[2];
	attachments[0].guest_extent             = guest;
	attachments[0].resource.kind            = ResolutionResourceKind::ColorAttachment;
	attachments[0].resource.cpu_transfer    = !display_attachment.tile;
	attachments[0].resource.ambiguous_alias = !color_alias_safe;
	uint32_t attachment_count               = 1;
	if (has_depth)
	{
		attachments[1].guest_extent             = {depth.width, depth.height};
		attachments[1].resource.kind            = ResolutionResourceKind::DepthStencilAttachment;
		attachments[1].resource.compressed      = depth.htile;
		attachments[1].resource.ambiguous_alias = !depth_alias_safe;
		attachment_count                        = 2;
	}

	RenderResolutionPlanInput input;
	input.attachments                            = attachments;
	input.attachment_count                       = attachment_count;
	input.expected_count                         = attachment_count;
	input.shader_usage.fragment_coordinates      = ps->ps_pos_xy;
	input.shader_usage.integer_image_coordinates = ps->integer_image_coordinates;
	input.shader_usage.image_size_query          = ps->image_size_query;

	const auto             snapshot = RenderResolutionGetSnapshot();
	RenderHostToGuestScale scale;
	input.shader_usage.fragment_coordinates_supported =
	    BuildRenderHostToGuestScale({guest.width, guest.height}, {snapshot.target_extent.width, snapshot.target_extent.height}, &scale) ==
	    RenderResolutionShaderScaleStatus::Success;
	auto decision = RenderResolutionEvaluatePlan(input);
	VideoOutHostExtentState existing_image_state {};
	EXIT_IF(!VideoOutBufferGetHostExtentState(display_attachment.existing_video_image, &existing_image_state));
	if (existing_image_state.selected)
	{
		decision = ReconcileCommittedRenderExtent(decision, {existing_image_state.width, existing_image_state.height});
	}

	if (decision.classification == ResolutionClassification::Scaled)
	{
		VulkanResolutionAttachmentRequest color_request;
		color_request.extent = decision.host_extent;
		color_request.format = display_attachment.existing_video_image->format;
		color_request.sample_count = display_attachment.existing_video_image->samples;
		color_request.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		                       VK_IMAGE_USAGE_SAMPLED_BIT;
		const auto color_capability = EvaluateVulkanResolutionAttachment(g_render_ctx->GetGraphicCtx(), color_request);
		const bool color_supported  = color_capability.status == VulkanRenderResolutionCapabilityStatus::Success &&
		                              color_capability.decision.status == RenderResolutionImageCapabilityStatus::Supported;
		bool       supported        = color_supported;
		bool       depth_supported  = true;
		if (has_depth)
		{
			VulkanResolutionAttachmentRequest depth_request;
			depth_request.extent        = decision.host_extent;
			depth_request.format        = depth.format;
			depth_request.sample_count  = depth.samples;
			depth_request.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (depth.sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
			const auto depth_capability = EvaluateVulkanResolutionAttachment(g_render_ctx->GetGraphicCtx(), depth_request);
			depth_supported             = depth_capability.status == VulkanRenderResolutionCapabilityStatus::Success &&
			                              depth_capability.decision.status == RenderResolutionImageCapabilityStatus::Supported;
			supported                   = supported && depth_supported;
		}
		if (!supported)
		{
			decision.classification = ResolutionClassification::Unsupported;
			decision.reason         = color_supported ? RenderResolutionPlanReason::DepthCapabilityUnsupported
			                                          : RenderResolutionPlanReason::ColorCapabilityUnsupported;
			decision.attachment_count          = attachment_count;
			decision.blocking_attachment_index = color_supported ? 1u : 0u;
		}
	}

	const ResolutionExtent requested   = decision.classification == ResolutionClassification::Scaled ? decision.host_extent : guest;
	auto                   unsupported = [&decision, guest](ResolutionExtent selected)
	{
		decision.classification = ResolutionClassification::Unsupported;
		if (decision.reason == RenderResolutionPlanReason::None)
		{
			decision.reason = RenderResolutionPlanReason::MismatchedHostExtent;
		}
		decision.guest_extent = guest;
		decision.host_extent  = selected;
		return decision;
	};

	uint32_t   registered_width  = 0;
	uint32_t   registered_height = 0;
	const auto registered_status =
	    VideoOut::VideoOutGetRegisteredHostExtent(buffer, guest.width, guest.height, &registered_width, &registered_height);
	const bool             registered_selected = registered_status == VideoOut::VideoOutRegisteredHostExtentStatus::Uniform;
	const bool             registered_pending  = registered_status == VideoOut::VideoOutRegisteredHostExtentStatus::Unselected;
	const ResolutionExtent registered {registered_selected ? registered_width : requested.width,
	                                   registered_selected ? registered_height : requested.height};
	if (!registered_selected && !registered_pending)
	{
		return unsupported(registered);
	}
	if (registered_selected && registered != requested)
	{
		return unsupported(registered);
	}

	ResolutionExtent selected      = registered;
	const auto*      authorization = requested != guest ? &decision : nullptr;
	const auto       selection     = RenderResolutionSelectDisplayHostExtent(guest, requested, authorization, &selected);
	if (selection != RenderDisplaySelectionStatus::Selected)
	{
		return unsupported(selected);
	}
	if (selected != requested)
	{
		return unsupported(selected);
	}
	const auto video_selection =
	    VideoOut::VideoOutSelectRegisteredHostExtent(buffer, guest.width, guest.height, selected.width, selected.height);
	if (video_selection != VideoOut::VideoOutRegisteredHostExtentStatus::Uniform)
	{
		return unsupported(selected);
	}
	// A mixed guest-extent cohort is intentionally native. Its color and depth
	// images retain their respective native extents, so only a scalable or
	// equal-extent plan may require a pre-existing depth image to match color.
	const bool attachments_share_guest_extent = decision.reason != RenderResolutionPlanReason::MismatchedGuestExtent;
	if (has_depth && attachments_share_guest_extent)
	{
		const auto existing_depth = GpuMemoryFindObjectsForSubmission(buffer, depth.vaddr, depth.size, depth.vaddr_num,
		                                                              GpuMemoryObjectType::DepthStencilBuffer, true, false);
		if (existing_depth.Size() > 1)
		{
			return unsupported(selected);
		}
		if (!existing_depth.IsEmpty())
		{
			const auto* image = static_cast<const DepthStencilVulkanImage*>(existing_depth[0].obj);
			if (image == nullptr || image->extent.width != selected.width || image->extent.height != selected.height)
			{
				return unsupported(selected);
			}
		}
	}

	VideoOutHostExtentState image_state {};
	if (!VideoOutBufferGetHostExtentState(display_attachment.existing_video_image, &image_state) || !image_state.selected ||
	    image_state.width != selected.width || image_state.height != selected.height)
	{
		return unsupported({image_state.width, image_state.height});
	}
	if (decision.classification == ResolutionClassification::Scaled)
	{
		if (BuildRenderHostToGuestScale({guest.width, guest.height}, {selected.width, selected.height}, &ps->host_to_guest_scale) !=
		    RenderResolutionShaderScaleStatus::Success)
		{
			decision.reason = RenderResolutionPlanReason::ShaderCoordinateAccess;
			return unsupported(selected);
		}
	}
	return decision;
}

void RequireSupportedRenderResolutionPlan(const RenderResolutionPlan& decision)
{
	if (decision.classification != ResolutionClassification::Unsupported)
	{
		return;
	}

	EXIT("Render resolution plan unsupported: guest=%ux%u selected=%ux%u reason=%s(%u) attachment_reason=%s(%u) "
	     "attachment_index=%u\n",
	     decision.guest_extent.width, decision.guest_extent.height, decision.host_extent.width, decision.host_extent.height,
	     RenderResolutionPlanReasonName(decision.reason), static_cast<unsigned>(decision.reason),
	     ResolutionNativeReasonName(decision.attachment_native_reason), static_cast<unsigned>(decision.attachment_native_reason),
	     decision.blocking_attachment_index);
}

void CommitMaterializedRenderResolutionPlan(const RenderResolutionPlan& decision, const RenderColorInfo& color,
                                                   const RenderDepthInfo& depth)
{
	RequireSupportedRenderResolutionPlan(decision);
	if (decision.classification != ResolutionClassification::Scaled)
	{
		return;
	}

	if (RenderColorHasActiveTarget(color))
	{
		auto* image = RenderColorFirstActiveImage(color);
		if (image == nullptr || image->extent.width != decision.host_extent.width || image->extent.height != decision.host_extent.height)
		{
			EXIT("Render resolution color materialization mismatch: expected=%ux%u actual=%ux%u\n", decision.host_extent.width,
			     decision.host_extent.height, image != nullptr ? image->extent.width : 0, image != nullptr ? image->extent.height : 0);
		}
	}
	if (depth.vulkan_buffer != nullptr &&
	    (depth.vulkan_buffer->extent.width != decision.host_extent.width || depth.vulkan_buffer->extent.height != decision.host_extent.height))
	{
		EXIT("Render resolution depth materialization mismatch: expected=%ux%u actual=%ux%u\n", decision.host_extent.width,
		     decision.host_extent.height, depth.vulkan_buffer->extent.width, depth.vulkan_buffer->extent.height);
	}
	EXIT_IF(!RenderResolutionMarkScalingApplied(decision));
}

void InvalidateMemoryObject(const RenderColorInfo& r)
{
	for (uint32_t slot = 0; slot < r.targets_num; slot++)
	{
		if (!RenderColorSlotActive(r, slot))
		{
			continue;
		}
		const auto& attachment = r.attachment[slot];
		if (attachment.type == RenderColorType::RenderTexture)
		{
			GpuMemoryResetHash(&attachment.base_addr, &attachment.size, 1, GpuMemoryObjectType::RenderTexture);
		} else if (attachment.type == RenderColorType::DisplayBuffer)
		{
			GpuMemoryResetHash(&attachment.base_addr, &attachment.size, 1, GpuMemoryObjectType::VideoOutBuffer);
		}
	}
}

void InvalidateMemoryObject(const RenderDepthInfo& r)
{
	bool with_depth = (r.format != VK_FORMAT_UNDEFINED && r.vulkan_buffer != nullptr);

	if (with_depth)
	{
		GpuMemoryResetHash(r.vaddr, r.size, r.vaddr_num, GpuMemoryObjectType::DepthStencilBuffer);
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
