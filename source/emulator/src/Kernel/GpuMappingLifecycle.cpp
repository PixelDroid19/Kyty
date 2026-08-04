#include "Emulator/Kernel/GpuMappingLifecycle.h"

#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::Memory {

namespace {

bool IsRepresentableRange(uint64_t vaddr, uint64_t size)
{
	return vaddr != 0 && size != 0 && vaddr <= std::numeric_limits<uint64_t>::max() - size;
}

} // namespace

bool GpuMappingLifecyclePort::Install(const GpuMappingLifecycleCallbacks& callbacks)
{
	if (callbacks.register_range == nullptr || callbacks.release_range == nullptr)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_callbacks.register_range != nullptr || m_callbacks.release_range != nullptr)
	{
		return false;
	}
	m_callbacks = callbacks;
	return true;
}

bool GpuMappingLifecyclePort::IsInstalled() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_callbacks.register_range != nullptr && m_callbacks.release_range != nullptr;
}

bool GpuMappingLifecyclePort::RegisterRange(uint64_t vaddr, uint64_t size)
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
	callbacks.register_range(callbacks.context, vaddr, size);
	return true;
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

GpuMappingLifecyclePort& GetGpuMappingLifecyclePort()
{
	static GpuMappingLifecyclePort port;
	return port;
}

} // namespace Kyty::Libs::LibKernel::Memory

#endif // KYTY_EMU_ENABLED
