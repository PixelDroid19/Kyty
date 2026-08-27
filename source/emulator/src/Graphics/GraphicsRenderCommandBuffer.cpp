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

namespace {

VulkanSubmitAttemptTrail g_submit_fault_trail;

const char* VulkanSubmitKindName(VulkanSubmitKind kind)
{
	switch (kind)
	{
		case VulkanSubmitKind::CommandBuffer: return "command_buffer";
		case VulkanSubmitKind::SemaphoreCommandBuffer: return "semaphore_command_buffer";
		case VulkanSubmitKind::TileDetile: return "tile_detile";
	}
	return "unknown";
}

} // namespace

bool VulkanSubmitFaultTraceEnabled()
{
	static const bool enabled = []
	{
		const char* value = std::getenv("KYTY_SUBMIT_FAULT_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

VulkanSubmitAttemptTrail* VulkanSubmitFaultTraceTrail()
{
	return VulkanSubmitFaultTraceEnabled() ? &g_submit_fault_trail : nullptr;
}

void VulkanSubmitFaultReport(const char* stage, VkResult result, const VulkanSubmitAttempt* immediate)
{
	auto* trail = VulkanSubmitFaultTraceTrail();
	if (trail == nullptr)
	{
		return;
	}
	VulkanSubmitAttemptSnapshot snapshot;
	if (!trail->LatchDeviceLost(result, &snapshot))
	{
		return;
	}

	if (immediate != nullptr)
	{
		KYTY_LOG_ERROR(
		    "KYTY_SUBMIT_FAULT stage=%s result=%d count=%" PRIu32 " dropped=%" PRIu64
		    " immediate_tracked=%d immediate_kind=%s immediate_queue=%" PRIu32 " immediate_slot=%" PRIu32
		    " immediate_host_sequence=%" PRIu64 " immediate_guest_submit=%" PRIu64 " immediate_frame=%" PRId32
		    " immediate_pm4_op=%" PRIu32 " immediate_pm4_dw=%" PRIu32 " immediate_semaphore=%d\n",
		    stage != nullptr ? stage : "unknown", static_cast<int>(result), snapshot.count, snapshot.dropped,
		    immediate->attempt != 0u ? 1 : 0, VulkanSubmitKindName(immediate->kind), immediate->queue,
		    immediate->command_buffer_slot, immediate->host_submission_sequence, immediate->guest_submit, immediate->frame,
		    immediate->pm4_op, immediate->pm4_dw, immediate->signals_semaphore ? 1 : 0);
	} else
	{
		KYTY_LOG_ERROR("KYTY_SUBMIT_FAULT stage=%s result=%d count=%" PRIu32 " dropped=%" PRIu64 "\n",
		               stage != nullptr ? stage : "unknown", static_cast<int>(result), snapshot.count, snapshot.dropped);
	}
	for (uint32_t i = 0; i < snapshot.count; ++i)
	{
		const auto& entry = snapshot.entries[i];
		KYTY_LOG_ERROR(
		    "KYTY_SUBMIT_FAULT_ENTRY attempt=%" PRIu64 " kind=%s queue=%" PRIu32 " slot=%" PRIu32
		    " host_sequence=%" PRIu64 " guest_submit=%" PRIu64 " frame=%" PRId32 " pm4_op=%" PRIu32 " pm4_dw=%" PRIu32
		    " semaphore=%d completed=%d result=%d\n",
		    entry.attempt, VulkanSubmitKindName(entry.kind), entry.queue, entry.command_buffer_slot,
		    entry.host_submission_sequence, entry.guest_submit, entry.frame, entry.pm4_op, entry.pm4_dw,
		    entry.signals_semaphore ? 1 : 0, entry.completed ? 1 : 0, static_cast<int>(entry.result));
	}
}

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
		bool         used    = false;
		bool         scratch = false;
		bool         snapshot_valid = false;
		uint64_t     snapshot_vaddr = 0;
		uint64_t     snapshot_size  = 0;
	};

public:
	VulkanBuffer* Upload(GraphicContext* ctx, const void* data, uint64_t size, uint32_t usage,
	                     GpuMemoryTransientBufferAllocationClass allocation_class)
	{
		EXIT_IF(ctx == nullptr || data == nullptr || size == 0u || usage == 0u);
		auto* entry = Acquire(ctx, size, usage, allocation_class, true, nullptr);
		if (entry == nullptr)
		{
			return nullptr;
		}

		const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, size);
		entry->snapshot_valid = false;
		std::memcpy(entry->mapped, data, static_cast<size_t>(size));
		Commit(entry, size);
		return &entry->buffer;
	}

	VulkanBuffer* Capture(GraphicContext* ctx, uint64_t vaddr, uint64_t size, uint32_t usage, uint64_t* validation_ns,
	                      uint64_t* upload_ns, uint64_t* compare_ns, bool* reused)
	{
		EXIT_IF(ctx == nullptr || validation_ns == nullptr || upload_ns == nullptr || compare_ns == nullptr || reused == nullptr);
		*validation_ns = 0u;
		*upload_ns     = 0u;
		*compare_ns    = 0u;
		*reused        = false;
		if (!GpuMemoryCanUseTransientReadOnlyBuffer(true, size, true, true) || usage == 0u)
		{
			return nullptr;
		}

		const auto capture_start = std::chrono::steady_clock::now();
		const auto finish_upload_time = [&]()
		{
			const auto total_ns = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - capture_start).count());
			const uint64_t excluded_ns = *validation_ns > UINT64_MAX - *compare_ns ? UINT64_MAX : *validation_ns + *compare_ns;
			*upload_ns = total_ns > excluded_ns ? total_ns - excluded_ns : 0u;
		};
		Entry*     previous      = nullptr;
		for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
		{
			auto* candidate = *it;
			if (candidate->used && !candidate->scratch && candidate->snapshot_valid && candidate->usage == usage &&
			    candidate->snapshot_vaddr == vaddr && candidate->snapshot_size == size)
			{
				previous = candidate;
				break;
			}
		}
		if (previous != nullptr)
		{
			bool     matches             = false;
			uint64_t reuse_validation_ns = 0u;
			if (!GpuMemoryCompareSnapshotReadOnlyBuffer(vaddr, size, previous->mapped, &matches, &reuse_validation_ns, compare_ns))
			{
				*validation_ns += reuse_validation_ns;
				return nullptr;
			}
			*validation_ns += reuse_validation_ns;
			if (matches)
			{
				previous->buffer.descriptor_range = size;
				*reused                           = true;
				return &previous->buffer;
			}
		}
		bool       created       = false;
		auto*      entry = Acquire(ctx, size, usage, GpuMemoryTransientBufferAllocationClass::Snapshot, false, nullptr);
		if (entry == nullptr)
		{
			const auto preflight_start = std::chrono::steady_clock::now();
			const bool eligible        = GpuMemoryCanSnapshotReadOnlyBuffer(vaddr, size);
			*validation_ns += static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - preflight_start).count());
			if (!eligible)
			{
				finish_upload_time();
				return nullptr;
			}
			entry = Acquire(ctx, size, usage, GpuMemoryTransientBufferAllocationClass::Snapshot, true, &created);
		}
		if (entry == nullptr)
		{
			finish_upload_time();
			return nullptr;
		}

		uint64_t atomic_validation_ns = 0u;
		uint64_t copy_ns              = 0u;
		if (!GpuMemoryCaptureSnapshotReadOnlyBuffer(vaddr, size, entry->mapped, &atomic_validation_ns, &copy_ns))
		{
			*validation_ns += atomic_validation_ns;
			finish_upload_time();
			if (created)
			{
				DiscardNew(ctx, entry);
			}
			return nullptr;
		}
		*validation_ns += atomic_validation_ns;
		DebugStatsRecordUpload(size, copy_ns);
		entry->snapshot_valid = true;
		entry->snapshot_vaddr = vaddr;
		entry->snapshot_size  = size;
		Commit(entry, size);
		finish_upload_time();
		return &entry->buffer;
	}

	VulkanBuffer* Scratch(GraphicContext* ctx, uint64_t size, uint32_t usage)
	{
		if (ctx == nullptr || size == 0u || usage == 0u)
		{
			return nullptr;
		}
		Entry*   best          = nullptr;
		uint32_t usage_entries = 0;
		for (auto* candidate: m_entries)
		{
			if (candidate->usage == usage)
			{
				usage_entries++;
			}
			if (candidate->scratch && candidate->usage == usage && candidate->size >= size &&
			    (best == nullptr || candidate->size < best->size))
			{
				best = candidate;
			}
		}
		if (best != nullptr)
		{
			best->buffer.descriptor_range = size;
			return &best->buffer;
		}
		if (!GpuMemoryTransientBufferPoolCanAllocate(usage_entries, static_cast<uint32_t>(m_entries.size()), m_total_bytes, size,
		                                               GpuMemoryTransientBufferAllocationClass::Critical))
		{
			return nullptr;
		}
		auto* entry                   = new Entry;
		entry->size                   = size;
		entry->usage                  = usage;
		entry->scratch                = true;
		entry->buffer.usage           = usage;
		entry->buffer.memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
		                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		VulkanCreateBuffer(ctx, size, &entry->buffer);
		VulkanMapMemory(ctx, &entry->buffer.memory, &entry->mapped);
		if (entry->mapped == nullptr)
		{
			VulkanDeleteBuffer(ctx, &entry->buffer);
			delete entry;
			return nullptr;
		}
		m_entries.push_back(entry);
		m_total_bytes += size;
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
	Entry* Acquire(GraphicContext* ctx, uint64_t size, uint32_t usage, GpuMemoryTransientBufferAllocationClass allocation_class,
	               bool allow_create, bool* created)
	{
		if (created != nullptr)
		{
			*created = false;
		}
		Entry*   entry              = nullptr;
		Entry*   larger_unused      = nullptr;
		uint32_t usage_entries      = 0;
		for (auto* candidate: m_entries)
		{
			if (candidate->scratch)
			{
				continue;
			}
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
			if (!allow_create)
			{
				return nullptr;
			}
			if (!GpuMemoryTransientBufferPoolCanAllocate(usage_entries, static_cast<uint32_t>(m_entries.size()), m_total_bytes, size,
			                                                   allocation_class))
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
			if (created != nullptr)
			{
				*created = true;
			}
		}
		return entry;
	}

	void DiscardNew(GraphicContext* ctx, Entry* entry)
	{
		EXIT_IF(ctx == nullptr || entry == nullptr || entry->used || m_entries.empty() || m_entries.back() != entry ||
		        m_total_bytes < entry->size);
		m_entries.pop_back();
		m_total_bytes -= entry->size;
		VulkanUnmapMemory(ctx, &entry->buffer.memory);
		entry->mapped = nullptr;
		VulkanDeleteBuffer(ctx, &entry->buffer);
		delete entry;
	}

	static void Commit(Entry* entry, uint64_t size)
	{
		EXIT_IF(entry == nullptr || entry->mapped == nullptr || size == 0u || size > entry->size);
		const uint64_t tail_bytes = GpuMemoryTransientBufferTailBytes(entry->size, size);
		if (tail_bytes != 0u)
		{
			std::memset(static_cast<uint8_t*>(entry->mapped) + size, 0, static_cast<size_t>(tail_bytes));
		}
		entry->buffer.descriptor_range = size;
		entry->used = true;
	}

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
			const auto reset_result = vkResetCommandBuffer(m_pool->buffers[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
			if (reset_result != VK_SUCCESS)
			{
				VulkanSubmitFaultReport("command_buffer_allocate_reset", reset_result);
				EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(reset_result), m_queue,
				     i);
			}
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
	if (m_transient_scratch_buffers != nullptr)
	{
		m_transient_scratch_buffers->Destroy(g_render_ctx->GetGraphicCtx());
		delete m_transient_scratch_buffers;
		m_transient_scratch_buffers = nullptr;
	}

	m_pool->busy[m_index] = false;
	const auto reset_result = vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	if (reset_result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("command_buffer_free_reset", reset_result);
		EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(reset_result), m_queue,
		     m_index);
	}
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
	if (m_transient_scratch_buffers != nullptr)
	{
		m_transient_scratch_buffers->Reset();
	}

	auto* buffer = m_pool->buffers[m_index];

	VkCommandBufferBeginInfo begin_info {};
	begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext            = nullptr;
	begin_info.flags            = 0;
	begin_info.pInheritanceInfo = nullptr;

	const auto result = vkBeginCommandBuffer(buffer, &begin_info);
	if (result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("command_buffer_begin", result);
		EXIT("vkBeginCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(result), m_queue, m_index);
	}
}

VulkanBuffer* CommandBuffer::UploadTransientBuffer(const void* data, uint64_t size, uint32_t usage)
{
	EXIT_IF(IsInvalid());
	if (m_transient_buffers == nullptr)
	{
		m_transient_buffers = new TransientBufferPool;
	}
	return m_transient_buffers->Upload(g_render_ctx->GetGraphicCtx(), data, size, usage,
	                                   GpuMemoryTransientBufferAllocationClass::Critical);
}

VulkanBuffer* CommandBuffer::CaptureTransientSnapshotBuffer(uint64_t vaddr, uint64_t size, uint32_t usage, uint64_t* validation_ns,
	                                                         uint64_t* upload_ns, uint64_t* compare_ns, bool* reused)
{
	EXIT_IF(IsInvalid());
	if (m_transient_buffers == nullptr)
	{
		m_transient_buffers = new TransientBufferPool;
	}
	return m_transient_buffers->Capture(g_render_ctx->GetGraphicCtx(), vaddr, size, usage, validation_ns, upload_ns, compare_ns, reused);
}

VulkanBuffer* CommandBuffer::AllocateTransientScratchBuffer(uint64_t size, uint32_t usage)
{
	EXIT_IF(IsInvalid());
	if (m_transient_scratch_buffers == nullptr)
	{
		m_transient_scratch_buffers = new TransientBufferPool;
	}
	return m_transient_scratch_buffers->Scratch(g_render_ctx->GetGraphicCtx(), size, usage);
}

void CommandBuffer::End() const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	const auto result = vkEndCommandBuffer(buffer);
	if (result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("command_buffer_end", result);
		EXIT("vkEndCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(result), m_queue, m_index);
	}
	DebugStatsRecordCommandBuffer();
}

VulkanSubmitAttempt CommandBuffer::MakeSubmitAttempt(VulkanSubmitKind kind, bool signals_semaphore) const
{
	VulkanSubmitAttempt attempt;
	attempt.kind                     = kind;
	attempt.host_submission_sequence = m_has_submission ? m_submission.sequence : 0u;
	attempt.guest_submit             = m_guest_submit;
	attempt.queue                    = static_cast<uint32_t>(m_queue);
	attempt.command_buffer_slot      = m_index;
	attempt.frame                    = GraphicsRunGetFrameNum();
	attempt.pm4_op                   = m_pm4_op;
	attempt.pm4_dw                   = m_pm4_dw;
	attempt.signals_semaphore        = signals_semaphore;
	return attempt;
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

	const auto& queue    = g_render_ctx->GetGraphicCtx()->queues[m_queue];
	auto*       trail    = VulkanSubmitFaultTraceTrail();
	const auto  sequence = m_has_submission ? m_submission.sequence : 0u;
	VulkanSubmitAttempt attempt {};
	VulkanSubmitAttempt observed {};
	if (trail != nullptr)
	{
		attempt = MakeSubmitAttempt(VulkanSubmitKind::CommandBuffer, false);
	}

	const VkResult result = VulkanCallAndPublishOnSuccess(
	    [&]() -> VkResult
	    {
		    EXIT_IF(queue.mutex == nullptr);
		    Core::LockGuard queue_lock(*queue.mutex);
		    return VulkanTraceSubmitAttempt(trail, attempt, [&] { return vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence); },
		                                    &observed);
	    },
	    [&]
	    {
		    DebugStatsRecordSubmit();
		    m_execute = true;
	    });
	if (result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("queue_submit", result, trail != nullptr ? &observed : nullptr);
		EXIT("vkQueueSubmit failed: result=%d queue=%d slot=%" PRIu32 " sequence=%" PRIu64 "\n", static_cast<int>(result),
		     m_queue, m_index, sequence);
	}
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

	const auto& queue    = g_render_ctx->GetGraphicCtx()->queues[m_queue];
	auto*       trail    = VulkanSubmitFaultTraceTrail();
	const auto  sequence = m_has_submission ? m_submission.sequence : 0u;
	VulkanSubmitAttempt attempt {};
	VulkanSubmitAttempt observed {};
	if (trail != nullptr)
	{
		attempt = MakeSubmitAttempt(VulkanSubmitKind::SemaphoreCommandBuffer, true);
	}

	const VkResult result = VulkanCallAndPublishOnSuccess(
	    [&]() -> VkResult
	    {
		    EXIT_IF(queue.mutex == nullptr);
		    Core::LockGuard queue_lock(*queue.mutex);
		    return VulkanTraceSubmitAttempt(trail, attempt, [&] { return vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence); },
		                                    &observed);
	    },
	    [&]
	    {
		    DebugStatsRecordSubmit();
		    m_execute = true;
	    });
	if (result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("queue_submit_semaphore", result, trail != nullptr ? &observed : nullptr);
		EXIT("vkQueueSubmit failed: result=%d queue=%d slot=%" PRIu32 " sequence=%" PRIu64 "\n", static_cast<int>(result),
		     m_queue, m_index, sequence);
	}
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

	auto* device = g_render_ctx->GetGraphicCtx()->device;
	const auto status = VulkanCallAndPublishOnSuccess(
	    [&] { return vkGetFenceStatus(device, m_pool->fences[m_index]); },
	    [&]
	    {
		    DebugStatsRecordSubmissionComplete();
		    g_render_ctx->GetVertexClipProbeRenderer()->Complete(this);
		    const auto fence_reset_result = vkResetFences(device, 1, &m_pool->fences[m_index]);
		    if (fence_reset_result != VK_SUCCESS)
		    {
			    VulkanSubmitFaultReport("fence_status_reset", fence_reset_result);
			    EXIT("vkResetFences failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(fence_reset_result), m_queue,
			         m_index);
		    }
		    const auto command_reset_result =
		        vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
		    if (command_reset_result != VK_SUCCESS)
		    {
			    VulkanSubmitFaultReport("fence_status_command_reset", command_reset_result);
			    EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(command_reset_result),
			         m_queue, m_index);
		    }
		    m_execute = false;
	    });
	if (status == VK_NOT_READY)
	{
		return false;
	}
	if (status != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("fence_status", status);
		EXIT("vkGetFenceStatus failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(status), m_queue, m_index);
	}
	return true;
}

void CommandBuffer::WaitForFence(bool drain_label_callbacks, bool reset_command_buffer)
{
	EXIT_IF(IsInvalid());

	if (m_execute)
	{
		auto* device = g_render_ctx->GetGraphicCtx()->device;

		const auto wait_start = std::chrono::steady_clock::now();
		const auto wait_result = VulkanCallAndPublishOnSuccess(
		    [&]
		    { return vkWaitForFences(device, 1, &m_pool->fences[m_index], VK_TRUE, 10000000000ULL); },
		    [&]
		    {
			    DebugStatsRecordSubmissionComplete();
			    g_render_ctx->GetVertexClipProbeRenderer()->Complete(this);
			    if (drain_label_callbacks)
			    {
				    LabelDrainCompleted();
			    }
			    const auto fence_reset_result = vkResetFences(device, 1, &m_pool->fences[m_index]);
			    if (fence_reset_result != VK_SUCCESS)
			    {
				    VulkanSubmitFaultReport("fence_wait_reset", fence_reset_result);
				    EXIT("vkResetFences failed: result=%d queue=%d slot=%" PRIu32 "\n", static_cast<int>(fence_reset_result),
				         m_queue, m_index);
			    }
			    if (reset_command_buffer)
			    {
				    const auto command_reset_result =
				        vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
				    if (command_reset_result != VK_SUCCESS)
				    {
					    VulkanSubmitFaultReport("fence_wait_command_reset", command_reset_result);
					    EXIT("vkResetCommandBuffer failed: result=%d queue=%d slot=%" PRIu32 "\n",
					         static_cast<int>(command_reset_result), m_queue, m_index);
				    }
			    }
			    m_execute = false;
		    });
		const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_start).count();
		DebugStatsRecordFenceWait(static_cast<uint64_t>(wait_ns));
		if (wait_result != VK_SUCCESS)
		{
			const uint64_t sequence = m_has_submission ? m_submission.sequence : 0u;
			VulkanSubmitFaultReport("fence_wait", wait_result);
			EXIT("vkWaitForFences failed: result=%d queue=%d slot=%" PRIu32 " sequence=%" PRIu64 " after=%" PRId64 "ns\n",
			     static_cast<int>(wait_result), m_queue, m_index, sequence, static_cast<int64_t>(wait_ns));
		}
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

	// Draw-side lifetime events cannot observe a clear-only render pass. Record
	// the contract before barriers mutate the emulator-side image layout.
	TraceRenderTargetLifetimePassBegin(m_guest_submit, *color, *framebuffer);

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
	// Transition to the render-pass initial layout, not the subpass layout.
	// First-use CLEAR keeps UNDEFINED so vkCmdBeginRenderPass can discard+clear;
	// a pre-pass UNDEFINED→ATTACHMENT would define nothing and then LOAD garbage.
	if (with_depth && framebuffer->depth_initial_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
	    depth->vulkan_buffer->layout != framebuffer->depth_initial_layout)
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
		image_memory_barrier.newLayout                       = framebuffer->depth_initial_layout;
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
