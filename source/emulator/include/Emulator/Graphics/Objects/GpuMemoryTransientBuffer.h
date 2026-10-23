#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYTRANSIENTBUFFER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYTRANSIENTBUFFER_H_

#include "Emulator/Graphics/Objects/GpuMemory.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

constexpr uint64_t kGpuMemoryMaxTransientReadOnlyBytes = 4u * 1024u * 1024u;
// Per recording. A 2D title streams about 31 MB of vertex and index
// snapshots into one command buffer; a refused snapshot falls back to a
// GpuMemory object whose upload waits on its own fence.
constexpr uint64_t kGpuMemoryTransientBufferPoolMaxBytes       = 64u * 1024u * 1024u;
constexpr uint32_t kGpuMemoryTransientBufferMaxEntriesPerUsage = 16384u;
// The command buffers of one command pool share a snapshot pool, so it holds
// the snapshots of every recording in flight (one per command buffer of the
// pool), each within the per-recording budget.
constexpr uint32_t kGpuMemoryTransientSnapshotPoolRecordings = 8u;

static_assert(kGpuMemoryMaxTransientReadOnlyBytes <= kGpuMemoryTransientBufferPoolMaxBytes);

enum class GpuMemoryTransientBufferAllocationClass: uint8_t
{
	// Guest bytes in the snapshot pool of the recording's command pool.
	Snapshot,
	// Host data only one recording reads, in that command buffer's own pool.
	Critical,
};

// Bounded immutable buffer views commonly come from guest command rings and
// receive a new address every frame. They are snapshots leased by the
// recordings that bind them, not persistent GPU resources. Surface aliases, writable buffers,
// oversized ranges, and incomplete overlap snapshots stay on the authoritative
// GpuMemory path.
// A writable storage buffer holds the guest bytes while none of its GPU writes is
// unpublished: `writable_storage_idle` states that for every such object over the range.
// An index view also accepts the surfaces an IndexBuffer object is linked under
// (GpuMemoryAllowsIndexContainedInSurface), and a storage view those a
// StorageBuffer object is linked under (GpuMemoryAllowsStorageSurfaceShare):
// that object is uploaded from the same guest bytes and never seeded from the
// surface, so the snapshot reads what the object path would.
[[nodiscard]] inline bool GpuMemorySnapshotViewLinksSurface(GpuMemorySnapshotAdmission admission, GpuMemoryObjectType type,
                                                            GpuMemoryOverlapType relation)
{
	switch (admission)
	{
		case GpuMemorySnapshotAdmission::IndexView:
			return GpuMemoryAllowsIndexContainedInSurface(type, relation, GpuMemoryObjectType::IndexBuffer);
		case GpuMemorySnapshotAdmission::StorageView:
			return GpuMemoryAllowsStorageSurfaceShare(type, relation, GpuMemoryObjectType::StorageBuffer);
		default: return false;
	}
}

[[nodiscard]] inline bool GpuMemoryOverlapsAllowTransientReadOnlyBuffer(const GpuMemoryOverlapSnapshot& overlaps,
                                                                        bool writable_storage_idle,
                                                                        GpuMemorySnapshotAdmission admission)
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
		const bool holds_guest_bytes =
		    is_buffer ? entry.all_read_only || (entry.type == GpuMemoryObjectType::StorageBuffer && writable_storage_idle)
		              : GpuMemorySnapshotViewLinksSurface(admission, entry.type, entry.relation);
		if (!holds_guest_bytes || entry.count == 0u || classified_count > UINT32_MAX - entry.count)
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
	return read_only && size != 0u && size <= kGpuMemoryMaxTransientReadOnlyBytes && allocated && overlap_snapshot_safe;
}

[[nodiscard]] constexpr bool GpuMemoryTransientBufferPoolCanAllocate(uint32_t usage_entries, uint32_t total_entries,
	                                                                    uint64_t total_bytes, uint64_t size,
	                                                                    GpuMemoryTransientBufferAllocationClass allocation_class)
{
	// Bytes bound the pool; the entry ceiling only bounds Vulkan objects. Ring
	// constants arrive as thousands of sub-KiB views per command buffer: a
	// 3,300-draw pass filled 4,096 storage entries with 8.8 MB, a seventh of the
	// byte budget, and every later view fell back to a persistent object.
	const uint32_t recordings =
	    allocation_class == GpuMemoryTransientBufferAllocationClass::Snapshot ? kGpuMemoryTransientSnapshotPoolRecordings : 1u;
	const uint32_t max_entries_per_usage = kGpuMemoryTransientBufferMaxEntriesPerUsage * recordings;
	const uint32_t max_entries           = max_entries_per_usage * 3u;
	const uint64_t max_bytes             = kGpuMemoryTransientBufferPoolMaxBytes * recordings;
	return size != 0u && usage_entries < max_entries_per_usage && total_entries < max_entries && total_bytes < max_bytes &&
	       size <= max_bytes - total_bytes;
}

// Bytes between the logical end of a transient entry and the end of what earlier
// writes may have left non-zero; those must be cleared before the entry is bound.
[[nodiscard]] constexpr uint64_t GpuMemoryTransientBufferTailBytes(uint64_t nonzero_end, uint64_t logical_size)
{
	return logical_size <= nonzero_end ? nonzero_end - logical_size : 0u;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_GPUMEMORYTRANSIENTBUFFER_H_ */
