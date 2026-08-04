#include "PthreadInternal.h"

#include "Emulator/Kernel/Errors.h"

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <limits>

#ifdef KYTY_EMU_ENABLED

#if defined(__APPLE__)
#include <unistd.h>
#endif

namespace Kyty::Kernel {

#ifdef __APPLE__
void usec_to_timespec(struct timespec* ts, KernelUseconds usec)
{
	ts->tv_sec  = usec / 1000000;
	ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>((usec % 1000000) * 1000);
}
#endif

// Guest timed-lock entry points pass durations. Native pthread timed waits
// instead take an absolute deadline in the clock domain selected by the
// synchronization object.
bool relative_usec_to_absolute_timespec(KernelClockid clock_id, KernelUseconds usec, timespec* deadline)
{
	if (deadline == nullptr)
	{
		return false;
	}

	KernelTimespec now {};
	if (KernelClockGettime(clock_id, &now) != OK || now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000)
	{
		return false;
	}

	const int64_t seconds = static_cast<int64_t>(usec / 1000000u);
	const int64_t nanos   = now.tv_nsec + static_cast<int64_t>((usec % 1000000u) * 1000u);
	if (now.tv_sec > std::numeric_limits<int64_t>::max() - seconds - (nanos >= 1000000000 ? 1 : 0))
	{
		return false;
	}

	deadline->tv_sec  = static_cast<decltype(deadline->tv_sec)>(now.tv_sec + seconds + (nanos >= 1000000000 ? 1 : 0));
	deadline->tv_nsec = static_cast<decltype(deadline->tv_nsec)>(nanos % 1000000000);
	return true;
}

bool guest_absolute_to_timespec(const KernelTimespec* abstime, timespec* deadline)
{
	if (abstime == nullptr || deadline == nullptr || abstime->tv_sec < 0 || abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000)
	{
		return false;
	}

	if (abstime->tv_sec > static_cast<int64_t>(std::numeric_limits<decltype(deadline->tv_sec)>::max()))
	{
		return false;
	}

	deadline->tv_sec  = static_cast<decltype(deadline->tv_sec)>(abstime->tv_sec);
	deadline->tv_nsec = static_cast<decltype(deadline->tv_nsec)>(abstime->tv_nsec);
	return true;
}

#ifdef __APPLE__
// macOS has no pthread_rwlock_timedrdlock/timedwrlock; emulate them by polling
// with try-locks. The timespec here is a relative timeout (see usec_to_timespec).
static int rwlock_timedlock_poll(pthread_rwlock_t* lock, const timespec* t, bool write)
{
	uint64_t timeout_us = static_cast<uint64_t>(t->tv_sec) * 1000000u + static_cast<uint64_t>(t->tv_nsec) / 1000u;
	uint64_t waited_us  = 0;
	for (;;)
	{
		int result = (write ? pthread_rwlock_trywrlock(lock) : pthread_rwlock_tryrdlock(lock));
		if (result != EBUSY)
		{
			return result;
		}
		if (waited_us >= timeout_us)
		{
			return ETIMEDOUT;
		}
		usleep(100);
		waited_us += 100;
	}
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t* lock, const timespec* t)
{
	return rwlock_timedlock_poll(lock, t, false);
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t* lock, const timespec* t)
{
	return rwlock_timedlock_poll(lock, t, true);
}

int mutex_timedlock_poll(pthread_mutex_t* mutex, const timespec* t)
{
	uint64_t timeout_us = static_cast<uint64_t>(t->tv_sec) * 1000000u + static_cast<uint64_t>(t->tv_nsec) / 1000u;
	uint64_t waited_us  = 0;
	for (;;)
	{
		int result = pthread_mutex_trylock(mutex);
		if (result != EBUSY)
		{
			return result;
		}
		if (waited_us >= timeout_us)
		{
			return ETIMEDOUT;
		}
		usleep(100);
		waited_us += 100;
	}
}
#endif

void sec_to_timespec(KernelTimespec* ts, double sec)
{
	ts->tv_sec  = static_cast<int64_t>(sec);
	ts->tv_nsec = static_cast<int64_t>((sec - static_cast<double>(ts->tv_sec)) * 1000000000.0);
}

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED
