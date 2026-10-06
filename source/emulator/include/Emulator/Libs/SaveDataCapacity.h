#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATACAPACITY_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATACAPACITY_H_

#include "Emulator/Common.h"

#include <cstdint>
#include <filesystem>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::SaveData {

// Allocation units: the standard mount calls count 32 KiB blocks, the native
// mount call 64 KiB blocks (the unit its search results are multiplied by).
constexpr uint64_t kSaveDataLegacyBlockSize = 32u * 1024u;
constexpr uint64_t kSaveDataNativeBlockSize = 64u * 1024u;

struct SaveDataCapacity
{
	uint64_t blocks      = 0;
	uint64_t free_blocks = 0;
};

// Record the allocation a mount requested for a save directory. An existing
// record is kept, and a request of zero blocks records nothing. The record is
// stored beside the title roots, outside the guest-visible payload.
[[nodiscard]] int SaveDataRecordAllocation(const std::filesystem::path& save_directory, uint64_t blocks, uint64_t block_size);

// Report a save directory's allocation and the blocks its payload leaves free.
// A directory without a record (created before allocations were recorded)
// reports its current usage, in default_block_size units, as its allocation.
[[nodiscard]] int SaveDataQueryCapacity(const std::filesystem::path& save_directory, uint64_t default_block_size,
                                        SaveDataCapacity* capacity);

// Drop the allocation record of a deleted save directory.
[[nodiscard]] int SaveDataRemoveAllocation(const std::filesystem::path& save_directory);

} // namespace Kyty::Libs::SaveData

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATACAPACITY_H_ */
