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
#include "Emulator/Graphics/GpuWriteHistory.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/StorageBuffer.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Profiler.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

void GpuMemory::FrameDone(GraphicContext* ctx)
{
	EXIT_IF(ctx == nullptr);

	constexpr uint64_t kRetireAfterFrames = 120;

	Vector<Destructor> destructors;
	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);

	m_current_frame++;
	if (m_current_frame < kRetireAfterFrames || (m_current_frame % 30u) != 0u)
	{
		return;
	}

	const uint32_t retire_batch_limit    = GpuMemoryRetirementBatchLimit(m_transient_creates_since_retirement);
	m_transient_creates_since_retirement = 0;
	uint32_t retired                     = 0;
	// Resume after the last examined slot so every object is reached across
	// passes. Cursors are positions, not identities; normalize after heap removal.
	uint32_t heaps_remaining  = m_heaps.Size();
	uint32_t visits_remaining = 4096u;
	while (heaps_remaining != 0 && visits_remaining != 0 && retired < retire_batch_limit)
	{
		visits_remaining--;
		if (m_retirement_heap_cursor >= m_heaps.Size())
		{
			m_retirement_heap_cursor   = 0;
			m_retirement_object_cursor = 0;
		}
		auto& heap = m_heaps[m_retirement_heap_cursor];
		if (m_retirement_object_cursor >= heap.objects.Size())
		{
			m_retirement_heap_cursor++;
			m_retirement_object_cursor = 0;
			heaps_remaining--;
			continue;
		}
		const int heap_id   = static_cast<int>(m_retirement_heap_cursor);
		const int object_id = static_cast<int>(m_retirement_object_cursor++);
		auto&     h         = heap.objects[object_id];
		if (h.free || h.scenario != GpuMemoryScenario::Common)
		{
			continue;
		}
		auto&      object                = h.info;
		const bool old_enough            = m_current_frame - object.use_last_frame >= kRetireAfterFrames;
		const bool dependencies_complete = m_deferred_deletions.AreDependenciesComplete(object.submission_uses.Dependencies());
		const bool owns_device_content   = object.in_use && !object.read_only;
		if (!h.others.IsEmpty())
		{
			// A read-only buffer owns no content: its bytes are guest memory or a
			// copy of a linked peer, and a pending GPU write keeps it writable
			// (GpuMemoryMergeReadOnlyUse). A writable buffer stops owning content
			// once its writes are written back (in_use clears). Retiring either
			// alone drops both link directions and leaves every peer intact, so
			// overlapping views cannot grow a linked graph without bound.
			if (GpuMemoryCanRetireLinkedBufferMember(object.object.type, owns_device_content, object.depth_meta_bound) && old_enough &&
			    dependencies_complete)
			{
				destructors.Add(Free(heap_id, object_id));
				retired++;
			}
			continue;
		}

		const bool reclaimable_type = object.object.type == GpuMemoryObjectType::Texture ||
		                              object.object.type == GpuMemoryObjectType::StorageTexture ||
		                              object.object.type == GpuMemoryObjectType::StorageBuffer;
		const bool storage_buffer_safe =
		    object.object.type != GpuMemoryObjectType::StorageBuffer || object.write_back_func == nullptr || !owns_device_content;
		if (reclaimable_type && storage_buffer_safe && old_enough && dependencies_complete)
		{
			destructors.Add(Free(heap_id, object_id));
			retired++;
		}
	}

	if (!destructors.IsEmpty())
	{
		ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
	}
}

void GpuMemory::WriteBackObjectLocked(GraphicContext* ctx, int heap_id, int object_id, Vector<Destructor>* destructors,
                                      const SubmissionId* publishing_submission)
{
	EXIT_IF(ctx == nullptr || destructors == nullptr);
	auto& heap = m_heaps[heap_id];
	auto& h    = heap.objects[object_id];
	EXIT_IF(h.free);
	auto& o = h.info;

	if (!o.in_use)
	{
		return;
	}
	EXIT_IF(o.write_back_func == nullptr || o.read_only);
	const bool dependencies_complete =
	    publishing_submission == nullptr
	        ? m_deferred_deletions.AreDependenciesComplete(o.submission_uses.Dependencies())
	        : m_deferred_deletions.AreDependenciesCompleteForPublication(o.submission_uses.Dependencies(), *publishing_submission);
	if (!dependencies_complete)
	{
		EXIT("GpuMemory write-back requested before exact resource dependencies completed: type=%s heap=%d id=%d\n",
		     Core::EnumName(o.object.type).C_Str(), heap_id, object_id);
	}

	auto& block = h.block;

	// Classify alias parents before touching GPU memory. Gen5 can attach
	// many VertexBuffer Crosses/IsContainedWithin links plus one Equals
	// RenderTexture peer to a RW StorageBuffer; only Equals parents get
	// full hash propagation. Parent count is not capped — post-logo FNA
	// dispose topologies exceed the former stack limit of 64.
	const uint32_t               parent_count = static_cast<uint32_t>(h.others.Size());
	Vector<GpuMemoryOverlapType> parent_rels;
	for (uint32_t oi = 0; oi < parent_count; oi++)
	{
		parent_rels.Add(h.others.At(static_cast<int>(oi)).relation);
	}
	bool     recompute_self   = true;
	uint32_t equals_count     = 0;
	uint32_t invalidate_count = 0;
	if (!GpuMemoryWriteBackClassifyParents(parent_rels.GetData(), parent_count, &recompute_self, &equals_count, &invalidate_count))
	{
		KYTY_LOG_DEBUG( "GpuMemory WriteBack unsupported parent relation in alias topology:\n");
		KYTY_LOG_DEBUG( "\t self: heap=%d id=%d type=%s others=%u\n", heap_id, object_id, Core::EnumName(o.object.type).C_Str(),
		             static_cast<unsigned>(h.others.Size()));
		for (uint32_t oi = 0; oi < h.others.Size(); oi++)
		{
			const auto& other = h.others.At(static_cast<int>(oi));
			KYTY_LOG_DEBUG( "\t other[%u]: id=%d relation=%s type=%s\n", oi, other.object_id, Core::EnumName(other.relation).C_Str(),
			             Core::EnumName(heap.objects[other.object_id].info.object.type).C_Str());
		}
		EXIT("WriteBack unsupported parent relation\n");
	}

	GpuWritebackResult writeback_result;
	{
		const auto writeback_start = std::chrono::steady_clock::now();
		// A uniform result already published to guest memory on the device needs
		// no read of the GPU copy (PublishComputeUniformFillToGuestAddress).
		const bool published_uniform = o.object.type == GpuMemoryObjectType::StorageBuffer && o.guest_published_uniform &&
		                               o.guest_published_write_uses != 0u && o.guest_published_write_uses == o.write_uses &&
		                               block.vaddr_num == 1 &&
		                               StorageBufferWriteBackPublishedUniform(o.object.obj, block.vaddr[0], block.size[0],
		                                                                      o.guest_published_words, &writeback_result);
		if (!published_uniform)
		{
			writeback_result = o.write_back_func(ctx, o.params, o.object.obj, block.vaddr, block.size, block.vaddr_num);
		}
		const auto writeback_elapsed =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - writeback_start).count();
		DebugStatsRecordGpuMemoryWriteBack(GpuMemoryStatsTypeIndex(o.object.type), writeback_result.copied_bytes,
		                                   static_cast<uint64_t>(writeback_elapsed));
	}
	if (!writeback_result.content_changed)
	{
		o.in_use = false;
		SyncWriteBackIndexes(heap_id, object_id);
		return;
	}
	const uint64_t content_sequence = NextContentSequence();
	o.cpu_update_time               = GpuMemoryGetCurrentTime();
	o.content_origin                = GpuMemoryContentOrigin::GpuWriteBack;
	o.content_sequence              = content_sequence;
	const auto history_kind = o.object.type == GpuMemoryObjectType::StorageBuffer
	                              ? GpuWriteHistoryKind::StorageWriteBack
	                              : (o.object.type == GpuMemoryObjectType::RenderTexture
	                                     ? GpuWriteHistoryKind::RenderTargetWriteBack
	                                     : GpuWriteHistoryKind::OtherWriteBack);
	for (int vi = 0; vi < block.vaddr_num; ++vi)
	{
		GpuWriteHistoryRecord(history_kind, block.vaddr[vi], block.size[vi], o.submit_id,
		                     static_cast<uint32_t>(o.object.type), content_sequence);
	}

	// Invalidate or propagate each parent according to its relation.
	// GPU-owned tiled RTs cannot be reconstructed from guest bytes.
	// Linked DepthStencilBuffer parents of a pending StorageBuffer use the same
	// walk: non-Equals relations zero hash/submit_id so the next depth bind
	// reloads from the published guest bytes after this write-back.
	for (uint32_t oi = 0; oi < h.others.Size(); oi++)
	{
		const auto& other  = h.others.At(static_cast<int>(oi));
		auto&       parent = heap.objects[other.object_id];
		EXIT_IF(parent.free);
		auto& o2 = parent.info;
		if (GpuMemorySkipWriteBackParentInvalidate(o2.object.type, o2.params))
		{
			continue;
		}
		// Completion runs after later submissions were recorded. A parent the
		// device wrote after this object's last write already holds newer bytes:
		// take the written-back guest bytes as its baseline instead of reloading
		// them, now or at its next use, over that device content.
		if (o2.device_write_time > o.write_time)
		{
			for (int vi = 0; vi < parent.block.vaddr_num; vi++)
			{
				if (o2.dirty_registered)
				{
					const auto read = GpuDirtyPageTracker::Instance().BeginRead(parent.block.vaddr[vi], parent.block.size[vi]);
					if (read.tracked)
					{
						o2.dirty_generation[vi] = read.generation;
					}
				}
				if (o2.check_hash)
				{
					o2.hash[vi] = GpuMemoryCalcHash(o2.object.type, reinterpret_cast<const uint8_t*>(parent.block.vaddr[vi]),
					                                parent.block.size[vi]);
				}
			}
			continue;
		}
		o2.cpu_update_time  = o.cpu_update_time;
		o2.submit_id        = 0;
		o2.content_origin   = GpuMemoryContentOrigin::AliasInvalidation;
		o2.content_sequence = content_sequence;
		for (int vi = 0; vi < parent.block.vaddr_num; vi++)
		{
			o2.hash[vi] = 0;
		}
		if (GpuMemoryWriteBackParentActionFor(other.relation) == GpuMemoryWriteBackParentAction::PropagateEquals)
		{
			Update(o.submit_id, ctx, heap_id, other.object_id, destructors);
		}
	}

	if (recompute_self)
	{
		for (int vi = 0; vi < block.vaddr_num; vi++)
		{
			uint64_t new_hash = 0;
			if (o.check_hash)
			{
				new_hash = GpuMemoryCalcHash(o.object.type, reinterpret_cast<const uint8_t*>(block.vaddr[vi]), block.size[vi]);
			}
			KYTY_LOG_DEBUG("WriteBack (GPU -> CPU): type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 ", old_hash = 0x%016" PRIx64
			       ", new_hash = 0x%016" PRIx64 ", equals=%u invalidate=%u\n",
			       Core::EnumName(o.object.type).C_Str(), block.vaddr[vi], block.size[vi], o.hash[vi], new_hash, equals_count,
			       invalidate_count);
			o.hash[vi] = new_hash;
		}
	} else
	{
		bool copied = false;
		for (uint32_t oi = 0; oi < h.others.Size() && !copied; oi++)
		{
			const auto& other = h.others.At(static_cast<int>(oi));
			if (other.relation != OverlapType::Equals)
			{
				continue;
			}
			const auto& o2 = heap.objects[other.object_id].info;
			for (int vi = 0; vi < block.vaddr_num; vi++)
			{
				const uint64_t new_hash = o2.hash[vi];
				KYTY_LOG_DEBUG("WriteBack (GPU -> CPU): type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 ", old_hash = 0x%016" PRIx64
				       ", new_hash = 0x%016" PRIx64 ", equals=%u invalidate=%u\n",
				       Core::EnumName(o.object.type).C_Str(), block.vaddr[vi], block.size[vi], o.hash[vi], new_hash, equals_count,
				       invalidate_count);
				o.hash[vi] = new_hash;
			}
			copied = true;
		}
		EXIT_IF(!copied);
	}

	o.in_use = false;
	SyncWriteBackIndexes(heap_id, object_id);
}

void GpuMemory::WriteBackCompletedSubmission(GraphicContext* ctx, SubmissionId submission)
{
	EXIT_IF(submission.sequence == 0);
	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;

	// Write-back changes the index; collect the candidates first.
	std::vector<std::pair<int, int>> objects;
	for (const auto& [heap_id, object_id]: m_pending_write_back)
	{
		const auto& o = m_heaps[heap_id].objects[object_id].info;
		if (GpuMemoryCanWriteBackAtSubmission(o.submission_uses, m_deferred_deletions, submission))
		{
			objects.emplace_back(heap_id, object_id);
		}
	}

	for (const auto& [heap_id, object_id]: objects)
	{
		WriteBackObjectLocked(ctx, heap_id, object_id, &destructors, &submission);
	}

	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
}

void GpuMemory::WriteBackAllCompleted(GraphicContext* ctx)
{
	EXIT_IF(ctx == nullptr);
	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;

	const std::vector<std::pair<int, int>> objects(m_pending_write_back.begin(), m_pending_write_back.end());
	for (const auto& [heap_id, object_id]: objects)
	{
		const auto& object = m_heaps[heap_id].objects[object_id].info;
		if (!m_deferred_deletions.AreDependenciesComplete(object.submission_uses.Dependencies()))
		{
			EXIT("GpuMemory all-completed write-back still has a pending resource use: type=%s\n",
			     Core::EnumName(object.object.type).C_Str());
		}
	}

	for (const auto& [heap_id, object_id]: objects)
	{
		WriteBackObjectLocked(ctx, heap_id, object_id, &destructors);
	}
	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
}

void GpuMemory::Flush(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;

	int heap_id = GetHeapId(vaddr, size);

	if (heap_id < 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: heap_id < 0 condition ignored (continuing)\n"); }

	auto& heap = m_heaps[heap_id];

	auto object_ids = FindBlocks(heap_id, &vaddr, &size, 1);

	for (const auto& obj: object_ids)
	{
		auto& h = heap.objects[obj.object_id];
		EXIT_IF(h.free);
		// A flush is the observed CPU/CP write event. Publish linked HTILE
		// semantics before Update can discard byte-identical content by hash.
		if (h.info.object.type == GpuMemoryObjectType::StorageBuffer && h.block.vaddr_num == 1)
		{
			const auto* storage = static_cast<const StorageVulkanBuffer*>(h.info.object.obj);
			if (storage != nullptr)
			{
				(void)DepthMetaObserveStorageFlush(h.block.vaddr[0], h.block.size[0], storage->depth_meta_addr,
				                                   storage->depth_meta_size, reinterpret_cast<const void*>(h.block.vaddr[0]), vaddr, size);
			}
		}

		Update(UINT64_MAX, ctx, heap_id, obj.object_id, &destructors);
	}
	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
}

// True when [vaddr, vaddr + size) meets one of the sorted, disjoint ranges.
static bool OverlapsSortedRanges(const GpuMemoryGuestRanges& ranges, uint64_t vaddr, uint64_t size)
{
	const uint64_t end   = vaddr + size;
	const auto     after = std::lower_bound(ranges.begin(), ranges.end(), end,
	                                        [](const std::pair<uint64_t, uint64_t>& range, uint64_t value) { return range.first < value; });
	if (after == ranges.begin())
	{
		return false;
	}
	const auto& range = *std::prev(after);
	return range.first + range.second > vaddr;
}

std::vector<std::pair<int, int>> GpuMemory::CollectWritableStorage(const GpuMemoryGuestRanges& ranges, GpuQueueId consumer) const
{
	std::vector<std::pair<int, int>> found;
	const auto&                      heaps = m_heaps;
	for (const auto& [heap_id, object_id]: m_writable_storage)
	{
		const auto& object = heaps[heap_id].objects[object_id];
		EXIT_IF(object.free);
		const auto& info = object.info;
		if (!info.in_use || info.read_only || info.object.obj == nullptr)
		{
			continue;
		}
		if (info.guest_published_write_uses != 0u && info.guest_published_write_uses == info.write_uses &&
		    info.guest_published_queue == consumer.Value())
		{
			continue;
		}
		for (int block = 0; block < object.block.vaddr_num; block++)
		{
			if (object.block.size[block] != 0 && OverlapsSortedRanges(ranges, object.block.vaddr[block], object.block.size[block]))
			{
				found.emplace_back(heap_id, object_id);
				break;
			}
		}
	}
	return found;
}

bool GpuMemory::PendingStorageWriteBack(const GpuMemoryGuestRanges& ranges, GpuQueueId consumer, SubmissionId* dependency)
{
	EXIT_IF(dependency == nullptr);
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
	const auto&     heaps = m_heaps;
	for (const auto& [heap_id, object_id]: CollectWritableStorage(ranges, consumer))
	{
		for (const auto& use: heaps[heap_id].objects[object_id].info.submission_uses.Dependencies())
		{
			if (m_deferred_deletions.AreDependenciesComplete({use})) { continue; }
			*dependency = use;
			return true;
		}
	}
	return false;
}

void GpuMemory::WriteBackStorageRanges(GraphicContext* ctx, const GpuMemoryGuestRanges& ranges, GpuQueueId consumer)
{
	EXIT_IF(ctx == nullptr);
	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;
	for (const auto& [heap_id, object_id]: CollectWritableStorage(ranges, consumer))
	{
		WriteBackObjectLocked(ctx, heap_id, object_id, &destructors);
	}
	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
}

bool GpuMemory::FindExactWritableStorage(uint64_t vaddr, uint64_t size, GpuMemoryStorageWriteIdentity* identity)
{
	EXIT_IF(identity == nullptr);
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
	for (const auto& [heap_id, object_id]: m_writable_storage)
	{
		const auto& object = m_heaps[heap_id].objects[object_id];
		const auto& info   = object.info;
		if (object.free || !info.in_use || info.read_only || info.object.obj == nullptr || object.block.vaddr_num != 1 ||
		    object.block.vaddr[0] != vaddr || object.block.size[0] != size)
		{
			continue;
		}
		identity->heap_id            = heap_id;
		identity->object_id          = object_id;
		identity->logical_generation = info.logical_generation;
		identity->write_uses         = info.write_uses;
		identity->buffer             = static_cast<const VulkanBuffer*>(info.object.obj);
		return true;
	}
	return false;
}

bool GpuMemory::MarkStorageGuestPublished(const GpuMemoryStorageWriteIdentity& identity, GpuQueueId queue,
                                          const GpuWritebackPageCache::UniformWords* uniform_words)
{
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
	if (identity.heap_id < 0 || static_cast<uint32_t>(identity.heap_id) >= m_heaps.Size() || identity.object_id < 0 ||
	    static_cast<uint32_t>(identity.object_id) >= m_heaps[identity.heap_id].objects.Size())
	{
		return false;
	}
	auto& object = m_heaps[identity.heap_id].objects[identity.object_id];
	auto& info   = object.info;
	if (object.free || info.logical_generation != identity.logical_generation || info.write_uses != identity.write_uses ||
	    !info.in_use || info.read_only || info.object.obj != identity.buffer || identity.write_uses == 0u || info.depth_meta_bound)
	{
		return false;
	}
	// Device-address consumers then skip the in-order write-back, and an object
	// rewritten every frame may not complete one for a long time. That is sound
	// only when the write-back has nothing else to update: every overlapping
	// object must be an exact image alias the device wrote after this object's
	// last write (the fill's propagated clear or later rendering). Depth/HTILE,
	// buffer and partial aliases keep the in-order write-back.
	for (const auto& other: object.others)
	{
		const auto& alias = m_heaps[identity.heap_id].objects[other.object_id];
		const auto  type  = alias.info.object.type;
		if (alias.free || other.relation != OverlapType::Equals ||
		    (type != GpuMemoryObjectType::RenderTexture && type != GpuMemoryObjectType::Texture) ||
		    alias.info.device_write_time <= info.write_time)
		{
			return false;
		}
	}
	info.guest_published_write_uses = identity.write_uses;
	info.guest_published_queue       = queue.Value();
	info.guest_published_uniform     = uniform_words != nullptr;
	info.guest_published_words       = uniform_words != nullptr ? *uniform_words : GpuWritebackPageCache::UniformWords {};
	return true;
}

void GpuMemory::FlushAll(GraphicContext* ctx)
{
	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;

	int heap_id = 0;
	for (auto& heap: m_heaps)
	{
		int index = 0;
		for (auto& h: heap.objects)
		{
			if (!h.free)
			{
				Update(UINT64_MAX, ctx, heap_id, index, &destructors);
			}
			index++;
		}
		heap_id++;
	}
	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
