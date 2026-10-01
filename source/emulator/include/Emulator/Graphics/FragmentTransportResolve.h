#pragma once

#include "Kyty/Core/String8.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::FragmentTransport {

// The normal attachment pipeline applies these static component enables,
// blend and fixed-function depth/stencil. Guest depth exports need a separate
// output ABI and are not admitted by the fragment-compute compiler.
[[nodiscard]] String8 GenerateResolveSource(uint32_t descriptor_set, uint32_t color_write_mask);

} // namespace Kyty::Libs::Graphics::FragmentTransport

#endif
