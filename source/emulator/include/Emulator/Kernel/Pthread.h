#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Time.h"

// IWYU pragma: no_include <pthread.h>

#ifdef KYTY_EMU_ENABLED

// MinGW's winpthread header defines this POSIX helper as a macro. Kyty
// exposes the guest ABI symbol with the same spelling, so keep the macro from
// rewriting the declaration below.
#ifdef pthread_attr_getguardsize
#undef pthread_attr_getguardsize
#endif
#ifdef pthread_attr_setguardsize
#undef pthread_attr_setguardsize
#endif

extern "C" {
struct sched_param;
}

namespace Kyty::Kernel {

class PthreadSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<PthreadSubsystem>::Instance(); }
	const char*       Id() override { return "Pthread"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


struct PthreadAttrPrivate;
struct PthreadPrivate;
struct PthreadMutexPrivate;
struct PthreadMutexattrPrivate;
struct PthreadRwlockPrivate;
struct PthreadRwlockattrPrivate;
struct PthreadCondattrPrivate;
struct PthreadCondPrivate;

using PthreadAttr       = PthreadAttrPrivate*;
using Pthread           = PthreadPrivate*;
using KernelCpumask     = uint64_t;
using PthreadMutex      = PthreadMutexPrivate*;
using PthreadMutexattr  = PthreadMutexattrPrivate*;
using KernelSchedParam  = struct sched_param;
using PthreadRwlock     = PthreadRwlockPrivate*;
using PthreadRwlockattr = PthreadRwlockattrPrivate*;
using KernelUseconds    = unsigned int;
using PthreadCondattr   = PthreadCondattrPrivate*;
using PthreadCond       = PthreadCondPrivate*;
using PthreadKey        = int;

using pthread_entry_func_t          = KYTY_SYSV_ABI void* (*)(void*);
using thread_dtors_func_t           = KYTY_SYSV_ABI void (*)();
using host_thread_dtors_func_t      = void (*)(void* guest_stack_top);
using pthread_key_destructor_func_t = KYTY_SYSV_ABI void (*)(void*);

void PthreadInitSelfForMainThread();
void* PthreadCreateMainGuestStack();
void PthreadDeleteStaticObjects(const void* program);
[[nodiscard]] bool PthreadIsInitialized();
bool PthreadQueryStack(const void* addr, void** start, void** end);

int KYTY_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* attr, int type);
int KYTY_SYSV_ABI PthreadMutexattrSetprotocol(PthreadMutexattr* attr, int protocol);
int KYTY_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name);
int KYTY_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex);
bool KYTY_SYSV_ABI PthreadMutexCurrentOwns(PthreadMutex* mutex);

Pthread KYTY_SYSV_ABI PthreadSelf();
int KYTY_SYSV_ABI     PthreadSignal(Pthread thread, int signum);
int KYTY_SYSV_ABI     PthreadSignalWithValue(Pthread thread, int signum, void* value);
int KYTY_SYSV_ABI     PthreadCreate(Pthread* thread, const PthreadAttr* attr, pthread_entry_func_t entry, void* arg, const char* name);
int KYTY_SYSV_ABI     PthreadDetach(Pthread thread);
int KYTY_SYSV_ABI     PthreadJoin(Pthread thread, void** value);
int KYTY_SYSV_ABI     PthreadCancel(Pthread thread);
int KYTY_SYSV_ABI     PthreadSetcancelstate(int state, int* old_state);
int KYTY_SYSV_ABI     PthreadSetcanceltype(int type, int* old_type);
int KYTY_SYSV_ABI     PthreadGetprio(Pthread thread, int* prio);
int KYTY_SYSV_ABI     PthreadSetprio(Pthread thread, int prio);
void KYTY_SYSV_ABI    PthreadTestcancel();
int KYTY_SYSV_ABI     PthreadSetaffinity(Pthread thread, KernelCpumask mask);
int KYTY_SYSV_ABI     PthreadGetaffinity(Pthread thread, KernelCpumask* mask);
int KYTY_SYSV_ABI     PthreadGetschedparam(Pthread thread, int* policy, KernelSchedParam* param);
int KYTY_SYSV_ABI     PthreadSetschedparam(Pthread thread, int policy, const KernelSchedParam* param);
int KYTY_SYSV_ABI     PthreadRename(Pthread thread, const char* name);
void KYTY_SYSV_ABI    PthreadExit(void* value);
int KYTY_SYSV_ABI     PthreadEqual(Pthread thread1, Pthread thread2);
int KYTY_SYSV_ABI     PthreadGetname(Pthread thread, char* name);
void KYTY_SYSV_ABI    PthreadYield();
int KYTY_SYSV_ABI     PthreadGetthreadid();

int KYTY_SYSV_ABI          KernelUsleep(KernelUseconds microseconds);
unsigned int KYTY_SYSV_ABI KernelSleep(unsigned int seconds);
int KYTY_SYSV_ABI          KernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp);

int KYTY_SYSV_ABI   PthreadKeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor);
int KYTY_SYSV_ABI   PthreadKeyDelete(PthreadKey key);
int KYTY_SYSV_ABI   PthreadSetspecific(PthreadKey key, void* value);
void* KYTY_SYSV_ABI PthreadGetspecific(PthreadKey key);

void KYTY_SYSV_ABI KernelSetThreadDtors(thread_dtors_func_t dtors);
void              PthreadSetHostThreadDtors(host_thread_dtors_func_t dtors);

int KYTY_SYSV_ABI PthreadAttrInit(PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrDestroy(PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrGet(Pthread thread, PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask);
int KYTY_SYSV_ABI PthreadAttrGetdetachstate(const PthreadAttr* attr, int* state);
int KYTY_SYSV_ABI PthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guard_size);
int KYTY_SYSV_ABI PthreadAttrGetinheritsched(const PthreadAttr* attr, int* inherit_sched);
int KYTY_SYSV_ABI PthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param);
int KYTY_SYSV_ABI PthreadAttrGetschedpolicy(const PthreadAttr* attr, int* policy);
int KYTY_SYSV_ABI PthreadAttrGetstack(const PthreadAttr* __restrict attr, void** __restrict stack_addr, size_t* __restrict stack_size);
int KYTY_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* attr, void** stack_addr);
int KYTY_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size);
int KYTY_SYSV_ABI PthreadAttrSetaffinity(PthreadAttr* attr, KernelCpumask mask);
int KYTY_SYSV_ABI PthreadAttrSetdetachstate(PthreadAttr* attr, int state);
int KYTY_SYSV_ABI PthreadAttrSetguardsize(PthreadAttr* attr, size_t guard_size);
int KYTY_SYSV_ABI PthreadAttrSetinheritsched(PthreadAttr* attr, int inherit_sched);
int KYTY_SYSV_ABI PthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param);
int KYTY_SYSV_ABI PthreadAttrSetschedpolicy(PthreadAttr* attr, int policy);
int KYTY_SYSV_ABI PthreadAttrSetstack(PthreadAttr* attr, void* addr, size_t size);
int KYTY_SYSV_ABI PthreadAttrSetstackaddr(PthreadAttr* attr, void* addr);
int KYTY_SYSV_ABI PthreadAttrSetstacksize(PthreadAttr* attr, size_t stack_size);

int KYTY_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* attr, const char* name);
int KYTY_SYSV_ABI PthreadRwlockRdlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockTimedrdlock(PthreadRwlock* rwlock, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadRwlockTimedwrlock(PthreadRwlock* rwlock, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadRwlockTryrdlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockTrywrlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockUnlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockWrlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockattrDestroy(PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockattrInit(PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockattrGetpshared(const PthreadRwlockattr* attr, int* pshared);
int KYTY_SYSV_ABI PthreadRwlockattrGettype(PthreadRwlockattr* attr, int* type);
int KYTY_SYSV_ABI PthreadRwlockattrSetpshared(PthreadRwlockattr* attr, int pshared);
int KYTY_SYSV_ABI PthreadRwlockattrSettype(PthreadRwlockattr* attr, int type);

int KYTY_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr);
int KYTY_SYSV_ABI PthreadCondattrInit(PthreadCondattr* attr);
int KYTY_SYSV_ABI PthreadCondattrSetclock(PthreadCondattr* attr, KernelClockid clock_id);
int KYTY_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondDestroy(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char* name);
int KYTY_SYSV_ABI PthreadCondSignal(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondSignalto(PthreadCond* cond, Pthread thread);
int KYTY_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec);
int KYTY_SYSV_ABI PthreadCondTimedwaitAbsolute(PthreadCond* cond, PthreadMutex* mutex,
                                               const KernelTimespec* abstime);
int KYTY_SYSV_ABI PthreadCondWait(PthreadCond* cond, PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadOnce(int* once_control, void (*init_routine)(void));

struct PthreadCondWaitDiagnostic
{
	uint64_t cond        = 0;
	uint64_t mutex       = 0;
	uint64_t return_addr = 0;
	uint64_t cond_handle = 0;
	uint64_t mutex_handle = 0;
	uint64_t last_signal_return_addr = 0;
	uint32_t signal_count = 0;
};

struct PthreadCondWaitDiagnostics
{
	bool                      enabled       = false;
	uint32_t                  blocked_count = 0;
	uint64_t                  tracked_cond  = 0;
	uint32_t                  tracked_waits = 0;
	uint32_t                  tracked_signals = 0;
	PthreadCondWaitDiagnostic blocked[8] {};
};

// Passive snapshot for opt-in condition-wait diagnostics. It never wakes a
// guest, changes mutex ownership, or creates synchronization objects.
bool PthreadGetCondWaitDiagnostics(PthreadCondWaitDiagnostics* out);

struct PthreadThreadDiagnostic
{
	uint64_t entry      = 0;
	uint64_t argument   = 0;
	int32_t  unique_id  = -1;
	bool     started    = false;
	bool     detached   = false;
	bool     almost_done = false;
	bool     free       = false;
};

struct PthreadThreadDiagnostics
{
	bool                    available       = false;
	uint32_t                allocated_count = 0;
	uint32_t                active_count    = 0;
	uint32_t                thread_count    = 0;
	PthreadThreadDiagnostic threads[32] {};
};

// Passive snapshot of guest pthread lifecycle state. It does not create,
// schedule, join, detach, or otherwise change guest threads.
bool PthreadGetThreadDiagnostics(PthreadThreadDiagnostics* out);

// Opt-in KYTY_SLOT_TRACE dump of currently blocked CondWait guests.
void SlotTraceDumpBlockedCondWaiters();

} // namespace Kyty::Kernel

namespace Kyty::Libs::Posix {

int KYTY_SYSV_ABI   getpid();
// Gen5 Posix_v1 pthread_self — NID EotR8a3ASf4.
Kernel::Pthread KYTY_SYSV_ABI pthread_self();
int KYTY_SYSV_ABI   pthread_create(Kernel::Pthread* thread, const Kernel::PthreadAttr* attr, Kernel::pthread_entry_func_t entry,
                                   void* arg);
int KYTY_SYSV_ABI   pthread_create_name_np(Kernel::Pthread* thread, const Kernel::PthreadAttr* attr,
                                           Kernel::pthread_entry_func_t entry, void* arg, const char* name);
int KYTY_SYSV_ABI   pthread_equal(Kernel::Pthread thread1, Kernel::Pthread thread2);
int KYTY_SYSV_ABI   pthread_setcancelstate(int state, int* old_state);
int KYTY_SYSV_ABI   pthread_setcanceltype(int type, int* old_type);
int KYTY_SYSV_ABI   pthread_setprio(Kernel::Pthread thread, int prio);
int KYTY_SYSV_ABI   pthread_join(Kernel::Pthread thread, void** value);
// Gen5 Posix_v1 thread control (+U1R4WtXvoc detach after attr setup).
int KYTY_SYSV_ABI   pthread_detach(Kernel::Pthread thread);
void KYTY_SYSV_ABI  pthread_exit(void* value);
void KYTY_SYSV_ABI  pthread_yield();
int KYTY_SYSV_ABI   sched_yield();
int KYTY_SYSV_ABI   pthread_cond_init(Kernel::PthreadCond* cond, const Kernel::PthreadCondattr* attr);
int KYTY_SYSV_ABI   pthread_cond_destroy(Kernel::PthreadCond* cond);
int KYTY_SYSV_ABI   pthread_cond_signal(Kernel::PthreadCond* cond);
int KYTY_SYSV_ABI   pthread_cond_broadcast(Kernel::PthreadCond* cond);
int KYTY_SYSV_ABI   pthread_cond_wait(Kernel::PthreadCond* cond, Kernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI   pthread_cond_timedwait(Kernel::PthreadCond* cond, Kernel::PthreadMutex* mutex,
                                           const Kernel::KernelTimespec* abstime);
int KYTY_SYSV_ABI   pthread_condattr_init(Kernel::PthreadCondattr* attr);
int KYTY_SYSV_ABI   pthread_condattr_destroy(Kernel::PthreadCondattr* attr);
int KYTY_SYSV_ABI   pthread_condattr_setclock(Kernel::PthreadCondattr* attr, Kernel::KernelClockid clock_id);
int KYTY_SYSV_ABI   pthread_once(int* once_control, void (*init_routine)(void));
int KYTY_SYSV_ABI   pthread_mutex_lock(Kernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI   pthread_mutex_trylock(Kernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI   pthread_mutex_unlock(Kernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI   pthread_rwlock_destroy(Kernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI   pthread_rwlock_init(Kernel::PthreadRwlock* rwlock, const Kernel::PthreadRwlockattr* attr);
int KYTY_SYSV_ABI   pthread_rwlock_rdlock(Kernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI   pthread_rwlock_timedrdlock(Kernel::PthreadRwlock* rwlock, const Kernel::KernelTimespec* abstime);
int KYTY_SYSV_ABI   pthread_rwlock_timedwrlock(Kernel::PthreadRwlock* rwlock, const Kernel::KernelTimespec* abstime);
int KYTY_SYSV_ABI   pthread_rwlock_tryrdlock(Kernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI   pthread_rwlock_trywrlock(Kernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI   pthread_rwlock_unlock(Kernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI   pthread_rwlock_wrlock(Kernel::PthreadRwlock* rwlock);
int KYTY_SYSV_ABI   pthread_rwlockattr_destroy(Kernel::PthreadRwlockattr* attr);
int KYTY_SYSV_ABI   pthread_rwlockattr_getpshared(const Kernel::PthreadRwlockattr* attr, int* pshared);
int KYTY_SYSV_ABI   pthread_rwlockattr_gettype_np(Kernel::PthreadRwlockattr* attr, int* type);
int KYTY_SYSV_ABI   pthread_rwlockattr_init(Kernel::PthreadRwlockattr* attr);
int KYTY_SYSV_ABI   pthread_rwlockattr_setpshared(Kernel::PthreadRwlockattr* attr, int pshared);
int KYTY_SYSV_ABI   pthread_rwlockattr_settype_np(Kernel::PthreadRwlockattr* attr, int type);
int KYTY_SYSV_ABI   pthread_key_create(Kernel::PthreadKey* key, Kernel::pthread_key_destructor_func_t destructor);
int KYTY_SYSV_ABI   pthread_key_delete(Kernel::PthreadKey key);
int KYTY_SYSV_ABI   pthread_setspecific(Kernel::PthreadKey key, void* value);
void* KYTY_SYSV_ABI pthread_getspecific(Kernel::PthreadKey key);
int KYTY_SYSV_ABI   pthread_mutex_destroy(Kernel::PthreadMutex* mutex);
int KYTY_SYSV_ABI   pthread_mutex_init(Kernel::PthreadMutex* mutex, const Kernel::PthreadMutexattr* attr);
int KYTY_SYSV_ABI   pthread_mutexattr_init(Kernel::PthreadMutexattr* attr);
int KYTY_SYSV_ABI   pthread_mutexattr_settype(Kernel::PthreadMutexattr* attr, int type);
int KYTY_SYSV_ABI   pthread_mutexattr_destroy(Kernel::PthreadMutexattr* attr);
// Gen5 Posix_v1 pthread_attr_*.
int KYTY_SYSV_ABI pthread_attr_init(Kernel::PthreadAttr* attr);
int KYTY_SYSV_ABI pthread_attr_destroy(Kernel::PthreadAttr* attr);
int KYTY_SYSV_ABI pthread_attr_getstack(const Kernel::PthreadAttr* attr, void** stack_addr, size_t* stack_size);
int KYTY_SYSV_ABI pthread_attr_setstacksize(Kernel::PthreadAttr* attr, size_t stack_size);
int KYTY_SYSV_ABI pthread_attr_getstacksize(const Kernel::PthreadAttr* attr, size_t* stack_size);
int KYTY_SYSV_ABI pthread_attr_get_np(Kernel::Pthread thread, Kernel::PthreadAttr* attr);
int KYTY_SYSV_ABI pthread_attr_getschedpolicy(const Kernel::PthreadAttr* attr, int* policy);
int KYTY_SYSV_ABI pthread_attr_setschedpolicy(Kernel::PthreadAttr* attr, int policy);
int KYTY_SYSV_ABI pthread_attr_setdetachstate(Kernel::PthreadAttr* attr, int state);
int KYTY_SYSV_ABI pthread_attr_getdetachstate(const Kernel::PthreadAttr* attr, int* state);
int KYTY_SYSV_ABI pthread_attr_setschedparam(Kernel::PthreadAttr* attr, const Kernel::KernelSchedParam* param);
int KYTY_SYSV_ABI pthread_attr_getschedparam(const Kernel::PthreadAttr* attr, Kernel::KernelSchedParam* param);
int KYTY_SYSV_ABI pthread_attr_setinheritsched(Kernel::PthreadAttr* attr, int inherit_sched);
int KYTY_SYSV_ABI pthread_attr_setguardsize(Kernel::PthreadAttr* attr, size_t guard_size);
int KYTY_SYSV_ABI pthread_attr_getguardsize(const Kernel::PthreadAttr* attr, size_t* guard_size);
int KYTY_SYSV_ABI pthread_getschedparam(Kernel::Pthread thread, int* policy, Kernel::KernelSchedParam* param);
int KYTY_SYSV_ABI pthread_setschedparam(Kernel::Pthread thread, int policy, const Kernel::KernelSchedParam* param);
int KYTY_SYSV_ABI pthread_rename_np(Kernel::Pthread thread, const char* name);
int KYTY_SYSV_ABI pthread_getthreadid_np();
int KYTY_SYSV_ABI pthread_mutexattr_setprotocol(Kernel::PthreadMutexattr* attr, int protocol);

Kernel::Pthread KYTY_SYSV_ABI pthread_self();
int KYTY_SYSV_ABI                  pthread_detach(Kernel::Pthread thread);
void KYTY_SYSV_ABI                 pthread_exit(void* value);
void KYTY_SYSV_ABI                 pthread_yield();
int KYTY_SYSV_ABI                  pthread_cond_signal(Kernel::PthreadCond* cond);
int KYTY_SYSV_ABI                  pthread_attr_init(Kernel::PthreadAttr* attr);
int KYTY_SYSV_ABI                  pthread_attr_destroy(Kernel::PthreadAttr* attr);
int KYTY_SYSV_ABI                  pthread_attr_getstack(const Kernel::PthreadAttr* attr, void** stack_addr, size_t* stack_size);
int KYTY_SYSV_ABI                  pthread_attr_setstacksize(Kernel::PthreadAttr* attr, size_t stack_size);
int KYTY_SYSV_ABI                  pthread_attr_getstacksize(const Kernel::PthreadAttr* attr, size_t* stack_size);
int KYTY_SYSV_ABI                  pthread_attr_get_np(Kernel::Pthread thread, Kernel::PthreadAttr* attr);
int KYTY_SYSV_ABI                  pthread_attr_getschedpolicy(const Kernel::PthreadAttr* attr, int* policy);
int KYTY_SYSV_ABI                  pthread_attr_setschedpolicy(Kernel::PthreadAttr* attr, int policy);
int KYTY_SYSV_ABI                  pthread_attr_setdetachstate(Kernel::PthreadAttr* attr, int state);
int KYTY_SYSV_ABI                  pthread_attr_getdetachstate(const Kernel::PthreadAttr* attr, int* state);
int KYTY_SYSV_ABI                  pthread_attr_setschedparam(Kernel::PthreadAttr* attr, const Kernel::KernelSchedParam* param);
int KYTY_SYSV_ABI                  pthread_attr_getschedparam(const Kernel::PthreadAttr* attr, Kernel::KernelSchedParam* param);
int KYTY_SYSV_ABI                  pthread_attr_setinheritsched(Kernel::PthreadAttr* attr, int inherit_sched);
int KYTY_SYSV_ABI                  pthread_attr_setguardsize(Kernel::PthreadAttr* attr, size_t guard_size);
int KYTY_SYSV_ABI                  pthread_attr_getguardsize(const Kernel::PthreadAttr* attr, size_t* guard_size);

} // namespace Kyty::Libs::Posix

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_ */
