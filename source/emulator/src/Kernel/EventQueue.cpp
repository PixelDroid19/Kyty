#include "Emulator/Kernel/EventQueue.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/LinkList.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Timer.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::EventQueue {

LIB_NAME("libkernel", "libkernel");

namespace {

struct KernelEqueueLifetime
{
	uint64_t generation = 0;
	uint32_t pins       = 0;
	bool     closing    = false;
};

Core::Mutex                                            g_equeue_lifetime_mutex;
Core::CondVar                                          g_equeue_lifetime_changed;
std::unordered_map<KernelEqueue, KernelEqueueLifetime> g_equeue_lifetimes;
std::unordered_map<uint64_t, KernelEqueue>              g_closing_equeue_generations;
uint64_t                                                g_next_equeue_generation = 1;

void ReleaseEqueue(KernelEqueueIdentity identity)
{
	Core::LockGuard lock(g_equeue_lifetime_mutex);
	const auto      found = g_equeue_lifetimes.find(identity.eq);
	EXIT_IF(found == g_equeue_lifetimes.end() || found->second.generation != identity.generation ||
	        found->second.pins == 0);
	found->second.pins--;
	if (found->second.pins == 0)
	{
		g_equeue_lifetime_changed.SignalAll();
	}
}

} // namespace

KernelEqueuePin::KernelEqueuePin(KernelEqueuePin&& other) noexcept: m_identity(other.m_identity)
{
	other.m_identity = {};
}

KernelEqueuePin& KernelEqueuePin::operator=(KernelEqueuePin&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		m_identity       = other.m_identity;
		other.m_identity = {};
	}
	return *this;
}

KernelEqueuePin::~KernelEqueuePin()
{
	Reset();
}

void KernelEqueuePin::Reset()
{
	if (m_identity)
	{
		const auto identity = m_identity;
		m_identity          = {};
		ReleaseEqueue(identity);
	}
}

KernelEqueuePin KernelAcquireEqueue(KernelEqueue eq)
{
	if (eq == nullptr)
	{
		return {};
	}

	g_equeue_lifetime_mutex.Lock();
	const auto found = g_equeue_lifetimes.find(eq);
	if (found == g_equeue_lifetimes.end())
	{
		g_equeue_lifetime_mutex.Unlock();
		return {};
	}
	if (found->second.closing)
	{
		g_equeue_lifetime_mutex.Unlock();
		return {};
	}
	EXIT_IF(found->second.pins == UINT32_MAX);
	found->second.pins++;
	const KernelEqueueIdentity identity {eq, found->second.generation};
	g_equeue_lifetime_mutex.Unlock();
	return KernelEqueuePin(identity);
}

KernelEqueuePin KernelAcquireEqueue(KernelEqueueIdentity identity)
{
	if (!identity)
	{
		return {};
	}
	Core::LockGuard lock(g_equeue_lifetime_mutex);
	const auto      found = g_equeue_lifetimes.find(identity.eq);
	if (found == g_equeue_lifetimes.end() || found->second.generation != identity.generation || found->second.closing)
	{
		return {};
	}
	EXIT_IF(found->second.pins == UINT32_MAX);
	found->second.pins++;
	return KernelEqueuePin(identity);
}

void KernelWaitEqueueClosed(KernelEqueueIdentity identity)
{
	if (!identity)
	{
		return;
	}
	Core::LockGuard lock(g_equeue_lifetime_mutex);
	for (;;)
	{
		const auto active  = g_equeue_lifetimes.find(identity.eq);
		const auto closing = g_closing_equeue_generations.find(identity.generation);
		const bool active_same_generation =
		    active != g_equeue_lifetimes.end() && active->second.generation == identity.generation;
		const bool closing_same_generation =
		    closing != g_closing_equeue_generations.end() && closing->second == identity.eq;
		if (!active_same_generation && !closing_same_generation)
		{
			return;
		}
		g_equeue_lifetime_changed.Wait(&g_equeue_lifetime_mutex);
	}
}

class KernelEqueuePrivate
{
public:
	KernelEqueuePrivate() = default;
	virtual ~KernelEqueuePrivate();

	KYTY_CLASS_NO_COPY(KernelEqueuePrivate);

	[[nodiscard]] const String& GetName() const { return m_name; }
	void                        SetName(const String& m_name) { this->m_name = m_name; }

	void AddEvent(const KernelEqueueEvent& event);
	bool TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data);
	bool DeleteEvent(uintptr_t ident, int16_t filter);
	void BeginClose();

	int GetTriggeredEvents(KernelEvent* ev, int num);
	int WaitForEvents(KernelEvent* ev, int num, uint32_t micros);

private:
	Core::List<KernelEqueueEvent> m_events;
	Core::Mutex                   m_mutex;
	Core::CondVar                 m_cond_var;
	String                        m_name;
	bool                          m_closing = false;
};

KernelEqueuePrivate::~KernelEqueuePrivate()
{
	Core::LockGuard lock(m_mutex);

	FOR_LIST(index, m_events)
	{
		auto& event = m_events[index];

		if (event.filter.delete_event_func != nullptr)
		{
			event.filter.delete_event_func(this, &event);
		}
	}
}

int KernelEqueuePrivate::GetTriggeredEvents(KernelEvent* ev, int num)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);
	if (m_closing)
	{
		return -1;
	}

	int ret = 0;

	FOR_LIST(index, m_events)
	{
		auto& event = m_events[index];

		if (event.triggered)
		{
			ev[ret++] = event.event;

			if (event.filter.reset_func != nullptr)
			{
				event.filter.reset_func(&event);
			}

			if (ret >= num)
			{
				break;
			}
		}
	}

	return ret;
}

int KernelEqueuePrivate::WaitForEvents(KernelEvent* ev, int num, uint32_t micros)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);

	uint32_t    elapsed = 0;
	Core::Timer t;
	t.Start();

	for (;;)
	{
		int ret = GetTriggeredEvents(ev, num);

		if (ret != 0 || (elapsed >= micros && micros != 0))
		{
			return ret;
		}

		if (micros == 0)
		{
			m_cond_var.Wait(&m_mutex);
		} else
		{
			m_cond_var.WaitFor(&m_mutex, micros - elapsed);
		}

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);
	}

	return 0;
}

void KernelEqueuePrivate::BeginClose()
{
	Core::LockGuard lock(m_mutex);
	m_closing = true;
	m_cond_var.SignalAll();
}

void KernelEqueuePrivate::AddEvent(const KernelEqueueEvent& event)
{
	Core::LockGuard lock(m_mutex);

	if (auto index = m_events.Find(event.event.ident, event.event.filter,
	                               [](auto e, auto ident, auto filter) { return e.event.ident == ident && e.event.filter == filter; });
	    m_events.IndexValid(index))
	{
		auto& previous = m_events[index];
		if (previous.filter.delete_event_func != nullptr)
		{
			previous.filter.delete_event_func(this, &previous);
		}
		m_events[index] = event;
	} else
	{
		m_events.Add(event);
	}

	if (event.triggered)
	{
		m_cond_var.Signal();
	}
}

bool KernelEqueuePrivate::TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data)
{
	Core::LockGuard lock(m_mutex);

	if (auto index = m_events.Find(ident, filter,
	                               [](auto e, auto ident, auto filter) { return e.event.ident == ident && e.event.filter == filter; });
	    m_events.IndexValid(index))
	{
		auto& event = m_events[index];

		if (event.filter.trigger_func != nullptr)
		{
			event.filter.trigger_func(&event, trigger_data);
		} else
		{
			event.triggered = true;
		}

		m_cond_var.Signal();

		return true;
	}

	return false;
}

bool KernelEqueuePrivate::DeleteEvent(uintptr_t ident, int16_t filter)
{
	Core::LockGuard lock(m_mutex);

	if (auto index = m_events.Find(ident, filter,
	                               [](auto e, auto ident, auto filter) { return e.event.ident == ident && e.event.filter == filter; });
	    m_events.IndexValid(index))
	{
		auto& event = m_events[index];

		if (event.filter.delete_event_func != nullptr)
		{
			event.filter.delete_event_func(this, &event);
		}

		m_events.Remove(index);

		return true;
	}

	return false;
}

int KYTY_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name)
{
	PRINT_NAME();

	if (eq == nullptr || name == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*eq = new KernelEqueuePrivate;

	(*eq)->SetName(String::FromUtf8(name));
	{
		Core::LockGuard lock(g_equeue_lifetime_mutex);
		EXIT_IF(g_next_equeue_generation == 0);
		KernelEqueueLifetime lifetime {};
		lifetime.generation             = g_next_equeue_generation++;
		const auto [iterator, inserted] = g_equeue_lifetimes.emplace(*eq, lifetime);
		(void)iterator;
		EXIT_IF(!inserted);
	}

	printf("\tEqueue create: %s\n", name);

	return OK;
}

static void ampr_event_reset_func(KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void ampr_event_trigger_func(KernelEqueueEvent* event, void* trigger_data)
{
	EXIT_IF(event == nullptr);
	event->triggered = true;
	event->event.fflags++;
	if (trigger_data != nullptr)
	{
		event->event.data = reinterpret_cast<intptr_t>(trigger_data);
	}
}

int KYTY_SYSV_ABI KernelAddAmprEvent(KernelEqueue eq, uint64_t reserved0, uint64_t reserved1, uintptr_t ident, void* udata)
{
	PRINT_NAME();

	printf("\t eq        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(eq));
	printf("\t reserved0 = 0x%016" PRIx64 "\n", reserved0);
	printf("\t reserved1 = 0x%016" PRIx64 "\n", reserved1);
	printf("\t ident     = 0x%016" PRIx64 "\n", static_cast<uint64_t>(ident));
	printf("\t udata     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(udata));

	if (eq == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = ident;
	event.event.filter             = KERNEL_EVFILT_AMPR;
	event.event.flags              = 0;
	event.event.fflags             = 0;
	event.event.data               = 0;
	event.event.udata              = udata;
	event.filter.trigger_func      = ampr_event_trigger_func;
	event.filter.reset_func        = ampr_event_reset_func;
	event.filter.delete_event_func = nullptr;
	event.filter.data              = nullptr;

	return KernelAddEvent(eq, event);
}

int KYTY_SYSV_ABI KernelDeleteAmprEvent(KernelEqueue eq, uintptr_t ident)
{
	PRINT_NAME();

	printf("\t eq    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(eq));
	printf("\t ident = 0x%016" PRIx64 "\n", static_cast<uint64_t>(ident));

	return KernelDeleteEvent(eq, ident, KERNEL_EVFILT_AMPR);
}

int KYTY_SYSV_ABI KernelAddEvent(KernelEqueue eq, const KernelEqueueEvent& event)
{
	auto pin = KernelAcquireEqueue(eq);
	if (!pin)
	{
		return KERNEL_ERROR_EBADF;
	}
	return KernelAddEvent(pin, event);
}

int KernelAddEvent(const KernelEqueuePin& eq, const KernelEqueueEvent& event)
{
	EXIT_IF(!eq);
	eq.Get()->AddEvent(event);
	return OK;
}

int KYTY_SYSV_ABI KernelTriggerEvent(KernelEqueue eq, uintptr_t ident, int16_t filter, void* trigger_data)
{
	auto pin = KernelAcquireEqueue(eq);
	if (!pin)
	{
		return KERNEL_ERROR_EBADF;
	}
	return KernelTriggerEvent(pin, ident, filter, trigger_data);
}

int KernelTriggerEvent(const KernelEqueuePin& eq, uintptr_t ident, int16_t filter, void* trigger_data)
{
	EXIT_IF(!eq);
	if (!eq.Get()->TriggerEvent(ident, filter, trigger_data))
	{
		return KERNEL_ERROR_ENOENT;
	}

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteEvent(KernelEqueue eq, uintptr_t ident, int16_t filter)
{
	auto pin = KernelAcquireEqueue(eq);
	if (!pin)
	{
		return KERNEL_ERROR_EBADF;
	}
	return KernelDeleteEvent(pin, ident, filter);
}

int KernelDeleteEvent(const KernelEqueuePin& eq, uintptr_t ident, int16_t filter)
{
	EXIT_IF(!eq);
	if (!eq.Get()->DeleteEvent(ident, filter))
	{
		return KERNEL_ERROR_ENOENT;
	}

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq)
{
	PRINT_NAME();

	if (eq == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	g_equeue_lifetime_mutex.Lock();
	auto found = g_equeue_lifetimes.find(eq);
	if (found == g_equeue_lifetimes.end() || found->second.closing)
	{
		g_equeue_lifetime_mutex.Unlock();
		return KERNEL_ERROR_EBADF;
	}
	found->second.closing = true;
	const KernelEqueueIdentity identity {eq, found->second.generation};
	g_equeue_lifetime_mutex.Unlock();

	eq->BeginClose();

	g_equeue_lifetime_mutex.Lock();
	for (;;)
	{
		found = g_equeue_lifetimes.find(eq);
		EXIT_IF(found == g_equeue_lifetimes.end() || !found->second.closing);
		if (found->second.pins == 0)
		{
			break;
		}
		g_equeue_lifetime_changed.Wait(&g_equeue_lifetime_mutex);
	}
	EXIT_IF(g_equeue_lifetimes.erase(eq) != 1);
	const auto [closing, inserted] = g_closing_equeue_generations.emplace(identity.generation, eq);
	(void)closing;
	EXIT_IF(!inserted);
	g_equeue_lifetime_changed.SignalAll();
	g_equeue_lifetime_mutex.Unlock();

	printf("\tEqueue delete: %s\n", eq->GetName().C_Str());

	delete eq;

	{
		Core::LockGuard lock(g_equeue_lifetime_mutex);
		const auto      closing = g_closing_equeue_generations.find(identity.generation);
		EXIT_IF(closing == g_closing_equeue_generations.end() || closing->second != eq);
		g_closing_equeue_generations.erase(closing);
		g_equeue_lifetime_changed.SignalAll();
	}

	return OK;
}

int KYTY_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out, const KernelUseconds* timo)
{
	// PRINT_NAME();

	if (eq == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (ev == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (num < 1)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(out == nullptr);

	auto pin = KernelAcquireEqueue(eq);
	if (!pin)
	{
		return KERNEL_ERROR_EBADF;
	}

	// printf("\tEqueue wait: %s\n", eq->GetName().C_Str());

	if (timo == nullptr)
	{
		*out = pin.Get()->WaitForEvents(ev, num, 0);
	}

	if (timo != nullptr)
	{
		if (*timo == 0)
		{
			*out = pin.Get()->GetTriggeredEvents(ev, num);
		} else
		{
			*out = pin.Get()->WaitForEvents(ev, num, *timo);
		}
	}

	if (*out < 0)
	{
		*out = 0;
		return KERNEL_ERROR_EBADF;
	}
	if (*out == 0)
	{
		// printf("\ttimedout\n");
		return KERNEL_ERROR_ETIMEDOUT;
	}

	// printf("\treceived %u events\n", *out);

	return OK;
}

intptr_t KYTY_SYSV_ABI KernelGetEventData(const KernelEvent* ev)
{
	PRINT_NAME();

	if (ev != nullptr)
	{
		return ev->data;
	}

	return 0;
}

intptr_t KYTY_SYSV_ABI KernelGetEventFflags(const KernelEvent* ev)
{
	PRINT_NAME();

	if (ev != nullptr)
	{
		return ev->fflags;
	}

	return 0;
}

int KYTY_SYSV_ABI KernelGetEventFilter(const KernelEvent* ev)
{
	PRINT_NAME();

	if (ev != nullptr)
	{
		return ev->filter;
	}

	return 0;
}

uintptr_t KYTY_SYSV_ABI KernelGetEventId(const KernelEvent* ev)
{
	PRINT_NAME();

	if (ev != nullptr)
	{
		return ev->ident;
	}

	return 0;
}

void* KYTY_SYSV_ABI KernelGetEventUserData(const KernelEvent* ev)
{
	PRINT_NAME();

	if (ev != nullptr)
	{
		return ev->udata;
	}

	return nullptr;
}

int KYTY_SYSV_ABI KernelGetEventError(const KernelEvent* /*ev*/)
{
	PRINT_NAME();

	KYTY_NOT_IMPLEMENTED;

	return 0;
}

} // namespace Kyty::Libs::LibKernel::EventQueue

#endif // KYTY_EMU_ENABLED
