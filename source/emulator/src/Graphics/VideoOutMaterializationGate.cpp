#include "Emulator/Graphics/VideoOutMaterializationGate.h"

#include "Kyty/Core/DbgAssert.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VideoOutMaterializationGate::Pin::Pin(Pin&& other) noexcept: m_gate(other.m_gate)
{
	other.m_gate = nullptr;
}

VideoOutMaterializationGate::Pin& VideoOutMaterializationGate::Pin::operator=(Pin&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		m_gate       = other.m_gate;
		other.m_gate = nullptr;
	}
	return *this;
}

VideoOutMaterializationGate::Pin::~Pin()
{
	Reset();
}

void VideoOutMaterializationGate::Pin::Reset()
{
	if (m_gate != nullptr)
	{
		auto* gate = m_gate;
		m_gate     = nullptr;
		gate->Release();
	}
}

VideoOutMaterializationGate::Pin VideoOutMaterializationGate::Acquire()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_active_pins == UINT32_MAX);
	m_active_pins++;
	return Pin(this);
}

void VideoOutMaterializationGate::WaitUntilIdle()
{
	Core::LockGuard lock(m_mutex);
	while (m_active_pins != 0)
	{
		m_idle_cond_var.Wait(&m_mutex);
	}
}

void VideoOutMaterializationGate::Release()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_active_pins == 0);
	m_active_pins--;
	if (m_active_pins == 0)
	{
		m_idle_cond_var.SignalAll();
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
