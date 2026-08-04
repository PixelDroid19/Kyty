#include "Emulator/Host/HostVulkanWindow.h"

#include "Emulator/Host/HostWindow.h"

#include "SDL_vulkan.h"

namespace Kyty::Emulator::Host {

bool HostVulkanWindow::GetInstanceExtensions(const HostWindow* window, std::vector<const char*>* extensions)
{
	if (window == nullptr || window->GetNativeHandle() == nullptr || extensions == nullptr)
	{
		return false;
	}

	uint32_t count = 0;
	if (SDL_Vulkan_GetInstanceExtensions(static_cast<SDL_Window*>(window->GetNativeHandle()), &count, nullptr) == SDL_FALSE || count == 0)
	{
		return false;
	}

	extensions->assign(count, nullptr);
	if (SDL_Vulkan_GetInstanceExtensions(static_cast<SDL_Window*>(window->GetNativeHandle()), &count, extensions->data()) == SDL_FALSE ||
	    count != extensions->size())
	{
		extensions->clear();
		return false;
	}
	return true;
}

bool HostVulkanWindow::CreateSurface(const HostWindow* window, VkInstance instance, VkSurfaceKHR* surface)
{
	if (window == nullptr || window->GetNativeHandle() == nullptr || instance == VK_NULL_HANDLE || surface == nullptr)
	{
		return false;
	}

	*surface = VK_NULL_HANDLE;
	return SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window->GetNativeHandle()), instance, surface) == SDL_TRUE;
}

} // namespace Kyty::Emulator::Host
