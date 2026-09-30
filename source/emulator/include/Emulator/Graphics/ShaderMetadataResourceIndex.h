#pragma once

#include <cstdint>

namespace Kyty::Libs::Graphics {

struct ShaderBindResources;

// Only host-assigned image/sampler indices are immutable. Guest descriptor
// fields, direct scalars and device-address metadata retain runtime loads.
[[nodiscard]] bool ShaderKnownMetadataResourceIndex(const ShaderBindResources& bind, bool next_gen, int row, int field,
                                                    uint32_t* value) noexcept;

} // namespace Kyty::Libs::Graphics
