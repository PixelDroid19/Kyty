#include "Emulator/Libs/SaveDataCapacity.h"

#include "Emulator/AtomicFile.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/SaveDataPaths.h"

#include <charconv>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::SaveData {

namespace {

constexpr std::string_view kRecordPrefix   = "KYTYSD1,";
constexpr size_t           kRecordMaxBytes = 64u;

struct Allocation
{
	uint64_t blocks     = 0;
	uint64_t block_size = 0;
};

bool BlockSizeValid(uint64_t block_size)
{
	return block_size == kSaveDataLegacyBlockSize || block_size == kSaveDataNativeBlockSize;
}

// <save root>/.kyty-capacity/<title>/<slot>.alloc: beside the title roots, so
// directory searches never see it and deleting the payload cannot drop it early.
std::filesystem::path RecordPath(const std::filesystem::path& save_directory)
{
	if (!save_directory.is_absolute())
	{
		return {};
	}
	const auto slot  = save_directory.filename().u8string();
	const auto title = save_directory.parent_path().filename().u8string();
	if (!SaveDataDirectoryNameValid(slot.c_str()) || title.empty() || SaveDataNormalizeTitleId(title.c_str()) != title)
	{
		return {};
	}
	return save_directory.parent_path().parent_path() / ".kyty-capacity" / title / (slot + ".alloc");
}

bool ParseNumber(std::string_view* text, char delimiter, uint64_t* value)
{
	const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), *value);
	const auto consumed     = static_cast<size_t>(end - text->data());
	if (error != std::errc {} || consumed == 0 || consumed >= text->size() || text->at(consumed) != delimiter)
	{
		return false;
	}
	text->remove_prefix(consumed + 1u);
	return true;
}

bool ParseRecord(std::string_view text, Allocation* allocation)
{
	if (text.substr(0, kRecordPrefix.size()) != kRecordPrefix)
	{
		return false;
	}
	text.remove_prefix(kRecordPrefix.size());
	Allocation parsed {};
	if (!ParseNumber(&text, ',', &parsed.blocks) || !ParseNumber(&text, '\n', &parsed.block_size) || !text.empty())
	{
		return false;
	}
	if (parsed.blocks == 0 || !BlockSizeValid(parsed.block_size) || parsed.blocks > std::numeric_limits<uint64_t>::max() / parsed.block_size)
	{
		return false;
	}
	*allocation = parsed;
	return true;
}

// OK with *found = false when no record exists; BROKEN for an unreadable or
// malformed record.
int ReadRecord(const std::filesystem::path& path, Allocation* allocation, bool* found)
{
	*found = false;
	std::error_code error;
	if (!std::filesystem::exists(path, error))
	{
		return error ? SAVE_DATA_ERROR_INTERNAL : OK;
	}
	std::ifstream stream(path, std::ios::binary);
	std::string   content(kRecordMaxBytes + 1u, '\0');
	stream.read(content.data(), static_cast<std::streamsize>(content.size()));
	content.resize(static_cast<size_t>(stream.gcount()));
	if (!ParseRecord(content, allocation))
	{
		return SAVE_DATA_ERROR_BROKEN;
	}
	*found = true;
	return OK;
}

int CountUsedBlocks(const std::filesystem::path& save_directory, uint64_t block_size, uint64_t* used_blocks)
{
	uint64_t        total_bytes = 0;
	std::error_code error;
	for (std::filesystem::recursive_directory_iterator entry(save_directory, error), end; !error && entry != end; entry.increment(error))
	{
		if (!entry->is_regular_file(error))
		{
			continue;
		}
		total_bytes += entry->file_size(error);
	}
	if (error)
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	*used_blocks = total_bytes / block_size + (total_bytes % block_size == 0 ? 0u : 1u);
	return OK;
}

} // namespace

int SaveDataRecordAllocation(const std::filesystem::path& save_directory, uint64_t blocks, uint64_t block_size)
{
	if (blocks == 0)
	{
		return OK;
	}
	if (!BlockSizeValid(block_size) || blocks > std::numeric_limits<uint64_t>::max() / block_size)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	const auto path = RecordPath(save_directory);
	if (path.empty())
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	Allocation existing {};
	bool       found  = false;
	const int  result = ReadRecord(path, &existing, &found);
	if (result != OK || found)
	{
		return result;
	}
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	if (error)
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	const auto content = std::string(kRecordPrefix) + std::to_string(blocks) + "," + std::to_string(block_size) + "\n";
	return AtomicFileWrite(path, content.data(), content.size()) ? OK : SAVE_DATA_ERROR_INTERNAL;
}

int SaveDataQueryCapacity(const std::filesystem::path& save_directory, uint64_t default_block_size, SaveDataCapacity* capacity)
{
	if (capacity == nullptr || !BlockSizeValid(default_block_size))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	const auto path = RecordPath(save_directory);
	if (path.empty())
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	Allocation allocation {0, default_block_size};
	bool       found  = false;
	int        result = ReadRecord(path, &allocation, &found);
	if (result != OK)
	{
		return result;
	}
	uint64_t used_blocks = 0;
	result               = CountUsedBlocks(save_directory, allocation.block_size, &used_blocks);
	if (result != OK)
	{
		return result;
	}
	if (!found)
	{
		*capacity = {used_blocks, 0};
		return OK;
	}
	*capacity = {allocation.blocks, used_blocks >= allocation.blocks ? 0 : allocation.blocks - used_blocks};
	return OK;
}

int SaveDataRemoveAllocation(const std::filesystem::path& save_directory)
{
	const auto path = RecordPath(save_directory);
	if (path.empty())
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	std::error_code error;
	std::filesystem::remove(path, error);
	return error ? SAVE_DATA_ERROR_INTERNAL : OK;
}

} // namespace Kyty::Libs::SaveData

#endif // KYTY_EMU_ENABLED
