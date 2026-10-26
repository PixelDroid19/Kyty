#include "Emulator/Host/InputMapper.h"

#include "Kyty/Core/DbgAssert.h"

#include <algorithm>
#include <cmath>

namespace Kyty::Emulator::Host {

namespace {

// Mouse speed, in pixels per second at sensitivity 1, that deflects the look stick fully.
constexpr float LOOK_FULL_SPEED = 1000.0f;
// The pad reports a stick deadzone of about 31/127 to the guest; titles that honour it would drop smaller deflections.
constexpr int LOOK_MIN_DEFLECTION = 32;

constexpr uint32_t BUTTON_CONTROLS_MASK = (1u << PAD_BUTTON_CONTROL_COUNT) - 1u;

int AxisFromDirections(bool negative, bool positive)
{
	return negative == positive ? 128 : (negative ? 0 : 255);
}

int LookAxis(float deflection)
{
	if (deflection == 0.0f)
	{
		return 128;
	}
	const float magnitude = static_cast<float>(LOOK_MIN_DEFLECTION) + std::fabs(deflection) * static_cast<float>(127 - LOOK_MIN_DEFLECTION);
	const int   offset    = static_cast<int>(std::lround(magnitude));
	return std::clamp(deflection < 0.0f ? 128 - offset : 128 + offset, 0, 255);
}

int StickAxisValue(float value)
{
	return std::clamp(static_cast<int>(std::lround(128.0f + value * (value < 0.0f ? 128.0f : 127.0f))), 0, 255);
}

} // namespace

KeyboardMouseMapper::KeyboardMouseMapper(const InputProfile* profile): m_profile(profile)
{
	EXIT_IF(profile == nullptr);
}

ControlChange KeyboardMouseMapper::SetInput(int code, bool down)
{
	ControlChange change {Controls(), 0};
	const bool    mouse_button = code < 0;
	const auto    held         = std::find(m_held.begin(), m_held.end(), code);
	if (down && held == m_held.end() && (!mouse_button || m_captured))
	{
		m_held.push_back(code);
	} else if (!down && held != m_held.end())
	{
		m_held.erase(held);
	}
	change.after = Controls();
	return change;
}

ControlChange KeyboardMouseMapper::SetMouseCaptured(bool captured)
{
	ControlChange change {Controls(), 0};
	m_captured = captured;
	if (!captured)
	{
		m_held.erase(std::remove_if(m_held.begin(), m_held.end(), [](int code) { return code < 0; }), m_held.end());
		m_motion_x = 0;
		m_motion_y = 0;
		m_look_x   = 0.0f;
		m_look_y   = 0.0f;
	}
	change.after = Controls();
	return change;
}

void KeyboardMouseMapper::AddMouseMotion(int dx, int dy)
{
	if (m_captured && m_profile->mouse_look)
	{
		m_motion_x += dx;
		m_motion_y += dy;
	}
}

void KeyboardMouseMapper::Tick(double now_seconds)
{
	const double elapsed = now_seconds - m_last_tick_s;
	const bool   first   = m_last_tick_s < 0.0;
	m_last_tick_s        = now_seconds;
	if (first || elapsed <= 0.0)
	{
		m_motion_x = 0;
		m_motion_y = 0;
		return;
	}
	const float scale = m_profile->mouse_sensitivity / (LOOK_FULL_SPEED * static_cast<float>(elapsed));
	m_look_x          = std::clamp(static_cast<float>(m_motion_x) * scale, -1.0f, 1.0f);
	m_look_y          = std::clamp(static_cast<float>(m_motion_y) * scale * (m_profile->mouse_invert_y ? -1.0f : 1.0f), -1.0f, 1.0f);
	m_motion_x        = 0;
	m_motion_y        = 0;
}

ControlChange KeyboardMouseMapper::ReleaseAll()
{
	ControlChange change {Controls(), 0};
	m_held.clear();
	m_motion_x = 0;
	m_motion_y = 0;
	m_look_x   = 0.0f;
	m_look_y   = 0.0f;
	return change;
}

uint32_t KeyboardMouseMapper::Controls() const
{
	uint32_t controls = 0;
	for (size_t control = 0; control < PAD_CONTROL_COUNT; ++control)
	{
		const auto& codes = m_profile->keyboard[control];
		if (std::any_of(codes.begin(), codes.end(),
		                [this](int code) { return std::find(m_held.begin(), m_held.end(), code) != m_held.end(); }))
		{
			controls |= 1u << control;
		}
	}
	return controls;
}

StickAxes KeyboardMouseMapper::LeftStick() const
{
	const uint32_t c = Controls();
	return {AxisFromDirections((c & ControlBit(PadControl::LeftStickLeft)) != 0, (c & ControlBit(PadControl::LeftStickRight)) != 0),
	        AxisFromDirections((c & ControlBit(PadControl::LeftStickUp)) != 0, (c & ControlBit(PadControl::LeftStickDown)) != 0)};
}

StickAxes KeyboardMouseMapper::RightStick() const
{
	// Held direction keys override mouse look on their axis.
	const uint32_t c = Controls();
	const int      keys_x =
	    AxisFromDirections((c & ControlBit(PadControl::RightStickLeft)) != 0, (c & ControlBit(PadControl::RightStickRight)) != 0);
	const int keys_y =
	    AxisFromDirections((c & ControlBit(PadControl::RightStickUp)) != 0, (c & ControlBit(PadControl::RightStickDown)) != 0);
	return {keys_x != 128 ? keys_x : LookAxis(m_look_x), keys_y != 128 ? keys_y : LookAxis(m_look_y)};
}

GamepadMapper::GamepadMapper(const InputProfile* profile): m_profile(profile)
{
	EXIT_IF(profile == nullptr);
}

ControlChange GamepadMapper::SetButton(int device, ControllerButton button, bool down)
{
	auto&          state = m_devices[device];
	ControlChange  change {Controls(state.buttons), 0};
	const uint32_t bit = 1u << static_cast<uint32_t>(button);
	state.buttons      = down ? (state.buttons | bit) : (state.buttons & ~bit);
	change.after       = Controls(state.buttons);
	return change;
}

void GamepadMapper::SetAxis(int device, ControllerAxis axis, int raw)
{
	m_devices[device].raw[static_cast<size_t>(axis)] = raw;
}

void GamepadMapper::Remove(int device)
{
	m_devices.erase(device);
}

StickAxes GamepadMapper::LeftStick(int device) const
{
	return ApplyStickDeadzone(Raw(device, ControllerAxis::LeftX), Raw(device, ControllerAxis::LeftY), m_profile->left_stick);
}

StickAxes GamepadMapper::RightStick(int device) const
{
	return ApplyStickDeadzone(Raw(device, ControllerAxis::RightX), Raw(device, ControllerAxis::RightY), m_profile->right_stick);
}

int GamepadMapper::Trigger(int device, ControllerAxis axis) const
{
	return ApplyTriggerDeadzone(Raw(device, axis), m_profile->trigger_deadzone);
}

uint32_t GamepadMapper::Controls(uint32_t buttons) const
{
	uint32_t controls = 0;
	for (size_t control = 0; control < PAD_BUTTON_CONTROL_COUNT; ++control)
	{
		for (const ControllerButton button: m_profile->gamepad_buttons[control])
		{
			if ((buttons & (1u << static_cast<uint32_t>(button))) != 0)
			{
				controls |= 1u << control;
			}
		}
	}
	return controls & BUTTON_CONTROLS_MASK;
}

int GamepadMapper::Raw(int device, ControllerAxis axis) const
{
	const auto it = m_devices.find(device);
	return it == m_devices.end() ? 0 : it->second.raw[static_cast<size_t>(axis)];
}

StickAxes ApplyStickDeadzone(int raw_x, int raw_y, const StickTuning& tuning)
{
	float       x         = std::clamp(static_cast<float>(raw_x) / 32767.0f, -1.0f, 1.0f);
	float       y         = std::clamp(static_cast<float>(raw_y) / 32767.0f, -1.0f, 1.0f);
	const float magnitude = std::sqrt(x * x + y * y);
	if (magnitude <= tuning.deadzone)
	{
		return {};
	}
	const float scale = std::min(1.0f, (magnitude - tuning.deadzone) / (1.0f - tuning.deadzone)) / magnitude;
	x *= scale * (tuning.invert_x ? -1.0f : 1.0f);
	y *= scale * (tuning.invert_y ? -1.0f : 1.0f);
	return {StickAxisValue(x), StickAxisValue(y)};
}

int ApplyTriggerDeadzone(int raw, float deadzone)
{
	const float travel = std::clamp(static_cast<float>(raw) / 32767.0f, 0.0f, 1.0f);
	if (travel <= deadzone)
	{
		return 0;
	}
	return std::clamp(static_cast<int>(std::lround((travel - deadzone) / (1.0f - deadzone) * 255.0f)), 0, 255);
}

} // namespace Kyty::Emulator::Host
