#include "Emulator/GpuMemoryFault.h"

namespace Kyty::Emulator::GpuMemoryFault {

bool Port::AreComplete(const Callbacks& callbacks) noexcept
{
	return callbacks.access_violation != nullptr && callbacks.fault_handler_installed != nullptr;
}

bool Port::Install(const Callbacks& callbacks)
{
	if (!AreComplete(callbacks))
	{
		return false;
	}

	std::lock_guard lock(m_install_mutex);
	if (m_callbacks.load(std::memory_order_acquire) != nullptr)
	{
		return false;
	}

	m_installed_callbacks = callbacks;
	m_callbacks.store(&m_installed_callbacks, std::memory_order_release);
	return true;
}

bool Port::HandleAccessViolation(uint64_t address, AccessViolationKind access) const noexcept
{
	const auto* callbacks = m_callbacks.load(std::memory_order_acquire);
	return callbacks != nullptr && callbacks->access_violation != nullptr && callbacks->access_violation(address, access);
}

void Port::NotifyFaultHandlerInstalled() const noexcept
{
	const auto* callbacks = m_callbacks.load(std::memory_order_acquire);
	if (callbacks != nullptr && callbacks->fault_handler_installed != nullptr)
	{
		callbacks->fault_handler_installed();
	}
}

Port& GetPort()
{
	// Installation initializes this on a normal thread before the fault handler
	// can dispatch. The installed handler remains live through runtime shutdown.
	static auto* port = new Port;
	return *port;
}

} // namespace Kyty::Emulator::GpuMemoryFault
