#ifndef EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
#define EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED

#include "Emulator/Graphics/HardwareContext.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::State {

void SetGenericScissorTl(HW::Context& context, uint32_t value);
void SetGenericScissorBr(HW::Context& context, uint32_t value);
void SetModeControl(HW::Context& context, uint32_t value);
void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value);

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
