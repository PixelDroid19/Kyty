#pragma once

#include "Emulator/Graphics/Shader.h"

#include <array>

namespace Kyty::Libs::Graphics {

// Synthetic comparison through the real parser/recompiler. Instrumentation
// only seeds scalar status and exports observations; it never replaces guest
// comparison, bank, ID initialization, or ballot lowering.
[[nodiscard]] bool BuildWaveMaskProbeSource(uint32_t compare_opcode, uint32_t compare_value, const ShaderComputeInputInfo& input,
                                            const std::array<uint32_t, 2>* exec_seed, Kyty::Core::String8* source,
                                            Kyty::Core::String8* error, uint32_t destination = 106u, uint32_t source_vgpr = 0u);

} // namespace Kyty::Libs::Graphics
