#include "PthreadInternal.h"

#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Kernel/Errors.h"

#include <cinttypes>
#include <iterator>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

namespace GuestRuntimePort = ::Kyty::Emulator::GuestRuntimePort;

void* PthreadStaticObjects::CreateObject(void* addr, PthreadStaticObject::Type type)
{
	Core::LockGuard lock(m_mutex);

	// A statically-initialized pthread object holds a small sentinel, not a real
	// handle: 0 = default initializer, 1 = adaptive, etc. (matches the FreeBSD/Orbis
	// PTHREAD_*_INITIALIZER values). Once initialized, the slot holds a real heap
	// pointer (a large address). Initialize on any sentinel; treat a large value as
	// an already-created object. Treating the sentinel 1 as a pointer would fault.
	if (addr == nullptr)
	{
		return addr;
	}
	if (uint64_t v = *static_cast<uint64_t*>(addr); v >= 0x100000)
	{
		return addr;
	}

	auto  vaddr   = reinterpret_cast<uint64_t>(addr);
	auto* program = GuestRuntimePort::FindProgramByAddr(vaddr);

	EXIT_NOT_IMPLEMENTED(program == nullptr);

	auto* obj    = new PthreadStaticObject;
	obj->program = program;
	obj->type    = type;
	obj->vaddr   = vaddr;

	String name = String::FromPrintf("Static%016" PRIx64, vaddr);

	int result = OK;
	switch (type)
	{
		case PthreadStaticObject::Type::Mutex: result = PthreadMutexInit(static_cast<PthreadMutex*>(addr), nullptr, name.C_Str()); break;
		case PthreadStaticObject::Type::Cond: result = PthreadCondInit(static_cast<PthreadCond*>(addr), nullptr, name.C_Str()); break;
		case PthreadStaticObject::Type::Rwlock: result = PthreadRwlockInit(static_cast<PthreadRwlock*>(addr), nullptr, name.C_Str()); break;
		default: EXIT("unknown type: %d\n", static_cast<int>(type));
	}

	EXIT_NOT_IMPLEMENTED(result != OK);

	auto index = m_objects.Find(nullptr);

	if (m_objects.IndexValid(index))
	{
		m_objects[index] = obj;
	} else
	{
		m_objects.Add(obj);
	}

	return addr;
}

void PthreadStaticObjects::DeleteObjects(const void* program)
{
	Core::LockGuard lock(m_mutex);

	for (auto& obj: m_objects)
	{
		if (obj != nullptr && obj->program == program)
		{
			int result = OK;
			switch (obj->type)
			{
				case PthreadStaticObject::Type::Mutex: result = PthreadMutexDestroy(reinterpret_cast<PthreadMutex*>(obj->vaddr)); break;
				case PthreadStaticObject::Type::Cond: result = PthreadCondDestroy(reinterpret_cast<PthreadCond*>(obj->vaddr)); break;
				case PthreadStaticObject::Type::Rwlock: result = PthreadRwlockDestroy(reinterpret_cast<PthreadRwlock*>(obj->vaddr)); break;
				default: EXIT("unknown type: %d\n", static_cast<int>(obj->type));
			}

			EXIT_NOT_IMPLEMENTED(result != OK);

			delete obj;
			obj = nullptr;
		}
	}
}

Pthread PthreadPool::Create()
{
	Core::LockGuard lock(m_mutex);

	for (auto* p: m_threads)
	{
		if (p->free)
		{
			p->free = false;
			return p;
		}
	}

	auto* ret = new PthreadPrivate {};

	ret->free        = false;
	ret->detached    = false;
	ret->almost_done = false;
	ret->attr        = nullptr;

	m_threads.Add(ret);

	return ret;
}

void PthreadPool::FreeDetachedThreads()
{
	Core::LockGuard lock(m_mutex);

	for (auto* p: m_threads)
	{
		if (p->detached && p->almost_done && !p->free)
		{
			PthreadJoin(p, nullptr);
		}
	}
}

void PthreadPool::GetDiagnostics(PthreadThreadDiagnostics* out)
{
	EXIT_IF(out == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto* thread: m_threads)
	{
		out->allocated_count++;
		if (!thread->free.load())
		{
			out->active_count++;
		}
		if (out->thread_count >= std::size(out->threads))
		{
			continue;
		}

		auto& snapshot = out->threads[out->thread_count++];
		snapshot.entry = reinterpret_cast<uint64_t>(thread->entry);
		snapshot.argument = reinterpret_cast<uint64_t>(thread->arg);
		snapshot.unique_id = thread->unique_id;
		snapshot.started = thread->started.load();
		snapshot.detached = thread->detached.load();
		snapshot.almost_done = thread->almost_done.load();
		snapshot.free = thread->free.load();
	}
}

bool PthreadPool::QueryStack(uint64_t addr, uint64_t* start, uint64_t* end)
{
	EXIT_IF(start == nullptr || end == nullptr);

	Core::LockGuard lock(m_mutex);
	for (auto* thread: m_threads)
	{
		const uint64_t base = thread->guest_stack_base;
		const uint64_t size = thread->guest_stack_size;
		if (!thread->free.load(std::memory_order_acquire) && size != 0 && addr >= base && addr - base < size)
		{
			*start = base;
			*end   = base + size;
			return true;
		}
	}
	return false;
}

bool PthreadQueryStack(const void* addr, void** start, void** end)
{
	if (g_pthread_context == nullptr || addr == nullptr)
	{
		return false;
	}

	const uint64_t query_addr = reinterpret_cast<uint64_t>(addr);
	if (g_pthread_self != nullptr && g_pthread_self->guest_stack_size != 0 && query_addr >= g_pthread_self->guest_stack_base &&
	    query_addr - g_pthread_self->guest_stack_base < g_pthread_self->guest_stack_size)
	{
		if (start != nullptr)
		{
			*start = reinterpret_cast<void*>(g_pthread_self->guest_stack_base);
		}
		if (end != nullptr)
		{
			*end = reinterpret_cast<void*>(g_pthread_self->guest_stack_base + g_pthread_self->guest_stack_size);
		}
		return true;
	}

	uint64_t stack_start = 0;
	uint64_t stack_end   = 0;
	if (!g_pthread_context->GetPthreadPool()->QueryStack(query_addr, &stack_start, &stack_end))
	{
		return false;
	}
	if (start != nullptr)
	{
		*start = reinterpret_cast<void*>(stack_start);
	}
	if (end != nullptr)
	{
		*end = reinterpret_cast<void*>(stack_end);
	}
	return true;
}


} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED
