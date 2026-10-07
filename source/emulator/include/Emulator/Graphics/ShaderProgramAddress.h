#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERPROGRAMADDRESS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERPROGRAMADDRESS_H_

#include <cstdint>

namespace Kyty::Libs::Graphics {

struct ShaderBindResources;

// Writes the aligned runtime address block in the shared push/UBO metadata.
// Rejects an invalid layout or insufficient capacity without partial writes.
[[nodiscard]] bool ShaderWriteProgramBaseMetadata(const ShaderBindResources& bind, uint32_t* metadata, uint32_t capacity_dw);

} // namespace Kyty::Libs::Graphics

#endif
