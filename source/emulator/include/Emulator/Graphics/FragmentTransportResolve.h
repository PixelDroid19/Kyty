#pragma once

#include "Kyty/Core/String8.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::FragmentTransport {

// The normal attachment pipeline applies these static component enables,
// blend and fixed-function depth/stencil. A guest depth export travels in the
// last lane word and replaces the fragment depth before those tests. The
// kernel parameters start at parameter_offset in the push constant block.
[[nodiscard]] String8 GenerateResolveSource(uint32_t descriptor_set, uint32_t parameter_offset, uint32_t color_write_mask,
                                            bool depth_export);

} // namespace Kyty::Libs::Graphics::FragmentTransport

#endif
