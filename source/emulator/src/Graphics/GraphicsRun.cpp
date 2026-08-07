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
	if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); }

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
	if (ring_addr == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ring_addr == nullptr condition ignored (continuing)\n"); }
	if (ring_size_dw == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ring_size_dw == 0 condition ignored (continuing)\n"); }
	if (read_ptr_addr == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: read_ptr_addr == nullptr condition ignored (continuing)\n"); }

	return m_submission_admission_gate.RunAdmitted(
	    [&]
	    {
		    auto v = pipe_id * 8 + queue_id;

		    if (v >= 64) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: v >= 64 condition ignored (continuing)\n"); }

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
		    if (ring_id < 1 || ring_id > 64) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ring_id < 1 || ring_id > 64 condition ignored (continuing)\n"); }

		    auto* ring = GetRing(ring_id);

		    if (!ring->IsActive()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ring->IsActive() condition ignored (continuing)\n"); }

		    ring->SetActive(false);
	    });
}

void Gpu::DingDong(uint32_t ring_id, uint32_t offset_dw)
{
	m_submission_admission_gate.RunAdmitted(
	    [&]
	    {
		    if (ring_id < 1 || ring_id > 64) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: ring_id < 1 || ring_id > 64 condition ignored (continuing)\n"); }
		    if (offset_dw == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: offset_dw == 0 condition ignored (continuing)\n"); }

		    auto* ring = GetRing(ring_id);

		    if (!ring->IsActive()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !ring->IsActive() condition ignored (continuing)\n"); }

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

SubmissionId CommandProcessor::BufferFlushForGpuWait()
{
	SubmissionId latest_completed;
	SubmissionId submitted;

	{
		Core::LockGuard lock(m_mutex);
		submitted = SubmitCurrentLocked(&latest_completed);
	}

	PublishCompletedSubmissions();
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
	if (!m_buffer[submitted_slot]->GetSubmissionId(&submitted_id)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !m_buffer[submitted_slot]->GetSubmissionId(&submitted_id) condition ignored (continuing)\n"); }
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
		if (!m_buffer[recording_slot]->GetSubmissionId(&slot_owner)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !m_buffer[recording_slot]->GetSubmissionId(&slot_owner) condition ignored (continuing)\n"); }
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
		if (!m_buffer[m_current_buffer]->GetSubmissionId(&recording)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !m_buffer[m_current_buffer]->GetSubmissionId(&recording) condition ignored (continuing)\n"); }
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
		DebugStatsRecordWaitRegMemClass(DebugStatsWaitRegMemClass::Satisfied);
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
	if (producer == GpuSubmissionResult::Success)
	{
		DebugStatsRecordWaitRegMemClass(producer_is_current_submission ? DebugStatsWaitRegMemClass::CurrentProducer
		                                                                 : DebugStatsWaitRegMemClass::QueuedProducer);
	} else if (producer == GpuSubmissionResult::ProducerValueMismatch)
	{
		DebugStatsRecordWaitRegMemClass(DebugStatsWaitRegMemClass::ProducerMismatch);
	} else if (producer == GpuSubmissionResult::ProducerNotFound)
	{
		DebugStatsRecordWaitRegMemClass(DebugStatsWaitRegMemClass::ProducerNotFound);
	}
	if (producer != GpuSubmissionResult::Success && producer != GpuSubmissionResult::ProducerValueMismatch &&
	                     producer != GpuSubmissionResult::ProducerNotFound) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: producer != GpuSubmissionResult::Success && producer != GpuSubmissionResult::Pro condition ignored (continuing)\n"); }
	// Submit the command buffer before resolving the dependency so pending EOP
	// labels and completion callbacks become visible. Unknown or mismatched
	// producers use a real DCB suspension instead of a CPU poll.
	// When the awaited producer is already queued, avoid the redundant
	// publication wait for the latest completed submission; the subsequent
	// WaitSubmission for the specific producer covers the needed ordering.
	BufferFlushForGpuWait();
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
	DebugStatsRecordWaitRegMemSuspended();
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
		DebugStatsRecordWaitRegMemClass(DebugStatsWaitRegMemClass::Satisfied);
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
	if (producer == GpuSubmissionResult::Success)
	{
		DebugStatsRecordWaitRegMemClass(producer_is_current_submission ? DebugStatsWaitRegMemClass::CurrentProducer
		                                                                 : DebugStatsWaitRegMemClass::QueuedProducer);
	} else if (producer == GpuSubmissionResult::ProducerValueMismatch)
	{
		DebugStatsRecordWaitRegMemClass(DebugStatsWaitRegMemClass::ProducerMismatch);
	} else if (producer == GpuSubmissionResult::ProducerNotFound)
	{
		DebugStatsRecordWaitRegMemClass(DebugStatsWaitRegMemClass::ProducerNotFound);
	}
	if (producer != GpuSubmissionResult::Success && producer != GpuSubmissionResult::ProducerValueMismatch &&
	                     producer != GpuSubmissionResult::ProducerNotFound) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: producer != GpuSubmissionResult::Success && producer != GpuSubmissionResult::Pro condition ignored (continuing)\n"); }
	// Submit before resolving the dependency so completion callbacks publish in
	// exact submission order. Suspend only when no matching producer is proven.
	// Avoid the redundant publication wait for the latest completed submission;
	// the subsequent WaitSubmission for the specific producer covers ordering.
	BufferFlushForGpuWait();
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
	DebugStatsRecordWaitRegMemSuspended();
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

		if (dw > num_dw) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dw > num_dw condition ignored (continuing)\n"); }

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
			if (remaining_including_header < special_packet_dwords) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: remaining_including_header < special_packet_dwords condition ignored (continuing)\n"); }
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

		if (dw < 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dw < 1 condition ignored (continuing)\n"); }

		auto op = (cmd_id >> 8u) & 0xffu;

		auto pfunc = g_cp_op_func[op];

		if (pfunc == nullptr)
		{
			const auto* packet_start = cmd - 1;
			if (Config::IsNextGen() && Pm4::Pm4Gen5OpaquePairPrecedesWaitFlipDone(packet_start, remaining_including_header))
			{
				if (dw < 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dw < 1 condition ignored (continuing)\n"); }
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

		// KYTY_LOG_DEBUG("\t %05" PRIx32 ": %u\n", num_dw - dw - 1, s);

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
		default: if (true) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: true condition ignored (continuing)\n"); } break;
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

	if (m_num_instances != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_num_instances != 1 condition ignored (continuing)\n"); }
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
	if (m_index_base_addr == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_index_base_addr == 0 condition ignored (continuing)\n"); }
	if ((flags & ~0xE0000001u) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (flags & ~0xE0000001u) != 0 condition ignored (continuing)\n"); }

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
	if (m_draw_indirect_args_base_addr == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_draw_indirect_args_base_addr == 0 condition ignored (continuing)\n"); }
	if (m_index_base_addr == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_index_base_addr == 0 condition ignored (continuing)\n"); }
	if ((initiator & ~0x20u) != 2u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: (initiator & ~0x20u) != 2u condition ignored (continuing)\n"); }

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
		default: if (true) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: true condition ignored (continuing)\n"); } break;
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
	if (m_dispatch_indirect_args_base_addr == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_dispatch_indirect_args_base_addr == 0 condition ignored (continuing)\n"); }

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
		if (!m_buffer[m_current_buffer]->GetSubmissionId(&submission)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !m_buffer[m_current_buffer]->GetSubmissionId(&submission) condition ignored (continuing)\n"); }
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
		KYTY_LOG_DEBUG("CommandProcessor::WriteAtEndOfPipe32()\n");
		KYTY_LOG_DEBUG("\t cache_policy        = 0x%08" PRIx32 "\n", cache_policy);
		KYTY_LOG_DEBUG("\t event_write_dest    = 0x%08" PRIx32 "\n", event_write_dest);
		KYTY_LOG_DEBUG("\t eop_event_type      = 0x%08" PRIx32 "\n", eop_event_type);
		KYTY_LOG_DEBUG("\t cache_action        = 0x%08" PRIx32 "\n", cache_action);
		KYTY_LOG_DEBUG("\t event_index         = 0x%08" PRIx32 "\n", event_index);
		KYTY_LOG_DEBUG("\t event_write_source  = 0x%08" PRIx32 "\n", event_write_source);
		KYTY_LOG_DEBUG("\t interrupt_selector  = 0x%08" PRIx32 "\n", interrupt_selector);
		KYTY_LOG_DEBUG("\t dst_gpu_addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		KYTY_LOG_DEBUG("\t value               = 0x%08" PRIx32 "\n", value);
	}

	if (cache_policy != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cache_policy != 0x00000000 condition ignored (continuing)\n"); }
	if (event_write_dest != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: event_write_dest != 0x00000000 condition ignored (continuing)\n"); }
	if (interrupt_selector != 0x0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: interrupt_selector != 0x0 condition ignored (continuing)\n"); }

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
		KYTY_LOG_DEBUG("CommandProcessor::WriteAtEndOfPipe64()\n");
		KYTY_LOG_DEBUG("\t cache_policy        = 0x%08" PRIx32 "\n", cache_policy);
		KYTY_LOG_DEBUG("\t event_write_dest    = 0x%08" PRIx32 "\n", event_write_dest);
		KYTY_LOG_DEBUG("\t eop_event_type      = 0x%08" PRIx32 "\n", eop_event_type);
		KYTY_LOG_DEBUG("\t cache_action        = 0x%08" PRIx32 "\n", cache_action);
		KYTY_LOG_DEBUG("\t event_index         = 0x%08" PRIx32 "\n", event_index);
		KYTY_LOG_DEBUG("\t event_write_source  = 0x%08" PRIx32 "\n", event_write_source);
		KYTY_LOG_DEBUG("\t interrupt_selector  = 0x%08" PRIx32 "\n", interrupt_selector);
		KYTY_LOG_DEBUG("\t dst_gpu_addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		KYTY_LOG_DEBUG("\t value               = 0x%016" PRIx64 "\n", value);
	}

	if (cache_policy != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cache_policy != 0x00000000 condition ignored (continuing)\n"); }
	if (event_write_dest != 0x00000000) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: event_write_dest != 0x00000000 condition ignored (continuing)\n"); }

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
		KYTY_LOG_DEBUG("CommandProcessor::TriggerEvent()\n");
		KYTY_LOG_DEBUG("\t event_type  = 0x%08" PRIx32 "\n", event_type);
		KYTY_LOG_DEBUG("\t event_index = 0x%08" PRIx32 "\n", event_index);
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
		KYTY_LOG_DEBUG("CommandProcessor::Flip()\n");
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
		KYTY_LOG_DEBUG("CommandProcessor::Flip()\n");
		KYTY_LOG_DEBUG("\t dst_gpu_addr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		KYTY_LOG_DEBUG("\t value        = 0x%08" PRIx32 "\n", value);
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
		KYTY_LOG_DEBUG("CommandProcessor::FlipWithInterrupt()\n");
		KYTY_LOG_DEBUG("\t eop_event_type      = 0x%08" PRIx32 "\n", eop_event_type);
		KYTY_LOG_DEBUG("\t cache_action        = 0x%08" PRIx32 "\n", cache_action);
		KYTY_LOG_DEBUG("\t dst_gpu_addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		KYTY_LOG_DEBUG("\t value               = 0x%08" PRIx32 "\n", value);
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


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
