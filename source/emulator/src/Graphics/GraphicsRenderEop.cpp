#include "Emulator/Graphics/GdsRange.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GpuSubmissionTracker.h"
#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/TimePort.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cstring>
#include <cinttypes>
#include <vector>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// EOP labels, eq events, GDS clear/read, memory free/flush

static bool ValidateTransientLabelDestination(const void* dst_gpu_addr, uint64_t size)
{
	if (dst_gpu_addr == nullptr)
	{
		return false;
	}
	const auto status = GpuMemoryValidateAllocatedRange(reinterpret_cast<uint64_t>(dst_gpu_addr), size);
	if (status != GpuMemoryRangeValidationStatus::Valid)
	{
		KYTY_LOG_DEBUG("WARNING: EOP destination range invalid (write skipped)\n");
		return false;
	}
	return true;
}

static void RecordTransientLabel32(CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value, uint32_t dst_word_count,
                                   LabelCallback callback_1, LabelCallback callback_2, const uint64_t* args)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	uint64_t empty_args[LABEL_ARGS_MAX] = {};
	auto*    label = LabelCreate32(g_render_ctx->GetGraphicCtx(), dst_gpu_addr, value, dst_word_count, callback_1, callback_2,
	                               args != nullptr ? args : empty_args);

	LabelSet(buffer, label);
	LabelDelete(label);
}

static void RecordTransientLabel64(CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value, LabelCallback callback_1,
                                   LabelCallback callback_2, const uint64_t* args)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	uint64_t empty_args[LABEL_ARGS_MAX] = {};
	auto*    label =
	    LabelCreate64(g_render_ctx->GetGraphicCtx(), dst_gpu_addr, value, callback_1, callback_2, args != nullptr ? args : empty_args);

	LabelSet(buffer, label);
	LabelDelete(label);
}

void GraphicsRenderWriteAtEndOfPipe32(uint64_t /*submit_id*/, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (!ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());
	RecordTransientLabel32(buffer, dst_gpu_addr, value, 1u, nullptr, nullptr, nullptr);
}

// One GDS read publication. The deferred release of the recording's submission owns it: that
// release runs at publication after the label has fired. The label only borrows it.
// Production command processors drain their recordings; discarding a recording
// would additionally require cancellation of its bound label.
struct GdsPublication
{
	VulkanBuffer staging;
	void*        mapped = nullptr;
	uint32_t*    dst    = nullptr;
	uint64_t     bytes  = 0;
};

static bool GdsPublishLabelCallback(SubmissionId /*submission*/, const uint64_t* args)
{
	const auto* record  = reinterpret_cast<const GdsPublication*>(args[0]);
	const auto  address = reinterpret_cast<uint64_t>(record->dst);

	// Releasing or invalidating a GPU mapping first drains every command processor, which publishes
	// this submission while the destination still exists. A destination that is no longer a GPU
	// mapping, or that the guest unmapped or made read-only directly, never receives stale bytes.
	bool written = GpuMemoryValidateAllocatedRange(address, record->bytes) == GpuMemoryRangeValidationStatus::Valid;
	if (written)
	{
		// The lease lifts tracker protection and keeps it from rearming during the copy, and publishes
		// the write generation when it ends. The guest copy validates ownership and write access in
		// one transaction with unmap and protection changes.
		auto&          tracker = GpuDirtyPageTracker::Instance();
		const uint64_t token   = tracker.BeginHostWrite(address, record->bytes);
		written                = Core::VirtualMemory::CopyToGuest(address, record->mapped, record->bytes);
		tracker.EndHostWrite(token);
	}
	if (!written)
	{
		EXIT("GDS publication destination is no longer a writable guest GPU mapping: address=0x%016" PRIx64 " bytes=%" PRIu64 "\n",
		     address, record->bytes);
	}
	GraphicsRenderMemoryFlush(address, record->bytes);

	// The label's own value must not overwrite the published destination.
	return false;
}

static void GdsReleasePublication(GdsPublication* record)
{
	GraphicContext* ctx = g_render_ctx->GetGraphicCtx();
	VulkanUnmapMemory(ctx, &record->staging.memory);
	VulkanDeleteBuffer(ctx, &record->staging);
	g_render_ctx->GetGdsBuffer()->ReleaseStaging(record->bytes);
	delete record;
}

static GraphicsGdsTransferResult GdsPublishToGuest(CommandBuffer* buffer, uint64_t dw_offset, uint32_t* dst, uint64_t dw_count)
{
	EXIT_IF(buffer == nullptr);
	if (!GraphicsGdsDwordRangeValid(dw_offset, dw_count))
	{
		return GraphicsGdsTransferResult::InvalidRange;
	}
	if (dw_count == 0)
	{
		return GraphicsGdsTransferResult::Recorded;
	}
	const uint64_t bytes = dw_count * sizeof(*dst);
	if (!ValidateTransientLabelDestination(dst, bytes))
	{
		return GraphicsGdsTransferResult::InvalidDestination;
	}
	// Labels complete exactly only on a command-processor recording with a submission.
	SubmissionId recording;
	if (buffer->GetParent() == nullptr || !buffer->GetSubmissionId(&recording))
	{
		EXIT("GDS publication requires a command-processor recording with a submission\n");
	}
	auto* gds = g_render_ctx->GetGdsBuffer();
	if (!gds->TryReserveStaging(bytes))
	{
		return GraphicsGdsTransferResult::StagingBudgetExhausted;
	}

	GraphicContext* ctx    = g_render_ctx->GetGraphicCtx();
	auto*           record = new GdsPublication;
	record->staging.usage           = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	record->staging.memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	record->staging.buffer          = nullptr;
	record->dst                     = dst;
	record->bytes                   = bytes;
	VulkanCreateBuffer(ctx, bytes, &record->staging);
	if (record->staging.buffer == nullptr)
	{
		EXIT("GDS publication staging allocation failed: bytes=%" PRIu64 "\n", bytes);
	}
	VulkanMapMemory(ctx, &record->staging.memory, &record->mapped);
	if (record->mapped == nullptr)
	{
		EXIT("GDS publication staging map failed\n");
	}
	// Ownership passes to the deferred release before any command can reference the record.
	GpuMemoryDeferUntilSubmissionComplete(recording, [record]() { GdsReleasePublication(record); });

	Core::LockGuard lock(g_render_ctx->GetMutex());

	if (!gds->RecordCopyToHost(buffer, ctx, dw_offset, &record->staging, dw_count))
	{
		EXIT("GDS publication copy rejected: offset=%" PRIu64 " count=%" PRIu64 "\n", dw_offset, dw_count);
	}
	uint64_t args[LABEL_ARGS_MAX] = {reinterpret_cast<uint64_t>(record), 0, 0, 0};
	RecordTransientLabel32(buffer, dst, 0, static_cast<uint32_t>(dw_count), GdsPublishLabelCallback, nullptr, args);
	return GraphicsGdsTransferResult::Recorded;
}

void GraphicsRenderWriteAtEndOfPipeGds32(uint64_t /*submit_id*/, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t dw_offset,
                                         uint32_t dw_num)
{
	EXIT_IF(g_render_ctx == nullptr);
	// The packed EOP value must name a span inside the guest window. Refusing here keeps the label from
	// being recorded, so no GDS read can run past the buffer.
	if (!GraphicsGdsDwordRangeValid(dw_offset, dw_num))
	{
		EXIT("EOP GDS range outside the guest window: offset=%" PRIu32 " count=%" PRIu32 "\n", dw_offset, dw_num);
	}
	const auto result = GdsPublishToGuest(buffer, dw_offset, dst_gpu_addr, dw_num);
	// An unregistered destination receives nothing, as for every other end-of-pipe write.
	if (result == GraphicsGdsTransferResult::Recorded || result == GraphicsGdsTransferResult::InvalidDestination)
	{
		return;
	}
	EXIT("EOP GDS publication rejected: offset=%" PRIu32 " count=%" PRIu32 " result=%u\n", dw_offset, dw_num,
	     static_cast<uint32_t>(result));
}

void GraphicsRenderWriteAtEndOfPipe64(uint64_t /*submit_id*/, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (!ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());
	RecordTransientLabel64(buffer, dst_gpu_addr, value, nullptr, nullptr, nullptr);
}

void GraphicsRenderWriteAtEndOfPipeClockCounter(uint64_t /*submit_id*/, CommandBuffer* buffer, uint64_t* dst_gpu_addr)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (!ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[LABEL_ARGS_MAX] = {reinterpret_cast<uint64_t>(dst_gpu_addr), 0, 0, 0};

	RecordTransientLabel64(
	    buffer, dst_gpu_addr, 0,
	    [](SubmissionId /*submission*/, const uint64_t* args)
	    {
		    auto* dst_gpu_addr = reinterpret_cast<uint64_t*>(args[0]);
		    EXIT_IF(dst_gpu_addr == nullptr);
		    *dst_gpu_addr = Kernel::TimePort::GetCounter();
		    KYTY_LOG_DEBUG(FG_BRIGHT_GREEN "EndOfPipe Signal!!! [0x%016" PRIx64 "] <- Clock: 0x%016" PRIx64 "\n" FG_DEFAULT,
		           reinterpret_cast<uint64_t>(dst_gpu_addr), *dst_gpu_addr);
		    return false;
	    },
	    nullptr, args);
}

void GraphicsRenderWriteAtEndOfPipeWithWriteBack64(uint64_t /*submit_id*/, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (dst_gpu_addr != nullptr && !ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	RecordTransientLabel64(
	    buffer, dst_gpu_addr, value,
	    [](SubmissionId submission, const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);

		    GpuMemoryWriteBackCompletedSubmission(g_render_ctx->GetGraphicCtx(), submission);
		    return true;
	    },
	    nullptr, nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack64(uint64_t /*submit_id*/, CommandBuffer* buffer, uint64_t* dst_gpu_addr,
                                                            uint64_t value, uint32_t interrupt_context_id)
{
	EXIT_IF(g_render_ctx == nullptr);
	// A null target is valid for callback-only cache/interrupt packets. Any
	// non-null target still has to be a registered guest allocation.
	if (dst_gpu_addr != nullptr && !ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	const uint64_t args[LABEL_ARGS_MAX] = {interrupt_context_id};

	RecordTransientLabel64(
	    buffer, dst_gpu_addr, value,
	    [](SubmissionId submission, const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);

		    GpuMemoryWriteBackCompletedSubmission(g_render_ctx->GetGraphicCtx(), submission);
		    return true;
	    },
	    [](SubmissionId /*submission*/, const uint64_t* args)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeWithInterrupt64(uint64_t /*submit_id*/, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value,
                                                   uint32_t interrupt_context_id)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (dst_gpu_addr != nullptr && !ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	const uint64_t args[LABEL_ARGS_MAX] = {interrupt_context_id};

	RecordTransientLabel64(
	    buffer, dst_gpu_addr, value, nullptr,
	    [](SubmissionId /*submission*/, const uint64_t* args)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeWithInterrupt32(uint64_t /*submit_id*/, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value,
                                                   uint32_t interrupt_context_id)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (!ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	const uint64_t args[LABEL_ARGS_MAX] = {interrupt_context_id};

	RecordTransientLabel32(
	    buffer, dst_gpu_addr, value, 1u, nullptr,
	    [](SubmissionId /*submission*/, const uint64_t* args)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent(static_cast<uint32_t>(args[0]));
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBackFlip32(uint64_t /*submit_id*/, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                                                                uint32_t value, int handle, int index, int flip_mode, int64_t flip_arg)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (!ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index), static_cast<uint64_t>(flip_mode),
	                                 static_cast<uint64_t>(flip_arg), 0};

	RecordTransientLabel32(
	    buffer, dst_gpu_addr, value, 1u,
	    [](SubmissionId submission, const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);

		    GpuMemoryWriteBackCompletedSubmission(g_render_ctx->GetGraphicCtx(), submission);
		    return true;
	    },
	    [](SubmissionId /*submission*/, const uint64_t* args)
	    {
		    EXIT_IF(g_render_ctx == nullptr);

		    int     handle    = static_cast<int>(args[0]);
		    int     index     = static_cast<int>(args[1]);
		    int     flip_mode = static_cast<int>(args[2]);
		    int64_t flip_arg  = static_cast<int64_t>(args[3]);

		    VideoOut::VideoOutSubmitFlipInternal(handle, index, flip_mode, flip_arg);
		    // The EOP flip packet carries no interrupt context id.
		    g_render_ctx->TriggerEopEvent(0);
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeWithFlip32(uint64_t /*submit_id*/, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value,
                                              int handle, int index, int flip_mode, int64_t flip_arg)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (!ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index), static_cast<uint64_t>(flip_mode),
	                                 static_cast<uint64_t>(flip_arg)};

	RecordTransientLabel32(
	    buffer, dst_gpu_addr, value, 1u, nullptr,
	    [](SubmissionId /*submission*/, const uint64_t* args)
	    {
		    int     handle    = static_cast<int>(args[0]);
		    int     index     = static_cast<int>(args[1]);
		    int     flip_mode = static_cast<int>(args[2]);
		    int64_t flip_arg  = static_cast<int64_t>(args[3]);

		    VideoOut::VideoOutSubmitFlipInternal(handle, index, flip_mode, flip_arg);
		    return true;
	    },
	    args);
}

void GraphicsRenderWriteAtEndOfPipeOnlyFlip(uint64_t /*submit_id*/, CommandBuffer* buffer, int handle, int index, int flip_mode,
                                            int64_t flip_arg)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index), static_cast<uint64_t>(flip_mode),
	                                 static_cast<uint64_t>(flip_arg)};

	RecordTransientLabel32(
	    buffer, nullptr, 0, 0u, nullptr,
	    [](SubmissionId /*submission*/, const uint64_t* args)
	    {
		    int     handle    = static_cast<int>(args[0]);
		    int     index     = static_cast<int>(args[1]);
		    int     flip_mode = static_cast<int>(args[2]);
		    int64_t flip_arg  = static_cast<int64_t>(args[3]);

		    VideoOut::VideoOutSubmitFlipInternal(handle, index, flip_mode, flip_arg);
		    return true;
	    },
	    args);
}

void GraphicsRenderQueueQueuedGraphicsInterrupt(CommandBuffer* buffer)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	RecordTransientLabel32(
	    buffer, nullptr, 0, 0u, nullptr,
	    [](SubmissionId /*submission*/, const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerQueuedGraphicsInterrupt();
		    return true;
	    },
	    nullptr);
}

void GraphicsRenderPrepareWriteBack(CommandBuffer* buffer)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	RecordTransientLabel32(
	    buffer, nullptr, 0, 0u,
	    [](SubmissionId submission, const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    GpuMemoryWriteBackCompletedSubmission(g_render_ctx->GetGraphicCtx(), submission);
		    return true;
	    },
	    nullptr, nullptr);
}

static void eop_event_reset_func(Kernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void eop_event_delete_func(Kernel::EventQueue::KernelEqueue eq, Kernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	if (event->event.filter != Kernel::EventQueue::KERNEL_EVFILT_GRAPHICS) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: event->event.filter != Kernel::EventQueue::KERNEL_EVFILT_GRAPHICS condition ignored (continuing)\n"); }
	// Only EOP-class ids are tracked for TriggerEopEvent; other graphics ids
	// are passive registrations until a producer is wired.
	if (IsGraphicsEopEventId(static_cast<int>(event->event.ident)))
	{
		EXIT_IF(event->filter.data == nullptr);
		g_render_ctx->DeleteEopEqRegistration(event->filter.data, eq, static_cast<int>(event->event.ident));
	}
}

static void eop_event_trigger_func(Kernel::EventQueue::KernelEqueueEvent* event, void* trigger_data)
{
	EXIT_IF(event == nullptr);
	event->triggered = true;
	event->event.fflags++;
	event->event.data = reinterpret_cast<intptr_t>(trigger_data);
}

int GraphicsRenderAddEqEvent(Kernel::EventQueue::KernelEqueue eq, int id, void* udata)
{
	EXIT_IF(g_render_ctx == nullptr);
	auto eq_pin = Kernel::EventQueue::KernelAcquireEqueue(eq);
	if (!eq_pin)
	{
		return Kernel::KERNEL_ERROR_EBADF;
	}
	Core::LockGuard registration_lock(g_render_ctx->GetEopRegistrationMutex());

	// Gen5 registers multiple graphics event idents (0 = queued interrupt,
	// 0x40 = EOP, 0x48 and others observed at device init). Accept any id on the
	// graphics filter; only EOP-class ids are added to the end-of-pipe list.
	Kernel::EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = static_cast<uintptr_t>(id);
	event.event.filter             = Kernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = id;
	event.filter.delete_event_func = eop_event_delete_func;
	event.filter.reset_func        = eop_event_reset_func;
	event.filter.trigger_func      = eop_event_trigger_func;
	void* registration             = nullptr;
	if (IsGraphicsEopEventId(id))
	{
		registration      = g_render_ctx->BeginEopEqRegistration(eq_pin.GetIdentity(), id);
		event.filter.data = registration;
	}

	const int result = Kernel::EventQueue::KernelAddEvent(eq_pin, event);
	if (registration != nullptr)
	{
		if (result == Kernel::OK)
		{
			g_render_ctx->PublishEopEqRegistration(registration);
		} else
		{
			g_render_ctx->CancelEopEqRegistration(registration);
		}
	}
	return result;
}

int GraphicsRenderDeleteEqEvent(Kernel::EventQueue::KernelEqueue eq, int id)
{
	EXIT_IF(g_render_ctx == nullptr);

	return Kernel::EventQueue::KernelDeleteEvent(eq, static_cast<uintptr_t>(id), Kernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
}

GraphicsGdsTransferResult GraphicsRenderClearGds(CommandBuffer* buffer, uint64_t dw_offset, uint64_t dw_count, uint32_t clear_value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(g_render_ctx->GetGdsBuffer() == nullptr);

	Core::LockGuard lock(g_render_ctx->GetMutex());

	return g_render_ctx->GetGdsBuffer()->RecordFill(buffer, g_render_ctx->GetGraphicCtx(), dw_offset, dw_count, clear_value)
	           ? GraphicsGdsTransferResult::Recorded
	           : GraphicsGdsTransferResult::InvalidRange;
}

GraphicsGdsTransferResult GraphicsRenderWriteGdsFromMemory(CommandBuffer* buffer, uint64_t dw_offset, uint64_t src_vaddr, uint64_t dw_count,
                                                           SubmissionId* dependency)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(g_render_ctx->GetGdsBuffer() == nullptr);
	EXIT_IF(dependency == nullptr);
	*dependency = {};
	if (!GraphicsGdsDwordRangeValid(dw_offset, dw_count))
	{
		return GraphicsGdsTransferResult::InvalidRange;
	}
	if (dw_count == 0)
	{
		return GraphicsGdsTransferResult::Recorded;
	}
	const uint64_t bytes = dw_count * 4u;
	if ((src_vaddr & 3u) != 0u || GpuMemoryValidateAllocatedRange(src_vaddr, bytes) != GpuMemoryRangeValidationStatus::Valid)
	{
		return GraphicsGdsTransferResult::InvalidSource;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	GraphicContext* ctx    = g_render_ctx->GetGraphicCtx();
	auto*           gds    = g_render_ctx->GetGdsBuffer();
	const auto      source = GpuMemoryAcquireGdsSource(ctx, buffer, src_vaddr, bytes);
	switch (source.status)
	{
		case GpuMemoryGdsSourceStatus::GuestBytesCurrent:
		{
			// No GPU writer owns the bytes, so the record-time snapshot is the source.
			std::vector<uint32_t> snapshot(static_cast<size_t>(dw_count));
			if (!Core::VirtualMemory::CopyFromGuest(snapshot.data(), src_vaddr, bytes))
			{
				return GraphicsGdsTransferResult::InvalidSource;
			}
			return gds->RecordUpdate(buffer, ctx, dw_offset, snapshot.data(), dw_count)
			           ? GraphicsGdsTransferResult::Recorded
			           : GraphicsGdsTransferResult::InvalidRange;
		}
		case GpuMemoryGdsSourceStatus::DeviceBuffer:
			return gds->RecordCopyFromBuffer(buffer, ctx, source.buffer, source.offset, dw_offset, dw_count)
			           ? GraphicsGdsTransferResult::Recorded
			           : GraphicsGdsTransferResult::InvalidSource;
		case GpuMemoryGdsSourceStatus::ProcessorWriteBackRequired: return GraphicsGdsTransferResult::ProcessorWriteBackRequired;
		case GpuMemoryGdsSourceStatus::SubmissionCompletionRequired:
			*dependency = source.dependency;
			return GraphicsGdsTransferResult::SubmissionCompletionRequired;
		case GpuMemoryGdsSourceStatus::Unsupported: return GraphicsGdsTransferResult::UnsupportedSource;
	}
	return GraphicsGdsTransferResult::UnsupportedSource;
}

GraphicsGdsTransferResult GraphicsRenderCopyGds(CommandBuffer* buffer, uint64_t src_dw_offset, uint64_t dst_dw_offset, uint64_t dw_count)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(g_render_ctx->GetGdsBuffer() == nullptr);

	Core::LockGuard lock(g_render_ctx->GetMutex());

	return g_render_ctx->GetGdsBuffer()->RecordCopy(buffer, g_render_ctx->GetGraphicCtx(), src_dw_offset, dst_dw_offset, dw_count)
	           ? GraphicsGdsTransferResult::Recorded
	           : GraphicsGdsTransferResult::InvalidRange;
}

GraphicsGdsTransferResult GraphicsRenderReadGds(CommandBuffer* buffer, uint32_t* dst, uint64_t dw_offset, uint64_t dw_count)
{
	EXIT_IF(g_render_ctx == nullptr);

	return GdsPublishToGuest(buffer, dw_offset, dst, dw_count);
}

void GraphicsRenderMemoryFree(uint64_t vaddr, uint64_t size)
{
	GpuMemoryFree(g_render_ctx->GetGraphicCtx(), vaddr, size);
}

void GraphicsRenderDeleteIndexBuffers()
{
	IndexBufferDeleteAll(g_render_ctx->GetGraphicCtx());
}

void GraphicsRenderMemoryFlush(uint64_t vaddr, uint64_t size)
{
	GpuMemoryFlush(g_render_ctx->GetGraphicCtx(), vaddr, size);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
