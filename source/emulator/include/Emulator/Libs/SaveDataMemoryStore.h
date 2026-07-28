#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMEMORYSTORE_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMEMORYSTORE_H_

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::SaveData {

struct SaveDataMemoryWriteRange
{
	const void* source = nullptr;
	size_t      size   = 0;
	int64_t     offset = 0;
};

enum class SaveDataMemoryStoreResult
{
	Success,
	InvalidArgument,
	NotReady,
	CapacityExceeded,
	IoError,
};

// Owns the fixed-size SaveDataMemory blocks for one emulator process. The
// canonical backing path is the complete slot identity; callers cannot read or
// write a slot until Setup has loaded it.
class SaveDataMemoryStore final
{
public:
	[[nodiscard]] static constexpr size_t MaximumSlotBytes() { return 64u * 1024u * 1024u; }
	[[nodiscard]] static constexpr size_t MaximumWriteRanges() { return 1024u; }

	[[nodiscard]] SaveDataMemoryStoreResult Setup(const std::filesystem::path& path, size_t requested_size, size_t* existed_size);
	[[nodiscard]] SaveDataMemoryStoreResult Read(const std::filesystem::path& path, void* destination, size_t size, int64_t offset) const;
	[[nodiscard]] SaveDataMemoryStoreResult Write(const std::filesystem::path& path, const void* source, size_t size, int64_t offset);
	[[nodiscard]] SaveDataMemoryStoreResult WriteRanges(const std::filesystem::path& path, const SaveDataMemoryWriteRange* ranges,
	                                                    size_t range_count);
	[[nodiscard]] SaveDataMemoryStoreResult Sync(const std::filesystem::path& path) const;

private:
	using SlotMap = std::map<std::filesystem::path, std::vector<uint8_t>>;

	mutable std::mutex m_mutex;
	SlotMap            m_slots;
};

} // namespace Kyty::Libs::SaveData

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMEMORYSTORE_H_ */
