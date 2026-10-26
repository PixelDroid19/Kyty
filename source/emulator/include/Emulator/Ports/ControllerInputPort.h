#ifndef EMULATOR_INCLUDE_EMULATOR_PORTS_CONTROLLERINPUTPORT_H_
#define EMULATOR_INCLUDE_EMULATOR_PORTS_CONTROLLERINPUTPORT_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

// Neutral contract between the host input path (SDL events, keyboard mapping)
// and the guest pad HLE. The pad bit values and axis ids are the guest-visible
// scePad ABI; owning them here lets Graphics forward input without including
// any HLE header, while the HLE Controller layer installs the implementation.
namespace Kyty::Emulator::Ports {

constexpr int CONTROLLER_KEYBOARD_ID = -1;

constexpr uint32_t PAD_BUTTON_L3        = 0x00000002;
constexpr uint32_t PAD_BUTTON_R3        = 0x00000004;
constexpr uint32_t PAD_BUTTON_OPTIONS   = 0x00000008;
constexpr uint32_t PAD_BUTTON_UP        = 0x00000010;
constexpr uint32_t PAD_BUTTON_RIGHT     = 0x00000020;
constexpr uint32_t PAD_BUTTON_DOWN      = 0x00000040;
constexpr uint32_t PAD_BUTTON_LEFT      = 0x00000080;
constexpr uint32_t PAD_BUTTON_L2        = 0x00000100;
constexpr uint32_t PAD_BUTTON_R2        = 0x00000200;
constexpr uint32_t PAD_BUTTON_L1        = 0x00000400;
constexpr uint32_t PAD_BUTTON_R1        = 0x00000800;
constexpr uint32_t PAD_BUTTON_TRIANGLE  = 0x00001000;
constexpr uint32_t PAD_BUTTON_CIRCLE    = 0x00002000;
constexpr uint32_t PAD_BUTTON_CROSS     = 0x00004000;
constexpr uint32_t PAD_BUTTON_SQUARE    = 0x00008000;
constexpr uint32_t PAD_BUTTON_TOUCH_PAD = 0x00100000;

enum class Axis
{
	LeftX        = 0,
	LeftY        = 1,
	RightX       = 2,
	RightY       = 3,
	TriggerLeft  = 4,
	TriggerRight = 5,

	AxisMax
};

enum class MotionSensor
{
	Acceleration,
	AngularVelocity,
};

struct ControllerInputCallbacks
{
	void (*connect)(int id)                          = nullptr;
	void (*disconnect)(int id)                       = nullptr;
	void (*button)(int id, uint32_t button, bool down) = nullptr;
	void (*axis)(int id, Axis axis, int value)       = nullptr;
	// The user ended the session (window close or the host quit key), as
	// opposed to a fault or a signal.
	void (*session_end)()                            = nullptr;
	// One motion sample of a gamepad: acceleration in G, angular velocity in
	// rad/s, in the gamepad's own frame; the timestamp is the sensor clock.
	void (*motion)(int id, MotionSensor sensor, float x, float y, float z, uint64_t timestamp_ns) = nullptr;
	// A finger (0 or 1) on the gamepad touch surface, normalized to [0, 1]
	// from the top-left corner; `down` is false once the finger lifts.
	void (*touch)(int id, int finger, bool down, float x, float y) = nullptr;
};

// Function-pointer bridge, mirroring GuestRuntimePort: the composition root
// installs the HLE implementation once at startup; callers before install
// (or after shutdown) are safe no-ops.
class ControllerInputPort final
{
public:
	static void Install(ControllerInputCallbacks callbacks);

	static void Connect(int id);
	static void Disconnect(int id);
	static void Button(int id, uint32_t button, bool down);
	static void Axis(int id, ::Kyty::Emulator::Ports::Axis axis, int value);
	static void SessionEnd();
	static void Motion(int id, MotionSensor sensor, float x, float y, float z, uint64_t timestamp_ns);
	static void Touch(int id, int finger, bool down, float x, float y);
};

// The opposite direction: the guest pad HLE asks for gamepad feedback and the
// host window, which owns the gamepads, installs the implementation. Values
// are the guest's: motor strengths 0..255 and an RGB light-bar colour.
struct ControllerFeedbackCallbacks
{
	void (*vibration)(uint8_t large_motor, uint8_t small_motor) = nullptr;
	void (*light_bar)(uint8_t red, uint8_t green, uint8_t blue) = nullptr;
};

class ControllerFeedbackPort final
{
public:
	static void Install(ControllerFeedbackCallbacks callbacks);

	static void Vibration(uint8_t large_motor, uint8_t small_motor);
	static void LightBar(uint8_t red, uint8_t green, uint8_t blue);
};

} // namespace Kyty::Emulator::Ports

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_PORTS_CONTROLLERINPUTPORT_H_ */
