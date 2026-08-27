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
#include "Emulator/GuestMemory.h"
#include "Emulator/Profiler.h"
#include "Emulator/Log.h"

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

GpuMemory*    g_gpu_memory    = nullptr;
GpuResources* g_gpu_resources = nullptr;

uint64_t GpuMemory::NextContentSequence()
{
	EXIT_IF(m_content_sequence == UINT64_MAX);
	return ++m_content_sequence;
}

uint64_t GpuMemoryCalcHash(GpuMemoryObjectType type, const uint8_t* buf, uint64_t size)
{
	KYTY_PROFILER_FUNCTION();

	if (size == 0 || buf == nullptr)
	{
		return 0;
	}
	// GPU-only guest mappings are not host-readable; hashing them faults. Once
	// the guest-memory boundary is installed, every GpuMemory range must resolve
	// through it. Unit tests construct host-only fixtures before that boundary is
	// installed and retain the historical direct-hash path.
	{
		auto&                              guest_memory = Emulator::GuestMemory::GetPort();
		Emulator::GuestMemory::MappedRange mapped {};
		const uint64_t                     addr = reinterpret_cast<uint64_t>(buf);
		if (guest_memory.IsInstalled())
		{
			const bool range_known = guest_memory.QueryMappedRange(addr, size, &mapped);
			if (!range_known)
			{
				KYTY_LOG_LIMIT(Log::Level::Warn, 8,
				               "WARNING: GpuMemoryCalcHash skipping unmapped guest range type=%u buf=0x%012" PRIx64
				               " size=0x%012" PRIx64 "\n",
				               static_cast<unsigned>(type), addr, size);
				return 0;
			}
			const bool covered = mapped.base != 0 && mapped.size != 0 && size <= mapped.size && addr >= mapped.base &&
			                     addr - mapped.base <= mapped.size - size;
			if (!covered)
			{
				KYTY_LOG_LIMIT(Log::Level::Warn, 8,
				               "WARNING: GpuMemoryCalcHash skipping incompletely mapped range type=%u buf=0x%012" PRIx64
				               " size=0x%012" PRIx64 "\n",
				               static_cast<unsigned>(type), addr, size);
				return 0;
			}

			void* protection_start = nullptr;
			void* protection_end   = nullptr;
			int   protection       = 0;
			const int protection_result =
			    guest_memory.QueryProtection(const_cast<uint8_t*>(buf), &protection_start, &protection_end, &protection);
			if (protection_result != 0 || (protection & 0x3) == 0)
			{
				KYTY_LOG_LIMIT(Log::Level::Warn, 8,
				               "WARNING: GpuMemoryCalcHash skipping non-CPU-readable range type=%u buf=0x%012" PRIx64
				               " size=0x%012" PRIx64 " prot=0x%x result=%d\n",
				               static_cast<unsigned>(type), addr, size, protection, protection_result);
				return 0;
			}
		}
	}
	const auto start   = std::chrono::steady_clock::now();
	const auto result  = XXH3_64bits(buf, size);
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
	DebugStatsRecordGpuMemoryHash(GpuMemoryStatsTypeIndex(type), size, static_cast<uint64_t>(elapsed));
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::Hash, static_cast<uint64_t>(elapsed), size);
	return result;
}

uint64_t GpuMemoryGetCurrentTime()
{
	static std::atomic_uint64_t t(0);
	return ++t;
}

void GpuMemory::SetAllocatedRange(uint64_t vaddr, uint64_t size)
{
	EXIT_IF(size == 0);

	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	if (IsAllocated(vaddr, size)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: IsAllocated(vaddr, size) condition ignored (continuing)\n"); }

	Core::LockGuard lock(m_mutex);

	Heap h;
	h.range.vaddr   = vaddr;
	h.range.size    = size;
	h.objects_map1  = new GpuMap1;
	h.objects_map2  = new GpuMap2;
	h.overlap_cache = new OverlapQueryCache;

	m_heaps.Add(h);
	m_allocated_validation_cache.Invalidate();
	m_allocated_prefix_cache.Invalidate();
	m_overlap_snapshot_cache.Invalidate();
}

bool GpuMemory::IsAllocated(uint64_t vaddr, uint64_t size)
{
	if (size == 0)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);

	return (GetHeapId(vaddr, size) >= 0);
}

GpuMemoryRangeValidationStatus GpuMemory::ValidateAllocatedRange(uint64_t vaddr, uint64_t size)
{
	if (size == 0 || vaddr > UINT64_MAX - (size - 1u))
	{
		return GpuMemoryRangeValidationStatus::InvalidArgument;
	}

	Core::LockGuard lock(m_mutex);
	const uint64_t query_size = size;
	const auto     query      = GpuMemoryRangeQueryKey::Create(&vaddr, &query_size, 1, false);
	return ValidateAllocatedRangeLocked(vaddr, size, query);
}

GpuMemoryRangeValidationStatus GpuMemory::ValidateAllocatedRangeLocked(uint64_t vaddr, uint64_t size,
	                                                                    const GpuMemoryRangeQueryKey& query)
{
	GpuMemoryRangeValidationStatus cached {};
	if (m_allocated_validation_cache.Lookup(query, &cached))
	{
		return cached;
	}
	for (const auto& heap: m_heaps)
	{
		if (vaddr < heap.range.vaddr)
		{
			continue;
		}
		const uint64_t offset = vaddr - heap.range.vaddr;
		if (offset < heap.range.size && size <= heap.range.size - offset)
		{
			m_allocated_validation_cache.Store(query, GpuMemoryRangeValidationStatus::Valid);
			return GpuMemoryRangeValidationStatus::Valid;
		}
	}
	m_allocated_validation_cache.Store(query, GpuMemoryRangeValidationStatus::Unallocated);
	return GpuMemoryRangeValidationStatus::Unallocated;
}

uint64_t GpuMemory::GetAllocatedRangePrefix(uint64_t vaddr, uint64_t maximum_size)
{
	if (maximum_size == 0)
	{
		return 0;
	}

	Core::LockGuard lock(m_mutex);
	const uint64_t query_size = maximum_size;
	const auto     query      = GpuMemoryRangeQueryKey::Create(&vaddr, &query_size, 1, false);
	uint64_t       cached     = 0;
	if (m_allocated_prefix_cache.Lookup(query, &cached))
	{
		return cached;
	}
	for (const auto& heap: m_heaps)
	{
		if (vaddr < heap.range.vaddr)
		{
			continue;
		}
		const uint64_t offset = vaddr - heap.range.vaddr;
		if (offset >= heap.range.size)
		{
			continue;
		}
		const uint64_t available = heap.range.size - offset;
		const uint64_t prefix    = available < maximum_size ? available : maximum_size;
		m_allocated_prefix_cache.Store(query, prefix);
		return prefix;
	}
	m_allocated_prefix_cache.Store(query, 0);
	return 0;
}

int GpuMemory::GetHeapId(uint64_t vaddr, uint64_t size)
{
	int index = 0;
	for (const auto& heap: m_heaps)
	{
		const auto& r = heap.range;
		if ((vaddr >= r.vaddr && vaddr < r.vaddr + r.size) || ((vaddr + size - 1) >= r.vaddr && (vaddr + size - 1) < r.vaddr + r.size))
		{
			return index;
		}
		index++;
	}
	return -1;
}

void GpuMemory::Free(GraphicContext* ctx, uint64_t vaddr, uint64_t size, GpuMemoryRangeReleaseMode mode)
{
	KYTY_PROFILER_BLOCK("GpuMemory::Free", profiler::colors::Green300);

	if (mode != GpuMemoryRangeReleaseMode::ObjectsOnly)
	{
		// The caller owns the GPU admission gate and has drained every queue.
		// Publish completed GPU writes before detaching resources from the range.
		WriteBackAllCompleted(ctx);
	}

	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	m_mutex.Lock();

	KYTY_LOG_DEBUG("Release gpu objects:\n");
	KYTY_LOG_DEBUG("\t gpu_vaddr = 0x%016" PRIx64 "\n", vaddr);
	KYTY_LOG_DEBUG("\t size   = 0x%016" PRIx64 "\n", size);

	int heap_id = GetHeapId(vaddr, size);

	if (heap_id < 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: heap_id < 0 condition ignored (continuing)\n"); }

	auto object_ids = FindBlocks(heap_id, &vaddr, &size, 1);

	Vector<Destructor> destructors;

	for (const auto& obj: object_ids)
	{
		switch (obj.relation)
		{
			case OverlapType::Equals:
			case OverlapType::IsContainedWithin:
			case OverlapType::Crosses: RequireDetachable(ctx, heap_id, obj.object_id, &destructors, "range_free"); break;
			// The stored object is the parent of the range being released.  It
			// remains live because freeing a child view must not destroy its
			// backing allocation.
			case OverlapType::Contains: break;
			default: GpuMemoryDbgDump(); EXIT("unknown obj.relation: %s\n", Core::EnumName(obj.relation).C_Str());
		}
	}
	for (const auto& obj: object_ids)
	{
		if (obj.relation != OverlapType::Contains)
		{
			destructors.Add(Free(heap_id, obj.object_id));
		}
	}

	if (mode == GpuMemoryRangeReleaseMode::Unmap)
	{
		// Already holding m_mutex — avoid IsAllocated's recursive re-lock.
		if (GetHeapId(vaddr, size) < 0)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !IsAllocated(vaddr, size) condition ignored (continuing)\n");
		}

		int index = 0;
		for (auto& a: m_heaps)
		{
			if (a.range.vaddr == vaddr && a.range.size == size)
			{
				EXIT_IF(a.objects_map1 == nullptr);
				EXIT_IF(a.objects_map2 == nullptr);
				EXIT_IF(a.overlap_cache == nullptr);
				if (heap_id != index) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: heap_id != index condition ignored (continuing)\n"); }
				if (a.objects_size != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: a.objects_size != 0 condition ignored (continuing)\n"); }
				if (!a.objects_map1->IsEmpty()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !a.objects_map1->IsEmpty() condition ignored (continuing)\n"); }
				if (!a.objects_map2->IsEmpty()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !a.objects_map2->IsEmpty() condition ignored (continuing)\n"); }

				delete a.objects_map1;
				delete a.objects_map2;
				delete a.overlap_cache;

				m_heaps.RemoveAt(index);
				m_allocated_validation_cache.Invalidate();
				m_allocated_prefix_cache.Invalidate();
				m_overlap_snapshot_cache.Invalidate();
				break;
			}
			index++;
		}

		if (GetHeapId(vaddr, size) >= 0)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: IsAllocated(vaddr, size) condition ignored (continuing)\n");
		}
	}

	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
	m_mutex.Unlock();
}

void GpuMemory::RequireDetachable(GraphicContext* ctx, int heap_id, int object_id, Vector<Destructor>* destructors, const char* operation,
                                  GpuMemoryObjectType incoming_type)
{
	EXIT_IF(ctx == nullptr || destructors == nullptr || operation == nullptr);
	auto& heap = m_heaps[heap_id];
	auto& h    = heap.objects[object_id];
	EXIT_IF(h.free);
	auto& object = h.info;

	if (object.in_use && object.write_back_func != nullptr && !object.read_only)
	{
		const auto dependencies = object.submission_uses.Dependencies();
		if (m_deferred_deletions.AreDependenciesComplete(dependencies))
		{
			WriteBackObjectLocked(ctx, heap_id, object_id, destructors);
			EXIT_IF(object.in_use);
			return;
		}

		KYTY_LOG_DEBUG(
		             "GpuMemory detach blocked: operation=%s incoming=%s type=%s heap=%d id=%d generation=%" PRIu64
		             " ranges=%d dependencies=%zu\n",
		             operation, Core::EnumName(incoming_type).C_Str(), Core::EnumName(object.object.type).C_Str(), heap_id, object_id,
		             object.backing_generation, h.block.vaddr_num, dependencies.size());
		for (int vi = 0; vi < h.block.vaddr_num; vi++)
		{
			KYTY_LOG_DEBUG( "  range[%d]=0x%016" PRIx64 "+0x%016" PRIx64 "\n", vi, h.block.vaddr[vi], h.block.size[vi]);
		}
		for (const auto& dependency: dependencies)
		{
			KYTY_LOG_DEBUG( "  dependency queue=%" PRIu32 " sequence=%" PRIu64 " complete=%d\n", dependency.queue.Value(),
			             dependency.sequence, m_deferred_deletions.AreDependenciesComplete({dependency}) ? 1 : 0);
		}
		EXIT("GpuMemory cannot detach an in-use write-back object before its completion callback\n");
	}
}

GpuMemory::Destructor GpuMemory::Free(int heap_id, int object_id)
{
	KYTY_PROFILER_BLOCK("GpuMemory::Free", profiler::colors::Green400);

	auto& heap = m_heaps[heap_id];

	auto& h = heap.objects[object_id];
	EXIT_IF(h.free);
	auto&       o     = h.info;
	const auto& block = h.block;
	// Every caller preflights the complete reclaim set before mutating it.
	// Reaching this point with unpublished GPU content would make the
	// transaction partial, so retain a hard invariant here.
	EXIT_IF(o.in_use && o.write_back_func != nullptr && !o.read_only);

	if (o.dirty_registered)
	{
		for (int vi = 0; vi < block.vaddr_num; vi++)
		{
			(void)GpuDirtyPageTracker::Instance().UnregisterRange(block.vaddr[vi], block.size[vi]);
		}
		o.dirty_registered = false;
	}

	EXIT_IF(o.delete_func == nullptr);

	Destructor ret {};

	uint64_t freed_bytes = 0;
	for (int vi = 0; vi < block.vaddr_num; vi++)
	{
		freed_bytes += block.size[vi];
	}
	DebugStatsRecordFree(freed_bytes);
	DebugStatsRecordGpuMemoryFree(GpuMemoryStatsTypeIndex(o.object.type));

	if (o.delete_func != nullptr)
	{
		if (Config::GetPrintfDirection() != Log::Direction::Silent)
		{
			for (int vi = 0; vi < block.vaddr_num; vi++)
			{
				KYTY_LOG_DEBUG("Delete: type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n", Core::EnumName(o.object.type).C_Str(),
				       block.vaddr[vi], block.size[vi]);
			}
		}
		// o.delete_func(ctx, o.object.obj, &o.mem);
		ret.delete_func     = o.delete_func;
		ret.obj             = o.object.obj;
		ret.type            = o.object.type;
		ret.submission_uses = o.submission_uses;
		ret.mem             = o.mem;
	}

	// Drop bidirectional alias links before recycling the slot. Multi-parent
	// reclaim (VB/Texture delete_all) otherwise leaves free object_ids in peer
	// others lists; WriteBack then EXIT_IF(parent.free) on those dangling links
	// (captured dual-strict after GPU-owned RT WriteBack skip). Treat the link
	// list as untrusted state: an object can be recycled while a cached overlap
	// entry still refers to the old slot, so never index the object table before
	// validating the id. Remove malformed reverse links as well, otherwise the
	// next reclaim would walk the same stale id again.
	const uint32_t object_count = heap.objects.Size();
	for (uint32_t oi = 0; oi < h.others.Size(); oi++)
	{
		const auto& other = h.others.At(oi);
		if (other.object_id < 0 || static_cast<uint32_t>(other.object_id) >= object_count)
		{
			continue;
		}

		auto& peer = heap.objects[other.object_id];
		if (peer.free)
		{
			continue;
		}

		for (uint32_t pi = peer.others.Size(); pi > 0;)
		{
			--pi;
			const auto& link = peer.others.At(pi);
			if (link.object_id == object_id || link.object_id < 0 || static_cast<uint32_t>(link.object_id) >= object_count)
			{
				peer.others.RemoveAt(pi);
			}
		}
	}
	h.others.Clear();

	h.free             = true;
	h.next_free_id     = heap.first_free_id;
	heap.first_free_id = object_id;
	DeleteBlock(&h.block, heap_id, object_id);

	return ret;
}

void GpuMemoryInit()
{
	EXIT_IF(g_gpu_memory != nullptr);
	EXIT_IF(g_gpu_resources != nullptr);

	g_gpu_memory    = new GpuMemory;
	g_gpu_resources = new GpuResources;

	GpuMemoryVulkanStatsInit();
}

void GpuMemorySetAllocatedRange(uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_gpu_memory == nullptr);

	g_gpu_memory->SetAllocatedRange(vaddr, size);
}

GpuMemoryRangeValidationStatus GpuMemoryValidateAllocatedRange(uint64_t vaddr, uint64_t size)
{
	if (g_gpu_memory == nullptr)
	{
		return GpuMemoryRangeValidationStatus::Unallocated;
	}
	return g_gpu_memory->ValidateAllocatedRange(vaddr, size);
}

uint64_t GpuMemoryGetAllocatedRangePrefix(uint64_t vaddr, uint64_t maximum_size)
{
	if (g_gpu_memory == nullptr)
	{
		return 0;
	}
	return g_gpu_memory->GetAllocatedRangePrefix(vaddr, maximum_size);
}

bool GpuMemoryCanSnapshotReadOnlyBuffer(uint64_t vaddr, uint64_t size)
{
	return g_gpu_memory != nullptr && g_gpu_memory->CanSnapshotReadOnlyBuffer(vaddr, size);
}

bool GpuMemoryCaptureSnapshotReadOnlyBuffer(uint64_t vaddr, uint64_t size, void* dst, uint64_t* validation_ns, uint64_t* copy_ns)
{
	return g_gpu_memory != nullptr && g_gpu_memory->CaptureSnapshotReadOnlyBuffer(vaddr, size, dst, validation_ns, copy_ns);
}

bool GpuMemoryCompareSnapshotReadOnlyBuffer(uint64_t vaddr, uint64_t size, const void* snapshot, bool* matches,
	                                        uint64_t* validation_ns, uint64_t* compare_ns)
{
	return g_gpu_memory != nullptr &&
	       g_gpu_memory->CompareSnapshotReadOnlyBuffer(vaddr, size, snapshot, matches, validation_ns, compare_ns);
}

void GpuMemoryFree(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	g_gpu_memory->Free(ctx, vaddr, size, GpuMemoryRangeReleaseMode::ObjectsOnly);
}

void GpuMemoryInvalidateMappedRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	g_gpu_memory->Free(ctx, vaddr, size, GpuMemoryRangeReleaseMode::PhysicalLifetime);
	LabelReleaseMappedRange(vaddr, size);
}

void GpuMemoryFreeMappedRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	g_gpu_memory->Free(ctx, vaddr, size, GpuMemoryRangeReleaseMode::Unmap);
	LabelReleaseMappedRange(vaddr, size);
}

void* GpuMemoryCreateObject(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, uint64_t vaddr, uint64_t size,
                            const GpuObject& info)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	return g_gpu_memory->CreateObject(submit_id, ctx, buffer, &vaddr, &size, 1, info);
}

void* GpuMemoryCreateObject(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
                            int vaddr_num, const GpuObject& info)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	return g_gpu_memory->CreateObject(submit_id, ctx, buffer, vaddr, size, vaddr_num, info);
}

Vector<GpuMemoryObject> GpuMemoryFindObjects(uint64_t vaddr, uint64_t size, GpuMemoryObjectType type, bool exact, bool only_first)
{
	EXIT_IF(g_gpu_memory == nullptr);

	return g_gpu_memory->FindObjects(&vaddr, &size, 1, type, exact, only_first);
}

Vector<GpuMemoryObject> GpuMemoryFindObjects(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type,
                                             bool exact, bool only_first)
{
	EXIT_IF(g_gpu_memory == nullptr);

	return g_gpu_memory->FindObjects(vaddr, size, vaddr_num, type, exact, only_first);
}

Vector<GpuMemoryObject> GpuMemoryFindObjectsForSubmission(SubmissionId submission, uint64_t vaddr, uint64_t size, GpuMemoryObjectType type,
                                                          bool exact, bool only_first)
{
	EXIT_IF(g_gpu_memory == nullptr);

	return g_gpu_memory->FindObjects(&vaddr, &size, 1, type, exact, only_first, &submission);
}

Vector<GpuMemoryObject> GpuMemoryFindObjectsForSubmission(SubmissionId submission, const uint64_t* vaddr, const uint64_t* size,
                                                          int vaddr_num, GpuMemoryObjectType type, bool exact, bool only_first)
{
	EXIT_IF(g_gpu_memory == nullptr);

	return g_gpu_memory->FindObjects(vaddr, size, vaddr_num, type, exact, only_first, &submission);
}

Vector<GpuMemoryObject> GpuMemoryFindObjectsForSubmission(CommandBuffer* buffer, uint64_t vaddr, uint64_t size, GpuMemoryObjectType type,
                                                          bool exact, bool only_first)
{
	EXIT_IF(buffer == nullptr);
	SubmissionId submission;
	if (!buffer->GetSubmissionId(&submission)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !buffer->GetSubmissionId(&submission) condition ignored (continuing)\n"); }
	return GpuMemoryFindObjectsForSubmission(submission, vaddr, size, type, exact, only_first);
}

Vector<GpuMemoryObject> GpuMemoryFindObjectsForSubmission(CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                                                          GpuMemoryObjectType type, bool exact, bool only_first)
{
	EXIT_IF(buffer == nullptr);
	SubmissionId submission;
	if (!buffer->GetSubmissionId(&submission)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !buffer->GetSubmissionId(&submission) condition ignored (continuing)\n"); }
	return GpuMemoryFindObjectsForSubmission(submission, vaddr, size, vaddr_num, type, exact, only_first);
}

bool GpuMemoryQueryOverlaps(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryOverlapSnapshot* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = GpuMemoryOverlapSnapshot {};

	return g_gpu_memory != nullptr && g_gpu_memory->QueryOverlaps(vaddr, size, vaddr_num, out);
}

bool GpuMemoryQueryRangeProvenance(uint64_t vaddr, uint64_t size, GpuMemoryRangeProvenance* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = GpuMemoryRangeProvenance {};
	return g_gpu_memory != nullptr && g_gpu_memory->QueryRangeProvenance(vaddr, size, out);
}

void GpuMemoryResetHash(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type)
{
	EXIT_IF(g_gpu_memory == nullptr);

	g_gpu_memory->ResetHash(vaddr, size, vaddr_num, type);
}

void GpuMemoryDbgDump()
{
	EXIT_IF(g_gpu_memory == nullptr);

	// g_gpu_memory->DbgDbDump();
	// g_gpu_memory->DbgDbSave(U"_gpu_memory.db");

	// static int test_ms = 0; // Core::mem_new_state();

	// Core::Thread::Sleep(2000);
	//	Core::MemStats test_mem_stat = {test_ms, 0, 0};
	//	Core::mem_get_stat(&test_mem_stat);
	//	size_t   ut_total_allocated = test_mem_stat.total_allocated;
	//	uint32_t ut_blocks_num      = test_mem_stat.blocks_num;
	//	std::printf("mem stat: state = %d, blocks_num = %u, total_allocated = %" PRIu64 "\n", test_ms, ut_blocks_num, ut_total_allocated);
	// Core::mem_print(6);
	// test_ms = Core::mem_new_state();
}

void GpuMemoryFlush(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	// update vulkan objects after CPU-drawing
	g_gpu_memory->Flush(ctx, vaddr, size);
}

void GpuMemoryFlushAll(GraphicContext* ctx)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	// update vulkan objects after CPU-drawing
	g_gpu_memory->FlushAll(ctx);
}

void GpuMemoryFrameDone(GraphicContext* ctx)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	g_gpu_memory->FrameDone(ctx);
}

void GpuMemoryFrameDone()
{
	GpuMemoryFrameDone(WindowGetGraphicContext());
}

void GpuMemoryWriteBackCompletedSubmission(GraphicContext* ctx, SubmissionId submission)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(submission.sequence == 0);

	g_gpu_memory->WriteBackCompletedSubmission(ctx, submission);
}

void GpuMemoryCompleteSubmission(SubmissionId submission)
{
	EXIT_IF(g_gpu_memory == nullptr);
	g_gpu_memory->CompleteSubmission(submission);
}

void GpuMemoryWriteBackStorageRange(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	g_gpu_memory->WriteBackStorageRange(ctx, vaddr, size);
}

bool GpuMemoryCheckAccessViolation(uint64_t vaddr)
{
	return GpuDirtyPageTracker::Instance().HandleWriteFault(vaddr);
}

bool GpuMemoryNotifyHostWrite(uint64_t vaddr, uint64_t size)
{
	return GpuDirtyPageTracker::Instance().NotifyWrite(vaddr, size);
}

bool GpuMemoryWatcherEnabled()
{
	return GpuDirtyPageTracker::Instance().Enabled();
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
