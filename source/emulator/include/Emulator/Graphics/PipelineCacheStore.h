#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PIPELINECACHESTORE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PIPELINECACHESTORE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include "vulkan/vulkan_core.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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
	return 64u * 1024u * 1024u;
}

[[nodiscard]] constexpr size_t PipelineCacheStoreSessionWriteBudgetBytes()
{
	return PipelineCacheStoreMaxBytes();
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

[[nodiscard]] constexpr uint64_t PipelineCacheStoreCheckpointSeconds()
{
	return 30u;
}

[[nodiscard]] constexpr bool PipelineCacheStoreCheckpointDue(bool dirty, bool saved_once, uint64_t elapsed_seconds)
{
	return dirty && (!saved_once || elapsed_seconds >= PipelineCacheStoreCheckpointSeconds());
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
	BudgetExceeded,
	Failed
};

[[nodiscard]] std::vector<uint8_t>         PipelineCacheStoreLoad(const VkPhysicalDeviceProperties& properties);
[[nodiscard]] PipelineCacheStoreSaveResult PipelineCacheStoreSave(VkDevice device, VkPipelineCache cache,
                                                                  const VkPhysicalDeviceProperties& properties,
                                                                  size_t remaining_write_budget, size_t* attempted_size);

} // namespace Kyty::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PIPELINECACHESTORE_H_ */
