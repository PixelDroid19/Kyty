#include "Emulator/GuestMemory.h"

namespace Kyty::Emulator::GuestMemory {

bool Port::AreComplete(const Callbacks& callbacks) noexcept
{
	return callbacks.query_mapped_range != nullptr && callbacks.query_protection != nullptr;
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

bool Port::QueryMappedRange(uint64_t address, uint64_t size, MappedRange* out) const noexcept
{
	const auto* callbacks = m_callbacks.load(std::memory_order_acquire);
	return callbacks != nullptr && callbacks->query_mapped_range != nullptr && callbacks->query_mapped_range(address, size, out);
}

int Port::QueryProtection(void* address, void** start, void** end, int* protection) const noexcept
{
	const auto* callbacks = m_callbacks.load(std::memory_order_acquire);
	return callbacks != nullptr && callbacks->query_protection != nullptr ? callbacks->query_protection(address, start, end, protection) : -1;
}

Port& GetPort()
{
	static Port port;
	return port;
}

} // namespace Kyty::Emulator::GuestMemory
