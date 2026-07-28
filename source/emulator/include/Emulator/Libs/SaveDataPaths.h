#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAPATHS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAPATHS_H_

#include "Emulator/Common.h"

#include <cstdint>
#include <filesystem>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::SaveData {

// Convert a guest title identifier into one portable host path segment.
[[nodiscard]] std::string SaveDataNormalizeTitleId(const char* title_id);

// Validate a guest save-slot name without rewriting it. Invalid or nested host
// path syntax is rejected at the HLE boundary instead of being redirected.
[[nodiscard]] bool SaveDataDirectoryNameValid(const char* directory_name);

// Build the canonical per-title save root below an already-resolved user root.
[[nodiscard]] std::filesystem::path SaveDataBuildTitleRoot(const std::filesystem::path& save_data_root, const char* title_id);

// Build the canonical persistent path for one SaveDataMemory identity. Invalid
// roots and guest user identifiers are rejected instead of being redirected.
[[nodiscard]] std::filesystem::path SaveDataBuildMemoryPath(const std::filesystem::path& title_root, int32_t user_id, uint32_t slot_id);

} // namespace Kyty::Libs::SaveData

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAPATHS_H_ */
