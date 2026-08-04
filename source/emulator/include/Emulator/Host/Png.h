#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_PNG_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_PNG_H_

#include <cstdint>

namespace Kyty::Emulator::Host {

// Write tightly or strided RGBA8 rows as a top-down PNG without depending on
// a graphics or window backend.
[[nodiscard]] bool WriteRgba8Png(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height,
                                 uint32_t row_pitch_pixels);

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_PNG_H_ */
