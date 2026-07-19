#include "Emulator/Graphics/Objects/Label.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

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
	uint64_t*                  dst_gpu_addr64       = nullptr;
	uint64_t                   value64              = 0;
	uint32_t*                  dst_gpu_addr32       = nullptr;
	uint32_t                   value32              = 0;
	LabelGpuObject::callback_t callback_1           = nullptr;
	LabelGpuObject::callback_t callback_2           = nullptr;
	uint64_t                   args[LABEL_ARGS_MAX] = {};
};

struct Label
{
	VkDevice          device = nullptr;
	VkEvent           event  = nullptr;
	LabelStatus       status = LabelStatus::New;
	LabelCallbacks    callbacks;
	CommandProcessor* cp = nullptr;
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

	Label* Create64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelGpuObject::callback_t callback_1,
	                LabelGpuObject::callback_t callback_2, const uint64_t* args);
	Label* Create32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, LabelGpuObject::callback_t callback_1,
	                LabelGpuObject::callback_t callback_2, const uint64_t* args);
	void   Delete(Label* label);
	void   Set(CommandBuffer* buffer, Label* label);
	void   DrainCompleted();
	void   CompleteSubmitted(CommandProcessor* cp);
	void   WriteBackCopy(void* guest_dst, const void* gpu_src, uint64_t size);

private:
	static void ThreadRun(void* data);
	static void FireCallbacks(const Vector<LabelCallbacks>& fired_labels);

	bool        Remove(Label* label);
	static void Destroy(Label* label);

	Core::Mutex    m_mutex;
	Core::CondVar  m_cond_var;
	Vector<Label*> m_labels;
	// OnlyFlip ActiveDeleted labels force-completed under BufferFlush: destroy
	// here (Label thread) so GraphicsRunCommandProcessorWait is not nested under
	// the CommandProcessor mutex.
	Vector<Label*> m_deferred_destroy;
	// Tracked labels are reclaimed from GpuMemory on this thread after their
	// callbacks finish, outside CommandProcessor and GpuMemory lock nesting.
	Vector<Label*> m_completed_tracked;
	// Durable WriteBack holes for fence addresses that have ever been a Label
	// dst. Survives GpuMemory Label reclaim / delete so StorageBuffer WriteBack
	// cannot zero guest EOP words after the Label object is gone.
	Vector<uint64_t> m_fence_hole_begin;
	Vector<uint64_t> m_fence_hole_end;

	void RegisterFenceHole(uint64_t addr, uint64_t bytes);
};

static LabelManager* g_label_manager = nullptr;

void LabelManager::RegisterFenceHole(uint64_t addr, uint64_t bytes)
{
	EXIT_IF(bytes == 0);
	for (int i = 0; i < static_cast<int>(m_fence_hole_begin.Size()); i++)
	{
		if (m_fence_hole_begin.At(i) == addr && (m_fence_hole_end.At(i) - m_fence_hole_begin.At(i)) == bytes)
		{
			return;
		}
	}
	m_fence_hole_begin.Add(addr);
	m_fence_hole_end.Add(addr + bytes);
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
			write = label.callback_1(label.args);
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
			label.callback_2(label.args);
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
		Vector<Label*>         completed_tracked;
		Vector<LabelCallbacks> fired_labels;

		for (auto& label: manager->m_labels)
		{
			if (label->status == LabelStatus::Active || label->status == LabelStatus::ActiveDeleted)
			{
				active_count++;

				if (vkGetEventStatus(label->device, label->event) == VK_EVENT_SET)
				{
					if (label->status == LabelStatus::ActiveDeleted)
					{
						deleted_labels.Add(label);
					} else
					{
						completed_tracked.Add(label);
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
		for (auto& label: manager->m_completed_tracked)
		{
			completed_tracked.Add(label);
		}
		manager->m_completed_tracked.Clear();

		if (active_count == 0 && deferred_destroy.IsEmpty() && completed_tracked.IsEmpty())
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
		for (auto& label: completed_tracked)
		{
			GpuMemoryDeleteLabel(label);
		}

		Core::Thread::SleepMicro(100);
	}
}

void LabelManager::DrainCompleted()
{
	Vector<LabelCallbacks> fired_labels;
	Vector<Label*>         completed_labels;

	{
		Core::LockGuard lock(m_mutex);

		for (auto& label: m_labels)
		{
			if (label->status == LabelStatus::Active && vkGetEventStatus(label->device, label->event) == VK_EVENT_SET)
			{
				label->status = LabelStatus::NotActive;
				fired_labels.Add(label->callbacks);
				completed_labels.Add(label);
			}
		}
	}

	FireCallbacks(fired_labels);
	if (!completed_labels.IsEmpty())
	{
		Core::LockGuard lock(m_mutex);
		for (auto& label: completed_labels)
		{
			m_completed_tracked.Add(label);
		}
		m_cond_var.Signal();
	}
}

void LabelManager::CompleteSubmitted(CommandProcessor* cp)
{
	EXIT_IF(cp == nullptr);

	Vector<LabelCallbacks> fired_labels;
	Vector<Label*>         destroy_labels;
	Vector<Label*>         completed_labels;

	{
		Core::LockGuard lock(m_mutex);

		for (auto& label: m_labels)
		{
			if (label->cp != cp)
			{
				continue;
			}

			const auto action = LabelForceCompleteActionFor(label->status == LabelStatus::Active,
			                                                label->status == LabelStatus::ActiveDeleted);
			if (action == LabelForceCompleteKind::Skip)
			{
				continue;
			}

			label->status = LabelStatus::NotActive;
			fired_labels.Add(label->callbacks);
			if (action == LabelForceCompleteKind::FireDestroy)
			{
				destroy_labels.Add(label);
			} else
			{
				completed_labels.Add(label);
			}
		}

		for (auto& label: destroy_labels)
		{
			auto index = m_labels.Find(label);
			EXIT_NOT_IMPLEMENTED(!m_labels.IndexValid(index));
			m_labels.RemoveAt(index);
			m_deferred_destroy.Add(label);
		}

		if (!destroy_labels.IsEmpty())
		{
			m_cond_var.Signal();
		}
	}

	FireCallbacks(fired_labels);
	if (!completed_labels.IsEmpty())
	{
		Core::LockGuard lock(m_mutex);
		for (auto& label: completed_labels)
		{
			m_completed_tracked.Add(label);
		}
		m_cond_var.Signal();
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

		for (int i = 0; i < static_cast<int>(m_fence_hole_begin.Size()); i++)
		{
			hole_begin.Add(m_fence_hole_begin.At(i));
			hole_end.Add(m_fence_hole_end.At(i));
		}

		auto add_holes = [&](Label* label) {
			if (label->callbacks.dst_gpu_addr64 != nullptr)
			{
				const uint64_t addr = reinterpret_cast<uint64_t>(label->callbacks.dst_gpu_addr64);
				hole_begin.Add(addr);
				hole_end.Add(addr + 8u);
			}
			if (label->callbacks.dst_gpu_addr32 != nullptr)
			{
				const uint64_t addr = reinterpret_cast<uint64_t>(label->callbacks.dst_gpu_addr32);
				hole_begin.Add(addr);
				hole_end.Add(addr + 4u);
			}
		};

		for (auto& label: m_labels)
		{
			add_holes(label);
		}
		for (auto& label: m_deferred_destroy)
		{
			add_holes(label);
		}
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

	MemcpySkipAbsoluteRanges(guest_dst, gpu_src, size, hole_begin.GetData(), hole_end.GetData(), static_cast<int>(hole_begin.Size()));
}

Label* LabelManager::Create64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelGpuObject::callback_t callback_1,
                              LabelGpuObject::callback_t callback_2, const uint64_t* args)
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

Label* LabelManager::Create32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, LabelGpuObject::callback_t callback_1,
                              LabelGpuObject::callback_t callback_2, const uint64_t* args)
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

	if (dst_gpu_addr != nullptr)
	{
		RegisterFenceHole(reinterpret_cast<uint64_t>(dst_gpu_addr), 4u);
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

	// All submitted commands that refer to event must have completed execution
	GraphicsRunCommandProcessorWait(label->cp);

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

	label->status = LabelStatus::Active;

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

Label* LabelCreate64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelGpuObject::callback_t callback_1,
                     LabelGpuObject::callback_t callback_2, const uint64_t* args)
{
	EXIT_IF(g_label_manager == nullptr);

	return g_label_manager->Create64(ctx, dst_gpu_addr, value, callback_1, callback_2, args);
}

Label* LabelCreate32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, LabelGpuObject::callback_t callback_1,
                     LabelGpuObject::callback_t callback_2, const uint64_t* args)
{
	EXIT_IF(g_label_manager == nullptr);

	return g_label_manager->Create32(ctx, dst_gpu_addr, value, callback_1, callback_2, args);
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

void LabelCompleteSubmitted(CommandProcessor* cp)
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->CompleteSubmitted(cp);
}

void LabelWriteBackCopy(void* guest_dst, const void* gpu_src, uint64_t size)
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->WriteBackCopy(guest_dst, gpu_src, size);
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* /*mem*/)
{
	KYTY_PROFILER_BLOCK("LabelGpuObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0);

	EXIT_NOT_IMPLEMENTED(*size != 8 && *size != 4);

	auto value      = params[LabelGpuObject::PARAM_VALUE];
	auto callback_1 = reinterpret_cast<LabelGpuObject::callback_t>(params[LabelGpuObject::PARAM_CALLBACK_1]);
	auto callback_2 = reinterpret_cast<LabelGpuObject::callback_t>(params[LabelGpuObject::PARAM_CALLBACK_2]);

	auto* label_obj = (*size == 8 ? LabelCreate64(ctx, reinterpret_cast<uint64_t*>(*vaddr), value, callback_1, callback_2,
	                                              params + LabelGpuObject::PARAM_ARG_1)
	                              : (*size == 4 ? LabelCreate32(ctx, reinterpret_cast<uint32_t*>(*vaddr), static_cast<uint32_t>(value),
	                                                            callback_1, callback_2, params + LabelGpuObject::PARAM_ARG_1)
	                                            : nullptr));

	EXIT_NOT_IMPLEMENTED(label_obj == nullptr);

	return label_obj;
}

static void update_func(GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* /*obj*/, const uint64_t* /*vaddr*/,
                        const uint64_t* /*size*/, int /*vaddr_num*/)
{
	KYTY_PROFILER_BLOCK("LabelGpuObject::update_func");

	KYTY_NOT_IMPLEMENTED;
}

static void delete_func(GraphicContext* /*ctx*/, void* obj, VulkanMemory* /*mem*/)
{
	KYTY_PROFILER_BLOCK("LabelGpuObject::delete_func");

	auto* label_obj = reinterpret_cast<Label*>(obj);

	EXIT_IF(label_obj == nullptr);

	LabelDelete(label_obj);
}

bool LabelGpuObject::Equal(const uint64_t* /*other*/) const
{
	// Each EOP submission owns a distinct event. The guest may reuse the same
	// fence address and value before the previous command has completed.
	return false;
}

GpuObject::create_func_t LabelGpuObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::delete_func_t LabelGpuObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t LabelGpuObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
