#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_INPUTMAPPER_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_INPUTMAPPER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Host/HostInput.h"
#include "Emulator/Host/InputProfile.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace Kyty::Emulator::Host {

// Pad state in guest units: sticks 0..255 with 128 at rest, triggers 0..255.
struct StickAxes
{
	int x = 128;
	int y = 128;
};

// Held controls as one bit per PadControl, before and after an input.
struct ControlChange
{
	uint32_t before = 0;
	uint32_t after  = 0;
};

[[nodiscard]] constexpr uint32_t ControlBit(PadControl control)
{
	return 1u << static_cast<uint32_t>(control);
}

// Keyboard and mouse source of the virtual pad.  Pure state: the window feeds it host events and forwards the result.
class KeyboardMouseMapper final
{
public:
	explicit KeyboardMouseMapper(const InputProfile* profile);

	// `code` is a scan code or a MOUSE_BUTTON_* code.  Mouse buttons count only while the mouse is captured.
	ControlChange SetInput(int code, bool down);
	// Releasing the capture drops held mouse buttons and stops mouse look.
	ControlChange      SetMouseCaptured(bool captured);
	[[nodiscard]] bool MouseCaptured() const { return m_captured; }
	void               AddMouseMotion(int dx, int dy);
	// Turns the motion gathered since the previous tick into the mouse-look deflection.
	void          Tick(double now_seconds);
	ControlChange ReleaseAll();

	[[nodiscard]] uint32_t  Controls() const;
	[[nodiscard]] StickAxes LeftStick() const;
	[[nodiscard]] StickAxes RightStick() const;

private:
	const InputProfile* m_profile;
	std::vector<int>    m_held;
	bool                m_captured    = false;
	int                 m_motion_x    = 0;
	int                 m_motion_y    = 0;
	double              m_last_tick_s = -1.0;
	float               m_look_x      = 0.0f;
	float               m_look_y      = 0.0f;
};

// Physical gamepad sources: button remapping and stick and trigger deadzones, per SDL instance id.
class GamepadMapper final
{
public:
	explicit GamepadMapper(const InputProfile* profile);

	ControlChange SetButton(int device, ControllerButton button, bool down);
	// `raw` is the SDL axis value: -32768..32767 for sticks, 0..32767 for triggers.
	void SetAxis(int device, ControllerAxis axis, int raw);
	void Remove(int device);

	[[nodiscard]] StickAxes LeftStick(int device) const;
	[[nodiscard]] StickAxes RightStick(int device) const;
	[[nodiscard]] int       Trigger(int device, ControllerAxis axis) const;

private:
	struct Device
	{
		uint32_t           buttons = 0;
		std::array<int, 7> raw {};
	};

	[[nodiscard]] uint32_t Controls(uint32_t buttons) const;
	[[nodiscard]] int      Raw(int device, ControllerAxis axis) const;

	const InputProfile*   m_profile;
	std::map<int, Device> m_devices;
};

// Radial deadzone: inside it the stick rests, outside it the remaining travel is rescaled to the full range.
[[nodiscard]] StickAxes ApplyStickDeadzone(int raw_x, int raw_y, const StickTuning& tuning);
[[nodiscard]] int       ApplyTriggerDeadzone(int raw, float deadzone);

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_INPUTMAPPER_H_ */
