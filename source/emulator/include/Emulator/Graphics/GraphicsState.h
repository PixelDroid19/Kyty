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

// Gen5 DB_STENCIL_INFO FORMAT is a 1-bit "stencil present" flag. When
// TILE_STENCIL_DISABLE is set and both stencil bases are null, there is no
// separate stencil plane — depth-only Vulkan target (D32), matching host paths
// that bind D32 without S8 for this ABI.
[[nodiscard]] inline uint32_t ResolveEffectiveStencilFormat(const HW::DepthRenderTarget& target)
{
	if (target.stencil_info.format == 0)
	{
		return 0;
	}
	if (target.stencil_read_base_addr != 0 || target.stencil_write_base_addr != 0)
	{
		return target.stencil_info.format;
	}
	if (target.stencil_info.tile_stencil_disable)
	{
		return 0;
	}
	return target.stencil_info.format;
}

void SetGenericScissorTl(HW::Context& context, uint32_t value);
void SetGenericScissorBr(HW::Context& context, uint32_t value);
// Gen5 CX-indirect emits screen scissor TL/BR as separate pairs.
void SetScreenScissorTl(HW::Context& context, uint32_t value);
void SetScreenScissorBr(HW::Context& context, uint32_t value);
void SetRenderControl(HW::Context& context, uint32_t value);
void SetStencilControl(HW::Context& context, uint32_t value);
void SetStencilRefMask(HW::Context& context, uint32_t value);
void SetStencilRefMaskBf(HW::Context& context, uint32_t value);
void SetModeControl(HW::Context& context, uint32_t value);
void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value);

// Guest top-left coordinates are inclusive, bottom-right coordinates are exclusive, and enabled rectangles intersect.
[[nodiscard]] ScissorRect ResolveScissor(const HW::ScreenViewport& viewport, const HW::ScanModeControl& mode, uint32_t viewport_id);
[[nodiscard]] DepthStencilUsage ResolveDepthStencilUsage(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control,
                                                         const HW::DepthControl& depth_control);

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
