#include "Emulator/Libs/SaveDataMemoryStore.h"

#include "Emulator/AtomicFile.h"

#include <cstring>
#include <fstream>
#include <system_error>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::SaveData {

namespace {

std::filesystem::path NormalizeBackingPath(const std::filesystem::path& path)
{
	return path.empty() || !path.is_absolute() ? std::filesystem::path() : path.lexically_normal();
}

bool RangeValid(size_t slot_size, const void* buffer, size_t size, int64_t offset)
{
	if (offset < 0 || (buffer == nullptr && size != 0))
	{
		return false;
	}
	const auto start = static_cast<size_t>(offset);
	return start <= slot_size && size <= slot_size - start;
}

} // namespace

SaveDataMemoryStoreResult SaveDataMemoryStore::Setup(const std::filesystem::path& path, size_t requested_size, size_t* existed_size)
{
	const auto normalized = NormalizeBackingPath(path);
	if (normalized.empty() || existed_size == nullptr)
	{
		return SaveDataMemoryStoreResult::InvalidArgument;
	}
	if (requested_size > MaximumSlotBytes())
	{
		return SaveDataMemoryStoreResult::CapacityExceeded;
	}

	std::lock_guard lock(m_mutex);
	if (auto ready = m_slots.find(normalized); ready != m_slots.end())
	{
		*existed_size = ready->second.size();
		if (requested_size > ready->second.size())
		{
			ready->second.resize(requested_size, 0);
		}
		return SaveDataMemoryStoreResult::Success;
	}

	std::vector<uint8_t> bytes;
	std::error_code      error;
	const bool           exists = std::filesystem::exists(normalized, error);
	if (error)
	{
		return SaveDataMemoryStoreResult::IoError;
	}
	if (exists)
	{
		if (!std::filesystem::is_regular_file(normalized, error) || error)
		{
			return SaveDataMemoryStoreResult::IoError;
		}
		const auto file_size = std::filesystem::file_size(normalized, error);
		if (error)
		{
			return SaveDataMemoryStoreResult::IoError;
		}
		if (file_size > MaximumSlotBytes())
		{
			return SaveDataMemoryStoreResult::CapacityExceeded;
		}
		bytes.resize(static_cast<size_t>(file_size));
		if (!bytes.empty())
		{
			std::ifstream file(normalized, std::ios::binary);
			if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
			{
				return SaveDataMemoryStoreResult::IoError;
			}
		}
	}

	*existed_size = bytes.size();
	if (requested_size > bytes.size())
	{
		bytes.resize(requested_size, 0);
	}
	m_slots.emplace(normalized, std::move(bytes));
	return SaveDataMemoryStoreResult::Success;
}

SaveDataMemoryStoreResult SaveDataMemoryStore::Read(const std::filesystem::path& path, void* destination, size_t size, int64_t offset) const
{
	const auto normalized = NormalizeBackingPath(path);
	if (normalized.empty())
	{
		return SaveDataMemoryStoreResult::InvalidArgument;
	}

	std::lock_guard lock(m_mutex);
	const auto      ready = m_slots.find(normalized);
	if (ready == m_slots.end())
	{
		return SaveDataMemoryStoreResult::NotReady;
	}
	if (!RangeValid(ready->second.size(), destination, size, offset))
	{
		return SaveDataMemoryStoreResult::InvalidArgument;
	}
	if (size != 0)
	{
		std::memcpy(destination, ready->second.data() + static_cast<size_t>(offset), size);
	}
	return SaveDataMemoryStoreResult::Success;
}

SaveDataMemoryStoreResult SaveDataMemoryStore::Write(const std::filesystem::path& path, const void* source, size_t size, int64_t offset)
{
	const SaveDataMemoryWriteRange range {source, size, offset};
	return WriteRanges(path, &range, 1);
}

SaveDataMemoryStoreResult SaveDataMemoryStore::WriteRanges(const std::filesystem::path& path, const SaveDataMemoryWriteRange* ranges,
                                                           size_t range_count)
{
	const auto normalized = NormalizeBackingPath(path);
	if (normalized.empty() || (ranges == nullptr && range_count != 0))
	{
		return SaveDataMemoryStoreResult::InvalidArgument;
	}

	std::lock_guard lock(m_mutex);
	auto            ready = m_slots.find(normalized);
	if (ready == m_slots.end())
	{
		return SaveDataMemoryStoreResult::NotReady;
	}
	for (size_t index = 0; index < range_count; ++index)
	{
		if (!RangeValid(ready->second.size(), ranges[index].source, ranges[index].size, ranges[index].offset))
		{
			return SaveDataMemoryStoreResult::InvalidArgument;
		}
	}
	for (size_t index = 0; index < range_count; ++index)
	{
		if (ranges[index].size != 0)
		{
			std::memcpy(ready->second.data() + static_cast<size_t>(ranges[index].offset), ranges[index].source, ranges[index].size);
		}
	}
	return SaveDataMemoryStoreResult::Success;
}

SaveDataMemoryStoreResult SaveDataMemoryStore::Sync(const std::filesystem::path& path) const
{
	const auto normalized = NormalizeBackingPath(path);
	if (normalized.empty())
	{
		return SaveDataMemoryStoreResult::InvalidArgument;
	}

	std::lock_guard lock(m_mutex);
	const auto      ready = m_slots.find(normalized);
	if (ready == m_slots.end())
	{
		return SaveDataMemoryStoreResult::NotReady;
	}
	return AtomicFileWrite(normalized, ready->second.data(), ready->second.size()) ? SaveDataMemoryStoreResult::Success
	                                                                               : SaveDataMemoryStoreResult::IoError;
}

} // namespace Kyty::Libs::SaveData

#endif
