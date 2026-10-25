#ifndef EMULATOR_SRC_GRAPHICS_SHADERSPIRVSTORAGEWRITEMASK_H_
#define EMULATOR_SRC_GRAPHICS_SHADERSPIRVSTORAGEWRITEMASK_H_

#include "Kyty/Core/String8.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Words per write-mask page: one flag covers 4 KiB, the write-back page size.
inline constexpr uint32_t kStorageWriteMaskPageShift = 10u;

// Rewrites SPIR-V function text so that every store or atomic through a
// storage element pointer (%buf or %buf_uint, indexed [buffer, member, word])
// first sets the flag of that word's page in %buf_write_mask. `writes` counts
// the marked accesses. Returns false when a storage pointer has any other use
// (copy, phi, call argument, partial-chain store), which the mask cannot cover.
[[nodiscard]] bool SpirvMarkStorageWrites(const String8& body, String8* marked, uint32_t* writes);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERSPIRVSTORAGEWRITEMASK_H_ */
