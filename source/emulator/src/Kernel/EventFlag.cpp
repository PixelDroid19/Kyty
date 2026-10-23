#include "Emulator/Kernel/EventFlag.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Timer.h"

#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Trace.h"
#include "Emulator/Log.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::EventFlag {

KERNEL_LIB_NAME();

// Handles are odd pointer-sized tokens, never object addresses. Their increasing
// identities are not reused during process lifetime; even values belong to semaphores.
// The registry validates handles and pins objects while calls race with deletion.
static Core::Mutex                                                                  g_event_flag_registry_mutex;
static std::unordered_map<KernelEventFlag, std::shared_ptr<KernelEventFlagPrivate>> g_live_event_flags;
static std::uintptr_t                                                               g_next_event_flag_identity = 1;

static constexpr std::uintptr_t kMaxEventFlagIdentity = std::numeric_limits<std::uintptr_t>::max() >> 1u;

static std::shared_ptr<KernelEventFlagPrivate> EventFlagAcquire(KernelEventFlag ef)
{
	if (ef == nullptr)
	{
		return {};
	}
	Core::LockGuard lock(g_event_flag_registry_mutex);
	auto            it = g_live_event_flags.find(ef);
	return it == g_live_event_flags.end() ? std::shared_ptr<KernelEventFlagPrivate> {} : it->second;
}

static KernelEventFlag EventFlagRegister(const std::shared_ptr<KernelEventFlagPrivate>& ef)
{
	EXIT_IF(ef == nullptr);
	Core::LockGuard lock(g_event_flag_registry_mutex);
	if (g_next_event_flag_identity > kMaxEventFlagIdentity)
	{
		EXIT("EventFlag token identity space exhausted\n");
		std::abort();
	}

	const std::uintptr_t token_value = (g_next_event_flag_identity++ << 1u) | std::uintptr_t {1};
	const auto           handle      = reinterpret_cast<KernelEventFlag>(token_value);
	const auto [it, inserted]        = g_live_event_flags.emplace(handle, ef);
	(void)it;
	if (!inserted)
	{
		EXIT("EventFlag token identity was reused\n");
		std::abort();
	}
	return handle;
}

static std::shared_ptr<KernelEventFlagPrivate> EventFlagUnregister(KernelEventFlag ef)
{
	if (ef == nullptr)
	{
		return {};
	}
	Core::LockGuard lock(g_event_flag_registry_mutex);
	auto            it = g_live_event_flags.find(ef);
	if (it == g_live_event_flags.end())
	{
		return {};
	}
	auto object = std::move(it->second);
	g_live_event_flags.erase(it);
	return object;
}

class KernelEventFlagPrivate
{
public:
	enum class Result
	{
		Ok,
		AlreadyWaiting,
		TimedOut,
		Canceled,
		Deleted
	};

	enum class ClearMode
	{
		None,
		All,
		Bits
	};

	enum class WaitMode
	{
		And,
		Or
	};

	KernelEventFlagPrivate(const String& name, bool single, bool /*fifo*/, uint64_t bits)
	    : m_name(name), m_single_thread(single), m_bits(bits) {};
	virtual ~KernelEventFlagPrivate();

	KYTY_CLASS_NO_COPY(KernelEventFlagPrivate);

	Result Set(uint64_t bits);
	Result Clear(uint64_t bits);
	Result Cancel(uint64_t bits, int* num_waiting_threads);
	Result Delete();
	Result Wait(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result, uint32_t* ptr_micros);

	Result Poll(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result)
	{
		uint32_t micros = 0;
		return Wait(bits, wait_mode, clear_mode, result, &micros);
	}

	[[nodiscard]] const String& GetName() const { return m_name; }
	[[nodiscard]] uint64_t      GetBits() const
	{
		Core::LockGuard lock(m_mutex);
		return m_bits;
	}

private:
	enum class Status
	{
		Set,
		Canceled,
		Deleted
	};

	class WaitCallGuard
	{
	public:
		explicit WaitCallGuard(KernelEventFlagPrivate* object): m_object(object) { m_object->m_active_wait_calls++; }
		~WaitCallGuard()
		{
			m_object->m_active_wait_calls--;
			if (m_object->m_status == Status::Deleted && m_object->m_active_wait_calls == 0)
			{
				m_object->m_cond_var.SignalAll();
			}
		}

	private:
		KernelEventFlagPrivate* m_object;
	};

	mutable Core::Mutex m_mutex;
	Core::CondVar       m_cond_var;
	Status              m_status            = Status::Set;
	int                 m_waiting_threads   = 0;
	int                 m_active_wait_calls = 0;
	String              m_name;
	bool                m_single_thread = false;
	uint64_t            m_bits          = 0;
};

KernelEventFlagPrivate::~KernelEventFlagPrivate()
{
	(void)Delete();
}

KernelEventFlagPrivate::Result KernelEventFlagPrivate::Delete()
{
	Core::LockGuard lock(m_mutex);

	while (m_status == Status::Canceled)
	{
		PthreadInterruptibleWait(&m_cond_var, &m_mutex);
	}
	if (m_status == Status::Deleted)
	{
		return Result::Deleted;
	}

	m_status = Status::Deleted;
	m_cond_var.SignalAll();
	while (m_active_wait_calls > 0)
	{
		PthreadInterruptibleWait(&m_cond_var, &m_mutex);
	}

	return Result::Ok;
}

KernelEventFlagPrivate::Result KernelEventFlagPrivate::Wait(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result,
                                                            uint32_t* ptr_micros)
{
	Core::LockGuard lock(m_mutex);
	WaitCallGuard   active_call(this);

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
	const auto condition_satisfied = [this, bits, wait_mode]
	{ return (wait_mode == WaitMode::And && (m_bits & bits) == bits) || (wait_mode == WaitMode::Or && (m_bits & bits) != 0); };
	if (m_status == Status::Deleted)
	{
		if (result != nullptr)
		{
			*result = m_bits;
		}
		if (ptr_micros != nullptr)
		{
			*ptr_micros = micros;
		}
		return Result::Deleted;
	}

	while (m_status == Status::Canceled && !condition_satisfied())
	{
		if (elapsed >= micros && !infinitely)
		{
			if (result != nullptr)
			{
				*result = m_bits;
			}
			if (ptr_micros != nullptr)
			{
				*ptr_micros = 0;
			}
			return Result::TimedOut;
		}

		if (infinitely)
		{
			PthreadInterruptibleWait(&m_cond_var, &m_mutex);
		} else
		{
			PthreadInterruptibleWaitFor(&m_cond_var, &m_mutex, micros - elapsed);
		}
		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);
		if (m_status == Status::Deleted)
		{
			if (result != nullptr)
			{
				*result = m_bits;
			}
			if (ptr_micros != nullptr)
			{
				*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			}
			return Result::Deleted;
		}
	}

	if (m_single_thread && m_waiting_threads > 0)
	{
		return Result::AlreadyWaiting;
	}

	while (!condition_satisfied())
	{
		if ((elapsed >= micros && !infinitely))
		{
			if (result != nullptr)
			{
				*result = m_bits;
			}
			*ptr_micros = 0;
			return Result::TimedOut;
		}

		m_waiting_threads++;

		if (infinitely)
		{
			PthreadInterruptibleWait(&m_cond_var, &m_mutex);
		} else
		{
			PthreadInterruptibleWaitFor(&m_cond_var, &m_mutex, micros - elapsed);
		}

		m_waiting_threads--;
		if (m_status == Status::Canceled && m_waiting_threads == 0)
		{
			m_cond_var.SignalAll();
		}

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);

		if (m_status == Status::Canceled)
		{
			if (result != nullptr)
			{
				*result = m_bits;
			}
			if (ptr_micros != nullptr)
			{
				*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			}
			return Result::Canceled;
		}

		if (m_status == Status::Deleted)
		{
			if (result != nullptr)
			{
				*result = m_bits;
			}
			if (ptr_micros != nullptr)
			{
				*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			}
			return Result::Deleted;
		}
	}

	if (result != nullptr)
	{
		*result = m_bits;
	}

	if (clear_mode == ClearMode::All)
	{
		m_bits = 0;
	} else if (clear_mode == ClearMode::Bits)
	{
		m_bits &= ~bits;
	}

	if (ptr_micros != nullptr)
	{
		*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
	}

	return Result::Ok;
}

KernelEventFlagPrivate::Result KernelEventFlagPrivate::Set(uint64_t bits)
{
	Core::LockGuard lock(m_mutex);

	while (m_status == Status::Canceled)
	{
		PthreadInterruptibleWait(&m_cond_var, &m_mutex);
	}
	if (m_status == Status::Deleted)
	{
		return Result::Deleted;
	}

	m_bits |= bits;

	m_cond_var.SignalAll();
	return Result::Ok;
}

KernelEventFlagPrivate::Result KernelEventFlagPrivate::Clear(uint64_t bits)
{
	Core::LockGuard lock(m_mutex);

	while (m_status == Status::Canceled)
	{
		PthreadInterruptibleWait(&m_cond_var, &m_mutex);
	}
	if (m_status == Status::Deleted)
	{
		return Result::Deleted;
	}

	m_bits &= bits;
	return Result::Ok;
}

KernelEventFlagPrivate::Result KernelEventFlagPrivate::Cancel(uint64_t bits, int* num_waiting_threads)
{
	Core::LockGuard lock(m_mutex);

	while (m_status == Status::Canceled)
	{
		PthreadInterruptibleWait(&m_cond_var, &m_mutex);
	}
	if (m_status == Status::Deleted)
	{
		return Result::Deleted;
	}

	if (num_waiting_threads != nullptr)
	{
		*num_waiting_threads = m_waiting_threads;
	}

	m_status = Status::Canceled;
	m_bits   = bits;

	m_cond_var.SignalAll();

	while (m_waiting_threads > 0)
	{
		PthreadInterruptibleWait(&m_cond_var, &m_mutex);
		if (m_status == Status::Deleted)
		{
			return Result::Deleted;
		}
	}

	m_status = Status::Set;
	m_cond_var.SignalAll();
	return Result::Ok;
}

int KYTY_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr, uint64_t init_pattern, const void* param)
{
	PRINT_NAME();

	if (param != nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n");
	}

	if (ef == nullptr || name == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// attr = waiter queue (0/FIFO 0x01/priority 0x02) | waiters (0/single 0x10/multi 0x20);
	// zero fields take the defaults, FIFO and single.
	const uint32_t queue_mode  = attr & 0x0fu;
	const uint32_t thread_mode = attr & 0xf0u;
	if (queue_mode > 0x02u || (thread_mode != 0u && thread_mode != 0x10u && thread_mode != 0x20u))
	{
		return KERNEL_ERROR_EINVAL;
	}
	const bool single = thread_mode != 0x20u;
	const bool fifo   = queue_mode != 0x02u;

	auto object = std::make_shared<KernelEventFlagPrivate>(String::FromUtf8(name), single, fifo, init_pattern);
	*ef         = EventFlagRegister(object);

	KYTY_LOG_DEBUG("\tEventFlag create: %s\n", name);
	KYTY_LOG_DEBUG("\tEventFlag ptr    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(*ef));

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag ef)
{
	PRINT_NAME();

	auto object = EventFlagUnregister(ef);
	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	return object->Delete() == KernelEventFlagPrivate::Result::Ok ? OK : KERNEL_ERROR_EACCES;
}

int KYTY_SYSV_ABI KernelWaitEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode, uint64_t* result_pat,
                                      KernelUseconds* timeout)
{
	PRINT_NAME();
	KYTY_GUEST_WAIT(PthreadWaitKind::EventFlag, ef);

	auto object = EventFlagAcquire(ef);
	if (!object)
	{
		KYTY_LOG_DEBUG("\tEventFlag wait: invalid handle 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(ef));
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KernelEventFlagPrivate::WaitMode  wait  = KernelEventFlagPrivate::WaitMode::And;
	KernelEventFlagPrivate::ClearMode clear = KernelEventFlagPrivate::ClearMode::None;

	switch (wait_mode & 0xfu)
	{
		case 0x01: wait = KernelEventFlagPrivate::WaitMode::And; break;
		case 0x02: wait = KernelEventFlagPrivate::WaitMode::Or; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	switch (wait_mode & 0xf0u)
	{
		case 0x00: clear = KernelEventFlagPrivate::ClearMode::None; break;
		case 0x10: clear = KernelEventFlagPrivate::ClearMode::All; break;
		case 0x20: clear = KernelEventFlagPrivate::ClearMode::Bits; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	if (std::getenv("KYTY_EVENTFLAG_TRACE") != nullptr)
	{
		static std::atomic<uint32_t> wait_logs {0};
		const uint32_t               n = wait_logs.fetch_add(1);
		if (n < 64u)
		{
			KYTY_LOG_DEBUG("EVENTFLAG_WAIT name=%s want=0x%016" PRIx64 " have=0x%016" PRIx64 " mode=0x%x timeout=%s\n",
			               object->GetName().C_Str(), bit_pattern, object->GetBits(), wait_mode, timeout == nullptr ? "inf" : "finite");
		}
	}

	auto result = object->Wait(bit_pattern, wait, clear, result_pat, timeout);

	int ret = OK;

	switch (result)
	{
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut: ret = KERNEL_ERROR_ETIMEDOUT; break;
		case KernelEventFlagPrivate::Result::Canceled: ret = KERNEL_ERROR_ECANCELED; break;
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EACCES; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelPollEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode, uint64_t* result_pat)
{
	PRINT_NAME();

	auto object = EventFlagAcquire(ef);
	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KernelEventFlagPrivate::WaitMode  wait  = KernelEventFlagPrivate::WaitMode::And;
	KernelEventFlagPrivate::ClearMode clear = KernelEventFlagPrivate::ClearMode::None;

	switch (wait_mode & 0xfu)
	{
		case 0x01: wait = KernelEventFlagPrivate::WaitMode::And; break;
		case 0x02: wait = KernelEventFlagPrivate::WaitMode::Or; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	switch (wait_mode & 0xf0u)
	{
		case 0x00: clear = KernelEventFlagPrivate::ClearMode::None; break;
		case 0x10: clear = KernelEventFlagPrivate::ClearMode::All; break;
		case 0x20: clear = KernelEventFlagPrivate::ClearMode::Bits; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	auto result = object->Poll(bit_pattern, wait, clear, result_pat);

	int ret = OK;

	switch (result)
	{
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut:
		case KernelEventFlagPrivate::Result::Canceled:
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EBUSY; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelSetEventFlag(KernelEventFlag ef, uint64_t bit_pattern)
{
	PRINT_NAME();

	auto object = EventFlagAcquire(ef);
	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	if (std::getenv("KYTY_EVENTFLAG_TRACE") != nullptr)
	{
		static std::atomic<uint32_t> set_logs {0};
		const uint32_t               n = set_logs.fetch_add(1);
		if (n < 128u)
		{
			KYTY_LOG_DEBUG("EVENTFLAG_SET name=%s bits=0x%016" PRIx64 "\n", object->GetName().C_Str(), bit_pattern);
		}
	}

	return object->Set(bit_pattern) == KernelEventFlagPrivate::Result::Ok ? OK : KERNEL_ERROR_EACCES;
}

int KYTY_SYSV_ABI KernelClearEventFlag(KernelEventFlag ef, uint64_t bit_pattern)
{
	PRINT_NAME();

	auto object = EventFlagAcquire(ef);
	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	return object->Clear(bit_pattern) == KernelEventFlagPrivate::Result::Ok ? OK : KERNEL_ERROR_EACCES;
}

int KYTY_SYSV_ABI KernelCancelEventFlag(KernelEventFlag ef, uint64_t set_pattern, int* num_wait_threads)
{
	PRINT_NAME();

	auto object = EventFlagAcquire(ef);
	if (!object)
	{
		return KERNEL_ERROR_ESRCH;
	}

	return object->Cancel(set_pattern, num_wait_threads) == KernelEventFlagPrivate::Result::Ok ? OK : KERNEL_ERROR_EACCES;
}

} // namespace Kyty::Kernel::EventFlag

#endif // KYTY_EMU_ENABLED
