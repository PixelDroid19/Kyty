#include "Emulator/Kernel/Pthread.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <cstddef>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace Posix {

LIB_NAME("Posix", "libkernel");

static Kernel::KernelUseconds abstime_remaining_usec(const Kernel::KernelTimespec* abstime)
{
	Kernel::KernelTimespec now {};
	if (Kernel::KernelClockGettime(0, &now) != OK || abstime == nullptr)
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
	return static_cast<Kernel::KernelUseconds>(delta);
}

int KYTY_SYSV_ABI pthread_create(Kernel::Pthread* thread, const Kernel::PthreadAttr* attr, Kernel::pthread_entry_func_t entry,
                                 void* arg)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCreate(thread, attr, entry, arg, ""));
}

int KYTY_SYSV_ABI pthread_create_name_np(Kernel::Pthread* thread, const Kernel::PthreadAttr* attr,
                                         Kernel::pthread_entry_func_t entry, void* arg, const char* name)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCreate(thread, attr, entry, arg, name != nullptr ? name : ""));
}

int KYTY_SYSV_ABI pthread_equal(Kernel::Pthread thread1, Kernel::Pthread thread2)
{
	return Kernel::PthreadEqual(thread1, thread2);
}

int KYTY_SYSV_ABI pthread_setcancelstate(int state, int* old_state)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadSetcancelstate(state, old_state));
}

int KYTY_SYSV_ABI pthread_setcanceltype(int type, int* old_type)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadSetcanceltype(type, old_type));
}

int KYTY_SYSV_ABI pthread_setprio(Kernel::Pthread thread, int prio)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadSetprio(thread, prio));
}

int KYTY_SYSV_ABI pthread_join(Kernel::Pthread thread, void** value)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadJoin(thread, value));
}

int KYTY_SYSV_ABI pthread_detach(Kernel::Pthread thread)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadDetach(thread));
}

void KYTY_SYSV_ABI pthread_exit(void* value)
{
	PRINT_NAME();

	Kernel::PthreadExit(value);
}

void KYTY_SYSV_ABI pthread_yield()
{
	PRINT_NAME();

	Kernel::PthreadYield();
}

int KYTY_SYSV_ABI sched_yield()
{
	PRINT_NAME();
	Kernel::PthreadYield();
	return 0;
}

int KYTY_SYSV_ABI pthread_cond_init(Kernel::PthreadCond* cond, const Kernel::PthreadCondattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCondInit(cond, attr, nullptr));
}

int KYTY_SYSV_ABI pthread_cond_destroy(Kernel::PthreadCond* cond)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCondDestroy(cond));
}

int KYTY_SYSV_ABI pthread_cond_signal(Kernel::PthreadCond* cond)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCondSignal(cond));
}

int KYTY_SYSV_ABI pthread_cond_broadcast(Kernel::PthreadCond* cond)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCondBroadcast(cond));
}

int KYTY_SYSV_ABI pthread_cond_wait(Kernel::PthreadCond* cond, Kernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCondWait(cond, mutex));
}

int KYTY_SYSV_ABI pthread_cond_timedwait(Kernel::PthreadCond* cond, Kernel::PthreadMutex* mutex,
                                         const Kernel::KernelTimespec* abstime)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCondTimedwaitAbsolute(cond, mutex, abstime));
}

int KYTY_SYSV_ABI pthread_condattr_init(Kernel::PthreadCondattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCondattrInit(attr));
}

int KYTY_SYSV_ABI pthread_condattr_destroy(Kernel::PthreadCondattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCondattrDestroy(attr));
}

int KYTY_SYSV_ABI pthread_condattr_setclock(Kernel::PthreadCondattr* attr, Kernel::KernelClockid clock_id)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadCondattrSetclock(attr, clock_id));
}

int KYTY_SYSV_ABI pthread_once(int* once_control, void (*init_routine)(void))
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadOnce(once_control, init_routine));
}

int KYTY_SYSV_ABI pthread_mutex_lock(Kernel::PthreadMutex* mutex)
{
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadMutexLock(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_trylock(Kernel::PthreadMutex* mutex)
{
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadMutexTrylock(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_unlock(Kernel::PthreadMutex* mutex)
{
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadMutexUnlock(mutex));
}

int KYTY_SYSV_ABI pthread_rwlock_destroy(Kernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockDestroy(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_init(Kernel::PthreadRwlock* rwlock, const Kernel::PthreadRwlockattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockInit(rwlock, attr, ""));
}

int KYTY_SYSV_ABI pthread_rwlock_rdlock(Kernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockRdlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_timedrdlock(Kernel::PthreadRwlock* rwlock, const Kernel::KernelTimespec* abstime)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockTimedrdlock(rwlock, abstime_remaining_usec(abstime)));
}

int KYTY_SYSV_ABI pthread_rwlock_timedwrlock(Kernel::PthreadRwlock* rwlock, const Kernel::KernelTimespec* abstime)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockTimedwrlock(rwlock, abstime_remaining_usec(abstime)));
}

int KYTY_SYSV_ABI pthread_rwlock_tryrdlock(Kernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockTryrdlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_trywrlock(Kernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockTrywrlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_unlock(Kernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockUnlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_wrlock(Kernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockWrlock(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlockattr_destroy(Kernel::PthreadRwlockattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockattrDestroy(attr));
}

int KYTY_SYSV_ABI pthread_rwlockattr_getpshared(const Kernel::PthreadRwlockattr* attr, int* pshared)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockattrGetpshared(attr, pshared));
}

int KYTY_SYSV_ABI pthread_rwlockattr_gettype_np(Kernel::PthreadRwlockattr* attr, int* type)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockattrGettype(attr, type));
}

int KYTY_SYSV_ABI pthread_rwlockattr_init(Kernel::PthreadRwlockattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockattrInit(attr));
}

int KYTY_SYSV_ABI pthread_rwlockattr_setpshared(Kernel::PthreadRwlockattr* attr, int pshared)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockattrSetpshared(attr, pshared));
}

int KYTY_SYSV_ABI pthread_rwlockattr_settype_np(Kernel::PthreadRwlockattr* attr, int type)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadRwlockattrSettype(attr, type));
}

int KYTY_SYSV_ABI pthread_key_create(Kernel::PthreadKey* key, Kernel::pthread_key_destructor_func_t destructor)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadKeyCreate(key, destructor));
}

int KYTY_SYSV_ABI pthread_key_delete(Kernel::PthreadKey key)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadKeyDelete(key));
}

int KYTY_SYSV_ABI pthread_setspecific(Kernel::PthreadKey key, void* value)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadSetspecific(key, value));
}

void* KYTY_SYSV_ABI pthread_getspecific(Kernel::PthreadKey key)
{
	PRINT_NAME();

	return (Kernel::PthreadGetspecific(key));
}

int KYTY_SYSV_ABI pthread_mutex_destroy(Kernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadMutexDestroy(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_init(Kernel::PthreadMutex* mutex, const Kernel::PthreadMutexattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadMutexInit(mutex, attr, nullptr));
}

int KYTY_SYSV_ABI pthread_mutexattr_init(Kernel::PthreadMutexattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadMutexattrInit(attr));
}

int KYTY_SYSV_ABI pthread_mutexattr_settype(Kernel::PthreadMutexattr* attr, int type)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadMutexattrSettype(attr, type));
}

int KYTY_SYSV_ABI pthread_mutexattr_destroy(Kernel::PthreadMutexattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(Kernel::PthreadMutexattrDestroy(attr));
}

// Gen5 Posix_v1 pthread_attr_* NIDs.
int KYTY_SYSV_ABI pthread_attr_init(Kernel::PthreadAttr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrInit(attr));
}

int KYTY_SYSV_ABI pthread_attr_destroy(Kernel::PthreadAttr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrDestroy(attr));
}

int KYTY_SYSV_ABI pthread_attr_getstack(const Kernel::PthreadAttr* attr, void** stack_addr, size_t* stack_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrGetstack(attr, stack_addr, stack_size));
}

int KYTY_SYSV_ABI pthread_attr_setstacksize(Kernel::PthreadAttr* attr, size_t stack_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrSetstacksize(attr, stack_size));
}

int KYTY_SYSV_ABI pthread_attr_getstacksize(const Kernel::PthreadAttr* attr, size_t* stack_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrGetstacksize(attr, stack_size));
}

int KYTY_SYSV_ABI pthread_attr_get_np(Kernel::Pthread thread, Kernel::PthreadAttr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrGet(thread, attr));
}

int KYTY_SYSV_ABI pthread_attr_getschedpolicy(const Kernel::PthreadAttr* attr, int* policy)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrGetschedpolicy(attr, policy));
}

int KYTY_SYSV_ABI pthread_attr_setschedpolicy(Kernel::PthreadAttr* attr, int policy)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrSetschedpolicy(attr, policy));
}

int KYTY_SYSV_ABI pthread_attr_setdetachstate(Kernel::PthreadAttr* attr, int state)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrSetdetachstate(attr, state));
}

int KYTY_SYSV_ABI pthread_attr_getdetachstate(const Kernel::PthreadAttr* attr, int* state)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrGetdetachstate(attr, state));
}

int KYTY_SYSV_ABI pthread_attr_setschedparam(Kernel::PthreadAttr* attr, const Kernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrSetschedparam(attr, param));
}

int KYTY_SYSV_ABI pthread_attr_getschedparam(const Kernel::PthreadAttr* attr, Kernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrGetschedparam(attr, param));
}

int KYTY_SYSV_ABI pthread_attr_setinheritsched(Kernel::PthreadAttr* attr, int inherit_sched)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrSetinheritsched(attr, inherit_sched));
}

int KYTY_SYSV_ABI pthread_attr_setguardsize(Kernel::PthreadAttr* attr, size_t guard_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrSetguardsize(attr, guard_size));
}

int KYTY_SYSV_ABI pthread_attr_getguardsize(const Kernel::PthreadAttr* attr, size_t* guard_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadAttrGetguardsize(attr, guard_size));
}

int KYTY_SYSV_ABI pthread_getschedparam(Kernel::Pthread thread, int* policy, Kernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadGetschedparam(thread, policy, param));
}

int KYTY_SYSV_ABI pthread_setschedparam(Kernel::Pthread thread, int policy, const Kernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadSetschedparam(thread, policy, param));
}

int KYTY_SYSV_ABI pthread_rename_np(Kernel::Pthread thread, const char* name)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadRename(thread, name));
}

int KYTY_SYSV_ABI pthread_getthreadid_np()
{
	PRINT_NAME();
	return Kernel::PthreadGetthreadid();
}

int KYTY_SYSV_ABI pthread_mutexattr_setprotocol(Kernel::PthreadMutexattr* attr, int protocol)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(Kernel::PthreadMutexattrSetprotocol(attr, protocol));
}

} // namespace Posix

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
