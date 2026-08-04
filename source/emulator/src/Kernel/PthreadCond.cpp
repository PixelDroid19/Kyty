#include "PthreadInternal.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Trace.h"
#include "Emulator/Log.h"
#include "Emulator/PresentationStats.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

KERNEL_LIB_NAME();

namespace PresentationStats = Kyty::Emulator::PresentationStats;
namespace GuestRuntimePort  = ::Kyty::Emulator::GuestRuntimePort;

int KYTY_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr)
{
	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

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

	if (attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*attr = new PthreadCondattrPrivate {};

	int result = pthread_condattr_init(&(*attr)->p);

	switch (result)
	{
		case 0: return OK;
		case ENOMEM:
			delete *attr;
			*attr = nullptr;
			return KERNEL_ERROR_ENOMEM;
		default:
			delete *attr;
			*attr = nullptr;
			return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadCondattrSetclock(PthreadCondattr* attr, KernelClockid clock_id)
{
	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KernelClockSource source {};
	if (!KernelClockSourceFromId(clock_id, &source))
	{
		return KERNEL_ERROR_EINVAL;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	const clockid_t host_clock = source == KernelClockSource::Realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC;
	if (pthread_condattr_setclock(&(*attr)->p, host_clock) != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
#else
	// Platforms without pthread_condattr_setclock can only represent the
	// default realtime contract; reject a different guest clock explicitly.
	if (source != KernelClockSource::Realtime)
	{
		return KERNEL_ERROR_EINVAL;
	}
#endif

	(*attr)->clock_id = clock_id;
	return OK;
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
	std::atomic<uint64_t> last_return_addr {0};
	std::atomic<uint32_t> count {0};
	std::atomic<uint32_t> after_stall {0};
};
static constexpr uint32_t    kSlotTraceSigSlots = 16;
static SlotTraceSigCount     g_slot_trace_sigs[kSlotTraceSigSlots];

static bool slot_trace_env()
{
	return std::getenv("KYTY_SLOT_TRACE") != nullptr;
}

static void slot_trace_note_signal(uint64_t cond_addr, uint64_t return_addr)
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
			g_slot_trace_sigs[i].last_return_addr.store(return_addr, std::memory_order_relaxed);
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
				g_slot_trace_sigs[i].last_return_addr.store(return_addr, std::memory_order_relaxed);
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

static bool slot_trace_cond_active(PresentationStats::Snapshot* stats_out)
{
	if (!slot_trace_env())
	{
		return false;
	}
	PresentationStats::Snapshot stats {};
	if (!PresentationStats::GetPort().Query(&stats) || stats.present < 2200ull)
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

static uint64_t slot_trace_last_signal_return_addr(uint64_t cond)
{
	for (uint32_t i = 0; i < kSlotTraceSigSlots; i++)
	{
		if (g_slot_trace_sigs[i].cond.load() == cond)
		{
			return g_slot_trace_sigs[i].last_return_addr.load(std::memory_order_relaxed);
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
		waiter.last_signal_return_addr = slot_trace_last_signal_return_addr(waiter.cond);
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
	KYTY_LOG_DEBUG( "COND_BLOCKED tracked=0x%016" PRIx64 " wait_reg=%u sig_total_trk=%u sig_after_stall=%u\n",
	             g_slot_trace_tracked_cond.load(), g_slot_trace_wait_for_tracked.load(), g_slot_trace_sig_for_tracked.load(),
	             g_slot_trace_sig_after_stall.load());
	for (uint32_t i = 0; i < kSlotTraceWaiterSlots; i++)
	{
		if (g_slot_trace_waiters[i].live.load() == 0)
		{
			continue;
		}
		KYTY_LOG_DEBUG(
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
		KYTY_LOG_DEBUG( "COND_SIGCNT cond=0x%016" PRIx64 " total=%u after_stall=%u last_ret=0x%016" PRIx64 "\n", c,
		             g_slot_trace_sigs[i].count.load(), g_slot_trace_sigs[i].after_stall.load(),
		             g_slot_trace_sigs[i].last_return_addr.load(std::memory_order_relaxed));
	}
}

static void slot_trace_cond_event(const char* kind, PthreadCond* guest_cond, PthreadMutex* guest_mutex, uint64_t ret)
{
	PresentationStats::Snapshot stats {};
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

	KYTY_LOG_DEBUG(
	             "COND_TRACE %s n=%u present=%llu ms_p=%llu ret=0x%016" PRIx64 " cond=0x%016" PRIx64 " cond_h=0x%016" PRIx64
	             " mutex=0x%016" PRIx64 " mutex_h=0x%016" PRIx64 " tracked=0x%016" PRIx64 " sig_trk=%u sig_stall=%u\n",
	             kind, seq, static_cast<unsigned long long>(stats.present), static_cast<unsigned long long>(stats.ms_since_present), ret,
	             cond_addr, cond_h, mutex_addr, mutex_h, g_slot_trace_tracked_cond.load(), g_slot_trace_sig_for_tracked.load(),
	             g_slot_trace_sig_after_stall.load());
}

int KYTY_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond)
{
	PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	const auto ret_addr = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	slot_trace_note_signal(reinterpret_cast<uint64_t>(cond), ret_addr);
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

	(*cond)->name     = name;
	(*cond)->clock_id = (*attr)->clock_id;

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
	slot_trace_note_signal(reinterpret_cast<uint64_t>(cond), ret_addr);
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

struct ResolvedCondWait
{
	PthreadCondPrivate*  cond  = nullptr;
	PthreadMutexPrivate* mutex = nullptr;
};

static int resolve_cond_wait(PthreadCond* cond, PthreadMutex* mutex, ResolvedCondWait* out)
{
	if (cond == nullptr || mutex == nullptr || out == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// Lazily initialize statically-initialized cond/mutex (sentinel values), same as
	// PthreadCondWait; otherwise the sentinel would be dereferenced as a handle.
	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	EXIT_IF(pthread_static_objects == nullptr);
	cond  = static_cast<PthreadCond*>(pthread_static_objects->CreateObject(cond, PthreadStaticObject::Type::Cond));
	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	if (*cond == nullptr || *mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	out->cond  = *cond;
	out->mutex = *mutex;
	return OK;
}

static int wait_on_resolved_cond(const ResolvedCondWait& resolved, const timespec& deadline)
{
	int result = pthread_cond_release_mutex_state(resolved.mutex);
	if (result == 0)
	{
		result = pthread_cond_timedwait(&resolved.cond->p, &resolved.mutex->p, &deadline);
		pthread_cond_restore_mutex_state(resolved.mutex);
	}

	switch (result)
	{
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EPERM: return KERNEL_ERROR_EPERM;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec)
{
	PRINT_NAME();

	ResolvedCondWait resolved {};
	const int        resolve_result = resolve_cond_wait(cond, mutex, &resolved);
	if (resolve_result != OK)
	{
		return resolve_result;
	}

	timespec deadline {};
	if (!relative_usec_to_absolute_timespec(resolved.cond->clock_id, usec, &deadline))
	{
		return KERNEL_ERROR_EINVAL;
	}

	return wait_on_resolved_cond(resolved, deadline);
}

int KYTY_SYSV_ABI PthreadCondTimedwaitAbsolute(PthreadCond* cond, PthreadMutex* mutex, const KernelTimespec* abstime)
{
	PRINT_NAME();

	ResolvedCondWait resolved {};
	const int        resolve_result = resolve_cond_wait(cond, mutex, &resolved);
	if (resolve_result != OK)
	{
		return resolve_result;
	}

	timespec deadline {};
	if (!guest_absolute_to_timespec(abstime, &deadline))
	{
		return KERNEL_ERROR_EINVAL;
	}

	return wait_on_resolved_cond(resolved, deadline);
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
		(void)GuestRuntimePort::Invoke(reinterpret_cast<uint64_t>(init_routine), 0, 0, 0);

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

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED
