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

uint64_t GpuMemoryCalcHash(GpuMemoryObjectType type, const uint8_t* buf, uint64_t size)
{
	KYTY_PROFILER_FUNCTION();

	if (size == 0 || buf == nullptr)
	{
		return 0;
	}
	if (std::getenv("KYTY_DUMP_HASH_RANGE") != nullptr)
	{
		static std::atomic_uint dump_count {0};
		const unsigned         ordinal = dump_count.fetch_add(1, std::memory_order_relaxed);
		if (ordinal < 128u)
		{
			Emulator::GuestMemory::MappedRange mapped {};
			const bool mapped_range = Emulator::GuestMemory::GetPort().QueryMappedRange(reinterpret_cast<uint64_t>(buf), size, &mapped);
			void*      protection_start = nullptr;
			void*      protection_end   = nullptr;
			int        protection       = 0;
			const int  protection_result = Emulator::GuestMemory::GetPort().QueryProtection(const_cast<uint8_t*>(buf), &protection_start,
			                                                                                &protection_end, &protection);
			KYTY_LOG_DEBUG(
			             "KYTY_DUMP_HASH_RANGE ordinal=%u type=%u buf=0x%012" PRIx64 " size=0x%012" PRIx64
			             " mapped=%u base=0x%012" PRIx64 " map_size=0x%012" PRIx64 " protection_result=%d start=0x%012" PRIx64
			             " end=0x%012" PRIx64 " prot=0x%x\n",
			             ordinal, static_cast<unsigned>(type), reinterpret_cast<uint64_t>(buf), size, mapped_range ? 1u : 0u,
			             mapped.base, mapped.size, protection_result, reinterpret_cast<uint64_t>(protection_start),
			             reinterpret_cast<uint64_t>(protection_end), protection);
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
	EXIT_NOT_IMPLEMENTED(IsAllocated(vaddr, size));

	Core::LockGuard lock(m_mutex);

	Heap h;
	h.range.vaddr   = vaddr;
	h.range.size    = size;
	h.objects_map1  = new GpuMap1;
	h.objects_map2  = new GpuMap2;
	h.overlap_cache = new OverlapQueryCache;

	m_heaps.Add(h);
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
	for (const auto& heap: m_heaps)
	{
		if (vaddr < heap.range.vaddr)
		{
			continue;
		}
		const uint64_t offset = vaddr - heap.range.vaddr;
		if (offset < heap.range.size && size <= heap.range.size - offset)
		{
			return GpuMemoryRangeValidationStatus::Valid;
		}
	}
	return GpuMemoryRangeValidationStatus::Unallocated;
}

uint64_t GpuMemory::GetAllocatedRangePrefix(uint64_t vaddr, uint64_t maximum_size)
{
	if (maximum_size == 0)
	{
		return 0;
	}

	Core::LockGuard lock(m_mutex);
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
		return available < maximum_size ? available : maximum_size;
	}
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

void GpuMemory::Free(GraphicContext* ctx, uint64_t vaddr, uint64_t size, bool unmap)
{
	KYTY_PROFILER_BLOCK("GpuMemory::Free", profiler::colors::Green300);

	if (unmap)
	{
		// KernelMunmap holds the GPU admission gate and drains every queue before
		// entering this teardown. Do not wait again here: the gate must remain
		// owned continuously through write-back, detach, and host VA release.
		WriteBackAllCompleted(ctx);
	}

	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	m_mutex.Lock();

	KYTY_LOG_DEBUG("Release gpu objects:\n");
	KYTY_LOG_DEBUG("\t gpu_vaddr = 0x%016" PRIx64 "\n", vaddr);
	KYTY_LOG_DEBUG("\t size   = 0x%016" PRIx64 "\n", size);

	int heap_id = GetHeapId(vaddr, size);

	EXIT_NOT_IMPLEMENTED(heap_id < 0);

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

	if (unmap)
	{
		EXIT_NOT_IMPLEMENTED(!IsAllocated(vaddr, size));

		int index = 0;
		for (auto& a: m_heaps)
		{
			if (a.range.vaddr == vaddr && a.range.size == size)
			{
				EXIT_IF(a.objects_map1 == nullptr);
				EXIT_IF(a.objects_map2 == nullptr);
				EXIT_IF(a.overlap_cache == nullptr);
				EXIT_NOT_IMPLEMENTED(heap_id != index);
				EXIT_NOT_IMPLEMENTED(a.objects_size != 0);
				EXIT_NOT_IMPLEMENTED(!a.objects_map1->IsEmpty());
				EXIT_NOT_IMPLEMENTED(!a.objects_map2->IsEmpty());

				delete a.objects_map1;
				delete a.objects_map2;
				delete a.overlap_cache;

				m_heaps.RemoveAt(index);
				break;
			}
			index++;
		}

		EXIT_NOT_IMPLEMENTED(IsAllocated(vaddr, size));
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

void GpuMemoryFree(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	g_gpu_memory->Free(ctx, vaddr, size, false);
}

void GpuMemoryFreeMappedRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_gpu_memory == nullptr);
	EXIT_IF(ctx == nullptr);

	g_gpu_memory->Free(ctx, vaddr, size, true);
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
	EXIT_NOT_IMPLEMENTED(!buffer->GetSubmissionId(&submission));
	return GpuMemoryFindObjectsForSubmission(submission, vaddr, size, type, exact, only_first);
}

Vector<GpuMemoryObject> GpuMemoryFindObjectsForSubmission(CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                                                          GpuMemoryObjectType type, bool exact, bool only_first)
{
	EXIT_IF(buffer == nullptr);
	SubmissionId submission;
	EXIT_NOT_IMPLEMENTED(!buffer->GetSubmissionId(&submission));
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

void GpuMemoryFrameDone()
{
	EXIT_IF(g_gpu_memory == nullptr);

	g_gpu_memory->FrameDone(WindowGetGraphicContext());
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
