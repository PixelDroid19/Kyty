#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GDSRANGE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GDSRANGE_H_

#include <cstdint>

namespace Kyty::Libs::Graphics {

// Guest-addressable GDS window in dwords (48 KiB). This is not the 64 kB physical
// capacity of the AMD global data share; raising it needs a strict trace that
// addresses above the window, not a capacity argument.
constexpr uint64_t kGraphicsGdsDwords = 0x3000;

// Every check is evaluated on the full 64-bit values before any narrowing, so an
// overflowing span can never wrap into a valid one.
[[nodiscard]] constexpr bool GraphicsGdsDwordRangeValid(uint64_t dw_offset, uint64_t dw_count) noexcept
{
	return dw_count <= kGraphicsGdsDwords && dw_offset <= kGraphicsGdsDwords - dw_count;
}

[[nodiscard]] constexpr bool GraphicsGdsByteRangeValid(uint64_t byte_offset, uint64_t byte_count) noexcept
{
	return (byte_offset & 3u) == 0u && (byte_count & 3u) == 0u && GraphicsGdsDwordRangeValid(byte_offset / 4u, byte_count / 4u);
}

// EVENT_WRITE_EOP with GDS source packs {count[31:16], offset[15:0]} in dwords.
[[nodiscard]] constexpr bool GraphicsGdsEopValueRangeValid(uint32_t value) noexcept
{
	return GraphicsGdsDwordRangeValid(value & 0xffffu, value >> 16u);
}

// Two validated spans overlap when both are non-empty and intersect. A
// same-buffer GPU copy requires non-overlapping regions.
[[nodiscard]] constexpr bool GraphicsGdsDwordRangesOverlap(uint64_t first_offset, uint64_t second_offset, uint64_t dw_count) noexcept
{
	return dw_count != 0 && first_offset < second_offset + dw_count && second_offset < first_offset + dw_count;
}

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GDSRANGE_H_ */
