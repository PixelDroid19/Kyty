#include "Emulator/Libs/SaveDataPaths.h"

#include <cctype>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::SaveData {

std::string SaveDataNormalizeTitleId(const char* title_id)
{
	std::string result;
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

std::filesystem::path SaveDataBuildTitleRoot(const std::filesystem::path& save_data_root, const char* title_id)
{
	const auto normalized_title_id = SaveDataNormalizeTitleId(title_id);
	if (save_data_root.empty() || normalized_title_id.empty())
	{
		return {};
	}
	return save_data_root / normalized_title_id;
}

std::filesystem::path SaveDataBuildMemoryPath(const std::filesystem::path& title_root, int32_t user_id, uint32_t slot_id)
{
	if (title_root.empty() || !title_root.is_absolute() || user_id < 0)
	{
		return {};
	}
	return title_root.lexically_normal() / "memory" / ("user-" + std::to_string(user_id)) / ("slot-" + std::to_string(slot_id) + ".bin");
}

} // namespace Kyty::Libs::SaveData

#endif
