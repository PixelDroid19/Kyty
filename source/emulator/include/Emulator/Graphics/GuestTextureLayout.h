#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_TEXTURE_LAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_TEXTURE_LAYOUT_H_

#include "Kyty/Core/Common.h"

#include <cstddef>
#include <cstdint>

namespace Kyty::Core::VirtualMemory {
struct GuestMappingIdentitySnapshot;
}

namespace Kyty::Libs::Graphics {

inline constexpr size_t kGuestTextureLayoutCapacity = 4096u;

#ifdef KYTY_EMU_ENABLED

bool     GuestTextureLayoutRegisterLinear(uint64_t base, size_t size, uint32_t row_pitch_bytes,
                                          const Core::VirtualMemory::GuestMappingIdentitySnapshot& identity);
void     GuestTextureLayoutPruneStale();
void     GuestTextureLayoutUnregister(uint64_t base);
// Removes each complete layout that overlaps [address, address + size). Zero or overflowing ranges are ignored.
void     GuestTextureLayoutInvalidateRange(uint64_t address, uint64_t size);
uint32_t GuestTextureLayoutGetLinearRowPitch(uint64_t address, uint32_t visible_row_bytes);
uint64_t GuestTextureLayoutGetLinearFootprint(uint64_t address, uint32_t visible_row_bytes, uint32_t visible_height);

#endif // KYTY_EMU_ENABLED

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_TEXTURE_LAYOUT_H_ */
