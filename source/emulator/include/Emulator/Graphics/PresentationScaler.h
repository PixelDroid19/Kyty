#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONSCALER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONSCALER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

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

[[nodiscard]] PresentationScaleStatus PresentationScalerBlitFinalImage(CommandBuffer* command_buffer, const GraphicContext* context,
                                                                       VulkanImage* source, VulkanSwapchain* destination);
[[nodiscard]] const char*             PresentationScaleStatusName(PresentationScaleStatus status);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONSCALER_H_ */
