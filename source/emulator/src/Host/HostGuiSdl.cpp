#include "Emulator/Host/HostGui.h"
#include "Emulator/Host/HostWindow.h"

#include <SDL3/SDL.h>

#include "imgui_impl_sdl3.h"

namespace Kyty::Emulator::Host {

bool HostGuiInit(const HostWindow* window)
{
	if (window == nullptr || window->GetNativeHandle() == nullptr)
	{
		return false;
	}
	return ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(window->GetNativeHandle()));
}

void HostGuiShutdown()
{
	ImGui_ImplSDL3_Shutdown();
}

void HostGuiProcessEvent(const void* native_event)
{
	if (native_event != nullptr)
	{
		ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(native_event));
	}
}

void HostGuiNewFrame()
{
	ImGui_ImplSDL3_NewFrame();
}

} // namespace Kyty::Emulator::Host
