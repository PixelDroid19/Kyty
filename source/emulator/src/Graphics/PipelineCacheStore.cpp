#include "Emulator/Graphics/PipelineCacheStore.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

std::filesystem::path CacheRoot()
{
	if (const char* explicit_path = std::getenv("KYTY_VULKAN_PIPELINE_CACHE"); explicit_path != nullptr && explicit_path[0] != '\0')
	{
		return std::filesystem::path(explicit_path);
	}

#ifdef _WIN32
	if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data != nullptr && local_app_data[0] != '\0')
	{
		return std::filesystem::path(local_app_data) / "Kyty" / "Cache";
	}
#elif defined(__APPLE__)
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
	{
		return std::filesystem::path(home) / "Library" / "Caches" / "Kyty";
	}
#else
	if (const char* xdg_cache = std::getenv("XDG_CACHE_HOME"); xdg_cache != nullptr && xdg_cache[0] != '\0')
	{
		return std::filesystem::path(xdg_cache) / "kyty";
	}
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
	{
		return std::filesystem::path(home) / ".cache" / "kyty";
	}
#endif

	std::error_code error;
	const auto      temp = std::filesystem::temp_directory_path(error);
	return error ? std::filesystem::path() : temp / "kyty";
}

std::filesystem::path CachePath(const VkPhysicalDeviceProperties& properties)
{
	const auto configured = CacheRoot();
	if (std::getenv("KYTY_VULKAN_PIPELINE_CACHE") != nullptr)
	{
		return configured;
	}

	std::ostringstream name;
	name << "vulkan-pipeline-" << std::hex << std::setfill('0') << std::setw(8) << properties.vendorID << '-' << std::setw(8)
	     << properties.deviceID << '-';
	for (uint8_t value: properties.pipelineCacheUUID)
	{
		name << std::setw(2) << static_cast<uint32_t>(value);
	}
	name << ".bin";
	return configured / name.str();
}

bool ReplaceFile(const std::filesystem::path& source, const std::filesystem::path& destination)
{
#ifdef _WIN32
	return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
	return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

} // namespace

std::vector<uint8_t> PipelineCacheStoreLoad(const VkPhysicalDeviceProperties& properties)
{
	const auto path = CachePath(properties);
	if (path.empty())
	{
		return {};
	}

	std::error_code error;
	const auto      file_size = std::filesystem::file_size(path, error);
	if (error || file_size < sizeof(PipelineCacheHeaderV1) || file_size > PipelineCacheStoreMaxBytes())
	{
		return {};
	}

	std::vector<uint8_t> data(static_cast<size_t>(file_size));
	std::ifstream        file(path, std::ios::binary);
	if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())) ||
	    !PipelineCacheDataMatchesDevice(data.data(), data.size(), properties))
	{
		return {};
	}
	return data;
}

PipelineCacheStoreSaveResult PipelineCacheStoreSave(VkDevice device, VkPipelineCache cache, const VkPhysicalDeviceProperties& properties,
                                                    size_t remaining_write_budget, size_t* saved_size)
{
	if (saved_size != nullptr)
	{
		*saved_size = 0;
	}
	if (device == VK_NULL_HANDLE || cache == VK_NULL_HANDLE)
	{
		return PipelineCacheStoreSaveResult::Failed;
	}

	size_t size   = 0;
	auto   result = vkGetPipelineCacheData(device, cache, &size, nullptr);
	if (result != VK_SUCCESS || size < sizeof(PipelineCacheHeaderV1) || size > PipelineCacheStoreMaxBytes())
	{
		return PipelineCacheStoreSaveResult::Failed;
	}

	std::vector<uint8_t> data(size);
	result = vkGetPipelineCacheData(device, cache, &size, data.data());
	if (result != VK_SUCCESS || size != data.size() || !PipelineCacheDataMatchesDevice(data.data(), data.size(), properties))
	{
		return PipelineCacheStoreSaveResult::Failed;
	}

	const auto path = CachePath(properties);
	if (path.empty())
	{
		return PipelineCacheStoreSaveResult::Failed;
	}
	if (data.size() > remaining_write_budget)
	{
		return PipelineCacheStoreSaveResult::BudgetExceeded;
	}

	std::error_code error;
	const auto      parent = path.parent_path();
	if (!parent.empty())
	{
		std::filesystem::create_directories(parent, error);
		if (error)
		{
			return PipelineCacheStoreSaveResult::Failed;
		}
	}

	auto temporary = path;
	temporary += ".tmp";
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if (!file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())) || !file.flush())
		{
			std::filesystem::remove(temporary, error);
			return PipelineCacheStoreSaveResult::Failed;
		}
	}

	if (!ReplaceFile(temporary, path))
	{
		std::filesystem::remove(temporary, error);
		return PipelineCacheStoreSaveResult::Failed;
	}

	if (saved_size != nullptr)
	{
		*saved_size = data.size();
	}
	return PipelineCacheStoreSaveResult::Written;
}

} // namespace Kyty::Libs::Graphics

#endif
