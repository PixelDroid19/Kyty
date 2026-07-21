#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYTRANSIENTBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYTRANSIENTBUFFER_H_

#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Small immutable buffer views commonly come from guest command rings and
// receive a new address every frame. They are snapshots, not persistent GPU
// resources. Surface aliases, writable buffers, large ranges, and incomplete
// overlap snapshots stay on the authoritative GpuMemory path.
[[nodiscard]] inline bool GpuMemoryOverlapsAllowTransientReadOnlyBuffer(const GpuMemoryOverlapSnapshot& overlaps)
{
	if (overlaps.truncated || overlaps.entry_count > GpuMemoryOverlapSnapshot::ENTRIES_MAX)
	{
		return false;
	}

	uint32_t classified_count = 0;
	for (uint32_t i = 0; i < overlaps.entry_count; i++)
	{
		const auto& entry = overlaps.entries[i];
		const bool  is_buffer =
		    entry.type == GpuMemoryObjectType::StorageBuffer || entry.type == GpuMemoryObjectType::VertexBuffer ||
		    entry.type == GpuMemoryObjectType::IndexBuffer;
		if (!is_buffer || !entry.all_read_only || entry.count == 0u || classified_count > UINT32_MAX - entry.count)
		{
			return false;
		}
		classified_count += entry.count;
	}
	return classified_count == overlaps.total_count;
}

[[nodiscard]] constexpr bool GpuMemoryCanUseTransientReadOnlyBuffer(bool read_only, uint64_t size, bool allocated,
                                                                   bool overlap_snapshot_safe)
{
	constexpr uint64_t MaxTransientBytes = 0x1000u;
	return read_only && size != 0u && size <= MaxTransientBytes && allocated && overlap_snapshot_safe;
}

[[nodiscard]] constexpr bool GpuMemoryTransientBufferPoolCanAllocate(uint32_t usage_entries, uint32_t total_entries,
                                                                    uint64_t total_bytes, uint64_t size)
{
	constexpr uint32_t MaxEntriesPerUsage = 512u;
	constexpr uint32_t MaxEntries         = MaxEntriesPerUsage * 3u;
	constexpr uint64_t MaxBytes           = 16u * 1024u * 1024u;
	return size != 0u && usage_entries < MaxEntriesPerUsage && total_entries < MaxEntries && total_bytes < MaxBytes &&
	       size <= MaxBytes - total_bytes;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYTRANSIENTBUFFER_H_ */
