#pragma once

#include "Kyty/Core/String8.h"

#include "Emulator/Common.h"

#include <array>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class ShaderCode;
struct ShaderPixelInputInfo;
struct ShaderFragmentComputeInfo;

namespace FragmentTransport {

// Lane record words one attribute needs, relative to the first attribute word.
// Interpolated parameters store the four components the hardware produced; a
// v_interp_mov source stores the three vertex values the instruction can name.
// Only the forms the program reads occupy lane storage.
struct LaneAttribute
{
	uint32_t parameter_offset = UINT32_MAX;
	uint32_t raw_offset       = UINT32_MAX;
};

struct LanePacking
{
	std::array<LaneAttribute, 32> attributes;
	uint32_t                      words = 0;
};

[[nodiscard]] LanePacking BuildLanePacking(const ShaderCode& code, const ShaderPixelInputInfo& pixel);

// Uses the same canonical inputs as the guest compiler. Unavailable parameter
// forms fail admission instead of reconstructing values from nearby pixels.
[[nodiscard]] String8 GenerateCaptureSource(const ShaderCode& code, const ShaderPixelInputInfo& pixel,
                                            const ShaderFragmentComputeInfo& transport);

} // namespace FragmentTransport
} // namespace Kyty::Libs::Graphics

#endif
