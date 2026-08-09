#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/GpuMemoryTransientBuffer.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/SampleLocations.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"

#include <chrono>
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// CommandBuffer methods + TransientBufferPool

struct ImageTransitionSource
{
	VkPipelineStageFlags stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkAccessFlags        access = 0;
};

static ImageTransitionSource ResolveImageTransitionSource(VkImageLayout layout)
{
	switch (layout)
	{
		case VK_IMAGE_LAYOUT_UNDEFINED: return {};
		case VK_IMAGE_LAYOUT_PREINITIALIZED: return {VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT};
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
		case VK_IMAGE_LAYOUT_GENERAL:
		default:
			return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
	}
}

class TransientBufferPool
{
	struct Entry
	{
		VulkanBuffer buffer;
		void*        mapped = nullptr;
		uint64_t     size   = 0;
		uint32_t     usage  = 0;
		bool         used   = false;
	};

public:
	VulkanBuffer* Upload(GraphicContext* ctx, const void* data, uint64_t size, uint32_t usage)
	{
		EXIT_IF(ctx == nullptr || data == nullptr || size == 0u || usage == 0u);

		Entry*   entry              = nullptr;
		Entry*   larger_unused      = nullptr;
		uint32_t usage_entries      = 0;
		for (auto* candidate: m_entries)
		{
			if (candidate->usage == usage)
			{
				usage_entries++;
			}
			if (candidate->used || candidate->usage != usage || candidate->size < size)
			{
				continue;
			}
			// Prefer exact size; fall back to the smallest free entry that fits.
			// Descriptor writes use an explicit range (not the full VkBuffer size),
			// so a larger free slab is valid. V# spill UBOs can otherwise fail
			// when many distinct sizes fragment MaxEntriesPerUsage=512.
			if (candidate->size == size)
			{
				entry = candidate;
				break;
			}
			if (larger_unused == nullptr || candidate->size < larger_unused->size)
			{
				larger_unused = candidate;
			}
		}
		if (entry == nullptr)
		{
			entry = larger_unused;
		}

		if (entry == nullptr)
		{
			if (!GpuMemoryTransientBufferPoolCanAllocate(usage_entries, static_cast<uint32_t>(m_entries.size()), m_total_bytes, size))
			{
				return nullptr;
			}

			entry                         = new Entry;
			entry->size                   = size;
			entry->usage                  = usage;
			entry->buffer.usage           = usage;
			entry->buffer.memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
			                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
			VulkanCreateBuffer(ctx, size, &entry->buffer);
			VulkanMapMemory(ctx, &entry->buffer.memory, &entry->mapped);
			EXIT_IF(entry->mapped == nullptr);
			m_entries.push_back(entry);
			m_total_bytes += size;
		}

		const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, size);
		std::memcpy(entry->mapped, data, static_cast<size_t>(size));
		entry->used = true;
		return &entry->buffer;
	}

	void Reset()
	{
		for (auto* entry: m_entries)
		{
			entry->used = false;
		}
	}

	void Destroy(GraphicContext* ctx)
	{
		EXIT_IF(ctx == nullptr);
		for (auto* entry: m_entries)
		{
			EXIT_IF(entry == nullptr || entry->mapped == nullptr);
			VulkanUnmapMemory(ctx, &entry->buffer.memory);
			entry->mapped = nullptr;
			VulkanDeleteBuffer(ctx, &entry->buffer);
			delete entry;
		}
		m_entries.clear();
		m_total_bytes = 0;
	}

private:
	std::vector<Entry*> m_entries;
	uint64_t            m_total_bytes = 0;
};


// CommandBuffer methods (uses TransientBufferPool from Internal.h)

bool CommandBuffer::IsInvalid() const
{
	EXIT_IF(g_render_ctx == nullptr);

	if (m_pool != nullptr)
	{
		Core::LockGuard lock(m_pool->mutex);

		return (m_index == static_cast<uint32_t>(-1) || m_index >= m_pool->buffers_count);
	}

	return true;
}

void CommandBuffer::Allocate()
{
	EXIT_IF(!IsInvalid());

	m_pool = g_command_pool.GetPool(m_queue);

	Core::LockGuard lock(m_pool->mutex);

	for (uint32_t i = 0; i < m_pool->buffers_count; i++)
	{
		if (!m_pool->busy[i])
		{
			m_pool->busy[i] = true;
			vkResetCommandBuffer(m_pool->buffers[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
			m_index = i;
			break;
		}
	}

	if (IsInvalid()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: IsInvalid() condition ignored (continuing)\n"); }
}

void CommandBuffer::Free()
{
	EXIT_IF(IsInvalid());

	Core::LockGuard lock(m_pool->mutex);

	WaitForFence();
	if (m_transient_buffers != nullptr)
	{
		m_transient_buffers->Destroy(g_render_ctx->GetGraphicCtx());
		delete m_transient_buffers;
		m_transient_buffers = nullptr;
	}

	m_pool->busy[m_index] = false;
	vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	m_index = static_cast<uint32_t>(-1);

	if (!IsInvalid()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !IsInvalid() condition ignored (continuing)\n"); }
}

void CommandBuffer::Begin() const
{
	EXIT_IF(IsInvalid());
	if (m_transient_buffers != nullptr)
	{
		m_transient_buffers->Reset();
	}

	auto* buffer = m_pool->buffers[m_index];

	VkCommandBufferBeginInfo begin_info {};
	begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext            = nullptr;
	begin_info.flags            = 0;
	begin_info.pInheritanceInfo = nullptr;

	auto result = vkBeginCommandBuffer(buffer, &begin_info);

	if (result != VK_SUCCESS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: result != VK_SUCCESS condition ignored (continuing)\n"); }
}

VulkanBuffer* CommandBuffer::UploadTransientBuffer(const void* data, uint64_t size, uint32_t usage)
{
	EXIT_IF(IsInvalid());
	if (m_transient_buffers == nullptr)
	{
		m_transient_buffers = new TransientBufferPool;
	}
	return m_transient_buffers->Upload(g_render_ctx->GetGraphicCtx(), data, size, usage);
}

void CommandBuffer::End() const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	auto result = vkEndCommandBuffer(buffer);

	if (result != VK_SUCCESS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: result != VK_SUCCESS condition ignored (continuing)\n"); }
	DebugStatsRecordCommandBuffer();
}

void CommandBuffer::Execute()
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 0;
	submit_info.pWaitSemaphores      = nullptr;
	submit_info.pWaitDstStageMask    = nullptr;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 0;
	submit_info.pSignalSemaphores    = nullptr;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	const auto& queue = g_render_ctx->GetGraphicCtx()->queues[m_queue];

	{
		EXIT_IF(queue.mutex == nullptr);
		Core::LockGuard queue_lock(*queue.mutex);
		// Bounded submit ring for DEVICE_LOST triage (slot that later fails wait).
		if (const char* submit_log = std::getenv("KYTY_SUBMIT_LOG"); submit_log != nullptr && submit_log[0] != '\0')
		{
			KYTY_LOG_DEBUG( "KYTY_SUBMIT slot=%" PRIu32 " queue=%d fence=%p frame=%d\n", m_index, m_queue, static_cast<void*>(fence),
			             GraphicsRunGetFrameNum());
		}
		auto result = vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence);
		if (result != VK_SUCCESS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: result != VK_SUCCESS condition ignored (continuing)\n"); }
	}
	DebugStatsRecordSubmit();

	m_execute = true;
}

void CommandBuffer::ExecuteWithSemaphore(VkSemaphore signal_semaphore)
{
	EXIT_IF(IsInvalid());
	EXIT_IF(signal_semaphore == nullptr);

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 0;
	submit_info.pWaitSemaphores      = nullptr;
	submit_info.pWaitDstStageMask    = nullptr;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores    = &signal_semaphore;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	const auto& queue = g_render_ctx->GetGraphicCtx()->queues[m_queue];

	{
		EXIT_IF(queue.mutex == nullptr);
		Core::LockGuard queue_lock(*queue.mutex);
		auto            result = vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence);
		if (result != VK_SUCCESS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: result != VK_SUCCESS condition ignored (continuing)\n"); }
	}
	DebugStatsRecordSubmit();

	m_execute = true;
}

void CommandBuffer::WaitForFence()
{
	WaitForFence(true, false);
}

void CommandBuffer::WaitForFenceWithoutLabelCallbacks()
{
	WaitForFence(false, false);
}

void CommandBuffer::WaitForFenceAndReset()
{
	WaitForFence(true, true);
}

void CommandBuffer::WaitForFenceAndResetWithoutLabelCallbacks()
{
	WaitForFence(false, true);
}

bool CommandBuffer::TryCompleteFenceAndResetWithoutLabelCallbacks()
{
	EXIT_IF(IsInvalid());
	if (!m_execute)
	{
		return true;
	}

	auto*      device = g_render_ctx->GetGraphicCtx()->device;
	const auto status = vkGetFenceStatus(device, m_pool->fences[m_index]);
	if (status == VK_NOT_READY)
	{
		return false;
	}
	if (status != VK_SUCCESS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: status != VK_SUCCESS condition ignored (continuing)\n"); }

	DebugStatsRecordSubmissionComplete();
	const auto fence_reset_result = vkResetFences(device, 1, &m_pool->fences[m_index]);
	if (fence_reset_result != VK_SUCCESS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: fence_reset_result != VK_SUCCESS condition ignored (continuing)\n"); }
	const auto command_reset_result = vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	if (command_reset_result != VK_SUCCESS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: command_reset_result != VK_SUCCESS condition ignored (continuing)\n"); }
	m_execute = false;
	return true;
}

void CommandBuffer::WaitForFence(bool drain_label_callbacks, bool reset_command_buffer)
{
	EXIT_IF(IsInvalid());

	if (m_execute)
	{
		auto* device = g_render_ctx->GetGraphicCtx()->device;

		const auto wait_start  = std::chrono::steady_clock::now();
		const auto wait_result = vkWaitForFences(device, 1, &m_pool->fences[m_index], VK_TRUE,
		                                         10000000000ULL); // 10s bounded timeout to prevent GPU pipeline freeze
		const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_start).count();
		if (wait_result == VK_TIMEOUT)
		{
			KYTY_LOG_DEBUG("WARNING: vkWaitForFences timeout on slot %" PRIu32 " after %" PRId64 "ns (fence may still signal)\n", m_index,
			       static_cast<int64_t>(wait_ns));
		}
		if (wait_result != VK_SUCCESS && wait_result != VK_TIMEOUT)
		{
			// Bounded diagnostic: classify device-lost vs other fence failures.
			KYTY_LOG_ERROR("ERROR: vkWaitForFences wait_result=%d slot=%" PRIu32 " after %" PRId64 "ns\n",
			             static_cast<int>(wait_result), m_index, static_cast<int64_t>(wait_ns));
		}
		if (wait_result != VK_SUCCESS && wait_result != VK_TIMEOUT) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: wait_result != VK_SUCCESS && wait_result != VK_TIMEOUT condition ignored (continuing)\n"); }
		DebugStatsRecordFenceWait(static_cast<uint64_t>(wait_ns));
		DebugStatsRecordSubmissionComplete();
		if (drain_label_callbacks)
		{
			LabelDrainCompleted();
		}
		if (wait_result != VK_TIMEOUT)
		{
			const auto fence_reset_result = vkResetFences(device, 1, &m_pool->fences[m_index]);
			if (fence_reset_result != VK_SUCCESS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: fence_reset_result != VK_SUCCESS condition ignored (continuing)\n"); }
		} else
		{
			// Fence did not signal within timeout — skip reset to avoid
			// VK_ERROR_DEVICE_LOST.  The fence slot will be reclaimed on
			// the next successful WaitForFence pass for this slot.
		}
		if (reset_command_buffer)
		{
			const auto command_reset_result = vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
			if (command_reset_result != VK_SUCCESS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: command_reset_result != VK_SUCCESS condition ignored (continuing)\n"); }
		}

		m_execute = false;
	}
}

void CommandBuffer::BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
                                    const VulkanSampleLocationState* sample_locations) const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	EXIT_IF(framebuffer == nullptr);

	bool with_depth = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	bool with_color = (RenderColorHasActiveTarget(*color) && RenderColorFirstActiveImage(*color) != nullptr);

	if (!with_depth && !with_color) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !with_depth && !with_color condition ignored (continuing)\n"); }

	auto* depth_image = (with_depth ? depth->vulkan_buffer : nullptr);
	// Custom sample locations apply only when the draw actually carries them.
	// A multisampled depth image reused by a 1x draw (or a copy whose guest
	// state did not program AA) has no location state; render it with the
	// driver default positions instead of rejecting the pass.
	const bool custom_depth_locations =
	    (sample_locations != nullptr && VulkanSampleLocationsEnabled(*sample_locations) && depth_image != nullptr &&
	     depth_image->sample_locations_compatible && depth_image->samples != VK_SAMPLE_COUNT_1_BIT);
	if (custom_depth_locations)
	{
		if (sample_locations == nullptr || !VulkanSampleLocationsEnabled(*sample_locations)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sample_locations == nullptr || !VulkanSampleLocationsEnabled(*sample_locations) condition ignored (continuing)\n"); }
		if (sample_locations->sample_count != depth_image->samples) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: sample_locations->sample_count != depth_image->samples condition ignored (continuing)\n"); }
	} else if (sample_locations != nullptr && VulkanSampleLocationsEnabled(*sample_locations) && with_depth)
	{
		// MSAA draw against a depth image that was not created
		// SAMPLE_LOCATIONS_COMPATIBLE: drop the custom locations for this pass
		// (default sample positions) instead of rejecting the draw. Possible
		// edge artifacts on the depth test; visual output is preserved.
	}

	const uint32_t color_count = (with_color ? color->targets_num : (with_depth ? 1u : 0u));
	VkClearValue   clears[RenderColorInfo::TARGETS_MAX + 1] {};
	uint32_t       clear_attachment = 0;
	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		if (with_color && !RenderColorSlotActive(*color, slot))
		{
			continue;
		}
		if (with_color)
		{
			// Clear values belong to VkRenderPassBeginInfo, not the render-pass or
			// framebuffer identity. Keeping them in the cache key creates a fresh
			// render pass/pipeline for every animated clear color and stalls loading
			// on Metal pipeline compilation.
			const auto& attachment = color->attachment[slot];
			const auto clear = ResolveColorAttachmentLoadOps(attachment.vulkan_buffer->layout, attachment.cmask_fast_clear_enable,
			                                                  attachment.clear_word0, attachment.clear_word1, attachment.vulkan_buffer->format);
			clears[clear_attachment].color = {{clear.clear_r, clear.clear_g, clear.clear_b, clear.clear_a}};
		} else
		{
			clears[clear_attachment].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
		}
		clear_attachment++;
	}
	if (with_depth)
	{
		if (framebuffer->depth_attachment_index >= framebuffer->attachment_count) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: framebuffer->depth_attachment_index >= framebuffer->attachment_count condition ignored (continuing)\n"); }
		clears[framebuffer->depth_attachment_index].depthStencil = {depth->depth_clear_value, depth->stencil_clear_value};
	}

	const VkExtent2D extent = framebuffer->extent;
	if (extent.width == 0 || extent.height == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: extent.width == 0 || extent.height == 0 condition ignored (continuing)\n"); }

	VkRenderPassBeginInfo render_pass_info {};
	render_pass_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render_pass_info.pNext             = nullptr;
	render_pass_info.renderPass        = framebuffer->render_pass;
	render_pass_info.framebuffer       = framebuffer->framebuffer;
	render_pass_info.renderArea.offset = {0, 0};
	render_pass_info.renderArea.extent = extent;
	render_pass_info.clearValueCount   = framebuffer->attachment_count;
	render_pass_info.pClearValues      = clears;

	VkSampleLocationEXT current_sample_location_values[kVulkanSampleLocationMaxCount] = {};
	VkSampleLocationEXT previous_sample_location_values[kVulkanSampleLocationMaxCount] = {};
	VkSampleLocationsInfoEXT current_sample_location_info {};
	VkSampleLocationsInfoEXT previous_sample_location_info {};
	VkAttachmentSampleLocationsEXT attachment_initial_sample_locations {};
	VkSubpassSampleLocationsEXT post_subpass_sample_locations {};
	VkRenderPassSampleLocationsBeginInfoEXT render_pass_sample_locations {};
	if (custom_depth_locations)
	{
		if (!VulkanSampleLocationsPopulateInfo(*sample_locations, current_sample_location_values,
		                                                       &current_sample_location_info)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanSampleLocationsPopulateInfo(*sample_locations, current_sample_location_va condition ignored (continuing)\n"); }
		post_subpass_sample_locations.subpassIndex         = 0;
		post_subpass_sample_locations.sampleLocationsInfo  = current_sample_location_info;
		render_pass_sample_locations.sType                 = VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT;
		render_pass_sample_locations.postSubpassSampleLocationsCount = 1;
		render_pass_sample_locations.pPostSubpassSampleLocations     = &post_subpass_sample_locations;

		if (depth_image->layout != VK_IMAGE_LAYOUT_UNDEFINED)
		{
			if (!VulkanSampleLocationsEnabled(depth_image->last_sample_locations)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanSampleLocationsEnabled(depth_image->last_sample_locations) condition ignored (continuing)\n"); }
			if (!VulkanSampleLocationsPopulateInfo(depth_image->last_sample_locations,
			                                                       previous_sample_location_values, &previous_sample_location_info)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanSampleLocationsPopulateInfo(depth_image->last_sample_locations, condition ignored (continuing)\n"); }
			attachment_initial_sample_locations.attachmentIndex       = framebuffer->depth_attachment_index;
			attachment_initial_sample_locations.sampleLocationsInfo   = previous_sample_location_info;
			render_pass_sample_locations.attachmentInitialSampleLocationsCount = 1;
			render_pass_sample_locations.pAttachmentInitialSampleLocations     = &attachment_initial_sample_locations;
		}
		render_pass_info.pNext = &render_pass_sample_locations;
	}

	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		if (!with_color || color->attachment[slot].vulkan_buffer == nullptr ||
		    color->attachment[slot].vulkan_buffer->layout == framebuffer->color_initial_layout[slot])
		{
			continue;
		}
		auto* image = color->attachment[slot].vulkan_buffer;
		const auto source = ResolveImageTransitionSource(image->layout);
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext                           = nullptr;
		image_memory_barrier.srcAccessMask                   = source.access;
		image_memory_barrier.dstAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		image_memory_barrier.oldLayout                       = image->layout;
		image_memory_barrier.newLayout                       = framebuffer->color_initial_layout[slot];
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = image->image;
		image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

		vkCmdPipelineBarrier(buffer, source.stages, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
		                     nullptr, 1, &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}

	const auto depth_stencil_layout = framebuffer->depth_stencil_layout;
	if (with_depth && depth_stencil_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
	                     depth_stencil_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: with_depth && depth_stencil_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_O condition ignored (continuing)\n"); }
	if (with_depth && depth->vulkan_buffer->layout != depth_stencil_layout)
	{
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext =
		    (custom_depth_locations && depth_image->layout != VK_IMAGE_LAYOUT_UNDEFINED ? &previous_sample_location_info : nullptr);
		image_memory_barrier.srcAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.dstAccessMask =
		    (depth_stencil_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
		         ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT
		         : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
		image_memory_barrier.oldLayout                       = depth->vulkan_buffer->layout;
		image_memory_barrier.newLayout                       = depth_stencil_layout;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = depth->vulkan_buffer->image;
		image_memory_barrier.subresourceRange.aspectMask     = DepthFormatAspectMask(depth->vulkan_buffer->format);
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

		vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &image_memory_barrier);

		depth->vulkan_buffer->layout = image_memory_barrier.newLayout;
	}

	vkCmdBeginRenderPass(buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

	// The render pass final layout is COLOR_ATTACHMENT_OPTIMAL. Keep the
	// emulator-side tracker in sync so a later sampled use emits the required
	// attachment-to-shader-read barrier instead of treating the image as new.
	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		if (with_color && color->attachment[slot].vulkan_buffer != nullptr)
		{
			color->attachment[slot].vulkan_buffer->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
	}
	if (with_depth)
	{
		depth->vulkan_buffer->layout = depth_stencil_layout;
		if (custom_depth_locations)
		{
			depth_image->last_sample_locations = *sample_locations;
		}
	}
}

void CommandBuffer::EndRenderPass() const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	vkCmdEndRenderPass(buffer);
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
