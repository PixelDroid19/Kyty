#ifndef EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
#define EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED

#include "Emulator/Graphics/HardwareContext.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::State {

struct ScissorRect
{
	int left   = 0;
	int top    = 0;
	int right  = 0;
	int bottom = 0;
};

struct DepthStencilUsage
{
	bool target_active      = false;
	bool depth_write_enable = false;
};

struct ViewportDepthRange
{
	float min_depth = 0.0f;
	float max_depth = 1.0f;
};

void SetGenericScissorTl(HW::Context& context, uint32_t value);
void SetGenericScissorBr(HW::Context& context, uint32_t value);
void SetModeControl(HW::Context& context, uint32_t value);
void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value);

// Guest top-left coordinates are inclusive, bottom-right coordinates are exclusive, and enabled rectangles intersect.
[[nodiscard]] ScissorRect ResolveScissor(const HW::ScreenViewport& viewport, const HW::ScanModeControl& mode, uint32_t viewport_id);
[[nodiscard]] DepthStencilUsage ResolveDepthStencilUsage(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control,
                                                         const HW::DepthControl& depth_control);

// AMD VTE window Z: OpenGL clip ([-W,+W]) uses zoffset±zscale; DX clip ([0,+W]) uses [zoffset, zoffset+zscale].
// Without VK_EXT_depth_range_unrestricted, clamp to [0,1] and pair with negativeOneToOne for OpenGL clip.
[[nodiscard]] ViewportDepthRange ResolveViewportDepth(float zscale, float zoffset, bool dx_clip_space, bool depth_range_unrestricted);

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
