#include "Emulator/Ports/SocketEventPort.h"

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Ports {

namespace {

std::atomic<SocketEventCallbacks> g_callbacks {SocketEventCallbacks {}};

} // namespace

void SocketEventPort::Install(SocketEventCallbacks callbacks)
{
	g_callbacks.store(callbacks);
}

bool SocketEventPort::IsSocket(int id)
{
	const auto callbacks = g_callbacks.load();
	return callbacks.is_socket != nullptr && callbacks.is_socket(id);
}

int SocketEventPort::Readiness(int id, bool write, bool* ready, int64_t* data)
{
	const auto callbacks = g_callbacks.load();
	return callbacks.readiness != nullptr ? callbacks.readiness(id, write, ready, data) : NOT_INSTALLED;
}

int SocketEventPort::Watch(int id, bool write, uint64_t owner, uint64_t generation, SocketReadyNotify notify)
{
	const auto callbacks = g_callbacks.load();
	return callbacks.watch != nullptr ? callbacks.watch(id, write, owner, generation, notify) : NOT_INSTALLED;
}

void SocketEventPort::Unwatch(int id, bool write, uint64_t owner)
{
	const auto callbacks = g_callbacks.load();
	if (callbacks.unwatch != nullptr)
	{
		callbacks.unwatch(id, write, owner);
	}
}

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED
