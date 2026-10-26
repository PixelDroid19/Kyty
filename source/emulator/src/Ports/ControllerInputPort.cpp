#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Ports/ControllerInputPort.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Ports {

namespace {

std::atomic<ControllerInputCallbacks>    g_callbacks {ControllerInputCallbacks {}};
std::atomic<ControllerFeedbackCallbacks> g_feedback {ControllerFeedbackCallbacks {}};

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

void ControllerInputPort::SessionEnd()
{
	auto callbacks = g_callbacks.load();
	if (callbacks.session_end != nullptr)
	{
		callbacks.session_end();
	}
}

void ControllerInputPort::Motion(int id, MotionSensor sensor, float x, float y, float z, uint64_t timestamp_ns)
{
	auto callbacks = g_callbacks.load();
	if (callbacks.motion != nullptr)
	{
		callbacks.motion(id, sensor, x, y, z, timestamp_ns);
	}
}

void ControllerInputPort::Touch(int id, int finger, bool down, float x, float y)
{
	auto callbacks = g_callbacks.load();
	if (callbacks.touch != nullptr)
	{
		callbacks.touch(id, finger, down, x, y);
	}
}

void ControllerFeedbackPort::Install(ControllerFeedbackCallbacks callbacks)
{
	g_feedback.store(callbacks);
}

void ControllerFeedbackPort::Vibration(uint8_t large_motor, uint8_t small_motor)
{
	auto callbacks = g_feedback.load();
	if (callbacks.vibration != nullptr)
	{
		callbacks.vibration(large_motor, small_motor);
	}
}

void ControllerFeedbackPort::LightBar(uint8_t red, uint8_t green, uint8_t blue)
{
	auto callbacks = g_feedback.load();
	if (callbacks.light_bar != nullptr)
	{
		callbacks.light_bar(red, green, blue);
	}
}

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED
