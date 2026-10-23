#include "Emulator/Host/HostVulkanWindow.h"

#include "Emulator/Host/HostWindow.h"

#include <SDL3/SDL_vulkan.h>

namespace Kyty::Emulator::Host {

bool HostVulkanWindow::GetInstanceExtensions(const HostWindow* window, std::vector<const char*>* extensions)
{
	if (window == nullptr || window->GetNativeHandle() == nullptr || extensions == nullptr)
	{
		return false;
	}

	Uint32             count = 0;
	const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
	if (names == nullptr || count == 0)
	{
		return false;
	}

	extensions->assign(names, names + count);
	return true;
}

bool HostVulkanWindow::CreateSurface(const HostWindow* window, VkInstance instance, VkSurfaceKHR* surface)
{
	if (window == nullptr || window->GetNativeHandle() == nullptr || instance == VK_NULL_HANDLE || surface == nullptr)
	{
		return false;
	}

	*surface = VK_NULL_HANDLE;
	return SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window->GetNativeHandle()), instance, nullptr, surface);
}

} // namespace Kyty::Emulator::Host
