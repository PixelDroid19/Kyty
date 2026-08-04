#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_HOSTWINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_HOSTWINDOW_H_

#include "Kyty/Core/Common.h"

#include <cstdint>

namespace Kyty::Emulator::Host {

// Owns the native host window and its SDL lifecycle. The native handle stays
// opaque so Graphics can use host-window behavior without depending on SDL's
// window types.
class HostWindow final
{
public:
	static HostWindow* Create(uint32_t width, uint32_t height);
	~HostWindow();

	[[nodiscard]] void* GetNativeHandle() const;

	void SetIcon(void* native_surface);
	void SetTitle(const char* title);
	[[nodiscard]] bool ShowAndPumpEvents();

	[[nodiscard]] bool IsHidden() const;
	[[nodiscard]] bool IsFullscreen() const;
	[[nodiscard]] bool IsMinimized() const;
	[[nodiscard]] bool IsFocused() const;
	void               SetMinimized(bool minimized);
	void               SetFocused(bool focused);

	void               SetCursorVisible(bool visible);
	void               UpdateCursorPolicy(bool temporary_visibility = false);
	[[nodiscard]] bool ToggleFullscreen();

	KYTY_CLASS_NO_COPY(HostWindow);

private:
	explicit HostWindow(void* native_window, uint32_t initialized_subsystems);

	void*    m_window                  = nullptr;
	uint32_t m_initialized_subsystems = 0;
	bool     m_hidden                  = true;
	bool     m_fullscreen              = false;
	bool     m_minimized               = false;
	bool     m_focused                 = false;
	bool     m_cursor_visible          = true;
	uint64_t m_cursor_hide_deadline_ms = 0;
};

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_HOSTWINDOW_H_ */
