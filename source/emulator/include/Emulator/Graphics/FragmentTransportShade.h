#pragma once

#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Builds the split-Wave64 compute module that shades captured pixel records.
// The module reads ShaderFragmentComputeInfo's lane records and writes its
// 34-word result lane; the caller supplies the guest PS resource SGPR.
[[nodiscard]] bool FragmentTransportBuildShadeModule(const ShaderCode& code, const ShaderPixelInputInfo& pixel,
                                                     uint32_t user_sgpr_count, uint32_t lds_dwords, Vector<uint32_t>* binary,
                                                     String8* error);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
