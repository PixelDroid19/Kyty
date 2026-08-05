#include "Emulator/Graphics/PipelineCacheStore.h"

#include "Emulator/AtomicFile.h"
#include "Emulator/Host/Platform.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

std::filesystem::path CacheRoot()
{
	if (const char* explicit_path = std::getenv("KYTY_VULKAN_PIPELINE_CACHE"); explicit_path != nullptr && explicit_path[0] != '\0')
	{
		return std::filesystem::path(explicit_path);
	}
	return Kyty::Emulator::Host::DefaultCacheDirectory();
}

std::filesystem::path CachePath(const VkPhysicalDeviceProperties& properties)
{
	const auto configured = CacheRoot();
	const char* explicit_path = std::getenv("KYTY_VULKAN_PIPELINE_CACHE");
	if (explicit_path != nullptr && explicit_path[0] != '\0')
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
                                                    size_t remaining_write_budget, size_t* attempted_size)
{
	if (attempted_size != nullptr)
	{
		*attempted_size = 0;
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

	if (attempted_size != nullptr)
	{
		// Charge the complete blob before opening the temporary file. This is a
		// conservative upper bound for partial writes and failed flush/replace
		// operations, which must not bypass the per-session disk budget.
		*attempted_size = data.size();
	}
	if (!AtomicFileWrite(path, data.data(), data.size()))
	{
		return PipelineCacheStoreSaveResult::Failed;
	}

	return PipelineCacheStoreSaveResult::Written;
}

} // namespace Kyty::Libs::Graphics

#endif
