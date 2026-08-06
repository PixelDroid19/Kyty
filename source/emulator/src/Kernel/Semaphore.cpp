#include "Emulator/Kernel/Semaphore.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Timer.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Kernel/Errors.h"
#include "Emulator/Log.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::Semaphore {

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
	void   Delete();
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

// Keep the synchronization trace independent from the global printf direction.
// A title can stall before normal HLE logging is practical, and an opt-in,
// bounded trace makes it possible to establish whether a waiter has a matching
// signal without turning on every import call.
static bool sema_trace_enabled()
{
	return std::getenv("KYTY_SEMA_TRACE") != nullptr;
}

static uint64_t sema_trace_focus_caller()
{
	static const uint64_t caller = []() {
		const char* value = std::getenv("KYTY_SEMA_TRACE_CALLER");
		return (value != nullptr ? std::strtoull(value, nullptr, 0) : 0ull);
	}();
	return caller;
}

// Keep a long-running trace useful when a title creates many unrelated
// semaphores before it reaches the blocked one. Unlike the caller focus, this
// filter applies to Create as well, so it exposes the initial count and every
// subsequent operation for one named guest semaphore.
static const char* sema_trace_name_filter()
{
	static const char* name = []() {
		const char* value = std::getenv("KYTY_SEMA_TRACE_NAME");
		return (value != nullptr && value[0] != '\0' ? value : nullptr);
	}();
	return name;
}

static std::atomic<KernelSema> g_sema_trace_focus {nullptr};

static void sema_trace(const char* operation, KernelSema sem, const KernelSemaPrivate* object, int value, int result,
	                   uint64_t caller)
{
	if (!sema_trace_enabled())
	{
		return;
	}

	if (const char* name_filter = sema_trace_name_filter(); name_filter != nullptr)
	{
		if (object == nullptr || std::strcmp(object->GetName().C_Str(), name_filter) != 0)
		{
			return;
		}
	}

	const uint64_t focus_caller = sema_trace_focus_caller();
	if (focus_caller != 0)
	{
		if (std::strcmp(operation, "wait") == 0 && caller == focus_caller)
		{
			g_sema_trace_focus.store(sem, std::memory_order_relaxed);
		}
		if (g_sema_trace_focus.load(std::memory_order_relaxed) != sem)
		{
			return;
		}
	}

	static std::atomic<uint32_t> trace_count {0};
	constexpr uint32_t           kMaxTraceEvents = 512;
	const uint32_t               index           = trace_count.fetch_add(1, std::memory_order_relaxed);
	if (index >= kMaxTraceEvents)
	{
		return;
	}

	KYTY_LOG_DEBUG( "SEMA_TRACE %s n=%u handle=%p name=%s value=%d result=%d caller=0x%016llx\n", operation, index,
	             reinterpret_cast<void*>(sem), (object != nullptr ? object->GetName().C_Str() : "<null>"), value, result,
	             static_cast<unsigned long long>(caller));
}

KernelSemaPrivate::~KernelSemaPrivate()
{
	Delete();
}

void KernelSemaPrivate::Delete()
{
	Core::LockGuard lock(m_mutex);
	m_status = Status::Deleted;
	m_cond_var.SignalAll();
}

KernelSemaPrivate::Result KernelSemaPrivate::Cancel(int set_count, int* num_waiting_threads)
{
	Core::LockGuard lock(m_mutex);

	if (set_count > m_max_count)
	{
		return Result::InvalCount;
	}

	if (m_status == Status::Deleted)
	{
		return Result::Deleted;
	}

	while (m_status != Status::Set)
	{
		m_mutex.Unlock();
		Core::Thread::SleepMicro(10);
		m_mutex.Lock();
		if (m_status == Status::Deleted)
		{
			return Result::Deleted;
		}
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

	if (m_status == Status::Deleted)
	{
		return Result::Deleted;
	}

	m_status = Status::Set;

	return Result::Ok;
}

KernelSemaPrivate::Result KernelSemaPrivate::Signal(int signal_count)
{
	Core::LockGuard lock(m_mutex);

	if (m_status == Status::Deleted)
	{
		return Result::Deleted;
	}

	while (m_status != Status::Set)
	{
		m_mutex.Unlock();
		Core::Thread::SleepMicro(10);
		m_mutex.Lock();
		if (m_status == Status::Deleted)
		{
			return Result::Deleted;
		}
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

	if (m_status == Status::Deleted)
	{
		return Result::Deleted;
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

namespace {

Core::Mutex                                                    g_sema_registry_mutex;
std::unordered_map<KernelSema, std::shared_ptr<KernelSemaPrivate>> g_live_semas;

std::shared_ptr<KernelSemaPrivate> AcquireSema(KernelSema sem)
{
	if (sem == nullptr)
	{
		return {};
	}

	Core::LockGuard lock(g_sema_registry_mutex);
	auto            it = g_live_semas.find(sem);
	return it == g_live_semas.end() ? std::shared_ptr<KernelSemaPrivate> {} : it->second;
}

} // namespace

int KYTY_SYSV_ABI KernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt)
{
	if (sem == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

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

	auto object = std::make_shared<KernelSemaPrivate>(String::FromUtf8(name), fifo, init, max);
	*sem        = object.get();
	{
		Core::LockGuard lock(g_sema_registry_mutex);
		g_live_semas.emplace(*sem, std::move(object));
	}
	const auto caller = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	sema_trace("create", *sem, AcquireSema(*sem).get(), init, max, caller);

	KYTY_LOG_DEBUG("\t Semaphore create: %s, %d, %d\n", name, init, max);

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteSema(KernelSema sem)
{
	std::shared_ptr<KernelSemaPrivate> object;
	{
		Core::LockGuard lock(g_sema_registry_mutex);
		auto            it = g_live_semas.find(sem);
		if (it == g_live_semas.end())
		{
			return KERNEL_ERROR_ESRCH;
		}
		object = std::move(it->second);
		g_live_semas.erase(it);
	}

	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	object->Delete();

	return OK;
}

int KYTY_SYSV_ABI KernelWaitSema(KernelSema sem, int need, KernelUseconds* time)
{
	auto object = AcquireSema(sem);
	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	const auto caller = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	KYTY_LOG_DEBUG("\t Semaphore wait: %s, %d, %d\n", object->GetName().C_Str(), need, (time != nullptr ? *time : -1));
	sema_trace("wait", sem, object.get(), need, 0, caller);

	auto result = object->Wait(need, time);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount: ret = KERNEL_ERROR_EINVAL; break;
		case KernelSemaPrivate::Result::TimedOut: ret = KERNEL_ERROR_ETIMEDOUT; break;
		case KernelSemaPrivate::Result::Canceled: ret = KERNEL_ERROR_ECANCELED; break;
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EACCES; break;
	}
	sema_trace("wait_done", sem, object.get(), need, ret, caller);

	return ret;
}

int KYTY_SYSV_ABI KernelPollSema(KernelSema sem, int need)
{
	auto object = AcquireSema(sem);
	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	KYTY_LOG_DEBUG("\t Semaphore poll: %s, %d\n", object->GetName().C_Str(), need);

	auto result = object->Poll(need);

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
	auto object = AcquireSema(sem);
	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	const auto caller = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	KYTY_LOG_DEBUG("\t Semaphore signal: %s, %d\n", object->GetName().C_Str(), count);
	sema_trace("signal", sem, object.get(), count, 0, caller);

	auto result = object->Signal(count);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount:
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EINVAL; break;
	}
	sema_trace("signal_done", sem, object.get(), count, ret, caller);

	return ret;
}

int KYTY_SYSV_ABI KernelCancelSema(KernelSema sem, int count, int* threads)
{
	auto object = AcquireSema(sem);
	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	auto result = object->Cancel(count, threads);

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

} // namespace Kyty::Kernel::Semaphore

#endif // KYTY_EMU_ENABLED
