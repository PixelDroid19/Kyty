#include "Emulator/Graphics/Objects/Label.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/LabelSubmissionTracker.h"
#include "Emulator/Graphics/Utils.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class CommandProcessor;

enum LabelStatus
{
	New,
	Active,
	ActiveDeleted,
	NotActive,
};

struct LabelCallbacks
{
	SubmissionId              submission;
	uint64_t*                  dst_gpu_addr64       = nullptr;
	uint64_t                   value64              = 0;
	uint32_t*                  dst_gpu_addr32       = nullptr;
	uint32_t                   value32              = 0;
	LabelCallback callback_1 = nullptr;
	LabelCallback callback_2 = nullptr;
	uint64_t                   args[LABEL_ARGS_MAX] = {};
};

struct Label
{
	VkDevice          device = nullptr;
	VkEvent           event  = nullptr;
	LabelStatus       status = LabelStatus::New;
	LabelCallbacks    callbacks;
	CommandProcessor* cp = nullptr;
	bool              exact_submission_completed = false;
};

class LabelManager
{
public:
	LabelManager()
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
		Core::Thread t(ThreadRun, this);
		t.Detach();
	}
	virtual ~LabelManager() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(LabelManager);

	Label* Create64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelCallback callback_1,
	                LabelCallback callback_2, const uint64_t* args);
	Label* Create32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, uint32_t dst_word_count,
	                LabelCallback callback_1, LabelCallback callback_2, const uint64_t* args);
	void   Delete(Label* label);
	void   Set(CommandBuffer* buffer, Label* label);
	void   DrainCompleted();
	void   CompleteSubmission(SubmissionId submission);
	void   WriteBackCopy(void* guest_dst, const void* gpu_src, uint64_t size);
	void   ReleaseMappedRange(uint64_t addr, uint64_t bytes);

private:
	static void ThreadRun(void* data);
	static void FireCallbacks(const Vector<LabelCallbacks>& fired_labels);

	bool        Remove(Label* label);
	static void Destroy(Label* label);

	Core::Mutex    m_mutex;
	Core::CondVar  m_cond_var;
	Vector<Label*> m_labels;
	// Exact logical submission containing each active Label's vkCmdSetEvent.
	// Protected by m_mutex together with m_labels and Label::status.
	LabelSubmissionTracker m_submission_labels;
	// Legacy labels that are not bound to an exact submission may still need
	// destruction on the Label thread.
	Vector<Label*> m_deferred_destroy;
	LabelFenceRegistry m_fence_holes;

	void RegisterFenceHole(uint64_t addr, uint64_t bytes);
};

static LabelManager* g_label_manager = nullptr;

LabelFenceRegistrationStatus LabelFenceRegistry::Register(uint64_t addr, uint64_t bytes)
{
	if (bytes == 0 || bytes > UINT64_MAX - addr)
	{
		return LabelFenceRegistrationStatus::InvalidArgument;
	}

	const uint64_t end = addr + bytes;
	for (int i = 0; i < static_cast<int>(m_begin.Size()); i++)
	{
		if (m_begin.At(i) == addr && m_end.At(i) == end)
		{
			return LabelFenceRegistrationStatus::AlreadyRegistered;
		}
	}

	m_begin.Add(addr);
	m_end.Add(end);
	return LabelFenceRegistrationStatus::Inserted;
}

LabelFenceReleaseStatus LabelFenceRegistry::ReleaseAllocation(uint64_t addr, uint64_t bytes)
{
	if (bytes == 0 || bytes > UINT64_MAX - addr)
	{
		return LabelFenceReleaseStatus::InvalidArgument;
	}

	const uint64_t end      = addr + bytes;
	bool           released = false;
	for (int i = 0; i < static_cast<int>(m_begin.Size()); i++)
	{
		const uint64_t hole_begin = m_begin.At(i);
		const uint64_t hole_end   = m_end.At(i);
		if (hole_begin >= end || addr >= hole_end)
		{
			continue;
		}
		if (hole_begin < addr || hole_end > end)
		{
			return LabelFenceReleaseStatus::PartialOverlap;
		}
		released = true;
	}

	if (!released)
	{
		return LabelFenceReleaseStatus::NotFound;
	}

	for (int i = static_cast<int>(m_begin.Size()) - 1; i >= 0; i--)
	{
		if (m_begin.At(i) >= addr && m_end.At(i) <= end)
		{
			m_begin.RemoveAt(i);
			m_end.RemoveAt(i);
		}
	}
	return LabelFenceReleaseStatus::Released;
}

void LabelFenceRegistry::Snapshot(Vector<uint64_t>* begin, Vector<uint64_t>* end) const
{
	EXIT_IF(begin == nullptr);
	EXIT_IF(end == nullptr);

	begin->Clear();
	end->Clear();
	for (int i = 0; i < static_cast<int>(m_begin.Size()); i++)
	{
		begin->Add(m_begin.At(i));
		end->Add(m_end.At(i));
	}
}

void LabelManager::RegisterFenceHole(uint64_t addr, uint64_t bytes)
{
	const auto result = m_fence_holes.Register(addr, bytes);
	EXIT_NOT_IMPLEMENTED(result == LabelFenceRegistrationStatus::InvalidArgument);
}

void LabelManager::FireCallbacks(const Vector<LabelCallbacks>& fired_labels)
{
	static const bool eop_trace = (std::getenv("KYTY_EOP_TRACE") != nullptr);
	Vector<bool>      allow_store;

	// Phase 1: side effects that may write guest memory (WriteBack / GDS).
	// Defer EOP stores so a later WriteBack cannot zero an earlier fence
	// (WaitRegMem64 timeout val=0 ref=1).
	for (auto& label: fired_labels)
	{
		bool write = true;
		if (label.callback_1 != nullptr)
		{
			write = label.callback_1(label.submission, label.args);
		}
		allow_store.Add(write);
	}

	// Phase 2: publish EOP fence values after all WriteBacks.
	for (int i = 0; i < static_cast<int>(fired_labels.Size()); i++)
	{
		if (!allow_store.At(i))
		{
			continue;
		}
		auto& label = fired_labels.At(i);
		if (label.dst_gpu_addr64 != nullptr)
		{
			*label.dst_gpu_addr64 = label.value64;

			printf(FG_BRIGHT_GREEN "EndOfPipe Signal!!! [0x%016" PRIx64 "] <- 0x%016" PRIx64 "\n" FG_DEFAULT,
			       reinterpret_cast<uint64_t>(label.dst_gpu_addr64), label.value64);
		}

		if (label.dst_gpu_addr32 != nullptr)
		{
			*label.dst_gpu_addr32 = label.value32;

			printf(FG_BRIGHT_GREEN "EndOfPipe Signal!!! [0x%016" PRIx64 "] <- 0x%08" PRIx32 "\n" FG_DEFAULT,
			       reinterpret_cast<uint64_t>(label.dst_gpu_addr32), label.value32);
		}
	}

	// Phase 3: interrupts / flips that depend on fence memory being visible.
	uint32_t interrupt_cbs = 0;
	for (auto& label: fired_labels)
	{
		if (label.callback_2 != nullptr)
		{
			interrupt_cbs++;
			label.callback_2(label.submission, label.args);
		}
	}

	if (eop_trace && !fired_labels.IsEmpty())
	{
		static std::atomic<uint32_t> fire_logs {0};
		const uint32_t               n = fire_logs.fetch_add(1);
		if (n < 128u)
		{
			uint32_t fence32 = 0;
			uint32_t fence64 = 0;
			uint32_t flip_only = 0;
			for (int i = 0; i < static_cast<int>(fired_labels.Size()); i++)
			{
				const auto& label = fired_labels.At(i);
				if (label.dst_gpu_addr64 != nullptr)
				{
					fence64++;
				} else if (label.dst_gpu_addr32 != nullptr)
				{
					fence32++;
				} else if (label.callback_1 != nullptr)
				{
					flip_only++;
				}
			}
			std::fprintf(stderr,
			             "EOP_FIRE labels=%u interrupt_cbs=%u fence32=%u fence64=%u flip_only=%u\n",
			             static_cast<unsigned>(fired_labels.Size()), interrupt_cbs, fence32, fence64, flip_only);
			// Last few fires before a present cliff: dump published fence words.
			if (n >= 96u)
			{
				for (int i = 0; i < static_cast<int>(fired_labels.Size()); i++)
				{
					if (!allow_store.At(i))
					{
						continue;
					}
					const auto& label = fired_labels.At(i);
					if (label.dst_gpu_addr64 != nullptr)
					{
						std::fprintf(stderr, "EOP_FENCE64 addr=0x%016" PRIx64 " val=0x%016" PRIx64 "\n",
						             reinterpret_cast<uint64_t>(label.dst_gpu_addr64), *label.dst_gpu_addr64);
					}
					if (label.dst_gpu_addr32 != nullptr)
					{
						std::fprintf(stderr, "EOP_FENCE32 addr=0x%016" PRIx64 " val=0x%08" PRIx32 "\n",
						             reinterpret_cast<uint64_t>(label.dst_gpu_addr32), *label.dst_gpu_addr32);
					}
				}
			}
		}
	}
}

void LabelManager::ThreadRun(void* data)
{
	auto* manager = static_cast<LabelManager*>(data);

	for (;;)
	{
		manager->m_mutex.Lock();

		int active_count = 0;

		Vector<Label*>         deleted_labels;
		Vector<Label*>         deferred_destroy;
		Vector<LabelCallbacks> fired_labels;

		for (auto& label: manager->m_labels)
		{
			if (label->status == LabelStatus::Active || label->status == LabelStatus::ActiveDeleted)
			{
				// Bound labels become visible only after their exact submission
				// fence completes. Event polling is retained solely for any
				// legacy unbound label and is never authoritative for a bound EOP.
				if (manager->m_submission_labels.IsBound(reinterpret_cast<uint64_t>(label)))
				{
					continue;
				}
				active_count++;

				if (vkGetEventStatus(label->device, label->event) == VK_EVENT_SET)
				{
					if (label->status == LabelStatus::ActiveDeleted)
					{
						deleted_labels.Add(label);
					}

					label->status = LabelStatus::NotActive;

					fired_labels.Add(label->callbacks);
				}
			}
		}

		for (auto& label: manager->m_deferred_destroy)
		{
			deferred_destroy.Add(label);
		}
		manager->m_deferred_destroy.Clear();

		if (active_count == 0 && deferred_destroy.IsEmpty())
		{
			manager->m_cond_var.Wait(&manager->m_mutex);
		}

		for (auto& label: deleted_labels)
		{
			bool removed = manager->Remove(label);
			EXIT_NOT_IMPLEMENTED(!removed);
		}

		manager->m_mutex.Unlock();

		for (auto& label: deleted_labels)
		{
			Destroy(label);
		}
		for (auto& label: deferred_destroy)
		{
			Destroy(label);
		}

		FireCallbacks(fired_labels);

		Core::Thread::SleepMicro(100);
	}
}

void LabelManager::DrainCompleted()
{
	Vector<LabelCallbacks> fired_labels;

	{
		Core::LockGuard lock(m_mutex);

		for (auto& label: m_labels)
		{
			if (label->status == LabelStatus::Active &&
			    !m_submission_labels.IsBound(reinterpret_cast<uint64_t>(label)) &&
			    vkGetEventStatus(label->device, label->event) == VK_EVENT_SET)
			{
				label->status = LabelStatus::NotActive;
				fired_labels.Add(label->callbacks);
			}
		}
	}

	FireCallbacks(fired_labels);
}

void LabelManager::CompleteSubmission(SubmissionId submission)
{
	EXIT_IF(submission.sequence == 0);

	Vector<LabelCallbacks> fired_labels;
	Vector<Label*>         destroy_labels;
	Vector<LabelSubmissionCompletion> completions;

	{
		Core::LockGuard lock(m_mutex);

		const auto result = m_submission_labels.TakeCompleted(submission, &completions);
		EXIT_NOT_IMPLEMENTED(result != LabelSubmissionResult::Success);

		for (const auto& completion: completions)
		{
			auto* label = reinterpret_cast<Label*>(completion.token);
			auto  index = m_labels.Find(label);
			EXIT_NOT_IMPLEMENTED(!m_labels.IndexValid(index));

			const auto action = LabelForceCompleteActionFor(label->status == LabelStatus::Active,
			                                                label->status == LabelStatus::ActiveDeleted);
			EXIT_NOT_IMPLEMENTED(action == LabelForceCompleteKind::Skip);
			const auto tracked_action = (completion.kind == LabelSubmissionCompletionKind::Destroy
			                                 ? LabelForceCompleteKind::FireDestroy
			                                 : LabelForceCompleteKind::FireKeep);
			EXIT_NOT_IMPLEMENTED(action != tracked_action);

			label->status                     = LabelStatus::NotActive;
			label->exact_submission_completed = true;
			fired_labels.Add(label->callbacks);
			if (action == LabelForceCompleteKind::FireDestroy)
			{
				destroy_labels.Add(label);
			}
		}

		for (auto& label: destroy_labels)
		{
			auto index = m_labels.Find(label);
			EXIT_NOT_IMPLEMENTED(!m_labels.IndexValid(index));
			m_labels.RemoveAt(index);
		}
	}

	FireCallbacks(fired_labels);

	for (auto& label: destroy_labels)
	{
		Destroy(label);
	}
}

void LabelManager::WriteBackCopy(void* guest_dst, const void* gpu_src, uint64_t size)
{
	EXIT_IF(guest_dst == nullptr);
	EXIT_IF(gpu_src == nullptr);

	Vector<uint64_t> hole_begin;
	Vector<uint64_t> hole_end;

	{
		Core::LockGuard lock(m_mutex);
		m_fence_holes.Snapshot(&hole_begin, &hole_end);
	}

	static const bool eop_trace = (std::getenv("KYTY_EOP_TRACE") != nullptr);
	if (eop_trace)
	{
		const auto* dst_bytes = static_cast<const uint8_t*>(guest_dst);
		const auto* src_bytes = static_cast<const uint8_t*>(gpu_src);
		const uint64_t base   = reinterpret_cast<uint64_t>(guest_dst);
		for (int i = 0; i < static_cast<int>(hole_begin.Size()); i++)
		{
			const uint64_t a = hole_begin.At(i);
			const uint64_t b = hole_end.At(i);
			if (a < base || b > base + size || b <= a || (b - a) > 8u)
			{
				continue;
			}
			uint64_t before = 0;
			uint64_t after  = 0;
			const uint64_t n = b - a;
			std::memcpy(&before, dst_bytes + (a - base), static_cast<size_t>(n));
			std::memcpy(&after, src_bytes + (a - base), static_cast<size_t>(n));
			if (before != 0 && after == 0)
			{
				static std::atomic<uint32_t> clobber_logs {0};
				if (clobber_logs.fetch_add(1) < 32u)
				{
					std::fprintf(stderr, "EOP_HOLE_PROTECT addr=0x%016" PRIx64 " keep=0x%016" PRIx64 " would_clobber=0\n", a, before);
				}
			}
		}
	}

	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(guest_dst), size);
	MemcpySkipAbsoluteRanges(guest_dst, gpu_src, size, hole_begin.GetData(), hole_end.GetData(), static_cast<int>(hole_begin.Size()));
}

void LabelManager::ReleaseMappedRange(uint64_t addr, uint64_t bytes)
{
	Core::LockGuard lock(m_mutex);
	const auto      result = m_fence_holes.ReleaseAllocation(addr, bytes);
	if (result == LabelFenceReleaseStatus::InvalidArgument || result == LabelFenceReleaseStatus::PartialOverlap)
	{
		EXIT("Label fence allocation release failed: address=0x%016" PRIx64 ", size=0x%016" PRIx64 ", status=%u\n", addr, bytes,
		     static_cast<uint32_t>(result));
	}
}

Label* LabelManager::Create64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelCallback callback_1,
                              LabelCallback callback_2, const uint64_t* args)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(args == nullptr);

	Core::LockGuard lock(m_mutex);

	auto* label = new Label;

	label->status                   = LabelStatus::New;
	label->callbacks.dst_gpu_addr64 = dst_gpu_addr;
	label->callbacks.value64        = value;
	label->callbacks.dst_gpu_addr32 = nullptr;
	label->callbacks.value32        = 0;
	label->event                    = nullptr;
	label->device                   = ctx->device;
	label->callbacks.callback_1     = callback_1;
	label->callbacks.callback_2     = callback_2;
	label->cp                       = nullptr;

	for (int i = 0; i < LABEL_ARGS_MAX; i++)
	{
		label->callbacks.args[i] = args[i];
	}

	if (dst_gpu_addr != nullptr)
	{
		RegisterFenceHole(reinterpret_cast<uint64_t>(dst_gpu_addr), 8u);
	}

	VkEventCreateInfo create_info {};
	create_info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
	create_info.pNext = nullptr;
	create_info.flags = 0;

	vkCreateEvent(ctx->device, &create_info, nullptr, &label->event);

	EXIT_NOT_IMPLEMENTED(label->event == nullptr);

	m_labels.Add(label);

	return label;
}

Label* LabelManager::Create32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, uint32_t dst_word_count,
                              LabelCallback callback_1, LabelCallback callback_2, const uint64_t* args)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(args == nullptr);

	Core::LockGuard lock(m_mutex);

	auto* label = new Label;

	label->status                   = LabelStatus::New;
	label->callbacks.dst_gpu_addr32 = dst_gpu_addr;
	label->callbacks.value32        = value;
	label->callbacks.dst_gpu_addr64 = nullptr;
	label->callbacks.value64        = 0;
	label->event                    = nullptr;
	label->device                   = ctx->device;
	label->callbacks.callback_1     = callback_1;
	label->callbacks.callback_2     = callback_2;
	label->cp                       = nullptr;

	for (int i = 0; i < LABEL_ARGS_MAX; i++)
	{
		label->callbacks.args[i] = args[i];
	}

	if (dst_gpu_addr != nullptr && dst_word_count != 0)
	{
		RegisterFenceHole(reinterpret_cast<uint64_t>(dst_gpu_addr), LabelDwordStoreSizeBytes(dst_word_count));
	}

	VkEventCreateInfo create_info {};
	create_info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
	create_info.pNext = nullptr;
	create_info.flags = 0;

	vkCreateEvent(ctx->device, &create_info, nullptr, &label->event);

	EXIT_NOT_IMPLEMENTED(label->event == nullptr);

	m_labels.Add(label);

	return label;
}

bool LabelManager::Remove(Label* label)
{
	EXIT_IF(label == nullptr);
	EXIT_IF(label->event == nullptr);
	EXIT_IF(label->device == nullptr);

	Core::LockGuard lock(m_mutex);

	auto index = m_labels.Find(label);

	EXIT_NOT_IMPLEMENTED(!m_labels.IndexValid(index));

	EXIT_NOT_IMPLEMENTED(label->status != LabelStatus::NotActive && label->status != LabelStatus::Active);

	if (label->status == LabelStatus::Active)
	{
		const auto result = m_submission_labels.MarkDeleted(reinterpret_cast<uint64_t>(label));
		EXIT_NOT_IMPLEMENTED(result != LabelSubmissionResult::Success);
		label->status = LabelStatus::ActiveDeleted;

		return false;
	}

	m_labels.RemoveAt(index);

	return true;
}

void LabelManager::Destroy(Label* label)
{
	EXIT_IF(label == nullptr);
	EXIT_IF(label->event == nullptr);
	EXIT_IF(label->device == nullptr);
	EXIT_IF(label->cp == nullptr);

	if (!label->exact_submission_completed)
	{
		// Legacy/unbound event polling proves only that vkCmdSetEvent executed;
		// it does not prove that every later command referencing the event has
		// completed. Exact-bound labels arrive here only after their fence.
		GraphicsRunCommandProcessorWait(label->cp);
	}

	vkDestroyEvent(label->device, label->event, nullptr);

	delete label;
}

void LabelManager::Delete(Label* label)
{
	if (Remove(label))
	{
		Destroy(label);
	}
}

void LabelManager::Set(CommandBuffer* buffer, Label* label)
{
	EXIT_IF(label == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());
	EXIT_IF(label->event == nullptr);
	EXIT_IF(label->device == nullptr);

	Core::LockGuard lock(m_mutex);

	auto index = m_labels.Find(label);

	EXIT_NOT_IMPLEMENTED(!m_labels.IndexValid(index));

	EXIT_NOT_IMPLEMENTED(label->status != LabelStatus::New && label->status != LabelStatus::NotActive);

	SubmissionId submission;
	EXIT_NOT_IMPLEMENTED(!buffer->GetSubmissionId(&submission));
	const auto bind_result = m_submission_labels.Bind(reinterpret_cast<uint64_t>(label), submission);
	EXIT_NOT_IMPLEMENTED(bind_result != LabelSubmissionResult::Success);

	label->callbacks.submission          = submission;
	label->status                        = LabelStatus::Active;
	label->exact_submission_completed = false;

	EXIT_IF(label->event == nullptr);

	label->cp = buffer->GetParent();

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	EXIT_NOT_IMPLEMENTED(vk_buffer == nullptr);

	vkResetEvent(label->device, label->event);
	vkCmdSetEvent(vk_buffer, label->event, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	m_cond_var.Signal();
}

void LabelInit()
{
	EXIT_IF(g_label_manager != nullptr);

	g_label_manager = new LabelManager;
}

Label* LabelCreate64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelCallback callback_1,
                     LabelCallback callback_2, const uint64_t* args)
{
	EXIT_IF(g_label_manager == nullptr);

	return g_label_manager->Create64(ctx, dst_gpu_addr, value, callback_1, callback_2, args);
}

Label* LabelCreate32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, uint32_t dst_word_count,
                     LabelCallback callback_1, LabelCallback callback_2, const uint64_t* args)
{
	EXIT_IF(g_label_manager == nullptr);

	return g_label_manager->Create32(ctx, dst_gpu_addr, value, dst_word_count, callback_1, callback_2, args);
}

void LabelDelete(Label* label)
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->Delete(label);
}

void LabelSet(CommandBuffer* buffer, Label* label)
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->Set(buffer, label);
}

void LabelDrainCompleted()
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->DrainCompleted();
}

void LabelCompleteSubmission(SubmissionId submission)
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->CompleteSubmission(submission);
}

void LabelWriteBackCopy(void* guest_dst, const void* gpu_src, uint64_t size)
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->WriteBackCopy(guest_dst, gpu_src, size);
}

void LabelReleaseMappedRange(uint64_t addr, uint64_t bytes)
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->ReleaseMappedRange(addr, bytes);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
