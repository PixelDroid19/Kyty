#include "Emulator/Graphics/GpuSubmissionTracker.h"
#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/TimePort.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <cinttypes>

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
		printf("WARNING: EOP destination range invalid (write skipped)\n");
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

void GraphicsRenderWriteAtEndOfPipeGds32(uint64_t /*submit_id*/, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t dw_offset,
                                         uint32_t dw_num)
{
	EXIT_IF(g_render_ctx == nullptr);
	const uint64_t write_size = std::max<uint64_t>(sizeof(*dst_gpu_addr), static_cast<uint64_t>(dw_num) * sizeof(*dst_gpu_addr));
	if (!ValidateTransientLabelDestination(dst_gpu_addr, write_size))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(dw_offset), static_cast<uint64_t>(dw_num),
	                                 reinterpret_cast<uint64_t>(dst_gpu_addr), 0};

	RecordTransientLabel32(
	    buffer, dst_gpu_addr, 0, dw_num,
	    [](SubmissionId /*submission*/, const uint64_t* args)
	    {
		    auto  dw_offset    = static_cast<uint32_t>(args[0]);
		    auto  dw_num       = static_cast<uint32_t>(args[1]);
		    auto* dst_gpu_addr = reinterpret_cast<uint32_t*>(args[2]);
		    g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), dst_gpu_addr, dw_offset, dw_num);
		    return false;
	    },
	    nullptr, args);
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
		    printf(FG_BRIGHT_GREEN "EndOfPipe Signal!!! [0x%016" PRIx64 "] <- Clock: 0x%016" PRIx64 "\n" FG_DEFAULT,
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
                                                            uint64_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	// A null target is valid for callback-only cache/interrupt packets. Any
	// non-null target still has to be a registered guest allocation.
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
	    [](SubmissionId /*submission*/, const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent();
		    return true;
	    },
	    nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithInterrupt64(uint64_t /*submit_id*/, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (dst_gpu_addr != nullptr && !ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	RecordTransientLabel64(
	    buffer, dst_gpu_addr, value, nullptr,
	    [](SubmissionId /*submission*/, const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent();
		    return true;
	    },
	    nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithInterrupt32(uint64_t /*submit_id*/, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	if (!ValidateTransientLabelDestination(dst_gpu_addr, sizeof(*dst_gpu_addr)))
	{
		return;
	}

	Core::LockGuard lock(g_render_ctx->GetMutex());

	RecordTransientLabel32(
	    buffer, dst_gpu_addr, value, 1u, nullptr,
	    [](SubmissionId /*submission*/, const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent();
		    return true;
	    },
	    nullptr);
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
		    g_render_ctx->TriggerEopEvent();
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

static void eop_event_reset_func(LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void eop_event_delete_func(LibKernel::EventQueue::KernelEqueue eq, LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_NOT_IMPLEMENTED(event->event.filter != LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
	// Only EOP-class ids are tracked for TriggerEopEvent; other graphics ids
	// are passive registrations until a producer is wired.
	if (IsGraphicsEopEventId(static_cast<int>(event->event.ident)))
	{
		EXIT_IF(event->filter.data == nullptr);
		g_render_ctx->DeleteEopEqRegistration(event->filter.data, eq, static_cast<int>(event->event.ident));
	}
}

static void eop_event_trigger_func(LibKernel::EventQueue::KernelEqueueEvent* event, void* trigger_data)
{
	EXIT_IF(event == nullptr);
	event->triggered = true;
	event->event.fflags++;
	event->event.data = reinterpret_cast<intptr_t>(trigger_data);
}

int GraphicsRenderAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata)
{
	EXIT_IF(g_render_ctx == nullptr);
	auto eq_pin = LibKernel::EventQueue::KernelAcquireEqueue(eq);
	if (!eq_pin)
	{
		return LibKernel::KERNEL_ERROR_EBADF;
	}
	Core::LockGuard registration_lock(g_render_ctx->GetEopRegistrationMutex());

	// Gen5 registers multiple graphics event idents (0 = queued interrupt,
	// 0x40 = EOP, 0x48 and others observed at device init). Accept any id on the
	// graphics filter; only EOP-class ids are added to the end-of-pipe list.
	LibKernel::EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = static_cast<uintptr_t>(id);
	event.event.filter             = LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
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

	const int result = LibKernel::EventQueue::KernelAddEvent(eq_pin, event);
	if (registration != nullptr)
	{
		if (result == OK)
		{
			g_render_ctx->PublishEopEqRegistration(registration);
		} else
		{
			g_render_ctx->CancelEopEqRegistration(registration);
		}
	}
	return result;
}

int GraphicsRenderDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id)
{
	EXIT_IF(g_render_ctx == nullptr);

	return LibKernel::EventQueue::KernelDeleteEvent(eq, static_cast<uintptr_t>(id), LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
}

void GraphicsRenderClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(g_render_ctx->GetGdsBuffer() == nullptr);

	g_render_ctx->GetGdsBuffer()->Clear(g_render_ctx->GetGraphicCtx(), dw_offset, dw_num, clear_value);
}

void GraphicsRenderReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(g_render_ctx->GetGdsBuffer() == nullptr);

	g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), dst, dw_offset, dw_size);
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
