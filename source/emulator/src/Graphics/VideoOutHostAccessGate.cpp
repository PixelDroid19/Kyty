#include "Emulator/Graphics/VideoOutHostAccessGate.h"

#include "Kyty/Core/DbgAssert.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VideoOutHostAccessGate::AccessPin::AccessPin(AccessPin&& other) noexcept: m_gate(other.m_gate), m_progress(other.m_progress)
{
	other.m_gate = nullptr;
}

VideoOutHostAccessGate::AccessPin& VideoOutHostAccessGate::AccessPin::operator=(AccessPin&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		m_gate       = other.m_gate;
		m_progress   = other.m_progress;
		other.m_gate = nullptr;
	}
	return *this;
}

VideoOutHostAccessGate::AccessPin::~AccessPin()
{
	Reset();
}

void VideoOutHostAccessGate::AccessPin::Reset()
{
	if (m_gate != nullptr)
	{
		auto* gate = m_gate;
		m_gate     = nullptr;
		gate->ReleaseAccess(m_progress);
	}
}

VideoOutHostAccessGate::QuiescePin::QuiescePin(QuiescePin&& other) noexcept: m_gate(other.m_gate)
{
	other.m_gate = nullptr;
}

VideoOutHostAccessGate::QuiescePin& VideoOutHostAccessGate::QuiescePin::operator=(QuiescePin&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		m_gate       = other.m_gate;
		other.m_gate = nullptr;
	}
	return *this;
}

VideoOutHostAccessGate::QuiescePin::~QuiescePin()
{
	Reset();
}

void VideoOutHostAccessGate::QuiescePin::Reset()
{
	if (m_gate != nullptr)
	{
		auto* gate = m_gate;
		m_gate     = nullptr;
		gate->EndQuiesce();
	}
}

VideoOutHostAccessGate::AccessPin VideoOutHostAccessGate::Acquire()
{
	m_mutex.Lock();
	while (m_quiescing)
	{
		m_state_changed.Wait(&m_mutex);
	}
	EXIT_IF(m_active_accesses == UINT32_MAX);
	m_active_accesses++;
	m_mutex.Unlock();
	return AccessPin(this);
}

VideoOutHostAccessGate::AccessPin VideoOutHostAccessGate::AcquireProgress()
{
	Core::LockGuard lock(m_mutex);
	while (m_quiescing && !m_draining)
	{
		m_state_changed.Wait(&m_mutex);
	}
	EXIT_IF(m_active_progress == UINT32_MAX);
	m_active_progress++;
	return AccessPin(this, true);
}

VideoOutHostAccessGate::DrainPin::DrainPin(DrainPin&& other) noexcept: m_gate(other.m_gate)
{
	other.m_gate = nullptr;
}

VideoOutHostAccessGate::DrainPin& VideoOutHostAccessGate::DrainPin::operator=(DrainPin&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		m_gate       = other.m_gate;
		other.m_gate = nullptr;
	}
	return *this;
}

VideoOutHostAccessGate::DrainPin::~DrainPin()
{
	Reset();
}

void VideoOutHostAccessGate::DrainPin::Reset()
{
	if (m_gate != nullptr)
	{
		auto* gate = m_gate;
		m_gate     = nullptr;
		gate->EndDrain();
	}
}

VideoOutHostAccessGate::QuiescePin VideoOutHostAccessGate::DrainPin::Quiesce()
{
	EXIT_IF(m_gate == nullptr);
	auto pin = m_gate->FinishDrain();
	m_gate   = nullptr;
	return pin;
}

VideoOutHostAccessGate::DrainPin VideoOutHostAccessGate::Drain()
{
	Core::LockGuard lock(m_mutex);
	while (m_quiescing)
	{
		m_state_changed.Wait(&m_mutex);
	}
	m_quiescing = true;
	m_draining  = true;
	while (m_active_accesses != 0)
	{
		m_state_changed.Wait(&m_mutex);
	}
	return DrainPin(this);
}

VideoOutHostAccessGate::QuiescePin VideoOutHostAccessGate::FinishDrain()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(!m_quiescing || !m_draining || m_active_accesses != 0);
	m_draining = false;
	while (m_active_progress != 0)
	{
		m_state_changed.Wait(&m_mutex);
	}
	return QuiescePin(this);
}

void VideoOutHostAccessGate::EndDrain()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(!m_quiescing || !m_draining || m_active_accesses != 0);
	m_draining  = false;
	m_quiescing = false;
	m_state_changed.SignalAll();
}

VideoOutHostAccessGate::QuiescePin VideoOutHostAccessGate::Quiesce()
{
	m_mutex.Lock();
	while (m_quiescing)
	{
		m_state_changed.Wait(&m_mutex);
	}
	m_quiescing = true;
	while (m_active_accesses != 0 || m_active_progress != 0)
	{
		m_state_changed.Wait(&m_mutex);
	}
	m_mutex.Unlock();
	return QuiescePin(this);
}

void VideoOutHostAccessGate::ReleaseAccess(bool progress)
{
	Core::LockGuard lock(m_mutex);
	auto& count = progress ? m_active_progress : m_active_accesses;
	EXIT_IF(count == 0);
	count--;
	if (count == 0)
	{
		m_state_changed.SignalAll();
	}
}

void VideoOutHostAccessGate::EndQuiesce()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(!m_quiescing || m_draining || m_active_accesses != 0 || m_active_progress != 0);
	m_quiescing = false;
	m_state_changed.SignalAll();
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
