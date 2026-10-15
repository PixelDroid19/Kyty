#include "Emulator/Libs/SaveDataPaths.h"

#include <cctype>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::SaveData {

std::string SaveDataNormalizeTitleId(const char* title_id)
{
	std::string result;
	if (title_id == nullptr || title_id[0] == '\0')
	{
		return "UNKNOWN";
	}

	if (title_id != nullptr)
	{
		for (const char* cursor = title_id; *cursor != '\0' && result.size() < 64; ++cursor)
		{
			const auto value = static_cast<unsigned char>(*cursor);
			if (std::isalnum(value) != 0)
			{
				result.push_back(static_cast<char>(std::toupper(value)));
			} else if (*cursor == '-' || *cursor == '_')
			{
				result.push_back(*cursor);
			} else
			{
				result.push_back('_');
			}
		}
	}
	return result;
}

bool SaveDataDirectoryNameValid(const char* directory_name)
{
	if (directory_name == nullptr)
	{
		return false;
	}
	const size_t length = std::strlen(directory_name);
	if (length == 0 || length > 31 || std::strcmp(directory_name, ".") == 0 || std::strcmp(directory_name, "..") == 0 ||
	    directory_name[length - 1] == ' ' || directory_name[length - 1] == '.')
	{
		return false;
	}
	for (size_t index = 0; index < length; ++index)
	{
		const auto value = static_cast<unsigned char>(directory_name[index]);
		if (value < 0x20 || value >= 0x7f || std::strchr("/\\<>:\"|?*", directory_name[index]) != nullptr)
		{
			return false;
		}
	}
	return true;
}

bool SaveDataDirectoryNameMatches(const char* directory_name, const char* pattern)
{
	if (directory_name == nullptr || pattern == nullptr)
	{
		return false;
	}
	// Greedy scan that backtracks to the last '%': it extends that run by one
	// character whenever the rest of the pattern fails to match.
	const char* name_resume    = nullptr;
	const char* pattern_resume = nullptr;
	while (*directory_name != '\0')
	{
		if (*pattern == '%')
		{
			pattern_resume = ++pattern;
			name_resume    = directory_name;
			continue;
		}
		if (*pattern == '_' || *pattern == *directory_name)
		{
			++pattern;
			++directory_name;
			continue;
		}
		if (pattern_resume == nullptr)
		{
			return false;
		}
		pattern        = pattern_resume;
		directory_name = ++name_resume;
	}
	while (*pattern == '%')
	{
		++pattern;
	}
	return *pattern == '\0';
}

std::filesystem::path SaveDataBuildTitleRoot(const std::filesystem::path& save_data_root, const char* title_id)
{
	const auto normalized_title_id = SaveDataNormalizeTitleId(title_id);
	if (save_data_root.empty() || normalized_title_id.empty())
	{
		return {};
	}
	return save_data_root / normalized_title_id;
}

std::filesystem::path SaveDataBuildMemoryRoot(const std::filesystem::path& title_root)
{
	if (title_root.empty() || !title_root.is_absolute())
	{
		return {};
	}
	const auto normalized_title_root = title_root.lexically_normal();
	const auto title_name            = normalized_title_root.filename().u8string();
	if (normalized_title_root.parent_path().empty() || title_name.empty() || SaveDataNormalizeTitleId(title_name.c_str()) != title_name)
	{
		return {};
	}
	return normalized_title_root.parent_path() / ".kyty-memory" / title_name / "memory";
}

std::filesystem::path SaveDataBuildMemoryPath(const std::filesystem::path& title_root, int32_t user_id, uint32_t slot_id)
{
	if (user_id < 0)
	{
		return {};
	}
	const auto memory_root = SaveDataBuildMemoryRoot(title_root);
	if (memory_root.empty())
	{
		return {};
	}
	return memory_root / ("user-" + std::to_string(user_id)) / ("slot-" + std::to_string(slot_id) + ".bin");
}

} // namespace Kyty::Libs::SaveData

#endif
