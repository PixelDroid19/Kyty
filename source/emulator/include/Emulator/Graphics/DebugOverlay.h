#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEBUGOVERLAY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEBUGOVERLAY_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <vulkan/vulkan_core.h>

struct SDL_Window;
union SDL_Event;

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct GraphicContext;
struct VulkanSwapchain;

// In-window diagnostic HUD (ImGui). Read-only; never changes guest GPU state.
void DebugOverlayInit(SDL_Window* window, GraphicContext* ctx, VulkanSwapchain* swapchain);
void DebugOverlayShutdown(GraphicContext* ctx);
void DebugOverlayOnSwapchainRecreated(GraphicContext* ctx, VulkanSwapchain* swapchain);
void DebugOverlayProcessEvent(const SDL_Event* event);
void DebugOverlayToggle();
[[nodiscard]] bool DebugOverlayIsVisible();

// Record ImGui draw commands into the present command buffer after the blit and
// before the PRESENT_SRC barrier. Returns true when the swapchain image was left
// in COLOR_ATTACHMENT_OPTIMAL (caller must barrier from that); false when the
// image remains TRANSFER_DST_OPTIMAL.
[[nodiscard]] bool DebugOverlayRecord(GraphicContext* ctx, VulkanSwapchain* swapchain, VkCommandBuffer cmd, double now_seconds, double fps,
                                      double frame_time_ms);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEBUGOVERLAY_H_ */
