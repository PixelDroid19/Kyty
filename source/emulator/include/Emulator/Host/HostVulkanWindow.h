#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_HOSTVULKANWINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_HOSTVULKANWINDOW_H_

#include <vector>
#include <vulkan/vulkan_core.h>

namespace Kyty::Emulator::Host {

class HostWindow;

// Vulkan is an optional host backend. Keep its surface/extension contract in
// this adapter so the generic HostWindow does not expose Vulkan types to the
// rest of the emulator.
class HostVulkanWindow final
{
public:
	[[nodiscard]] static bool GetInstanceExtensions(const HostWindow* window, std::vector<const char*>* extensions);
	[[nodiscard]] static bool CreateSurface(const HostWindow* window, VkInstance instance, VkSurfaceKHR* surface);
};

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_HOSTVULKANWINDOW_H_ */
