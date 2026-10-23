#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/GuestCodeRanges.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Pthread.h"

#include "PthreadInternal.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <asm/prctl.h>
#include <csignal>
#include <ctime>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

// glibc stores the SIGEV_THREAD_ID target in this field without naming it.
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif
#endif

#ifdef KYTY_EMU_ENABLED

// A guest collector stops the world by raising signal 30 on each of its threads
// and waiting for the installed handler to acknowledge with the thread's
// register context. The handler then blocks until the world restarts, so it
// must never run where the interrupted thread holds a host lock: that lock
// would stay held for the whole collection, and any thread that needs it,
// including the interrupted thread inside the handler, would deadlock.
//
// A request is therefore delivered only at a safe point: while the thread runs
// guest code, or while it is blocked in a wait opened by
// PthreadInterruptibleScope, which waits without holding a host lock. A signal
// that lands anywhere else re-arms a per-thread retry timer and returns, so the
// request is delivered as soon as the thread next reaches a safe point.

namespace Kyty::Kernel {

namespace GuestCodeRanges  = ::Kyty::Emulator::GuestCodeRanges;
namespace GuestRuntimePort = ::Kyty::Emulator::GuestRuntimePort;

namespace {

constexpr uint32_t kContextOpen    = 1u << 0u; // the thread runs and accepts requests
constexpr uint32_t kContextPending = 1u << 1u; // a request waits for the next safe point
constexpr uint32_t kContextRunning = 1u << 2u; // the guest handler runs on the thread

constexpr int      kGuestContextSignal = 30;
constexpr uint32_t kRetryMicros        = 100;

std::mutex            g_context_mutex;
std::atomic<uint64_t> g_context_handler {0};

} // namespace

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX

namespace {

// Guest ucontext layout handed to the handler.
struct SignalMcontext
{
	uint64_t mc_onstack;
	uint64_t mc_rdi;
	uint64_t mc_rsi;
	uint64_t mc_rdx;
	uint64_t mc_rcx;
	uint64_t mc_r8;
	uint64_t mc_r9;
	uint64_t mc_rax;
	uint64_t mc_rbx;
	uint64_t mc_rbp;
	uint64_t mc_r10;
	uint64_t mc_r11;
	uint64_t mc_r12;
	uint64_t mc_r13;
	uint64_t mc_r14;
	uint64_t mc_r15;
	int      mc_trapno;
	uint16_t mc_fs;
	uint16_t mc_gs;
	uint64_t mc_addr;
	int      mc_flags;
	uint16_t mc_es;
	uint16_t mc_ds;
	uint64_t mc_err;
	uint64_t mc_rip;
	uint64_t mc_cs;
	uint64_t mc_rflags;
	uint64_t mc_rsp;
	uint64_t mc_ss;
	uint64_t mc_len;
	uint64_t mc_fpformat;
	uint64_t mc_ownedfp;
	uint64_t mc_lbrfrom;
	uint64_t mc_lbrto;
	uint64_t mc_aux1;
	uint64_t mc_aux2;
	uint64_t mc_fpstate[104];
	uint64_t mc_fsbase;
	uint64_t mc_gsbase;
	uint64_t mc_spare[6];
};

struct SignalUcontext
{
	uint8_t        uc_sigmask[0x10] {};
	uint8_t        uc_private[0x30] {};
	SignalMcontext uc_mcontext;
};

static_assert(offsetof(SignalUcontext, uc_mcontext) == 0x40);
static_assert(offsetof(SignalMcontext, mc_rip) == 0xa0);
static_assert(offsetof(SignalMcontext, mc_rsp) == 0xb8);
static_assert(offsetof(SignalMcontext, mc_fsbase) == 0x440);
static_assert(offsetof(SignalMcontext, mc_gsbase) == 0x448);

constexpr size_t kTrampolineStackSize = 1024 * 1024;

} // namespace

// Allocated on a thread's first request and kept with its pooled PthreadPrivate.
struct PthreadContextDelivery
{
	SignalUcontext             guest_context {};
	ucontext_t                 resume_context {};
	ucontext_t*                signal_context = nullptr;
	uintptr_t                  signal_frame   = 0;
	timer_t                    retry_timer {};
	bool                       has_retry_timer = false;
	std::unique_ptr<uint8_t[]> trampoline_stack;
};

namespace {

int ContextSignal()
{
	return SIGRTMIN + 6;
}

// Linux does not place FSBASE/GSBASE in the public x86_64 gregset. The request
// is delivered on the interrupted thread, so ARCH_GET_* reads its exact bases.
uint64_t ReadSegmentBase(int selector)
{
	uint64_t value = 0;
	return (::syscall(SYS_arch_prctl, selector, &value) == 0 ? value : 0);
}

void CopyGuestContext(SignalMcontext* guest, const ucontext_t* host)
{
	const auto& gregs = host->uc_mcontext.gregs;
	guest->mc_len     = sizeof(SignalMcontext);
	guest->mc_rdi     = static_cast<uint64_t>(gregs[REG_RDI]);
	guest->mc_rsi     = static_cast<uint64_t>(gregs[REG_RSI]);
	guest->mc_rdx     = static_cast<uint64_t>(gregs[REG_RDX]);
	guest->mc_rcx     = static_cast<uint64_t>(gregs[REG_RCX]);
	guest->mc_r8      = static_cast<uint64_t>(gregs[REG_R8]);
	guest->mc_r9      = static_cast<uint64_t>(gregs[REG_R9]);
	guest->mc_rax     = static_cast<uint64_t>(gregs[REG_RAX]);
	guest->mc_rbx     = static_cast<uint64_t>(gregs[REG_RBX]);
	guest->mc_rbp     = static_cast<uint64_t>(gregs[REG_RBP]);
	guest->mc_r10     = static_cast<uint64_t>(gregs[REG_R10]);
	guest->mc_r11     = static_cast<uint64_t>(gregs[REG_R11]);
	guest->mc_r12     = static_cast<uint64_t>(gregs[REG_R12]);
	guest->mc_r13     = static_cast<uint64_t>(gregs[REG_R13]);
	guest->mc_r14     = static_cast<uint64_t>(gregs[REG_R14]);
	guest->mc_r15     = static_cast<uint64_t>(gregs[REG_R15]);
	guest->mc_rip     = static_cast<uint64_t>(gregs[REG_RIP]);
	guest->mc_rflags  = static_cast<uint64_t>(gregs[REG_EFL]);
	guest->mc_rsp     = static_cast<uint64_t>(gregs[REG_RSP]);
	guest->mc_fsbase  = ReadSegmentBase(ARCH_GET_FS);
	guest->mc_gsbase  = ReadSegmentBase(ARCH_GET_GS);
}

// Linux leaves a blocking syscall that this signal interrupted either failed
// with EINTR, where RCX (loaded by the syscall instruction with its return
// address) equals RIP, or rewound onto the 2-byte syscall instruction for a
// restart, where RCX equals RIP + 2. RDI, the first argument, names the object
// the syscall waited on.
bool AtSafePoint(const PthreadPrivate* self, const mcontext_t& mcontext)
{
	const auto rip = static_cast<uint64_t>(mcontext.gregs[REG_RIP]);
	if (GuestCodeRanges::Contains(rip))
	{
		return true;
	}
	const auto rax                 = static_cast<uint64_t>(mcontext.gregs[REG_RAX]);
	const auto rcx                 = static_cast<uint64_t>(mcontext.gregs[REG_RCX]);
	const auto rdi                 = static_cast<uint64_t>(mcontext.gregs[REG_RDI]);
	const bool interrupted_syscall = (rcx == rip && rax == static_cast<uint64_t>(-EINTR)) || rcx == rip + 2;
	return interrupted_syscall && rdi >= self->interruptible_begin.load(std::memory_order_relaxed) &&
	       rdi < self->interruptible_end.load(std::memory_order_relaxed);
}

void ArmRetry(PthreadContextDelivery* delivery)
{
	itimerspec retry {};
	retry.it_value.tv_nsec = static_cast<long>(kRetryMicros) * 1000;
	(void)timer_settime(delivery->retry_timer, 0, &retry, nullptr);
}

[[noreturn]] void ContextTrampoline(PthreadPrivate* self)
{
	auto*     delivery    = self->context_delivery;
	const int saved_errno = errno;
	(void)GuestRuntimePort::Invoke(g_context_handler.load(std::memory_order_acquire), kGuestContextSignal,
	                               reinterpret_cast<uint64_t>(&delivery->guest_context), 0);

	// The signal stays blocked until rt_sigreturn restores the interrupted mask,
	// so a request raised while the handler ran is delivered right after the
	// resume, at the same safe point.
	sigset_t blocked {};
	sigemptyset(&blocked);
	sigaddset(&blocked, ContextSignal());
	pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
	if ((self->context_state.fetch_and(~kContextRunning, std::memory_order_acq_rel) & kContextPending) != 0)
	{
		::syscall(SYS_tgkill, getpid(), self->host_tid, ContextSignal());
	}
	errno = saved_errno;

	// Returning through rt_sigreturn instead of setcontext preserves Linux's
	// restart of the interrupted syscall. libc's signal restorer pops the
	// synthetic return address before rt_sigreturn, leaving RSP at the ucontext.
	*delivery->signal_context = delivery->resume_context;
	asm volatile("mov %0, %%rsp\n\t"
	             "mov %1, %%rax\n\t"
	             "syscall\n\t"
	             :
	             : "r"(delivery->signal_frame), "i"(SYS_rt_sigreturn)
	             : "memory", "rax");
	__builtin_unreachable();
}

// Runs on the signalled thread; only atomics, copies and raw syscalls are used.
void HandleContextSignal(const siginfo_t* info, ucontext_t* context)
{
	auto* self = g_pthread_self;
	if (self == nullptr || (info->si_code != SI_TKILL && info->si_code != SI_TIMER))
	{
		return;
	}
	const uint32_t state = self->context_state.load(std::memory_order_acquire);
	if ((state & kContextPending) == 0 || (state & kContextRunning) != 0)
	{
		return;
	}
	auto* delivery = self->context_delivery;
	if (!AtSafePoint(self, context->uc_mcontext))
	{
		self->context_deferred_rip.store(static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RIP]), std::memory_order_relaxed);
		ArmRetry(delivery);
		return;
	}

	// Requests raised up to here are answered by this delivery.
	self->context_state.fetch_or(kContextRunning, std::memory_order_acq_rel);
	self->context_state.fetch_and(~kContextPending, std::memory_order_acq_rel);

	CopyGuestContext(&delivery->guest_context.uc_mcontext, context);
	delivery->resume_context = *context;
	delivery->signal_context = context;
	delivery->signal_frame   = reinterpret_cast<uintptr_t>(context);

	uintptr_t stack_top = reinterpret_cast<uintptr_t>(delivery->trampoline_stack.get()) + kTrampolineStackSize;
	stack_top &= ~static_cast<uintptr_t>(0xf);
	stack_top -= sizeof(uintptr_t);
	*reinterpret_cast<uintptr_t*>(stack_top) = 0;
	context->uc_mcontext.gregs[REG_RIP]      = reinterpret_cast<greg_t>(ContextTrampoline);
	context->uc_mcontext.gregs[REG_RDI]      = reinterpret_cast<greg_t>(self);
	context->uc_mcontext.gregs[REG_RSP]      = static_cast<greg_t>(stack_top);
}

void ContextSignalHandler(int /*signal*/, siginfo_t* info, void* raw_context)
{
	if (info == nullptr || raw_context == nullptr)
	{
		return;
	}
	const int saved_errno = errno;
	HandleContextSignal(info, static_cast<ucontext_t*>(raw_context));
	errno = saved_errno;
}

// Called with g_context_mutex held.
int PrepareDelivery(PthreadPrivate* thread)
{
	if (thread->context_delivery == nullptr)
	{
		auto* delivery = new (std::nothrow) PthreadContextDelivery {};
		if (delivery == nullptr)
		{
			return KERNEL_ERROR_ENOMEM;
		}
		delivery->trampoline_stack.reset(new (std::nothrow) uint8_t[kTrampolineStackSize]);
		if (delivery->trampoline_stack == nullptr)
		{
			delete delivery;
			return KERNEL_ERROR_ENOMEM;
		}
		thread->context_delivery = delivery;
	}
	auto* delivery = thread->context_delivery;
	if (!delivery->has_retry_timer)
	{
		sigevent event {};
		event.sigev_notify           = SIGEV_THREAD_ID;
		event.sigev_signo            = ContextSignal();
		event.sigev_notify_thread_id = thread->host_tid;
		if (timer_create(CLOCK_MONOTONIC, &event, &delivery->retry_timer) != 0)
		{
			return KERNEL_ERROR_EAGAIN;
		}
		delivery->has_retry_timer = true;
	}
	return OK;
}

} // namespace

bool PthreadInstallContextHandler(uint64_t handler)
{
	EXIT_IF(handler == 0);

	struct sigaction action {};
	action.sa_sigaction = ContextSignalHandler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	if (sigaction(ContextSignal(), &action, nullptr) != 0)
	{
		return false;
	}
	g_context_handler.store(handler, std::memory_order_release);
	return true;
}

int PthreadRaiseContext(Pthread thread)
{
	EXIT_IF(thread == nullptr);

	std::lock_guard lock(g_context_mutex);
	if ((thread->context_state.load(std::memory_order_relaxed) & kContextOpen) == 0)
	{
		return KERNEL_ERROR_ESRCH;
	}
	if (g_context_handler.load(std::memory_order_acquire) == 0)
	{
		return OK;
	}
	// A created thread that has not started yet takes the request when it starts.
	if (thread->host_tid == 0)
	{
		thread->context_state.fetch_or(kContextPending, std::memory_order_acq_rel);
		return OK;
	}
	if (const int prepared = PrepareDelivery(thread); prepared != OK)
	{
		return prepared;
	}
	if ((thread->context_state.fetch_or(kContextPending, std::memory_order_acq_rel) & (kContextPending | kContextRunning)) != 0)
	{
		return OK;
	}
	if (::syscall(SYS_tgkill, getpid(), thread->host_tid, ContextSignal()) != 0)
	{
		thread->context_state.fetch_and(~kContextPending, std::memory_order_acq_rel);
		return KERNEL_ERROR_EAGAIN;
	}
	return OK;
}

// Called with g_context_mutex held by the starting thread. A request raised
// before the start was already accepted, so a failure to deliver it is fatal.
static void DeliverRequestRaisedBeforeStart(PthreadPrivate* thread)
{
	if ((thread->context_state.load(std::memory_order_acquire) & kContextPending) == 0)
	{
		return;
	}
	EXIT_IF(PrepareDelivery(thread) != OK);
	EXIT_IF(::syscall(SYS_tgkill, getpid(), thread->host_tid, ContextSignal()) != 0);
}

static void ReleaseRetryTimer(PthreadPrivate* thread)
{
	auto* delivery = thread->context_delivery;
	if (delivery != nullptr && delivery->has_retry_timer)
	{
		EXIT_IF(timer_delete(delivery->retry_timer) != 0);
		delivery->has_retry_timer = false;
	}
}

static int CurrentHostThreadId()
{
	return static_cast<int>(::syscall(SYS_gettid));
}

#else

struct PthreadContextDelivery
{
};

bool PthreadInstallContextHandler(uint64_t /*handler*/)
{
	return false;
}

int PthreadRaiseContext(Pthread thread)
{
	EXIT_IF(thread == nullptr);

	std::lock_guard lock(g_context_mutex);
	return ((thread->context_state.load(std::memory_order_relaxed) & kContextOpen) == 0 ? KERNEL_ERROR_ESRCH : OK);
}

static void DeliverRequestRaisedBeforeStart(PthreadPrivate* /*thread*/) {}

static void ReleaseRetryTimer(PthreadPrivate* /*thread*/) {}

static int CurrentHostThreadId()
{
	return 0;
}

#endif

void PthreadOpenContext(PthreadPrivate* thread)
{
	std::lock_guard lock(g_context_mutex);
	thread->host_tid = 0;
	thread->interruptible_begin.store(0, std::memory_order_relaxed);
	thread->interruptible_end.store(0, std::memory_order_relaxed);
	thread->context_deferred_rip.store(0, std::memory_order_relaxed);
	thread->context_state.store(kContextOpen, std::memory_order_release);
}

void PthreadStartContext(PthreadPrivate* thread)
{
	std::lock_guard lock(g_context_mutex);
	thread->host_tid = CurrentHostThreadId();
	DeliverRequestRaisedBeforeStart(thread);
}

void PthreadCloseContext(PthreadPrivate* thread)
{
	bool started = false;
	{
		std::lock_guard lock(g_context_mutex);
		thread->context_state.fetch_and(~kContextOpen, std::memory_order_acq_rel);
		started = thread->host_tid != 0;
	}
	// The raiser of a request accepted before the close waits for its
	// acknowledgement, so the thread takes it before it stops for good. A
	// thread whose creation failed never started and has no safe point.
	while (started && (thread->context_state.load(std::memory_order_acquire) & kContextPending) != 0)
	{
		PthreadInterruptibleScope any_syscall;
		Core::Thread::SleepMicro(kRetryMicros);
	}
	std::lock_guard lock(g_context_mutex);
	ReleaseRetryTimer(thread);
}

PthreadInterruptibleScope::PthreadInterruptibleScope()
{
	Enter(0, UINT64_MAX);
}

PthreadInterruptibleScope::PthreadInterruptibleScope(const void* object, size_t size)
{
	const auto begin = reinterpret_cast<uint64_t>(object);
	Enter(begin, begin + size);
}

void PthreadInterruptibleScope::Enter(uint64_t begin, uint64_t end)
{
	m_thread = g_pthread_self;
	if (m_thread == nullptr)
	{
		return;
	}
	m_previous_begin = m_thread->interruptible_begin.load(std::memory_order_relaxed);
	m_previous_end   = m_thread->interruptible_end.load(std::memory_order_relaxed);
	m_thread->interruptible_begin.store(begin, std::memory_order_relaxed);
	m_thread->interruptible_end.store(end, std::memory_order_relaxed);
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

PthreadInterruptibleScope::~PthreadInterruptibleScope()
{
	if (m_thread == nullptr)
	{
		return;
	}
	std::atomic_signal_fence(std::memory_order_seq_cst);
	m_thread->interruptible_begin.store(m_previous_begin, std::memory_order_relaxed);
	m_thread->interruptible_end.store(m_previous_end, std::memory_order_relaxed);
}

void PthreadSnapshotContext(const PthreadPrivate* thread, PthreadThreadDiagnostic* snapshot)
{
	const uint32_t state           = thread->context_state.load(std::memory_order_acquire);
	snapshot->context_pending      = (state & kContextPending) != 0;
	snapshot->context_running      = (state & kContextRunning) != 0;
	snapshot->context_deferred_rip = thread->context_deferred_rip.load(std::memory_order_relaxed);
}

void PthreadInterruptibleWait(Core::CondVar* cond_var, Core::Mutex* mutex)
{
	PthreadInterruptibleScope scope(cond_var->NativeObject(), cond_var->NativeObjectSize());
	cond_var->Wait(mutex);
}

bool PthreadInterruptibleWaitFor(Core::CondVar* cond_var, Core::Mutex* mutex, uint32_t micros)
{
	PthreadInterruptibleScope scope(cond_var->NativeObject(), cond_var->NativeObjectSize());
	return cond_var->WaitFor(mutex, micros);
}

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED
