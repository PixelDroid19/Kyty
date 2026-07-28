#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOWCONTROLS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOWCONTROLS_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

enum class HostWindowCommand
{
	None,
	Quit,
	TogglePause,
	ToggleFullscreen,
};

// SDL-independent key facts used by the host-window command policy.
struct HostWindowKey
{
	bool pressed = false;
	bool repeat  = false;
	bool escape  = false;
	bool pause   = false;
	bool f11     = false;
	bool enter   = false;
	bool alt     = false;
};

// Owns host shortcuts and prevents an Alt+Enter chord from leaking a partial
// Enter edge into the guest controller stream when focus changes mid-chord.
class HostWindowControls final
{
public:
	[[nodiscard]] constexpr HostWindowCommand HandleKey(const HostWindowKey& key)
	{
		if (key.enter && !key.pressed)
		{
			m_suppress_guest_enter = false;
		}
		if (key.enter && key.pressed && key.alt)
		{
			m_suppress_guest_enter = true;
		}
		if (!key.pressed || key.repeat)
		{
			return HostWindowCommand::None;
		}
		if (key.escape)
		{
			return HostWindowCommand::Quit;
		}
		if (key.pause)
		{
			return HostWindowCommand::TogglePause;
		}
		if (key.f11 || (key.enter && key.alt))
		{
			return HostWindowCommand::ToggleFullscreen;
		}
		return HostWindowCommand::None;
	}

	[[nodiscard]] constexpr bool GuestEnterAllowed() const { return !m_suppress_guest_enter; }

	constexpr void SetFocused(bool focused) { m_focused = focused; }

	constexpr void ReconcileEnter(bool enter_down)
	{
		if (m_focused && !enter_down)
		{
			m_suppress_guest_enter = false;
		}
	}

	[[nodiscard]] static constexpr HostWindowCommand HandlePrimaryClick(bool primary, bool pressed, uint8_t clicks)
	{
		return primary && pressed && clicks == 2 ? HostWindowCommand::ToggleFullscreen : HostWindowCommand::None;
	}

private:
	bool m_suppress_guest_enter = false;
	bool m_focused              = true;
};

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOWCONTROLS_H_ */
