#include "Emulator/VideoFrameMemory.h"

#include "Kyty/Core/VirtualMemory.h"

#include <cerrno>
#include <mutex>

namespace Kyty::Emulator::VideoFrameMemory {

namespace {

std::mutex g_callbacks_mutex;
Callbacks  g_callbacks {};

bool callbacks_are_empty(const Callbacks& callbacks)
{
	return callbacks.register_linear_frame == nullptr && callbacks.unregister_frame == nullptr && callbacks.notify_host_write == nullptr &&
	       callbacks.begin_host_write == nullptr && callbacks.end_host_write == nullptr && callbacks.prune_stale_layouts == nullptr;
}

bool callbacks_are_complete(const Callbacks& callbacks)
{
	return callbacks.register_linear_frame != nullptr && callbacks.unregister_frame != nullptr && callbacks.notify_host_write != nullptr &&
	       ((callbacks.begin_host_write == nullptr) == (callbacks.end_host_write == nullptr));
}

Callbacks copy_callbacks()
{
	std::lock_guard lock(g_callbacks_mutex);
	return g_callbacks;
}

} // namespace

bool InstallCallbacks(const Callbacks& callbacks)
{
	if (!callbacks_are_empty(callbacks) && !callbacks_are_complete(callbacks))
	{
		return false;
	}

	std::lock_guard lock(g_callbacks_mutex);
	g_callbacks = callbacks;
	return true;
}

bool RegisterLinearFrame(uint64_t base, size_t size, uint32_t row_pitch_bytes)
{
	if (base == 0 || size == 0 || row_pitch_bytes == 0 || size > UINT64_MAX - base)
	{
		return false;
	}

	const auto callbacks = copy_callbacks();
	if (callbacks.register_linear_frame == nullptr)
	{
		return true;
	}
	if (callbacks.prune_stale_layouts != nullptr)
	{
		callbacks.prune_stale_layouts();
	}

	Core::VirtualMemory::GuestMappingIdentitySnapshot identity {};
	if (!Core::VirtualMemory::CaptureGuestMappingIdentitySnapshot(base, size, &identity))
	{
		return false;
	}
	return callbacks.register_linear_frame(base, size, row_pitch_bytes, identity);
}

bool RegisterLinearFrame(uint64_t base, size_t size, uint32_t row_pitch_bytes,
                         const Core::VirtualMemory::GuestMappingIdentitySnapshot& identity)
{
	if (base == 0 || size == 0 || row_pitch_bytes == 0 || size > UINT64_MAX - base || identity.address != base || identity.size != size)
	{
		return false;
	}

	const auto callbacks = copy_callbacks();
	return callbacks.register_linear_frame == nullptr || callbacks.register_linear_frame(base, size, row_pitch_bytes, identity);
}

void PruneStaleLayouts()
{
	const auto callbacks = copy_callbacks();
	if (callbacks.prune_stale_layouts != nullptr)
	{
		callbacks.prune_stale_layouts();
	}
}

void UnregisterFrame(uint64_t base)
{
	const auto callbacks = copy_callbacks();
	if (callbacks.unregister_frame != nullptr)
	{
		callbacks.unregister_frame(base);
	}
}

void NotifyHostWrite(uint64_t base, uint64_t size)
{
	const auto callbacks = copy_callbacks();
	if (callbacks.notify_host_write != nullptr)
	{
		callbacks.notify_host_write(base, size);
	}
}

HostWriteLease::HostWriteLease(uint64_t base, uint64_t size)
{
	const int saved_errno = errno;
	const auto callbacks = copy_callbacks();
	if (callbacks.begin_host_write != nullptr)
	{
		m_end = callbacks.end_host_write;
		m_token = callbacks.begin_host_write(base, size);
	} else if (callbacks.notify_host_write != nullptr)
	{
		callbacks.notify_host_write(base, size);
	}
	errno = saved_errno;
}

HostWriteLease::~HostWriteLease()
{
	const int saved_errno = errno;
	if (m_end != nullptr) { m_end(m_token); }
	errno = saved_errno;
}

} // namespace Kyty::Emulator::VideoFrameMemory
