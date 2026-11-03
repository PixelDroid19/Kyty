#ifndef EMULATOR_INCLUDE_EMULATOR_ATOMICFILE_H_
#define EMULATOR_INCLUDE_EMULATOR_ATOMICFILE_H_

#include "Emulator/Common.h"

#include <cstddef>
#include <filesystem>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

// Publish one complete host file. Data is written to a process-unique sibling
// and then atomically replaces the destination, so readers never observe a
// partially written cache or save.
[[nodiscard]] bool AtomicFileWrite(const std::filesystem::path& destination, const void* data, size_t size);

// Removes the temporary siblings in `directory` whose writer process has
// exited: a writer killed before its replace leaves one behind for good.
void AtomicFileRemoveOrphans(const std::filesystem::path& directory);

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_ATOMICFILE_H_ */
