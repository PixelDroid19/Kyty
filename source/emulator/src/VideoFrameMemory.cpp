#include "Emulator/VideoFrameMemory.h"

#include <mutex>

namespace Kyty::Emulator::VideoFrameMemory {

namespace {

std::mutex g_callbacks_mutex;
Callbacks  g_callbacks {};

bool callbacks_are_empty(const Callbacks& callbacks)
{
	return callbacks.register_linear_frame == nullptr && callbacks.unregister_frame == nullptr && callbacks.notify_host_write == nullptr;
}

bool callbacks_are_complete(const Callbacks& callbacks)
{
	return callbacks.register_linear_frame != nullptr && callbacks.unregister_frame != nullptr && callbacks.notify_host_write != nullptr;
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

void RegisterLinearFrame(uint64_t base, size_t size, uint32_t row_pitch_bytes)
{
	const auto callbacks = copy_callbacks();
	if (callbacks.register_linear_frame != nullptr)
	{
		callbacks.register_linear_frame(base, size, row_pitch_bytes);
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

} // namespace Kyty::Emulator::VideoFrameMemory
