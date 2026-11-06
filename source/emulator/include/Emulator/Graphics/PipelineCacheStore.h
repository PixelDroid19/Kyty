#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PIPELINECACHESTORE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PIPELINECACHESTORE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include "vulkan/vulkan_core.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <shared_mutex>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct PipelineCacheHeaderV1
{
	uint32_t header_size;
	uint32_t header_version;
	uint32_t vendor_id;
	uint32_t device_id;
	uint8_t  uuid[VK_UUID_SIZE];
};

[[nodiscard]] constexpr size_t PipelineCacheStoreMaxBytes()
{
	return 512u * 1024u * 1024u;
}

// Every checkpoint rewrites the whole blob, so the budget has to cover several
// blobs: pipelines compiled after it runs out are compiled again on every launch.
[[nodiscard]] constexpr size_t PipelineCacheStoreSessionWriteBudgetBytes()
{
	return 4u * PipelineCacheStoreMaxBytes();
}

[[nodiscard]] constexpr bool PipelineCacheStoreWriteBudgetAllows(size_t bytes_written, size_t next_write)
{
	return bytes_written <= PipelineCacheStoreSessionWriteBudgetBytes() &&
	       next_write <= PipelineCacheStoreSessionWriteBudgetBytes() - bytes_written;
}

[[nodiscard]] constexpr size_t PipelineCacheStoreAccountWriteAttempt(size_t bytes_attempted, size_t next_attempt)
{
	const size_t budget = PipelineCacheStoreSessionWriteBudgetBytes();
	return bytes_attempted >= budget || next_attempt >= budget - bytes_attempted ? budget : bytes_attempted + next_attempt;
}

// The interval grows with what this session already wrote, which keeps the disk
// traffic of a title with a large blob proportional to the pipelines it adds.
[[nodiscard]] constexpr uint64_t PipelineCacheStoreCheckpointSeconds(size_t bytes_attempted)
{
	const uint64_t steps = bytes_attempted / (64u * 1024u * 1024u);
	return 30u * (steps > 0u ? steps : 1u);
}

[[nodiscard]] constexpr bool PipelineCacheStoreCheckpointDue(bool dirty, bool saved_once, uint64_t elapsed_seconds, size_t bytes_attempted)
{
	return dirty && (!saved_once || elapsed_seconds >= PipelineCacheStoreCheckpointSeconds(bytes_attempted));
}

[[nodiscard]] inline bool PipelineCacheDataMatchesDevice(const void* data, size_t size, const VkPhysicalDeviceProperties& properties)
{
	if (data == nullptr || size < sizeof(PipelineCacheHeaderV1) || size > PipelineCacheStoreMaxBytes())
	{
		return false;
	}

	PipelineCacheHeaderV1 header {};
	memcpy(&header, data, sizeof(header));

	return header.header_size >= sizeof(header) && header.header_size <= size &&
	       header.header_version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header.vendor_id == properties.vendorID &&
	       header.device_id == properties.deviceID && memcmp(header.uuid, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

enum class PipelineCacheStoreSaveResult
{
	Written,
	// The blob equals the one the file already holds; nothing was written.
	Unchanged,
	BudgetExceeded,
	Failed
};

// Vulkan lets vkGetPipelineCacheData run while other threads create pipelines with
// the same cache, but the Intel Windows driver then faults inside the read. Every
// pipeline creation that passes the shared cache holds this lock shared, and a
// checkpoint read holds it exclusive.
[[nodiscard]] std::shared_mutex& PipelineCacheAccess();

[[nodiscard]] std::vector<uint8_t>         PipelineCacheStoreLoad(const VkPhysicalDeviceProperties& properties);
[[nodiscard]] PipelineCacheStoreSaveResult PipelineCacheStoreSave(VkDevice device, VkPipelineCache cache,
                                                                  const VkPhysicalDeviceProperties& properties,
                                                                  size_t remaining_write_budget, size_t* attempted_size);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PIPELINECACHESTORE_H_ */
