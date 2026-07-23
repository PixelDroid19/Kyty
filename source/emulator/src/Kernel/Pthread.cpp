#include "Emulator/Kernel/Pthread.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DateTime.h"
#include "Kyty/Core/MemoryAlloc.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Singleton.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Timer.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Graphics/Window.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/GuestCall.h"

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <pthread.h>
#include <unistd.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <pthread_time.h>
#ifdef pthread_attr_getguardsize
#undef pthread_attr_getguardsize
#endif
#ifdef pthread_attr_setguardsize
#undef pthread_attr_setguardsize
#endif
#endif

namespace Kyty::Libs {

namespace LibKernel {

LIB_NAME("libkernel", "libkernel");

constexpr int KEYS_MAX              = 256;
constexpr int DESTRUCTOR_ITERATIONS = 4;
constexpr int MUTEX_TYPE_ERRORCHECK = 1;
constexpr int MUTEX_TYPE_RECURSIVE  = 2;
constexpr int MUTEX_TYPE_NORMAL     = 3;
constexpr int MUTEX_TYPE_ADAPTIVE   = 4;

struct PthreadMutexPrivate
{
	uint8_t         reserved[256];
	String          name;
	pthread_mutex_t p;
	std::mutex      state_mutex;
	pthread_t       owner {};
	uint32_t        recursion_count = 0;
	int             type            = MUTEX_TYPE_ERRORCHECK;
};

struct PthreadMutexattrPrivate
{
	uint8_t             reserved[64];
	pthread_mutexattr_t p;
	int                 pprotocol;
	int                 type = MUTEX_TYPE_ERRORCHECK;
};

struct PthreadAttrPrivate
{
	uint8_t        reserved[64];
	KernelCpumask  affinity;
	size_t         guard_size;
	int            policy;
	bool           detached;
	pthread_attr_t p;
};

struct PthreadPrivate
{
	uint8_t              reserved[4096];
	String               name;
	pthread_t            p;
	PthreadAttr          attr;
	pthread_entry_func_t entry;
	void*                arg;
	int                  unique_id;
	std::atomic_bool     started;
	std::atomic_bool     detached;
	std::atomic_bool     almost_done;
	std::atomic_bool     free;
	std::atomic_int      guest_priority {700};
	uint64_t             guest_stack_base = 0;
	uint64_t             guest_stack_size = 0;
};

struct PthreadRwlockPrivate
{
	uint8_t          reserved[256];
	String           name;
	pthread_rwlock_t p;
};

struct PthreadRwlockattrPrivate
{
	uint8_t              reserved[64];
	int                  type;
	pthread_rwlockattr_t p;
};

struct PthreadCondattrPrivate
{
	uint8_t            reserved[64];
	pthread_condattr_t p;
};

struct PthreadCondPrivate
{
	uint8_t        reserved[256];
	String         name;
	pthread_cond_t p;
};

struct PthreadStaticObject
{
	enum class Type
	{
		Mutex,
		Cond,
		Rwlock
	};

	Type             type;
	uint64_t         vaddr;
	Loader::Program* program;
};

class PthreadStaticObjects
{
public:
	PthreadStaticObjects() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PthreadStaticObjects() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PthreadStaticObjects);

	void* CreateObject(void* addr, PthreadStaticObject::Type type);
	void  DeleteObjects(Loader::Program* program);

private:
	Vector<PthreadStaticObject*> m_objects;
	Core::Mutex                  m_mutex;
};

class PthreadKeys
{
public:
	PthreadKeys() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PthreadKeys() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PthreadKeys);

	bool Create(int* key, pthread_key_destructor_func_t destructor);
	bool Delete(int key);
	void Destruct(int thread_id);
	bool Set(int key, int thread_id, void* data);
	bool Get(int key, int thread_id, void** data);

private:
	struct Map
	{
		int   thread_id = -1;
		void* data      = nullptr;
	};

	struct Key
	{
		bool                          used       = false;
		pthread_key_destructor_func_t destructor = nullptr;
		Vector<Map>                   specific_values;
	};

	Core::Mutex m_mutex;
	Key         m_keys[KEYS_MAX];
};

class PthreadPool
{
public:
	PthreadPool() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PthreadPool() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PthreadPool);

	Pthread Create();

	void FreeDetachedThreads();
	void GetDiagnostics(PthreadThreadDiagnostics* out);
	bool QueryStack(uint64_t addr, uint64_t* start, uint64_t* end);

private:
	Vector<Pthread> m_threads;
	Core::Mutex     m_mutex;
};

class PThreadContext
{
public:
	PThreadContext() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PThreadContext() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PThreadContext);

	PthreadAttr*          GetDefaultAttr() { return &m_default_attr; }
	void                  SetDefaultAttr(PthreadAttr attr) { m_default_attr = attr; }
	PthreadCondattr*      GetDefaultCondattr() { return &m_default_condattr; }
	void                  SetDefaultCondattr(PthreadCondattr attr) { m_default_condattr = attr; }
	PthreadMutexattr*     GetDefaultMutexattr() { return &m_default_mutexattr; }
	void                  SetDefaultMutexattr(PthreadMutexattr attr) { m_default_mutexattr = attr; }
	PthreadRwlockattr*    GetDefaultRwlockattr() { return &m_default_rwlockattr; }
	void                  SetDefaultRwlockattr(PthreadRwlockattr attr) { m_default_rwlockattr = attr; }
	PthreadPool*          GetPthreadPool() { return m_pthread_pool; }
	void                  SetPthreadPool(PthreadPool* pool) { m_pthread_pool = pool; }
	PthreadStaticObjects* GetPthreadStaticObjects() { return m_pthread_static_objects; }
	void                  SetPthreadStaticObjects(PthreadStaticObjects* objs) { m_pthread_static_objects = objs; }
	PthreadKeys*          GetPthreadKeys() { return m_pthread_keys; }
	void                  SetPthreadKeys(PthreadKeys* keys) { m_pthread_keys = keys; }

	[[nodiscard]] thread_dtors_func_t GetThreadDtors() const { return m_thread_dtors; }
	void                              SetThreadDtors(thread_dtors_func_t dtors) { m_thread_dtors = dtors; }

private:
	// Core::Mutex           m_mutex;
	PthreadMutexattr      m_default_mutexattr      = nullptr;
	PthreadRwlockattr     m_default_rwlockattr     = nullptr;
	PthreadCondattr       m_default_condattr       = nullptr;
	PthreadAttr           m_default_attr           = nullptr;
	PthreadPool*          m_pthread_pool           = nullptr;
	PthreadStaticObjects* m_pthread_static_objects = nullptr;
	PthreadKeys*          m_pthread_keys           = nullptr;

	std::atomic<thread_dtors_func_t> m_thread_dtors = nullptr;
};

thread_local Pthread g_pthread_self    = nullptr;
PThreadContext*      g_pthread_context = nullptr;

static void FreeDetachedThreads(void* /*arg*/)
{
	PRINT_NAME_ENABLE(false);

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_pool = g_pthread_context->GetPthreadPool();

	EXIT_IF(pthread_pool == nullptr);

	while (true)
	{
		Core::Thread::Sleep(10000);
		pthread_pool->FreeDetachedThreads();
	}
}

void PthreadDeleteStaticObjects(Loader::Program* program)
{
	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	pthread_static_objects->DeleteObjects(program);
}

void PthreadInitSelfForMainThread()
{
	EXIT_IF(g_pthread_self != nullptr);

	g_pthread_self = new PthreadPrivate {};
	PthreadAttrInit(&g_pthread_self->attr);
	g_pthread_self->p           = pthread_self();
	g_pthread_self->name        = "MainThread";
	g_pthread_self->unique_id   = Core::Thread::GetThreadIdUnique();
	g_pthread_self->free        = false;
	g_pthread_self->detached    = false;
	g_pthread_self->almost_done = false;
	g_pthread_self->entry       = nullptr;
	g_pthread_self->arg         = nullptr;
}

KYTY_SUBSYSTEM_INIT(Pthread)
{
	PRINT_NAME_ENABLE(false);

	EXIT_IF(g_pthread_context != nullptr);

	g_pthread_context = new PThreadContext;

	g_pthread_context->SetPthreadStaticObjects(new PthreadStaticObjects);
	g_pthread_context->SetPthreadPool(new PthreadPool);
	g_pthread_context->SetPthreadKeys(new PthreadKeys);

	PthreadMutexattr  default_mutexattr  = nullptr;
	PthreadRwlockattr default_rwlockattr = nullptr;
	PthreadCondattr   default_condattr   = nullptr;
	PthreadAttr       default_attr       = nullptr;

	PthreadAttrInit(&default_attr);
	PthreadMutexattrInit(&default_mutexattr);
	PthreadRwlockattrInit(&default_rwlockattr);
	PthreadCondattrInit(&default_condattr);

	g_pthread_context->SetDefaultMutexattr(default_mutexattr);
	g_pthread_context->SetDefaultRwlockattr(default_rwlockattr);
	g_pthread_context->SetDefaultCondattr(default_condattr);
	g_pthread_context->SetDefaultAttr(default_attr);

	PRINT_NAME_ENABLE(true);

	Core::Thread thread(FreeDetachedThreads, nullptr);
	thread.Detach();
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Pthread) {}

KYTY_SUBSYSTEM_DESTROY(Pthread) {}

static int pthread_attr_copy(PthreadAttr* dst, const PthreadAttr* src)
{
	if (dst == nullptr || *dst == nullptr || src == nullptr || *src == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KernelCpumask    mask          = 0;
	int              state         = 0;
	size_t           guard_size    = 0;
	int              inherit_sched = 0;
	KernelSchedParam param         = {};
	int              policy        = 0;
	void*            stack_addr    = nullptr;
	size_t           stack_size    = 0;

	int result = 0;

	result = (result == 0 ? PthreadAttrGetaffinity(src, &mask) : result);
	result = (result == 0 ? PthreadAttrGetdetachstate(src, &state) : result);
	result = (result == 0 ? PthreadAttrGetguardsize(src, &guard_size) : result);
	result = (result == 0 ? PthreadAttrGetinheritsched(src, &inherit_sched) : result);
	result = (result == 0 ? PthreadAttrGetschedparam(src, &param) : result);
	result = (result == 0 ? PthreadAttrGetschedpolicy(src, &policy) : result);
	result = (result == 0 ? PthreadAttrGetstackaddr(src, &stack_addr) : result);
	result = (result == 0 ? PthreadAttrGetstacksize(src, &stack_size) : result);

	result = (result == 0 ? PthreadAttrSetaffinity(dst, mask) : result);
	result = (result == 0 ? PthreadAttrSetdetachstate(dst, state) : result);
	result = (result == 0 ? PthreadAttrSetguardsize(dst, guard_size) : result);
	result = (result == 0 ? PthreadAttrSetinheritsched(dst, inherit_sched) : result);
	result = (result == 0 ? PthreadAttrSetschedparam(dst, &param) : result);
	result = (result == 0 ? PthreadAttrSetschedpolicy(dst, policy) : result);
	if (stack_addr != nullptr)
	{
		result = (result == 0 ? PthreadAttrSetstackaddr(dst, stack_addr) : result);
	}
	if (stack_size != 0)
	{
		result = (result == 0 ? PthreadAttrSetstacksize(dst, stack_size) : result);
	}

	return result;
}

static void pthread_attr_dbg_print(const PthreadAttr* src)
{
	KernelCpumask    mask          = 0;
	int              state         = 0;
	size_t           guard_size    = 0;
	int              inherit_sched = 0;
	KernelSchedParam param         = {};
	int              policy        = 0;
	void*            stack_addr    = nullptr;
	size_t           stack_size    = 0;

	PthreadAttrGetaffinity(src, &mask);
	PthreadAttrGetdetachstate(src, &state);
	PthreadAttrGetguardsize(src, &guard_size);
	PthreadAttrGetinheritsched(src, &inherit_sched);
	PthreadAttrGetschedparam(src, &param);
	PthreadAttrGetschedpolicy(src, &policy);
	PthreadAttrGetstackaddr(src, &stack_addr);
	PthreadAttrGetstacksize(src, &stack_size);

	printf("\tcpu_mask       = 0x%" PRIx64 "\n", mask);
	printf("\tdetach_state   = %d\n", state);
	printf("\tguard_size     = %" PRIu64 "\n", guard_size);
	printf("\tinherit_sched  = %d\n", inherit_sched);
	printf("\tsched_priority = %d\n", param.sched_priority);
	printf("\tpolicy         = %d\n", policy);
	printf("\tstack_addr     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(stack_addr));
	printf("\tstack_size    = %" PRIu64 "\n", static_cast<uint64_t>(stack_size));
}

static void usec_to_timespec(struct timespec* ts, KernelUseconds usec)
{
	ts->tv_sec  = usec / 1000000;
	ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>((usec % 1000000) * 1000);
}

#ifdef __APPLE__
// macOS has no pthread_rwlock_timedrdlock/timedwrlock; emulate them by polling
// with try-locks. The timespec here is a relative timeout (see usec_to_timespec).
static int rwlock_timedlock_poll(pthread_rwlock_t* lock, const timespec* t, bool write)
{
	uint64_t timeout_us = static_cast<uint64_t>(t->tv_sec) * 1000000u + static_cast<uint64_t>(t->tv_nsec) / 1000u;
	uint64_t waited_us  = 0;
	for (;;)
	{
		int result = (write ? pthread_rwlock_trywrlock(lock) : pthread_rwlock_tryrdlock(lock));
		if (result != EBUSY)
		{
			return result;
		}
		if (waited_us >= timeout_us)
		{
			return ETIMEDOUT;
		}
		usleep(100);
		waited_us += 100;
	}
}

static int pthread_rwlock_timedrdlock(pthread_rwlock_t* lock, const timespec* t)
{
	return rwlock_timedlock_poll(lock, t, false);
}

static int pthread_rwlock_timedwrlock(pthread_rwlock_t* lock, const timespec* t)
{
	return rwlock_timedlock_poll(lock, t, true);
}

static int mutex_timedlock_poll(pthread_mutex_t* mutex, const timespec* t)
{
	uint64_t timeout_us = static_cast<uint64_t>(t->tv_sec) * 1000000u + static_cast<uint64_t>(t->tv_nsec) / 1000u;
	uint64_t waited_us  = 0;
	for (;;)
	{
		int result = pthread_mutex_trylock(mutex);
		if (result != EBUSY)
		{
			return result;
		}
		if (waited_us >= timeout_us)
		{
			return ETIMEDOUT;
		}
		usleep(100);
		waited_us += 100;
	}
}
#endif

static void sec_to_timespec(KernelTimespec* ts, double sec)
{
	ts->tv_sec  = static_cast<int64_t>(sec);
	ts->tv_nsec = static_cast<int64_t>((sec - static_cast<double>(ts->tv_sec)) * 1000000000.0);
}

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

	auto* rt      = Core::Singleton<Loader::RuntimeLinker>::Instance();
	auto  vaddr   = reinterpret_cast<uint64_t>(addr);
	auto* program = rt->FindProgramByAddr(vaddr);

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

void PthreadStaticObjects::DeleteObjects(Loader::Program* program)
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

	uint64_t stack_start = 0;
	uint64_t stack_end   = 0;
	if (!g_pthread_context->GetPthreadPool()->QueryStack(reinterpret_cast<uint64_t>(addr), &stack_start, &stack_end))
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

bool PthreadKeys::Create(int* key, pthread_key_destructor_func_t destructor)
{
	EXIT_IF(key == nullptr);

	Core::LockGuard lock(m_mutex);

	for (int index = 0; index < KEYS_MAX; index++)
	{
		if (!m_keys[index].used)
		{
			*key                     = index;
			m_keys[index].used       = true;
			m_keys[index].destructor = destructor;
			m_keys[index].specific_values.Clear();
			return true;
		}
	}

	return false;
}

bool PthreadKeys::Delete(int key)
{
	Core::LockGuard lock(m_mutex);

	if (key < 0 || key >= KEYS_MAX || !m_keys[key].used)
	{
		return false;
	}

	m_keys[key].used       = false;
	m_keys[key].destructor = nullptr;
	m_keys[key].specific_values.Clear();

	return true;
}

void PthreadKeys::Destruct(int thread_id)
{
	Core::LockGuard lock(m_mutex);

	struct CallInfo
	{
		pthread_key_destructor_func_t destructor;
		void*                         data;
	};

	for (int iter = 0; iter < DESTRUCTOR_ITERATIONS; iter++)
	{
		Vector<CallInfo> delete_list;

		for (auto& key: m_keys)
		{
			if (key.used && key.destructor != nullptr)
			{
				for (auto& v: key.specific_values)
				{
					if (v.thread_id == thread_id && v.data != nullptr)
					{
						delete_list.Add(CallInfo({key.destructor, v.data}));
					}
				}
			}
		}

		if (delete_list.IsEmpty())
		{
			return;
		}

		for (auto& d: delete_list)
		{
			d.destructor(d.data);
		}
	}
}

bool PthreadKeys::Set(int key, int thread_id, void* data)
{
	Core::LockGuard lock(m_mutex);

	if (key < 0 || key >= KEYS_MAX || !m_keys[key].used)
	{
		return false;
	}

	for (auto& v: m_keys[key].specific_values)
	{
		if (v.thread_id == thread_id)
		{
			v.data = data;
			return true;
		}
	}

	m_keys[key].specific_values.Add(Map({thread_id, data}));

	return true;
}

bool PthreadKeys::Get(int key, int thread_id, void** data)
{
	EXIT_IF(data == nullptr);

	Core::LockGuard lock(m_mutex);

	if (key < 0 || key >= KEYS_MAX || !m_keys[key].used)
	{
		return false;
	}

	for (auto& v: m_keys[key].specific_values)
	{
		if (v.thread_id == thread_id)
		{
			*data = v.data;
			return true;
		}
	}

	*data = nullptr;

	return true;
}

int KYTY_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr)
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr);

	*attr = new PthreadMutexattrPrivate {};

	int result = pthread_mutexattr_init(&(*attr)->p);

	result = (result == 0 ? PthreadMutexattrSettype(attr, MUTEX_TYPE_ERRORCHECK) : result);
	result = (result == 0 ? PthreadMutexattrSetprotocol(attr, 0) : result);

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr)
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int result = pthread_mutexattr_destroy(&(*attr)->p);

	delete *attr;
	*attr = nullptr;

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* attr, int type)
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int ptype = PTHREAD_MUTEX_DEFAULT;
	switch (type)
	{
		case MUTEX_TYPE_ERRORCHECK: ptype = PTHREAD_MUTEX_ERRORCHECK; break;
		case MUTEX_TYPE_RECURSIVE: ptype = PTHREAD_MUTEX_RECURSIVE; break;
		case MUTEX_TYPE_NORMAL:
		case MUTEX_TYPE_ADAPTIVE: ptype = PTHREAD_MUTEX_NORMAL; break;
		default: EXIT("invalid type: %d\n", type);
	}

	int result = pthread_mutexattr_settype(&(*attr)->p, ptype);

	if (result == 0)
	{
		(*attr)->type = type;
		return OK;
	}

	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadMutexattrSetprotocol([[maybe_unused]] PthreadMutexattr* attr, int protocol)
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	[[maybe_unused]] int pprotocol = PTHREAD_PRIO_NONE;
	switch (protocol)
	{
		case 0: pprotocol = PTHREAD_PRIO_NONE; break;
		case 1: pprotocol = PTHREAD_PRIO_INHERIT; break;
		case 2: pprotocol = PTHREAD_PRIO_PROTECT; break;
		default: EXIT("invalid protocol: %d\n", protocol);
	}

	// protocol doesn't work in winpthreads
	int result         = 0; // pthread_mutexattr_setprotocol(&(*attr)->p, pprotocol);
	(*attr)->pprotocol = pprotocol;

	if (result == 0)
	{
		return OK;
	}

	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name)
{
	if (name != nullptr)
	{
		PRINT_NAME();
	}

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (attr == nullptr)
	{
		EXIT_IF(g_pthread_context == nullptr);

		attr = g_pthread_context->GetDefaultMutexattr();
	}

	*mutex = new PthreadMutexPrivate {};

	(*mutex)->name = name;
	(*mutex)->type = (*attr)->type;

	int result = pthread_mutex_init(&(*mutex)->p, &(*attr)->p);

	if (name != nullptr)
	{
		printf("\tmutex init: %s, %d\n", (*mutex)->name.C_Str(), result);
	}

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex)
{
	PRINT_NAME();

	if (mutex == nullptr || *mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_mutex_destroy(&(*mutex)->p);

	printf("\tmutex destroy: %s, %d\n", (*mutex)->name.C_Str(), result);

	delete *mutex;
	*mutex = nullptr;

	switch (result)
	{
		case 0: return OK;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex)
{
	// PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	auto* private_mutex = *mutex;
	{
		std::lock_guard lock(private_mutex->state_mutex);
		if (private_mutex->recursion_count != 0 && pthread_equal(private_mutex->owner, pthread_self()) != 0)
		{
			if (private_mutex->type == MUTEX_TYPE_ERRORCHECK)
			{
				return KERNEL_ERROR_EDEADLK;
			}

			// Some guest runtimes layer normal or adaptive lock calls within one
			// logical critical section. Keep their depth in the guest object so a
			// host normal mutex cannot deadlock the owning guest thread.
			private_mutex->recursion_count++;
			return OK;
		}
	}

	int result = pthread_mutex_lock(&private_mutex->p);

	if (result == 0)
	{
		std::lock_guard lock(private_mutex->state_mutex);
		private_mutex->owner           = pthread_self();
		private_mutex->recursion_count = 1;
	}

	// printf("\tmutex lock: %s, %d\n", (*mutex)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex)
{
	// PRINT_NAME();

	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// Lazily initialize a statically-initialized mutex (sentinel), like PthreadMutexLock.
	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	EXIT_IF(pthread_static_objects == nullptr);
	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	auto* private_mutex = *mutex;
	{
		std::lock_guard lock(private_mutex->state_mutex);
		if (private_mutex->recursion_count != 0 && pthread_equal(private_mutex->owner, pthread_self()) != 0)
		{
			if (private_mutex->type == MUTEX_TYPE_RECURSIVE)
			{
				private_mutex->recursion_count++;
				return OK;
			}
			return KERNEL_ERROR_EBUSY;
		}
	}

	int result = pthread_mutex_trylock(&private_mutex->p);

	if (result == 0)
	{
		std::lock_guard lock(private_mutex->state_mutex);
		private_mutex->owner           = pthread_self();
		private_mutex->recursion_count = 1;
	}

	// printf("\tmutex trylock: %s, %d\n", (*mutex)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, KernelUseconds usec)
{
	PRINT_NAME();

	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	EXIT_IF(pthread_static_objects == nullptr);
	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	auto* private_mutex = *mutex;
	{
		std::lock_guard lock(private_mutex->state_mutex);
		if (private_mutex->recursion_count != 0 && pthread_equal(private_mutex->owner, pthread_self()) != 0 &&
		    private_mutex->type == MUTEX_TYPE_RECURSIVE)
		{
			private_mutex->recursion_count++;
			return OK;
		}
	}

	timespec t {};
	usec_to_timespec(&t, usec);

#ifdef __APPLE__
	const int result = mutex_timedlock_poll(&private_mutex->p, &t);
#else
	const int result = pthread_mutex_timedlock(&private_mutex->p, &t);
#endif
	if (result == 0)
	{
		std::lock_guard lock(private_mutex->state_mutex);
		private_mutex->owner           = pthread_self();
		private_mutex->recursion_count = 1;
	}
	switch (result)
	{
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex)
{
	// PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	auto* private_mutex = *mutex;
	std::lock_guard lock(private_mutex->state_mutex);
	if (private_mutex->recursion_count == 0 || pthread_equal(private_mutex->owner, pthread_self()) == 0)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (private_mutex->recursion_count > 1)
	{
		private_mutex->recursion_count--;
		return OK;
	}

	int result = pthread_mutex_unlock(&private_mutex->p);
	if (result == 0)
	{
		private_mutex->owner           = {};
		private_mutex->recursion_count = 0;
	}

	// printf("\tmutex unlock: %s, %d\n", (*mutex)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;

		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EPERM: return KERNEL_ERROR_EPERM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadAttrInit(PthreadAttr* attr)
{
	PRINT_NAME();

	*attr = new PthreadAttrPrivate {};

	int result = pthread_attr_init(&(*attr)->p);

	(*attr)->affinity   = 0x7f;
	(*attr)->guard_size = 0x1000;

	KernelSchedParam param;
	param.sched_priority = 700;

	result = (result == 0 ? PthreadAttrSetinheritsched(attr, 4) : result);
	result = (result == 0 ? PthreadAttrSetschedparam(attr, &param) : result);
	result = (result == 0 ? PthreadAttrSetschedpolicy(attr, 1) : result);
	result = (result == 0 ? PthreadAttrSetdetachstate(attr, 0) : result);

	if (PRINT_NAME_ENABLED)
	{
		pthread_attr_dbg_print(attr);
	}

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadAttrDestroy(PthreadAttr* attr)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int result = pthread_attr_destroy(&(*attr)->p);

	delete *attr;
	*attr = nullptr;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrGet(Pthread thread, PthreadAttr* attr)
{
	PRINT_NAME();

	if (thread == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return pthread_attr_copy(attr, &thread->attr);
}

int KYTY_SYSV_ABI PthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask)
{
	PRINT_NAME();

	if (mask == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*mask = (*attr)->affinity;

	return OK;
}

int KYTY_SYSV_ABI PthreadAttrGetdetachstate(const PthreadAttr* attr, int* state)
{
	PRINT_NAME();

	if (state == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// int result = pthread_attr_getdetachstate(&(*attr)->p, state);
	int result = 0;

	*state = ((*attr)->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);

	switch (*state)
	{
		case PTHREAD_CREATE_JOINABLE: *state = 0; break;
		case PTHREAD_CREATE_DETACHED: *state = 1; break;
		default: EXIT("unknown state: %d\n", *state);
	}

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guard_size)
{
	PRINT_NAME();

	if (guard_size == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*guard_size = (*attr)->guard_size;

	return OK;
}

int KYTY_SYSV_ABI PthreadAttrGetinheritsched(const PthreadAttr* attr, int* inherit_sched)
{
	PRINT_NAME();

	if (inherit_sched == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_attr_getinheritsched(&(*attr)->p, inherit_sched);

	switch (*inherit_sched)
	{
		case PTHREAD_EXPLICIT_SCHED: *inherit_sched = 0; break;
		case PTHREAD_INHERIT_SCHED: *inherit_sched = 4; break;
		default: EXIT("unknown inherit_sched: %d\n", *inherit_sched);
	}

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param)
{
	PRINT_NAME();

	if (param == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_attr_getschedparam(&(*attr)->p, param);

	if (param->sched_priority <= -2)
	{
		param->sched_priority = 767;
	} else if (param->sched_priority >= +2)
	{
		param->sched_priority = 256;
	} else
	{
		param->sched_priority = 700;
	}

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrGetschedpolicy(const PthreadAttr* attr, int* policy)
{
	PRINT_NAME();

	if (policy == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_attr_getschedpolicy(&(*attr)->p, policy);

	switch (*policy)
	{
		case SCHED_OTHER: *policy = (*attr)->policy; break;
		case SCHED_FIFO: *policy = 1; break;
		case SCHED_RR: *policy = 3; break;
		default: EXIT("unknown policy: %d\n", *policy);
	}

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrGetstack(const PthreadAttr* __restrict attr, void** __restrict stack_addr, size_t* __restrict stack_size)
{
	PRINT_NAME();

	if (stack_size == nullptr || stack_addr == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result1 = pthread_attr_getstackaddr(&(*attr)->p, stack_addr);
	int result2 = pthread_attr_getstacksize(&(*attr)->p, stack_size);

	if (result1 == 0 && result2 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* attr, void** stack_addr)
{
	PRINT_NAME();

	if (stack_addr == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result1 = pthread_attr_getstackaddr(&(*attr)->p, stack_addr);

	if (result1 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size)
{
	PRINT_NAME();

	if (stack_size == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result2 = pthread_attr_getstacksize(&(*attr)->p, stack_size);

	if (result2 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrSetaffinity(PthreadAttr* attr, KernelCpumask mask)
{
	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	(*attr)->affinity = mask;

	return OK;
}

int KYTY_SYSV_ABI PthreadAttrSetdetachstate(PthreadAttr* attr, int state)
{
	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int pstate = PTHREAD_CREATE_JOINABLE;
	switch (state)
	{
		case 0: pstate = PTHREAD_CREATE_JOINABLE; break;
		case 1: pstate = PTHREAD_CREATE_DETACHED; break;
		default: EXIT("unknown state: %d\n", state);
	}

	// int result = pthread_attr_setdetachstate(&(*attr)->p, pstate);
	int result = 0;

	(*attr)->detached = (pstate == PTHREAD_CREATE_DETACHED);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrSetguardsize(PthreadAttr* attr, size_t guard_size)
{
	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	(*attr)->guard_size = guard_size;

	return OK;
}

int KYTY_SYSV_ABI PthreadAttrSetinheritsched(PthreadAttr* attr, int inherit_sched)
{
	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int pinherit_sched = PTHREAD_INHERIT_SCHED;
	switch (inherit_sched)
	{
		case 0: pinherit_sched = PTHREAD_EXPLICIT_SCHED; break;
		case 4: pinherit_sched = PTHREAD_INHERIT_SCHED; break;
		default: EXIT("unknown inherit_sched: %d\n", inherit_sched);
	}

	int result = pthread_attr_setinheritsched(&(*attr)->p, pinherit_sched);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param)
{
	PRINT_NAME();

	if (param == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// PS5 guest priorities span a wide numeric range (observed Thread.cpp
	// clamps to roughly 256..767). Host Linux SCHED_OTHER only accepts
	// sched_priority 0 — mapping guest lows/highs to ±2 returns EINVAL and
	// the game asserts (int $0x41, "ret == 0" / scePthreadAttrSetschedparam).
	// Apply a host-valid param and always succeed for a well-formed guest call.
	KernelSchedParam pparam {};
	pparam.sched_priority = 0;
	(void)pthread_attr_setschedparam(&(*attr)->p, &pparam);

	return OK;
}

int KYTY_SYSV_ABI PthreadAttrSetschedpolicy(PthreadAttr* attr, int policy)
{
	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// winpthreads supports only SCHED_OTHER policy
	int ppolicy = SCHED_OTHER;

	(*attr)->policy = policy;

	int result = pthread_attr_setschedpolicy(&(*attr)->p, ppolicy);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrSetstack(PthreadAttr* attr, void* addr, size_t size)
{
	PRINT_NAME();

	if (addr == nullptr || size == 0 || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result1 = pthread_attr_setstackaddr(&(*attr)->p, addr);
	int result2 = pthread_attr_setstacksize(&(*attr)->p, size);

	if (result1 == 0 && result2 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrSetstackaddr(PthreadAttr* attr, void* addr)
{
	PRINT_NAME();

	if (addr == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result1 = pthread_attr_setstackaddr(&(*attr)->p, addr);

	if (result1 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadAttrSetstacksize(PthreadAttr* attr, size_t stack_size)
{
	PRINT_NAME();

	if (stack_size == 0 || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result2 = pthread_attr_setstacksize(&(*attr)->p, stack_size);

	if (result2 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_destroy(&(*rwlock)->p);

	printf("\trwlock destroy: %s, %d\n", (*rwlock)->name.C_Str(), result);

	delete *rwlock;
	*rwlock = nullptr;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* attr, const char* name)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (attr == nullptr)
	{
		EXIT_IF(g_pthread_context == nullptr);

		attr = g_pthread_context->GetDefaultRwlockattr();
	}

	*rwlock = new PthreadRwlockPrivate {};

	(*rwlock)->name = name;

	int result = pthread_rwlock_init(&(*rwlock)->p, &(*attr)->p);

	printf("\trwlock init: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadRwlockRdlock(PthreadRwlock* rwlock)
{
	PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	rwlock = static_cast<PthreadRwlock*>(pthread_static_objects->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	{
		auto* so = g_pthread_context->GetPthreadStaticObjects();
		EXIT_IF(so == nullptr);
		rwlock = static_cast<PthreadRwlock*>(so->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_rdlock(&(*rwlock)->p);

	// printf("\trwlock rdlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadRwlockTimedrdlock(PthreadRwlock* rwlock, KernelUseconds usec)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	{
		auto* so = g_pthread_context->GetPthreadStaticObjects();
		EXIT_IF(so == nullptr);
		rwlock = static_cast<PthreadRwlock*>(so->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	timespec t {};
	usec_to_timespec(&t, usec);

	int result = pthread_rwlock_timedrdlock(&(*rwlock)->p, &t);

	// printf("\trwlock timedrdlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadRwlockTimedwrlock(PthreadRwlock* rwlock, KernelUseconds usec)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	{
		auto* so = g_pthread_context->GetPthreadStaticObjects();
		EXIT_IF(so == nullptr);
		rwlock = static_cast<PthreadRwlock*>(so->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	timespec t {};
	usec_to_timespec(&t, usec);

	int result = pthread_rwlock_timedwrlock(&(*rwlock)->p, &t);

	// printf("\trwlock timedwrlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadRwlockTryrdlock(PthreadRwlock* rwlock)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	{
		auto* so = g_pthread_context->GetPthreadStaticObjects();
		EXIT_IF(so == nullptr);
		rwlock = static_cast<PthreadRwlock*>(so->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_tryrdlock(&(*rwlock)->p);

	// printf("\trwlock tryrdlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadRwlockTrywrlock(PthreadRwlock* rwlock)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	{
		auto* so = g_pthread_context->GetPthreadStaticObjects();
		EXIT_IF(so == nullptr);
		rwlock = static_cast<PthreadRwlock*>(so->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_trywrlock(&(*rwlock)->p);

	// printf("\trwlock trywrlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadRwlockUnlock(PthreadRwlock* rwlock)
{
	// PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	rwlock = static_cast<PthreadRwlock*>(pthread_static_objects->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	{
		auto* so = g_pthread_context->GetPthreadStaticObjects();
		EXIT_IF(so == nullptr);
		rwlock = static_cast<PthreadRwlock*>(so->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_unlock(&(*rwlock)->p);

	// printf("\trwlock unlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;

		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EPERM: return KERNEL_ERROR_EPERM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadRwlockWrlock(PthreadRwlock* rwlock)
{
	// PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	rwlock = static_cast<PthreadRwlock*>(pthread_static_objects->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	{
		auto* so = g_pthread_context->GetPthreadStaticObjects();
		EXIT_IF(so == nullptr);
		rwlock = static_cast<PthreadRwlock*>(so->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_wrlock(&(*rwlock)->p);

	// printf("\trwlock wrlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadRwlockattrDestroy(PthreadRwlockattr* attr)
{
	PRINT_NAME();

	if (attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*attr == nullptr);

	int result = pthread_rwlockattr_destroy(&(*attr)->p);

	delete *attr;
	*attr = nullptr;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadRwlockattrInit(PthreadRwlockattr* attr)
{
	PRINT_NAME();

	*attr = new PthreadRwlockattrPrivate {};

	int result = pthread_rwlockattr_init(&(*attr)->p);

	result = (result == 0 ? PthreadRwlockattrSettype(attr, 1) : result);

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadRwlockattrGetpshared(const PthreadRwlockattr* attr, int* pshared)
{
	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr || pshared == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_rwlockattr_getpshared(&(*attr)->p, pshared);
	return (result == 0 ? OK : KERNEL_ERROR_EINVAL);
}

int KYTY_SYSV_ABI PthreadRwlockattrGettype(PthreadRwlockattr* attr, int* type)
{
	PRINT_NAME();

	if (type == nullptr || attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*type = (*attr)->type;

	return OK;
}

int KYTY_SYSV_ABI PthreadRwlockattrSetpshared(PthreadRwlockattr* attr, int pshared)
{
	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// Only PTHREAD_PROCESS_PRIVATE is supported.
	if (pshared != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_rwlockattr_setpshared(&(*attr)->p, pshared);
	return (result == 0 ? OK : KERNEL_ERROR_EINVAL);
}

int KYTY_SYSV_ABI PthreadRwlockattrSettype(PthreadRwlockattr* attr, int type)
{
	PRINT_NAME();

	if (attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	(*attr)->type = type;

	return OK;
}

int KYTY_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr)
{
	PRINT_NAME();

	if (attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*attr == nullptr);

	int result = pthread_condattr_destroy(&(*attr)->p);

	delete *attr;
	*attr = nullptr;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadCondattrInit(PthreadCondattr* attr)
{
	PRINT_NAME();

	*attr = new PthreadCondattrPrivate {};

	int result = pthread_condattr_init(&(*attr)->p);

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

// Soft-lock diagnostic: identify which guest condition/mutex a blocked wait
// uses, and whether Signal/Broadcast ever reaches it.
// Opt-in via KYTY_SLOT_TRACE=1; only after present>=2200 for event spam, but
// blocked-waiter slots are always recorded under the env so Flip-idle can dump
// a waiter that entered CondWait before the present cliff.
struct SlotTraceBlockedWaiter
{
	std::atomic<uint64_t> cond {0};
	std::atomic<uint64_t> mutex {0};
	std::atomic<uint64_t> ret {0};
	std::atomic<uint64_t> cond_h {0};
	std::atomic<uint64_t> mutex_h {0};
	std::atomic<uint32_t> live {0};
};

static constexpr uint32_t         kSlotTraceWaiterSlots = 8;
static SlotTraceBlockedWaiter     g_slot_trace_waiters[kSlotTraceWaiterSlots];
static std::atomic<uint64_t>      g_slot_trace_tracked_cond {0};
static std::atomic<uint32_t>      g_slot_trace_sig_for_tracked {0};
static std::atomic<uint32_t>      g_slot_trace_wait_for_tracked {0};
static std::atomic<uint32_t>      g_slot_trace_sig_after_stall {0};
static std::atomic<uint64_t>      g_slot_trace_stall_present {0};

struct SlotTraceSigCount
{
	std::atomic<uint64_t> cond {0};
	std::atomic<uint32_t> count {0};
	std::atomic<uint32_t> after_stall {0};
};
static constexpr uint32_t    kSlotTraceSigSlots = 16;
static SlotTraceSigCount     g_slot_trace_sigs[kSlotTraceSigSlots];

static bool slot_trace_env()
{
	return std::getenv("KYTY_SLOT_TRACE") != nullptr;
}

static void slot_trace_note_signal(uint64_t cond_addr)
{
	if (!slot_trace_env() || cond_addr == 0)
	{
		return;
	}
	const bool after_stall = g_slot_trace_stall_present.load() != 0;
	for (uint32_t i = 0; i < kSlotTraceSigSlots; i++)
	{
		uint64_t cur = g_slot_trace_sigs[i].cond.load();
		if (cur == cond_addr)
		{
			g_slot_trace_sigs[i].count.fetch_add(1);
			if (after_stall)
			{
				g_slot_trace_sigs[i].after_stall.fetch_add(1);
			}
			if (cond_addr == g_slot_trace_tracked_cond.load())
			{
				g_slot_trace_sig_for_tracked.fetch_add(1);
				if (after_stall)
				{
					g_slot_trace_sig_after_stall.fetch_add(1);
				}
			}
			return;
		}
		if (cur == 0)
		{
			uint64_t expected = 0;
			if (g_slot_trace_sigs[i].cond.compare_exchange_strong(expected, cond_addr))
			{
				g_slot_trace_sigs[i].count.fetch_add(1);
				if (after_stall)
				{
					g_slot_trace_sigs[i].after_stall.fetch_add(1);
				}
				if (cond_addr == g_slot_trace_tracked_cond.load())
				{
					g_slot_trace_sig_for_tracked.fetch_add(1);
					if (after_stall)
					{
						g_slot_trace_sig_after_stall.fetch_add(1);
					}
				}
				return;
			}
		}
	}
}

static bool slot_trace_cond_active(Graphics::WindowPresentStats* stats_out)
{
	if (!slot_trace_env())
	{
		return false;
	}
	Graphics::WindowPresentStats stats {};
	if (!Graphics::WindowGetPresentStats(&stats) || stats.present < 2200ull)
	{
		return false;
	}
	if (stats_out != nullptr)
	{
		*stats_out = stats;
	}
	if (g_slot_trace_stall_present.load() == 0 && stats.ms_since_present >= 2000ull)
	{
		g_slot_trace_stall_present.store(stats.present);
	}
	return true;
}

static int slot_trace_register_waiter(uint64_t cond, uint64_t mutex, uint64_t ret, uint64_t cond_h, uint64_t mutex_h)
{
	if (!slot_trace_env())
	{
		return -1;
	}
	for (uint32_t i = 0; i < kSlotTraceWaiterSlots; i++)
	{
		uint32_t expected = 0;
		if (g_slot_trace_waiters[i].live.compare_exchange_strong(expected, 1u))
		{
			g_slot_trace_waiters[i].cond.store(cond);
			g_slot_trace_waiters[i].mutex.store(mutex);
			g_slot_trace_waiters[i].ret.store(ret);
			g_slot_trace_waiters[i].cond_h.store(cond_h);
			g_slot_trace_waiters[i].mutex_h.store(mutex_h);
			// Prefer the soft-lock worker return range around 0x90173da75.
			const bool prefer = (ret >= 0x90173d000ull && ret < 0x90173e000ull);
			if (prefer || g_slot_trace_tracked_cond.load() == 0)
			{
				if (cond != 0)
				{
					g_slot_trace_tracked_cond.store(cond);
				}
			}
			if (cond != 0 && cond == g_slot_trace_tracked_cond.load())
			{
				g_slot_trace_wait_for_tracked.fetch_add(1);
			}
			return static_cast<int>(i);
		}
	}
	return -1;
}

static void slot_trace_unregister_waiter(int slot)
{
	if (slot < 0 || static_cast<uint32_t>(slot) >= kSlotTraceWaiterSlots)
	{
		return;
	}
	g_slot_trace_waiters[static_cast<uint32_t>(slot)].live.store(0);
}

static uint32_t slot_trace_signal_count(uint64_t cond)
{
	for (uint32_t i = 0; i < kSlotTraceSigSlots; i++)
	{
		if (g_slot_trace_sigs[i].cond.load() == cond)
		{
			return g_slot_trace_sigs[i].count.load();
		}
	}
	return 0;
}

bool PthreadGetCondWaitDiagnostics(PthreadCondWaitDiagnostics* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};
	if (!slot_trace_env())
	{
		return false;
	}

	out->enabled         = true;
	out->tracked_cond    = g_slot_trace_tracked_cond.load();
	out->tracked_waits   = g_slot_trace_wait_for_tracked.load();
	out->tracked_signals = g_slot_trace_sig_for_tracked.load();
	for (uint32_t i = 0; i < kSlotTraceWaiterSlots; i++)
	{
		if (g_slot_trace_waiters[i].live.load() == 0)
		{
			continue;
		}
		if (out->blocked_count >= std::size(out->blocked))
		{
			break;
		}
		auto& waiter         = out->blocked[out->blocked_count++];
		waiter.cond          = g_slot_trace_waiters[i].cond.load();
		waiter.mutex         = g_slot_trace_waiters[i].mutex.load();
		waiter.return_addr   = g_slot_trace_waiters[i].ret.load();
		waiter.cond_handle   = g_slot_trace_waiters[i].cond_h.load();
		waiter.mutex_handle  = g_slot_trace_waiters[i].mutex_h.load();
		waiter.signal_count  = slot_trace_signal_count(waiter.cond);
	}
	return true;
}

bool PthreadGetThreadDiagnostics(PthreadThreadDiagnostics* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};

	if (g_pthread_context == nullptr)
	{
		return false;
	}
	auto* pthread_pool = g_pthread_context->GetPthreadPool();
	if (pthread_pool == nullptr)
	{
		return false;
	}

	out->available = true;
	pthread_pool->GetDiagnostics(out);
	return true;
}

void SlotTraceDumpBlockedCondWaiters()
{
	if (!slot_trace_env())
	{
		return;
	}
	std::fprintf(stderr, "COND_BLOCKED tracked=0x%016" PRIx64 " wait_reg=%u sig_total_trk=%u sig_after_stall=%u\n",
	             g_slot_trace_tracked_cond.load(), g_slot_trace_wait_for_tracked.load(), g_slot_trace_sig_for_tracked.load(),
	             g_slot_trace_sig_after_stall.load());
	for (uint32_t i = 0; i < kSlotTraceWaiterSlots; i++)
	{
		if (g_slot_trace_waiters[i].live.load() == 0)
		{
			continue;
		}
		std::fprintf(stderr,
		             "COND_BLOCKED[%u] cond=0x%016" PRIx64 " cond_h=0x%016" PRIx64 " mutex=0x%016" PRIx64 " mutex_h=0x%016" PRIx64
		             " ret=0x%016" PRIx64 "\n",
		             i, g_slot_trace_waiters[i].cond.load(), g_slot_trace_waiters[i].cond_h.load(),
		             g_slot_trace_waiters[i].mutex.load(), g_slot_trace_waiters[i].mutex_h.load(),
		             g_slot_trace_waiters[i].ret.load());
	}
	for (uint32_t i = 0; i < kSlotTraceSigSlots; i++)
	{
		const uint64_t c = g_slot_trace_sigs[i].cond.load();
		if (c == 0)
		{
			continue;
		}
		std::fprintf(stderr, "COND_SIGCNT cond=0x%016" PRIx64 " total=%u after_stall=%u\n", c, g_slot_trace_sigs[i].count.load(),
		             g_slot_trace_sigs[i].after_stall.load());
	}
	std::fflush(stderr);
}

static void slot_trace_cond_event(const char* kind, PthreadCond* guest_cond, PthreadMutex* guest_mutex, uint64_t ret)
{
	Graphics::WindowPresentStats stats {};
	if (!slot_trace_cond_active(&stats))
	{
		return;
	}

	const uint64_t cond_addr  = reinterpret_cast<uint64_t>(guest_cond);
	const uint64_t mutex_addr = reinterpret_cast<uint64_t>(guest_mutex);
	uint64_t       cond_h     = 0;
	uint64_t       mutex_h    = 0;
	if (guest_cond != nullptr)
	{
		cond_h = *reinterpret_cast<const volatile uint64_t*>(guest_cond);
	}
	if (guest_mutex != nullptr)
	{
		mutex_h = *reinterpret_cast<const volatile uint64_t*>(guest_mutex);
	}

	static std::atomic<uint32_t> wait_n {0};
	static std::atomic<uint32_t> sig_n {0};
	static std::atomic<uint32_t> bcast_n {0};

	const uint64_t tracked = g_slot_trace_tracked_cond.load();
	uint32_t       seq     = 0;
	if (kind[0] == 'W')
	{
		seq = wait_n.fetch_add(1);
		if (tracked == 0 && cond_addr != 0)
		{
			g_slot_trace_tracked_cond.store(cond_addr);
		}
		if (seq >= 16u && (seq % 8u) != 0u && cond_addr != tracked)
		{
			return;
		}
	} else if (kind[0] == 'S')
	{
		seq = sig_n.fetch_add(1);
		if (seq >= 32u && (seq % 16u) != 0u && cond_addr != tracked)
		{
			return;
		}
	} else
	{
		seq = bcast_n.fetch_add(1);
		if (seq >= 16u && (seq % 8u) != 0u && cond_addr != tracked)
		{
			return;
		}
	}

	std::fprintf(stderr,
	             "COND_TRACE %s n=%u present=%llu ms_p=%llu ret=0x%016" PRIx64 " cond=0x%016" PRIx64 " cond_h=0x%016" PRIx64
	             " mutex=0x%016" PRIx64 " mutex_h=0x%016" PRIx64 " tracked=0x%016" PRIx64 " sig_trk=%u sig_stall=%u\n",
	             kind, seq, static_cast<unsigned long long>(stats.present), static_cast<unsigned long long>(stats.ms_since_present), ret,
	             cond_addr, cond_h, mutex_addr, mutex_h, g_slot_trace_tracked_cond.load(), g_slot_trace_sig_for_tracked.load(),
	             g_slot_trace_sig_after_stall.load());
	std::fflush(stderr);
}

int KYTY_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond)
{
	PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	const auto ret_addr = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	slot_trace_note_signal(reinterpret_cast<uint64_t>(cond));
	slot_trace_cond_event("BCAST", cond, nullptr, ret_addr);

	cond = static_cast<PthreadCond*>(pthread_static_objects->CreateObject(cond, PthreadStaticObject::Type::Cond));

	if (cond == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);

	int result = pthread_cond_broadcast(&(*cond)->p);

	printf("\tcond broadcast: %s, %d\n", (*cond)->name.C_Str(), result);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadCondDestroy(PthreadCond* cond)
{
	PRINT_NAME();

	if (cond == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);

	int result = pthread_cond_destroy(&(*cond)->p);

	printf("\tcond destroy: %s, %d\n", (*cond)->name.C_Str(), result);

	delete *cond;
	*cond = nullptr;

	switch (result)
	{
		case 0: return OK;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char* name)
{
	PRINT_NAME();

	if (cond == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (attr == nullptr)
	{
		EXIT_IF(g_pthread_context == nullptr);

		attr = g_pthread_context->GetDefaultCondattr();
	}

	*cond = new PthreadCondPrivate {};

	(*cond)->name = name;

	int result = pthread_cond_init(&(*cond)->p, &(*attr)->p);

	printf("\tcond init: %s, %d\n", (*cond)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadCondSignal(PthreadCond* cond)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(cond == nullptr);

	if (cond == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	const auto ret_addr = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	slot_trace_note_signal(reinterpret_cast<uint64_t>(cond));
	slot_trace_cond_event("SIGNAL", cond, nullptr, ret_addr);

	// Lazily initialize a statically-initialized cond (sentinel), like the other paths.
	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	EXIT_IF(pthread_static_objects == nullptr);
	cond = static_cast<PthreadCond*>(pthread_static_objects->CreateObject(cond, PthreadStaticObject::Type::Cond));

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);

	int result = pthread_cond_signal(&(*cond)->p);

	// printf("\tcond signal: %s, %d\n", (*cond)->name.C_Str(), result);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadCondSignalto(PthreadCond* cond, Pthread thread)
{
	PRINT_NAME();

	if (cond == nullptr || thread == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);

	int result = 0;

	KYTY_NOT_IMPLEMENTED;

	// printf("\tcond signalto: %s, %d\n", (*cond)->name.C_Str(), result);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

static int pthread_cond_release_mutex_state(PthreadMutexPrivate* mutex)
{
	std::lock_guard lock(mutex->state_mutex);

	if (mutex->recursion_count == 0 || pthread_equal(mutex->owner, pthread_self()) == 0)
	{
		return EPERM;
	}
	if (mutex->recursion_count != 1)
	{
		return EINVAL;
	}

	mutex->owner           = {};
	mutex->recursion_count = 0;
	return 0;
}

static void pthread_cond_restore_mutex_state(PthreadMutexPrivate* mutex)
{
	std::lock_guard lock(mutex->state_mutex);
	mutex->owner           = pthread_self();
	mutex->recursion_count = 1;
}

int KYTY_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec)
{
	PRINT_NAME();

	if (cond == nullptr || mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// Lazily initialize statically-initialized cond/mutex (sentinel values), same as
	// PthreadCondWait; otherwise the sentinel would be dereferenced as a handle.
	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	EXIT_IF(pthread_static_objects == nullptr);
	cond  = static_cast<PthreadCond*>(pthread_static_objects->CreateObject(cond, PthreadStaticObject::Type::Cond));
	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);
	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	timespec t {};
	usec_to_timespec(&t, usec);

	auto* private_mutex = *mutex;
	int   result        = pthread_cond_release_mutex_state(private_mutex);
	if (result == 0)
	{
		result = pthread_cond_timedwait(&(*cond)->p, &private_mutex->p, &t);
		pthread_cond_restore_mutex_state(private_mutex);
	}

	// printf("\tcond timedwait: %s, %d\n", (*cond)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EPERM: return KERNEL_ERROR_EPERM;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

namespace {

constexpr int kPthreadOnceUninitialized = 0;
constexpr int kPthreadOnceInProgress      = 1;
constexpr int kPthreadOnceDone            = 2;

Core::Mutex g_pthread_once_mutex;

} // namespace

int KYTY_SYSV_ABI PthreadOnce(int* once_control, void (*init_routine)(void))
{
	PRINT_NAME();

	if (once_control == nullptr || init_routine == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (*once_control == kPthreadOnceDone)
	{
		return OK;
	}

	bool should_call = false;
	g_pthread_once_mutex.Lock();
	while (*once_control == kPthreadOnceInProgress)
	{
		g_pthread_once_mutex.Unlock();
		KernelUsleep(1000);
		g_pthread_once_mutex.Lock();
	}

	if (*once_control == kPthreadOnceDone)
	{
		g_pthread_once_mutex.Unlock();
		return OK;
	}

	*once_control = kPthreadOnceInProgress;
	should_call   = true;
	g_pthread_once_mutex.Unlock();

	if (should_call)
	{
		Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(init_routine), 0, 0, 0);

		g_pthread_once_mutex.Lock();
		*once_control = kPthreadOnceDone;
		g_pthread_once_mutex.Unlock();
	}

	return OK;
}

int KYTY_SYSV_ABI PthreadCondWait(PthreadCond* cond, PthreadMutex* mutex)
{
	PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	const auto   ret_addr = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	const uint64_t cond_addr  = reinterpret_cast<uint64_t>(cond);
	const uint64_t mutex_addr = reinterpret_cast<uint64_t>(mutex);

	cond  = static_cast<PthreadCond*>(pthread_static_objects->CreateObject(cond, PthreadStaticObject::Type::Cond));
	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	if (cond == nullptr || mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);
	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	// Peek handles only after CreateObject resolved static sentinels, and only
	// under KYTY_SLOT_TRACE — early guest BSS reads crashed boot diagnostics.
	uint64_t cond_h  = 0;
	uint64_t mutex_h = 0;
	int      waiter_slot = -1;
	if (slot_trace_env())
	{
		cond_h  = *reinterpret_cast<const volatile uint64_t*>(cond);
		mutex_h = *reinterpret_cast<const volatile uint64_t*>(mutex);
		slot_trace_cond_event("WAIT", cond, mutex, ret_addr);
		waiter_slot = slot_trace_register_waiter(cond_addr, mutex_addr, ret_addr, cond_h, mutex_h);
	}

	auto* private_mutex = *mutex;
	int   result        = pthread_cond_release_mutex_state(private_mutex);
	if (result == 0)
	{
		result = pthread_cond_wait(&(*cond)->p, &private_mutex->p);
		pthread_cond_restore_mutex_state(private_mutex);
	}

	slot_trace_unregister_waiter(waiter_slot);

	// printf("\tcond wait: %s, %d\n", (*cond)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EPERM: return KERNEL_ERROR_EPERM;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

Pthread KYTY_SYSV_ABI PthreadSelf()
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_pthread_self == nullptr);

	return g_pthread_self;
}

static void cleanup_thread(void* arg)
{
	auto* thread = static_cast<Pthread>(arg);

	EXIT_IF(g_pthread_context == nullptr);

	auto thread_dtors = g_pthread_context->GetThreadDtors();

	if (thread_dtors != nullptr)
	{
		thread_dtors();
	}

	Core::mem_guest_thread_leave();

	thread->almost_done = true;
}

static void* run_thread(void* arg)
{
	auto* thread = static_cast<Pthread>(arg);
	void* ret    = nullptr;

	thread->unique_id = Core::Thread::GetThreadIdUnique();

	g_pthread_self = thread;
	Core::mem_guest_thread_enter();

	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
	pthread_cleanup_push(cleanup_thread, thread);

	thread->started = true;

	ret = reinterpret_cast<void*>(Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(thread->entry),
	                                                       reinterpret_cast<uint64_t>(thread->arg), 0, 0));

	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
	pthread_cleanup_pop(1);

	return ret;
}

int KYTY_SYSV_ABI PthreadCreate(Pthread* thread, const PthreadAttr* attr, pthread_entry_func_t entry, void* arg, const char* name)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_pool = g_pthread_context->GetPthreadPool();

	EXIT_IF(pthread_pool == nullptr);

	if (attr == nullptr)
	{
		attr = g_pthread_context->GetDefaultAttr();
	}

	PRINT_NAME_ENABLE(false);

	*thread = pthread_pool->Create();

	if ((*thread)->attr != nullptr)
	{
		PthreadAttrDestroy(&(*thread)->attr);
	}

	PthreadAttrInit(&(*thread)->attr);

	int result = pthread_attr_copy(&(*thread)->attr, attr);

	if (result == 0)
	{
		EXIT_IF((*thread)->free);

		(*thread)->name        = name;
		(*thread)->entry       = entry;
		(*thread)->arg         = arg;
		(*thread)->almost_done = false;
		(*thread)->detached    = (*attr)->detached;
		(*thread)->started     = false;
		(*thread)->unique_id   = -1;
		(*thread)->guest_priority.store(700, std::memory_order_relaxed);
		(*thread)->guest_stack_base = 0;
		(*thread)->guest_stack_size = 0;

		// Host pthread_create may memset/setup a guest-provided stack. Guests
		// often demote a guard page with mprotect(prot=0) first; leaving that
		// page PROT_NONE makes libc pthread_create SIGSEGV in host memset.
		// Ensure the configured stack is host-writable for the create path.
		// Prefer pthread_attr_getstack — getstackaddr alone can miss stacks set
		// via setstack and is deprecated.
		void*  stack_addr = nullptr;
		size_t stack_size = 0;
		if (pthread_attr_getstack(&(*attr)->p, &stack_addr, &stack_size) == 0 && stack_addr != nullptr && stack_size != 0 &&
		    reinterpret_cast<uintptr_t>(stack_addr) <= UINTPTR_MAX - stack_size)
		{
			(*thread)->guest_stack_base = reinterpret_cast<uint64_t>(stack_addr);
			(*thread)->guest_stack_size = stack_size;
			Core::VirtualMemory::Mode old_mode {};
			(void)Core::VirtualMemory::Protect(reinterpret_cast<uint64_t>(stack_addr), stack_size,
			                                   Core::VirtualMemory::Mode::ReadWrite, &old_mode);
			std::fprintf(stderr, "PthreadCreate reprotect stack=0x%016" PRIx64 " size=0x%zx\n",
			             reinterpret_cast<uint64_t>(stack_addr), stack_size);
		}

		result = pthread_create(&(*thread)->p, &(*attr)->p, run_thread, *thread);
	}

	// Do not wait for the child to enter its guest entry. Real pthread_create
	// returns as soon as the thread is constructed; the parent may still run
	// setup (e.g. sceKernelCreateEventFlag "ThreadFlag") before the child is
	// scheduled. Waiting for `started` inverted that order on Linux: the
	// VibrationTrackThread reached KernelWaitEventFlag with an uninitialized
	// handle (observed poison 0xcccccccc00007fff) and SIGSEGV'd in Mutex::Lock.
	// unique_id may still be -1 in the create log until the child runs.

	printf("\tthread create: %s, id = %d, %d\n", (*thread)->name.C_Str(), (*thread)->unique_id, result);

	pthread_attr_dbg_print(attr);

	PRINT_NAME_ENABLE(true);

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EPERM: return KERNEL_ERROR_EPERM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadDetach(Pthread thread)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	printf("\tthread detach: %s, %d\n", thread->name.C_Str(), 0);

	thread->detached = true;

	return OK;
}

int KYTY_SYSV_ABI PthreadJoin(Pthread thread, void** value)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_join(thread->p, value);

	if (PRINT_NAME_ENABLED)
	{
		printf("\tthread join: %s, %d\n", thread->name.C_Str(), result);
	}

	int id = thread->unique_id;

	thread->almost_done = false;
	thread->free        = true;

	// Key destructors may still touch guest TLS / allocator state. Run them
	// before releasing the TLS image.
	g_pthread_context->GetPthreadKeys()->Destruct(id);

	// Do not DeleteTlss(id) here. Gen5 titles (and FNA/Mono-style runtimes) can
	// keep freelist control blocks at TP-0x158 alive across threads; freeing the
	// image at join turns those slots into use-after-free (observed begin/end
	// patterns 0x1fffffffe / TCB self-pointer). TLS blocks are released with the
	// process / RuntimeLinker teardown instead.

	switch (result)
	{
		case 0: return OK;
		case ESRCH: return KERNEL_ERROR_ESRCH;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EOPNOTSUPP: return KERNEL_ERROR_EOPNOTSUPP;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadCancel(Pthread thread)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_cancel(thread->p);

	printf("\tthread cancel: %s, %d\n", thread->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case ESRCH: return KERNEL_ERROR_ESRCH;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadSetaffinity(Pthread thread, KernelCpumask mask)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	auto result = PthreadAttrSetaffinity(&thread->attr, mask);

	return result;
}

int KYTY_SYSV_ABI PthreadSetcancelstate(int state, int* old_state)
{
	PRINT_NAME();

	int pstate = PTHREAD_CANCEL_DISABLE;

	switch (state)
	{
		case 0: pstate = PTHREAD_CANCEL_ENABLE; break;
		case 1: pstate = PTHREAD_CANCEL_DISABLE; break;
		default: EXIT("unknown state: %d", state);
	}

	int result = pthread_setcancelstate(pstate, old_state);

	printf("\tthread setcancelstate: %d\n", result);

	switch (*old_state)
	{
		case PTHREAD_CANCEL_ENABLE: *old_state = 0; break;
		case PTHREAD_CANCEL_DISABLE: *old_state = 1; break;
		default: EXIT("unknown old_state: %d", *old_state);
	}

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadSetcanceltype(int type, int* old_type)
{
	PRINT_NAME();

	int ptype = PTHREAD_CANCEL_DEFERRED;

	switch (type)
	{
		case 0: ptype = PTHREAD_CANCEL_DEFERRED; break;
		case 2: ptype = PTHREAD_CANCEL_ASYNCHRONOUS; break;
		default: EXIT("unknown type: %d", type);
	}

	int result = pthread_setcanceltype(ptype, old_type);

	printf("\tthread setcanceltype: %d\n", result);

	switch (*old_type)
	{
		case PTHREAD_CANCEL_DEFERRED: *old_type = 0; break;
		case PTHREAD_CANCEL_ASYNCHRONOUS: *old_type = 2; break;
		default: EXIT("unknown type: %d", *old_type);
	}

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadGetprio(Pthread thread, int* prio)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	EXIT_NOT_IMPLEMENTED(prio == nullptr);

	*prio = thread->guest_priority.load(std::memory_order_relaxed);

	printf("\t PthreadGetprio: %d, %d\n", thread->unique_id, *prio);

	return OK;
}

int KYTY_SYSV_ABI PthreadSetprio(Pthread thread, int prio)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	thread->guest_priority.store(prio, std::memory_order_relaxed);

	printf("\t PthreadSetprio: %d, %d\n", thread->unique_id, prio);

	return OK;
}

void KYTY_SYSV_ABI PthreadTestcancel()
{
	PRINT_NAME();

	pthread_testcancel();
}

void KYTY_SYSV_ABI PthreadExit(void* value)
{
	PRINT_NAME();

	pthread_exit(value);
}

int KYTY_SYSV_ABI PthreadEqual(Pthread thread1, Pthread thread2)
{
	// PRINT_NAME();

	return (thread1 == thread2 ? 1 : 0);
}

int KYTY_SYSV_ABI PthreadGetname(Pthread thread, char* name)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	if (name == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	strncpy(name, thread->name.C_Str(), 32);
	name[31] = '\0';

	return OK;
}

int KYTY_SYSV_ABI PthreadRename(Pthread thread, const char* name)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	if (name != nullptr)
	{
		thread->name = String::FromUtf8(name);
		printf("\t PthreadRename: %s\n", name);
	}

	return OK;
}

int KYTY_SYSV_ABI PthreadGetaffinity(Pthread thread, KernelCpumask* mask)
{
	PRINT_NAME();

	if (thread == nullptr || mask == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrGetaffinity(&thread->attr, mask);
}

int KYTY_SYSV_ABI PthreadGetschedparam(Pthread thread, int* policy, KernelSchedParam* param)
{
	PRINT_NAME();

	if (thread == nullptr || policy == nullptr || param == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	sched_param host_param {};
	int         host_policy = 0;
	const int   result      = pthread_getschedparam(thread->p, &host_policy, &host_param);
	if (result != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*policy        = host_policy;
	param->sched_priority = host_param.sched_priority;
	return OK;
}

int KYTY_SYSV_ABI PthreadSetschedparam(Pthread thread, int policy, const KernelSchedParam* param)
{
	PRINT_NAME();

	if (thread == nullptr || param == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	sched_param host_param {};
	host_param.sched_priority = param->sched_priority;
	const int result = pthread_setschedparam(thread->p, policy, &host_param);
	return (result == 0 ? OK : KERNEL_ERROR_EINVAL);
}

void KYTY_SYSV_ABI PthreadYield()
{
	PRINT_NAME();

	sched_yield();
}

int KYTY_SYSV_ABI PthreadGetthreadid()
{
	PRINT_NAME();

	return Core::Thread::GetThreadIdUnique();
}

void KYTY_SYSV_ABI KernelSetThreadDtors(thread_dtors_func_t dtors)
{
	PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	EXIT_NOT_IMPLEMENTED(g_pthread_context->GetThreadDtors() != nullptr);

	g_pthread_context->SetThreadDtors(dtors);
	// g_thread_dtors = dtors;
}

int KYTY_SYSV_ABI KernelUsleep(KernelUseconds microseconds)
{
	Core::Thread::SleepMicro(microseconds);
	return OK;
}

unsigned int KYTY_SYSV_ABI KernelSleep(unsigned int seconds)
{
	PRINT_NAME();
	printf("\tsleep: %u\n", seconds);
	Core::Timer t;
	t.Start();
	Core::Thread::Sleep(seconds);
	double ts = t.GetTimeS();
	printf("\tactual: %g seconds\n", ts);
	return OK;
}

int KYTY_SYSV_ABI KernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp)
{
	PRINT_NAME();

	if (rqtp == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t nanos = rqtp->tv_sec * 1000000000 + rqtp->tv_nsec;

	// Soft-lock diagnostic: a job pump may sleep-poll via Nanosleep while
	// waiting on guest slot table 0x901c434c8 (0x20-byte entries). Guest VAs are
	// host-mapped only after load — never read the table before present>=2200.
	// Opt-in via KYTY_SLOT_TRACE=1. Do not invent EventFlag wakes.
	if (std::getenv("KYTY_SLOT_TRACE") != nullptr && nanos >= 1000000ull)
	{
		Graphics::WindowPresentStats stats {};
		if (Graphics::WindowGetPresentStats(&stats) && stats.present >= 2200ull)
		{
			static std::atomic<uint32_t> ns_logs {0};
			const uint32_t               n = ns_logs.fetch_add(1);
			if (n < 64u || (n % 4u) == 0u)
			{
				const auto         ret        = reinterpret_cast<uint64_t>(__builtin_return_address(0));
				constexpr uint64_t kSlotTable  = 0x901c434c8ull;
				constexpr uint64_t kSlotStride = 0x20ull;
				std::fprintf(stderr, "SLOT_TRACE ns=%" PRIu64 " ret=0x%016" PRIx64 " n=%u present=%llu\n", nanos, ret, n,
				             static_cast<unsigned long long>(stats.present));
				for (uint32_t i = 8; i <= 11; i++)
				{
					const auto*    entry = reinterpret_cast<const volatile uint64_t*>(kSlotTable + i * kSlotStride);
					const uint64_t typ   = entry[0];
					const uint64_t obj   = entry[1];
					uint32_t       s0    = 0;
					uint32_t       s1    = 0;
					uint64_t       fn    = 0;
					if (obj >= 0x900000000ull && obj < 0x940000000ull)
					{
						const auto* o = reinterpret_cast<const volatile uint32_t*>(obj);
						s0            = o[0];
						s1            = o[1];
						fn            = *reinterpret_cast<const volatile uint64_t*>(obj + 0x10);
					}
					std::fprintf(stderr,
					             "SLOT[%u] typ=0x%" PRIx64 " obj=0x%016" PRIx64 " state=%u/%u fn=0x%016" PRIx64 "\n", i, typ, obj, s0,
					             s1, fn);
				}
				std::fflush(stderr);
			}
		}
	}

	printf("\tnanosleep: %" PRIu64 "\n", nanos);

	Core::Timer t;
	t.Start();
	Core::Thread::SleepNano(nanos);
	double ts = t.GetTimeS();
	printf("\tactual: %g nanoseconds\n", ts * 1000000000.0);

	if (rmtp != nullptr)
	{
		sec_to_timespec(rmtp, ts);
	}

	return OK;
}

int KYTY_SYSV_ABI PthreadKeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor)
{
	PRINT_NAME();

	if (key == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_IF(g_pthread_context == nullptr || g_pthread_context->GetPthreadKeys() == nullptr);

	if (!g_pthread_context->GetPthreadKeys()->Create(key, destructor))
	{
		return KERNEL_ERROR_EAGAIN;
	}

	printf("\t destructor = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(destructor));
	printf("\t key        = %d\n", *key);

	return OK;
}

int KYTY_SYSV_ABI PthreadKeyDelete(PthreadKey key)
{
	PRINT_NAME();

	printf("\t key = %d\n", key);

	EXIT_IF(g_pthread_context == nullptr || g_pthread_context->GetPthreadKeys() == nullptr);

	if (!g_pthread_context->GetPthreadKeys()->Delete(key))
	{
		return KERNEL_ERROR_EINVAL;
	}

	return OK;
}

int KYTY_SYSV_ABI PthreadSetspecific(PthreadKey key, void* value)
{
	PRINT_NAME();

	int thread_id = Core::Thread::GetThreadIdUnique();

	printf("\t key       = %d\n", key);
	printf("\t thread_id = %d\n", thread_id);
	printf("\t value     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(value));

	EXIT_IF(g_pthread_context == nullptr || g_pthread_context->GetPthreadKeys() == nullptr);

	if (!g_pthread_context->GetPthreadKeys()->Set(key, thread_id, value))
	{
		return KERNEL_ERROR_EINVAL;
	}

	return OK;
}

void* KYTY_SYSV_ABI PthreadGetspecific(PthreadKey key)
{
	PRINT_NAME();

	int thread_id = Core::Thread::GetThreadIdUnique();

	printf("\t key       = %d\n", key);
	printf("\t thread_id = %d\n", thread_id);

	EXIT_IF(g_pthread_context == nullptr || g_pthread_context->GetPthreadKeys() == nullptr);

	void* value = nullptr;

	if (!g_pthread_context->GetPthreadKeys()->Get(key, thread_id, &value))
	{
		return nullptr;
	}

	printf("\t value     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(value));

	return value;
}

} // namespace LibKernel

namespace Posix {

LIB_NAME("Posix", "libkernel");

int KYTY_SYSV_ABI pthread_create(LibKernel::Pthread* thread, const LibKernel::PthreadAttr* attr, LibKernel::pthread_entry_func_t entry,
                                 void* arg)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCreate(thread, attr, entry, arg, ""));
}

int KYTY_SYSV_ABI pthread_create_name_np(LibKernel::Pthread* thread, const LibKernel::PthreadAttr* attr,
                                         LibKernel::pthread_entry_func_t entry, void* arg, const char* name)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCreate(thread, attr, entry, arg, name != nullptr ? name : ""));
}

int KYTY_SYSV_ABI pthread_equal(LibKernel::Pthread thread1, LibKernel::Pthread thread2)
{
	return LibKernel::PthreadEqual(thread1, thread2);
}

int KYTY_SYSV_ABI pthread_setcancelstate(int state, int* old_state)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadSetcancelstate(state, old_state));
}

int KYTY_SYSV_ABI pthread_setprio(LibKernel::Pthread thread, int prio)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadSetprio(thread, prio));
}

int KYTY_SYSV_ABI pthread_join(LibKernel::Pthread thread, void** value)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadJoin(thread, value));
}

int KYTY_SYSV_ABI pthread_detach(LibKernel::Pthread thread)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadDetach(thread));
}

void KYTY_SYSV_ABI pthread_exit(void* value)
{
	PRINT_NAME();

	LibKernel::PthreadExit(value);
}

void KYTY_SYSV_ABI pthread_yield()
{
	PRINT_NAME();

	LibKernel::PthreadYield();
}

int KYTY_SYSV_ABI sched_yield()
{
	PRINT_NAME();
	LibKernel::PthreadYield();
	return 0;
}

int KYTY_SYSV_ABI pthread_cond_init(LibKernel::PthreadCond* cond, const LibKernel::PthreadCondattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondInit(cond, attr, nullptr));
}

int KYTY_SYSV_ABI pthread_cond_destroy(LibKernel::PthreadCond* cond)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondDestroy(cond));
}

int KYTY_SYSV_ABI pthread_cond_signal(LibKernel::PthreadCond* cond)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondSignal(cond));
}

int KYTY_SYSV_ABI pthread_cond_broadcast(LibKernel::PthreadCond* cond)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondBroadcast(cond));
}

static LibKernel::KernelUseconds abstime_remaining_usec(const LibKernel::KernelTimespec* abstime)
{
	LibKernel::KernelTimespec now {};
	if (LibKernel::KernelClockGettime(0, &now) != OK || abstime == nullptr)
	{
		return 0;
	}

	const int64_t now_us = now.tv_sec * 1000000 + now.tv_nsec / 1000;
	const int64_t abs_us = abstime->tv_sec * 1000000 + abstime->tv_nsec / 1000;
	if (abs_us <= now_us)
	{
		return 0;
	}

	const int64_t delta = abs_us - now_us;
	if (delta > static_cast<int64_t>(UINT32_MAX))
	{
		return UINT32_MAX;
	}
	return static_cast<LibKernel::KernelUseconds>(delta);
}

int KYTY_SYSV_ABI pthread_cond_wait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondWait(cond, mutex));
}

int KYTY_SYSV_ABI pthread_cond_timedwait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex,
                                         const LibKernel::KernelTimespec* abstime)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondTimedwait(cond, mutex, abstime_remaining_usec(abstime)));
}

int KYTY_SYSV_ABI pthread_once(int* once_control, void (*init_routine)(void))
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadOnce(once_control, init_routine));
}

int KYTY_SYSV_ABI pthread_mutex_lock(LibKernel::PthreadMutex* mutex)
{
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexLock(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_trylock(LibKernel::PthreadMutex* mutex)
{
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexTrylock(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_unlock(LibKernel::PthreadMutex* mutex)
{
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexUnlock(mutex));
}

int KYTY_SYSV_ABI pthread_rwlock_destroy(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockDestroy(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_init(LibKernel::PthreadRwlock* rwlock, const LibKernel::PthreadRwlockattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockInit(rwlock, attr, ""));
}

int KYTY_SYSV_ABI pthread_rwlock_rdlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockRdlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_timedrdlock(LibKernel::PthreadRwlock* rwlock, const LibKernel::KernelTimespec* abstime)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockTimedrdlock(rwlock, abstime_remaining_usec(abstime)));
}

int KYTY_SYSV_ABI pthread_rwlock_timedwrlock(LibKernel::PthreadRwlock* rwlock, const LibKernel::KernelTimespec* abstime)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockTimedwrlock(rwlock, abstime_remaining_usec(abstime)));
}

int KYTY_SYSV_ABI pthread_rwlock_tryrdlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockTryrdlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_trywrlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockTrywrlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_unlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockUnlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_wrlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockWrlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlockattr_destroy(LibKernel::PthreadRwlockattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockattrDestroy(attr));
}

int KYTY_SYSV_ABI pthread_rwlockattr_getpshared(const LibKernel::PthreadRwlockattr* attr, int* pshared)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockattrGetpshared(attr, pshared));
}

int KYTY_SYSV_ABI pthread_rwlockattr_gettype_np(LibKernel::PthreadRwlockattr* attr, int* type)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockattrGettype(attr, type));
}

int KYTY_SYSV_ABI pthread_rwlockattr_init(LibKernel::PthreadRwlockattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockattrInit(attr));
}

int KYTY_SYSV_ABI pthread_rwlockattr_setpshared(LibKernel::PthreadRwlockattr* attr, int pshared)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockattrSetpshared(attr, pshared));
}

int KYTY_SYSV_ABI pthread_rwlockattr_settype_np(LibKernel::PthreadRwlockattr* attr, int type)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockattrSettype(attr, type));
}

int KYTY_SYSV_ABI pthread_key_create(LibKernel::PthreadKey* key, LibKernel::pthread_key_destructor_func_t destructor)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadKeyCreate(key, destructor));
}

int KYTY_SYSV_ABI pthread_key_delete(LibKernel::PthreadKey key)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadKeyDelete(key));
}

int KYTY_SYSV_ABI pthread_setspecific(LibKernel::PthreadKey key, void* value)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadSetspecific(key, value));
}

void* KYTY_SYSV_ABI pthread_getspecific(LibKernel::PthreadKey key)
{
	PRINT_NAME();

	return (LibKernel::PthreadGetspecific(key));
}

int KYTY_SYSV_ABI pthread_mutex_destroy(LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexDestroy(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_init(LibKernel::PthreadMutex* mutex, const LibKernel::PthreadMutexattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexInit(mutex, attr, nullptr));
}

int KYTY_SYSV_ABI pthread_mutexattr_init(LibKernel::PthreadMutexattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrInit(attr));
}

int KYTY_SYSV_ABI pthread_mutexattr_settype(LibKernel::PthreadMutexattr* attr, int type)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrSettype(attr, type));
}

int KYTY_SYSV_ABI pthread_mutexattr_destroy(LibKernel::PthreadMutexattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrDestroy(attr));
}

// Gen5 Posix_v1 pthread_attr_* NIDs (Astro after package path bring-up).
int KYTY_SYSV_ABI pthread_attr_init(LibKernel::PthreadAttr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrInit(attr));
}

int KYTY_SYSV_ABI pthread_attr_destroy(LibKernel::PthreadAttr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrDestroy(attr));
}

int KYTY_SYSV_ABI pthread_attr_getstack(const LibKernel::PthreadAttr* attr, void** stack_addr, size_t* stack_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetstack(attr, stack_addr, stack_size));
}

int KYTY_SYSV_ABI pthread_attr_setstacksize(LibKernel::PthreadAttr* attr, size_t stack_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetstacksize(attr, stack_size));
}

int KYTY_SYSV_ABI pthread_attr_getstacksize(const LibKernel::PthreadAttr* attr, size_t* stack_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetstacksize(attr, stack_size));
}

int KYTY_SYSV_ABI pthread_attr_get_np(LibKernel::Pthread thread, LibKernel::PthreadAttr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGet(thread, attr));
}

int KYTY_SYSV_ABI pthread_attr_getschedpolicy(const LibKernel::PthreadAttr* attr, int* policy)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetschedpolicy(attr, policy));
}

int KYTY_SYSV_ABI pthread_attr_setschedpolicy(LibKernel::PthreadAttr* attr, int policy)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetschedpolicy(attr, policy));
}

int KYTY_SYSV_ABI pthread_attr_setdetachstate(LibKernel::PthreadAttr* attr, int state)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetdetachstate(attr, state));
}

int KYTY_SYSV_ABI pthread_attr_getdetachstate(const LibKernel::PthreadAttr* attr, int* state)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetdetachstate(attr, state));
}

int KYTY_SYSV_ABI pthread_attr_setschedparam(LibKernel::PthreadAttr* attr, const LibKernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetschedparam(attr, param));
}

int KYTY_SYSV_ABI pthread_attr_getschedparam(const LibKernel::PthreadAttr* attr, LibKernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetschedparam(attr, param));
}

int KYTY_SYSV_ABI pthread_attr_setinheritsched(LibKernel::PthreadAttr* attr, int inherit_sched)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetinheritsched(attr, inherit_sched));
}

int KYTY_SYSV_ABI pthread_attr_setguardsize(LibKernel::PthreadAttr* attr, size_t guard_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetguardsize(attr, guard_size));
}

int KYTY_SYSV_ABI pthread_attr_getguardsize(const LibKernel::PthreadAttr* attr, size_t* guard_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrGetguardsize(attr, guard_size));
}

int KYTY_SYSV_ABI pthread_getschedparam(LibKernel::Pthread thread, int* policy, LibKernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadGetschedparam(thread, policy, param));
}

int KYTY_SYSV_ABI pthread_setschedparam(LibKernel::Pthread thread, int policy, const LibKernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadSetschedparam(thread, policy, param));
}

int KYTY_SYSV_ABI pthread_rename_np(LibKernel::Pthread thread, const char* name)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadRename(thread, name));
}

int KYTY_SYSV_ABI pthread_getthreadid_np()
{
	PRINT_NAME();
	return LibKernel::PthreadGetthreadid();
}

int KYTY_SYSV_ABI pthread_mutexattr_setprotocol(LibKernel::PthreadMutexattr* attr, int protocol)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrSetprotocol(attr, protocol));
}

} // namespace Posix

} // namespace Kyty::Libs

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#pragma GCC diagnostic pop
#endif

#endif // KYTY_EMU_ENABLED
