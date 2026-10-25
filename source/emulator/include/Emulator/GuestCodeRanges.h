#ifndef EMULATOR_INCLUDE_EMULATOR_GUEST_CODE_RANGES_H_
#define EMULATOR_INCLUDE_EMULATOR_GUEST_CODE_RANGES_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

// Executable guest memory: segments of the loaded programs and mappings the kernel
// makes executable. Add and Remove take a lock; Contains() takes none and is
// async-signal-safe, so a signal handler can tell whether an interrupted
// instruction is guest code rather than host code.
namespace Kyty::Emulator::GuestCodeRanges {

void               Add(uint64_t begin, uint64_t end) noexcept;
void               Remove(uint64_t begin, uint64_t end) noexcept;
[[nodiscard]] bool Contains(uint64_t address) noexcept;

} // namespace Kyty::Emulator::GuestCodeRanges

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GUEST_CODE_RANGES_H_ */
