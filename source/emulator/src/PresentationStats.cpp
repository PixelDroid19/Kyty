#include "Emulator/PresentationStats.h"

namespace Kyty::Emulator::PresentationStats {

bool Port::Install(const Callbacks& callbacks)
{
	if (callbacks.query == nullptr)
	{
		return false;
	}

	std::lock_guard lock(m_mutex);
	if (m_callbacks.query != nullptr)
	{
		return false;
	}
	m_callbacks = callbacks;
	return true;
}

bool Port::Query(Snapshot* out) const
{
	if (out == nullptr)
	{
		return false;
	}

	Callbacks callbacks {};
	{
		std::lock_guard lock(m_mutex);
		if (m_callbacks.query == nullptr)
		{
			return false;
		}
		callbacks = m_callbacks;
	}
	return callbacks.query(callbacks.context, out);
}

Port& GetPort()
{
	static Port port;
	return port;
}

} // namespace Kyty::Emulator::PresentationStats
