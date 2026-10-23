#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERBUFFERFORMAT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERBUFFERFORMAT_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Number format of a buffer channel: how BUFFER_LOAD_FORMAT turns its bits
// into the 32-bit value it returns. Integer formats return integers, every
// other format returns a float (RDNA2 ISA 8.1.3).
enum class ShaderBufferNumber : uint8_t
{
	Unorm,
	Snorm,
	Uscaled,
	Sscaled,
	Uint,
	Sint,
	Float,
};

struct ShaderBufferChannel
{
	uint8_t offset = 0; // bit offset inside the element
	uint8_t bits   = 0;
};

// One Gen5 buffer data format of RDNA2 ISA table 47. Channel 0 (X) is the
// lowest-addressed channel; packed formats name channels from the high bits,
// so X is in the low bits.
struct ShaderBufferFormat
{
	uint8_t             format   = 0;
	uint8_t             bytes    = 0;
	uint8_t             channels = 0;
	ShaderBufferNumber  number   = ShaderBufferNumber::Float;
	ShaderBufferChannel layout[4] {};
};

// The decodable format, or nullptr for INVALID, image-only and unknown values.
[[nodiscard]] const ShaderBufferFormat* ShaderGen5BufferFormat(uint32_t format);
// Every decodable format in ascending order; `count` receives their number.
[[nodiscard]] const ShaderBufferFormat* ShaderGen5BufferFormats(uint32_t* count);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERBUFFERFORMAT_H_ */
