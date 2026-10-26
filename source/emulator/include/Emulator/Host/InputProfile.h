#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_INPUTPROFILE_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_INPUTPROFILE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Host/HostInput.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Kyty::Emulator::Host {

// Guest pad controls a profile can bind.  The names match the .pad route actions.
enum class PadControl : uint8_t
{
	Cross,
	Circle,
	Square,
	Triangle,
	Up,
	Down,
	Left,
	Right,
	L1,
	R1,
	L2,
	R2,
	L3,
	R3,
	Options,
	Touchpad,
	LeftStickLeft,
	LeftStickRight,
	LeftStickUp,
	LeftStickDown,
	RightStickLeft,
	RightStickRight,
	RightStickUp,
	RightStickDown,
};

inline constexpr size_t PAD_CONTROL_COUNT = 24;
// Controls before LeftStickLeft are buttons; the stick directions bind only on the keyboard.
inline constexpr size_t PAD_BUTTON_CONTROL_COUNT = 16;

[[nodiscard]] const char* PadControlName(PadControl control);

// Keyboard bindings hold SDL scan codes; mouse buttons use these negative codes.
inline constexpr int MOUSE_BUTTON_LEFT   = -1;
inline constexpr int MOUSE_BUTTON_MIDDLE = -2;
inline constexpr int MOUSE_BUTTON_RIGHT  = -3;
inline constexpr int MOUSE_BUTTON_X1     = -4;
inline constexpr int MOUSE_BUTTON_X2     = -5;

struct StickTuning
{
	float deadzone = 0.10f;
	bool  invert_x = false;
	bool  invert_y = false;
};

// One kyty_input_profile_v1 document (docs/input-profiles.md).
struct InputProfile
{
	std::string                                     name;
	std::array<std::vector<int>, PAD_CONTROL_COUNT> keyboard;
	bool                                            mouse_look        = true;
	float                                           mouse_sensitivity = 1.0f;
	bool                                            mouse_invert_y    = false;
	// Empty means every connected gamepad; otherwise a GUID as HostInput::ControllerGuid reports it.
	std::string                                                         gamepad_device;
	std::array<std::vector<ControllerButton>, PAD_BUTTON_CONTROL_COUNT> gamepad_buttons;
	StickTuning                                                         left_stick;
	StickTuning                                                         right_stick;
	float                                                               trigger_deadzone = 0.0f;
};

// Parses a profile document.  Returns an error naming the offending field, or an empty string.
[[nodiscard]] std::string ParseInputProfile(const std::string& text, InputProfile* profile);
// The built-in profile used when no profile file is configured.
[[nodiscard]] InputProfile DefaultInputProfile();
// Reads and parses a profile file.  Returns an error, or an empty string.
[[nodiscard]] std::string LoadInputProfile(const std::string& path, InputProfile* profile);
// True when two GUIDs name the same controller model, ignoring the name CRC in bytes 2-3.
[[nodiscard]] bool ControllerGuidMatches(const std::string& a, const std::string& b);

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_INPUTPROFILE_H_ */
