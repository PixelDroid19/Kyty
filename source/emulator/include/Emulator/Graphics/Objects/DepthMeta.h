#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTH_META_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTH_META_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Observed HTILE clear pattern owner: exact-range pending clears only.
// Mark inserts a pending clear; ConsumeClear returns true once then removes it.
// No invented addresses; zero/partial ranges never match.

[[nodiscard]] bool DepthMetaIsClearPattern(const void* data, uint64_t size);
[[nodiscard]] bool DepthMetaMatchesStorageRange(uint64_t storage_address, uint64_t storage_size, uint64_t htile_address,
                                                uint64_t htile_size);
void               DepthMetaMarkClear(uint64_t address);
[[nodiscard]] bool DepthMetaConsumeClear(uint64_t address);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_DEPTH_META_H_
