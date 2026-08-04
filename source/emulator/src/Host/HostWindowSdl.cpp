#include "Emulator/Host/HostWindow.h"

#include "Kyty/Core/DbgAssert.h"

#include "SDL.h"
#include "SDL_error.h"
#include "SDL_surface.h"
#include "SDL_video.h"

#include <chrono>
#include <climits>
#include <cstdio>

namespace Kyty::Emulator::Host {

namespace {

constexpr const char* kWindowCaption = "Game";
constexpr uint32_t    kWindowFlags = static_cast<uint32_t>(SDL_WINDOW_HIDDEN) | static_cast<uint32_t>(SDL_WINDOW_VULKAN);
constexpr int         kWindowPositionCentered = SDL_WINDOWPOS_CENTERED; // NOLINT(hicpp-signed-bitwise)
constexpr uint64_t    kCursorHideDelayMilliseconds = 2000;

[[nodiscard]] uint64_t SteadyMilliseconds()
{
	using clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

} // namespace

HostWindow::HostWindow(void* native_window, uint32_t initialized_subsystems): m_window(native_window), m_initialized_subsystems(initialized_subsystems)
{
	auto* window = static_cast<SDL_Window*>(m_window);
	EXIT_IF(window == nullptr);

	const uint32_t flags = SDL_GetWindowFlags(window);
	m_fullscreen         = (flags & SDL_WINDOW_FULLSCREEN) != 0u;
	m_minimized          = (flags & SDL_WINDOW_MINIMIZED) != 0u;
	m_focused            = (flags & SDL_WINDOW_INPUT_FOCUS) != 0u;
}

HostWindow::~HostWindow()
{
	if (m_window != nullptr)
	{
		SDL_DestroyWindow(static_cast<SDL_Window*>(m_window));
		m_window = nullptr;
	}

	if (m_initialized_subsystems != 0)
	{
		SDL_QuitSubSystem(m_initialized_subsystems);
	}
}

HostWindow* HostWindow::Create(uint32_t width, uint32_t height)
{
	EXIT_IF(width == 0 || height == 0 || width > static_cast<uint32_t>(INT_MAX) || height > static_cast<uint32_t>(INT_MAX));

	constexpr uint32_t requested_subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
	const uint32_t     previously_initialized = SDL_WasInit(requested_subsystems);

	if (SDL_InitSubSystem(requested_subsystems) < 0)
	{
		EXIT("%s\n", SDL_GetError());
	}
	const uint32_t initialized_subsystems = SDL_WasInit(requested_subsystems) & ~previously_initialized;

	std::printf("WindowCreate(): width = %d, height = %d\n", static_cast<int>(width), static_cast<int>(height));

	auto* window = SDL_CreateWindow(kWindowCaption, kWindowPositionCentered, kWindowPositionCentered, static_cast<int>(width),
	                                static_cast<int>(height), kWindowFlags);
	if (window == nullptr)
	{
		if (initialized_subsystems != 0)
		{
			SDL_QuitSubSystem(initialized_subsystems);
		}
		EXIT("%s\n", SDL_GetError());
	}

	SDL_SetWindowResizable(window, SDL_TRUE);
	return new HostWindow(window, initialized_subsystems);
}

void* HostWindow::GetNativeHandle() const
{
	return m_window;
}

void HostWindow::SetIcon(void* native_surface)
{
	EXIT_IF(m_window == nullptr || native_surface == nullptr);
	SDL_SetWindowIcon(static_cast<SDL_Window*>(m_window), static_cast<SDL_Surface*>(native_surface));
}

void HostWindow::SetTitle(const char* title)
{
	EXIT_IF(m_window == nullptr || title == nullptr);
	SDL_SetWindowTitle(static_cast<SDL_Window*>(m_window), title);
}

bool HostWindow::ShowAndPumpEvents()
{
	EXIT_IF(m_window == nullptr);
	if (!m_hidden)
	{
		return false;
	}

	SDL_ShowWindow(static_cast<SDL_Window*>(m_window));
	SDL_PumpEvents();
	m_hidden = false;
	return true;
}

bool HostWindow::IsHidden() const
{
	return m_hidden;
}

bool HostWindow::IsFullscreen() const
{
	return m_fullscreen;
}

bool HostWindow::IsMinimized() const
{
	return m_minimized;
}

bool HostWindow::IsFocused() const
{
	return m_focused;
}

void HostWindow::SetMinimized(bool minimized)
{
	m_minimized = minimized;
}

void HostWindow::SetFocused(bool focused)
{
	m_focused = focused;
}

void HostWindow::SetCursorVisible(bool visible)
{
	if (m_window == nullptr || m_cursor_visible == visible)
	{
		return;
	}

	SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
	m_cursor_visible = visible;
}

void HostWindow::UpdateCursorPolicy(bool temporary_visibility)
{
	if (m_window == nullptr)
	{
		return;
	}
	if (!m_fullscreen)
	{
		m_cursor_hide_deadline_ms = 0;
		SetCursorVisible(true);
		return;
	}
	if (temporary_visibility)
	{
		SetCursorVisible(true);
		m_cursor_hide_deadline_ms = SteadyMilliseconds() + kCursorHideDelayMilliseconds;
		return;
	}
	if (m_cursor_hide_deadline_ms == 0 || SteadyMilliseconds() >= m_cursor_hide_deadline_ms)
	{
		m_cursor_hide_deadline_ms = 0;
		SetCursorVisible(false);
	}
}

bool HostWindow::ToggleFullscreen()
{
	if (m_window == nullptr)
	{
		return false;
	}

	const bool     target = !m_fullscreen;
	const uint32_t flags  = target ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u;
	if (SDL_SetWindowFullscreen(static_cast<SDL_Window*>(m_window), flags) != 0)
	{
		std::fprintf(stderr, "Kyty window fullscreen toggle failed: %s\n", SDL_GetError());
		return false;
	}

	m_fullscreen = target;
	UpdateCursorPolicy();
	return true;
}

} // namespace Kyty::Emulator::Host
