#include "Emulator/Host/HostGui.h"
#include "Emulator/Host/HostWindow.h"

#include "SDL.h"
#include "SDL_events.h"

#include "imgui_impl_sdl2.h"

namespace Kyty::Emulator::Host {

bool HostGuiInit(const HostWindow* window)
{
	if (window == nullptr || window->GetNativeHandle() == nullptr)
	{
		return false;
	}
	return ImGui_ImplSDL2_InitForVulkan(static_cast<SDL_Window*>(window->GetNativeHandle()));
}

void HostGuiShutdown()
{
	ImGui_ImplSDL2_Shutdown();
}

void HostGuiProcessEvent(const void* native_event)
{
	if (native_event != nullptr)
	{
		ImGui_ImplSDL2_ProcessEvent(static_cast<const SDL_Event*>(native_event));
	}
}

void HostGuiNewFrame()
{
	ImGui_ImplSDL2_NewFrame();
}

} // namespace Kyty::Emulator::Host
