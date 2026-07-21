#include "Emulator/Graphics/VideoOutFlipQueueAdmissionGate.h"

#include "Kyty/Core/DbgAssert.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VideoOutFlipQueueAdmissionGate::VideoOutFlipQueueAdmissionGate(uint32_t capacity): m_capacity(capacity)
{
	EXIT_IF(capacity == 0);
}

bool VideoOutFlipQueueAdmissionGate::TryAcquire()
{
	Core::LockGuard lock(m_mutex);
	if (m_active >= m_capacity)
	{
		return false;
	}
	m_active++;
	return true;
}

void VideoOutFlipQueueAdmissionGate::AcquireBlocking()
{
	m_mutex.Lock();
	while (m_active >= m_capacity)
	{
		m_space_available.Wait(&m_mutex);
	}
	m_active++;
	m_mutex.Unlock();
}

void VideoOutFlipQueueAdmissionGate::Release()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_active == 0);
	m_active--;
	m_space_available.Signal();
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
