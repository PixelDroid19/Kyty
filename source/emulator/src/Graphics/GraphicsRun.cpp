#include "Emulator/Graphics/GraphicsRun.h"

#include "GraphicsComputeRegisters.h"

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/LinkList.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/AsyncJob.h"
#include "Emulator/Graphics/CommandProcessorSubmissionSlots.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GpuSubmissionPublicationGate.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"

#include "GraphicsRunTrace.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#ifdef KYTY_EMU_ENABLED

#include "GraphicsRunInternal.h"

namespace Kyty::Libs::Graphics {

hw_ctx_parser_func_t   g_hw_ctx_func[Pm4::CX_NUM]          = {};
hw_ctx_indirect_func_t g_hw_ctx_indirect_func[Pm4::CX_NUM] = {};
hw_sh_parser_func_t    g_hw_sh_func[Pm4::SH_NUM]           = {};
hw_sh_indirect_func_t  g_hw_sh_indirect_func[Pm4::SH_NUM]  = {};
hw_uc_parser_func_t    g_hw_uc_func[Pm4::UC_NUM]           = {};
hw_uc_indirect_func_t  g_hw_uc_indirect_func[Pm4::UC_NUM]  = {};
hw_sh_parser_func_t    g_hw_sh_custom_func[Pm4::R_NUM]     = {};
cp_op_parser_func_t    g_cp_op_func[256]                   = {};
cp_op_parser_func_t    g_cp_op_custom_func[Pm4::R_NUM]     = {};

Gpu* g_gpu = nullptr;

void GraphicsRunInit()
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	EXIT_IF(g_gpu != nullptr);

	graphics_init_jmp_tables();

	g_gpu = new Gpu;
}

void Gpu::Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw,
                 GraphicsSubmissionCompletion completion)
{
	m_submission_admission_gate.RunAdmitted(
	    [&]
	    {
		    m_gfx_ring->Submit(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw, 0, 0, 0, 0, false, completion);
	    });
}

void Gpu::SubmitAndFlip(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw, int handle,
                        int index, int flip_mode, int64_t flip_arg)
{
	m_submission_admission_gate.RunAdmitted(
	    [&]
	    {
		    m_gfx_ring->Submit(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw, handle, index, flip_mode, flip_arg, true,
		                         GraphicsSubmissionCompletion::None);
	    });
}

uint32_t Gpu::MapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw, uint32_t* read_ptr_addr)
{
	EXIT_NOT_IMPLEMENTED(ring_addr == nullptr);
	EXIT_NOT_IMPLEMENTED(ring_size_dw == 0);
	EXIT_NOT_IMPLEMENTED(read_ptr_addr == nullptr);

	return m_submission_admission_gate.RunAdmitted(
	    [&]
	    {
		    auto v = pipe_id * 8 + queue_id;

		    EXIT_NOT_IMPLEMENTED(v >= 64);

		    //	for (int i = 0; i < 8; i++)
		    //	{
		    //		EXIT_NOT_IMPLEMENTED(m_compute_ring[pipe_id * 8 + i] != nullptr && m_compute_ring[pipe_id * 8 + i]->IsActive());
		    //	}

		    auto* ring = GetRing(v + 1);

		    ring->MapComputeQueue(ring_addr, ring_size_dw, read_ptr_addr);

		    EXIT_IF(!ring->IsActive());

		    *read_ptr_addr = 0;

		    return v + 1;
	    });
}

void Gpu::Unmap(uint32_t ring_id)
{
	m_submission_admission_gate.RunAdmitted(
	    [&]
	    {
		    EXIT_NOT_IMPLEMENTED(ring_id < 1 || ring_id > 64);

		    auto* ring = GetRing(ring_id);

		    EXIT_NOT_IMPLEMENTED(!ring->IsActive());

		    ring->SetActive(false);
	    });
}

void Gpu::DingDong(uint32_t ring_id, uint32_t offset_dw)
{
	m_submission_admission_gate.RunAdmitted(
	    [&]
	    {
		    EXIT_NOT_IMPLEMENTED(ring_id < 1 || ring_id > 64);
		    EXIT_NOT_IMPLEMENTED(offset_dw == 0);

		    auto* ring = GetRing(ring_id);

		    EXIT_NOT_IMPLEMENTED(!ring->IsActive());

		    ring->DingDong(offset_dw);
	    });
}

void Gpu::Done()
{
	m_submission_admission_gate.RunAdmitted(
	    [&]
	    {
		    m_gfx_ring->Done();
		    for (auto& cr: m_compute_ring)
		    {
			    if (cr != nullptr)
			    {
				    cr->Done();
			    }
		    }

		    m_done_num++;
	    });
}

bool Gpu::AreSubmitsAllowed()
{
	return m_submission_admission_gate.RunAdmitted(
	    [&]
	    {
		    if (m_gfx_ring->IsIdle())
		    {
			    for (auto& cr: m_compute_ring)
			    {
				    if (cr != nullptr && !cr->IsIdle())
				    {
					    return false;
				    }
			    }
			    return true;
		    }
		    return false;
	    });
}

int Gpu::GetFrameNum()
{
	return m_done_num;
}

void Gpu::Wait()
{
	m_submission_admission_gate.RunAdmitted([&] { WaitLocked(); });
}

void Gpu::WaitLocked()
{
	m_gfx_ring->WaitForIdle();
	m_gfx_cp->SubmitAndWait();
	for (auto& cr: m_compute_ring)
	{
		if (cr != nullptr)
		{
			cr->WaitForIdle();
		}
	}
	for (auto& cp: m_compute_cp)
	{
		if (cp != nullptr)
		{
			cp->SubmitAndWait();
		}
	}
}

void Gpu::WaitSubmission(SubmissionId submission)
{
	// This is completion of work that already crossed the admission gate, not
	// a new admission. A quiesce drain waits for the ring worker, so taking the
	// gate here would deadlock that worker against WaitForIdle.
	CommandProcessor* owner = nullptr;
	{
		std::lock_guard<std::mutex> topology_lock(m_topology_mutex);
		if (m_gfx_cp->OwnsSubmissionQueue(submission))
		{
			owner = m_gfx_cp;
		} else
		{
			for (auto* cp: m_compute_cp)
			{
				if (cp != nullptr && cp->OwnsSubmissionQueue(submission))
				{
					owner = cp;
					break;
				}
			}
		}
	}
	if (owner != nullptr)
	{
		owner->WaitSubmission(submission);
		return;
	}
	EXIT("GPU submission wait has no owning command processor: queue=%" PRIu32 " sequence=%" PRIu64 "\n", submission.queue.Value(),
	     submission.sequence);
}

bool Gpu::RunQuiesced(GraphicsRunQuiescedAction action, void* data)
{
	EXIT_IF(action == nullptr);
	return m_submission_admission_gate.RunQuiesced([&] { WaitLocked(); }, [&] { return action(data); });
}

void Gpu::Init()
{
	EXIT_IF(m_gfx_cp != nullptr);
	EXIT_IF(m_gfx_ring != nullptr);

	m_gfx_cp   = new CommandProcessor(&m_submission_coordinator, GraphicContext::QUEUE_GFX);
	m_gfx_ring = new GraphicsRing;
	m_gfx_ring->SetCp(m_gfx_cp);

	EXIT_IF(GraphicContext::QUEUE_COMPUTE_NUM < 8);
}

ComputeRing* Gpu::GetRing(uint32_t ring_id)
{
	std::lock_guard<std::mutex> topology_lock(m_topology_mutex);

	int v        = static_cast<int>(ring_id - 1);
	int pipe_id  = v / 8;
	int queue_id = v % 8;

	if (m_compute_cp[pipe_id] == nullptr)
	{
		m_compute_cp[pipe_id] = new CommandProcessor(&m_submission_coordinator, GraphicContext::QUEUE_COMPUTE_START + pipe_id);
	}

	if (m_compute_ring[v] == nullptr)
	{
		m_compute_ring[v] = new ComputeRing;
		m_compute_ring[v]->SetQueueId(queue_id);
		m_compute_ring[v]->SetActive(false);
		m_compute_ring[v]->SetCp(m_compute_cp[pipe_id]);
	}

	return m_compute_ring[v];
}

void CommandProcessor::Reset()
{
	BufferWait();

	Core::LockGuard lock(m_mutex);

	GraphicsRenderDeleteIndexBuffers();

	m_sh_ctx.Reset();
	m_ucfg.Reset();
	m_ctx.Reset();
	m_index_type_and_size = 0;
	m_index_buffer_size   = 0;
	m_index_base_addr     = 0;
	m_user_data_marker    = HW::UserSgprType::Unknown;

	std::memset(m_const_ram, 0, sizeof(m_const_ram));
}

void CommandProcessor::BufferInit()
{
	Core::LockGuard lock(m_mutex);

	if (m_current_buffer < 0)
	{
		for (auto& buf: m_buffer)
		{
			EXIT_IF(buf != nullptr);

			buf = new CommandBuffer(m_queue);
			buf->SetParent(this);
			// buf->SetQueue(m_queue);
		}

		m_current_buffer = 0;
		SubmissionId submission;
		require_submission_success(m_submission_slots.BeginRecording(static_cast<uint32_t>(m_current_buffer), &submission, nullptr),
		                           "BeginRecording", m_queue, static_cast<uint32_t>(m_current_buffer));
		m_buffer[m_current_buffer]->SetSubmissionId(submission);
		m_buffer[m_current_buffer]->Begin();
	}
}

SubmissionId CommandProcessor::BufferFlush()
{
	SubmissionId latest_completed;
	SubmissionId submitted;

	{
		Core::LockGuard lock(m_mutex);
		submitted = SubmitCurrentLocked(&latest_completed);
	}

	PublishCompletedSubmissions();
	WaitUntilPublishedUnlessReentrant(latest_completed);
	return submitted;
}

SubmissionId CommandProcessor::SubmitCurrentLocked(SubmissionId* latest_completed)
{
	EXIT_IF(latest_completed == nullptr);
	DebugStatsRecordBufferFlush();
	TryCompleteSubmittedLocked(latest_completed);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	EXIT_IF(m_buffer[m_current_buffer] == nullptr);

	const uint32_t submitted_slot = static_cast<uint32_t>(m_current_buffer);
	SubmissionId   submitted_id;
	EXIT_NOT_IMPLEMENTED(!m_buffer[submitted_slot]->GetSubmissionId(&submitted_id));
	m_buffer[submitted_slot]->End();
	m_buffer[submitted_slot]->Execute();
	require_submission_success(m_submission_slots.MarkSubmitted(submitted_slot), "MarkSubmitted", m_queue, submitted_slot);
	require_publication_success(m_publication_gate.RegisterSubmitted(submitted_id), "RegisterSubmitted", m_queue, submitted_id);

	const uint32_t recording_slot = (submitted_slot + 1u) % static_cast<uint32_t>(VK_BUFFERS_NUM);
	EXIT_IF(m_buffer[recording_slot] == nullptr);

	TryCompleteSubmittedLocked(latest_completed);
	if (m_buffer[recording_slot]->IsExecute())
	{
		SubmissionId slot_owner;
		EXIT_NOT_IMPLEMENTED(!m_buffer[recording_slot]->GetSubmissionId(&slot_owner));
		CompleteSubmittedThroughLocked(slot_owner, latest_completed);
	}

	SubmissionId recording_id;
	require_submission_success(m_submission_slots.BeginRecording(recording_slot, &recording_id, nullptr), "BeginRecording", m_queue,
	                           recording_slot);
	m_current_buffer = static_cast<int>(recording_slot);
	m_buffer[recording_slot]->SetSubmissionId(recording_id);
	m_buffer[recording_slot]->Begin();
	return submitted_id;
}

void CommandProcessor::TryCompleteSubmittedLocked(SubmissionId* latest_completed)
{
	EXIT_IF(latest_completed == nullptr);

	for (;;)
	{
		uint32_t     slot = 0;
		SubmissionId oldest;
		const auto   result = m_submission_slots.GetOldestSubmitted(&slot, &oldest);
		if (result == GpuSubmissionResult::UnknownSubmission)
		{
			return;
		}
		require_submission_success(result, "GetOldestSubmitted", m_queue, slot);
		EXIT_IF(slot >= static_cast<uint32_t>(VK_BUFFERS_NUM));
		EXIT_IF(!m_buffer[slot]->IsExecute());
		if (!m_buffer[slot]->TryCompleteFenceAndResetWithoutLabelCallbacks())
		{
			return;
		}

		require_submission_success(m_submission_slots.MarkFenceCompleted(slot), "MarkFenceCompleted", m_queue, slot);
		require_publication_success(m_publication_gate.MarkFenceComplete(oldest), "MarkFenceComplete", m_queue, oldest);
		*latest_completed = oldest;
	}
}

void CommandProcessor::CompleteSubmittedThroughLocked(SubmissionId target, SubmissionId* latest_completed)
{
	EXIT_IF(latest_completed == nullptr);
	EXIT_IF(!OwnsSubmissionQueue(target));

	for (;;)
	{
		uint32_t     slot = 0;
		SubmissionId oldest;
		require_submission_success(m_submission_slots.GetOldestSubmitted(&slot, &oldest), "GetOldestSubmitted", m_queue, slot);
		EXIT_IF(slot >= static_cast<uint32_t>(VK_BUFFERS_NUM));
		EXIT_IF(!m_buffer[slot]->IsExecute());

		m_buffer[slot]->WaitForFenceAndResetWithoutLabelCallbacks();
		require_submission_success(m_submission_slots.MarkFenceCompleted(slot), "MarkFenceCompleted", m_queue, slot);
		require_publication_success(m_publication_gate.MarkFenceComplete(oldest), "MarkFenceComplete", m_queue, oldest);
		*latest_completed = oldest;

		if (oldest == target)
		{
			return;
		}
	}
}

void CommandProcessor::PublishCompletedSubmissions()
{
	LabelDrainCompleted();
	for (;;)
	{
		SubmissionId ready;
		const auto   result = m_publication_gate.TryAcquireNextForPublication(&ready);
		if (result == GpuSubmissionPublicationResult::NotReady)
		{
			return;
		}
		require_publication_success(result, "TryAcquireNextForPublication", m_queue, ready);
		LabelCompleteSubmission(ready);
		GpuMemoryCompleteSubmission(ready);
		require_publication_success(m_publication_gate.MarkPublished(ready), "MarkPublished", m_queue, ready);
		require_submission_success(m_submission_slots.RetirePublished(ready), "RetirePublished", m_queue, 0);
	}
}

void CommandProcessor::WaitUntilPublishedUnlessReentrant(SubmissionId submission)
{
	if (submission.sequence == 0 || m_publication_gate.IsPublishingOnCurrentThread())
	{
		return;
	}
	const auto started = std::chrono::steady_clock::now();
	TraceWait("publication_begin", m_queue, 0, 0, 0, 0, submission.sequence);
	require_publication_success(m_publication_gate.WaitUntilPublished(submission), "WaitUntilPublished", m_queue, submission);
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
	TraceWait("publication_end", m_queue, 0, 0, 0, 0, submission.sequence, static_cast<uint64_t>(elapsed));
}

void CommandProcessor::WaitSubmission(SubmissionId submission)
{
	EXIT_IF(!OwnsSubmissionQueue(submission));

	SubmissionId latest_completed;

	{
		Core::LockGuard lock(m_mutex);

		uint32_t           slot  = 0;
		GpuSubmissionState state = GpuSubmissionState::Completed;
		auto               find  = m_submission_slots.FindSlot(submission, &slot);
		if (find == GpuSubmissionResult::Success)
		{
			require_submission_success(m_submission_slots.GetState(submission, &state), "GetState", m_queue, slot);
		}
		if (find == GpuSubmissionResult::Success && state == GpuSubmissionState::Recording)
		{
			EXIT_IF(slot != static_cast<uint32_t>(m_current_buffer));
			(void)SubmitCurrentLocked(&latest_completed);
			find = m_submission_slots.FindSlot(submission, &slot);
			if (find == GpuSubmissionResult::Success)
			{
				require_submission_success(m_submission_slots.GetState(submission, &state), "GetState", m_queue, slot);
			}
		}
		if (find == GpuSubmissionResult::Success)
		{
			EXIT_IF(state != GpuSubmissionState::Submitted || !m_buffer[slot]->IsExecute());
			CompleteSubmittedThroughLocked(submission, &latest_completed);
		} else if (find != GpuSubmissionResult::UnknownSubmission)
		{
			require_submission_success(find, "FindSlot", m_queue, slot);
		}
	}

	PublishCompletedSubmissions();
	WaitUntilPublishedUnlessReentrant(submission);
}

void CommandProcessor::BufferWait()
{
	BufferInit();

	SubmissionId latest_completed;

	{
		Core::LockGuard lock(m_mutex);

		for (;;)
		{
			uint32_t     slot = 0;
			SubmissionId oldest;
			const auto   result = m_submission_slots.GetOldestSubmitted(&slot, &oldest);
			if (result == GpuSubmissionResult::UnknownSubmission)
			{
				break;
			}
			require_submission_success(result, "GetOldestSubmitted", m_queue, slot);
			CompleteSubmittedThroughLocked(oldest, &latest_completed);
		}

		EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
		EXIT_IF(m_buffer[m_current_buffer]->IsExecute());
		SubmissionId recording;
		uint32_t     recording_slot = 0;
		EXIT_NOT_IMPLEMENTED(!m_buffer[m_current_buffer]->GetSubmissionId(&recording));
		require_submission_success(m_submission_slots.FindSlot(recording, &recording_slot), "FindSlot", m_queue,
		                           static_cast<uint32_t>(m_current_buffer));
		EXIT_IF(recording_slot != static_cast<uint32_t>(m_current_buffer));
	}

	PublishCompletedSubmissions();
	WaitUntilPublishedUnlessReentrant(latest_completed);
}

void CommandProcessor::PumpCompletedSubmissions()
{
	BufferInit();

	SubmissionId latest_completed;
	{
		Core::LockGuard lock(m_mutex);
		TryCompleteSubmittedLocked(&latest_completed);
	}
	PublishCompletedSubmissions();
}

void CommandProcessor::SubmitAndWait()
{
	BufferInit();
	BufferFlush();
	BufferWait();
}

void CommandProcessor::ResetDeCe()
{
	m_de_counter.mutex.Lock();
	m_de_counter.value = 0;
	m_de_counter.cond_var.Signal();
	m_de_counter.mutex.Unlock();
	m_ce_counter.mutex.Lock();
	m_ce_counter.value = 0;
	m_ce_counter.cond_var.Signal();
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WaitCe()
{
	m_de_counter.mutex.Lock();
	auto de_value = m_de_counter.value;
	m_de_counter.mutex.Unlock();

	m_ce_counter.mutex.Lock();
	while (!(m_ce_counter.value > de_value))
	{
		m_ce_counter.cond_var.Wait(&m_ce_counter.mutex);
	}
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WaitDeDiff(uint32_t diff)
{
	m_ce_counter.mutex.Lock();
	auto ce_value = m_ce_counter.value;
	m_ce_counter.mutex.Unlock();

	m_de_counter.mutex.Lock();
	while (!(ce_value - m_de_counter.value < diff))
	{
		m_de_counter.cond_var.Wait(&m_de_counter.mutex);
	}
	m_de_counter.mutex.Unlock();
}

void CommandProcessor::IncremenetDe()
{
	BufferFlush();
	BufferWait();

	m_de_counter.mutex.Lock();
	m_de_counter.value++;
	m_de_counter.cond_var.Signal();
	m_de_counter.mutex.Unlock();
}

void CommandProcessor::IncremenetCe()
{
	m_ce_counter.mutex.Lock();
	m_ce_counter.value++;
	m_ce_counter.cond_var.Signal();
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num)
{
	Core::LockGuard lock(m_mutex);

	memcpy(m_const_ram + offset / 4, src, static_cast<size_t>(dw_num) * 4);
}

void CommandProcessor::DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num)
{
	Core::LockGuard lock(m_mutex);

	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(dst), static_cast<size_t>(dw_num) * 4);

	memcpy(dst, m_const_ram + offset / 4, static_cast<size_t>(dw_num) * 4);

	GraphicsRenderMemoryFlush(reinterpret_cast<uint64_t>(dst), static_cast<size_t>(dw_num) * 4);
}

void CommandProcessor::WaitRegMem32(uint32_t func, const uint32_t* addr, uint32_t ref, uint32_t mask, uint32_t poll)
{
	if (addr == nullptr)
	{
		// Guest WAIT_REG_MEM against memory address 0: nothing in the emulated
		// GPU writes there, so the condition can never change. Titles use it as
		// an unconditional marker; submit the preceding callback-only ReleaseMem
		// before continuing instead of dereferencing address 0/1.
		BufferFlush();
		return;
	}
	(void)poll;

	const ScopedDebugStatsTimer wait_timer(DebugStatsRecordWaitRegMem);
	TraceWait("wait32_begin", m_queue, reinterpret_cast<uint64_t>(addr), *addr, ref, mask, m_sumbit_id);
	if (GraphicsWaitRegMemCompare(func, *addr, ref, mask))
	{
		TraceWait("wait32_satisfied", m_queue, reinterpret_cast<uint64_t>(addr), *addr, ref, mask, m_sumbit_id);
		return;
	}

	SubmissionDependency dependency;
	const auto           producer = m_submission_slots.FindPendingProducer(reinterpret_cast<uint64_t>(addr), 4, ref, mask, &dependency);
	SubmissionId         current_submission;
	const bool           has_current_submission = m_current_buffer >= 0 && m_current_buffer < VK_BUFFERS_NUM &&
	                                              m_buffer[m_current_buffer] != nullptr &&
	                                              m_buffer[m_current_buffer]->GetSubmissionId(&current_submission);
	const bool           producer_is_current_submission = has_current_submission && dependency.producer == current_submission;
	EXIT_NOT_IMPLEMENTED(producer != GpuSubmissionResult::Success && producer != GpuSubmissionResult::ProducerValueMismatch &&
	                     producer != GpuSubmissionResult::ProducerNotFound);
	// Submit the command buffer before resolving the dependency so pending EOP
	// labels become visible to the completion publisher.  A matching producer
	// is already ordered by the submission graph; keep that path in the current
	// decode so render-pass continuity is preserved.  Unknown or mismatched
	// producers use a real DCB suspension instead of a CPU poll.
	BufferFlush();
	if (producer == GpuSubmissionResult::Success)
	{
		TraceWait("wait32_producer_begin", m_queue, reinterpret_cast<uint64_t>(addr), *addr, ref, mask,
		          dependency.producer.sequence);
		g_gpu->WaitSubmission(dependency.producer);
		// A producer recorded in this same command buffer is the exact EOP that
		// follows the preceding clear/write in the guest stream. Once its
		// submission has completed, the command stream may continue even if the
		// guest CPU has already reused the backing word for another value. A
		// host-side re-read in that race would incorrectly suspend and replay the
		// render pass after the producer has already been published.
		if (producer_is_current_submission || GraphicsWaitRegMemCompare(func, *addr, ref, mask))
		{
			return;
		}
	}
	TraceWait("wait32_suspended", m_queue, reinterpret_cast<uint64_t>(addr), *addr, ref, mask, m_sumbit_id);
	RequestSuspendedWait(addr, sizeof(uint32_t), func, ref, mask);
	return;
}

void CommandProcessor::WaitRegMem64(uint32_t func, const uint64_t* addr, uint64_t ref, uint64_t mask, uint32_t poll)
{
	if (addr == nullptr)
	{
		// A null address is the encoded form of a guest marker when the preceding
		// ReleaseMem has no destination. There is no memory location to poll, so
		// preserve packet ordering by submitting the callback-only packet and
		// continue without dereferencing address 0/1.
		BufferFlush();
		return;
	}
	(void)poll;

	const ScopedDebugStatsTimer wait_timer(DebugStatsRecordWaitRegMem);
	TraceWait("wait64_begin", m_queue, reinterpret_cast<uint64_t>(addr), *addr, ref, mask, m_sumbit_id);
	if (GraphicsWaitRegMemCompare(func, *addr, ref, mask))
	{
		TraceWait("wait64_satisfied", m_queue, reinterpret_cast<uint64_t>(addr), *addr, ref, mask, m_sumbit_id);
		return;
	}

	SubmissionDependency dependency;
	const auto           producer = m_submission_slots.FindPendingProducer(reinterpret_cast<uint64_t>(addr), 8, ref, mask, &dependency);
	SubmissionId         current_submission;
	const bool           has_current_submission = m_current_buffer >= 0 && m_current_buffer < VK_BUFFERS_NUM &&
	                                              m_buffer[m_current_buffer] != nullptr &&
	                                              m_buffer[m_current_buffer]->GetSubmissionId(&current_submission);
	const bool           producer_is_current_submission = has_current_submission && dependency.producer == current_submission;
	EXIT_NOT_IMPLEMENTED(producer != GpuSubmissionResult::Success && producer != GpuSubmissionResult::ProducerValueMismatch &&
	                     producer != GpuSubmissionResult::ProducerNotFound);
	// Keep a matching producer in the current decode for render-pass continuity;
	// suspend only when the tracker cannot prove that the current value will
	// satisfy this wait.
	BufferFlush();
	if (producer == GpuSubmissionResult::Success)
	{
		TraceWait("wait64_producer_begin", m_queue, reinterpret_cast<uint64_t>(addr), *addr, ref, mask,
		          dependency.producer.sequence);
		g_gpu->WaitSubmission(dependency.producer);
		if (producer_is_current_submission || GraphicsWaitRegMemCompare(func, *addr, ref, mask))
		{
			return;
		}
	}
	TraceWait("wait64_suspended", m_queue, reinterpret_cast<uint64_t>(addr), *addr, ref, mask, m_sumbit_id);
	RequestSuspendedWait(addr, sizeof(uint64_t), func, ref, mask);
	return;
}

bool GraphicsWriteDataPrecedesMatchingWaitMem64(const uint32_t* write_body, uint32_t write_body_dwords, const uint32_t* next_packet,
                                                uint32_t next_packet_dwords)
{
	if (write_body == nullptr || next_packet == nullptr || write_body_dwords != 5u || next_packet_dwords < 9u ||
	    next_packet[0] != 0xc0071058u)
	{
		return false;
	}

	const uint32_t write_confirm = (write_body[0] >> 24u) & 0xffu;
	const uint64_t write_address = write_body[1] | (static_cast<uint64_t>(write_body[2]) << 32u);
	const uint64_t write_value   = write_body[3] | (static_cast<uint64_t>(write_body[4]) << 32u);
	const uint64_t wait_address  = next_packet[1] | (static_cast<uint64_t>(next_packet[2]) << 32u);
	const uint64_t wait_mask     = next_packet[3] | (static_cast<uint64_t>(next_packet[4]) << 32u);
	const uint64_t wait_ref      = next_packet[5] | (static_cast<uint64_t>(next_packet[6]) << 32u);

	return write_confirm == 1u && write_address != 0u && write_address == wait_address &&
	       (write_value & wait_mask) == (wait_ref & wait_mask);
}

GraphicsAgcReleaseMemControl GraphicsDecodeAgcReleaseMemControl(uint32_t control_dw)
{
	GraphicsAgcReleaseMemControl control {};
	control.gcr_cntl  = static_cast<uint16_t>(control_dw & 0xffffu);
	control.data_sel  = static_cast<uint8_t>((control_dw >> 16u) & 0xffu);
	control.interrupt = static_cast<uint8_t>((control_dw >> 24u) & 0x7u);
	return control;
}

uint32_t GraphicsAgcReleaseMemCacheAction(uint16_t gcr_cntl)
{
	constexpr uint16_t GcrGl2Writeback = 1u << 9u;
	return ((gcr_cntl & GcrGl2Writeback) != 0u) ? 0x38u : 0x00u;
}

void CommandProcessor::WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num, uint32_t write_control, bool custom,
                                 bool matching_wait_mem64)
{
	Core::LockGuard lock(m_mutex);

	// Non-custom IT_WRITE_DATA: historical control word.
	if (!custom && write_control != 0x04100500)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: WriteData not supported (continuing)\n");
	}

	// Custom R_WRITE_DATA control = dst | cache_policy<<8 | increment<<16 |
	// write_confirm<<24 (matches GraphicsDcbWriteData). Software CP always
	// performs an immediate host memcpy; cache_policy is accepted when it is a
	// form already emitted by the encoder.
	//   0x01000004 — dst=4, cache_policy=0, write_confirm=1
	//   0x01000204 — dst=4, cache_policy=2, write_confirm=1 (post-Play load)
	if (custom)
	{
		const uint32_t dst_sel       = write_control & 0xffu;
		const uint32_t cache_policy  = (write_control >> 8u) & 0xffu;
		const uint32_t increment     = (write_control >> 16u) & 0xffu;
		const uint32_t write_confirm = (write_control >> 24u) & 0xffu;
		if (dst_sel != 4u || increment != 0u || write_confirm != 1u || (cache_policy != 0u && cache_policy != 2u))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: WriteData not supported (continuing)\n");
		}
	}

	if (matching_wait_mem64)
	{
		EXIT_IF(!custom || dw_num != 2u);
		EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
		const uint64_t value = src[0] | (static_cast<uint64_t>(src[1]) << 32u);
		GraphicsRenderWriteAtEndOfPipe64(m_sumbit_id, m_buffer[m_current_buffer], reinterpret_cast<uint64_t*>(dst), value);
		const auto register_result = m_submission_slots.RegisterProducer(static_cast<uint32_t>(m_current_buffer),
		                                                                 reinterpret_cast<uint64_t>(dst), sizeof(uint64_t), value);
		require_submission_success(register_result, "RegisterProducer", m_queue, static_cast<uint32_t>(m_current_buffer));
		return;
	}

	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(dst), static_cast<size_t>(dw_num) * 4);

	memcpy(dst, src, static_cast<size_t>(dw_num) * 4);

	GraphicsRenderMemoryFlush(reinterpret_cast<uint64_t>(dst), static_cast<size_t>(dw_num) * 4);
}

void GraphicsRing::Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw, int handle,
	                          int index, int flip_mode, int64_t flip_arg, bool with_api_flip, GraphicsSubmissionCompletion completion)
{
	EXIT_IF(m_cp == nullptr);

	const auto decode_completion = completion == GraphicsSubmissionCompletion::QueuedGraphicsInterrupt
	                                   ? std::make_shared<CmdBatch::DecodeCompletion>()
	                                   : nullptr;
	{
		Core::LockGuard lock(m_mutex);

		WindowWaitForGraphicInitialized();
		GraphicsRenderCreateContext();

		if (m_done)
		{
			while (!m_idle)
			{
				m_idle_cond_var.Wait(&m_mutex);
			}
			m_done = false;

			m_cp->Reset();
		}

		// Submission transfers ownership to the ring but does not execute PM4.
		// Capture guest-owned command and indirect-register storage before the
		// producer can recycle its recording arena while this batch is queued.
		CmdBatch buf {};
		buf.draw_buffer         = SnapshotCommandBuffer(cmd_draw_buffer, num_draw_dw);
		buf.const_buffer        = SnapshotCommandBuffer(cmd_const_buffer, num_const_dw);
		buf.flip.handle         = handle;
		buf.flip.index          = index;
		buf.flip.flip_mode      = flip_mode;
		buf.flip.flip_arg       = flip_arg;
		buf.with_api_flip       = with_api_flip;
		buf.completion          = completion;
		buf.decode_completion   = decode_completion;

		m_cmd_batches.Add(buf);

		m_cond_var.Signal();
	}

	if (decode_completion != nullptr)
	{
		decode_completion->Wait();
	}
}

GraphicsRing::CmdBuffer GraphicsRing::SnapshotCommandBuffer(uint32_t* data, uint32_t num_dw)
{
	CmdBuffer result {};
	result.source_data = data;
	result.num_dw      = num_dw;
	if (num_dw == 0)
	{
		EXIT_IF(data != nullptr);
		return result;
	}
	EXIT_IF(data == nullptr);

	auto snapshot = std::make_shared<CmdBuffer::Snapshot>();
	snapshot->words.assign(data, data + num_dw);

	constexpr uint32_t kMaxIndirectRegisters = 4096u;
	uint32_t           offset                = 0;
	while (offset < num_dw)
	{
		const uint32_t header    = snapshot->words[offset];
		const uint32_t remaining = num_dw - offset;
		const uint32_t type      = header >> 30u;
		uint32_t       packet_dw = 1u;

		if (type == 0u || type == 1u)
		{
			packet_dw = header != 0u && remaining >= 2u ? 2u : 1u;
		} else if (type == 3u)
		{
			packet_dw = Pm4::Pm4SpecialType3PacketDwords(header);
			if (packet_dw == 0u)
			{
				packet_dw = KYTY_PM4_LEN(header);
			}
			if (packet_dw > remaining)
			{
				packet_dw = remaining >= 2u ? 2u : 1u;
			}

			const uint32_t custom = KYTY_PM4_R(header);
			const bool indirect_register_packet =
			    header == KYTY_PM4(4, Pm4::IT_NOP, Pm4::R_CX_REGS_INDIRECT) ||
			    header == KYTY_PM4(4, Pm4::IT_NOP, Pm4::R_SH_REGS_INDIRECT) ||
			    header == KYTY_PM4(4, Pm4::IT_NOP, Pm4::R_UC_REGS_INDIRECT);
			if (indirect_register_packet)
			{
				EXIT_IF(packet_dw != 4u || remaining < 4u);
				const uint32_t count = snapshot->words[offset + 1u];
				if (count > kMaxIndirectRegisters)
				{
					EXIT("indirect register packet exceeds the supported bound: class=0x%02" PRIx32 " count=%" PRIu32 "\n",
					     custom, count);
				}
				if (count != 0u)
				{
					const uint64_t address = snapshot->words[offset + 2u] |
					                         (static_cast<uint64_t>(snapshot->words[offset + 3u]) << 32u);
					const uint64_t byte_count = static_cast<uint64_t>(count) * 2u * sizeof(uint32_t);
					if (address == 0u || GpuMemoryValidateAllocatedRange(address, byte_count) != GpuMemoryRangeValidationStatus::Valid)
					{
						EXIT("indirect register packet references invalid guest storage: class=0x%02" PRIx32
						     " address=0x%016" PRIx64 " count=%" PRIu32 "\n",
						     custom, address, count);
					}

					auto block = std::make_shared<std::vector<uint32_t>>(static_cast<size_t>(count) * 2u);
					std::memcpy(block->data(), reinterpret_cast<const void*>(address), static_cast<size_t>(byte_count));
					const uint64_t captured_address = reinterpret_cast<uint64_t>(block->data());
					snapshot->words[offset + 2u]    = static_cast<uint32_t>(captured_address);
					snapshot->words[offset + 3u]    = static_cast<uint32_t>(captured_address >> 32u);
					snapshot->indirect_register_blocks.push_back(std::move(block));
				}
			}

			if (((header >> 8u) & 0xffu) == Pm4::IT_INDIRECT_BUFFER_END)
			{
				break;
			}
		}

		offset += packet_dw;
	}

	result.snapshot = std::move(snapshot);
	result.data     = result.snapshot->words.data();
	return result;
}

void GraphicsRing::Done()
{
	Core::LockGuard lock(m_mutex);
	if (!m_done)
	{
		while (!m_idle)
		{
			m_idle_cond_var.Wait(&m_mutex);
		}
	}
	m_done = true;
}

void GraphicsRing::WaitForIdle()
{
	Core::LockGuard lock(m_mutex);
	while (!m_idle)
	{
		m_idle_cond_var.Wait(&m_mutex);
	}
}

bool GraphicsRing::IsIdle()
{
	Core::LockGuard lock(m_mutex);
	return m_idle;
}

static uint64_t ReadSuspendedRunValue(const CommandProcessor::SuspendedRun& suspended)
{
	EXIT_IF(suspended.address == nullptr || suspended.size == 0);
	const auto address = reinterpret_cast<const volatile uint8_t*>(suspended.address);
	if (suspended.size == sizeof(uint32_t))
	{
		return *reinterpret_cast<const volatile uint32_t*>(address);
	}
	EXIT_IF(suspended.size != sizeof(uint64_t));
	return *reinterpret_cast<const volatile uint64_t*>(address);
}


static bool SuspendedRunReady(const CommandProcessor::SuspendedRun& suspended)
{
	const auto value = ReadSuspendedRunValue(suspended);
	return GraphicsWaitRegMemCompare(suspended.function, value, suspended.reference, suspended.mask);
}

static void WaitForSuspendedRuns(CommandProcessor* cp, CommandProcessor::SuspendedRun* first,
	                             CommandProcessor::SuspendedRun* second = nullptr)
{
	EXIT_IF(cp == nullptr);
	EXIT_IF(first == nullptr && second == nullptr);

	for (;;)
	{
		if ((first != nullptr && SuspendedRunReady(*first)) || (second != nullptr && SuspendedRunReady(*second)))
		{
			return;
		}

		const auto now_ns = SuspendedWaitNowNs();
		const auto timeout_ns = SuspendedWaitTimeoutMs() * 1'000'000ull;
		for (auto* suspended: {first, second})
		{
			if (suspended == nullptr || timeout_ns == 0 || suspended->blocked_since_ns == 0 ||
			    now_ns - suspended->blocked_since_ns < timeout_ns || SuspendedRunReady(*suspended))
			{
				continue;
			}

			// The bounded WAIT_REG_MEM fallback is a liveness guard for a
			// stale/external label, not a memory write: skip only this wait packet
			// and resume at the first downstream packet. Never fabricate the watched
			// value, and keep the default timeout finite so a diagnostic trace cannot
			// pin a command-processor worker forever.
			suspended->skip_wait = true;
			TraceWait("wait_timeout", 0, reinterpret_cast<uint64_t>(suspended->address), ReadSuspendedRunValue(*suspended),
			          suspended->reference, suspended->mask, 0,
			          now_ns - suspended->blocked_since_ns);
			return;
		}
		if (std::getenv("KYTY_WAIT_TRACE") != nullptr)
		{
			const auto& suspended = first != nullptr ? *first : *second;
			KYTY_LOG_LIMIT(Log::Level::Warn, 8,
			               "KYTY_PENDING_WAIT addr=0x%016" PRIx64 " value=0x%016" PRIx64
			               " ref=0x%016" PRIx64 " mask=0x%016" PRIx64 " func=%" PRIu32 " size=%" PRIu32 "\n",
			               reinterpret_cast<uint64_t>(suspended.address), ReadSuspendedRunValue(suspended), suspended.reference,
			               suspended.mask, suspended.function, suspended.size);
		}

		// Completion callbacks for an already submitted command buffer are
		// published by its owning command processor. Briefly reacquire the run
		// lock to drain those callbacks, then yield so another compute queue can
		// produce the watched label.
		cp->RunLock();
		cp->PumpCompletedSubmissions();
		cp->RunUnlock();
		Core::Thread::SleepMicro(1000);
	}
}

static void WaitForSuspendedRun(CommandProcessor* cp, CommandProcessor::SuspendedRun* suspended)
{
	WaitForSuspendedRuns(cp, suspended);
}

GraphicsRing::CmdBatch GraphicsRing::GetCmdBatch()
{
	Core::LockGuard lock(m_mutex);

	while (m_cmd_batches.Size() == 0)
	{
		m_idle = true;
		m_idle_cond_var.Signal();

		// Do not dump guest slot table from ring-idle: GetCmdBatch idles before
		// guest BSS is mapped, and mincore is not a reliable guard on macOS /
		// Rosetta (SLOT_IDLE_DUMP segfaulted during init). Soft-lock dumps live
		// in KernelNanosleep under KYTY_SLOT_TRACE once the guest sleep-polls.

		m_cond_var.Wait(&m_mutex);
	}

	m_idle = false;

	auto first = m_cmd_batches.First();

	CmdBatch buf = m_cmd_batches.At(first);

	m_cmd_batches.Remove(first);

	return buf;
}

void GraphicsRing::ThreadBatchRun(void* data)
{
	EXIT_IF(data == nullptr);

	static std::atomic_uint64_t seq = 0;

	auto* ring = static_cast<GraphicsRing*>(data);
	auto* cp   = ring->m_cp;

	EXIT_IF(ring == nullptr);
	EXIT_IF(cp == nullptr);

	for (;;)
	{
		CmdBatch buf = ring->GetCmdBatch();

		cp->RunLock();
		{
			cp->BufferInit();
			cp->ResetDeCe();
			cp->SetFlip(buf.flip);
			cp->SetSumbitId(++seq);

			struct ScheduledStream
			{
				AsyncJob*                      job       = nullptr;
				CmdBuffer                      command;
				CommandProcessor::SuspendedRun suspended;
				bool                           blocked   = false;
				bool                           completed = false;
			};

			ScheduledStream constant {&ring->m_job1, buf.const_buffer};
			ScheduledStream draw {&ring->m_job2, buf.draw_buffer};

			auto run_ready_stream = [&](ScheduledStream* stream)
			{
				EXIT_IF(stream == nullptr || stream->job == nullptr);
				if (stream->completed)
				{
					return false;
				}
				if (stream->blocked)
				{
					if (!stream->suspended.skip_wait && !SuspendedRunReady(stream->suspended))
					{
						return false;
					}
					stream->command.data   = stream->suspended.skip_wait ? stream->suspended.resume_data : stream->suspended.data;
					stream->command.num_dw = stream->suspended.skip_wait ? stream->suspended.resume_num_dw : stream->suspended.num_dw;
					stream->suspended      = {};
					stream->blocked        = false;
				}

				const auto command = stream->command;
				stream->job->Execute(
				    [cp, command](void* /*unused*/) { cp->Run(command.data, command.num_dw, command.source_data); });
				stream->job->Wait();
				stream->command.source_data = nullptr;

				if (cp->TakeSuspendedRun(&stream->suspended))
				{
					stream->blocked = true;
				} else
				{
					stream->completed = true;
				}
				return true;
			};

			while (!constant.completed || !draw.completed)
			{
				bool progressed = run_ready_stream(&constant);
				progressed      = run_ready_stream(&draw) || progressed;
				if (progressed)
				{
					continue;
				}

				auto* constant_wait = constant.blocked ? &constant.suspended : nullptr;
				auto* draw_wait     = draw.blocked ? &draw.suspended : nullptr;
				EXIT_IF(constant_wait == nullptr && draw_wait == nullptr);
				cp->RunUnlock();
				WaitForSuspendedRuns(cp, constant_wait, draw_wait);
				cp->RunLock();
			}
			if (buf.completion == GraphicsSubmissionCompletion::QueuedGraphicsInterrupt)
			{
				cp->QueueQueuedGraphicsInterrupt();
			}

			SubmissionId flip_submission = cp->BufferFlush();

			// SubmitAndFlip carries flip args on the batch. DCBs often embed
			// R_FLIP / marker 0x777 (which sets m_flip_issued); when they do not,
			// the API flip must still run or VideoOutSubmitFlip never happens
			// (empty Flip queue → guest ThreadFlag bit 0x1 soft-lock).
			if (GraphicsBatchNeedsApiFlip(buf.with_api_flip, cp->FlipIssued()))
			{
				cp->Flip();
				flip_submission = cp->BufferFlush();
			}
			if (buf.decode_completion != nullptr)
			{
				buf.decode_completion->Signal();
			}
			if (GraphicsBatchNeedsSubmissionCompletion(cp->CompletionCallbackIssued()))
			{
				cp->WaitSubmission(flip_submission);
			}
		}
		cp->RunUnlock();
	}
}

void ComputeRing::ThreadRun(void* data)
{
	EXIT_IF(data == nullptr);

	auto* ring = static_cast<ComputeRing*>(data);
	auto* cp   = ring->m_cp;

	EXIT_IF(ring == nullptr);
	EXIT_IF(cp == nullptr);

	uint32_t* buffer = nullptr;

	KYTY_PROFILER_THREAD("Thread_Compute");

	ring->m_mutex.Lock();

	for (;;)
	{
		while (!(ring->m_active && ring->m_run_offset_dw > 0))
		{
			ring->m_idle = true;
			ring->m_idle_cond_var.Signal();
			ring->m_cond_var.Wait(&ring->m_mutex);
		}

		ring->m_idle = false;

		auto  pos       = *ring->m_read_ptr_addr;
		auto  num_dw    = ring->m_run_offset_dw;
		auto  next_pos  = pos + num_dw;
		auto  ring_size = ring->m_ring_size_dw;
		auto* ring_addr = ring->m_ring_addr;

		if (next_pos <= ring_size)
		{
			buffer = ring_addr + pos;
		} else
		{
			auto d1 = next_pos - ring_size;
			auto d2 = num_dw - d1;
			buffer  = ring->m_internal_buffer;
			memcpy(buffer, ring_addr + pos, d2);
			memcpy(buffer + d2, ring_addr, d1);
		}

		ring->m_mutex.Unlock();

		cp->RunLock();
		{
			cp->BufferInit();
			cp->ResetDeCe();

			GraphicsDbgDumpDcb("cc", num_dw, buffer);

			uint32_t*       run_data        = buffer;
			const uint32_t* run_source_data = buffer;
			uint32_t        run_num_dw      = num_dw;
			for (;;)
			{
				cp->Run(run_data, run_num_dw, run_source_data);

				CommandProcessor::SuspendedRun suspended;
				if (!cp->TakeSuspendedRun(&suspended))
				{
					break;
				}

				cp->RunUnlock();
				WaitForSuspendedRun(cp, &suspended);
				cp->RunLock();
				run_data   = suspended.skip_wait ? suspended.resume_data : suspended.data;
				run_num_dw = suspended.skip_wait ? suspended.resume_num_dw : suspended.num_dw;
				run_source_data = nullptr;
			}

			cp->BufferFlush();
		}
		cp->RunUnlock();

		ring->m_mutex.Lock();

		ring->m_run_offset_dw -= num_dw;
		*ring->m_read_ptr_addr = next_pos % ring_size;
	}
}

void ComputeRing::MapComputeQueue(uint32_t* ring_addr, uint32_t ring_size_dw, uint32_t* read_ptr_addr)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_active);

	if (m_ring_size_dw != ring_size_dw)
	{
		delete[] m_internal_buffer;
		m_internal_buffer = new uint32_t[ring_size_dw];
	}

	m_ring_addr               = ring_addr;
	m_ring_size_dw            = ring_size_dw;
	m_read_ptr_addr           = read_ptr_addr;
	m_run_offset_dw           = 0;
	m_last_dingdong_offset_dw = 0;

	*m_read_ptr_addr = 0;
	m_active         = true;

	m_cond_var.Signal();
}

void ComputeRing::DingDong(uint32_t offset_dw)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(!m_active);

	WindowWaitForGraphicInitialized();
	GraphicsRenderCreateContext();

	if (m_done)
	{
		while (!m_idle)
		{
			m_idle_cond_var.Wait(&m_mutex);
		}
		m_done = false;
		/*m_cp->Reset();*/
	}

	uint32_t advance = (offset_dw + (offset_dw < m_last_dingdong_offset_dw ? m_ring_size_dw : 0)) - m_last_dingdong_offset_dw;

	m_run_offset_dw += advance;

	m_last_dingdong_offset_dw = offset_dw;

	m_cond_var.Signal();
}

void ComputeRing::Done()
{
	Core::LockGuard lock(m_mutex);
	if (!m_done)
	{
		while (!m_idle)
		{
			m_idle_cond_var.Wait(&m_mutex);
		}
	}
	m_done = true;
}

void ComputeRing::WaitForIdle()
{
	Core::LockGuard lock(m_mutex);
	while (!m_idle)
	{
		m_idle_cond_var.Wait(&m_mutex);
	}
}

bool ComputeRing::IsIdle()
{
	Core::LockGuard lock(m_mutex);
	return m_idle;
}

void ComputeRing::SetActive(bool flag)
{
	Core::LockGuard lock(m_mutex);

	m_active = flag;

	m_cond_var.Signal();
}

bool ComputeRing::IsActive()
{
	Core::LockGuard lock(m_mutex);

	return m_active;
}

void CommandProcessor::Run(uint32_t* data, uint32_t num_dw, const uint32_t* source_data)
{
	KYTY_PROFILER_BLOCK("CommandProcessor::Run");
	const uint32_t* const previous_run_begin = m_active_run_begin;
	const uint32_t* const previous_run_end   = m_active_run_end;
	m_active_run_begin                       = data;
	m_active_run_end                         = data != nullptr ? data + num_dw : nullptr;

	if (source_data != nullptr && num_dw > 0)
	{
		GraphicsRenderMemoryFree(reinterpret_cast<uint64_t>(source_data), static_cast<size_t>(num_dw) * 4);
	}

	auto* cmd = data;
	auto  dw  = num_dw;
	for (;;)
	{
		if (dw == 0)
		{
			break;
		}

		EXIT_NOT_IMPLEMENTED(dw > num_dw);

		auto cmd_id = *cmd++;
		dw -= 1;

		// PM4 packet type in bits [31:30]:
		//   3 = Type3 (IT_* opcodes) — primary path
		//   2 = Type2 NOP padding — header only
		//   0 = Type0 register write — header + 1 body dword (single register)
		//   1 = Type1 — classical GCN multi-reg; on Gen5 titles observed only as
		//       fixed 2-dword units (COUNT ignored), same sizing as Type0.
		// Observed post-Play stream: a run of Type0 single-register writes
		// (including header 0x01fe0000) immediately before WaitFlipDone.
		// Gen5 Type1 pairs (e.g. 0x7d0703e0 / 0x7d070440) interleave
		// with Type0 before WaitFlipDone — consume 2 dwords to stay aligned.
		// Also: headers with type bits 11 but COUNT that cannot fit the buffer
		// (e.g. 0xf84d2e90) — same 2-dword align units, not real IT_* packets.
		const uint32_t pkt_type                   = cmd_id >> 30u;
		const uint32_t remaining_including_header = dw + 1u;
		if (pkt_type != 3u)
		{
			if (pkt_type == 2u)
			{
				// Type2 filler: no body.
				continue;
			}
			if (pkt_type == 0u || pkt_type == 1u)
			{
				if (cmd_id == 0u)
				{
					// Zero dwords are padding. Do not consume the following
					// dword as a Type0 body; it can be a real Type3 header.
					continue;
				}
				// Type0/Type1 single-body form: always one body dword.
				// Classical GCN COUNT is ignored until multi-reg bodies are
				// evidenced with a bounded size that matches the stream.
				if (dw < 1)
				{
					// Trailing Type0/1 header with no body at the end of the
					// buffer: padding, nothing to apply.
					continue;
				}
				cmd += 1;
				dw -= 1;
				continue;
			}
			{
				const uint32_t at = num_dw - dw - 1u;
				KYTY_LOG_LIMIT(Log::Level::Warn, 8,
				               "unknown PM4 packet type %u (cmd_id=0x%08" PRIx32 ") at dw=0x%05" PRIx32 " num_dw=0x%05" PRIx32 "\n",
				               pkt_type, cmd_id, at, num_dw);
			}
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown PM4 packet type (continuing)\n");
		}

		const uint32_t special_packet_dwords = Pm4::Pm4SpecialType3PacketDwords(cmd_id);
		if (special_packet_dwords != 0u)
		{
			EXIT_NOT_IMPLEMENTED(remaining_including_header < special_packet_dwords);
		} else if (KYTY_PM4_LEN(cmd_id) > remaining_including_header)
		{
			if (dw < 1)
			{
				// Oversized packet at the very end of the buffer: padding.
				continue;
			}
			cmd += 1;
			dw -= 1;
			continue;
		}

		EXIT_NOT_IMPLEMENTED(dw < 1);

		auto op = (cmd_id >> 8u) & 0xffu;

		auto pfunc = g_cp_op_func[op];

		if (pfunc == nullptr)
		{
			const auto* packet_start = cmd - 1;
			if (Config::IsNextGen() && Pm4::Pm4Gen5OpaquePairPrecedesWaitFlipDone(packet_start, remaining_including_header))
			{
				EXIT_NOT_IMPLEMENTED(dw < 1);
				cmd += 1;
				dw -= 1;
				continue;
			}
			const uint32_t at = num_dw - dw - 1u;
			KYTY_LOG_LIMIT(Log::Level::Warn, 8,
			               "unknown op at dw=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " num_dw=0x%05" PRIx32 "\n", at, cmd_id, num_dw);
			uint32_t begin = 0;
			uint32_t end   = (num_dw < 32u) ? num_dw : 32u;
			if (at >= 32u)
			{
				begin = (at > 32u) ? (at - 32u) : 0u;
				end   = ((at + 24u) < num_dw) ? (at + 24u) : num_dw;
			}
			for (uint32_t i = begin; i < end; i++)
			{
				KYTY_LOG_LIMIT(Log::Level::Warn, 8, "\t %05" PRIx32 "%s 0x%08" PRIx32 "\n", i, (i == at) ? " <<<" : "    ", data[i]);
			}
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown PM4 op (continuing)\n");
		}

		auto s = pfunc(this, cmd_id, cmd, dw + 1, num_dw);

		if (m_suspend_run_requested)
		{
			// The wait packet itself must be replayed after its watched value is
			// genuinely written. Keep both the packet start and the first packet
			// after it: the latter is used only by the bounded liveness fallback.
			// Commands before it were submitted by WaitRegMem32/64.
			m_suspended_run.data            = cmd - 1;
			m_suspended_run.num_dw          = dw + 1u;
			m_suspended_run.resume_data     = cmd + s;
			m_suspended_run.resume_num_dw   = dw - s;
			m_suspended_run.blocked_since_ns = SuspendedWaitNowNs();
			m_suspended_run.skip_wait       = false;
			m_suspended_run_valid           = true;
			m_suspend_run_requested         = false;
			break;
		}

		// printf("\t %05" PRIx32 ": %u\n", num_dw - dw - 1, s);

		cmd += s;
		dw -= s;
	}

	m_active_run_begin = previous_run_begin;
	m_active_run_end   = previous_run_end;
}

bool CommandProcessor::TakeSuspendedRun(SuspendedRun* run)
{
	EXIT_IF(run == nullptr);
	if (!m_suspended_run_valid)
	{
		return false;
	}
	*run                  = m_suspended_run;
	m_suspended_run       = {};
	m_suspended_run_valid = false;
	return true;
}

void CommandProcessor::SetIndexType(uint32_t index_type_and_size)
{
	Core::LockGuard lock(m_mutex);

	m_index_type_and_size = index_type_and_size;
}

void CommandProcessor::SetIndexBaseAddress(uint64_t index_base_addr)
{
	Core::LockGuard lock(m_mutex);

	m_index_base_addr = index_base_addr;
}

void CommandProcessor::SetIndexBufferSize(uint32_t index_buffer_size)
{
	Core::LockGuard lock(m_mutex);

	m_index_buffer_size = index_buffer_size;
}

void CommandProcessor::SetIndirectArgsBaseAddress(uint32_t base_index, uint64_t address)
{
	Core::LockGuard lock(m_mutex);

	switch (base_index)
	{
		case 0: m_draw_indirect_args_base_addr = address; break;
		case 1: m_dispatch_indirect_args_base_addr = address; break;
		default: EXIT_NOT_IMPLEMENTED(true); break;
	}

	if (std::getenv("KYTY_DUMP_INDIRECT") != nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Debug, 64, "KYTY_DUMP_INDIRECT set_base index=%u addr=0x%012" PRIx64 "\n", base_index, address);
	}
}

void CommandProcessor::SetNumInstances(uint32_t num_instances)
{
	Core::LockGuard lock(m_mutex);

	if (num_instances == 0)
	{
		num_instances = 1;
	}

	m_num_instances = num_instances;

	EXIT_NOT_IMPLEMENTED(m_num_instances != 1);
}

void CommandProcessor::DrawIndex(uint32_t index_count, const void* index_addr, uint64_t draw_modifier, uint32_t type)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (std::getenv("KYTY_DUMP_DRAW") != nullptr)
	{
		static uint32_t logs = 0;
		if (logs < 48u)
		{
			++logs;
			KYTY_LOG_DEBUG(
			             "KYTY_DUMP_DRAW_INDEX count=%u modifier=0x%016" PRIx64 " type=%u index_addr=0x%012" PRIx64
			             " index_base=0x%012" PRIx64 " index_buf_size=%u index_type=%u\n",
			             index_count, draw_modifier, type, reinterpret_cast<uint64_t>(index_addr), m_index_base_addr,
			             m_index_buffer_size, m_index_type_and_size);
		}
	}

	GraphicsRenderDrawIndex(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_ucfg, &m_sh_ctx, m_index_type_and_size, index_count,
	                        index_addr, draw_modifier, type);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count, uint32_t flags)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	EXIT_NOT_IMPLEMENTED(m_index_base_addr == 0);
	EXIT_NOT_IMPLEMENTED((flags & ~0xE0000001u) != 0);

	uint32_t index_bytes = 0;
	switch (m_index_type_and_size)
	{
		case 0: index_bytes = 2; break;
		case 1: index_bytes = 4; break;
		default: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown index_type_and_size %u (continuing)\n", m_index_type_and_size); break;
	}

	auto* index_addr = reinterpret_cast<void*>(m_index_base_addr + static_cast<uint64_t>(index_offset) * index_bytes);
	GraphicsRenderDrawIndex(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_ucfg, &m_sh_ctx, m_index_type_and_size, index_count,
	                        index_addr, flags, 1);
}

void CommandProcessor::DrawIndexIndirect(uint32_t data_offset, uint32_t initiator)
{
	struct DrawIndexedIndirectArgs
	{
		uint32_t index_count_per_instance;
		uint32_t instance_count;
		uint32_t start_index_location;
		uint32_t base_vertex_location;
		uint32_t start_instance_location;
	};

	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);
	EXIT_NOT_IMPLEMENTED(m_index_base_addr == 0);
	EXIT_NOT_IMPLEMENTED((initiator & ~0x20u) != 2u);

	DrawIndexedIndirectArgs args {};
	memcpy(&args, reinterpret_cast<const void*>(m_draw_indirect_args_base_addr + data_offset), sizeof(args));
	if (args.index_count_per_instance == 0 || args.instance_count == 0)
	{
		if (std::getenv("KYTY_DUMP_INDIRECT") != nullptr)
		{
			static uint32_t logs = 0;
			if (logs < 64u)
			{
				++logs;
				KYTY_LOG_DEBUG(
				             "KYTY_DUMP_INDIRECT draw_index_skip offset=0x%08" PRIx32 " count=%u instances=%u base=0x%012" PRIx64
				             "\n",
				             data_offset, args.index_count_per_instance, args.instance_count, m_draw_indirect_args_base_addr);
			}
		}
		return;
	}

	uint32_t index_bytes = 0;
	switch (m_index_type_and_size)
	{
		case 0: index_bytes = 2; break;
		case 1: index_bytes = 4; break;
		case 2: index_bytes = 1; break;
		default: EXIT_NOT_IMPLEMENTED(true); break;
	}

	const uint32_t index_count =
	    (m_index_buffer_size != 0 && args.index_count_per_instance > m_index_buffer_size) ? m_index_buffer_size
	                                                                                     : args.index_count_per_instance;
	auto*          index_addr  = reinterpret_cast<void*>(m_index_base_addr + static_cast<uint64_t>(args.start_index_location) * index_bytes);

	if (std::getenv("KYTY_DUMP_INDIRECT") != nullptr)
	{
		static uint32_t logs = 0;
		if (logs < 64u)
		{
			++logs;
			KYTY_LOG_DEBUG(
			             "KYTY_DUMP_INDIRECT draw_index offset=0x%08" PRIx32 " count=%u instances=%u start_index=%u base_vertex=%u "
			             "first_instance=%u initiator=0x%08" PRIx32 "\n",
			             data_offset, index_count, args.instance_count, args.start_index_location, args.base_vertex_location,
			             args.start_instance_location, initiator);
		}
	}

	GraphicsRenderDrawIndex(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_ucfg, &m_sh_ctx, m_index_type_and_size, index_count,
	                        index_addr, 0, 1);
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	GraphicsRenderDispatchDirect(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_sh_ctx, thread_group_x, thread_group_y, thread_group_z,
	                             mode);
}

void CommandProcessor::DispatchIndirect(uint32_t data_offset, uint32_t mode)
{
	struct DispatchIndirectArgs
	{
		uint32_t thread_group_x;
		uint32_t thread_group_y;
		uint32_t thread_group_z;
	};

	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	EXIT_NOT_IMPLEMENTED(m_dispatch_indirect_args_base_addr == 0);

	DispatchIndirectArgs args {};
	memcpy(&args, reinterpret_cast<const void*>(m_dispatch_indirect_args_base_addr + data_offset), sizeof(args));
	if (args.thread_group_x == 0 || args.thread_group_y == 0 || args.thread_group_z == 0)
	{
		if (std::getenv("KYTY_DUMP_INDIRECT") != nullptr)
		{
			static uint32_t logs = 0;
			if (logs < 64u)
			{
				++logs;
				KYTY_LOG_DEBUG(
				             "KYTY_DUMP_INDIRECT dispatch_skip offset=0x%08" PRIx32 " dims=%ux%ux%u base=0x%012" PRIx64 "\n",
				             data_offset, args.thread_group_x, args.thread_group_y, args.thread_group_z,
				             m_dispatch_indirect_args_base_addr);
			}
		}
		return;
	}

	if (std::getenv("KYTY_DUMP_INDIRECT") != nullptr)
	{
		static uint32_t logs = 0;
		if (logs < 64u)
		{
			++logs;
			KYTY_LOG_DEBUG( "KYTY_DUMP_INDIRECT dispatch offset=0x%08" PRIx32 " dims=%ux%ux%u mode=0x%08" PRIx32 "\n",
			             data_offset, args.thread_group_x, args.thread_group_y, args.thread_group_z, mode);
		}
	}

	GraphicsRenderDispatchDirect(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_sh_ctx, args.thread_group_x, args.thread_group_y,
	                             args.thread_group_z, mode);
}

void CommandProcessor::DrawIndexAuto(uint32_t index_count, uint64_t draw_modifier)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (std::getenv("KYTY_DUMP_DRAW") != nullptr)
	{
		static uint32_t auto_logs = 0;
		if (auto_logs < 48u)
		{
			++auto_logs;
			KYTY_LOG_DEBUG(
			             "KYTY_DUMP_DRAW_AUTO count=%u modifier=0x%016" PRIx64 " index_base=0x%012" PRIx64
			             " index_buf_size=%u index_type=%u",
			             index_count, draw_modifier, m_index_base_addr, m_index_buffer_size, m_index_type_and_size);
			if (m_index_base_addr != 0 && index_count > 0 && index_count <= 64)
			{
				KYTY_LOG_DEBUG( " idx=");
				if (m_index_type_and_size == 0)
				{
					const auto* idx = reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(m_index_base_addr));
					for (uint32_t i = 0; i < index_count; i++)
					{
						KYTY_LOG_DEBUG( "%s%u", (i ? "," : ""), static_cast<unsigned>(idx[i]));
					}
				} else if (m_index_type_and_size == 1)
				{
					const auto* idx = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(m_index_base_addr));
					for (uint32_t i = 0; i < index_count; i++)
					{
						KYTY_LOG_DEBUG( "%s%u", (i ? "," : ""), idx[i]);
					}
				}
			}
			KYTY_LOG_DEBUG( "\n");
		}
	}

	GraphicsRenderDrawIndexAuto(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_ucfg, &m_sh_ctx, index_count, draw_modifier);
}

void CommandProcessor::ClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value)
{
	Core::LockGuard lock(m_mutex);

	GraphicsRenderClearGds(dw_offset, dw_num, clear_value);
}

void CommandProcessor::ReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size)
{
	Core::LockGuard lock(m_mutex);

	GraphicsRenderReadGds(dst, dw_offset, dw_size);
}

void CommandProcessor::WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index)
{
	const ScopedDebugStatsTimer wait_timer(DebugStatsRecordWaitFlipDone);

	SubmissionId submission;
	{
		Core::LockGuard lock(m_mutex);
		EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
		EXIT_NOT_IMPLEMENTED(!m_buffer[m_current_buffer]->GetSubmissionId(&submission));
	}
	BufferFlush();
	g_gpu->WaitSubmission(submission);

	VideoOut::VideoOutWaitFlipDone(static_cast<int>(video_out_handle), static_cast<int>(display_buffer_index));
}

void CommandProcessor::WriteAtEndOfPipe32(uint32_t cache_policy, uint32_t event_write_dest, uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source, void* dst_gpu_addr, uint32_t value,
                                          uint32_t interrupt_selector)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (Log::ShouldLog(Log::Level::Debug))
	{
		printf("CommandProcessor::WriteAtEndOfPipe32()\n");
		printf("\t cache_policy        = 0x%08" PRIx32 "\n", cache_policy);
		printf("\t event_write_dest    = 0x%08" PRIx32 "\n", event_write_dest);
		printf("\t eop_event_type      = 0x%08" PRIx32 "\n", eop_event_type);
		printf("\t cache_action        = 0x%08" PRIx32 "\n", cache_action);
		printf("\t event_index         = 0x%08" PRIx32 "\n", event_index);
		printf("\t event_write_source  = 0x%08" PRIx32 "\n", event_write_source);
		printf("\t interrupt_selector  = 0x%08" PRIx32 "\n", interrupt_selector);
		printf("\t dst_gpu_addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		printf("\t value               = 0x%08" PRIx32 "\n", value);
	}

	EXIT_NOT_IMPLEMENTED(cache_policy != 0x00000000);
	EXIT_NOT_IMPLEMENTED(event_write_dest != 0x00000000);
	EXIT_NOT_IMPLEMENTED(interrupt_selector != 0x0);

	if (event_write_source == 0x00000002 && eop_event_type == 0x0000002f && cache_action == 0x00000000 && event_index == 0x00000006)
	{
		GraphicsRenderWriteAtEndOfPipe32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value);
		const auto register_result = m_submission_slots.RegisterProducer(static_cast<uint32_t>(m_current_buffer),
		                                                                 reinterpret_cast<uint64_t>(dst_gpu_addr), 4, value);
		require_submission_success(register_result, "RegisterProducer", m_queue, static_cast<uint32_t>(m_current_buffer));
	} else if (event_write_source == 0x00000001 && eop_event_type == 0x0000002f && cache_action == 0x00000000 && event_index == 0x00000006)
	{
		GraphicsRenderWriteAtEndOfPipeGds32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value & 0xffffu,
		                                    value >> 16u);
	} else
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown event type (continuing)\n");
	}
}

void CommandProcessor::WriteAtEndOfPipe64(uint32_t cache_policy, uint32_t event_write_dest, uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source, void* dst_gpu_addr, uint64_t value,
                                          uint32_t interrupt_selector)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (Log::ShouldLog(Log::Level::Debug))
	{
		printf("CommandProcessor::WriteAtEndOfPipe64()\n");
		printf("\t cache_policy        = 0x%08" PRIx32 "\n", cache_policy);
		printf("\t event_write_dest    = 0x%08" PRIx32 "\n", event_write_dest);
		printf("\t eop_event_type      = 0x%08" PRIx32 "\n", eop_event_type);
		printf("\t cache_action        = 0x%08" PRIx32 "\n", cache_action);
		printf("\t event_index         = 0x%08" PRIx32 "\n", event_index);
		printf("\t event_write_source  = 0x%08" PRIx32 "\n", event_write_source);
		printf("\t interrupt_selector  = 0x%08" PRIx32 "\n", interrupt_selector);
		printf("\t dst_gpu_addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		printf("\t value               = 0x%016" PRIx64 "\n", value);
	}

	EXIT_NOT_IMPLEMENTED(cache_policy != 0x00000000);
	EXIT_NOT_IMPLEMENTED(event_write_dest != 0x00000000);

	bool     with_interrupt = false;
	bool     source64       = (event_write_source == 0x02);
	bool     source32       = (event_write_source == 0x01);
	bool     source_counter = (event_write_source == 0x04);
	uint32_t producer_size  = 0;

	switch (interrupt_selector)
	{
		case 0x00:
		case 0x03: with_interrupt = false; break;
		case 0x02: with_interrupt = true; break;
		default: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown interrupt_selector (continuing)\n"); break;
	}

	if (dst_gpu_addr == nullptr)
	{
		if (eop_event_type == 0x14 && cache_action == 0x00 && event_index == 0x00 && source32 && !with_interrupt && value == 0)
		{
			// CACHE_FLUSH_AND_INV_TS without a destination has no guest memory
			// write to publish; preserve the ordering side effect only.
			GraphicsRenderMemoryBarrier(m_buffer[m_current_buffer]);
			return;
		}
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unsupported ReleaseMem null destination (continuing)\n");
	}

	if (eop_event_type == 0x04 && cache_action == 0x00 && event_index == 0x05 && source64 && !with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipe64(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint64_t*>(dst_gpu_addr), value);
		producer_size = 8;
	} else if (eop_event_type == 0x04 && cache_action == 0x00 && event_index == 0x05 && source32 && !with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipe32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value);
		producer_size = 4;
	} else if (((eop_event_type == 0x14 && event_index == 0x00) || (eop_event_type == 0x30 && event_index == 0x00) ||
	            (eop_event_type == 0x2f && event_index == 0x00)) &&
	           cache_action == 0x00 && source32 && !with_interrupt)
	{
		// ReleaseMem data_sel=1: 32-bit immediate label write.
		// Event type varies by flush/completion form; the write is driven by
		// data_sel. Observed: 0x14 (CACHE_FLUSH_AND_INV_TS), 0x30, 0x2f@0.
		GraphicsRenderWriteAtEndOfPipe32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr),
		                                 static_cast<uint32_t>(value));
		producer_size = 4;
	} else if (((eop_event_type == 0x04 && (event_index == 0x00 || event_index == 0x05)) ||
	            (eop_event_type == 0x28 && (event_index == 0x00 || event_index == 0x05)) ||
	            (eop_event_type == 0x2f && event_index == 0x06) || (eop_event_type == 0x14 && event_index == 0x00) ||
	            (eop_event_type == 0x30 && event_index == 0x00) || (eop_event_type == 0x2f && event_index == 0x00)) &&
	           cache_action == 0x38 && source64)
	{
		if (with_interrupt)
		{
			GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack64(m_sumbit_id, m_buffer[m_current_buffer],
			                                                       static_cast<uint64_t*>(dst_gpu_addr), value);
		} else
		{
			GraphicsRenderWriteAtEndOfPipeWithWriteBack64(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint64_t*>(dst_gpu_addr),
			                                              value);
		}
		producer_size = 8;
	} else if (((eop_event_type == 0x04 && event_index == 0x05) || (eop_event_type == 0x28 && event_index == 0x00)) &&
	           cache_action == 0x00 && source_counter && !with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipeClockCounter(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint64_t*>(dst_gpu_addr));
	} else if ((eop_event_type == 0x04 && event_index == 0x05) && cache_action == 0x00 && source64 && with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipeWithInterrupt64(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint64_t*>(dst_gpu_addr), value);
		producer_size = 8;
	} else if ((eop_event_type == 0x04 && event_index == 0x05) && cache_action == 0x00 && source32 && with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipeWithInterrupt32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value);
		producer_size = 4;
	} else if ((eop_event_type == 0x04 && event_index == 0x05) && cache_action == 0x3b && source64 && with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack64(m_sumbit_id, m_buffer[m_current_buffer],
		                                                       static_cast<uint64_t*>(dst_gpu_addr), value);
		producer_size = 8;
	} else
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown event type (continuing)\n");
	}

	const bool valid_producer_destination =
		producer_size != 0 && dst_gpu_addr != nullptr &&
		GpuMemoryValidateAllocatedRange(reinterpret_cast<uint64_t>(dst_gpu_addr), producer_size) ==
			GpuMemoryRangeValidationStatus::Valid;
	if (valid_producer_destination)
	{
		const auto register_result = m_submission_slots.RegisterProducer(static_cast<uint32_t>(m_current_buffer),
		                                                                 reinterpret_cast<uint64_t>(dst_gpu_addr), producer_size, value);
		require_submission_success(register_result, "RegisterProducer", m_queue, static_cast<uint32_t>(m_current_buffer));
	}
	if (with_interrupt)
	{
		m_completion_callback_issued = true;
	}
}

void CommandProcessor::MemoryBarrier()
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	GraphicsRenderMemoryBarrier(m_buffer[m_current_buffer]);
}

void CommandProcessor::RenderTextureBarrier(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	GraphicsRenderRenderTextureBarrier(m_buffer[m_current_buffer], vaddr, size);
}

void CommandProcessor::DepthStencilBarrier(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	GraphicsRenderDepthStencilBarrier(m_buffer[m_current_buffer], vaddr, size);
}

void CommandProcessor::TriggerEvent(uint32_t event_type, uint32_t event_index)
{
	Core::LockGuard lock(m_mutex);

	if (Log::ShouldLog(Log::Level::Debug))
	{
		printf("CommandProcessor::TriggerEvent()\n");
		printf("\t event_type  = 0x%08" PRIx32 "\n", event_type);
		printf("\t event_index = 0x%08" PRIx32 "\n", event_index);
	}

	if ((event_type == 0x00000016 || event_type == 0x00000031) && event_index == 0x00000007)
	{
		// CacheFlushAndInvEvent
		// FlushAndInvalidateCbPixelData
		MemoryBarrier();
	} else if (event_type == 0x0000002c && (event_index == 0x00000000 || event_index == 0x00000007))
	{
		// FLUSH_AND_INV_DB_META — index 7 was already accepted; post-Play also
		// emits index 0 on EVENT_WRITE.
		MemoryBarrier();
	} else if (event_type == 0x0000002e && (event_index == 0x00000000 || event_index == 0x00000007))
	{
		// FLUSH_AND_INV_CB_META — observed post-Play EVENT_WRITE (index 0).
		// Ensure color-target metadata/data are coherent for later samples.
		MemoryBarrier();
	} else if ((event_type == 0x00000010) && event_index == 0x00000000)
	{
		// PsPartialFlush
	} else if (event_type == 0x00000007 && event_index == 0x00000000)
	{
		// CS_PARTIAL_FLUSH — wait for outstanding compute work. Treat as a barrier.
		MemoryBarrier();
	} else
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown event type (continuing)\n");
	}
}

void CommandProcessor::Flip()
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (Log::ShouldLog(Log::Level::Debug))
	{
		printf("CommandProcessor::Flip()\n");
	}

	GraphicsRenderWriteAtEndOfPipeOnlyFlip(m_sumbit_id, m_buffer[m_current_buffer], m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                       m_flip.flip_arg);
	m_flip_issued                = true;
	m_completion_callback_issued = true;
}

void CommandProcessor::Flip(void* dst_gpu_addr, uint32_t value)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (Log::ShouldLog(Log::Level::Debug))
	{
		printf("CommandProcessor::Flip()\n");
		printf("\t dst_gpu_addr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		printf("\t value        = 0x%08" PRIx32 "\n", value);
	}

	GraphicsRenderWriteAtEndOfPipeWithFlip32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value,
	                                         m_flip.handle, m_flip.index, m_flip.flip_mode, m_flip.flip_arg);
	m_flip_issued                = true;
	m_completion_callback_issued = true;
}

void CommandProcessor::FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action, void* dst_gpu_addr, uint32_t value)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (Log::ShouldLog(Log::Level::Debug))
	{
		printf("CommandProcessor::FlipWithInterrupt()\n");
		printf("\t eop_event_type      = 0x%08" PRIx32 "\n", eop_event_type);
		printf("\t cache_action        = 0x%08" PRIx32 "\n", cache_action);
		printf("\t dst_gpu_addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		printf("\t value               = 0x%08" PRIx32 "\n", value);
	}

	if (eop_event_type == 0x00000004 && cache_action == 0x00000038)
	{
		GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBackFlip32(m_sumbit_id, m_buffer[m_current_buffer],
		                                                           static_cast<uint32_t*>(dst_gpu_addr), value, m_flip.handle, m_flip.index,
		                                                           m_flip.flip_mode, m_flip.flip_arg);
		m_flip_issued                = true;
		m_completion_callback_issued = true;
	} else
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unknown event type (continuing)\n");
	}
}

void CommandProcessor::QueueQueuedGraphicsInterrupt()
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	GraphicsRenderQueueQueuedGraphicsInterrupt(m_buffer[m_current_buffer]);
	m_completion_callback_issued = true;
}

void CommandProcessor::WriteBack()
{
	{
		Core::LockGuard lock(m_mutex);
		EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
		GraphicsRenderPrepareWriteBack(m_buffer[m_current_buffer]);
	}

	// The completion-only label runs in the WriteBack phase before this
	// submission is published or its resources become retirement-eligible.
	BufferFlush();
	BufferWait();
}

// void CommandBuffer::CommandProcessorWait()
//{
//	EXIT_IF(m_parent == nullptr);
//
//	m_parent->BufferWait();
//}

void GraphicsRunSubmit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw,
                       GraphicsSubmissionCompletion completion)
{
	EXIT_IF(cmd_draw_buffer == nullptr);
	EXIT_IF(num_draw_dw == 0);
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Submit(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw, completion);
}

void GraphicsRunSubmitAndFlip(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw,
                              int handle, int index, int flip_mode, int64_t flip_arg)
{
	EXIT_IF(cmd_draw_buffer == nullptr);
	EXIT_IF(num_draw_dw == 0);
	EXIT_IF(g_gpu == nullptr);

	g_gpu->SubmitAndFlip(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw, handle, index, flip_mode, flip_arg);
}

void GraphicsRunDingDong(uint32_t ring_id, uint32_t offset_dw)
{
	EXIT_IF(g_gpu == nullptr);

	g_gpu->DingDong(ring_id, offset_dw);
}

uint32_t GraphicsRunMapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw,
                                    uint32_t* read_ptr_addr)
{
	EXIT_IF(g_gpu == nullptr);

	return g_gpu->MapComputeQueue(pipe_id, queue_id, ring_addr, ring_size_dw, read_ptr_addr);
}

void GraphicsRunUnmapComputeQueue(uint32_t id)
{
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Unmap(id);
}

void GraphicsRunWait()
{
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Wait();
}

void GraphicsRunDone()
{
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Done();
}

bool GraphicsRunAreSubmitsAllowed()
{
	EXIT_IF(g_gpu == nullptr);

	return g_gpu->AreSubmitsAllowed();
}

bool GraphicsRunWithQuiescedSubmissions(GraphicsRunQuiescedAction action, void* data)
{
	EXIT_IF(g_gpu == nullptr);
	EXIT_IF(action == nullptr);

	return g_gpu->RunQuiesced(action, data);
}

int GraphicsRunGetFrameNum()
{
	EXIT_IF(g_gpu == nullptr);

	return g_gpu->GetFrameNum();
}

void GraphicsRunCommandProcessorFlush(CommandProcessor* cp)
{
	EXIT_IF(cp == nullptr);

	cp->BufferFlush();
}

void GraphicsRunCommandProcessorWait(CommandProcessor* cp)
{
	EXIT_IF(cp == nullptr);

	cp->BufferWait();
}

static void graphics_init_jmp_tables_cx_indirect()
{
	for (auto& func: g_hw_ctx_indirect_func)
	{
		func = nullptr;
	}

	g_hw_ctx_indirect_func[Pm4::PA_SC_GENERIC_SCISSOR_TL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ State::SetGenericScissorTl(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_GENERIC_SCISSOR_BR] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ State::SetGenericScissorBr(*cp->GetCtx(), value); };
	// Screen scissor is handled as a TL+BR pair on the direct SET_CONTEXT_REG path;
	// Gen5 CX-indirect emits the halves separately.
	g_hw_ctx_indirect_func[Pm4::PA_SC_SCREEN_SCISSOR_TL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ State::SetScreenScissorTl(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_SCREEN_SCISSOR_BR] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ State::SetScreenScissorBr(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::PA_SU_SC_MODE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetModeControl(*cp->GetCtx(), value); };
	for (uint32_t reg = Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL; reg <= Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET; reg++)
	{
		g_hw_ctx_indirect_func[reg] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetPolygonOffsetRegister(*cp->GetCtx(), cmd_offset, value); };
	}
	// Gen5 CX-indirect emits DB_RENDER_CONTROL (offset 0) as a lone pair; direct
	// SET_CONTEXT_REG already uses the same decoder.
	g_hw_ctx_indirect_func[Pm4::DB_RENDER_CONTROL]  = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetRenderControl(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetStencilControl(*cp->GetCtx(), value); };
	// Direct SET_CONTEXT_REG writes REFMASK+REFMASK_BF as a 2-dword pair; Gen5
	// CX-indirect emits each half alone.
	g_hw_ctx_indirect_func[Pm4::DB_STENCILREFMASK]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetStencilRefMask(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::DB_STENCILREFMASK_BF] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetStencilRefMaskBf(*cp->GetCtx(), value); };
	// Direct path registers CB_BLEND0..7; indirect must share the same decoder
	// (captured: CB_BLEND1_CONTROL = 0x1e1 after post-menu load).
	for (uint32_t slot = 0; slot < 8; slot++)
	{
		const uint32_t blend_reg          = Pm4::CB_BLEND0_CONTROL + slot;
		g_hw_ctx_indirect_func[blend_reg] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			const uint32_t blend_slot = cmd_offset - Pm4::CB_BLEND0_CONTROL;
			State::SetBlendControl(*cp->GetCtx(), blend_slot, value);
		};
	}

	for (auto cmd_offset = Pm4::SPI_PS_INPUT_CNTL_0; cmd_offset <= Pm4::SPI_PS_INPUT_CNTL_31; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = cmd_offset - Pm4::SPI_PS_INPUT_CNTL_0;
			cp->GetCtx()->SetPsInputSettings(slot, value);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE; cmd_offset <= Pm4::CB_COLOR7_BASE; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).base;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorBase(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).base;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorBase(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_VIEW; cmd_offset <= Pm4::CB_COLOR7_VIEW; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t      slot = (cmd_offset - Pm4::CB_COLOR0_VIEW) / 15;
			HW::ColorView view;
			view.base_array_slice_index = KYTY_PM4_GET(value, CB_COLOR0_VIEW, SLICE_START);
			view.last_array_slice_index = KYTY_PM4_GET(value, CB_COLOR0_VIEW, SLICE_MAX);
			view.current_mip_level      = KYTY_PM4_GET(value, CB_COLOR0_VIEW, MIP_LEVEL);
			cp->GetCtx()->SetColorView(slot, view);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_INFO; cmd_offset <= Pm4::CB_COLOR7_INFO; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			const uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_INFO) / 15;
			cp->GetCtx()->SetColorInfo(slot, State::DecodeColorInfo(value, Config::IsNextGen()));
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB; cmd_offset <= Pm4::CB_COLOR7_ATTRIB; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t        slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB) / 15;
			if (slot == 0)
			{
				trace_aa_register_write("indirect", "CB_COLOR0_ATTRIB", value);
			}
			HW::ColorAttrib attrib;
			attrib.force_dest_alpha_to_one = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, FORCE_DST_ALPHA_1) != 0;
			attrib.tile_mode               = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, TILE_MODE_INDEX);
			attrib.fmask_tile_mode         = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, FMASK_TILE_MODE_INDEX);
			attrib.num_samples             = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_SAMPLES);
			attrib.num_fragments           = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_FRAGMENTS);
			cp->GetCtx()->SetColorAttrib(slot, attrib);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_CONTROL; cmd_offset <= Pm4::CB_COLOR7_DCC_CONTROL; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t            slot = (cmd_offset - Pm4::CB_COLOR0_DCC_CONTROL) / 15;
			HW::ColorDccControl dcc;
			dcc.overwrite_combiner_disable     = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, OVERWRITE_COMBINER_DISABLE) != 0;
			dcc.dcc_clear_key_enable           = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, KEY_CLEAR_ENABLE) != 0;
			dcc.max_uncompressed_block_size    = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_UNCOMPRESSED_BLOCK_SIZE);
			dcc.min_compressed_block_size      = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MIN_COMPRESSED_BLOCK_SIZE);
			dcc.max_compressed_block_size      = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_COMPRESSED_BLOCK_SIZE);
			dcc.color_transform                = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, COLOR_TRANSFORM);
			dcc.independent_64b_blocks         = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_64B_BLOCKS) != 0;
			dcc.data_write_on_dcc_clear_to_reg = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, ENABLE_CONSTANT_ENCODE_REG_WRITE) != 0;
			dcc.independent_128b_blocks        = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_128B_BLOCKS) != 0;
			cp->GetCtx()->SetColorDccControl(slot, dcc);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CMASK; cmd_offset <= Pm4::CB_COLOR7_CMASK; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_CMASK) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).cmask;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorCmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CMASK_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_CMASK_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_CMASK_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).cmask;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorCmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_FMASK; cmd_offset <= Pm4::CB_COLOR7_FMASK; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_FMASK) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).fmask;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorFmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_FMASK_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_FMASK_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_FMASK_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).fmask;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorFmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CLEAR_WORD0; cmd_offset <= Pm4::CB_COLOR7_CLEAR_WORD0; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			HW::ColorClearWord0 clear_word0;
			clear_word0.word0 = value;
			uint32_t slot     = (cmd_offset - Pm4::CB_COLOR0_CLEAR_WORD0) / 15;
			cp->GetCtx()->SetColorClearWord0(slot, clear_word0);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CLEAR_WORD1; cmd_offset <= Pm4::CB_COLOR7_CLEAR_WORD1; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			HW::ColorClearWord1 clear_word1;
			clear_word1.word1 = value;
			uint32_t slot     = (cmd_offset - Pm4::CB_COLOR0_CLEAR_WORD1) / 15;
			cp->GetCtx()->SetColorClearWord1(slot, clear_word1);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_BASE; cmd_offset <= Pm4::CB_COLOR7_DCC_BASE; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_DCC_BASE) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).dcc_addr;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorDccAddr(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_DCC_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_DCC_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).dcc_addr;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorDccAddr(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB2; cmd_offset <= Pm4::CB_COLOR7_ATTRIB2; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t         slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB2);
			HW::ColorAttrib2 attrib2;
			attrib2.height         = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB2, MIP0_HEIGHT);
			attrib2.width          = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB2, MIP0_WIDTH);
			attrib2.num_mip_levels = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB2, MAX_MIP);
			cp->GetCtx()->SetColorAttrib2(slot, attrib2);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB3; cmd_offset <= Pm4::CB_COLOR7_ATTRIB3; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t         slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB3);
			HW::ColorAttrib3 attrib3;
			attrib3.depth              = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, MIP0_DEPTH);
			attrib3.tile_mode          = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, COLOR_SW_MODE);
			attrib3.dimension          = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, RESOURCE_TYPE);
			attrib3.cmask_pipe_aligned = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, CMASK_PIPE_ALIGNED);
			attrib3.dcc_pipe_aligned   = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, DCC_PIPE_ALIGNED);
			cp->GetCtx()->SetColorAttrib3(slot, attrib3);
		};
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_XSCALE; cmd_offset <= Pm4::PA_CL_VPORT_XSCALE_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportXScale((cmd_offset - Pm4::PA_CL_VPORT_XSCALE) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_XOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_XOFFSET_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportXOffset((cmd_offset - Pm4::PA_CL_VPORT_XOFFSET) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_YSCALE; cmd_offset <= Pm4::PA_CL_VPORT_YSCALE_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportYScale((cmd_offset - Pm4::PA_CL_VPORT_YSCALE) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_YOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_YOFFSET_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportYOffset((cmd_offset - Pm4::PA_CL_VPORT_YOFFSET) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_ZSCALE; cmd_offset <= Pm4::PA_CL_VPORT_ZSCALE_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZScale((cmd_offset - Pm4::PA_CL_VPORT_ZSCALE) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_ZOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_ZOFFSET_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZOffset((cmd_offset - Pm4::PA_CL_VPORT_ZOFFSET) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	// Guard-band adj floats written one-at-a-time via IT_SET_CONTEXT_REG
	// indirect (Gen5 AGC). Bulk four-dword form is handled by hw_ctx_set_guard_bands.
	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_VERT_CLIP_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(vp.guard_band_horz_clip, *reinterpret_cast<const float*>(&value), vp.guard_band_horz_discard,
		                            vp.guard_band_vert_discard);
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_VERT_DISC_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(vp.guard_band_horz_clip, vp.guard_band_vert_clip, vp.guard_band_horz_discard,
		                            *reinterpret_cast<const float*>(&value));
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_HORZ_CLIP_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(*reinterpret_cast<const float*>(&value), vp.guard_band_vert_clip, vp.guard_band_horz_discard,
		                            vp.guard_band_vert_discard);
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_HORZ_DISC_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(vp.guard_band_horz_clip, vp.guard_band_vert_clip, *reinterpret_cast<const float*>(&value),
		                            vp.guard_band_vert_discard);
	};

	// Single-dword forms of bulk CX parsers (Gen5 indirect set path).
	g_hw_ctx_indirect_func[Pm4::PA_SU_HARDWARE_SCREEN_OFFSET] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const uint32_t x = KYTY_PM4_GET(value, PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_X);
		const uint32_t y = KYTY_PM4_GET(value, PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_Y);
		cp->GetCtx()->SetHardwareScreenOffset(x, y);
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_CLIP_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		HW::ClipControl r;
		r.user_clip_planes                    = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, UCP_ENA);
		r.user_clip_plane_mode                = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, PS_UCP_MODE);
		r.dx_clip_space                       = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF) != 0;
		r.vertex_kill_any                     = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, VTX_KILL_OR) != 0;
		r.min_z_clip_disable                  = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE) != 0;
		r.max_z_clip_disable                  = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE) != 0;
		r.user_clip_plane_negate_y            = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, PS_UCP_Y_SCALE_NEG) != 0;
		r.clip_disable                        = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, CLIP_DISABLE) != 0;
		r.user_clip_plane_cull_only           = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, UCP_CULL_ONLY_ENA) != 0;
		r.cull_on_clipping_error_disable      = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, DIS_CLIP_ERR_DETECT) != 0;
		r.linear_attribute_clip_enable        = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA) != 0;
		r.force_viewport_index_from_vs_enable = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE) != 0;
		cp->GetCtx()->SetClipControl(r);
	};
	g_hw_ctx_indirect_func[Pm4::CB_COLOR_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		trace_aa_register_write("indirect", "CB_COLOR_CONTROL", value);
		HW::ColorControl r;
		r.mode = KYTY_PM4_GET(value, CB_COLOR_CONTROL, MODE);
		r.op   = KYTY_PM4_GET(value, CB_COLOR_CONTROL, ROP3);
		cp->GetCtx()->SetColorControl(r);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_LINE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto line_width = KYTY_PM4_GET(value, PA_SU_LINE_CNTL, WIDTH);
		cp->GetCtx()->SetLineWidth(line_width == 8 ? 1.0f : static_cast<float>(line_width) / 8.0f);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_AA_CONFIG] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		trace_aa_register_write("indirect", "PA_SC_AA_CONFIG", value);
		HW::AaConfig r;
		r.msaa_num_samples      = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, MSAA_NUM_SAMPLES);
		r.aa_mask_centroid_dtmn = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, AA_MASK_CENTROID_DTMN) != 0;
		r.max_sample_dist       = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, MAX_SAMPLE_DIST);
		r.msaa_exposed_samples  = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, MSAA_EXPOSED_SAMPLES);
		cp->GetCtx()->SetAaConfig(r);
	};
	g_hw_ctx_indirect_func[Pm4::DB_EQAA] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		trace_aa_register_write("indirect", "DB_EQAA", value);
		HW::EqaaControl r;
		r.max_anchor_samples         = KYTY_PM4_GET(value, DB_EQAA, MAX_ANCHOR_SAMPLES);
		r.ps_iter_samples            = KYTY_PM4_GET(value, DB_EQAA, PS_ITER_SAMPLES);
		r.mask_export_num_samples    = KYTY_PM4_GET(value, DB_EQAA, MASK_EXPORT_NUM_SAMPLES);
		r.alpha_to_mask_num_samples  = KYTY_PM4_GET(value, DB_EQAA, ALPHA_TO_MASK_NUM_SAMPLES);
		r.high_quality_intersections = KYTY_PM4_GET(value, DB_EQAA, HIGH_QUALITY_INTERSECTIONS) != 0;
		r.incoherent_eqaa_reads      = KYTY_PM4_GET(value, DB_EQAA, INCOHERENT_EQAA_READS) != 0;
		r.interpolate_comp_z         = KYTY_PM4_GET(value, DB_EQAA, INTERPOLATE_COMP_Z) != 0;
		r.static_anchor_associations = KYTY_PM4_GET(value, DB_EQAA, STATIC_ANCHOR_ASSOCIATIONS) != 0;
		cp->GetCtx()->SetEqaaControl(r);
	};
	g_hw_ctx_indirect_func[Pm4::CB_BLEND_RED] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto color = cp->GetCtx()->GetBlendColor();
		color.red  = *reinterpret_cast<const float*>(&value);
		cp->GetCtx()->SetBlendColor(color);
	};
	g_hw_ctx_indirect_func[Pm4::CB_BLEND_GREEN] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto color  = cp->GetCtx()->GetBlendColor();
		color.green = *reinterpret_cast<const float*>(&value);
		cp->GetCtx()->SetBlendColor(color);
	};
	g_hw_ctx_indirect_func[Pm4::CB_BLEND_BLUE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto color = cp->GetCtx()->GetBlendColor();
		color.blue = *reinterpret_cast<const float*>(&value);
		cp->GetCtx()->SetBlendColor(color);
	};
	g_hw_ctx_indirect_func[Pm4::CB_BLEND_ALPHA] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto color  = cp->GetCtx()->GetBlendColor();
		color.alpha = *reinterpret_cast<const float*>(&value);
		cp->GetCtx()->SetBlendColor(color);
	};

	for (uint32_t sample = 0; sample < 16u; sample++)
	{
		g_hw_ctx_indirect_func[Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 + sample] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			auto control                                                           = cp->GetCtx()->GetAaSampleControl();
			control.locations[cmd_offset - Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0] = value;
			cp->GetCtx()->SetAaSampleControl(control);
		};
	}

	// Host-irrelevant GPU metadata / modes that Kyty accepts without state
	// (no guest-visible Vulkan mapping yet). Accept to keep PM4 streams moving.
	const auto ignore_cx = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		(void)cp;
		(void)cmd_offset;
		(void)value;
	};
	g_hw_ctx_indirect_func[Pm4::CB_DCC_CONTROL]               = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_COUNT_CONTROL]             = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_RENDER_OVERRIDE]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_RENDER_OVERRIDE2]          = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DFSM_CONTROL]              = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_RMI_L2_CACHE_CONTROL]      = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::CB_RMI_GL2_CACHE_CONTROL]     = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::TA_BC_BASE_ADDR]              = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::TA_BC_BASE_ADDR_HI]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SU_POINT_SIZE]             = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SU_POINT_MINMAX]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::SPI_TMPRING_SIZE]             = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_MULTI_PRIM_IB_RESET_INDX] = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_DRAW_PAYLOAD_CNTL]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_PRIMITIVEID_RESET]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_CL_OBJPRIM_ID_CNTL]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_FOV_WINDOW_LR]          = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_FOV_WINDOW_TB]          = ignore_cx;
	// PA_SC_FSR_ENABLE / FSR_RECURSIONS* use host-only fake offsets
	// (0x800003FC..) outside the CX table; bulk path only.
	g_hw_ctx_indirect_func[Pm4::PA_SC_MODE_CNTL_1]                     = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_AA_MASK_X0Y0_X1Y0]               = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_AA_MASK_X0Y1_X1Y1]               = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SU_VTX_CNTL]                        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PS_SHADER_SAMPLE_EXCLUSION_MASK]       = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_BINNER_CNTL_0]                   = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_BINNER_CNTL_1]                   = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_CONSERVATIVE_RASTERIZATION_CNTL] = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_NGG_MODE_CNTL]                   = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_ALPHA_TO_MASK]                      = ignore_cx;
	// Window scissor/offset and tessellation stage regs need full Context
	// fields (Kyty). Accept values for now so Gen5 bootstreams proceed;
	// geometry that depends on them will need the proper setters later.
	g_hw_ctx_indirect_func[Pm4::PA_SC_WINDOW_OFFSET]     = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetWindowOffset(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_WINDOW_SCISSOR_TL] = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_WINDOW_SCISSOR_BR] = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_HOS_MAX_TESS_LEVEL]  = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_HOS_MIN_TESS_LEVEL]  = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_PRIMITIVEID_EN]      = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_REUSE_OFF]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_TESS_DISTRIBUTION]   = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_LS_HS_CONFIG]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_TF_PARAM]            = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_BOUNDS_MIN]     = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_BOUNDS_MAX]     = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_HTILE_SURFACE]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_INFO]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SIZE]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SLICE]          = ignore_cx;
	// Legacy EPITCH fields. GFX10 Vulkan depth resources derive their geometry
	// from the image descriptor rather than these GFX9-era context registers.
	g_hw_ctx_indirect_func[Pm4::DB_Z_INFO2]                = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_INFO2]          = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_CENTROID_PRIORITY_0] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto r              = cp->GetCtx()->GetAaSampleControl();
		r.centroid_priority = (r.centroid_priority & 0xffffffff00000000ull) | value;
		cp->GetCtx()->SetAaSampleControl(r);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_CENTROID_PRIORITY_1] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto r              = cp->GetCtx()->GetAaSampleControl();
		r.centroid_priority = (r.centroid_priority & 0x00000000ffffffffull) | (static_cast<uint64_t>(value) << 32u);
		cp->GetCtx()->SetAaSampleControl(r);
	};

	for (auto cmd_offset = Pm4::PA_SC_VPORT_SCISSOR_0_TL; cmd_offset <= Pm4::PA_SC_VPORT_SCISSOR_15_TL; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			int  left                  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, TL_X)));
			int  top                   = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, TL_Y)));
			bool window_offset_disable = KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, WINDOW_OFFSET_DISABLE) != 0;
			cp->GetCtx()->SetViewportScissorTL((cmd_offset - Pm4::PA_SC_VPORT_SCISSOR_0_TL) / 2, left, top, !window_offset_disable);
		};
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_SCISSOR_0_BR; cmd_offset <= Pm4::PA_SC_VPORT_SCISSOR_15_BR; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			int right  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_BR, BR_X)));
			int bottom = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_BR, BR_Y)));
			cp->GetCtx()->SetViewportScissorBR((cmd_offset - Pm4::PA_SC_VPORT_SCISSOR_0_BR) / 2, right, bottom);
		};
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_ZMIN_0; cmd_offset <= Pm4::PA_SC_VPORT_ZMIN_15; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZMin((cmd_offset - Pm4::PA_SC_VPORT_ZMIN_0) / 2, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_ZMAX_0; cmd_offset <= Pm4::PA_SC_VPORT_ZMAX_15; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZMax((cmd_offset - Pm4::PA_SC_VPORT_ZMAX_0) / 2, *reinterpret_cast<const float*>(&value)); };
	}

	g_hw_ctx_indirect_func[Pm4::SPI_VS_OUT_CONFIG] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetVsOutConfig(value); };

	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_POS_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderPosFormat(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_IDX_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderIdxFormat(value); };
	g_hw_ctx_indirect_func[Pm4::PA_CL_VS_OUT_CNTL]     = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetClVsOutCntl(value); };
	g_hw_ctx_indirect_func[Pm4::GE_NGG_SUBGRP_CNTL]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetNggSubgrpCntl(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_INSTANCE_CNT]   = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsInstanceCnt(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_ONCHIP_CNTL]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsOnchipCntl(value); };

	g_hw_ctx_indirect_func[Pm4::GE_MAX_OUTPUT_PER_SUBGROUP] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetMaxOutputPerSubgroup(value); };

	g_hw_ctx_indirect_func[Pm4::VGT_ESGS_RING_ITEMSIZE] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetEsgsRingItemsize(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_MAX_VERT_OUT]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsMaxVertOut(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_SHADER_STAGES_EN]   = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderStages(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_OUT_PRIM_TYPE]   = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsOutPrimType(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_Z_FORMAT]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderZFormat(value); };

	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_COL_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		for (uint32_t i = 0; i < 8; i++)
		{
			cp->GetCtx()->SetTargetOutputMode(i, (value >> (i * 4)) & 0xFu);
		}
	};

	g_hw_ctx_indirect_func[Pm4::SPI_PS_INPUT_ENA]  = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetPsInputEna(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_PS_INPUT_ADDR] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetPsInputAddr(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_PS_IN_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetPsInControl(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_BARYC_CNTL]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetBarycCntl(value); };

	g_hw_ctx_indirect_func[Pm4::DB_SHADER_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthShaderControl db_shader_control {};
		db_shader_control.other_bits                  = value & 0xFFFF9B8Eu;
		db_shader_control.conservative_z_export_value = KYTY_PM4_GET(value, DB_SHADER_CONTROL, CONSERVATIVE_Z_EXPORT);
		db_shader_control.shader_z_behavior           = KYTY_PM4_GET(value, DB_SHADER_CONTROL, Z_ORDER);
		db_shader_control.shader_kill_enable          = KYTY_PM4_GET(value, DB_SHADER_CONTROL, KILL_ENABLE) != 0;
		db_shader_control.shader_z_export_enable      = KYTY_PM4_GET(value, DB_SHADER_CONTROL, Z_EXPORT_ENABLE) != 0;
		db_shader_control.shader_execute_on_noop      = KYTY_PM4_GET(value, DB_SHADER_CONTROL, EXEC_ON_NOOP) != 0;
		cp->GetCtx()->SetDepthShaderControl(db_shader_control);
	};

	g_hw_ctx_indirect_func[Pm4::CB_SHADER_MASK]       = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderMask(value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_SHADER_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetScShaderControl(value); };
	g_hw_ctx_indirect_func[Pm4::CB_TARGET_MASK]       = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetRenderTargetMask(value); };

	g_hw_ctx_indirect_func[Pm4::DB_Z_INFO] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetDepthZInfo(State::DecodeDepthZInfo(value)); };

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_INFO] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetDepthStencilInfo(State::DecodeDepthStencilInfo(value)); };

	g_hw_ctx_indirect_func[Pm4::DB_Z_READ_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_read_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthZReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_READ_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_read_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthZReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_READ_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_read_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthStencilReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_READ_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_read_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthStencilReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_WRITE_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_write_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthZWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_WRITE_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_write_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthZWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_WRITE_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_write_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthStencilWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_WRITE_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_write_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthStencilWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_HTILE_DATA_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().htile_data_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthHTileDataBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_HTILE_DATA_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().htile_data_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthHTileDataBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_VIEW] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthDepthView r;
		r.slice_start = KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_START) + (KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_START_HI) << 11u);
		r.slice_max   = KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_MAX) + (KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_MAX_HI) << 11u);
		r.depth_write_disable   = KYTY_PM4_GET(value, DB_DEPTH_VIEW, Z_READ_ONLY) != 0;
		r.stencil_write_disable = KYTY_PM4_GET(value, DB_DEPTH_VIEW, STENCIL_READ_ONLY) != 0;
		r.current_mip_level     = KYTY_PM4_GET(value, DB_DEPTH_VIEW, MIPID);
		cp->GetCtx()->SetDepthDepthView(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SIZE_XY] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthDepthSizeXY r;
		r.x_max = KYTY_PM4_GET(value, DB_DEPTH_SIZE_XY, X_MAX);
		r.y_max = KYTY_PM4_GET(value, DB_DEPTH_SIZE_XY, Y_MAX);
		r.valid = true;
		cp->GetCtx()->SetDepthDepthSizeXY(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_CLEAR] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetDepthClearValue(*reinterpret_cast<const float*>(&value)); };
	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_CLEAR] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetStencilClearValue(KYTY_PM4_GET(value, DB_STENCIL_CLEAR, CLEAR)); };
	g_hw_ctx_indirect_func[Pm4::PA_CL_VTE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetViewportTransformControl(value); };

	g_hw_ctx_indirect_func[Pm4::PA_SC_MODE_CNTL_0] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		trace_aa_register_write("indirect", "PA_SC_MODE_CNTL_0", value);
		HW::ScanModeControl r;
		r.msaa_enable          = KYTY_PM4_GET(value, PA_SC_MODE_CNTL_0, MSAA_ENABLE) != 0;
		r.vport_scissor_enable = KYTY_PM4_GET(value, PA_SC_MODE_CNTL_0, VPORT_SCISSOR_ENABLE) != 0;
		r.line_stipple_enable  = KYTY_PM4_GET(value, PA_SC_MODE_CNTL_0, LINE_STIPPLE_ENABLE) != 0;
		cp->GetCtx()->SetScanModeControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetDepthControl(*cp->GetCtx(), value); };
}

static void graphics_init_jmp_tables_sh_indirect()
{
	for (auto& func: g_hw_sh_indirect_func)
	{
		func = nullptr;
	}

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_ES] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetVs().es_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetEsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_ES] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetVs().es_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetEsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_CHKSUM_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetGsShaderChksum(value); };

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_GS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		HW::GsShaderResource1 r1;
		r1.vgprs                    = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, VGPRS);
		r1.sgprs                    = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, SGPRS);
		r1.priority                 = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, PRIORITY);
		r1.float_mode               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE);
		r1.dx10_clamp               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP) != 0;
		r1.debug_mode               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE) != 0;
		r1.ieee_mode                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, IEEE_MODE) != 0;
		r1.cu_group_enable          = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, CU_GROUP_ENABLE) != 0;
		r1.require_forward_progress = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FWD_PROGRESS) != 0;
		r1.lds_configuration        = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE) != 0;
		r1.gs_vgpr_component_count  = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT);
		r1.fp16_overflow            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetGsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_GS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		HW::GsShaderResource2 r2;
		r2.scratch_en = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, SCRATCH_EN) != 0;
		r2.user_sgpr =
		    KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR) + (KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB) << 5u);
		r2.es_vgpr_component_count = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT);
		r2.offchip_lds             = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN) != 0;
		r2.lds_size                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE);
		r2.shared_vgprs            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetGsShaderResource2(r2);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_PS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetPs().ps_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetPsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_PS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetPs().ps_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetPsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_CHKSUM_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetPsShaderChksum(value); };

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_PS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		HW::PsShaderResource1 r1;
		r1.vgprs                    = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, VGPRS);
		r1.sgprs                    = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, SGPRS);
		r1.priority                 = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, PRIORITY);
		r1.float_mode               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE);
		r1.dx10_clamp               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) != 0;
		r1.debug_mode               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE) != 0;
		r1.ieee_mode                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, IEEE_MODE) != 0;
		r1.cu_group_disable         = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, CU_GROUP_DISABLE) != 0;
		r1.require_forward_progress = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FWD_PROGRESS) != 0;
		r1.fp16_overflow            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetPsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_PS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		HW::PsShaderResource2 r2;
		r2.scratch_en = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, SCRATCH_EN);
		r2.user_sgpr =
		    KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR) + (KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB) << 5u);
		r2.wave_cnt_en            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, WAVE_CNT_EN);
		r2.extra_lds_size         = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, EXTRA_LDS_SIZE);
		r2.raster_ordered_shading = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, LOAD_INTRAWAVE_COLLISION);
		r2.shared_vgprs           = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetPsShaderResource2(r2);
	};

	// Gen5 SH-indirect emits COMPUTE_* as lone offset/value pairs. Mirror the
	// direct SET_SH_REG decoders.
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_LO] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto& r = cp->GetShCtx()->CsRegs();
		r.data_addr &= 0xFFFFFF00000000FFull;
		r.data_addr |= static_cast<uint64_t>(value) << 8u;
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_HI] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto& r = cp->GetShCtx()->CsRegs();
		r.data_addr &= 0xFFFF00FFFFFFFFFFull;
		r.data_addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_RSRC1] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{ decode_compute_pgm_rsrc1(cp->GetShCtx()->CsRegs(), value); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_RSRC2] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{ decode_compute_pgm_rsrc2(cp->GetShCtx()->CsRegs(), value); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_RESOURCE_LIMITS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{ EXIT_NOT_IMPLEMENTED(!GraphicsDecodeComputeResourceLimits(&cp->GetShCtx()->CsRegs(), cmd_offset, &value, 1)); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_RSRC3]     = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->CsRegs().rsrc3 = value; };
	g_hw_sh_indirect_func[Pm4::COMPUTE_SHADER_CHKSUM] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto& r  = cp->GetShCtx()->CsRegs();
		r.chksum = (r.chksum & 0xffffffff00000000ull) | static_cast<uint64_t>(value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_SHADER_CHKSUM_HI] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto& r  = cp->GetShCtx()->CsRegs();
		r.chksum = (r.chksum & 0x00000000ffffffffull) | (static_cast<uint64_t>(value) << 32u);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_NUM_THREAD_X] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetCsNumThreadX(value); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_NUM_THREAD_Y] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetCsNumThreadY(value); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_NUM_THREAD_Z] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetCsNumThreadZ(value); };
	for (uint32_t slot = 0; slot < 16; slot++)
	{
		g_hw_sh_indirect_func[Pm4::COMPUTE_USER_DATA_0 + slot] = [](KYTY_HW_SH_INDIRECT_ARGS)
		{
			const uint32_t id = cmd_offset - Pm4::COMPUTE_USER_DATA_0;
			cp->GetShCtx()->SetCsUserSgpr(id, value, cp->GetUserDataMarker());
			cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		};
	}
}

static void graphics_init_jmp_tables_uc_indirect()
{
	for (auto& func: g_hw_uc_indirect_func)
	{
		func = nullptr;
	}

	g_hw_uc_indirect_func[Pm4::GE_CNTL] = [](KYTY_HW_UC_INDIRECT_ARGS)
	{
		HW::GeControl r;
		r.primitive_group_size = KYTY_PM4_GET(value, GE_CNTL, PRIM_GRP_SIZE);
		r.vertex_group_size    = KYTY_PM4_GET(value, GE_CNTL, VERT_GRP_SIZE);
		cp->GetUcfg()->SetGeControl(r);
	};

	g_hw_uc_indirect_func[Pm4::GE_USER_VGPR_EN] = [](KYTY_HW_UC_INDIRECT_ARGS)
	{
		HW::GeUserVgprEn r;
		r.vgpr1 = KYTY_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR1) != 0;
		r.vgpr2 = KYTY_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR2) != 0;
		r.vgpr3 = KYTY_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR3) != 0;
		cp->GetUcfg()->SetGeUserVgprEn(r);
	};

	g_hw_uc_indirect_func[Pm4::VGT_PRIMITIVE_TYPE] = [](KYTY_HW_UC_INDIRECT_ARGS)
	{
		uint32_t prim_type = KYTY_PM4_GET(value, VGT_PRIMITIVE_TYPE, PRIM_TYPE);
		cp->GetUcfg()->SetPrimitiveType(prim_type);
	};

	// Index type via UCONFIG (same 2-bit size field as IT_INDEX_TYPE).
	g_hw_uc_indirect_func[Pm4::VGT_INDEX_TYPE] = [](KYTY_HW_UC_INDIRECT_ARGS) { cp->SetIndexType(value & 0x3u); };

	// Remaining UCONFIG regs accepted without host state until HardwareContext
	// gains matching setters.
	const auto ignore_uc = [](KYTY_HW_UC_INDIRECT_ARGS)
	{
		(void)cp;
		(void)cmd_offset;
		(void)value;
	};
	g_hw_uc_indirect_func[Pm4::GE_INDX_OFFSET]            = [](KYTY_HW_UC_INDIRECT_ARGS) { cp->GetUcfg()->SetIndexOffset(value); };
	g_hw_uc_indirect_func[Pm4::GE_MULTI_PRIM_IB_RESET_EN] = ignore_uc;
	g_hw_uc_indirect_func[Pm4::VGT_OBJECT_ID]             = ignore_uc;
	g_hw_uc_indirect_func[Pm4::TEXTURE_GRADIENT_FACTORS]  = ignore_uc;
	g_hw_uc_indirect_func[Pm4::IA_MULTI_VGT_PARAM]        = ignore_uc;
	g_hw_uc_indirect_func[Pm4::TA_CS_BC_BASE_ADDR]        = ignore_uc;
	g_hw_uc_indirect_func[Pm4::TA_CS_BC_BASE_ADDR_HI]     = ignore_uc;
	g_hw_uc_indirect_func[Pm4::GDS_OA_CNTL]               = ignore_uc;
	g_hw_uc_indirect_func[Pm4::GDS_OA_COUNTER]            = ignore_uc;
	g_hw_uc_indirect_func[Pm4::GDS_OA_ADDRESS]            = ignore_uc;
	g_hw_uc_indirect_func[Pm4::GE_STEREO_CNTL]            = ignore_uc;
}

void graphics_init_jmp_tables()
{
	for (auto& func: g_hw_ctx_func)
	{
		func = nullptr;
	}

	g_hw_ctx_func[Pm4::DB_RENDER_CONTROL]            = hw_ctx_set_render_control;
	g_hw_ctx_func[Pm4::DB_STENCIL_CLEAR]             = hw_ctx_set_stencil_clear;
	g_hw_ctx_func[Pm4::DB_DEPTH_CLEAR]               = hw_ctx_set_depth_clear;
	g_hw_ctx_func[Pm4::PA_SC_SCREEN_SCISSOR_TL]      = hw_ctx_set_screen_scissor;
	g_hw_ctx_func[Pm4::DB_Z_INFO]                    = hw_ctx_set_depth_render_target;
	g_hw_ctx_func[Pm4::DB_STENCIL_INFO]              = hw_ctx_set_stencil_info;
	g_hw_ctx_func[Pm4::PA_SU_HARDWARE_SCREEN_OFFSET] = hw_ctx_set_hardware_screen_offset;
	g_hw_ctx_func[Pm4::PA_SC_WINDOW_OFFSET]          = hw_ctx_set_window_offset;
	g_hw_ctx_func[Pm4::CB_TARGET_MASK]               = hw_ctx_set_render_target_mask;
	g_hw_ctx_func[Pm4::PA_SC_GENERIC_SCISSOR_TL]     = hw_ctx_set_generic_scissor;
	g_hw_ctx_func[Pm4::CB_BLEND_RED]                 = hw_ctx_set_blend_color;
	g_hw_ctx_func[Pm4::DB_STENCIL_CONTROL]           = hw_ctx_set_stencil_control;
	g_hw_ctx_func[Pm4::DB_STENCILREFMASK]            = hw_ctx_set_stencil_mask;
	g_hw_ctx_func[Pm4::SPI_PS_INPUT_CNTL_0]          = hw_ctx_set_ps_input;
	g_hw_ctx_func[Pm4::DB_DEPTH_CONTROL]             = hw_ctx_set_depth_control;
	g_hw_ctx_func[Pm4::DB_EQAA]                      = hw_ctx_set_eqaa_control;
	g_hw_ctx_func[Pm4::CB_COLOR_CONTROL]             = hw_ctx_set_color_control;
	g_hw_ctx_func[Pm4::PA_CL_CLIP_CNTL]              = hw_ctx_set_clip_control;
	g_hw_ctx_func[Pm4::PA_SU_SC_MODE_CNTL]           = hw_ctx_set_mode_control;
	for (uint32_t reg = Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL; reg <= Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET; reg++)
	{
		g_hw_ctx_func[reg] = hw_ctx_set_polygon_offset;
	}
	g_hw_ctx_func[Pm4::PA_CL_VTE_CNTL]                    = hw_ctx_set_viewport_transform_control;
	g_hw_ctx_func[Pm4::PA_SU_LINE_CNTL]                   = hw_ctx_set_line_control;
	g_hw_ctx_func[Pm4::PA_SC_MODE_CNTL_0]                 = hw_ctx_set_scan_mode_control;
	g_hw_ctx_func[Pm4::PA_SC_AA_CONFIG]                   = hw_ctx_set_aa_config;
	g_hw_ctx_func[Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0] = hw_ctx_set_aa_sample_control;
	// Sample masks are already intentionally ignored by the indirect register
	// path. Accept the direct form as well so both PM4 encodings agree.
	g_hw_ctx_func[Pm4::PA_SC_AA_MASK_X0Y0_X1Y0] = hw_ctx_ignore;
	g_hw_ctx_func[Pm4::PA_SC_AA_MASK_X0Y1_X1Y1] = hw_ctx_ignore;
	// Keep direct and indirect handling aligned for metadata-only registers.
	for (const uint32_t reg: {Pm4::CB_DCC_CONTROL,
	                          Pm4::DB_COUNT_CONTROL,
	                          Pm4::DB_RENDER_OVERRIDE,
	                          Pm4::DB_RENDER_OVERRIDE2,
	                          Pm4::DB_DFSM_CONTROL,
	                          Pm4::DB_RMI_L2_CACHE_CONTROL,
	                          Pm4::CB_RMI_GL2_CACHE_CONTROL,
	                          Pm4::TA_BC_BASE_ADDR,
	                          Pm4::TA_BC_BASE_ADDR_HI,
	                          Pm4::PA_SU_POINT_SIZE,
	                          Pm4::PA_SU_POINT_MINMAX,
	                          Pm4::SPI_TMPRING_SIZE,
	                          Pm4::VGT_MULTI_PRIM_IB_RESET_INDX,
	                          Pm4::VGT_DRAW_PAYLOAD_CNTL,
	                          Pm4::VGT_PRIMITIVEID_RESET,
	                          Pm4::PA_CL_OBJPRIM_ID_CNTL,
	                          Pm4::PA_SC_FOV_WINDOW_LR,
	                          Pm4::PA_SC_FOV_WINDOW_TB,
	                          Pm4::PA_SC_MODE_CNTL_1,
	                          Pm4::PA_SU_VTX_CNTL,
	                          Pm4::PS_SHADER_SAMPLE_EXCLUSION_MASK,
	                          Pm4::PA_SC_BINNER_CNTL_0,
	                          Pm4::PA_SC_BINNER_CNTL_1,
	                          Pm4::PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,
	                          Pm4::PA_SC_NGG_MODE_CNTL,
	                          Pm4::DB_ALPHA_TO_MASK,
	                          Pm4::PA_SC_WINDOW_SCISSOR_TL,
	                          Pm4::PA_SC_WINDOW_SCISSOR_BR,
	                          Pm4::VGT_HOS_MAX_TESS_LEVEL,
	                          Pm4::VGT_HOS_MIN_TESS_LEVEL,
	                          Pm4::VGT_PRIMITIVEID_EN,
	                          Pm4::VGT_REUSE_OFF,
	                          Pm4::VGT_TESS_DISTRIBUTION,
	                          Pm4::VGT_LS_HS_CONFIG,
	                          Pm4::VGT_TF_PARAM,
	                          Pm4::DB_DEPTH_BOUNDS_MIN,
	                          Pm4::DB_DEPTH_BOUNDS_MAX,
	                          Pm4::DB_HTILE_SURFACE,
	                          Pm4::DB_DEPTH_INFO,
	                          Pm4::DB_DEPTH_SIZE,
	                          Pm4::DB_DEPTH_SLICE})
	{
		g_hw_ctx_func[reg] = hw_ctx_ignore;
	}
	g_hw_ctx_func[Pm4::VGT_SHADER_STAGES_EN]   = hw_ctx_set_shader_stages;
	g_hw_ctx_func[Pm4::PA_CL_GB_VERT_CLIP_ADJ] = hw_ctx_set_guard_bands;

	for (uint32_t slot = 0; slot < 8; slot++)
	{
		g_hw_ctx_func[Pm4::CB_COLOR0_BASE + slot * 15] = hw_ctx_set_render_target;
		g_hw_ctx_func[Pm4::CB_COLOR0_INFO + slot * 15] = hw_ctx_set_color_info;

		g_hw_ctx_func[Pm4::CB_BLEND0_CONTROL + slot * 1] = hw_ctx_set_blend_control;
	}

	for (uint32_t viewport = 0; viewport < 16; viewport++)
	{
		g_hw_ctx_func[Pm4::PA_SC_VPORT_ZMIN_0 + viewport * 2] = hw_ctx_set_viewport_z;
		for (uint32_t component = 0; component < 6; component++)
		{
			g_hw_ctx_func[Pm4::PA_CL_VPORT_XSCALE + viewport * 6 + component] = hw_ctx_set_viewport_scale_offset;
		}
	}

	for (auto& func: g_hw_sh_func)
	{
		func = nullptr;
	}

	for (uint32_t slot = 0; slot < 16; slot++)
	{
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_VS_0 + slot * 1] = hw_sh_set_vs_user_sgpr;
		g_hw_sh_func[Pm4::COMPUTE_USER_DATA_0 + slot * 1]       = hw_sh_set_cs_user_sgpr;
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_GS_0 + slot * 1] = hw_sh_set_gs_user_sgpr;
	}
	// PS user data is 32 dwords on Gen5 (SPI_SHADER_USER_DATA_PS_0..31).
	for (uint32_t slot = 0; slot < 32; slot++)
	{
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_PS_0 + slot * 1] = hw_sh_set_ps_user_sgpr;
	}

	g_hw_sh_func[Pm4::COMPUTE_NUM_THREAD_X]     = hw_sh_set_cs_num_thread;
	g_hw_sh_func[Pm4::COMPUTE_NUM_THREAD_Y]     = hw_sh_set_cs_num_thread;
	g_hw_sh_func[Pm4::COMPUTE_NUM_THREAD_Z]     = hw_sh_set_cs_num_thread;
	g_hw_sh_func[Pm4::COMPUTE_PGM_LO]           = hw_sh_set_cs_pgm;
	g_hw_sh_func[Pm4::COMPUTE_PGM_HI]           = hw_sh_set_cs_pgm;
	g_hw_sh_func[Pm4::COMPUTE_PGM_RSRC1]        = hw_sh_set_cs_rsrc;
	g_hw_sh_func[Pm4::COMPUTE_PGM_RSRC2]        = hw_sh_set_cs_rsrc;
	g_hw_sh_func[Pm4::COMPUTE_RESOURCE_LIMITS]  = hw_sh_set_cs_resource_limits;
	g_hw_sh_func[Pm4::COMPUTE_PGM_RSRC3]        = hw_sh_set_cs_rsrc;
	g_hw_sh_func[Pm4::COMPUTE_SHADER_CHKSUM]    = hw_sh_set_cs_rsrc;
	g_hw_sh_func[Pm4::COMPUTE_SHADER_CHKSUM_HI] = hw_sh_set_cs_rsrc;

	for (auto& func: g_hw_uc_func)
	{
		func = nullptr;
	}

	g_hw_uc_func[Pm4::VGT_PRIMITIVE_TYPE] = hw_uc_set_primitive_type;

	for (auto& func: g_hw_sh_custom_func)
	{
		func = nullptr;
	}

	g_hw_sh_custom_func[Pm4::R_VS]          = hw_sh_set_vs_shader;
	g_hw_sh_custom_func[Pm4::R_PS]          = hw_sh_set_ps_shader;
	g_hw_sh_custom_func[Pm4::R_CS]          = hw_sh_set_cs_shader;
	g_hw_sh_custom_func[Pm4::R_VS_EMBEDDED] = hw_sh_set_vs_embedded;
	g_hw_sh_custom_func[Pm4::R_PS_EMBEDDED] = hw_sh_set_ps_embedded;
	g_hw_sh_custom_func[Pm4::R_VS_UPDATE]   = hw_sh_update_vs_shader;
	g_hw_sh_custom_func[Pm4::R_PS_UPDATE]   = hw_sh_update_ps_shader;

	for (auto& func: g_cp_op_func)
	{
		func = nullptr;
	}

	g_cp_op_func[Pm4::IT_NOP]                     = cp_op_nop;
	g_cp_op_func[Pm4::IT_CLEAR_STATE]             = cp_op_clear_state;
	g_cp_op_func[Pm4::IT_SET_BASE]                = cp_op_set_base;
	g_cp_op_func[Pm4::IT_DISPATCH_INDIRECT]       = cp_op_dispatch_indirect;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_INDIRECT]     = cp_op_draw_index_indirect;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_2]            = cp_op_draw_index;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_OFFSET_2]     = cp_op_draw_index_offset;
	g_cp_op_func[Pm4::IT_INDEX_BASE]              = cp_op_index_base;
	g_cp_op_func[Pm4::IT_INDEX_BUFFER_SIZE]       = cp_op_index_buffer_size;
	g_cp_op_func[Pm4::IT_INDEX_TYPE]              = cp_op_index_type;
	g_cp_op_func[Pm4::IT_NUM_INSTANCES]           = cp_op_num_instances;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_AUTO]         = cp_op_draw_index_auto;
	g_cp_op_func[Pm4::IT_WAIT_REG_MEM]            = cp_op_wait_reg_mem;
	g_cp_op_func[Pm4::IT_WRITE_DATA]              = cp_op_write_data;
	g_cp_op_func[Pm4::IT_INDIRECT_BUFFER]         = cp_op_indirect_buffer;
	g_cp_op_func[Pm4::IT_INDIRECT_BUFFER_END]     = cp_op_indirect_buffer_end;
	g_cp_op_func[Pm4::IT_EVENT_WRITE]             = cp_op_event_write;
	g_cp_op_func[Pm4::IT_EVENT_WRITE_EOP]         = cp_op_event_write_eop;
	g_cp_op_func[Pm4::IT_EVENT_WRITE_EOS]         = cp_op_event_write_eos;
	g_cp_op_func[Pm4::IT_RELEASE_MEM]             = cp_op_release_mem;
	g_cp_op_func[Pm4::IT_DMA_DATA]                = cp_op_dma_data;
	g_cp_op_func[Pm4::IT_ONE_REG_WRITE]           = cp_op_one_reg_write;
	g_cp_op_func[Pm4::IT_ACQUIRE_MEM]             = cp_op_acquire_mem;
	g_cp_op_func[Pm4::IT_SET_CONTEXT_REG]         = cp_op_set_context_reg;
	g_cp_op_func[Pm4::IT_SET_SH_REG]              = cp_op_set_shader_reg;
	g_cp_op_func[Pm4::IT_DISPATCH_DIRECT]         = cp_op_dispatch_direct;
	g_cp_op_func[Pm4::IT_SET_UCONFIG_REG]         = cp_op_set_uconfig_reg;
	g_cp_op_func[Pm4::IT_SET_UCONFIG_REG_INDEX]   = cp_op_set_uconfig_reg_index;
	g_cp_op_func[Pm4::IT_WRITE_CONST_RAM]         = cp_op_write_const_ram;
	g_cp_op_func[Pm4::IT_DUMP_CONST_RAM]          = cp_op_dump_const_ram;
	g_cp_op_func[Pm4::IT_INCREMENT_CE_COUNTER]    = cp_op_increment_ce_counter;
	g_cp_op_func[Pm4::IT_INCREMENT_DE_COUNTER]    = cp_op_increment_de_counter;
	g_cp_op_func[Pm4::IT_WAIT_ON_CE_COUNTER]      = cp_op_wait_on_ce_counter;
	g_cp_op_func[Pm4::IT_WAIT_ON_DE_COUNTER_DIFF] = cp_op_wait_on_de_counter_diff;
	g_cp_op_func[Pm4::IT_GET_LOD_STATS]           = cp_op_get_lod_stats;

	for (auto& func: g_cp_op_custom_func)
	{
		func = nullptr;
	}

	g_cp_op_custom_func[Pm4::R_DRAW_INDEX]       = cp_op_draw_index;
	g_cp_op_custom_func[Pm4::R_DRAW_INDEX_AUTO]  = cp_op_draw_index_auto;
	g_cp_op_custom_func[Pm4::R_DISPATCH_DIRECT]  = cp_op_dispatch_direct;
	g_cp_op_custom_func[Pm4::R_DISPATCH_RESET]   = cp_op_dispatch_reset;
	g_cp_op_custom_func[Pm4::R_WAIT_MEM_32]      = cp_op_wait_reg_mem_32;
	g_cp_op_custom_func[Pm4::R_DRAW_RESET]       = cp_op_draw_reset;
	g_cp_op_custom_func[Pm4::R_WAIT_FLIP_DONE]   = cp_op_wait_flip_done;
	g_cp_op_custom_func[Pm4::R_PUSH_MARKER]      = cp_op_push_marker;
	g_cp_op_custom_func[Pm4::R_POP_MARKER]       = cp_op_pop_marker;
	g_cp_op_custom_func[Pm4::R_CX_REGS_INDIRECT] = cp_op_indirect_cx_regs;
	g_cp_op_custom_func[Pm4::R_SH_REGS_INDIRECT] = cp_op_indirect_sh_regs;
	g_cp_op_custom_func[Pm4::R_UC_REGS_INDIRECT] = cp_op_indirect_uc_regs;
	g_cp_op_custom_func[Pm4::R_ACQUIRE_MEM]      = cp_op_acquire_mem;
	g_cp_op_custom_func[Pm4::R_WRITE_DATA]       = cp_op_write_data;
	g_cp_op_custom_func[Pm4::R_WAIT_MEM_64]      = cp_op_wait_reg_mem_64;
	g_cp_op_custom_func[Pm4::R_FLIP]             = cp_op_flip;
	g_cp_op_custom_func[Pm4::R_RELEASE_MEM]      = cp_op_release_mem;
	g_cp_op_custom_func[Pm4::R_DMA_DATA]         = cp_op_custom_dma_data;

	graphics_init_jmp_tables_cx_indirect();
	graphics_init_jmp_tables_sh_indirect();
	graphics_init_jmp_tables_uc_indirect();
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
