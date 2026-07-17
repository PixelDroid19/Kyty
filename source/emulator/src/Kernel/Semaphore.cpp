#include "Emulator/Kernel/Semaphore.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Timer.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <chrono>
#include <cstdio>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::Semaphore {

LIB_NAME("libkernel", "libkernel");

class KernelSemaPrivate
{
public:
	enum class Result
	{
		Ok,
		TimedOut,
		Canceled,
		Deleted,
		InvalCount
	};

	KernelSemaPrivate(const String& name, bool /*fifo*/, int init_count, int max_count)
	    : m_name(name), /*m_fifo_order(fifo),*/ m_count(init_count), m_init_count(init_count), m_max_count(max_count) {};
	virtual ~KernelSemaPrivate();

	KYTY_CLASS_NO_COPY(KernelSemaPrivate);

	Result Cancel(int set_count, int* num_waiting_threads);
	Result Signal(int signal_count);
	Result Wait(int need_count, uint32_t* ptr_micros);

	Result Poll(int need_count)
	{
		uint32_t micros = 0;
		return Wait(need_count, &micros);
	}

	[[nodiscard]] const String& GetName() const { return m_name; }

private:
	enum class Status
	{
		Set,
		Canceled,
		Deleted
	};

	Core::Mutex   m_mutex;
	Core::CondVar m_cond_var;
	String        m_name;
	Status        m_status = Status::Set;
	Vector<int>   m_waiting_threads;
	// bool          m_fifo_order;
	int m_count;
	int m_init_count;
	int m_max_count;
};

KernelSemaPrivate::~KernelSemaPrivate()
{
	Core::LockGuard lock(m_mutex);

	while (m_status != Status::Set)
	{
		m_mutex.Unlock();
		Core::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	m_status = Status::Deleted;

	m_cond_var.SignalAll();

	while (!m_waiting_threads.IsEmpty())
	{
		m_mutex.Unlock();
		Core::Thread::SleepMicro(10);
		m_mutex.Lock();
	}
}

KernelSemaPrivate::Result KernelSemaPrivate::Cancel(int set_count, int* num_waiting_threads)
{
	Core::LockGuard lock(m_mutex);

	if (set_count > m_max_count)
	{
		return Result::InvalCount;
	}

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set)
	{
		m_mutex.Unlock();
		Core::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	if (num_waiting_threads != nullptr)
	{
		*num_waiting_threads = static_cast<int>(m_waiting_threads.Size());
	}

	m_status = Status::Canceled;

	m_count = (set_count < 0 ? m_init_count : set_count);

	m_cond_var.SignalAll();

	while (!m_waiting_threads.IsEmpty())
	{
		m_mutex.Unlock();
		Core::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	m_status = Status::Set;

	return Result::Ok;
}

KernelSemaPrivate::Result KernelSemaPrivate::Signal(int signal_count)
{
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set)
	{
		m_mutex.Unlock();
		Core::Thread::SleepMicro(10);
		m_mutex.Lock();
	}

	if (m_count + signal_count > m_max_count)
	{
		return Result::InvalCount;
	}

	m_count += signal_count;

	m_cond_var.SignalAll();

	return Result::Ok;
}

KernelSemaPrivate::Result KernelSemaPrivate::Wait(int need_count, uint32_t* ptr_micros)
{
	Core::LockGuard lock(m_mutex);

	if (need_count < 1 || need_count > m_max_count)
	{
		return Result::InvalCount;
	}

	uint32_t micros     = 0;
	bool     infinitely = true;
	if (ptr_micros != nullptr)
	{
		micros     = *ptr_micros;
		infinitely = false;
	}

	uint32_t    elapsed = 0;
	Core::Timer t;
	t.Start();

	int id = Core::Thread::GetThreadIdUnique();

	while (!(m_count - need_count >= 0))
	{
		if ((elapsed >= micros && !infinitely))
		{
			*ptr_micros = 0;
			return Result::TimedOut;
		}

		m_waiting_threads.Add(id);

		if (infinitely)
		{
			m_cond_var.Wait(&m_mutex);
		} else
		{
			m_cond_var.WaitFor(&m_mutex, micros - elapsed);
		}

		m_waiting_threads.Remove(id);

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);

		if (m_status == Status::Canceled)
		{
			if (ptr_micros != nullptr)
			{
				*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			}
			return Result::Canceled;
		}

		if (m_status == Status::Deleted)
		{
			if (ptr_micros != nullptr)
			{
				*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			}
			return Result::Deleted;
		}
	}

	m_count -= need_count;

	if (ptr_micros != nullptr)
	{
		*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
	}

	return Result::Ok;
}

int KYTY_SYSV_ABI KernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(sem == nullptr);

	if (name == nullptr || init < 0 || init > max || opt != nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	bool fifo = false;

	switch (attr)
	{
		case 0x01: fifo = true; break;
		case 0x02:
		default: fifo = false; break;
	}

	*sem = new KernelSemaPrivate(String::FromUtf8(name), fifo, init, max);

	printf("\t Semaphore create: %s, %d, %d\n", name, init, max);

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteSema(KernelSema sem)
{
	PRINT_NAME();

	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	delete sem;

	return OK;
}

int KYTY_SYSV_ABI KernelWaitSema(KernelSema sem, int need, KernelUseconds* time)
{
	PRINT_NAME();

	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	printf("\t Semaphore wait: %s, %d, %d\n", sem->GetName().C_Str(), need, (time != nullptr ? *time : -1));

	auto result = sem->Wait(need, time);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount: ret = KERNEL_ERROR_EINVAL; break;
		case KernelSemaPrivate::Result::TimedOut: ret = KERNEL_ERROR_ETIMEDOUT; break;
		case KernelSemaPrivate::Result::Canceled: ret = KERNEL_ERROR_ECANCELED; break;
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EACCES; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelPollSema(KernelSema sem, int need)
{
	PRINT_NAME();

	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	printf("\t Semaphore poll: %s, %d\n", sem->GetName().C_Str(), need);

	auto result = sem->Poll(need);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount: ret = KERNEL_ERROR_EINVAL; break;
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EBUSY; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelSignalSema(KernelSema sem, int count)
{
	PRINT_NAME();

	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	printf("\t Semaphore signal: %s, %d\n", sem->GetName().C_Str(), count);

	auto result = sem->Signal(count);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount:
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EINVAL; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelCancelSema(KernelSema sem, int count, int* threads)
{
	PRINT_NAME();

	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	auto result = sem->Cancel(count, threads);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount:
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EINVAL; break;
	}

	return ret;
}

} // namespace Kyty::Libs::LibKernel::Semaphore

// Gen5 Posix semaphore exports (libkernel Posix_v1). Guest object is 16 bytes.
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
	PosixSemGuest* m_guest   = nullptr;
	int            m_count   = 0;
	uint32_t       m_waiters = 0;
};

Core::Mutex                                         g_posix_sem_mutex;
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
	// #region agent log
	{
		const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
		                    std::chrono::system_clock::now().time_since_epoch())
		                    .count();
		if (FILE* f = std::fopen("/home/monasterios/.cursor/debug-logs/debug-0fe784.log", "a"))
		{
			std::fprintf(f,
			             "{\"sessionId\":\"0fe784\",\"runId\":\"post-fix\",\"hypothesisId\":\"F\","
			             "\"location\":\"Semaphore.cpp:sem_init\",\"message\":\"posix sem_init\","
			             "\"data\":{\"sem\":%llu,\"pshared\":%d,\"value\":%u},\"timestamp\":%lld}\n",
			             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(sem)), pshared, value,
			             static_cast<long long>(ts));
			std::fclose(f);
		}
	}
	// #endregion
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
		}
		else
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

#endif
