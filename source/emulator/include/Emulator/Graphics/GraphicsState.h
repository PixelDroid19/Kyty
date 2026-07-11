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

void SetGenericScissorTl(HW::Context& context, uint32_t value);
void SetGenericScissorBr(HW::Context& context, uint32_t value);
void SetModeControl(HW::Context& context, uint32_t value);
void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value);

// Guest top-left coordinates are inclusive, bottom-right coordinates are exclusive, and enabled rectangles intersect.
[[nodiscard]] ScissorRect ResolveScissor(const HW::ScreenViewport& viewport, const HW::ScanModeControl& mode, uint32_t viewport_id);

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
