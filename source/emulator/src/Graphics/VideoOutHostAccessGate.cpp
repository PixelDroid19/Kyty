#include "Emulator/Graphics/VideoOutHostAccessGate.h"

#include "Kyty/Core/DbgAssert.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VideoOutHostAccessGate::AccessPin::AccessPin(AccessPin&& other) noexcept: m_gate(other.m_gate)
{
	other.m_gate = nullptr;
}

VideoOutHostAccessGate::AccessPin& VideoOutHostAccessGate::AccessPin::operator=(AccessPin&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		m_gate       = other.m_gate;
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
		gate->ReleaseAccess();
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

VideoOutHostAccessGate::QuiescePin VideoOutHostAccessGate::Quiesce()
{
	m_mutex.Lock();
	while (m_quiescing)
	{
		m_state_changed.Wait(&m_mutex);
	}
	m_quiescing = true;
	while (m_active_accesses != 0)
	{
		m_state_changed.Wait(&m_mutex);
	}
	m_mutex.Unlock();
	return QuiescePin(this);
}

void VideoOutHostAccessGate::ReleaseAccess()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_active_accesses == 0);
	m_active_accesses--;
	if (m_active_accesses == 0)
	{
		m_state_changed.SignalAll();
	}
}

void VideoOutHostAccessGate::EndQuiesce()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(!m_quiescing || m_active_accesses != 0);
	m_quiescing = false;
	m_state_changed.SignalAll();
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
