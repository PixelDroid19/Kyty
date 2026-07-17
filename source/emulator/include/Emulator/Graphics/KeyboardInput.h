#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_KEYBOARDINPUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_KEYBOARDINPUT_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Pure keyboard → left-stick state for real press/release mapping.
// SDL event decoding stays in Window.cpp; this module owns only the axes contract.
// Header-only so unit tests resolve symbols without circular static-lib order issues.

struct KeyboardLeftStickState
{
	bool left  = false;
	bool right = false;
	bool up    = false;
	bool down  = false;
};

struct KeyboardLeftStickAxes
{
	int x = 128;
	int y = 128;
};

struct KeyboardLeftStickUpdate
{
	bool                  handled = false;
	bool                  changed = false;
	KeyboardLeftStickAxes axes {};
};

// Key codes match SDL letter values (SDLK_a == 'a', etc.).
inline constexpr int kKeyboardLeftStickKeyLeft  = 'a';
inline constexpr int kKeyboardLeftStickKeyRight = 'd';
inline constexpr int kKeyboardLeftStickKeyUp    = 'w';
inline constexpr int kKeyboardLeftStickKeyDown  = 's';

[[nodiscard]] inline KeyboardLeftStickAxes KeyboardLeftStickAxesFromState(const KeyboardLeftStickState& state)
{
	KeyboardLeftStickAxes axes {};
	axes.x = (state.left == state.right ? 128 : (state.left ? 0 : 255));
	axes.y = (state.up == state.down ? 128 : (state.up ? 0 : 255));
	return axes;
}

// Handles only A/D/W/S. Other keys return handled=false.
[[nodiscard]] inline KeyboardLeftStickUpdate ApplyKeyboardLeftStickKey(KeyboardLeftStickState& state, int key_code, bool down)
{
	bool* held = nullptr;
	switch (key_code)
	{
		case kKeyboardLeftStickKeyLeft: held = &state.left; break;
		case kKeyboardLeftStickKeyRight: held = &state.right; break;
		case kKeyboardLeftStickKeyUp: held = &state.up; break;
		case kKeyboardLeftStickKeyDown: held = &state.down; break;
		default: return {};
	}

	KeyboardLeftStickUpdate update {};
	update.handled = true;
	if (*held != down)
	{
		*held          = down;
		update.changed = true;
	}
	update.axes = KeyboardLeftStickAxesFromState(state);
	return update;
}

// Clears held directions; reports changed when any direction was held.
[[nodiscard]] inline KeyboardLeftStickUpdate ResetKeyboardLeftStick(KeyboardLeftStickState& state)
{
	const bool any_held = state.left || state.right || state.up || state.down;
	state               = {};
	KeyboardLeftStickUpdate update {};
	update.handled = true;
	update.changed = any_held;
	update.axes    = KeyboardLeftStickAxesFromState(state);
	return update;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_KEYBOARDINPUT_H_ */
