#include "Emulator/Kernel/Fiber.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Fiber", 1, "Fiber", 1, 1);

namespace Fiber {

namespace {

thread_local FiberObject*    g_current_fiber       = nullptr;
thread_local FiberObject*    g_thread_return_fiber = nullptr;
thread_local FiberObject*    g_starting_fiber      = nullptr;
thread_local FiberObject*    g_pending_idle_fiber  = nullptr;
thread_local FiberCpuContext g_thread_fiber_context {};

std::mutex                                 g_fiber_owner_mutex;
std::unordered_map<FiberObject*, uint64_t> g_fiber_owner_thread;
std::unordered_map<uint64_t, FiberObject*> g_fiber_current_by_thread;

// Guest state field is a plain u32 in the FiberObject layout; access it with
// atomic ops on the same address so Run/Switch races stay defined.
std::atomic<uint32_t>* FiberStateAtomic(FiberObject* fiber)
{
	return reinterpret_cast<std::atomic<uint32_t>*>(&fiber->state);
}

const std::atomic<uint32_t>* FiberStateAtomic(const FiberObject* fiber)
{
	return reinterpret_cast<const std::atomic<uint32_t>*>(&fiber->state);
}

uint32_t FiberLoadState(const FiberObject* fiber)
{
	return FiberStateAtomic(fiber)->load(std::memory_order_acquire);
}

uint64_t FiberCurrentHostThreadId()
{
	return std::hash<std::thread::id> {}(std::this_thread::get_id());
}

void FiberSetOwner(FiberObject* fiber)
{
	std::lock_guard lock(g_fiber_owner_mutex);
	g_fiber_owner_thread[fiber] = FiberCurrentHostThreadId();
}

void FiberClearOwner(FiberObject* fiber)
{
	std::lock_guard lock(g_fiber_owner_mutex);
	g_fiber_owner_thread.erase(fiber);
}

uint64_t FiberGetOwner(FiberObject* fiber)
{
	std::lock_guard lock(g_fiber_owner_mutex);
	const auto      it = g_fiber_owner_thread.find(fiber);
	return it != g_fiber_owner_thread.end() ? it->second : 0;
}

void FiberSetCurrentFiber(FiberObject* fiber)
{
	g_current_fiber = fiber;

	std::lock_guard lock(g_fiber_owner_mutex);
	const auto      thread_id = FiberCurrentHostThreadId();
	if (fiber != nullptr)
	{
		g_fiber_current_by_thread[thread_id] = fiber;
	}
	else
	{
		g_fiber_current_by_thread.erase(thread_id);
	}
}

void FiberStoreState(FiberObject* fiber, uint32_t state)
{
	FiberStateAtomic(fiber)->store(state, std::memory_order_release);
	if (state == FIBER_STATE_RUNNING)
	{
		FiberSetOwner(fiber);
	}
	else
	{
		FiberClearOwner(fiber);
	}
}

void FiberDeferIdle(FiberObject* fiber)
{
	g_pending_idle_fiber = fiber;
}

void FiberCommitDeferredIdle()
{
	auto* fiber          = g_pending_idle_fiber;
	g_pending_idle_fiber = nullptr;
	if (fiber != nullptr)
	{
		FiberStoreState(fiber, FIBER_STATE_IDLE);
	}
}

bool FiberCompareExchangeState(FiberObject* fiber, uint32_t expected, uint32_t desired)
{
	const bool ok =
	    FiberStateAtomic(fiber)->compare_exchange_strong(expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
	if (!ok)
	{
		return false;
	}
	if (desired == FIBER_STATE_RUNNING)
	{
		FiberSetOwner(fiber);
	}
	else
	{
		FiberClearOwner(fiber);
	}
	return true;
}

bool FiberWaitAndEnterRunning(FiberObject* fiber, uint32_t* observed_state)
{
	const auto start = std::chrono::steady_clock::now();
	uint32_t   spin  = 0;
	auto*      ref   = FiberStateAtomic(fiber);

	for (;;)
	{
		uint32_t expected = FIBER_STATE_IDLE;
		if (ref->compare_exchange_strong(expected, FIBER_STATE_RUNNING, std::memory_order_acq_rel, std::memory_order_acquire))
		{
			FiberSetOwner(fiber);
			return true;
		}
		if (observed_state != nullptr)
		{
			*observed_state = expected;
		}
		if (expected != FIBER_STATE_RUNNING)
		{
			return false;
		}
		if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(5))
		{
			return false;
		}
		if (spin++ < 64)
		{
			std::this_thread::yield();
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
	}
}

bool FiberRepairStaleRunningOnThisThread(FiberObject* fiber, uint32_t observed_state)
{
	if (observed_state != FIBER_STATE_RUNNING || fiber == g_current_fiber)
	{
		return false;
	}
	if (FiberGetOwner(fiber) != 0)
	{
		return false;
	}
	return FiberCompareExchangeState(fiber, FIBER_STATE_RUNNING, FIBER_STATE_IDLE);
}

void FiberSetContextValid(FiberObject* fiber, bool valid)
{
	fiber->context_valid = valid;
	fiber->context       = valid ? &fiber->saved_context : nullptr;
}

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((noinline, returns_twice)) int FiberSaveContext(FiberCpuContext* ctx)
{
	int ret = 0;
	asm volatile("movq %[ctx], %%r10\n\t"
	             "movq %%rbx, 0(%%r10)\n\t"
	             "movq %%rbp, 8(%%r10)\n\t"
	             "movq %%rdi, 16(%%r10)\n\t"
	             "movq %%rsi, 24(%%r10)\n\t"
	             "movq %%r12, 32(%%r10)\n\t"
	             "movq %%r13, 40(%%r10)\n\t"
	             "movq %%r14, 48(%%r10)\n\t"
	             "movq %%r15, 56(%%r10)\n\t"
	             "leaq 8(%%rsp), %%r11\n\t"
	             "movq %%r11, 64(%%r10)\n\t"
	             "movq (%%rsp), %%r11\n\t"
	             "movq %%r11, 72(%%r10)\n\t"
	             "xorl %%eax, %%eax\n\t"
	             : "=a"(ret)
	             : [ctx] "r"(ctx)
	             : "memory", "r10", "r11");
	return ret;
}

__attribute__((noreturn, noinline)) void FiberRestoreContext(FiberCpuContext* ctx, uint64_t ret)
{
	asm volatile("movq %[ctx], %%r10\n\t"
	             "movq 72(%%r10), %%r11\n\t"
	             "movq 0(%%r10), %%rbx\n\t"
	             "movq 8(%%r10), %%rbp\n\t"
	             "movq 16(%%r10), %%rdi\n\t"
	             "movq 24(%%r10), %%rsi\n\t"
	             "movq 32(%%r10), %%r12\n\t"
	             "movq 40(%%r10), %%r13\n\t"
	             "movq 48(%%r10), %%r14\n\t"
	             "movq 56(%%r10), %%r15\n\t"
	             "movq 64(%%r10), %%rsp\n\t"
	             "movq %[ret], %%rax\n\t"
	             "jmp *%%r11\n\t"
	             :
	             : [ctx] "r"(ctx), [ret] "r"(ret)
	             : "memory", "rax", "r10", "r11");
	__builtin_unreachable();
}
#else
int FiberSaveContext(FiberCpuContext* ctx)
{
	(void)ctx;
	return 0;
}

void FiberRestoreContext(FiberCpuContext* ctx, uint64_t ret)
{
	(void)ctx;
	(void)ret;
	EXIT("Fiber context switching is only implemented on x86_64\n");
}
#endif

[[noreturn]] void FiberStartTrampoline();

[[noreturn]] void FiberStartOnGuestStack(FiberObject* fiber)
{
	FiberCpuContext ctx {};
	const auto      stack_top = reinterpret_cast<uintptr_t>(fiber->addr_context) + static_cast<uintptr_t>(fiber->size_context);
	auto            rsp       = (stack_top & ~static_cast<uintptr_t>(0x0f));
	rsp -= sizeof(uint64_t);
	*reinterpret_cast<uint64_t*>(rsp) = 0;

	ctx.rsp = rsp;
	ctx.rip = reinterpret_cast<uint64_t>(&FiberStartTrampoline);

	g_starting_fiber = fiber;
	FiberRestoreContext(&ctx, 1);
}

[[noreturn]] void FiberStartTrampoline()
{
	auto* fiber      = g_starting_fiber;
	g_starting_fiber = nullptr;
	EXIT_IF(fiber == nullptr);

	FiberCommitDeferredIdle();
	FiberSetCurrentFiber(fiber);

	fiber->entry(fiber->arg_on_initialize, fiber->arg_on_run);

	FiberStoreState(fiber, FIBER_STATE_TERMINATED);
	FiberSetContextValid(fiber, false);
	fiber->arg_on_return  = 0;
	g_thread_return_fiber = fiber;
	FiberSetCurrentFiber(nullptr);

	FiberRestoreContext(&g_thread_fiber_context, 1);
}

} // namespace

bool FiberObjectIsValid(const FiberObject* fiber)
{
	return fiber != nullptr && fiber->magic_start == FIBER_MAGIC_START && fiber->magic_end == FIBER_MAGIC_END;
}

int32_t FiberValidateInitializeArgs(const FiberObject* fiber, const char* name, FiberEntry entry, void* addr_context,
                                    uint64_t size_context, const FiberOptParam* opt_param)
{
	if (fiber == nullptr || name == nullptr || entry == nullptr)
	{
		return FIBER_ERROR_NULL;
	}
	if ((reinterpret_cast<uint64_t>(fiber) & 7u) != 0 || (reinterpret_cast<uint64_t>(addr_context) & 15u) != 0 ||
	    (opt_param != nullptr && (reinterpret_cast<uint64_t>(opt_param) & 7u) != 0))
	{
		return FIBER_ERROR_ALIGNMENT;
	}
	if (size_context != 0 && size_context < FIBER_CONTEXT_MIN_SIZE)
	{
		return FIBER_ERROR_RANGE;
	}
	if ((size_context & 15u) != 0 || (addr_context == nullptr && size_context != 0) || (addr_context != nullptr && size_context == 0) ||
	    (opt_param != nullptr && opt_param->magic != FIBER_OPT_MAGIC))
	{
		return FIBER_ERROR_INVALID;
	}
	return OK;
}

int32_t KYTY_SYSV_ABI FiberInitialize(FiberObject* fiber, const char* name, FiberEntry entry, uint64_t arg_on_initialize,
                                      void* addr_context, uint64_t size_context, const FiberOptParam* opt_param, uint32_t build_version)
{
	PRINT_NAME();

	const int32_t check = FiberValidateInitializeArgs(fiber, name, entry, addr_context, size_context, opt_param);
	if (check != OK)
	{
		return check;
	}

	std::memset(fiber, 0, sizeof(*fiber));
	std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
	fiber->name[FIBER_MAX_NAME_LENGTH] = '\0';

	fiber->magic_start       = FIBER_MAGIC_START;
	fiber->state             = FIBER_STATE_IDLE;
	fiber->entry             = entry;
	fiber->arg_on_initialize = arg_on_initialize;
	fiber->addr_context      = addr_context;
	fiber->size_context      = size_context;
	fiber->flags             = (build_version >= 0x03500000u ? FIBER_FLAG_SET_FPU_REGS : 0);
	fiber->context_start     = addr_context;
	fiber->context_end =
	    (addr_context != nullptr ? static_cast<uint8_t*>(addr_context) + size_context : nullptr);
	std::memset(&fiber->saved_context, 0, sizeof(fiber->saved_context));
	FiberSetContextValid(fiber, false);
	fiber->magic_end = FIBER_MAGIC_END;

	if (addr_context != nullptr)
	{
		*static_cast<uint64_t*>(addr_context) = FIBER_STACK_MAGIC;
	}

	printf("\t fiber init: %s, entry = 0x%016" PRIx64 ", context = 0x%016" PRIx64 ", size = %" PRIu64 "\n", fiber->name,
	       reinterpret_cast<uint64_t>(entry), reinterpret_cast<uint64_t>(addr_context), size_context);

	// #region agent log
	{
		static int logged = 0;
		if (logged++ < 3)
		{
			if (FILE* f = std::fopen("/home/monasterios/Kyty/.cursor/debug-f08e58.log", "a"))
			{
				std::fprintf(f,
				             "{\"sessionId\":\"f08e58\",\"runId\":\"post-fix\",\"hypothesisId\":\"K\","
				             "\"location\":\"Fiber.cpp:FiberInitialize\",\"message\":\"fiber init\","
				             "\"data\":{\"name\":\"%s\",\"entry\":%llu,\"context\":%llu,\"size\":%llu},"
				             "\"timestamp\":%lld}\n",
				             fiber->name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(entry)),
				             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr_context)),
				             static_cast<unsigned long long>(size_context),
				             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
				                                            std::chrono::system_clock::now().time_since_epoch())
				                                        .count()));
				std::fclose(f);
			}
		}
	}
	// #endregion

	return OK;
}

int32_t KYTY_SYSV_ABI FiberInitializeInternal(FiberObject* fiber, const char* name, FiberEntry entry, uint64_t arg_on_initialize,
                                              void* addr_context, uint64_t size_context, const FiberOptParam* opt_param, uint32_t flags,
                                              uint32_t build_version)
{
	const auto ret =
	    FiberInitialize(fiber, name, entry, arg_on_initialize, addr_context, size_context, opt_param, build_version);
	if (ret == OK && fiber != nullptr)
	{
		fiber->flags |= flags;
	}
	return ret;
}

int32_t KYTY_SYSV_ABI FiberOptParamInitialize(FiberOptParam* opt_param)
{
	PRINT_NAME();

	if (opt_param == nullptr)
	{
		return FIBER_ERROR_NULL;
	}
	if ((reinterpret_cast<uint64_t>(opt_param) & 7u) != 0)
	{
		return FIBER_ERROR_ALIGNMENT;
	}

	std::memset(opt_param, 0, 128);
	opt_param->magic = FIBER_OPT_MAGIC;
	return OK;
}

int32_t KYTY_SYSV_ABI FiberFinalize(FiberObject* fiber)
{
	PRINT_NAME();

	if (fiber == nullptr)
	{
		return FIBER_ERROR_NULL;
	}
	if (!FiberObjectIsValid(fiber))
	{
		return FIBER_ERROR_INVALID;
	}
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_TERMINATED))
	{
		return FIBER_ERROR_STATE;
	}
	return OK;
}

int32_t KYTY_SYSV_ABI FiberRun(FiberObject* fiber, uint64_t arg_on_run, uint64_t* arg_on_return)
{
	PRINT_NAME();

	if (!FiberObjectIsValid(fiber))
	{
		return FIBER_ERROR_INVALID;
	}
	if (g_current_fiber != nullptr)
	{
		return FIBER_ERROR_PERMISSION;
	}
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING))
	{
		return FIBER_ERROR_STATE;
	}

	fiber->arg_on_run     = arg_on_run;
	fiber->arg_on_return  = 0;
	g_thread_return_fiber = nullptr;

	if (FiberSaveContext(&g_thread_fiber_context) == 0)
	{
		if (fiber->context_valid)
		{
			FiberRestoreContext(&fiber->saved_context, 1);
		}
		FiberStartOnGuestStack(fiber);
	}

	FiberCommitDeferredIdle();
	FiberSetCurrentFiber(nullptr);
	auto* returned_fiber  = (g_thread_return_fiber != nullptr ? g_thread_return_fiber : fiber);
	g_thread_return_fiber = nullptr;

	if (arg_on_return != nullptr)
	{
		*arg_on_return = returned_fiber->arg_on_return;
	}

	return (FiberLoadState(returned_fiber) == FIBER_STATE_TERMINATED ? FIBER_ERROR_STATE : OK);
}

int32_t KYTY_SYSV_ABI FiberSwitch(FiberObject* fiber, uint64_t arg_on_run, uint64_t* arg_on_return)
{
	PRINT_NAME();

	if (!FiberObjectIsValid(fiber))
	{
		return FIBER_ERROR_INVALID;
	}
	if (g_current_fiber == nullptr)
	{
		return FIBER_ERROR_PERMISSION;
	}

	for (;;)
	{
		uint32_t observed_state = 0;
		if (FiberWaitAndEnterRunning(fiber, &observed_state))
		{
			break;
		}
		if (FiberRepairStaleRunningOnThisThread(fiber, observed_state))
		{
			continue;
		}
		return FIBER_ERROR_STATE;
	}

	auto* caller = g_current_fiber;

	fiber->arg_on_run    = arg_on_run;
	fiber->arg_on_return = 0;

	if (FiberSaveContext(&caller->saved_context) == 0)
	{
		FiberSetContextValid(caller, true);
		FiberDeferIdle(caller);
		if (fiber->context_valid)
		{
			FiberRestoreContext(&fiber->saved_context, 1);
		}
		FiberStartOnGuestStack(fiber);
	}

	FiberCommitDeferredIdle();
	FiberSetCurrentFiber(caller);
	FiberStoreState(caller, FIBER_STATE_RUNNING);

	if (arg_on_return != nullptr)
	{
		*arg_on_return = caller->arg_on_run;
	}
	return OK;
}

int32_t KYTY_SYSV_ABI FiberGetSelf(FiberObject** fiber)
{
	PRINT_NAME();

	if (fiber == nullptr)
	{
		return FIBER_ERROR_NULL;
	}
	*fiber = g_current_fiber;
	return OK;
}

int32_t KYTY_SYSV_ABI FiberReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run)
{
	PRINT_NAME();

	if (g_current_fiber == nullptr)
	{
		return FIBER_ERROR_PERMISSION;
	}

	auto* fiber          = g_current_fiber;
	fiber->arg_on_return = arg_on_return;

	if (FiberSaveContext(&fiber->saved_context) == 0)
	{
		FiberSetContextValid(fiber, true);
		g_thread_return_fiber = fiber;
		FiberDeferIdle(fiber);
		FiberRestoreContext(&g_thread_fiber_context, 1);
	}

	FiberCommitDeferredIdle();
	FiberSetCurrentFiber(fiber);
	FiberStoreState(fiber, FIBER_STATE_RUNNING);
	if (arg_on_run != nullptr)
	{
		*arg_on_run = fiber->arg_on_run;
	}
	return OK;
}

int32_t KYTY_SYSV_ABI FiberGetInfo(FiberObject* fiber, FiberInfo* fiber_info)
{
	PRINT_NAME();

	if (fiber == nullptr || fiber_info == nullptr)
	{
		return FIBER_ERROR_NULL;
	}
	if (!FiberObjectIsValid(fiber))
	{
		return FIBER_ERROR_INVALID;
	}

	std::memset(fiber_info, 0, sizeof(*fiber_info));
	fiber_info->size              = sizeof(*fiber_info);
	fiber_info->entry             = fiber->entry;
	fiber_info->arg_on_initialize = fiber->arg_on_initialize;
	fiber_info->addr_context      = fiber->addr_context;
	fiber_info->size_context      = fiber->size_context;
	std::strncpy(fiber_info->name, fiber->name, FIBER_MAX_NAME_LENGTH);
	return OK;
}

int32_t KYTY_SYSV_ABI FiberStartContextSizeCheck(uint32_t flags)
{
	PRINT_NAME();
	printf("\t flags = 0x%08" PRIx32 "\n", flags);
	return OK;
}

int32_t KYTY_SYSV_ABI FiberStopContextSizeCheck()
{
	PRINT_NAME();
	return OK;
}

int32_t KYTY_SYSV_ABI FiberRename(FiberObject* fiber, const char* name)
{
	PRINT_NAME();

	if (fiber == nullptr || name == nullptr)
	{
		return FIBER_ERROR_NULL;
	}
	if (!FiberObjectIsValid(fiber))
	{
		return FIBER_ERROR_INVALID;
	}

	std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
	fiber->name[FIBER_MAX_NAME_LENGTH] = '\0';
	return OK;
}

int32_t KYTY_SYSV_ABI FiberGetThreadFramePointerAddress(uint64_t* addr_frame_pointer)
{
	PRINT_NAME();

	if (addr_frame_pointer == nullptr)
	{
		return FIBER_ERROR_NULL;
	}
	*addr_frame_pointer = 0;
	return OK;
}

} // namespace Fiber

LIB_DEFINE(InitFiber_1)
{
	LIB_FUNC("hVYD7Ou2pCQ", Fiber::FiberInitialize);
	LIB_FUNC("7+OJIpko9RY", Fiber::FiberInitializeInternal);
	LIB_FUNC("asjUJJ+aa8s", Fiber::FiberOptParamInitialize);
	LIB_FUNC("JeNX5F-NzQU", Fiber::FiberFinalize);
	LIB_FUNC("a0LLrZWac0M", Fiber::FiberRun);
	LIB_FUNC("PFT2S-tJ7Uk", Fiber::FiberSwitch);
	LIB_FUNC("p+zLIOg27zU", Fiber::FiberGetSelf);
	LIB_FUNC("B0ZX2hx9DMw", Fiber::FiberReturnToThread);
	// AttachContext aliases used by some titles map to Run/Switch.
	LIB_FUNC("avfGJ94g36Q", Fiber::FiberRun);
	LIB_FUNC("ZqhZFuzKT6U", Fiber::FiberSwitch);
	LIB_FUNC("uq2Y5BFz0PE", Fiber::FiberGetInfo);
	LIB_FUNC("Lcqty+QNWFc", Fiber::FiberStartContextSizeCheck);
	LIB_FUNC("Kj4nXMpnM8Y", Fiber::FiberStopContextSizeCheck);
	LIB_FUNC("JzyT91ucGDc", Fiber::FiberRename);
	LIB_FUNC("0dy4JtMUcMQ", Fiber::FiberGetThreadFramePointerAddress);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
