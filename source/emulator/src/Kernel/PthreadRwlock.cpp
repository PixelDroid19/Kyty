#include "PthreadInternal.h"

#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Trace.h"
#include "Emulator/Log.h"

#include <cerrno>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

KERNEL_LIB_NAME();

int KYTY_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_destroy(&(*rwlock)->p);

	KYTY_LOG_DEBUG("\trwlock destroy: %s, %d\n", (*rwlock)->name.C_Str(), result);

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

	KYTY_LOG_DEBUG("\trwlock init: %s, %d\n", (*rwlock)->name.C_Str(), result);

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

	// KYTY_LOG_DEBUG("\trwlock rdlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

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

#ifdef __APPLE__
	timespec timeout {};
	usec_to_timespec(&timeout, usec);
	const int result = pthread_rwlock_timedrdlock(&(*rwlock)->p, &timeout);
#else
	timespec deadline {};
	if (!relative_usec_to_absolute_timespec(0, usec, &deadline))
	{
		return KERNEL_ERROR_EINVAL;
	}
	const int result = pthread_rwlock_timedrdlock(&(*rwlock)->p, &deadline);
#endif

	// KYTY_LOG_DEBUG("\trwlock timedrdlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

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

#ifdef __APPLE__
	timespec timeout {};
	usec_to_timespec(&timeout, usec);
	const int result = pthread_rwlock_timedwrlock(&(*rwlock)->p, &timeout);
#else
	timespec deadline {};
	if (!relative_usec_to_absolute_timespec(0, usec, &deadline))
	{
		return KERNEL_ERROR_EINVAL;
	}
	const int result = pthread_rwlock_timedwrlock(&(*rwlock)->p, &deadline);
#endif

	// KYTY_LOG_DEBUG("\trwlock timedwrlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

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

	// KYTY_LOG_DEBUG("\trwlock tryrdlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

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

	// KYTY_LOG_DEBUG("\trwlock trywrlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

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

	// KYTY_LOG_DEBUG("\trwlock unlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

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

	// KYTY_LOG_DEBUG("\trwlock wrlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

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

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED
