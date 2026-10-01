#pragma once

#include "Kyty/Core/String8.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class ShaderCode;
struct ShaderPixelInputInfo;
struct ShaderFragmentComputeInfo;

namespace FragmentTransport {

// Uses the same canonical inputs as the guest compiler. Unavailable parameter
// forms fail admission instead of reconstructing values from nearby pixels.
[[nodiscard]] String8 GenerateCaptureSource(const ShaderCode& code, const ShaderPixelInputInfo& pixel,
                                            const ShaderFragmentComputeInfo& transport);

} // namespace FragmentTransport
} // namespace Kyty::Libs::Graphics

#endif
