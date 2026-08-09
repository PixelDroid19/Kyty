#include "Emulator/Kernel/Pthread.h"
#include "PthreadInternal.h"
#include "Emulator/Kernel/Errors.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DateTime.h"
#include "Kyty/Core/MemoryAlloc.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Timer.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Kernel/Trace.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/PresentationStats.h"
#include "Emulator/Log.h"

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <pthread.h>
#include <signal.h>
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

namespace Kyty::Kernel {

KERNEL_LIB_NAME();

namespace PresentationStats = Kyty::Emulator::PresentationStats;
namespace GuestRuntimePort  = ::Kyty::Emulator::GuestRuntimePort;


thread_local Pthread g_pthread_self                   = nullptr;
thread_local bool    g_pthread_key_destructors_active = false;
PThreadContext*      g_pthread_context                = nullptr;

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

void PthreadDeleteStaticObjects(const void* program)
{
	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	pthread_static_objects->DeleteObjects(program);
}

bool PthreadIsInitialized()
{
	return g_pthread_context != nullptr;
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


void PthreadSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
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

void PthreadSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void PthreadSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}


Pthread KYTY_SYSV_ABI PthreadSelf()
{
	// PRINT_NAME();

	if (g_pthread_self == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	return g_pthread_self;
}

int KYTY_SYSV_ABI PthreadSignal(Pthread thread, int signum)
{
	if (thread == nullptr || thread->free.load(std::memory_order_acquire))
	{
		return KERNEL_ERROR_ESRCH;
	}

	switch (pthread_kill(thread->p, signum))
	{
		case 0: return OK;
		case ESRCH: return KERNEL_ERROR_ESRCH;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadSignalWithValue(Pthread thread, int signum, void* value)
{
	if (thread == nullptr || thread->free.load(std::memory_order_acquire))
	{
		return KERNEL_ERROR_ESRCH;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	union sigval signal_value {};
	signal_value.sival_ptr = value;
	switch (pthread_sigqueue(thread->p, signum, signal_value))
	{
		case 0: return OK;
		case ESRCH: return KERNEL_ERROR_ESRCH;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		default: return KERNEL_ERROR_EINVAL;
	}
#else
	(void)signum;
	(void)value;
	return KERNEL_ERROR_EINVAL;
#endif
}

static void cleanup_thread(void* arg)
{
	auto* thread = static_cast<Pthread>(arg);

	EXIT_IF(g_pthread_context == nullptr);

	auto thread_dtors = g_pthread_context->GetThreadDtors();
	auto host_thread_dtors = g_pthread_context->GetHostThreadDtors();
	EXIT_IF(thread->attr == nullptr || thread->attr->stack_addr == nullptr || thread->attr->stack_size == 0);
	auto* stack_top =
	    reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(thread->attr->stack_addr) + thread->attr->stack_size) &
	                            ~static_cast<uintptr_t>(0xf));

	if (host_thread_dtors != nullptr)
	{
		host_thread_dtors(stack_top);
	}
	if (thread_dtors != nullptr)
	{
		thread_dtors();
	}

	// POSIX key destructors belong to the exiting thread. They can re-register
	// values for up to DESTRUCTOR_ITERATIONS passes and may still use guest TLS.
	// Keep both the guest stack and the TLS image live until all callbacks finish.
	g_pthread_context->GetPthreadKeys()->Destruct(thread->unique_id, stack_top);
	GuestRuntimePort::ReleaseThreadDynamicTls(thread->unique_id);

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

	if (thread->name.ContainsStr(U"AsyncMemoryThread"))
	{
		usleep(50000);
	}

	EXIT_IF(thread->attr == nullptr || thread->attr->stack_addr == nullptr || thread->attr->stack_size == 0);
	auto* stack_top = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(thread->attr->stack_addr) + thread->attr->stack_size) &
	                                          ~static_cast<uintptr_t>(0xf));
	ret = reinterpret_cast<void*>(GuestRuntimePort::InvokeOnStack(reinterpret_cast<uint64_t>(thread->entry),
	                                                               reinterpret_cast<uint64_t>(thread->arg), 0, 0, stack_top));

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
	if (attr == nullptr || *attr == nullptr || !IsValidGuestSchedPolicy((*attr)->policy) ||
	    !IsValidGuestPriority((*attr)->priority))
	{
		return KERNEL_ERROR_EINVAL;
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

		(*thread)->name        = name != nullptr ? name : "";
		(*thread)->entry       = entry;
		(*thread)->arg         = arg;
		(*thread)->almost_done = false;
		(*thread)->detached    = (*attr)->detached;
		(*thread)->started     = false;
		(*thread)->unique_id   = -1;
		(*thread)->guest_policy.store((*thread)->attr->policy, std::memory_order_relaxed);
		(*thread)->guest_priority.store((*thread)->attr->priority, std::memory_order_relaxed);
		(*thread)->guest_stack_base = 0;
		(*thread)->guest_stack_size = 0;

			result = create_guest_stack((*thread)->attr);
			if (result == OK)
			{
				(*thread)->guest_stack_base = reinterpret_cast<uint64_t>((*thread)->attr->stack_addr);
				(*thread)->guest_stack_size = (*thread)->attr->stack_size;
			}

			if (result == OK)
			{
				result = pthread_create(&(*thread)->p, &(*thread)->attr->p, run_thread, *thread);
				if (result != 0)
				{
					free_guest_stack((*thread)->attr);
					(*thread)->guest_stack_base = 0;
					(*thread)->guest_stack_size = 0;
				}
			}
		}

	// Do not wait for the child to enter its guest entry. Real pthread_create
	// returns as soon as the thread is constructed; the parent may still run
	// setup (e.g. sceKernelCreateEventFlag "ThreadFlag") before the child is
	// scheduled. Waiting for `started` inverted that order on Linux: the
	// VibrationTrackThread reached KernelWaitEventFlag with an uninitialized
	// handle (observed poison 0xcccccccc00007fff) and SIGSEGV'd in Mutex::Lock.
	// unique_id may still be -1 in the create log until the child runs.

	KYTY_LOG_DEBUG("\tthread create: %s, id = %d, %d\n", (*thread)->name.C_Str(), (*thread)->unique_id, result);

	pthread_attr_dbg_print(attr);

	PRINT_NAME_ENABLE(true);

	if (result < 0)
	{
		return result;
	}

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

	KYTY_LOG_DEBUG("\tthread detach: %s, %d\n", thread->name.C_Str(), 0);

	thread->detached = true;

	return OK;
}

static int pthread_join_internal(Pthread thread, void** value)
{
	int result = pthread_join(thread->p, value);

	if (PRINT_NAME_ENABLED)
	{
		KYTY_LOG_DEBUG("\tthread join: %s, %d\n", thread->name.C_Str(), result);
	}

	if (result == 0)
	{
		free_guest_stack(thread->attr);
		thread->guest_stack_base = 0;
		thread->guest_stack_size = 0;
		thread->almost_done      = false;
		thread->free             = true;
	}

	switch (result)
	{
		case 0: return OK;
		case ESRCH: return KERNEL_ERROR_ESRCH;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EOPNOTSUPP: return KERNEL_ERROR_EOPNOTSUPP;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadJoin(Pthread thread, void** value)
{
	PRINT_NAME();

	if (thread == nullptr || thread->detached.load(std::memory_order_acquire))
	{
		return KERNEL_ERROR_EINVAL;
	}

	return pthread_join_internal(thread, value);
}

int PthreadReapDetached(Pthread thread)
{
	if (thread == nullptr || !thread->detached.load(std::memory_order_acquire))
	{
		return KERNEL_ERROR_EINVAL;
	}

	return pthread_join_internal(thread, nullptr);
}

int KYTY_SYSV_ABI PthreadCancel(Pthread thread)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_cancel(thread->p);

	KYTY_LOG_DEBUG("\tthread cancel: %s, %d\n", thread->name.C_Str(), result);

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
		default: return KERNEL_ERROR_EINVAL;
	}

	int host_old_state = PTHREAD_CANCEL_DISABLE;
	int result         = pthread_setcancelstate(pstate, old_state != nullptr ? &host_old_state : nullptr);

	KYTY_LOG_DEBUG("\tthread setcancelstate: %d\n", result);

	if (result != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (old_state == nullptr)
	{
		return OK;
	}

	switch (host_old_state)
	{
		case PTHREAD_CANCEL_ENABLE: *old_state = 0; return OK;
		case PTHREAD_CANCEL_DISABLE: *old_state = 1; return OK;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadSetcanceltype(int type, int* old_type)
{
	PRINT_NAME();

	int ptype = PTHREAD_CANCEL_DEFERRED;

	switch (type)
	{
		case 0: ptype = PTHREAD_CANCEL_DEFERRED; break;
		case 2: ptype = PTHREAD_CANCEL_ASYNCHRONOUS; break;
		default: return KERNEL_ERROR_EINVAL;
	}

	int host_old_type = PTHREAD_CANCEL_DEFERRED;
	int result        = pthread_setcanceltype(ptype, old_type != nullptr ? &host_old_type : nullptr);

	KYTY_LOG_DEBUG("\tthread setcanceltype: %d\n", result);

	if (result != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (old_type == nullptr)
	{
		return OK;
	}

	switch (host_old_type)
	{
		case PTHREAD_CANCEL_DEFERRED: *old_type = 0; return OK;
		case PTHREAD_CANCEL_ASYNCHRONOUS: *old_type = 2; return OK;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadGetprio(Pthread thread, int* prio)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	if (prio == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*prio = thread->guest_priority.load(std::memory_order_relaxed);

	KYTY_LOG_DEBUG("\t PthreadGetprio: %d, %d\n", thread->unique_id, *prio);

	return OK;
}

int KYTY_SYSV_ABI PthreadSetprio(Pthread thread, int prio)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}
	if (!IsValidGuestPriority(prio))
	{
		return KERNEL_ERROR_EINVAL;
	}

	thread->guest_priority.store(prio, std::memory_order_relaxed);

	KYTY_LOG_DEBUG("\t PthreadSetprio: %d, %d\n", thread->unique_id, prio);

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
		KYTY_LOG_DEBUG("\t PthreadRename: %s\n", name);
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

	// Guest scheduler values are part of the guest ABI. Passing them to the
	// host scheduler makes results privilege- and platform-dependent.
	*policy               = thread->guest_policy.load(std::memory_order_relaxed);
	param->sched_priority = thread->guest_priority.load(std::memory_order_relaxed);
	return OK;
}

int KYTY_SYSV_ABI PthreadSetschedparam(Pthread thread, int policy, const KernelSchedParam* param)
{
	PRINT_NAME();

	if (thread == nullptr || param == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (!IsValidGuestSchedPolicy(policy) || !IsValidGuestPriority(param->sched_priority))
	{
		return KERNEL_ERROR_EINVAL;
	}

	thread->guest_policy.store(policy, std::memory_order_relaxed);
	thread->guest_priority.store(param->sched_priority, std::memory_order_relaxed);
	return OK;
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
	if (g_pthread_context->GetThreadDtors() != nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	g_pthread_context->SetThreadDtors(dtors);
	// g_thread_dtors = dtors;
}

void PthreadSetHostThreadDtors(host_thread_dtors_func_t dtors)
{
	EXIT_IF(g_pthread_context == nullptr || dtors == nullptr);

	const auto current = g_pthread_context->GetHostThreadDtors();
	EXIT_IF(current != nullptr && current != dtors);
	g_pthread_context->SetHostThreadDtors(dtors);
}

int KYTY_SYSV_ABI KernelUsleep(KernelUseconds microseconds)
{
	Core::Thread::SleepMicro(microseconds);
	return OK;
}

unsigned int KYTY_SYSV_ABI KernelSleep(unsigned int seconds)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\tsleep: %u\n", seconds);
	Core::Timer t;
	t.Start();
	Core::Thread::Sleep(seconds);
	double ts = t.GetTimeS();
	KYTY_LOG_DEBUG("\tactual: %g seconds\n", ts);
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
		PresentationStats::Snapshot stats {};
		if (PresentationStats::GetPort().Query(&stats) && stats.present >= 2200ull)
		{
			static std::atomic<uint32_t> ns_logs {0};
			const uint32_t               n = ns_logs.fetch_add(1);
			if (n < 64u || (n % 4u) == 0u)
			{
				const auto         ret        = reinterpret_cast<uint64_t>(__builtin_return_address(0));
				constexpr uint64_t kSlotTable  = 0x901c434c8ull;
				constexpr uint64_t kSlotStride = 0x20ull;
				KYTY_LOG_DEBUG( "SLOT_TRACE ns=%" PRIu64 " ret=0x%016" PRIx64 " n=%u present=%llu\n", nanos, ret, n,
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
					KYTY_LOG_DEBUG(
					             "SLOT[%u] typ=0x%" PRIx64 " obj=0x%016" PRIx64 " state=%u/%u fn=0x%016" PRIx64 "\n", i, typ, obj, s0,
					             s1, fn);
				}
			}
		}
	}

	KYTY_LOG_DEBUG("\tnanosleep: %" PRIu64 "\n", nanos);

	Core::Timer t;
	t.Start();
	Core::Thread::SleepNano(nanos);
	double ts = t.GetTimeS();
	KYTY_LOG_DEBUG("\tactual: %g nanoseconds\n", ts * 1000000000.0);

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

	KYTY_LOG_DEBUG("\t destructor = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(destructor));
	KYTY_LOG_DEBUG("\t key        = %d\n", *key);

	return OK;
}

int KYTY_SYSV_ABI PthreadKeyDelete(PthreadKey key)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t key = %d\n", key);

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

	KYTY_LOG_DEBUG("\t key       = %d\n", key);
	KYTY_LOG_DEBUG("\t thread_id = %d\n", thread_id);
	KYTY_LOG_DEBUG("\t value     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(value));

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

	KYTY_LOG_DEBUG("\t key       = %d\n", key);
	KYTY_LOG_DEBUG("\t thread_id = %d\n", thread_id);

	EXIT_IF(g_pthread_context == nullptr || g_pthread_context->GetPthreadKeys() == nullptr);

	void* value = nullptr;

	if (!g_pthread_context->GetPthreadKeys()->Get(key, thread_id, &value))
	{
		return nullptr;
	}

	KYTY_LOG_DEBUG("\t value     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(value));

	return value;
}

} // namespace Kyty::Kernel

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#pragma GCC diagnostic pop
#endif

#endif // KYTY_EMU_ENABLED
