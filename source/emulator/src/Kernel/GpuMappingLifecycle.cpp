#include "Emulator/Kernel/GpuMappingLifecycle.h"

#include "Kyty/Core/DbgAssert.h"

#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::Memory {

namespace {

bool IsRepresentableRange(uint64_t vaddr, uint64_t size)
{
	return vaddr != 0 && size != 0 && vaddr <= std::numeric_limits<uint64_t>::max() - size;
}

} // namespace

bool GpuMappingLifecyclePort::Install(const GpuMappingLifecycleCallbacks& callbacks)
{
	if (callbacks.register_range == nullptr || callbacks.invalidate_ranges == nullptr || callbacks.release_range == nullptr ||
	    callbacks.protect_range == nullptr || callbacks.invalidate_ranges_deferred == nullptr || callbacks.ticket_retired == nullptr ||
	    callbacks.wait_ticket == nullptr)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_callbacks.register_range != nullptr || m_callbacks.invalidate_ranges != nullptr || m_callbacks.release_range != nullptr ||
	    m_callbacks.protect_range != nullptr)
	{
		return false;
	}
	m_callbacks = callbacks;
	return true;
}

bool GpuMappingLifecyclePort::IsInstalled() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_callbacks.register_range != nullptr && m_callbacks.invalidate_ranges != nullptr && m_callbacks.release_range != nullptr &&
	       m_callbacks.protect_range != nullptr;
}

bool GpuMappingLifecyclePort::RegisterRange(uint64_t vaddr, uint64_t size, KernelGpuMappingBacking backing)
{
	if (!IsRepresentableRange(vaddr, size))
	{
		return false;
	}

	GpuMappingLifecycleCallbacks callbacks {};
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_callbacks.register_range == nullptr || m_callbacks.release_range == nullptr)
		{
			return false;
		}
		callbacks = m_callbacks;
	}
	callbacks.register_range(callbacks.context, vaddr, size, backing);
	return true;
}

bool GpuMappingLifecyclePort::InvalidateRanges(const KernelGpuMappingRange* ranges, size_t range_count)
{
	if (ranges == nullptr || range_count == 0)
	{
		return false;
	}
	for (size_t i = 0; i < range_count; ++i)
	{
		if (!IsRepresentableRange(ranges[i].vaddr, ranges[i].size)) { return false; }
	}

	GpuMappingLifecycleCallbacks callbacks {};
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_callbacks.register_range == nullptr || m_callbacks.invalidate_ranges == nullptr || m_callbacks.release_range == nullptr)
		{
			return false;
		}
		callbacks = m_callbacks;
	}
	return callbacks.invalidate_ranges(callbacks.context, ranges, range_count);
}

bool GpuMappingLifecyclePort::InvalidateRange(uint64_t vaddr, uint64_t size)
{
	const KernelGpuMappingRange range {vaddr, size};
	return InvalidateRanges(&range, 1);
}

bool GpuMappingLifecyclePort::InvalidateRangesDeferred(const KernelGpuMappingRange* ranges, size_t range_count, uint64_t* retire_ticket)
{
	if (retire_ticket == nullptr)
	{
		return false;
	}
	*retire_ticket = 0;
	if (ranges == nullptr || range_count == 0)
	{
		return false;
	}
	for (size_t i = 0; i < range_count; ++i)
	{
		if (!IsRepresentableRange(ranges[i].vaddr, ranges[i].size)) { return false; }
	}

	GpuMappingLifecycleCallbacks callbacks {};
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_callbacks.register_range == nullptr || m_callbacks.invalidate_ranges == nullptr || m_callbacks.release_range == nullptr)
		{
			return false;
		}
		callbacks = m_callbacks;
	}
	return callbacks.invalidate_ranges_deferred(callbacks.context, ranges, range_count, retire_ticket);
}

bool GpuMappingLifecyclePort::IsTicketRetired(uint64_t ticket)
{
	GpuMappingLifecycleCallbacks callbacks {};
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		callbacks = m_callbacks;
	}
	if (ticket == 0)
	{
		return true;
	}
	// A nonzero ticket only comes from an installed adapter; anything else is a broken lifecycle.
	EXIT_IF(callbacks.ticket_retired == nullptr);
	return callbacks.ticket_retired(callbacks.context, ticket);
}

void GpuMappingLifecyclePort::WaitTicket(uint64_t ticket)
{
	GpuMappingLifecycleCallbacks callbacks {};
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		callbacks = m_callbacks;
	}
	if (ticket == 0)
	{
		return;
	}
	EXIT_IF(callbacks.wait_ticket == nullptr);
	callbacks.wait_ticket(callbacks.context, ticket);
}

bool GpuMappingLifecyclePort::ReleaseRange(uint64_t vaddr, uint64_t size, KernelGpuMappingCompletion completion,
	                                        void* completion_data)
{
	if (!IsRepresentableRange(vaddr, size) || completion == nullptr)
	{
		return false;
	}

	GpuMappingLifecycleCallbacks callbacks {};
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_callbacks.register_range == nullptr || m_callbacks.release_range == nullptr)
		{
			return false;
		}
		callbacks = m_callbacks;
	}
	return callbacks.release_range(callbacks.context, vaddr, size, completion, completion_data);
}

bool GpuMappingLifecyclePort::ProtectRange(uint64_t vaddr, uint64_t size, KernelGpuMappingCompletion completion,
	                                        void* completion_data)
{
	if (!IsRepresentableRange(vaddr, size) || completion == nullptr)
	{
		return false;
	}

	GpuMappingLifecycleCallbacks callbacks {};
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_callbacks.protect_range == nullptr)
		{
			return false;
		}
		callbacks = m_callbacks;
	}
	return callbacks.protect_range(callbacks.context, vaddr, size, completion, completion_data);
}

GpuMappingLifecyclePort& GetGpuMappingLifecyclePort()
{
	static GpuMappingLifecyclePort port;
	return port;
}

} // namespace Kyty::Kernel::Memory

#endif // KYTY_EMU_ENABLED
