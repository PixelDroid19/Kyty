#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONSCALER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONSCALER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct VulkanImage;
struct VulkanSwapchain;

enum class PresentationScaleStatus : uint8_t
{
	Success,
	InvalidArgument,
	UnsupportedSourceFormat,
	UnsupportedDestinationFormat,
	UnsupportedFilter,
};

// A display buffer in an sRGB or float format holds linear light; a UNORM one already holds display-encoded values.
[[nodiscard]] bool     PresentationSourceHoldsLinearLight(VkFormat format);
// The sRGB format with the swapchain's byte layout, or VK_FORMAT_UNDEFINED when it has none.
[[nodiscard]] VkFormat PresentationSrgbTwin(VkFormat swapchain_format);
// A blit into a UNORM swapchain stores linear values unencoded (vkCmdBlitImage decodes sRGB on read and encodes only
// into sRGB destinations), which darkens the image. Linear-light sources therefore go through the sRGB twin first and
// reach the swapchain by a bit-exact copy; already-encoded sources and sRGB swapchains blit directly.
[[nodiscard]] bool     PresentationNeedsEncodeStage(VkFormat source_format, VkFormat swapchain_format);

[[nodiscard]] PresentationScaleStatus PresentationScalerBlitFinalImage(CommandBuffer* command_buffer, GraphicContext* context,
                                                                       VulkanImage* source, VulkanSwapchain* destination);
[[nodiscard]] const char*             PresentationScaleStatusName(PresentationScaleStatus status);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONSCALER_H_ */
