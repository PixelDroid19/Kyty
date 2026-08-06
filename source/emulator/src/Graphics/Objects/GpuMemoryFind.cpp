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
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

	const auto ranges_overlap = [](uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size)
	{ return a <= b ? b - a < a_size : a - b < b_size; };
	bool intersects_allocated_range = false;
	for (const auto& heap: m_heaps)
	{
		for (int i = 0; i < vaddr_num; i++)
		{
			if (ranges_overlap(vaddr[i], size[i], heap.range.vaddr, heap.range.size))
			{
				intersects_allocated_range = true;
				break;
			}
		}
		if (intersects_allocated_range)
		{
			break;
		}
	}
	if (!intersects_allocated_range)
	{
		return true;
	}

	for (uint32_t heap_id = 0; heap_id < m_heaps.Size(); heap_id++)
	{
		const auto& heap    = m_heaps[heap_id];
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
	if (heap.overlap_cache->Lookup(query, &ret))
	{
		return ret;
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
		heap.overlap_cache->InvalidateRange(b->vaddr[vi], b->size[vi]);
		heap.objects_size -= b->size[vi];
		heap.objects_map1->Erase(b->vaddr[vi], obj_id);
		heap.objects_map2->Erase(b->vaddr[vi], b->size[vi], obj_id);
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED