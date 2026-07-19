#include "Emulator/Graphics/Objects/ExactStagingPool.h"

#include "Kyty/Core/DbgAssert.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

ExactStagingPool::ExactStagingPool(uint32_t max_slots, ExactStagingPoolBackend backend): m_max_slots(max_slots), m_backend(backend)
{
	EXIT_IF(m_backend.create == nullptr);
	EXIT_IF(m_backend.destroy == nullptr);
}

ExactStagingPool::~ExactStagingPool()
{
	EXIT_IF(!m_slots.IsEmpty());
	EXIT_IF(!m_transients.IsEmpty());
}

ExactStagingLease ExactStagingPool::CreatePooled(uint32_t slot, uint64_t size)
{
	auto& entry = m_slots[slot];

	if (entry.resource.object != nullptr)
	{
		m_backend.destroy(m_backend.user, entry.resource);
		entry.resource = {};
	}

	entry.resource = m_backend.create(m_backend.user, size);
	if (entry.resource.object == nullptr || entry.resource.mapped == nullptr)
	{
		if (entry.resource.object != nullptr)
		{
			m_backend.destroy(m_backend.user, entry.resource);
		}
		entry = {};
		return {};
	}

	entry.size       = size;
	entry.generation = ++m_sequence;
	entry.last_use   = m_sequence;
	entry.in_use     = true;

	return {entry.resource, entry.size, entry.generation, slot};
}

ExactStagingLease ExactStagingPool::Acquire(uint64_t size)
{
	if (size == 0)
	{
		return {};
	}

	Core::LockGuard lock(m_mutex);

	for (uint32_t i = 0; i < m_slots.Size(); i++)
	{
		auto& entry = m_slots[i];
		if (!entry.in_use && entry.size == size)
		{
			entry.in_use     = true;
			entry.generation = ++m_sequence;
			entry.last_use   = m_sequence;
			return {entry.resource, entry.size, entry.generation, i};
		}
	}

	if (m_slots.Size() < m_max_slots)
	{
		m_slots.Add(Slot {});
		return CreatePooled(m_slots.Size() - 1, size);
	}

	uint32_t replacement = UINT32_MAX;
	uint64_t oldest      = UINT64_MAX;
	for (uint32_t i = 0; i < m_slots.Size(); i++)
	{
		const auto& entry = m_slots[i];
		if (!entry.in_use && entry.last_use < oldest)
		{
			replacement = i;
			oldest      = entry.last_use;
		}
	}
	if (replacement != UINT32_MAX)
	{
		return CreatePooled(replacement, size);
	}

	const auto resource = m_backend.create(m_backend.user, size);
	if (resource.object == nullptr || resource.mapped == nullptr)
	{
		if (resource.object != nullptr)
		{
			m_backend.destroy(m_backend.user, resource);
		}
		return {};
	}

	const auto generation = ++m_sequence;
	m_transients.Add({resource, generation});
	return {resource, size, generation, UINT32_MAX};
}

bool ExactStagingPool::Release(const ExactStagingLease& lease)
{
	if (!lease.IsValid())
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);

	if (lease.IsPooled())
	{
		if (lease.slot >= m_slots.Size())
		{
			return false;
		}
		auto& entry = m_slots[lease.slot];
		if (!entry.in_use || entry.generation != lease.generation || entry.size != lease.size ||
		    entry.resource.object != lease.resource.object || entry.resource.mapped != lease.resource.mapped)
		{
			return false;
		}
		entry.in_use   = false;
		entry.last_use = ++m_sequence;
		return true;
	}

	for (uint32_t i = 0; i < m_transients.Size(); i++)
	{
		const auto& transient = m_transients[i];
		if (transient.generation == lease.generation && transient.resource.object == lease.resource.object &&
		    transient.resource.mapped == lease.resource.mapped)
		{
			m_backend.destroy(m_backend.user, transient.resource);
			m_transients.RemoveAt(i);
			return true;
		}
	}

	return false;
}

bool ExactStagingPool::DeleteAll()
{
	Core::LockGuard lock(m_mutex);

	if (!m_transients.IsEmpty())
	{
		return false;
	}
	for (const auto& entry: m_slots)
	{
		if (entry.in_use)
		{
			return false;
		}
	}
	for (const auto& entry: m_slots)
	{
		if (entry.resource.object != nullptr)
		{
			m_backend.destroy(m_backend.user, entry.resource);
		}
	}
	m_slots.Clear();
	return true;
}

uint32_t ExactStagingPool::Size() const
{
	Core::LockGuard lock(m_mutex);
	return m_slots.Size();
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
