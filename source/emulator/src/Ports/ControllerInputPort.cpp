#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Ports/ControllerInputPort.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Ports {

namespace {

std::atomic<ControllerInputCallbacks> g_callbacks {ControllerInputCallbacks {}};

} // namespace

void ControllerInputPort::Install(ControllerInputCallbacks callbacks)
{
	g_callbacks.store(callbacks);
}

void ControllerInputPort::Connect(int id)
{
	auto callbacks = g_callbacks.load();
	if (callbacks.connect != nullptr)
	{
		callbacks.connect(id);
	}
}

void ControllerInputPort::Disconnect(int id)
{
	auto callbacks = g_callbacks.load();
	if (callbacks.disconnect != nullptr)
	{
		callbacks.disconnect(id);
	}
}

void ControllerInputPort::Button(int id, uint32_t button, bool down)
{
	auto callbacks = g_callbacks.load();
	if (callbacks.button != nullptr)
	{
		callbacks.button(id, button, down);
	}
}

void ControllerInputPort::Axis(int id, ::Kyty::Emulator::Ports::Axis axis, int value)
{
	auto callbacks = g_callbacks.load();
	if (callbacks.axis != nullptr)
	{
		callbacks.axis(id, axis, value);
	}
}

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED
