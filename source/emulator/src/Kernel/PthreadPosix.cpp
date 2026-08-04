#include "Emulator/Kernel/Pthread.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <cstddef>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace Posix {

LIB_NAME("Posix", "libkernel");

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

int KYTY_SYSV_ABI pthread_cond_wait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondWait(cond, mutex));
}

int KYTY_SYSV_ABI pthread_cond_timedwait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex,
                                         const LibKernel::KernelTimespec* abstime)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondTimedwaitAbsolute(cond, mutex, abstime));
}

int KYTY_SYSV_ABI pthread_condattr_init(LibKernel::PthreadCondattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondattrInit(attr));
}

int KYTY_SYSV_ABI pthread_condattr_destroy(LibKernel::PthreadCondattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondattrDestroy(attr));
}

int KYTY_SYSV_ABI pthread_condattr_setclock(LibKernel::PthreadCondattr* attr, LibKernel::KernelClockid clock_id)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondattrSetclock(attr, clock_id));
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

#endif // KYTY_EMU_ENABLED
