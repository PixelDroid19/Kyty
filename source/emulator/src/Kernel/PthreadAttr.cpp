#include "PthreadInternal.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Trace.h"
#include "Emulator/Log.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

KERNEL_LIB_NAME();

void* PthreadCreateMainGuestStack()
{
	EXIT_IF(g_pthread_self == nullptr || g_pthread_self->attr == nullptr);

	auto* attr = g_pthread_self->attr;
	if (attr->stack_addr == nullptr)
	{
		const size_t stack_size = (attr->stack_size >= MAIN_GUEST_STACK_MIN ? attr->stack_size : MAIN_GUEST_STACK_MIN);
		const uint64_t base = Core::VirtualMemory::Alloc(0, stack_size, Core::VirtualMemory::Mode::ReadWrite);
		if (base == 0)
		{
			return nullptr;
		}
		attr->stack_addr = reinterpret_cast<void*>(base);
		attr->stack_size = stack_size;
		g_pthread_self->guest_stack_base = base;
		g_pthread_self->guest_stack_size = stack_size;
	}

	return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(attr->stack_addr) + attr->stack_size) & ~static_cast<uintptr_t>(0xf));
}

static size_t stack_align_up(size_t value, size_t alignment)
{
	return ((value + alignment - 1) / alignment) * alignment;
}

int create_guest_stack(PthreadAttr attr)
{
	if (attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (attr->stack_addr != nullptr)
	{
		attr->stack_owned    = false;
		attr->stack_map_addr = 0;
		attr->stack_map_size = 0;
		return OK;
	}

	const size_t requested_stack_size = (attr->stack_size >= GUEST_PTHREAD_STACK_MIN ? attr->stack_size : GUEST_PTHREAD_STACK_MIN);
	const size_t stack_size           = stack_align_up(requested_stack_size + PTHREAD_STACK_EXTRA, PTHREAD_STACK_PAGE);
	const size_t guard_size           = stack_align_up(attr->guard_size, PTHREAD_STACK_PAGE);
	const size_t map_size             = guard_size + stack_size;
	const auto   map_addr             = Core::VirtualMemory::Alloc(0, map_size, Core::VirtualMemory::Mode::ReadWrite);
	if (map_addr == 0)
	{
		return KERNEL_ERROR_EAGAIN;
	}
	if (guard_size != 0 && !Core::VirtualMemory::Protect(map_addr, guard_size, Core::VirtualMemory::Mode::NoAccess))
	{
		Core::VirtualMemory::Free(map_addr);
		return KERNEL_ERROR_EAGAIN;
	}

	attr->stack_addr     = reinterpret_cast<void*>(map_addr + guard_size);
	attr->stack_size     = stack_size;
	attr->stack_owned    = true;
	attr->stack_map_addr = map_addr;
	attr->stack_map_size = map_size;

	std::memset(attr->stack_addr, 0, stack_size);
	return OK;
}

void free_guest_stack(PthreadAttr attr)
{
	if (attr == nullptr || !attr->stack_owned || attr->stack_map_addr == 0)
	{
		return;
	}

	Core::VirtualMemory::Free(attr->stack_map_addr);
	attr->stack_addr     = nullptr;
	attr->stack_size     = 0;
	attr->stack_owned    = false;
	attr->stack_map_addr = 0;
	attr->stack_map_size = 0;
}

int pthread_attr_copy(PthreadAttr* dst, const PthreadAttr* src)
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

void pthread_attr_dbg_print(const PthreadAttr* src)
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

	KYTY_LOG_DEBUG("\tcpu_mask       = 0x%" PRIx64 "\n", mask);
	KYTY_LOG_DEBUG("\tdetach_state   = %d\n", state);
	KYTY_LOG_DEBUG("\tguard_size     = %" PRIu64 "\n", guard_size);
	KYTY_LOG_DEBUG("\tinherit_sched  = %d\n", inherit_sched);
	KYTY_LOG_DEBUG("\tsched_priority = %d\n", param.sched_priority);
	KYTY_LOG_DEBUG("\tpolicy         = %d\n", policy);
	KYTY_LOG_DEBUG("\tstack_addr     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(stack_addr));
	KYTY_LOG_DEBUG("\tstack_size    = %" PRIu64 "\n", static_cast<uint64_t>(stack_size));
}

int KYTY_SYSV_ABI PthreadAttrInit(PthreadAttr* attr)
{
	PRINT_NAME();

	*attr = new PthreadAttrPrivate {};

	int result = pthread_attr_init(&(*attr)->p);
	if (result == 0)
	{
		result = pthread_attr_getstacksize(&(*attr)->p, &(*attr)->stack_size);
	}

	(*attr)->affinity       = 0x7f;
	(*attr)->guard_size     = 0x1000;
	(*attr)->stack_addr     = nullptr;
	(*attr)->stack_owned    = false;
	(*attr)->stack_map_addr = 0;
	(*attr)->stack_map_size = 0;

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

	if (attr == nullptr || *attr == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	free_guest_stack(*attr);
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

	const int copy_result = pthread_attr_copy(attr, &thread->attr);
	if (copy_result != OK)
	{
		return copy_result;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	// pthread attributes are queried by IL2CPP while registering conservative
	// GC roots.  The creation template keeps a null stack address when Linux
	// chooses the native stack, so expose the active pthread mapping instead.
	// This must agree with the stack pointer captured by KernelRaiseException.
	pthread_attr_t native_attr {};
	if ((*attr)->stack_addr == nullptr && pthread_getattr_np(thread->p, &native_attr) == 0)
	{
		void*  stack_addr = nullptr;
		size_t stack_size = 0;
		const int get_result = pthread_attr_getstack(&native_attr, &stack_addr, &stack_size);
		pthread_attr_destroy(&native_attr);
		if (get_result != 0 || stack_addr == nullptr || stack_size == 0 ||
		    pthread_attr_setstack(&(*attr)->p, stack_addr, stack_size) != 0)
		{
			return KERNEL_ERROR_EINVAL;
		}
		(*attr)->stack_addr = stack_addr;
		(*attr)->stack_size = stack_size;
	}
#endif

	return OK;
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

	*stack_addr = (*attr)->stack_addr;
	*stack_size = (*attr)->stack_size;
	return OK;
}

int KYTY_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* attr, void** stack_addr)
{
	PRINT_NAME();

	if (stack_addr == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*stack_addr = (*attr)->stack_addr;
	return OK;
}

int KYTY_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size)
{
	PRINT_NAME();

	if (stack_size == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*stack_size = (*attr)->stack_size;
	return OK;
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

	if (addr == nullptr || size < GUEST_PTHREAD_STACK_MIN || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	const size_t host_stack_size = (size < HOST_PTHREAD_STACK_MIN ? HOST_PTHREAD_STACK_MIN : size);
	const int    result          = pthread_attr_setstacksize(&(*attr)->p, host_stack_size);

	if (result == 0)
	{
		(*attr)->stack_addr     = addr;
		(*attr)->stack_size     = size;
		(*attr)->stack_owned    = false;
		(*attr)->stack_map_addr = 0;
		(*attr)->stack_map_size = 0;
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

	const size_t stack_size = (*attr)->stack_size;
	if (stack_size == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	const size_t host_stack_size = (stack_size < HOST_PTHREAD_STACK_MIN ? HOST_PTHREAD_STACK_MIN : stack_size);
	const int    result          = pthread_attr_setstacksize(&(*attr)->p, host_stack_size);

	if (result == 0)
	{
		(*attr)->stack_addr     = addr;
		(*attr)->stack_owned    = false;
		(*attr)->stack_map_addr = 0;
		(*attr)->stack_map_size = 0;
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

	const size_t host_stack_size = (stack_size < HOST_PTHREAD_STACK_MIN ? HOST_PTHREAD_STACK_MIN : stack_size);
	const int    result2         = pthread_attr_setstacksize(&(*attr)->p, host_stack_size);

	if (result2 == 0)
	{
		(*attr)->stack_size = stack_size;
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED
