#ifndef EMULATOR_SRC_KERNEL_PTHREADINTERNAL_H_
#define EMULATOR_SRC_KERNEL_PTHREADINTERNAL_H_

#include "Emulator/Kernel/Pthread.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include <atomic>
#include <ctime>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

#include <pthread.h>

namespace Kyty::Kernel {

constexpr int KEYS_MAX              = 256;
constexpr int DESTRUCTOR_ITERATIONS = 4;
constexpr int MUTEX_TYPE_ERRORCHECK = 1;
constexpr int MUTEX_TYPE_RECURSIVE  = 2;
constexpr int MUTEX_TYPE_NORMAL     = 3;
constexpr int MUTEX_TYPE_ADAPTIVE   = 4;

constexpr size_t GUEST_PTHREAD_STACK_MIN = 0x4000;
constexpr size_t PTHREAD_STACK_PAGE      = 0x4000;
constexpr size_t PTHREAD_STACK_EXTRA     = 0x100000;
constexpr size_t HOST_PTHREAD_STACK_MIN  = 0x100000;
constexpr size_t MAIN_GUEST_STACK_MIN    = 64ull * 1024ull * 1024ull;

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
	void*          stack_addr = nullptr;
	size_t         stack_size = 0;
	bool           stack_owned = false;
	uint64_t       stack_map_addr = 0;
	size_t         stack_map_size = 0;
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
	KernelClockid      clock_id = 0;
	pthread_condattr_t p;
};

struct PthreadCondPrivate
{
	uint8_t        reserved[256];
	String         name;
	KernelClockid  clock_id = 0;
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
	const void* program;
};

class PthreadStaticObjects
{
public:
	PthreadStaticObjects() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); } }
	virtual ~PthreadStaticObjects() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PthreadStaticObjects);

	void* CreateObject(void* addr, PthreadStaticObject::Type type);
	void  DeleteObjects(const void* program);

private:
	Vector<PthreadStaticObject*> m_objects;
	Core::Mutex                  m_mutex;
};

class PthreadKeys
{
public:
	PthreadKeys() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); } }
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
	PthreadPool() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); } }
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
	PThreadContext() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); } }
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
	[[nodiscard]] host_thread_dtors_func_t GetHostThreadDtors() const { return m_host_thread_dtors; }
	void SetHostThreadDtors(host_thread_dtors_func_t dtors) { m_host_thread_dtors = dtors; }

private:
	PthreadMutexattr      m_default_mutexattr      = nullptr;
	PthreadRwlockattr     m_default_rwlockattr     = nullptr;
	PthreadCondattr       m_default_condattr       = nullptr;
	PthreadAttr           m_default_attr           = nullptr;
	PthreadPool*          m_pthread_pool           = nullptr;
	PthreadStaticObjects* m_pthread_static_objects = nullptr;
	PthreadKeys*          m_pthread_keys           = nullptr;

	std::atomic<thread_dtors_func_t>      m_thread_dtors      = nullptr;
	std::atomic<host_thread_dtors_func_t> m_host_thread_dtors = nullptr;
};

extern thread_local Pthread g_pthread_self;
extern PThreadContext*      g_pthread_context;

bool relative_usec_to_absolute_timespec(KernelClockid clock_id, KernelUseconds usec, timespec* deadline);
bool guest_absolute_to_timespec(const KernelTimespec* abstime, timespec* deadline);
void sec_to_timespec(KernelTimespec* ts, double sec);

int  create_guest_stack(PthreadAttr attr);
void free_guest_stack(PthreadAttr attr);
int  pthread_attr_copy(PthreadAttr* dst, const PthreadAttr* src);
void pthread_attr_dbg_print(const PthreadAttr* src);

#ifdef __APPLE__
void usec_to_timespec(struct timespec* ts, KernelUseconds usec);
int  mutex_timedlock_poll(pthread_mutex_t* mutex, const timespec* t);
int pthread_rwlock_timedrdlock(pthread_rwlock_t* lock, const timespec* t);
int pthread_rwlock_timedwrlock(pthread_rwlock_t* lock, const timespec* t);
#endif

bool PthreadQueryStack(const void* addr, void** start, void** end);

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_SRC_KERNEL_PTHREADINTERNAL_H_
