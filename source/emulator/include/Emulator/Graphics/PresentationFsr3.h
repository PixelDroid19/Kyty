#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONFSR3_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONFSR3_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct PresentationFsr3Stage;
struct RenderColorInfo;
struct RenderDepthInfo;
struct VulkanImage;

// True when the build links the FSR 3.1 upscaler.
[[nodiscard]] bool PresentationFsr3Available();

// Offers a render pass of the current frame as the source of the temporal guides.
void PresentationFsr3NoteRenderPass(const RenderColorInfo& color, const RenderDepthInfo& depth);

// Records source -> FSR 3.1 -> half-float image at the target extent and returns that image in TRANSFER_SRC_OPTIMAL.
// Returns nullptr when the frame has no temporal guides, and the caller presents it spatially.
[[nodiscard]] VulkanImage* PresentationFsr3Record(CommandBuffer* command_buffer, GraphicContext* context, VulkanImage* source,
                                                  VkExtent2D target_extent, uint32_t sharpness_percent, PresentationFsr3Stage** stage);
void                       PresentationFsr3Destroy(GraphicContext* context, PresentationFsr3Stage** stage);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATIONFSR3_H_ */
