#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_HOSTINPUT_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_HOSTINPUT_H_

#include "Kyty/Core/Common.h"

#include <cstdint>
#include <string>

namespace Kyty::Emulator::Host {

// HostInput is the only owner of the native event queue.  Graphics consumes
// these small value types and never needs to include a window-system header.
enum class InputEventType : uint8_t
{
	None,
	Quit,
	Terminate,
	LowMemory,
	WillEnterBackground,
	DidEnterBackground,
	WillEnterForeground,
	DidEnterForeground,
	Keyboard,
	Window,
	Display,
	MouseButton,
	MouseWheel,
	MouseMotion,
	Finger,
	ControllerAxis,
	ControllerButton,
	ControllerDevice,
	ControllerSensor,
	ControllerTouchpad,
};

enum class WindowEvent : uint8_t
{
	Shown,
	Hidden,
	Exposed,
	Moved,
	Resized,
	SizeChanged,
	Minimized,
	Maximized,
	Restored,
	Enter,
	Leave,
	FocusGained,
	FocusLost,
	Close,
	Unknown,
};

enum class DisplayOrientation : int32_t
{
	Unknown          = 0,
	Landscape        = 1,
	LandscapeFlipped = 2,
	Portrait         = 3,
	PortraitFlipped  = 4,

	// Reserved for the emulator's synthetic display-orientation event.
	DisplayEventOrientation = 0xF0,
};

enum class ControllerButton : uint8_t
{
	Invalid,
	A,
	B,
	X,
	Y,
	Back,
	Guide,
	Start,
	LeftStick,
	RightStick,
	LeftShoulder,
	RightShoulder,
	DpadUp,
	DpadDown,
	DpadLeft,
	DpadRight,
	Misc1,
	Paddle1,
	Paddle2,
	Paddle3,
	Paddle4,
	Touchpad,
};

enum class ControllerAxis : uint8_t
{
	Invalid,
	LeftX,
	LeftY,
	RightX,
	RightY,
	TriggerLeft,
	TriggerRight,
};

enum class ControllerSensor : uint8_t
{
	Invalid,
	Acceleration,
	AngularVelocity,
};

struct KeyboardEvent
{
	bool     down             = false;
	bool     up               = false;
	bool     pressed          = false;
	bool     released         = false;
	bool     repeat           = false;
	int      scan_code        = 0;
	int      key_code         = 0;
	uint16_t mod              = 0;
	double   timestamp_seconds = 0.0;
};

struct MouseEvent
{
	bool   down             = false;
	bool   up               = false;
	bool   left             = false;
	bool   middle           = false;
	bool   right            = false;
	bool   x1               = false;
	bool   x2               = false;
	bool   touch            = false;
	bool   pressed          = false;
	bool   released         = false;
	int    num_of_clicks    = 0;
	bool   wheel            = false;
	int    x                = 0;
	int    y                = 0;
	bool   motion           = false;
	int    motion_x         = 0;
	int    motion_y         = 0;
	double timestamp_seconds = 0.0;
};

struct FingerEvent
{
	bool   down             = false;
	bool   up               = false;
	bool   motion           = false;
	int    touch_id         = 0;
	int    finger_id        = 0;
	float  x                = 0.0f;
	float  y                = 0.0f;
	float  dx               = 0.0f;
	float  dy               = 0.0f;
	float  pressure         = 0.0f;
	double timestamp_seconds = 0.0;
};

struct ControllerEvent
{
	int              id                = 0;
	ControllerButton button            = ControllerButton::Invalid;
	ControllerAxis   axis_id           = ControllerAxis::Invalid;
	int              axis_value        = 0;
	bool             down              = false;
	bool             up                = false;
	bool             added             = false;
	bool             removed           = false;
	bool             remapped          = false;
	bool             axis              = false;
	bool             pressed           = false;
	bool             released          = false;
	// Motion sample: acceleration in G, angular velocity in rad/s; the
	// timestamp is the sensor's own clock.
	ControllerSensor sensor              = ControllerSensor::Invalid;
	float            sensor_data[3]      = {};
	uint64_t         sensor_timestamp_ns = 0;
	bool             touchpad          = false;
	// Touchpad finger, normalized to [0, 1] from the top-left corner;
	// touch_down stays true while the finger moves and is false once it lifts.
	bool             touch_down        = false;
	int              touch_finger      = 0;
	float            touch_x           = 0.0f;
	float            touch_y           = 0.0f;
	double           timestamp_seconds = 0.0;
};

struct WindowEventData
{
	uint32_t    window_id = 0;
	WindowEvent event     = WindowEvent::Unknown;
	int32_t     data1     = 0;
	int32_t     data2     = 0;
};

struct DisplayEventData
{
	uint32_t          display              = 0;
	DisplayOrientation orientation         = DisplayOrientation::Unknown;
	int32_t           orientation_value   = 0;
	bool              native_orientation  = false;
};

struct InputEvent
{
	InputEventType    type = InputEventType::None;
	KeyboardEvent     keyboard;
	MouseEvent        mouse;
	FingerEvent       finger;
	ControllerEvent   controller;
	WindowEventData   window;
	DisplayEventData  display;
};

class HostInput final
{
public:
	static HostInput* Create();
	~HostInput();

	[[nodiscard]] int Poll();
	[[nodiscard]] int Wait();
	[[nodiscard]] const InputEvent* GetEvent() const;

	// The pointer is deliberately opaque.  It is used only by the host GUI
	// adapter (ImGui's SDL backend) and is never inspected by Graphics.
	[[nodiscard]] const void* GetNativeEvent() const;
	[[nodiscard]] bool       DisplayEventsEnabled() const;
	[[nodiscard]] bool       EnterPressed() const;

	[[nodiscard]] bool OpenController(int device_id, int* instance_id);
	// SDL GUID string of a connected controller.
	[[nodiscard]] static std::string ControllerGuid(int device_id);
	void               CloseController(int instance_id);
	// Motor strengths 0..0xFFFF; the effect holds until the next call. A
	// gamepad without motors or without a light bar ignores the request.
	static void RumbleController(int instance_id, uint16_t low_frequency, uint16_t high_frequency);
	static void SetControllerLed(int instance_id, uint8_t red, uint8_t green, uint8_t blue);
	[[nodiscard]] const char* LastError() const;

	[[nodiscard]] static bool IsEscapeKey(int key_code);
	[[nodiscard]] static bool IsPauseKey(int key_code);
	[[nodiscard]] static bool IsF11Key(int key_code);
	[[nodiscard]] static bool IsEnterKey(int key_code);
	[[nodiscard]] static bool IsF1Key(int key_code);
	// F2 switches the keyboard between the virtual pad and text entry for the guest IME keyboard.
	[[nodiscard]] static bool IsTextEntryToggleKey(int key_code);
	// F3 captures or releases the mouse for camera look.
	[[nodiscard]] static bool IsMouseCaptureKey(int key_code);
	// Physical keys of the host commands (Escape, F1, F2, F3, F9, F11), which an input profile may not bind.
	[[nodiscard]] static bool IsReservedScanCode(int scan_code);
	// Scan code for an SDL scancode name ("Z", "Space", "Left Shift"), 0 when the name is unknown.  Profiles bind
	// physical keys so a binding keeps its position under any keyboard layout.
	[[nodiscard]] static int ScanCodeFromName(const char* name);
	// Controller button for an SDL gamepad button name ("a", "leftshoulder"), Invalid when unknown.
	[[nodiscard]] static ControllerButton ControllerButtonFromName(const char* name);
	// USB HID keyboard usage of a key, 0 when it has none.
	[[nodiscard]] static uint16_t KeyboardHidUsage(int scan_code);
	// UTF-16 code unit the key types under the host layout, 0 when it types none or only a
	// layout-dependent shifted symbol.
	[[nodiscard]] static uint16_t KeyboardTextCharacter(int key_code, uint16_t modifiers);
	[[nodiscard]] static bool HasAltModifier(uint16_t modifiers);

	KYTY_CLASS_NO_COPY(HostInput);

private:
	HostInput();

	void*      m_native_event = nullptr;
	InputEvent m_event;
};

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_HOSTINPUT_H_ */
