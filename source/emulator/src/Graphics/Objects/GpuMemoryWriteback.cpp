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
	int      heap_id                     = 0;
	for (auto& heap: m_heaps)
	{
		int object_id = 0;
		for (auto& h: heap.objects)
		{
			if (retired >= retire_batch_limit)
			{
				break;
			}
			if (h.free || h.scenario != GpuMemoryScenario::Common || !h.others.IsEmpty())
			{
				object_id++;
				continue;
			}

			auto&      object           = h.info;
			const bool reclaimable_type = object.object.type == GpuMemoryObjectType::Texture ||
			                              object.object.type == GpuMemoryObjectType::StorageTexture ||
			                              object.object.type == GpuMemoryObjectType::StorageBuffer;
			const bool storage_buffer_safe =
			    object.object.type != GpuMemoryObjectType::StorageBuffer || object.write_back_func == nullptr || object.read_only;
			const bool old_enough            = m_current_frame - object.use_last_frame >= kRetireAfterFrames;
			const bool dependencies_complete = m_deferred_deletions.AreDependenciesComplete(object.submission_uses.Dependencies());
			if (reclaimable_type && storage_buffer_safe && old_enough && dependencies_complete)
			{
				destructors.Add(Free(heap_id, object_id));
				retired++;
			}
			object_id++;
		}
		heap_id++;
		if (retired >= retire_batch_limit)
		{
			break;
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
		writeback_result           = o.write_back_func(ctx, o.params, o.object.obj, block.vaddr, block.size, block.vaddr_num);
		const auto writeback_elapsed =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - writeback_start).count();
		DebugStatsRecordGpuMemoryWriteBack(GpuMemoryStatsTypeIndex(o.object.type), writeback_result.copied_bytes,
		                                   static_cast<uint64_t>(writeback_elapsed));
	}
	if (!writeback_result.content_changed)
	{
		o.in_use = false;
		return;
	}
	o.cpu_update_time = GpuMemoryGetCurrentTime();

	// Invalidate or propagate each parent according to its relation.
	// GPU-owned tiled RTs cannot be reconstructed from guest bytes.
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
		o2.cpu_update_time = o.cpu_update_time;
		o2.submit_id       = 0;
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
			printf("WriteBack (GPU -> CPU): type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 ", old_hash = 0x%016" PRIx64
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
				printf("WriteBack (GPU -> CPU): type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 ", old_hash = 0x%016" PRIx64
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
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void GpuMemory::WriteBackCompletedSubmission(GraphicContext* ctx, SubmissionId submission)
{
	EXIT_IF(submission.sequence == 0);
	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;

	struct WriteBackObject
	{
		int heap_id   = -1;
		int object_id = -1;
	};

	Vector<WriteBackObject> objects;

	int heap_id = 0;
	for (auto& heap: m_heaps)
	{
		int index = 0;
		for (auto& h: heap.objects)
		{
			if (!h.free)
			{
				auto& o = h.info;
				if (o.in_use && o.write_back_func != nullptr && !o.read_only)
				{
					SubmissionId queue_use;
					if (o.submission_uses.LatestForQueue(submission.queue, &queue_use))
					{
						if (queue_use.sequence > submission.sequence)
						{
							EXIT("GpuMemory write-back crossed a later same-queue use: type=%s completing=%" PRIu64 " latest=%" PRIu64
							     " queue=%" PRIu32 "\n",
							     Core::EnumName(o.object.type).C_Str(), submission.sequence, queue_use.sequence, submission.queue.Value());
						}
						for (const auto& dependency: o.submission_uses.Dependencies())
						{
							if (dependency.queue == submission.queue)
							{
								continue;
							}
							const std::vector<SubmissionId> exact_dependency {dependency};
							if (!m_deferred_deletions.AreDependenciesComplete(exact_dependency))
							{
								EXIT("GpuMemory write-back has an unordered cross-queue use: type=%s completing_queue=%" PRIu32
								     " completing_sequence=%" PRIu64 " blocking_queue=%" PRIu32 " blocking_sequence=%" PRIu64 "\n",
								     Core::EnumName(o.object.type).C_Str(), submission.queue.Value(), submission.sequence,
								     dependency.queue.Value(), dependency.sequence);
							}
						}
						objects.Add(WriteBackObject({heap_id, index}));
					}
				}
			}
			index++;
		}
		heap_id++;
	}

	for (const auto& object: objects)
	{
		WriteBackObjectLocked(ctx, object.heap_id, object.object_id, &destructors, &submission);
	}

	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
}

void GpuMemory::WriteBackAllCompleted(GraphicContext* ctx)
{
	EXIT_IF(ctx == nullptr);
	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;

	struct WriteBackObject
	{
		int heap_id   = -1;
		int object_id = -1;
	};
	Vector<WriteBackObject> objects;

	int heap_id = 0;
	for (auto& heap: m_heaps)
	{
		int object_id = 0;
		for (auto& h: heap.objects)
		{
			if (!h.free)
			{
				auto& object = h.info;
				if (object.in_use && object.write_back_func != nullptr && !object.read_only)
				{
					if (!m_deferred_deletions.AreDependenciesComplete(object.submission_uses.Dependencies()))
					{
						EXIT("GpuMemory all-completed write-back still has a pending resource use: type=%s\n",
						     Core::EnumName(object.object.type).C_Str());
					}
					objects.Add(WriteBackObject({heap_id, object_id}));
				}
			}
			object_id++;
		}
		heap_id++;
	}

	for (const auto& object: objects)
	{
		WriteBackObjectLocked(ctx, object.heap_id, object.object_id, &destructors);
	}
	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
}

void GpuMemory::Flush(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;

	int heap_id = GetHeapId(vaddr, size);

	EXIT_NOT_IMPLEMENTED(heap_id < 0);

	auto& heap = m_heaps[heap_id];

	auto object_ids = FindBlocks(heap_id, &vaddr, &size, 1);

	for (const auto& obj: object_ids)
	{
		auto& h = heap.objects[obj.object_id];
		EXIT_IF(h.free);

		Update(UINT64_MAX, ctx, heap_id, obj.object_id, &destructors);
	}
	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
}

void GpuMemory::WriteBackStorageRange(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(ctx == nullptr);
	if (size == 0)
	{
		return;
	}

	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;

	const int heap_id = GetHeapId(vaddr, size);
	if (heap_id < 0)
	{
		return;
	}

	auto& heap       = m_heaps[heap_id];
	auto  object_ids = FindBlocks(heap_id, &vaddr, &size, 1);

	for (const auto& obj: object_ids)
	{
		auto& h = heap.objects[obj.object_id];
		EXIT_IF(h.free);
		auto& o = h.info;
		if (o.object.type != GpuMemoryObjectType::StorageBuffer || !o.in_use || o.write_back_func == nullptr || o.read_only ||
		    o.object.obj == nullptr)
		{
			continue;
		}
		WriteBackObjectLocked(ctx, heap_id, obj.object_id, &destructors);
	}
	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
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
