#include "PthreadInternal.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Trace.h"
#include "Emulator/Log.h"

#include <cerrno>
#include <cstdint>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

KERNEL_LIB_NAME();

int KYTY_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr)
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr);

	*attr = new PthreadMutexattrPrivate {};

	int result = pthread_mutexattr_init(&(*attr)->p);

	result = (result == 0 ? PthreadMutexattrSettype(attr, MUTEX_TYPE_ERRORCHECK) : result);
	result = (result == 0 ? PthreadMutexattrSetprotocol(attr, 0) : result);

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr)
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int result = pthread_mutexattr_destroy(&(*attr)->p);

	delete *attr;
	*attr = nullptr;

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* attr, int type)
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int ptype = PTHREAD_MUTEX_DEFAULT;
	switch (type)
	{
		case MUTEX_TYPE_ERRORCHECK: ptype = PTHREAD_MUTEX_ERRORCHECK; break;
		case MUTEX_TYPE_RECURSIVE: ptype = PTHREAD_MUTEX_RECURSIVE; break;
		case MUTEX_TYPE_NORMAL:
		case MUTEX_TYPE_ADAPTIVE: ptype = PTHREAD_MUTEX_NORMAL; break;
		default: EXIT("invalid type: %d\n", type);
	}

	int result = pthread_mutexattr_settype(&(*attr)->p, ptype);

	if (result == 0)
	{
		(*attr)->type = type;
		return OK;
	}

	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadMutexattrSetprotocol([[maybe_unused]] PthreadMutexattr* attr, int protocol)
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	[[maybe_unused]] int pprotocol = PTHREAD_PRIO_NONE;
	switch (protocol)
	{
		case 0: pprotocol = PTHREAD_PRIO_NONE; break;
		case 1: pprotocol = PTHREAD_PRIO_INHERIT; break;
		case 2: pprotocol = PTHREAD_PRIO_PROTECT; break;
		default: EXIT("invalid protocol: %d\n", protocol);
	}

	// protocol doesn't work in winpthreads
	int result         = 0; // pthread_mutexattr_setprotocol(&(*attr)->p, pprotocol);
	(*attr)->pprotocol = pprotocol;

	if (result == 0)
	{
		return OK;
	}

	return KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name)
{
	if (name != nullptr)
	{
		PRINT_NAME();
	}

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (attr == nullptr)
	{
		EXIT_IF(g_pthread_context == nullptr);

		attr = g_pthread_context->GetDefaultMutexattr();
	}

	*mutex = new PthreadMutexPrivate {};

	(*mutex)->name = name;
	(*mutex)->type = (*attr)->type;

	int result = pthread_mutex_init(&(*mutex)->p, &(*attr)->p);

	if (name != nullptr)
	{
		KYTY_LOG_DEBUG("\tmutex init: %s, %d\n", (*mutex)->name.C_Str(), result);
	}

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex)
{
	PRINT_NAME();

	if (mutex == nullptr || *mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int result = pthread_mutex_destroy(&(*mutex)->p);

	KYTY_LOG_DEBUG("\tmutex destroy: %s, %d\n", (*mutex)->name.C_Str(), result);

	delete *mutex;
	*mutex = nullptr;

	switch (result)
	{
		case 0: return OK;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex)
{
	// PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	auto* private_mutex = *mutex;
	{
		std::lock_guard lock(private_mutex->state_mutex);
		if (private_mutex->recursion_count != 0 && pthread_equal(private_mutex->owner, pthread_self()) != 0)
		{
			if (private_mutex->type == MUTEX_TYPE_ERRORCHECK)
			{
				return KERNEL_ERROR_EDEADLK;
			}

			// Some guest runtimes layer normal or adaptive lock calls within one
			// logical critical section. Keep their depth in the guest object so a
			// host normal mutex cannot deadlock the owning guest thread.
			private_mutex->recursion_count++;
			return OK;
		}
	}

	int result = pthread_mutex_lock(&private_mutex->p);

	if (result == 0)
	{
		std::lock_guard lock(private_mutex->state_mutex);
		private_mutex->owner           = pthread_self();
		private_mutex->recursion_count = 1;
	}

	// KYTY_LOG_DEBUG("\tmutex lock: %s, %d\n", (*mutex)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex)
{
	// PRINT_NAME();

	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// Lazily initialize a statically-initialized mutex (sentinel), like PthreadMutexLock.
	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	EXIT_IF(pthread_static_objects == nullptr);
	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	auto* private_mutex = *mutex;
	{
		std::lock_guard lock(private_mutex->state_mutex);
		if (private_mutex->recursion_count != 0 && pthread_equal(private_mutex->owner, pthread_self()) != 0)
		{
			if (private_mutex->type == MUTEX_TYPE_RECURSIVE)
			{
				private_mutex->recursion_count++;
				return OK;
			}
			return KERNEL_ERROR_EBUSY;
		}
	}

	int result = pthread_mutex_trylock(&private_mutex->p);

	if (result == 0)
	{
		std::lock_guard lock(private_mutex->state_mutex);
		private_mutex->owner           = pthread_self();
		private_mutex->recursion_count = 1;
	}

	// KYTY_LOG_DEBUG("\tmutex trylock: %s, %d\n", (*mutex)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, KernelUseconds usec)
{
	PRINT_NAME();

	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();
	EXIT_IF(pthread_static_objects == nullptr);
	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	auto* private_mutex = *mutex;
	{
		std::lock_guard lock(private_mutex->state_mutex);
		if (private_mutex->recursion_count != 0 && pthread_equal(private_mutex->owner, pthread_self()) != 0 &&
		    private_mutex->type == MUTEX_TYPE_RECURSIVE)
		{
			private_mutex->recursion_count++;
			return OK;
		}
	}

#ifdef __APPLE__
	timespec t {};
	usec_to_timespec(&t, usec);
	const int result = mutex_timedlock_poll(&private_mutex->p, &t);
#else
	timespec deadline {};
	if (!relative_usec_to_absolute_timespec(0, usec, &deadline))
	{
		return KERNEL_ERROR_EINVAL;
	}
	const int result = pthread_mutex_timedlock(&private_mutex->p, &deadline);
#endif
	if (result == 0)
	{
		std::lock_guard lock(private_mutex->state_mutex);
		private_mutex->owner           = pthread_self();
		private_mutex->recursion_count = 1;
	}
	switch (result)
	{
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex)
{
	// PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	mutex = static_cast<PthreadMutex*>(pthread_static_objects->CreateObject(mutex, PthreadStaticObject::Type::Mutex));

	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*mutex == nullptr);

	auto* private_mutex = *mutex;
	std::lock_guard lock(private_mutex->state_mutex);
	if (private_mutex->recursion_count == 0 || pthread_equal(private_mutex->owner, pthread_self()) == 0)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (private_mutex->recursion_count > 1)
	{
		private_mutex->recursion_count--;
		return OK;
	}

	int result = pthread_mutex_unlock(&private_mutex->p);
	if (result == 0)
	{
		private_mutex->owner           = {};
		private_mutex->recursion_count = 0;
	}

	// KYTY_LOG_DEBUG("\tmutex unlock: %s, %d\n", (*mutex)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;

		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EPERM: return KERNEL_ERROR_EPERM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

bool KYTY_SYSV_ABI PthreadMutexCurrentOwns(PthreadMutex* mutex)
{
	if (mutex == nullptr)
	{
		return false;
	}

	auto* private_mutex = *mutex;
	if (private_mutex == nullptr || reinterpret_cast<uintptr_t>(private_mutex) < 0x100000)
	{
		return false;
	}

	std::lock_guard lock(private_mutex->state_mutex);
	return private_mutex->recursion_count != 0 && pthread_equal(private_mutex->owner, pthread_self()) != 0;
}

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED
