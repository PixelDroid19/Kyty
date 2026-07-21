#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOLUTIONALIASPOLICY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOLUTIONALIASPOLICY_H_

#include "Emulator/Graphics/Objects/GpuMemory.h"

namespace Kyty::Libs::Graphics {

[[nodiscard]] bool ResolutionAliasPolicyAllowsSnapshot(const GpuMemoryOverlapSnapshot& overlaps, GpuMemoryObjectType expected_type,
                                                       bool allow_empty);

[[nodiscard]] bool ResolutionAliasPolicyAllowsRanges(const uint64_t* vaddr, const uint64_t* size, int count,
                                                     GpuMemoryObjectType expected_type, bool allow_empty);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOLUTIONALIASPOLICY_H_ */
