#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_TEXTURE_LAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_TEXTURE_LAYOUT_H_

#include "Kyty/Core/Common.h"

#include <cstddef>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

void     GuestTextureLayoutRegisterLinear(uint64_t base, size_t size, uint32_t row_pitch_bytes);
void     GuestTextureLayoutUnregister(uint64_t base);
uint32_t GuestTextureLayoutGetLinearRowPitch(uint64_t address, uint32_t visible_row_bytes);
uint64_t GuestTextureLayoutGetLinearFootprint(uint64_t address, uint32_t visible_row_bytes, uint32_t visible_height);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_TEXTURE_LAYOUT_H_ */
