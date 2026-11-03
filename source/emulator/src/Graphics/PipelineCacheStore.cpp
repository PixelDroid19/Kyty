#include "Emulator/Graphics/PipelineCacheStore.h"

#include "Emulator/AtomicFile.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"
#include "Emulator/Host/Platform.h"
#include "Emulator/SystemContentPort.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

const char* ExplicitCachePath()
{
	const char* path = std::getenv("KYTY_VULKAN_PIPELINE_CACHE");
	return path != nullptr && path[0] != '\0' ? path : nullptr;
}

std::string DeviceName(const VkPhysicalDeviceProperties& properties)
{
	std::ostringstream name;
	name << std::hex << std::setfill('0') << std::setw(8) << properties.vendorID << '-' << std::setw(8) << properties.deviceID;
	return name.str();
}

// The running title's id made safe for a file name; empty before a title is loaded.
std::string TitleName()
{
	Core::String title_id;
	Core::String app_version;
	if (!::Kyty::Emulator::SystemContentPort::GetMetadata(&title_id, &app_version) || title_id.IsEmpty())
	{
		return {};
	}
	std::string name = title_id.C_Str();
	std::replace_if(name.begin(), name.end(), [](char c) { return !(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.'); },
	                '_');
	return name;
}

// One blob per title, device, driver and translator version: titles share no
// pipelines, and a blob shared by every title filled its size cap with modules
// no translator produces any more, after which no save succeeded.
std::filesystem::path CachePath(const VkPhysicalDeviceProperties& properties)
{
	if (const char* explicit_path = ExplicitCachePath(); explicit_path != nullptr)
	{
		return std::filesystem::path(explicit_path);
	}
	const auto title = TitleName();
	if (title.empty())
	{
		return {};
	}

	std::ostringstream name;
	name << "vulkan-pipeline-" << title << '-' << DeviceName(properties) << '-' << std::hex << std::setfill('0');
	for (uint8_t value: properties.pipelineCacheUUID)
	{
		name << std::setw(2) << static_cast<uint32_t>(value);
	}
	name << ShaderTranslatorStoreSuffix() << ".bin";
	return Kyty::Emulator::Host::DefaultCacheDirectory() / name.str();
}

// Removes this device's blobs that `current` replaces: the title's blobs from
// other drivers or translator versions, and the blob every title once shared.
void RemoveReplacedBlobs(const std::filesystem::path& current, const VkPhysicalDeviceProperties& properties)
{
	const auto      device = DeviceName(properties);
	const auto      shared = "vulkan-pipeline-" + device + "-";
	const auto      title  = "vulkan-pipeline-" + TitleName() + "-" + device + "-";
	std::error_code error;
	for (std::filesystem::directory_iterator it(current.parent_path(), error), end; !error && it != end; it.increment(error))
	{
		const auto name = it->path().filename().string();
		if (it->path() != current && it->path().extension() == ".bin" && (name.rfind(shared, 0) == 0 || name.rfind(title, 0) == 0) &&
		    it->is_regular_file(error))
		{
			std::filesystem::remove(it->path(), error);
		}
		error.clear();
	}
}

// Identity of the blob the cache file holds, as this process last loaded or
// wrote it. Pipelines that hit the cache still mark it dirty, and rewriting an
// unchanged blob would spend the session write budget before new pipelines
// appear.
struct PersistedBlob
{
	std::mutex mutex;
	size_t     size   = 0;
	uint64_t   digest = 0;
};

PersistedBlob& Persisted()
{
	static PersistedBlob blob;
	return blob;
}

void RecordPersisted(const std::vector<uint8_t>& data)
{
	auto&           blob = Persisted();
	std::lock_guard lock(blob.mutex);
	blob.size   = data.size();
	blob.digest = XXH3_64bits(data.data(), data.size());
}

bool MatchesPersisted(const std::vector<uint8_t>& data)
{
	auto&           blob = Persisted();
	std::lock_guard lock(blob.mutex);
	return blob.size == data.size() && blob.digest == XXH3_64bits(data.data(), data.size());
}

} // namespace

std::vector<uint8_t> PipelineCacheStoreLoad(const VkPhysicalDeviceProperties& properties)
{
	const auto path = CachePath(properties);
	if (path.empty())
	{
		return {};
	}
	if (ExplicitCachePath() == nullptr)
	{
		RemoveReplacedBlobs(path, properties);
	}
	AtomicFileRemoveOrphans(path.parent_path());

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
	RecordPersisted(data);
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

	if (MatchesPersisted(data))
	{
		return PipelineCacheStoreSaveResult::Unchanged;
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

	RecordPersisted(data);
	return PipelineCacheStoreSaveResult::Written;
}

} // namespace Kyty::Libs::Graphics

#endif
