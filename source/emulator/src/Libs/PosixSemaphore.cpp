#include "Emulator/Libs/PosixSemaphore.h"

#include "Emulator/Kernel/Namespace.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include "Kyty/Core/Threads.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Posix {

LIB_NAME("Posix", "libkernel");

namespace {

constexpr uint16_t kPosixSemMagic     = 0x09fa;
constexpr int      kPosixSemValueMax  = 0x7fffffff;
constexpr uint32_t kPosixSemPollMicro = 10000;

struct PosixSemGuest
{
	uint16_t          magic       = 0;
	uint16_t          nameid      = 0;
	volatile uint32_t has_waiters = 0;
	volatile uint32_t count       = 0;
	uint32_t          flags       = 0;
};
static_assert(sizeof(PosixSemGuest) == 16);

class PosixSemPrivate
{
public:
	PosixSemPrivate(PosixSemGuest* guest, unsigned int value, uint32_t flags): m_guest(guest), m_count(static_cast<int>(value))
	{
		SyncGuest(flags);
	}

	int Wait()
	{
		Core::LockGuard lock(m_mutex);
		while (m_count <= 0)
		{
			m_waiters++;
			SyncGuest();
			m_cond_var.WaitFor(&m_mutex, kPosixSemPollMicro);
			m_waiters--;
		}
		m_count--;
		SyncGuest();
		return OK;
	}

	int TryWait()
	{
		Core::LockGuard lock(m_mutex);
		if (m_count <= 0)
		{
			SyncGuest();
			return POSIX_EAGAIN;
		}
		m_count--;
		SyncGuest();
		return OK;
	}

	// timeout_usec == UINT32_MAX means wait forever (same as Wait).
	int TimedWait(uint32_t timeout_usec)
	{
		if (timeout_usec == UINT32_MAX)
		{
			return Wait();
		}

		Core::LockGuard lock(m_mutex);
		const auto      deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_usec);
		while (m_count <= 0)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
			{
				SyncGuest();
				return POSIX_ETIMEDOUT;
			}
			const auto remain_us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
			const auto slice     = static_cast<uint32_t>(std::min<int64_t>(remain_us, kPosixSemPollMicro));
			m_waiters++;
			SyncGuest();
			m_cond_var.WaitFor(&m_mutex, slice);
			m_waiters--;
		}
		m_count--;
		SyncGuest();
		return OK;
	}

	int Post()
	{
		Core::LockGuard lock(m_mutex);
		if (m_count == kPosixSemValueMax)
		{
			return POSIX_EOVERFLOW;
		}
		m_count++;
		SyncGuest();
		m_cond_var.Signal();
		return OK;
	}

	int GetValue()
	{
		Core::LockGuard lock(m_mutex);
		return m_count;
	}

private:
	void SyncGuest(uint32_t flags = 0xffffffffu)
	{
		if (m_guest == nullptr)
		{
			return;
		}
		m_guest->magic       = kPosixSemMagic;
		m_guest->nameid      = 0;
		m_guest->has_waiters = (m_waiters > 0 ? 1u : 0u);
		m_guest->count       = static_cast<uint32_t>(m_count);
		if (flags != 0xffffffffu)
		{
			m_guest->flags = flags;
		}
	}

	Core::Mutex    m_mutex;
	Core::CondVar  m_cond_var;
	PosixSemGuest* m_guest    = nullptr;
	int            m_count    = 0;
	uint32_t       m_waiters = 0;
};

Core::Mutex                                     g_posix_sem_mutex;
std::unordered_map<PosixSemGuest*, PosixSemPrivate*> g_posix_sems;

static int SetErrnoReturn(int posix_errno)
{
	*Posix::GetErrorAddr() = posix_errno;
	return -1;
}

static PosixSemPrivate* LookupSem(void* sem)
{
	if (sem == nullptr)
	{
		return nullptr;
	}
	Core::LockGuard lock(g_posix_sem_mutex);
	const auto      it = g_posix_sems.find(static_cast<PosixSemGuest*>(sem));
	return (it != g_posix_sems.end() ? it->second : nullptr);
}

} // namespace

int KYTY_SYSV_ABI sem_init(void* sem, int pshared, unsigned int value)
{
	PRINT_NAME();
	if (sem == nullptr || value > static_cast<unsigned int>(kPosixSemValueMax))
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}

	auto* guest = static_cast<PosixSemGuest*>(sem);
	auto* obj   = new PosixSemPrivate(guest, value, static_cast<uint32_t>(pshared));
	{
		Core::LockGuard lock(g_posix_sem_mutex);
		if (auto it = g_posix_sems.find(guest); it != g_posix_sems.end())
		{
			delete it->second;
			it->second = obj;
		} else
		{
			g_posix_sems.insert({guest, obj});
		}
	}
	return OK;
}

int KYTY_SYSV_ABI sem_destroy(void* sem)
{
	PRINT_NAME();
	if (sem == nullptr)
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}
	auto*            guest = static_cast<PosixSemGuest*>(sem);
	PosixSemPrivate* obj   = nullptr;
	{
		Core::LockGuard lock(g_posix_sem_mutex);
		const auto      it = g_posix_sems.find(guest);
		if (it == g_posix_sems.end())
		{
			return SetErrnoReturn(POSIX_EINVAL);
		}
		obj = it->second;
		g_posix_sems.erase(it);
	}
	delete obj;
	guest->magic       = 0;
	guest->has_waiters = 0;
	guest->count       = 0;
	guest->flags       = 0;
	return OK;
}

int KYTY_SYSV_ABI sem_wait(void* sem)
{
	PRINT_NAME();
	auto* obj = LookupSem(sem);
	if (obj == nullptr)
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}
	const int rc = obj->Wait();
	return (rc == OK ? OK : SetErrnoReturn(rc));
}

int KYTY_SYSV_ABI sem_trywait(void* sem)
{
	PRINT_NAME();
	auto* obj = LookupSem(sem);
	if (obj == nullptr)
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}
	const int rc = obj->TryWait();
	return (rc == OK ? OK : SetErrnoReturn(rc));
}

static uint32_t TimespecToUsec(const ::Kyty::Kernel::KernelTimespec* ts)
{
	if (ts == nullptr || ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
	{
		return 0;
	}
	const int64_t usec = ts->tv_sec * 1000000 + ts->tv_nsec / 1000;
	if (usec <= 0)
	{
		return 0;
	}
	if (usec > static_cast<int64_t>(UINT32_MAX))
	{
		return UINT32_MAX;
	}
	return static_cast<uint32_t>(usec);
}

static uint32_t AbstimeRemainingUsec(const ::Kyty::Kernel::KernelTimespec* abstime)
{
	::Kyty::Kernel::KernelTimespec now {};
	if (abstime == nullptr || ::Kyty::Kernel::KernelClockGettime(0, &now) != OK)
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
	return static_cast<uint32_t>(delta);
}

int KYTY_SYSV_ABI sem_timedwait(void* sem, const ::Kyty::Kernel::KernelTimespec* abstime)
{
	PRINT_NAME();
	if (abstime == nullptr)
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}
	auto* obj = LookupSem(sem);
	if (obj == nullptr)
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}
	const int rc = obj->TimedWait(AbstimeRemainingUsec(abstime));
	return (rc == OK ? OK : SetErrnoReturn(rc));
}

int KYTY_SYSV_ABI sem_reltimedwait_np(void* sem, const ::Kyty::Kernel::KernelTimespec* reltime)
{
	PRINT_NAME();
	if (reltime == nullptr)
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}
	auto* obj = LookupSem(sem);
	if (obj == nullptr)
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}
	const int rc = obj->TimedWait(TimespecToUsec(reltime));
	return (rc == OK ? OK : SetErrnoReturn(rc));
}

int KYTY_SYSV_ABI sem_post(void* sem)
{
	PRINT_NAME();
	auto* obj = LookupSem(sem);
	if (obj == nullptr)
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}
	const int rc = obj->Post();
	return (rc == OK ? OK : SetErrnoReturn(rc));
}

int KYTY_SYSV_ABI sem_getvalue(void* sem, int* sval)
{
	PRINT_NAME();
	auto* obj = LookupSem(sem);
	if (obj == nullptr || sval == nullptr)
	{
		return SetErrnoReturn(POSIX_EINVAL);
	}
	*sval = obj->GetValue();
	return OK;
}

} // namespace Kyty::Libs::Posix

#endif // KYTY_EMU_ENABLED
