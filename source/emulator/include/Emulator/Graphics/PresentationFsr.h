#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONFSR_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONFSR_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct VulkanImage;
struct PresentationFsrStage;

// FSR 1.0 is an upscaler: it runs only when the source fits inside the target and is smaller on at least one axis.
[[nodiscard]] bool  PresentationFsrApplies(VkExtent2D source, VkExtent2D target);
// PresentationFsrSharpness is a percentage; RCAS takes attenuation stops, 0 being the sharpest.
[[nodiscard]] float PresentationFsrSharpnessStops(uint32_t percent);

// Records source -> EASU -> RCAS -> target. The target ends in TRANSFER_DST_OPTIMAL like a blit destination. The stage
// is created or resized on demand; its resources must be idle, which holds because every present waits for its fence.
// Returns false when the device cannot run the passes.
[[nodiscard]] bool PresentationFsrRecord(CommandBuffer* command_buffer, GraphicContext* context, VulkanImage* source, VkImage target,
                                         VkExtent2D target_extent, VkFormat target_format, uint32_t sharpness_percent,
                                         PresentationFsrStage** stage);
void               PresentationFsrDestroy(GraphicContext* context, PresentationFsrStage** stage);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONFSR_H_ */
