#include "GpuMemoryInternal.h"

#include "Kyty/Core/Database.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GpuDeferredDeletionQueue.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GpuMemoryMaterializationCache.h"
#include "Emulator/Graphics/GpuMemoryRangeQueryCache.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/GpuMemoryTransientBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vulkan/vk_enum_string_helper.h>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

Vector<GpuMemoryObject> GpuMemory::FindObjects(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type,
                                               bool exact, bool only_first, const SubmissionId* submission)
{
	KYTY_PROFILER_BLOCK("GpuMemory::FindObjects", profiler::colors::Green200);

	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num > VADDR_BLOCKS_MAX || vaddr_num <= 0);

	Core::LockGuard lock(m_mutex);

	Vector<GpuMemoryObject> ret;

	int heap_id = GetHeapId(vaddr[0], size[0]);

	if (heap_id < 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: heap_id < 0 condition ignored (continuing)\n"); }

	auto& heap = m_heaps[heap_id];

	if (exact)
	{
		int fast_id = -1;
		if (FindFast(heap_id, vaddr, size, vaddr_num, type, only_first, &fast_id))
		{
			auto& h = heap.objects[fast_id];
			EXIT_IF(h.free);
			if (submission != nullptr)
			{
				RecordUse(&h.info, *submission);
				h.info.use_last_frame = GpuMemoryAliasLookupUseFrame(m_current_frame, h.info.use_last_frame);
			}
			ret.Add(h.info.object);
		}
		return ret;
	}

	auto objects = FindBlocks(heap_id, vaddr, size, vaddr_num, only_first);

	for (const auto& obj: objects)
	{
		auto& h = heap.objects[obj.object_id];
		EXIT_IF(h.free);
		const bool same_base = (h.block.vaddr_num > 0 && h.block.vaddr[0] == vaddr[0]);
		if (h.info.object.type == type && GpuMemoryFindObjectsAcceptsRelation(obj.relation, exact, same_base))
		{
			if (submission != nullptr)
			{
				RecordUse(&h.info, *submission);
				h.info.use_last_frame = GpuMemoryAliasLookupUseFrame(m_current_frame, h.info.use_last_frame);
			}
			ret.Add(h.info.object);
		}
	}

	return ret;
}

bool GpuMemory::QueryOverlaps(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryOverlapSnapshot* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = GpuMemoryOverlapSnapshot {};

	if (vaddr == nullptr || size == nullptr || vaddr_num <= 0 || vaddr_num > VADDR_BLOCKS_MAX)
	{
		return false;
	}
	for (int i = 0; i < vaddr_num; i++)
	{
		if (size[i] == 0 || vaddr[i] > UINT64_MAX - (size[i] - 1))
		{
			return false;
		}
	}

	Core::LockGuard lock(m_mutex);
	const auto query = GpuMemoryRangeQueryKey::Create(vaddr, size, vaddr_num, false);
	return QueryOverlapsLocked(vaddr, size, vaddr_num, query, out);
}

bool GpuMemory::QueryRangeProvenance(uint64_t vaddr, uint64_t size, GpuMemoryRangeProvenance* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = GpuMemoryRangeProvenance {};
	if (vaddr == 0 || size == 0 || vaddr > UINT64_MAX - (size - 1u))
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);
	const int       heap_id = GetHeapId(vaddr, size);
	if (heap_id < 0)
	{
		return false;
	}
	auto& heap = m_heaps[heap_id];
	// A draw-selected trace must stay cheap even when guest ranges have an
	// unexpectedly dense alias topology. False-positive bucket candidates are
	// bounded separately from the 16 returned real overlaps.
	constexpr uint32_t kMaxPages      = 64;
	constexpr uint32_t kMaxCandidates = 64;
	const bool complete = heap.objects_map2->VisitCandidatesBounded(
	    vaddr, size, kMaxPages, kMaxCandidates,
	    [&](int object_id)
	    {
		    const auto& stored = heap.objects[object_id];
		    if (stored.free)
		    {
			    return true;
		    }
		    const auto relation = GpuMemoryClassifyRangeSets(stored.block.vaddr, stored.block.size, stored.block.vaddr_num, &vaddr,
			                                                   &size, 1, false);
		    if (relation == OverlapType::None)
		    {
			    return true;
		    }
		    if (out->total_count != UINT32_MAX)
		    {
			    out->total_count++;
		    }
		    if (out->entry_count == GpuMemoryRangeProvenance::ENTRIES_MAX)
		    {
			    return false;
		    }
		EXIT_IF(stored.free);
		const auto& info  = stored.info;
		auto&       entry = out->entries[out->entry_count++];
		entry.type                  = info.object.type;
		entry.relation              = relation;
		entry.heap_id               = heap_id;
		entry.object_id             = object_id;
		entry.logical_generation    = info.logical_generation;
		entry.backing_generation    = info.backing_generation;
		entry.content_sequence      = info.content_sequence;
		entry.content_origin        = info.content_origin;
		entry.submit_id             = info.submit_id;
		entry.cpu_update_time       = info.cpu_update_time;
		entry.gpu_update_time       = info.gpu_update_time;
		entry.read_only             = info.read_only;
		entry.in_use                = info.in_use;
		entry.write_back_capable    = info.write_back_func != nullptr && !info.read_only;
		entry.dependencies_complete = m_deferred_deletions.AreDependenciesComplete(info.submission_uses.Dependencies());
		entry.check_hash            = info.check_hash;
		entry.dirty_registered      = info.dirty_registered;
		    return true;
	    });
	out->truncated = !complete;
	return true;
}

bool GpuMemory::QueryOverlapsLocked(const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
	                                const GpuMemoryRangeQueryKey& query, GpuMemoryOverlapSnapshot* out)
{
	*out = GpuMemoryOverlapSnapshot {};
	if (m_overlap_snapshot_cache.Lookup(query, out))
	{
		return true;
	}

	const auto ranges_overlap = [](uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size)
	{ return a <= b ? b - a < a_size : a - b < b_size; };
	const auto intersects_heap = [&](const Heap& heap)
	{
		for (int i = 0; i < vaddr_num; ++i)
		{
			if (ranges_overlap(vaddr[i], size[i], heap.range.vaddr, heap.range.size))
			{
				return true;
			}
		}
		return false;
	};
	bool intersects_allocated_range = false;
	for (const auto& heap: m_heaps)
	{
		if (intersects_heap(heap))
		{
			intersects_allocated_range = true;
			break;
		}
	}
	if (!intersects_allocated_range)
	{
		m_overlap_snapshot_cache.Store(query, *out);
		return true;
	}

	for (uint32_t heap_id = 0; heap_id < m_heaps.Size(); heap_id++)
	{
		const auto& heap    = m_heaps[heap_id];
		if (!intersects_heap(heap))
		{
			continue;
		}
		const auto  objects = FindBlocks(static_cast<int>(heap_id), vaddr, size, vaddr_num);
		for (const auto& object: objects)
		{
			const auto& stored = heap.objects[object.object_id];
			EXIT_IF(stored.free);

			out->total_count++;
			if (object.relation == GpuMemoryOverlapType::Equals)
			{
				out->exact_count++;
			}

			GpuMemoryOverlapEntry* entry = nullptr;
			for (uint32_t i = 0; i < out->entry_count; i++)
			{
				if (out->entries[i].type == stored.info.object.type && out->entries[i].relation == object.relation)
				{
					entry = &out->entries[i];
					break;
				}
			}
			if (entry == nullptr)
			{
				if (out->entry_count == GpuMemoryOverlapSnapshot::ENTRIES_MAX)
				{
					out->truncated = true;
					continue;
				}
				entry                = &out->entries[out->entry_count++];
				entry->type          = stored.info.object.type;
				entry->relation      = object.relation;
				entry->exact         = object.relation == GpuMemoryOverlapType::Equals;
				entry->all_read_only = true;
			}
			entry->all_read_only = entry->all_read_only && stored.info.read_only;
			entry->count++;
		}
	}

	m_overlap_snapshot_cache.Store(query, *out);
	return true;
}

bool GpuMemory::CanSnapshotReadOnlyBuffer(uint64_t vaddr, uint64_t size)
{
	if (!GpuMemoryCanUseTransientReadOnlyBuffer(true, size, true, true) || vaddr > UINT64_MAX - (size - 1u))
	{
		return false;
	}

	const auto      query = GpuMemoryRangeQueryKey::Create(&vaddr, &size, 1, false);
	Core::LockGuard lock(m_mutex);
	if (ValidateAllocatedRangeLocked(vaddr, size, query) != GpuMemoryRangeValidationStatus::Valid)
	{
		return false;
	}
	if (const auto* cached = m_overlap_snapshot_cache.BorrowLookup(query); cached != nullptr)
	{
		return GpuMemoryOverlapsAllowTransientReadOnlyBuffer(*cached);
	}
	GpuMemoryOverlapSnapshot overlaps {};
	return QueryOverlapsLocked(&vaddr, &size, 1, query, &overlaps) && GpuMemoryOverlapsAllowTransientReadOnlyBuffer(overlaps);
}

bool GpuMemory::CaptureSnapshotReadOnlyBuffer(uint64_t vaddr, uint64_t size, void* dst, uint64_t* validation_ns, uint64_t* copy_ns)
{
	const auto validation_start = std::chrono::steady_clock::now();
	const auto finish_validation = [&]()
	{
		if (validation_ns != nullptr)
		{
			*validation_ns = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - validation_start).count());
		}
	};
	if (copy_ns != nullptr)
	{
		*copy_ns = 0u;
	}
	if (dst == nullptr || !GpuMemoryCanUseTransientReadOnlyBuffer(true, size, true, true) || vaddr > UINT64_MAX - (size - 1u))
	{
		finish_validation();
		return false;
	}

	const auto      query = GpuMemoryRangeQueryKey::Create(&vaddr, &size, 1, false);
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
	if (ValidateAllocatedRangeLocked(vaddr, size, query) != GpuMemoryRangeValidationStatus::Valid)
	{
		finish_validation();
		return false;
	}
	if (const auto* cached = m_overlap_snapshot_cache.BorrowLookup(query); cached != nullptr)
	{
		if (!GpuMemoryOverlapsAllowTransientReadOnlyBuffer(*cached))
		{
			finish_validation();
			return false;
		}
	} else
	{
		GpuMemoryOverlapSnapshot overlaps {};
		if (!QueryOverlapsLocked(&vaddr, &size, 1, query, &overlaps) || !GpuMemoryOverlapsAllowTransientReadOnlyBuffer(overlaps))
		{
			finish_validation();
			return false;
		}
	}
	auto& dirty_tracker = GpuDirtyPageTracker::Instance();
	if (!dirty_tracker.RegisterRange(vaddr, size))
	{
		finish_validation();
		return false;
	}
	const auto dirty_read = dirty_tracker.BeginRead(vaddr, size);
	if (!dirty_read.tracked)
	{
		(void)dirty_tracker.UnregisterRange(vaddr, size);
		finish_validation();
		return false;
	}

	finish_validation();
	const auto copy_start = std::chrono::steady_clock::now();
	std::memcpy(dst, reinterpret_cast<const void*>(vaddr), static_cast<size_t>(size));
	const bool stable_copy = dirty_tracker.ReadObservationIsStable(vaddr, size, dirty_read);
	if (copy_ns != nullptr)
	{
		*copy_ns = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - copy_start).count());
	}
	(void)dirty_tracker.UnregisterRange(vaddr, size);
	return stable_copy;
}

bool GpuMemory::CompareSnapshotReadOnlyBuffer(uint64_t vaddr, uint64_t size, const void* snapshot, bool* matches,
	                                          uint64_t* validation_ns, uint64_t* compare_ns)
{
	const auto validation_start = std::chrono::steady_clock::now();
	const auto finish_validation = [&]()
	{
		if (validation_ns != nullptr)
		{
			*validation_ns = static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - validation_start).count());
		}
	};
	if (matches == nullptr)
	{
		return false;
	}
	*matches = false;
	if (validation_ns != nullptr)
	{
		*validation_ns = 0u;
	}
	if (compare_ns != nullptr)
	{
		*compare_ns = 0u;
	}
	if (snapshot == nullptr || !GpuMemoryCanUseTransientReadOnlyBuffer(true, size, true, true) ||
	    vaddr > UINT64_MAX - (size - 1u))
	{
		finish_validation();
		return false;
	}

	const auto      query = GpuMemoryRangeQueryKey::Create(&vaddr, &size, 1, false);
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
	if (ValidateAllocatedRangeLocked(vaddr, size, query) != GpuMemoryRangeValidationStatus::Valid)
	{
		finish_validation();
		return false;
	}
	if (const auto* cached = m_overlap_snapshot_cache.BorrowLookup(query); cached != nullptr)
	{
		if (!GpuMemoryOverlapsAllowTransientReadOnlyBuffer(*cached))
		{
			finish_validation();
			return false;
		}
	} else
	{
		GpuMemoryOverlapSnapshot overlaps {};
		if (!QueryOverlapsLocked(&vaddr, &size, 1, query, &overlaps) || !GpuMemoryOverlapsAllowTransientReadOnlyBuffer(overlaps))
		{
			finish_validation();
			return false;
		}
	}
	auto& dirty_tracker = GpuDirtyPageTracker::Instance();
	if (!dirty_tracker.RegisterRange(vaddr, size))
	{
		finish_validation();
		return false;
	}
	const auto dirty_read = dirty_tracker.BeginRead(vaddr, size);
	if (!dirty_read.tracked)
	{
		(void)dirty_tracker.UnregisterRange(vaddr, size);
		finish_validation();
		return false;
	}

	finish_validation();
	const auto compare_start = std::chrono::steady_clock::now();
	*matches = std::memcmp(reinterpret_cast<const void*>(vaddr), snapshot, static_cast<size_t>(size)) == 0;
	const bool stable_compare = dirty_tracker.ReadObservationIsStable(vaddr, size, dirty_read);
	if (compare_ns != nullptr)
	{
		*compare_ns = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - compare_start).count());
	}
	if (!stable_compare)
	{
		(void)dirty_tracker.UnregisterRange(vaddr, size);
		*matches = false;
		return false;
	}
	(void)dirty_tracker.UnregisterRange(vaddr, size);
	return true;
}

void GpuMemory::ResetHash(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type)
{
	EXIT_IF(type == GpuMemoryObjectType::Invalid);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num > VADDR_BLOCKS_MAX || vaddr_num <= 0);

	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);

	for (int vi = 0; vi < vaddr_num; vi++)
	{
		m_materialization_cache.InvalidateRange(vaddr[vi], size[vi]);
	}

	int heap_id = GetHeapId(vaddr[0], size[0]);

	if (heap_id < 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: heap_id < 0 condition ignored (continuing)\n"); }

	auto& heap = m_heaps[heap_id];

	uint64_t new_hash = 0;

	int fast_id = -1;
	if (FindFast(heap_id, vaddr, size, vaddr_num, type, false, &fast_id))
	{
		auto& h = heap.objects[fast_id];
		EXIT_IF(h.free);
		auto& o = h.info;

		if (h.scenario == GpuMemoryScenario::Common)
		{
			for (int vi = 0; vi < vaddr_num; vi++)
			{
				KYTY_LOG_DEBUG("ResetHash: type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 ", old_hash = 0x%016" PRIx64
				       ", new_hash = 0x%016" PRIx64 "\n",
				       Core::EnumName(o.object.type).C_Str(), vaddr[vi], size[vi], o.hash[vi], new_hash);
			}
			o.gpu_update_time = GpuMemoryGetCurrentTime();

			return;
		}
	}

	auto object_ids = FindBlocks(heap_id, vaddr, size, vaddr_num);

	if (!object_ids.IsEmpty())
	{
		for (const auto& obj: object_ids)
		{
			auto& h = heap.objects[obj.object_id];
			EXIT_IF(h.free);

			auto& o = h.info;
			if (o.object.type == type)
			{
				if (obj.relation != OverlapType::Equals) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: obj.relation != OverlapType::Equals condition ignored (continuing)\n"); }

				for (int vi = 0; vi < vaddr_num; vi++)
				{
					KYTY_LOG_DEBUG("ResetHash: type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 ", old_hash = 0x%016" PRIx64
					       ", new_hash = 0x%016" PRIx64 "\n",
					       Core::EnumName(o.object.type).C_Str(), vaddr[vi], size[vi], o.hash[vi], new_hash);
				}
				o.gpu_update_time = GpuMemoryGetCurrentTime();
			}
		}
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool GpuMemory::FindFast(int heap_id, const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type, bool only_first,
                         int* id)
{
	KYTY_PROFILER_BLOCK("GpuMemory::FindFast", profiler::colors::Green200);

	auto& heap = m_heaps[heap_id];

	EXIT_IF(id == nullptr);

	for (int vi = 0; vi < vaddr_num; vi++)
	{
		for (int obj_id: heap.objects_map1->FindAll(vaddr[vi]))
		{
			auto& b = heap.objects[obj_id];
			EXIT_IF(b.free);
			if (b.info.object.type == type && GpuMemoryClassifyRangeSets(b.block.vaddr, b.block.size, b.block.vaddr_num, vaddr, size,
			                                                             vaddr_num, only_first) == OverlapType::Equals)
			{
				*id = obj_id;
				return true;
			}
		}
	}

	return false;
}
Vector<GpuMemory::OverlappedBlock> GpuMemory::FindBlocks(int heap_id, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                                                         bool only_first)
{
	KYTY_PROFILER_BLOCK("GpuMemory::FindBlocks", profiler::colors::Green100);

	auto& heap = m_heaps[heap_id];

	EXIT_IF(vaddr_num <= 0 || vaddr_num > VADDR_BLOCKS_MAX);
	EXIT_IF(vaddr == nullptr || size == nullptr);
	EXIT_IF(only_first && vaddr_num != 1);

	Vector<GpuMemory::OverlappedBlock> ret;
	EXIT_IF(heap.overlap_cache == nullptr);
	for (int i = 0; i < vaddr_num; ++i)
	{
		// An empty range cannot overlap a GPU object and must not enter the
		// range-query cache, whose keys intentionally reject zero-sized spans.
		if (size[i] == 0)
		{
			return ret;
		}
	}
	const auto query = GpuMemoryRangeQueryKey::Create(vaddr, size, vaddr_num, only_first);
	EXIT_IF(!query.Valid());
	if (const auto* cached = heap.overlap_cache->BorrowLookup(query))
	{
		return *cached;
	}

	const Vector<int> candidates =
	    vaddr_num == 1 ? heap.objects_map2->FindAll(vaddr[0], size[0]) : heap.objects_map2->FindAll(vaddr, size, vaddr_num);
	for (int index: candidates)
	{
		const auto& object = heap.objects[index];
		if (object.free)
		{
			continue;
		}
		const auto relation =
		    GpuMemoryClassifyRangeSets(object.block.vaddr, object.block.size, object.block.vaddr_num, vaddr, size, vaddr_num, only_first);
		if (relation != OverlapType::None)
		{
			ret.Add({relation, index});
		}
	}

	{
		KYTY_PROFILER_BLOCK("sort");
		ret.Sort([](auto& b1, auto& b2) { return b1.object_id < b2.object_id; });
	}
	heap.overlap_cache->Store(query, ret);

	return ret;
}

GpuMemory::Block GpuMemory::CreateBlock(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, int heap_id, int obj_id)
{
	EXIT_IF(vaddr_num > VADDR_BLOCKS_MAX);
	EXIT_IF(vaddr == nullptr || size == nullptr);

	auto& heap = m_heaps[heap_id];
	EXIT_IF(heap.overlap_cache == nullptr);

	Block nb {};
	nb.vaddr_num = vaddr_num;
	for (int vi = 0; vi < vaddr_num; vi++)
	{
		m_materialization_cache.InvalidateRange(vaddr[vi], size[vi]);
		m_overlap_snapshot_cache.InvalidateRange(vaddr[vi], size[vi]);
		heap.overlap_cache->InvalidateRange(vaddr[vi], size[vi]);
		nb.vaddr[vi] = vaddr[vi];
		nb.size[vi]  = size[vi];
		heap.objects_size += size[vi];
		heap.objects_map1->Insert(vaddr[vi], obj_id);
		heap.objects_map2->Insert(vaddr[vi], size[vi], obj_id);
	}
	return nb;
}

void GpuMemory::DeleteBlock(Block* b, int heap_id, int obj_id)
{
	auto& heap = m_heaps[heap_id];
	EXIT_IF(heap.overlap_cache == nullptr);

	for (int vi = 0; vi < b->vaddr_num; vi++)
	{
		m_materialization_cache.InvalidateRange(b->vaddr[vi], b->size[vi]);
		m_overlap_snapshot_cache.InvalidateRange(b->vaddr[vi], b->size[vi]);
		heap.overlap_cache->InvalidateRange(b->vaddr[vi], b->size[vi]);
		heap.objects_size -= b->size[vi];
		heap.objects_map1->Erase(b->vaddr[vi], obj_id);
		heap.objects_map2->Erase(b->vaddr[vi], b->size[vi], obj_id);
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
