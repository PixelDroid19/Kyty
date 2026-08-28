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
#include "Emulator/Graphics/Objects/RenderTexture.h"
#include "Emulator/Graphics/Objects/StorageTexture.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"

#include "GpuMemoryInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <vulkan/vk_enum_string_helper.h>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static_assert(GpuObject::PARAMS_MAX == GpuMemoryMaterializationKey::MaxParams,
              "materialization keys must cover every GPU object parameter");

void GpuMemory::Link(int heap_id, int id1, int id2, OverlapType rel, GpuMemoryScenario scenario)
{
	OverlapType other_rel = GpuMemoryReverseOverlap(rel);
	EXIT_IF(other_rel == OverlapType::None);

	auto& heap = m_heaps[heap_id];

	auto& h1 = heap.objects[id1];
	EXIT_IF(h1.free);

	auto& h2 = heap.objects[id2];
	EXIT_IF(h2.free);

	h1.others.Add({rel, id2});
	h2.others.Add({other_rel, id1});

	h1.scenario = scenario;
	h2.scenario = scenario;
}

DebugStatsGpuMemoryLinkedTopology GpuMemory::ClassifyLinkedStorageTopology(
    int heap_id, const Vector<OverlappedBlock>& parents, const GpuObject& incoming) const
{
	constexpr uint32_t MaxVisited       = 64u;
	constexpr uint32_t MaxExaminedEdges = 128u;
	if (heap_id < 0 || static_cast<uint32_t>(heap_id) >= m_heaps.Size() || incoming.type != GpuMemoryObjectType::StorageBuffer ||
	    parents.IsEmpty())
	{
		return DebugStatsGpuMemoryLinkedTopology::MutableOrOther;
	}

	const auto&               heap = m_heaps[heap_id];
	std::array<int, MaxVisited> pending {};
	uint32_t                  pending_size = 0;
	uint32_t                  next         = 0;
	uint32_t                  examined_edges = 0;
	bool                      mutable_or_other = !incoming.read_only;
	bool                      truncated        = false;

	const auto enqueue = [&](int object_id)
	{
		if (object_id < 0 || static_cast<uint32_t>(object_id) >= heap.objects.Size())
		{
			mutable_or_other = true;
			return;
		}
		for (uint32_t i = 0; i < pending_size; ++i)
		{
			if (pending[i] == object_id)
			{
				return;
			}
		}
		if (pending_size >= MaxVisited)
		{
			truncated = true;
			return;
		}
		pending[pending_size++] = object_id;
	};
	const auto examine_and_enqueue = [&](int object_id)
	{
		if (examined_edges >= MaxExaminedEdges)
		{
			truncated = true;
			return false;
		}
		examined_edges++;
		enqueue(object_id);
		return !truncated;
	};

	for (const auto& parent: parents)
	{
		if (!examine_and_enqueue(parent.object_id))
		{
			break;
		}
	}

	while (next < pending_size && !truncated)
	{
		const auto& object = heap.objects[pending[next++]];
		if (object.free)
		{
			mutable_or_other = true;
			continue;
		}

		const auto type = object.info.object.type;
		if (type == GpuMemoryObjectType::VideoOutBuffer || type == GpuMemoryObjectType::DepthStencilBuffer ||
		    type == GpuMemoryObjectType::Texture || type == GpuMemoryObjectType::RenderTexture ||
		    type == GpuMemoryObjectType::StorageTexture)
		{
			return DebugStatsGpuMemoryLinkedTopology::SurfaceConnected;
		}
		if (type != GpuMemoryObjectType::StorageBuffer && type != GpuMemoryObjectType::VertexBuffer &&
		    type != GpuMemoryObjectType::IndexBuffer)
		{
			mutable_or_other = true;
		}
		if (!object.info.read_only)
		{
			mutable_or_other = true;
		}
		for (const auto& link: object.others)
		{
			if (!examine_and_enqueue(link.object_id))
			{
				break;
			}
		}
	}

	if (truncated)
	{
		return DebugStatsGpuMemoryLinkedTopology::TraversalTruncated;
	}
	return mutable_or_other ? DebugStatsGpuMemoryLinkedTopology::MutableOrOther
	                        : DebugStatsGpuMemoryLinkedTopology::BufferOnlyReadOnly;
}

void GpuMemory::VersionBacking(GraphicContext* ctx, int heap_id, int obj_id, Vector<Destructor>* destructors,
	                           const uint64_t* source_vaddr)
{
	EXIT_IF(destructors == nullptr);

	auto& heap = m_heaps[heap_id];
	auto& h    = heap.objects[obj_id];
	EXIT_IF(h.free);
	auto& o = h.info;

	if (o.object.obj == nullptr || o.create_func == nullptr || o.delete_func == nullptr)
	{
		EXIT("GpuMemory backing version unsupported for type=%s: object=%s create=%s delete=%s\n", Core::EnumName(o.object.type).C_Str(),
		     o.object.obj != nullptr ? "set" : "null", o.create_func != nullptr ? "set" : "null",
		     o.delete_func != nullptr ? "set" : "null");
	}
	EXIT_IF(o.write_back_func != nullptr && !o.read_only);

	const auto old_object     = o.object.obj;
	const auto old_generation = o.backing_generation;
	const auto create_func    = o.create_func;
	const auto delete_func    = o.delete_func;
	const auto object_type    = o.object.type;

	uint64_t params[GpuObject::PARAMS_MAX] = {};
	uint64_t vaddr[VADDR_BLOCKS_MAX]       = {};
	uint64_t size[VADDR_BLOCKS_MAX]        = {};
	for (int i = 0; i < GpuObject::PARAMS_MAX; i++)
	{
		params[i] = o.params[i];
	}
	for (int i = 0; i < h.block.vaddr_num; i++)
	{
		vaddr[i] = source_vaddr != nullptr ? source_vaddr[i] : h.block.vaddr[i];
		size[i]  = h.block.size[i];
	}
	const int vaddr_num = h.block.vaddr_num;

	// Host creation/upload may block on its private transfer fence. Keep the
	// logical object stable with m_backing_mutation_mutex, but never hold the
	// global GpuMemory mutex across that work.
	m_mutex.Unlock();
	VulkanMemory new_memory {};
	const auto   create_start = std::chrono::steady_clock::now();
	void* const  new_object   = create_func(ctx, params, vaddr, size, vaddr_num, &new_memory);
	const auto   create_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - create_start).count();
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::CreateFunc, static_cast<uint64_t>(create_ns));
	m_mutex.Lock();

	auto&      current_heap = m_heaps[heap_id];
	auto&      current      = current_heap.objects[obj_id];
	const bool valid        = !current.free && current.info.object.type == object_type && current.info.object.obj == old_object &&
	                          current.info.backing_generation == old_generation;
	if (!valid || new_object == nullptr || new_object == old_object)
	{
		m_mutex.Unlock();
		if (new_object != nullptr && new_object != old_object)
		{
			delete_func(ctx, new_object, &new_memory);
		}
		m_mutex.Lock();
		if (new_object == nullptr || new_object == old_object)
		{
			EXIT("GpuMemory backing version factory did not create a distinct backing: type=%s heap=%d id=%d old=%p new=%p\n",
			     Core::EnumName(object_type).C_Str(), heap_id, obj_id, old_object, new_object);
		}
		EXIT("GpuMemory backing version transaction lost logical identity: type=%s heap=%d id=%d generation=%" PRIu64 "\n",
		     Core::EnumName(object_type).C_Str(), heap_id, obj_id, old_generation);
	}

	auto&      current_info = current.info;
	Destructor retired;
	retired.obj             = current_info.object.obj;
	retired.delete_func     = current_info.delete_func;
	retired.type            = current_info.object.type;
	retired.submission_uses = current_info.submission_uses;
	retired.mem             = current_info.mem;

	current_info.object.obj      = new_object;
	current_info.mem             = new_memory;
	current_info.submission_uses = {};
	EXIT_IF(current_info.backing_generation == UINT64_MAX);
	current_info.backing_generation++;
	destructors->Add(retired);
}

void GpuMemory::Update(uint64_t submit_id, GraphicContext* ctx, int heap_id, int obj_id, Vector<Destructor>* destructors)
{
	KYTY_PROFILER_BLOCK("GpuMemory::Update");

	auto& heap = m_heaps[heap_id];

	auto& h           = heap.objects[obj_id];
	auto& o           = h.info;
	bool  need_update = false;

	bool mem_watch = false;

	if ((mem_watch && o.cpu_update_time > o.gpu_update_time) || (!mem_watch && submit_id > o.submit_id))
	{
		uint64_t                hash[VADDR_BLOCKS_MAX] = {};
		GpuDirtyReadObservation dirty_read[VADDR_BLOCKS_MAX] {};
		bool                    hash_compared[VADDR_BLOCKS_MAX] = {};
		bool                    hash_tracked[VADDR_BLOCKS_MAX]  = {};
		bool                    tracker_ready                   = o.check_hash && o.dirty_registered;
		uint64_t                upload_vaddr[VADDR_BLOCKS_MAX]  = {};
		std::vector<uint8_t>    stable_buffer_source;
		bool                    stable_buffer_source_ready = false;
		for (int vi = 0; vi < h.block.vaddr_num; vi++)
		{
			upload_vaddr[vi] = h.block.vaddr[vi];
		}
		const bool page_fault_tracking =
		    tracker_ready && h.block.vaddr_num == 1 &&
		    GpuDirtyPageTracker::Instance().Mode(h.block.vaddr[0], h.block.size[0]) == GpuDirtyTrackingMode::PageFault;
		const bool stage_stable_buffer =
		    page_fault_tracking && o.read_only &&
		    (o.object.type == GpuMemoryObjectType::VertexBuffer || o.object.type == GpuMemoryObjectType::IndexBuffer) &&
		    GpuMemoryCanUseTransientReadOnlyBuffer(true, h.block.size[0], true, true);

		for (int vi = 0; vi < h.block.vaddr_num; vi++)
		{
			EXIT_IF(h.block.size[vi] == 0);

			bool clean = false;
			if (tracker_ready)
			{
				dirty_read[vi] = GpuDirtyPageTracker::Instance().BeginRead(h.block.vaddr[vi], h.block.size[vi]);
				if (dirty_read[vi].tracked)
				{
					clean = !GpuDirtyPageTracker::Instance().ChangedSince(h.block.vaddr[vi], h.block.size[vi], o.dirty_generation[vi]);
				}
			}
			if (!clean && stage_stable_buffer)
			{
				if (!dirty_read[vi].tracked)
				{
					// Keep the previous GPU backing and retry on the next use. The
					// staging contract is entered only for a registered page-fault
					// range, so an untracked result here is a raced/failed arm.
					DebugStatsRecordStableBufferSource(false, h.block.size[vi], false);
					return;
				} else
				{
					stable_buffer_source.resize(static_cast<size_t>(h.block.size[vi]));
					std::memcpy(stable_buffer_source.data(), reinterpret_cast<const void*>(h.block.vaddr[vi]),
					            stable_buffer_source.size());
					if (!GpuDirtyPageTracker::Instance().ReadObservationIsStable(h.block.vaddr[vi], h.block.size[vi],
					                                                              dirty_read[vi]))
					{
						DebugStatsRecordStableBufferSource(false, h.block.size[vi], false);
						return;
					}
					upload_vaddr[vi]          = reinterpret_cast<uint64_t>(stable_buffer_source.data());
					stable_buffer_source_ready = true;
					DebugStatsRecordStableBufferSource(false, h.block.size[vi], true);
				}
			}
			if (clean)
			{
				hash[vi] = o.hash[vi];
			} else if (o.check_hash)
			{
				hash[vi]          = GpuMemoryCalcHash(o.object.type, reinterpret_cast<const uint8_t*>(upload_vaddr[vi]), h.block.size[vi]);
				hash_compared[vi] = true;
				hash_tracked[vi]  = dirty_read[vi].tracked;
			} else
			{
				hash[vi] = 0;
			}
		}

		for (int vi = 0; vi < h.block.vaddr_num; vi++)
		{
			const bool changed = o.hash[vi] != hash[vi];
			if (hash_compared[vi])
			{
				DebugStatsRecordGpuMemoryHashComparison(GpuMemoryStatsTypeIndex(o.object.type), hash_tracked[vi], changed);
			}
			if (changed)
			{
				KYTY_LOG_DEBUG("Update (CPU -> GPU): type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n",
				               Core::EnumName(o.object.type).C_Str(), h.block.vaddr[vi], h.block.size[vi]);
				need_update = true;
			}
		}

		if (submit_id != UINT64_MAX)
		{
			o.submit_id = submit_id;
		}

		if (o.dirty_registered && !need_update)
		{
			for (int vi = 0; vi < h.block.vaddr_num; vi++)
			{
				if (dirty_read[vi].tracked)
				{
					o.dirty_generation[vi] = dirty_read[vi].generation;
				}
			}
		}
		if (!need_update)
		{
			return;
		}

		EXIT_IF(o.update_func == nullptr);
		// Textures linked under a live RT/StorageTexture must not re-detile from
		// guest after StorageBuffer writebacks clobber the same pages: the guest
		// then holds linear SSBO bytes, and tile-27/9 detile produces horizontal
		// bands. Keep the last GPU-resident image; sample bind prefers the live
		// surface when still present.
		bool surface_parent = false;
		if (o.object.type == GpuMemoryObjectType::Texture)
		{
			for (const auto& link: h.others)
			{
				if (link.object_id < 0 || static_cast<uint32_t>(link.object_id) >= heap.objects.Size())
				{
					continue;
				}
				const auto& parent = heap.objects[link.object_id];
				if (parent.free)
				{
					continue;
				}
				const auto pt = parent.info.object.type;
				if (pt == GpuMemoryObjectType::RenderTexture || pt == GpuMemoryObjectType::StorageTexture)
				{
					surface_parent = true;
					break;
				}
			}
		}

		const bool pending_uses       = !m_deferred_deletions.AreDependenciesComplete(o.submission_uses.Dependencies());
		const bool write_back_capable = o.write_back_func != nullptr && !o.read_only;
		const auto mutation           = GpuMemoryChooseMutationAction(need_update, surface_parent, pending_uses, write_back_capable);

		if (mutation == GpuMemoryMutationAction::RejectWriteBackConflict)
		{
			const auto& dependencies = o.submission_uses.Dependencies();
			const auto  pending      = std::find_if(dependencies.begin(), dependencies.end(), [this](const auto& dependency)
			                                        { return !m_deferred_deletions.AreDependenciesComplete({dependency}); });
			EXIT_IF(pending == dependencies.end());
			EXIT("GpuMemory cannot version an in-flight write-back backing: type=%s vaddr=0x%016" PRIx64 " size=0x%016" PRIx64
			     " queue=%" PRIu32 " sequence=%" PRIu64 "\n",
			     Core::EnumName(o.object.type).C_Str(), h.block.vaddr[0], h.block.size[0], pending->queue.Value(), pending->sequence);
		}
		if (mutation == GpuMemoryMutationAction::VersionBacking)
		{
			VersionBacking(ctx, heap_id, obj_id, destructors, stable_buffer_source_ready ? upload_vaddr : nullptr);
		} else if (mutation == GpuMemoryMutationAction::UpdateInPlace)
		{
			const auto update_start = std::chrono::steady_clock::now();
			o.update_func(ctx, o.params, o.object.obj, stable_buffer_source_ready ? upload_vaddr : h.block.vaddr, h.block.size,
			              h.block.vaddr_num);
			const auto update_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - update_start).count();
			DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::UpdateFunc, static_cast<uint64_t>(update_ns));
		}

		// VersionBacking may have released m_mutex, so reacquire the current
		// logical object instead of retaining a stale reference.
		auto& updated = m_heaps[heap_id].objects[obj_id].info;
		for (int vi = 0; vi < h.block.vaddr_num; vi++)
		{
			updated.hash[vi] = hash[vi];
		}
		updated.gpu_update_time  = GpuMemoryGetCurrentTime();
		updated.content_origin   = GpuMemoryContentOrigin::CpuUpload;
		updated.content_sequence = NextContentSequence();
		if (updated.dirty_registered)
		{
			for (int vi = 0; vi < h.block.vaddr_num; vi++)
			{
				if (dirty_read[vi].tracked)
				{
					updated.dirty_generation[vi] = dirty_read[vi].generation;
				}
			}
		}
	}
}

bool GpuMemory::create_existing(const Vector<OverlappedBlock>& others, const GpuObject& info, int heap_id, const uint64_t* vaddr,
                                const uint64_t* size, int vaddr_num, int* id, bool* covered_reuse, int* stale_reuse_id)
{
	EXIT_IF(vaddr == nullptr || size == nullptr || id == nullptr || covered_reuse == nullptr || stale_reuse_id == nullptr);

	auto& heap = m_heaps[heap_id];

	uint64_t               max_gpu_update_time = 0;
	const OverlappedBlock* latest_block        = nullptr;
	int                    exact_id            = -1;
	uint64_t               exact_gpu_time      = 0;
	int                    latest_surface_id   = -1;
	uint64_t               latest_surface_time = 0;
	int                    reusable_index_id   = -1;
	uint64_t               reusable_index_size = UINT64_MAX;
	int                    reusable_rt_id      = -1;
	uint64_t               reusable_rt_layers  = UINT64_MAX;
	*covered_reuse                             = false;
	*stale_reuse_id                            = -1;

	for (const auto& obj: others)
	{
		auto& h = heap.objects[obj.object_id];
		EXIT_IF(h.free);
		auto& o = h.info;

		if (h.scenario == GpuMemoryScenario::Common && obj.relation == OverlapType::Equals && o.object.type == info.type &&
		    info.Equal(o.params))
		{
			exact_id       = obj.object_id;
			exact_gpu_time = o.gpu_update_time;
			continue;
		}

		if (info.type == GpuMemoryObjectType::Texture &&
		    (o.object.type == GpuMemoryObjectType::RenderTexture || o.object.type == GpuMemoryObjectType::StorageTexture) &&
		    o.gpu_update_time > latest_surface_time)
		{
			latest_surface_id   = obj.object_id;
			latest_surface_time = o.gpu_update_time;
		}

		if (vaddr_num == 1 && h.block.vaddr_num == 1 && h.scenario == GpuMemoryScenario::Common &&
		    o.object.type == GpuMemoryObjectType::IndexBuffer && info.type == GpuMemoryObjectType::IndexBuffer && info.Equal(o.params) &&
		    GpuMemoryCanReuseIndexBacking(h.block.vaddr[0], h.block.size[0], vaddr[0], size[0]) && h.block.size[0] < reusable_index_size)
		{
			reusable_index_id   = obj.object_id;
			reusable_index_size = h.block.size[0];
		}

		if (vaddr_num == 1 && h.block.vaddr_num == 1 && h.scenario == GpuMemoryScenario::Common &&
		    o.object.type == GpuMemoryObjectType::RenderTexture && info.type == GpuMemoryObjectType::RenderTexture &&
		    obj.relation == OverlapType::Contains && h.block.vaddr[0] == vaddr[0] && h.block.size[0] >= size[0] &&
		    RenderTextureCanReuseLargerBacking(o.params, info.params) &&
		    o.params[RenderTextureObject::PARAM_ARRAY_LAYERS] < reusable_rt_layers)
		{
			reusable_rt_id     = obj.object_id;
			reusable_rt_layers = o.params[RenderTextureObject::PARAM_ARRAY_LAYERS];
		}

		if (o.gpu_update_time > max_gpu_update_time)
		{
			max_gpu_update_time = o.gpu_update_time;
			latest_block        = &obj;
		}
	}

	if (exact_id >= 0)
	{
		if (info.type == GpuMemoryObjectType::Texture && latest_surface_id >= 0 && latest_surface_time > exact_gpu_time)
		{
			*stale_reuse_id = exact_id;
			return false;
		}
		*id = exact_id;
		return true;
	}

	if (reusable_index_id >= 0)
	{
		*id            = reusable_index_id;
		*covered_reuse = true;
		return true;
	}
	if (reusable_rt_id >= 0)
	{
		*id            = reusable_rt_id;
		*covered_reuse = true;
		return true;
	}

	if (latest_block != nullptr)
	{
		auto& h = heap.objects[latest_block->object_id];
		auto& o = h.info;

		if (h.scenario == GpuMemoryScenario::GenerateMips && latest_block->relation == OverlapType::Equals && o.object.type == info.type &&
		    info.Equal(o.params))
		{
			*id = latest_block->object_id;
			return true;
		}
	}

	return false;
}

bool GpuMemory::create_generate_mips(const Vector<OverlappedBlock>& others, GpuMemoryObjectType type, int heap_id)
{
	auto& heap = m_heaps[heap_id];

	if (others.Size() == 3 && type == GpuMemoryObjectType::RenderTexture)
	{
		const auto&         b0    = others.At(0);
		const auto&         b1    = others.At(1);
		const auto&         b2    = others.At(2);
		OverlapType         rel0  = b0.relation;
		OverlapType         rel1  = b1.relation;
		OverlapType         rel2  = b2.relation;
		const auto&         o0    = heap.objects[b0.object_id];
		const auto&         o1    = heap.objects[b1.object_id];
		const auto&         o2    = heap.objects[b2.object_id];
		GpuMemoryObjectType type0 = o0.info.object.type;
		GpuMemoryObjectType type1 = o1.info.object.type;
		GpuMemoryObjectType type2 = o2.info.object.type;

		if (rel0 == OverlapType::Contains && rel1 == OverlapType::Contains && rel2 == OverlapType::Contains &&
		    type0 == GpuMemoryObjectType::StorageBuffer && type1 == GpuMemoryObjectType::Texture &&
		    type2 == GpuMemoryObjectType::StorageTexture &&
		    ((o0.others.Size() == 2 && o0.scenario == GpuMemoryScenario::TextureTriplet && o1.others.Size() == 2 &&
		      o1.scenario == GpuMemoryScenario::TextureTriplet && o2.others.Size() == 2 &&
		      o2.scenario == GpuMemoryScenario::TextureTriplet) ||
		     (o0.others.Size() >= 3 && o0.scenario == GpuMemoryScenario::GenerateMips && o1.others.Size() >= 3 &&
		      o1.scenario == GpuMemoryScenario::GenerateMips && o2.others.Size() >= 3 && o2.scenario == GpuMemoryScenario::GenerateMips)))
		{
			return true;
		}
	} else if (others.Size() >= 3 && type == GpuMemoryObjectType::Texture)
	{
		const auto&         b0    = others.At(0);
		const auto&         b1    = others.At(1);
		const auto&         b2    = others.At(2);
		OverlapType         rel0  = b0.relation;
		OverlapType         rel1  = b1.relation;
		OverlapType         rel2  = b2.relation;
		const auto&         o0    = heap.objects[b0.object_id];
		const auto&         o1    = heap.objects[b1.object_id];
		const auto&         o2    = heap.objects[b2.object_id];
		GpuMemoryObjectType type0 = o0.info.object.type;
		GpuMemoryObjectType type1 = o1.info.object.type;
		GpuMemoryObjectType type2 = o2.info.object.type;

		if (((rel0 == OverlapType::Contains && rel1 == OverlapType::Contains && rel2 == OverlapType::Contains) ||
		     (rel0 == OverlapType::Equals && rel1 == OverlapType::Equals && rel2 == OverlapType::Equals)) &&
		    type0 == GpuMemoryObjectType::StorageBuffer && type1 == GpuMemoryObjectType::Texture &&
		    type2 == GpuMemoryObjectType::StorageTexture && o0.scenario == GpuMemoryScenario::GenerateMips &&
		    o1.scenario == GpuMemoryScenario::GenerateMips && o2.scenario == GpuMemoryScenario::GenerateMips)
		{
			return true;
		}
	}

	return false;
}

bool GpuMemory::create_texture_triplet(const Vector<OverlappedBlock>& others, GpuMemoryObjectType type, int heap_id)
{
	auto& heap = m_heaps[heap_id];

	if (others.Size() == 2 && type == GpuMemoryObjectType::StorageTexture)
	{
		const auto&         b0    = others.At(0);
		const auto&         b1    = others.At(1);
		OverlapType         rel0  = b0.relation;
		OverlapType         rel1  = b1.relation;
		const auto&         o0    = heap.objects[b0.object_id];
		const auto&         o1    = heap.objects[b1.object_id];
		GpuMemoryObjectType type0 = o0.info.object.type;
		GpuMemoryObjectType type1 = o1.info.object.type;

		if (rel0 == OverlapType::Equals && rel1 == OverlapType::Equals && type0 == GpuMemoryObjectType::StorageBuffer &&
		    type1 == GpuMemoryObjectType::Texture &&
		    (o0.others.Size() == 1 && o0.scenario == GpuMemoryScenario::Common && o1.others.Size() == 1 &&
		     o1.scenario == GpuMemoryScenario::Common))
		{
			return true;
		}
	}
	return false;
}

bool GpuMemory::create_maybe_deleted(const Vector<OverlappedBlock>& others, GpuMemoryObjectType type, int heap_id)
{
	auto& heap = m_heaps[heap_id];

	if (type == GpuMemoryObjectType::VertexBuffer || type == GpuMemoryObjectType::IndexBuffer)
	{
		return std::all_of(others.begin(), others.end(),
		                   [heap](auto& r)
		                   {
			                   OverlapType         rel    = r.relation;
			                   const auto&         o      = heap.objects[r.object_id];
			                   GpuMemoryObjectType o_type = o.info.object.type;
			                   return ((rel == OverlapType::IsContainedWithin || rel == OverlapType::Crosses) &&
			                           (o_type == GpuMemoryObjectType::VertexBuffer || o_type == GpuMemoryObjectType::IndexBuffer));
		                   });
	}
	if (type == GpuMemoryObjectType::Texture)
	{
		return std::all_of(others.begin(), others.end(),
		                   [heap](auto& r)
		                   {
			                   OverlapType         rel    = r.relation;
			                   const auto&         o      = heap.objects[r.object_id];
			                   GpuMemoryObjectType o_type = o.info.object.type;
			                   return ((rel == OverlapType::IsContainedWithin || rel == OverlapType::Crosses) &&
			                           o_type == GpuMemoryObjectType::Texture);
		                   });
	}
	if (type == GpuMemoryObjectType::RenderTexture)
	{
		return std::all_of(others.begin(), others.end(),
		                   [heap](auto& r)
		                   {
			                   OverlapType         rel    = r.relation;
			                   const auto&         o      = heap.objects[r.object_id];
			                   GpuMemoryObjectType o_type = o.info.object.type;
			                   return ((rel == OverlapType::IsContainedWithin || rel == OverlapType::Crosses) &&
			                           (o_type == GpuMemoryObjectType::RenderTexture || o_type == GpuMemoryObjectType::DepthStencilBuffer));
		                   });
	}
	return false;
}

bool GpuMemory::create_all_the_same(const Vector<OverlappedBlock>& others, int heap_id)
{
	auto&               heap = m_heaps[heap_id];
	OverlapType         rel  = others.At(0).relation;
	GpuMemoryObjectType type = heap.objects[others.At(0).object_id].info.object.type;

	return std::all_of(others.begin(), others.end(),
	                   [rel, type, heap](auto& r) { return (rel == r.relation && type == heap.objects[r.object_id].info.object.type); });
}

String GpuMemory::create_dbg_exit(const String& msg, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                                  const Vector<OverlappedBlock>& others, GpuMemoryObjectType type)
{
	Core::StringList list;
	list.Add(String::FromPrintf("Exit:"));
	list.Add(msg);
	for (int vi = 0; vi < vaddr_num; vi++)
	{
		list.Add(String::FromPrintf("\t vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "", vaddr[vi], size[vi]));
	}

	list.Add(String::FromPrintf("\t info.type = %s", Core::EnumName(type).C_Str()));
	// Parent type is required to diagnose create_all_the_same failures (same
	// relation can pair with mixed object types). Heap is not available here;
	// callers that have heap context should prefer the typed dump below.
	for (const auto& d: others)
	{
		list.Add(String::FromPrintf("\t id = %d, rel = %s", d.object_id, Core::EnumName(d.relation).C_Str()));
	}
	DbgDbDump();
	DbgDbSave(U"_gpu_memory.db");
	auto str = list.Concat(U'\n');
	KYTY_LOG_DEBUG("%s\n", str.C_Str());
	return str;
}

void GpuMemory::RecordUse(ObjectInfo* object, SubmissionId submission)
{
	EXIT_IF(object == nullptr);
	if (object->submission_uses.RecordUse(submission) != GpuDeferredDeletionResult::Success)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8,
		               "WARNING: object->submission_uses.RecordUse(submission) != GpuDeferredDeletionResult::Success condition ignored "
		               "(continuing)\n");
	}
}

void GpuMemory::RecordUse(ObjectInfo* object, CommandBuffer* buffer)
{
	EXIT_IF(object == nullptr);
	if (buffer == nullptr)
	{
		return;
	}

	SubmissionId submission;
	if (!buffer->GetSubmissionId(&submission))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !buffer->GetSubmissionId(&submission) condition ignored (continuing)\n");
	}
	RecordUse(object, submission);
}

void GpuMemory::ScheduleDestructors(GraphicContext* ctx, Vector<Destructor>* destructors)
{
	EXIT_IF(ctx == nullptr || destructors == nullptr);

	for (auto& destructor: *destructors)
	{
		EXIT_IF(destructor.delete_func == nullptr || destructor.obj == nullptr);

		auto       delete_func = destructor.delete_func;
		auto*      object      = destructor.obj;
		auto       memory      = destructor.mem;
		const auto result      = m_deferred_deletions.Enqueue(
		    destructor.submission_uses.Dependencies(), [ctx, delete_func, object, memory]() mutable { delete_func(ctx, object, &memory); });
		if (result != GpuDeferredDeletionResult::Success)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: result != GpuDeferredDeletionResult::Success condition ignored (continuing)\n");
		}
	}
	destructors->Clear();
}

void GpuMemory::ScheduleDestructorsOutsideMutationLocks(GraphicContext* ctx, Vector<Destructor>* destructors)
{
	EXIT_IF(destructors == nullptr);
	if (destructors->IsEmpty())
	{
		return;
	}

	// Enqueue may immediately run a destructor when all dependencies are
	// already complete. Release both GpuMemory locks so deletion callbacks can
	// never re-enter or block the logical object graph.
	m_mutex.Unlock();
	m_backing_mutation_mutex.Unlock();
	ScheduleDestructors(ctx, destructors);
	m_backing_mutation_mutex.Lock();
	m_mutex.Lock();
}

void GpuMemory::CompleteSubmission(SubmissionId submission)
{
	if (m_deferred_deletions.CompleteSubmission(submission) != GpuDeferredDeletionResult::Success)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8,
		               "WARNING: m_deferred_deletions.CompleteSubmission(submission) != GpuDeferredDeletionResult::Success condition "
		               "ignored (continuing)\n");
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void* GpuMemory::CreateObject(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
                              int vaddr_num, const GpuObject& info)
{
	KYTY_PROFILER_BLOCK("GpuMemory::CreateObject", profiler::colors::Green300);

	EXIT_IF(info.type == GpuMemoryObjectType::Invalid);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num > VADDR_BLOCKS_MAX || vaddr_num <= 0);

	uint64_t requested_bytes = 0;
	for (int vi = 0; vi < vaddr_num; ++vi)
	{
		requested_bytes = size[vi] > UINT64_MAX - requested_bytes ? UINT64_MAX : requested_bytes + size[vi];
	}
	DebugStatsGpuMemoryCreateTrace create_stats(GpuMemoryStatsTypeIndex(info.type), requested_bytes, static_cast<uint32_t>(vaddr_num));
	const auto                     backing_lock_start = std::chrono::steady_clock::now();
	Core::LockGuard                backing_lock(m_backing_mutation_mutex);
	const auto                     backing_lock_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - backing_lock_start).count();
	create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::BackingLockWait, static_cast<uint64_t>(backing_lock_ns));
	const auto      registry_lock_start = std::chrono::steady_clock::now();
	Core::LockGuard lock(m_mutex);
	const auto      registry_lock_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - registry_lock_start).count();
	create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::RegistryLockWait, static_cast<uint64_t>(registry_lock_ns));
	const auto classification_start = std::chrono::steady_clock::now();
	bool       classification_done  = false;
	const auto finish_classification =
	    [&](uint32_t candidates = 0, uint32_t relation_mask = 0, uint32_t reclaimed = 0, bool from_objects = false)
	{
		if (classification_done)
		{
			return;
		}
		const auto elapsed =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - classification_start).count();
		create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::Classification, static_cast<uint64_t>(elapsed));
		create_stats.SetClassification(candidates, relation_mask, reclaimed, from_objects);
		classification_done = true;
	};
	Vector<Destructor> destructors;

	int heap_id = GetHeapId(vaddr[0], size[0]);

	// Guest libc heap (host malloc) is never MapDirectMemory'd. Dreaming Sarah
	// embeds small VS V# tables there (fetch_embedded); register a page cover
	// so VertexBuffer/IndexBuffer staging can memcpy from that memory.
	// Already holding m_mutex — do not call SetAllocatedRange (it re-locks).
	if (heap_id < 0 && GpuMemoryIsHostGuestMallocRange(vaddr[0], size[0]))
	{
		uint64_t cover_start = 0;
		uint64_t cover_size  = 0;
		GpuMemoryHostGuestMallocPageCover(vaddr[0], size[0], &cover_start, &cover_size);
		if (GetHeapId(cover_start, cover_size) < 0)
		{
			Heap h;
			h.range.vaddr   = cover_start;
			h.range.size    = cover_size;
			h.objects_map1  = new GpuMap1;
			h.objects_map2  = new GpuMap2;
			h.overlap_cache = new OverlapQueryCache;
			m_heaps.Add(h);
			m_allocated_validation_cache.Invalidate();
			m_allocated_prefix_cache.Invalidate();
			m_overlap_snapshot_cache.Invalidate();
		}
		heap_id = GetHeapId(vaddr[0], size[0]);
	}

	if (heap_id < 0)
	{
		Vector<OverlappedBlock> no_parents;
		EXIT("%s\n", create_dbg_exit(U"unallocated gpu object range", vaddr, size, vaddr_num, no_parents, info.type).C_Str());
	}

	auto&      heap                         = m_heaps[heap_id];
	const auto invalidate_overlap_snapshots = [this](const Block& block)
	{
		for (int vi = 0; vi < block.vaddr_num; ++vi)
		{
			m_overlap_snapshot_cache.InvalidateRange(block.vaddr[vi], block.size[vi]);
		}
	};

	GpuMemoryMaterializationKey materialization_key;
	if (buffer != nullptr)
	{
		SubmissionId submission;
		if (!buffer->GetSubmissionId(&submission))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !buffer->GetSubmissionId(&submission) condition ignored (continuing)\n");
		}
		materialization_key = GpuMemoryMaterializationKey::Create(submit_id, submission.queue.Value(), submission.sequence, vaddr, size,
		                                                          vaddr_num, static_cast<uint32_t>(info.type), info.params,
		                                                          GpuObject::PARAMS_MAX, info.check_hash, info.read_only);

		Materialization cached;
		if (m_materialization_cache.Lookup(materialization_key, &cached) && cached.heap_id == heap_id && cached.object_id >= 0 &&
		    static_cast<uint32_t>(cached.object_id) < heap.objects.Size())
		{
			auto&      h              = heap.objects[cached.object_id];
			auto&      o              = h.info;
			const bool same_callbacks = cached.create_func == info.GetCreateFunc() &&
			                            cached.create_from_objects_func == info.GetCreateFromObjectsFunc() &&
			                            cached.write_back_func == info.GetWriteBackFunc() && cached.delete_func == info.GetDeleteFunc() &&
			                            cached.update_func == info.GetUpdateFunc();
			if (!h.free && h.scenario == GpuMemoryScenario::Common && o.logical_generation == cached.logical_generation &&
			    o.object.type == info.type && o.submit_id == submit_id && o.in_use && o.check_hash == info.check_hash &&
			    info.Equal(o.params) && same_callbacks)
			{
				o.use_num++;
				o.use_last_frame              = m_current_frame;
				const bool previous_read_only = o.read_only;
				o.read_only                   = GpuMemoryMergeReadOnlyUse(o.in_use, o.read_only, info.read_only);
				if (o.read_only != previous_read_only)
				{
					invalidate_overlap_snapshots(h.block);
				}
				o.in_use     = true;
				o.check_hash = info.check_hash;
				RecordUse(&o, buffer);
				finish_classification();
				create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::CachedReuse);
				return o.object.obj;
			}
		}
	}

	const auto cache_materialization = [&](int object_id)
	{
		if (!materialization_key.Valid() || object_id < 0 || static_cast<uint32_t>(object_id) >= heap.objects.Size())
		{
			return;
		}
		const auto& h = heap.objects[object_id];
		const auto& o = h.info;
		if (h.free || h.scenario != GpuMemoryScenario::Common || o.object.obj == nullptr || o.object.type != info.type ||
		    o.submit_id != submit_id || !o.in_use || o.check_hash != info.check_hash || !info.Equal(o.params))
		{
			return;
		}
		m_materialization_cache.Store(materialization_key, Materialization {heap_id, object_id, o.logical_generation, info.GetCreateFunc(),
		                                                                    info.GetCreateFromObjectsFunc(), info.GetWriteBackFunc(),
		                                                                    info.GetDeleteFunc(), info.GetUpdateFunc()});
	};

	bool        overlap             = false;
	bool        delete_all          = false;
	bool        create_from_objects = false;
	Vector<int> selective_reclaim_ids;
	Vector<int> depth_stencil_reclaim_ids;
	Vector<int> retire_after_copy_ids;

	GpuMemoryScenario scenario = GpuMemoryScenario::Common;

	int fast_id = -1;
	if (FindFast(heap_id, vaddr, size, vaddr_num, info.type, false, &fast_id))
	{
		auto& h = heap.objects[fast_id];
		EXIT_IF(h.free);
		auto& o = h.info;

		if (h.scenario == GpuMemoryScenario::Common && info.Equal(o.params))
		{
			finish_classification();
			Update(submit_id, ctx, heap_id, fast_id, &destructors);

			o.use_num++;
			o.use_last_frame              = m_current_frame;
			const bool previous_read_only = o.read_only;
			o.read_only                   = GpuMemoryMergeReadOnlyUse(o.in_use, o.read_only, info.read_only);
			if (o.read_only != previous_read_only)
			{
				invalidate_overlap_snapshots(h.block);
			}
			o.in_use     = true;
			o.check_hash = info.check_hash;
			RecordUse(&o, buffer);

			void* const result = o.object.obj;
			cache_materialization(fast_id);
			ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
			create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::FastReuse);
			return result;
		}
	}

	auto others = FindBlocks(heap_id, vaddr, size, vaddr_num);

	if (!others.IsEmpty())
	{
		int  existing_id    = -1;
		bool covered_reuse  = false;
		int  stale_reuse_id = -1;
		if (create_existing(others, info, heap_id, vaddr, size, vaddr_num, &existing_id, &covered_reuse, &stale_reuse_id))
		{
			auto& h = heap.objects[existing_id];
			EXIT_IF(h.free);
			auto& o = h.info;

			uint32_t relation_mask = 0;
			for (const auto& candidate: others)
			{
				const auto relation = static_cast<uint32_t>(candidate.relation);
				if (relation < 32u)
				{
					relation_mask |= 1u << relation;
				}
			}
			finish_classification(others.Size(), relation_mask);
			Update(submit_id, ctx, heap_id, existing_id, &destructors);

			o.use_num++;
			o.use_last_frame              = m_current_frame;
			const bool previous_read_only = o.read_only;
			o.read_only                   = GpuMemoryMergeReadOnlyUse(o.in_use, o.read_only, info.read_only);
			if (o.read_only != previous_read_only)
			{
				invalidate_overlap_snapshots(h.block);
			}
			o.in_use     = true;
			o.check_hash = info.check_hash;
			RecordUse(&o, buffer);

			void* const result = o.object.obj;
			cache_materialization(existing_id);
			ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
			create_stats.Complete(covered_reuse ? DebugStatsGpuMemoryCreateOutcome::CoveredReuse
			                                    : DebugStatsGpuMemoryCreateOutcome::ExactReuse);
			return result;
		}
		if (stale_reuse_id >= 0)
		{
			selective_reclaim_ids.Add(stale_reuse_id);
		}

		if (others.Size() == 1)
		{
			const auto& obj = others.At(0);
			auto&       h   = heap.objects[obj.object_id];
			EXIT_IF(h.free);
			auto& o = h.info;

			if (o.object.type == GpuMemoryObjectType::StorageBuffer && info.type == GpuMemoryObjectType::StorageBuffer &&
			    (obj.relation == OverlapType::Equals ||
			     GpuMemoryCanShareReadOnlyStorageViews(h.block.vaddr[0], h.block.size[0], o.read_only, vaddr[0], size[0], info.read_only)))
			{
				// Equals: same guest range re-registered as StorageBuffer (captured
				// post-menu). RO share: partial overlapping RO views.
				overlap = true;
			} else if (buffer != nullptr && o.object.type == GpuMemoryObjectType::RenderTexture &&
			           info.type == GpuMemoryObjectType::RenderTexture && obj.relation == OverlapType::IsContainedWithin &&
			           h.block.vaddr_num == 1 && vaddr_num == 1 && h.block.vaddr[0] == vaddr[0] && h.block.size[0] < size[0] &&
			           h.others.IsEmpty() && RenderTextureCanCopyGrowingBacking(o.params, info.params))
			{
				create_from_objects = true;
				retire_after_copy_ids.Add(obj.object_id);
			} else if (buffer != nullptr && o.object.type == GpuMemoryObjectType::StorageTexture &&
			           info.type == GpuMemoryObjectType::StorageTexture && obj.relation == OverlapType::IsContainedWithin &&
			           h.block.vaddr_num == 1 && vaddr_num == 1 && h.block.vaddr[0] == vaddr[0] && h.block.size[0] < size[0] &&
			           h.others.IsEmpty() && StorageTextureCanCopyGrowingBacking(o.params, info.params))
			{
				create_from_objects = true;
				retire_after_copy_ids.Add(obj.object_id);
			} else if (GpuMemoryAllowsTextureStorageAlias(o.object.type, obj.relation, info.type))
			{
				// Texture↔StorageBuffer partial shares and Texture↔StorageTexture
				// Equals (same guest range as sampled + storage image). Texture
				// created from a live StorageTexture parent uses CreateFromObjects.
				overlap = true;
				if (info.type == GpuMemoryObjectType::Texture &&
				    (o.object.type == GpuMemoryObjectType::StorageTexture || o.object.type == GpuMemoryObjectType::RenderTexture))
				{
					create_from_objects = true;
				}
			} else if (GpuMemoryAllowsIndexStorageShare(o.object.type, obj.relation, info.type))
			{
				overlap = true;
			} else if (GpuMemoryAllowsVertexStorageShare(o.object.type, obj.relation, info.type) &&
			           obj.relation == OverlapType::Equals)
			{
				// Same guest range as a live VertexBuffer: Fetch keeps the VB,
				// untagged MUBUF addresses the stream as a raw SSBO.
				overlap = true;
			} else if (GpuMemoryAllowsTextureContainedInSurface(o.object.type, obj.relation, info.type))
			{
				// Incoming Texture under a live RT/ST/SB/Texture surface: link and,
				// when the parent holds a Vulkan image, copy from it instead of
				// CPU-detiling empty GPU-owned guest memory.
				overlap = true;
				if (o.object.type == GpuMemoryObjectType::RenderTexture || o.object.type == GpuMemoryObjectType::StorageTexture)
				{
					create_from_objects = true;
				}
			} else if (GpuMemoryAllowsVertexContainedInSurface(o.object.type, obj.relation, info.type))
			{
				overlap = true;
			} else if (GpuMemoryAllowsIndexContainedInSurface(o.object.type, obj.relation, info.type))
			{
				// Texture/RT/SB Contains (or other surface overlap) an IndexBuffer.
				// Captured: Texture Contains IB 0xe4 — link, do not delete Texture.
				overlap = true;
			} else if (GpuMemoryAllowsVertexLinkIndexBuffer(o.object.type, obj.relation, info.type) ||
			           GpuMemoryAllowsIndexLinkVertexBuffer(o.object.type, obj.relation, info.type))
			{
				// Vertex and index views are independent bindings over the same
				// guest bytes. Keep both for every explicitly supported overlap,
				// including Equals exposed by resident index backing reuse.
				overlap = true;
			} else if (GpuMemoryAllowsTextureLinkVertex(o.object.type, obj.relation, info.type) ||
			           GpuMemoryAllowsTextureLinkIndexBuffer(o.object.type, obj.relation, info.type))
			{
				// Incoming Texture over a larger VertexBuffer (Contains/Equals) or an
				// existing IndexBuffer. Link both views.
				overlap = true;
			} else if (GpuMemoryAllowsTextureReclaimVertex(o.object.type, obj.relation, info.type))
			{
				// Large Texture superseding partial VertexBuffers in its range.
				delete_all = true;
			} else if (GpuMemoryShouldLinkPendingDepthStencilStorage(
			               o.object.type, obj.relation, info.type, o.in_use, o.read_only, o.write_back_func != nullptr,
			               m_deferred_deletions.AreDependenciesComplete(o.submission_uses.Dependencies())))
			{
				// Queue order keeps the old storage use before the new depth use. The
				// completion callback owns publication and invalidates linked depth.
				overlap = true;
			} else if (GpuMemoryAllowsStorageSurfaceShare(o.object.type, obj.relation, info.type))
			{
				// Single-parent RT/DS/Texture/SB share with an incoming StorageBuffer
				// (captured DepthStencilBuffer Crosses StorageBuffer 0x8000).
				overlap = true;
			} else if (GpuMemoryAllowsRenderTargetSurfaceAlias(o.object.type, obj.relation, info.type))
			{
				// An incoming render target may alias one surface parent for the same
				// reasons the multi-parent path accepts: the guest rebinds a range it
				// already exposed as a sampled/storage/render view. A lone parent is
				// not a stricter contract than several, so share instead of failing
				// (captured full-screen RenderTexture Crossing a stale Texture).
				overlap = true;
			} else if (GpuMemoryAllowsDepthStencilReclaimSurface(o.object.type, obj.relation, info.type))
			{
				delete_all = true;
			} else
				switch (ObjectsRelation(o.object.type, obj.relation, info.type))
				{
					case ObjectsRelation(GpuMemoryObjectType::VideoOutBuffer, OverlapType::Equals, GpuMemoryObjectType::StorageBuffer):
					case ObjectsRelation(GpuMemoryObjectType::VideoOutBuffer, OverlapType::Equals, GpuMemoryObjectType::RenderTexture):
					{
						overlap = true;
						break;
					}
					case ObjectsRelation(GpuMemoryObjectType::DepthStencilBuffer, OverlapType::Contains,
					                     GpuMemoryObjectType::DepthStencilBuffer):
					case ObjectsRelation(GpuMemoryObjectType::IndexBuffer, OverlapType::Crosses, GpuMemoryObjectType::IndexBuffer):
					case ObjectsRelation(GpuMemoryObjectType::IndexBuffer, OverlapType::Contains, GpuMemoryObjectType::IndexBuffer):
					case ObjectsRelation(GpuMemoryObjectType::IndexBuffer, OverlapType::IsContainedWithin,
					                     GpuMemoryObjectType::IndexBuffer):
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Crosses, GpuMemoryObjectType::VertexBuffer):
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Contains, GpuMemoryObjectType::VertexBuffer):
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::IsContainedWithin,
					                     GpuMemoryObjectType::VertexBuffer):
					// Gen5 alias observed when a storage view crosses an active vertex
					// allocation. Keep both resource views linked so the storage access
					// sees the same guest memory without reclaiming the vertex object.
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Crosses, GpuMemoryObjectType::StorageBuffer):
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::IsContainedWithin,
					                     GpuMemoryObjectType::StorageBuffer):
					// Existing VertexBuffer fully contains a new StorageBuffer view
					// (relation Contains). Reclaim the vertex object so the storage
					// view owns the range; multi-parent path links instead when a
					// Texture alias coexists. Peer Texture overlaps are handled by
					// GpuMemoryAllowsTextureContainedInSurface above (link, not reclaim).
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Contains, GpuMemoryObjectType::StorageBuffer):
					{
						delete_all = true;
						break;
					}
					default:
					{
						auto msg = String::FromPrintf("unknown relation: %s - %s - %s\n", Core::EnumName(o.object.type).C_Str(),
						                              Core::EnumName(obj.relation).C_Str(), Core::EnumName(info.type).C_Str());
						EXIT("%s\n", create_dbg_exit(msg, vaddr, size, vaddr_num, others, info.type).C_Str());
					}
				}
		} else
		{
			// Multiple existing blocks. Gen5 constant/storage views often create a
			// partial RO StorageBuffer that is Contained in one larger RO view and
			// Crosses another adjacent RO view (observed: new 0x70 inside 0x80 and
			// crossing a neighbor 0x70). The single-overlap path already allows
			// each share; require every parent to be an RO StorageBuffer that
			// shares safely before treating the multi-overlap as linkable.
			Vector<int> storage_growth_ids;
			bool        multi_storage_growth = buffer != nullptr && info.type == GpuMemoryObjectType::StorageTexture && !others.IsEmpty();
			if (multi_storage_growth)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o          = h.info;
					const bool  compatible = o.object.type == GpuMemoryObjectType::StorageTexture &&
					                         obj.relation == OverlapType::IsContainedWithin && h.block.vaddr_num == 1 && vaddr_num == 1 &&
					                         h.block.vaddr[0] == vaddr[0] && h.block.size[0] < size[0] && h.others.IsEmpty() &&
					                         StorageTextureCanCopyGrowingBacking(o.params, info.params);
					if (!compatible)
					{
						multi_storage_growth = false;
						storage_growth_ids.Clear();
						break;
					}
					storage_growth_ids.Add(obj.object_id);
				}
			}

			bool multi_ro_storage_share = (info.type == GpuMemoryObjectType::StorageBuffer && info.read_only);
			if (multi_ro_storage_share)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					if (o.object.type != GpuMemoryObjectType::StorageBuffer ||
					    !GpuMemoryCanShareReadOnlyStorageViews(h.block.vaddr[0], h.block.size[0], o.read_only, vaddr[0], size[0],
					                                           info.read_only))
					{
						multi_ro_storage_share = false;
						break;
					}
				}
			}

			bool multi_vertex_storage_alias = (info.type == GpuMemoryObjectType::StorageBuffer && !others.IsEmpty());
			if (multi_vertex_storage_alias)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					// Contains: existing larger VertexBuffer fully covers the new
					// StorageBuffer (observed multi-parent load path alongside
					// Texture Contains). Crosses/IsContainedWithin already covered.
					if (o.object.type != GpuMemoryObjectType::VertexBuffer ||
					    (obj.relation != OverlapType::Crosses && obj.relation != OverlapType::IsContainedWithin &&
					     obj.relation != OverlapType::Contains && obj.relation != OverlapType::Equals))
					{
						multi_vertex_storage_alias = false;
						break;
					}
				}
			}

			// Multi-parent StorageBuffer where every parent independently supports
			// the exact role and overlap relation. create_all_the_same rejects mixed
			// parent types; this policy links validated mixed parents.
			bool multi_mixed_storage_alias = (info.type == GpuMemoryObjectType::StorageBuffer && !others.IsEmpty());
			if (multi_mixed_storage_alias)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					if (!GpuMemoryAllowsStorageParent(o.object.type, obj.relation, info.type))
					{
						multi_mixed_storage_alias = false;
						break;
					}
				}
			}

			// Multi-parent VertexBuffer Contained in StorageBuffer/RenderTexture
			// (and similar surfaces). Observed: new VB 0x480 inside a 0x60000
			// StorageBuffer+RenderTexture Equals pair at the same guest base.
			bool multi_vertex_in_surface = (info.type == GpuMemoryObjectType::VertexBuffer && !others.IsEmpty());
			if (multi_vertex_in_surface)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					if (!GpuMemoryAllowsVertexContainedInSurface(o.object.type, obj.relation, info.type))
					{
						multi_vertex_in_surface = false;
						break;
					}
				}
			}

			// Multi-parent VertexBuffer with mixed surface parents (link) and peer
			// VertexBuffers (reclaim). Captured after dmask 0xb load path:
			// SB/RT Contains or IsContainedWithin/Crosses + VB IsContainedWithin +
			// VB Crosses → create_all_the_same rejects mixed relations/types.
			// Also captured: Texture Contains + IndexBuffer Crosses (link IB).
			Vector<int> vertex_reclaim_vertex_ids;
			bool        multi_vertex_mixed = (info.type == GpuMemoryObjectType::VertexBuffer && !others.IsEmpty());
			if (multi_vertex_mixed)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					if (GpuMemoryAllowsVertexReclaimVertex(o.object.type, obj.relation, info.type))
					{
						vertex_reclaim_vertex_ids.Add(obj.object_id);
						continue;
					}
					if (GpuMemoryAllowsVertexContainedInSurface(o.object.type, obj.relation, info.type))
					{
						continue;
					}
					if (GpuMemoryAllowsVertexLinkIndexBuffer(o.object.type, obj.relation, info.type))
					{
						continue;
					}
					multi_vertex_mixed = false;
					break;
				}
			}

			// Multi-parent IndexBuffer: peer IB reclaim + VB/surface link.
			// Captured: IndexBuffer IsContainedWithin (0xe4) + VertexBuffer
			// Contains (0x100) a new IndexBuffer 0xfc at the same base.
			Vector<int> index_reclaim_index_ids;
			bool        multi_index_mixed = (info.type == GpuMemoryObjectType::IndexBuffer && !others.IsEmpty());
			if (multi_index_mixed)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					if (GpuMemoryAllowsIndexReclaimIndex(o.object.type, obj.relation, info.type))
					{
						index_reclaim_index_ids.Add(obj.object_id);
						continue;
					}
					if (GpuMemoryAllowsIndexContainedInSurface(o.object.type, obj.relation, info.type))
					{
						continue;
					}
					if (GpuMemoryAllowsIndexLinkVertexBuffer(o.object.type, obj.relation, info.type))
					{
						continue;
					}
					multi_index_mixed = false;
					break;
				}
			}

			// Multi-parent RenderTexture: surface peers/parents (SB/RT/Texture)
			// and partial VertexBuffers. Captured after Param5: SB Equals +
			// SB Contains + RT Contains (and permutations).
			bool multi_render_target_alias = (info.type == GpuMemoryObjectType::RenderTexture && !others.IsEmpty());
			if (multi_render_target_alias)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					if (!GpuMemoryAllowsRenderTargetSurfaceAlias(o.object.type, obj.relation, info.type))
					{
						multi_render_target_alias = false;
						break;
					}
				}
			}

			bool multi_texture_reclaim = (info.type == GpuMemoryObjectType::Texture && !others.IsEmpty());
			if (multi_texture_reclaim)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					if (o.object.type != GpuMemoryObjectType::VertexBuffer ||
					    (obj.relation != OverlapType::Crosses && obj.relation != OverlapType::IsContainedWithin))
					{
						multi_texture_reclaim = false;
						break;
					}
				}
			}

			// DepthStencilBuffer with multi-plane vaddrs that Cross Texture and
			// StorageBuffer (captured: depth/stencil/htile vs large Texture +
			// 0x8000 Storage at the htile plane). Reclaim parents so the DS
			// object owns the guest ranges.
			bool multi_depth_stencil_reclaim = (info.type == GpuMemoryObjectType::DepthStencilBuffer && !others.IsEmpty());
			if (multi_depth_stencil_reclaim)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					if (!GpuMemoryAllowsDepthStencilReclaimSurface(o.object.type, obj.relation, info.type))
					{
						multi_depth_stencil_reclaim = false;
						break;
					}
				}
			}

			// Texture with mixed parents: reclaim peer VBs (Crosses/IsContainedWithin),
			// link larger VBs that Contain the texture, and link SB/RT surfaces.
			// Captured: VB Contains + SB Contains + RT Contains (0x1000 texture).
			bool multi_texture_mixed = (info.type == GpuMemoryObjectType::Texture && !others.IsEmpty());
			if (multi_texture_mixed)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					if (GpuMemoryAllowsTextureReclaimVertex(o.object.type, obj.relation, info.type))
					{
						selective_reclaim_ids.Add(obj.object_id);
						continue;
					}
					if (GpuMemoryAllowsTextureLinkVertex(o.object.type, obj.relation, info.type))
					{
						continue;
					}
					if (GpuMemoryAllowsTextureContainedInSurface(o.object.type, obj.relation, info.type))
					{
						continue;
					}
					if (GpuMemoryAllowsTextureLinkDepthMetadata(o.object.type, obj.relation, info.type))
					{
						continue;
					}
					if (GpuMemoryAllowsTextureLinkIndexBuffer(o.object.type, obj.relation, info.type))
					{
						continue;
					}
					multi_texture_mixed = false;
					break;
				}
			}

			if (multi_depth_stencil_reclaim)
			{
				const uint64_t htile_addr = info.params[DepthStencilBufferObject::PARAM_HTILE_ADDR];
				const uint64_t htile_size = info.params[DepthStencilBufferObject::PARAM_HTILE_SIZE];
				for (const auto& obj: others)
				{
					auto& parent = heap.objects[obj.object_id];
					if (parent.info.object.type == GpuMemoryObjectType::StorageBuffer && parent.block.vaddr_num == 1 &&
					    parent.block.vaddr[0] == htile_addr && parent.block.size[0] == htile_size)
					{
						auto* storage = static_cast<StorageVulkanBuffer*>(parent.info.object.obj);
						EXIT_IF(storage == nullptr || storage->guest_addr != htile_addr || storage->guest_size != htile_size);
						storage->depth_meta_addr = htile_addr;
						storage->depth_meta_size = htile_size;
						parent.info.depth_meta_bound = true;
					}
					const auto& parent_info = parent.info;
					// Pending writable storage stays linked; completed storage and
					// other reclaimable surfaces are freed below.
					const bool pending_storage_write_back = GpuMemoryShouldLinkPendingDepthStencilStorage(
					    parent_info.object.type, obj.relation, info.type, parent_info.in_use, parent_info.read_only,
					    parent_info.write_back_func != nullptr,
					    m_deferred_deletions.AreDependenciesComplete(parent_info.submission_uses.Dependencies()));
					if (!pending_storage_write_back)
					{
						depth_stencil_reclaim_ids.Add(obj.object_id);
					}
				}
			}

			if (multi_storage_growth)
			{
				create_from_objects   = true;
				retire_after_copy_ids = storage_growth_ids;
			} else if (multi_ro_storage_share || multi_vertex_storage_alias || multi_mixed_storage_alias || multi_vertex_in_surface ||
			           multi_render_target_alias)
			{
				overlap = true;
			} else if (multi_texture_reclaim)
			{
				delete_all = true;
			} else if (multi_depth_stencil_reclaim)
			{
				if (depth_stencil_reclaim_ids.Size() == others.Size())
				{
					delete_all = true;
				} else
				{
					Vector<OverlappedBlock> keep;
					for (const auto& obj: others)
					{
						bool reclaim = false;
						for (int id: depth_stencil_reclaim_ids)
						{
							if (id == obj.object_id)
							{
								reclaim = true;
								break;
							}
						}
						if (!reclaim)
						{
							keep.Add(obj);
						}
					}
					selective_reclaim_ids = depth_stencil_reclaim_ids;
					others                = keep;
					overlap               = true;
				}
			} else if (multi_texture_mixed)
			{
				// Drop reclaimed VBs from the link set; free them after create.
				if (selective_reclaim_ids.Size() == others.Size())
				{
					delete_all = true;
				} else
				{
					Vector<OverlappedBlock> keep;
					for (const auto& obj: others)
					{
						bool reclaim = false;
						for (int id: selective_reclaim_ids)
						{
							if (id == obj.object_id)
							{
								reclaim = true;
								break;
							}
						}
						if (!reclaim)
						{
							keep.Add(obj);
						}
					}
					others  = keep;
					overlap = true;
					for (const auto& obj: others)
					{
						const auto parent_type = heap.objects[obj.object_id].info.object.type;
						if (parent_type == GpuMemoryObjectType::RenderTexture || parent_type == GpuMemoryObjectType::StorageTexture)
						{
							create_from_objects = true;
							break;
						}
					}
				}
			} else if (multi_vertex_mixed)
			{
				// Drop reclaimed peer VBs; keep surface parents linked.
				if (vertex_reclaim_vertex_ids.Size() == others.Size())
				{
					delete_all = true;
				} else if (vertex_reclaim_vertex_ids.IsEmpty())
				{
					// All parents were surfaces (should have been multi_vertex_in_surface).
					overlap = true;
				} else
				{
					Vector<OverlappedBlock> keep;
					for (const auto& obj: others)
					{
						bool reclaim = false;
						for (int id: vertex_reclaim_vertex_ids)
						{
							if (id == obj.object_id)
							{
								reclaim = true;
								break;
							}
						}
						if (!reclaim)
						{
							keep.Add(obj);
						}
					}
					selective_reclaim_ids = vertex_reclaim_vertex_ids;
					others                = keep;
					overlap               = true;
				}
			} else if (multi_index_mixed)
			{
				// Drop reclaimed peer IBs; keep VB/surface parents linked.
				if (index_reclaim_index_ids.Size() == others.Size())
				{
					delete_all = true;
				} else if (index_reclaim_index_ids.IsEmpty())
				{
					overlap = true;
				} else
				{
					Vector<OverlappedBlock> keep;
					for (const auto& obj: others)
					{
						bool reclaim = false;
						for (int id: index_reclaim_index_ids)
						{
							if (id == obj.object_id)
							{
								reclaim = true;
								break;
							}
						}
						if (!reclaim)
						{
							keep.Add(obj);
						}
					}
					selective_reclaim_ids = index_reclaim_index_ids;
					others                = keep;
					overlap               = true;
				}
			} else if (create_generate_mips(others, info.type, heap_id))
			{
				overlap             = true;
				create_from_objects = true;
				scenario            = GpuMemoryScenario::GenerateMips;
			} else if (create_texture_triplet(others, info.type, heap_id))
			{
				overlap  = true;
				scenario = GpuMemoryScenario::TextureTriplet;
			} else if (create_maybe_deleted(others, info.type, heap_id))
			{
				delete_all = true;
			} else
			{
				if (!create_all_the_same(others, heap_id))
				{
					// Typed dump: create_all_the_same fails on mixed parent types
					// even when every relation is the same (observed multi-parent
					// StorageBuffer with two Contains parents of different kinds).
					KYTY_LOG_DEBUG("GpuMemory !create_all_the_same: new type=%s parents=%u\n", Core::EnumName(info.type).C_Str(),
					               static_cast<unsigned>(others.Size()));
					for (int vi = 0; vi < vaddr_num; vi++)
					{
						KYTY_LOG_DEBUG("  new range[%d]: vaddr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vi, vaddr[vi], size[vi]);
					}
					for (const auto& d: others)
					{
						const auto& oh = heap.objects[d.object_id];
						const auto& oi = oh.info;
						KYTY_LOG_DEBUG("  parent id=%d type=%s rel=%s read_only=%d vaddr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
						               d.object_id, Core::EnumName(oi.object.type).C_Str(), Core::EnumName(d.relation).C_Str(),
						               oi.read_only ? 1 : 0, oh.block.vaddr[0], oh.block.size[0]);
					}
					EXIT("%s\n", create_dbg_exit(U"!create_all_the_same", vaddr, size, vaddr_num, others, info.type).C_Str());
				}

				OverlapType         rel  = others.At(0).relation;
				GpuMemoryObjectType type = heap.objects[others.At(0).object_id].info.object.type;

				switch (ObjectsRelation(type, rel, info.type))
				{
					// Same policy as the single-overlap path: Texture reclaiming
					// memory previously tracked as VertexBuffers.
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::IsContainedWithin, GpuMemoryObjectType::Texture):
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Crosses, GpuMemoryObjectType::Texture):
						delete_all = true;
						break;
					case ObjectsRelation(GpuMemoryObjectType::RenderTexture, OverlapType::IsContainedWithin, GpuMemoryObjectType::Texture):
						overlap             = true;
						create_from_objects = true;
						break;
					default:
					{
						auto msg = String::FromPrintf("unknown relation: %s - %s - %s\n", Core::EnumName(type).C_Str(),
						                              Core::EnumName(rel).C_Str(), Core::EnumName(info.type).C_Str());
						EXIT("%s\n", create_dbg_exit(msg, vaddr, size, vaddr_num, others, info.type).C_Str());
					}
				}
			}
		}
	}

	EXIT_IF(delete_all && overlap);

	const bool reclaimed_existing = delete_all || !selective_reclaim_ids.IsEmpty() || !retire_after_copy_ids.IsEmpty();
	if (delete_all)
	{
		for (const auto& obj: others)
		{
			RequireDetachable(ctx, heap_id, obj.object_id, &destructors, "create_reclaim_all", info.type);
		}
		for (const auto& obj: others)
		{
			destructors.Add(Free(heap_id, obj.object_id));
		}
	} else if (!selective_reclaim_ids.IsEmpty())
	{
		for (int id: selective_reclaim_ids)
		{
			RequireDetachable(ctx, heap_id, id, &destructors, "create_selective_reclaim", info.type);
		}
		for (int id: selective_reclaim_ids)
		{
			destructors.Add(Free(heap_id, id));
		}
	}

	// Already holding m_mutex — do not call IsAllocated (it re-locks).
	for (int vi = 0; vi < vaddr_num; vi++)
	{
		if (GetHeapId(vaddr[vi], size[vi]) < 0)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !IsAllocated(vaddr[vi], size[vi]) condition ignored (continuing)\n");
		}
	}
	uint32_t relation_mask = 0;
	for (const auto& candidate: others)
	{
		const auto relation = static_cast<uint32_t>(candidate.relation);
		if (relation < 32u)
		{
			relation_mask |= 1u << relation;
		}
	}
	const uint32_t reclaimed_count =
	    delete_all ? others.Size() : static_cast<uint32_t>(selective_reclaim_ids.Size() + retire_after_copy_ids.Size());
	finish_classification(others.Size(), relation_mask, reclaimed_count, create_from_objects);
	const bool record_linked_storage_topology = overlap && !reclaimed_existing && !create_from_objects &&
	                                            info.type == GpuMemoryObjectType::StorageBuffer;
	const auto linked_storage_topology =
	    record_linked_storage_topology ? ClassifyLinkedStorageTopology(heap_id, others, info)
	                                   : DebugStatsGpuMemoryLinkedTopology::MutableOrOther;

	ObjectInfo o {};

	for (int i = 0; i < GpuObject::PARAMS_MAX; i++)
	{
		o.params[i] = info.params[i];
	}

	uint64_t hash[VADDR_BLOCKS_MAX]         = {};
	uint64_t create_vaddr[VADDR_BLOCKS_MAX] = {};
	for (int vi = 0; vi < vaddr_num; vi++)
	{
		create_vaddr[vi] = vaddr[vi];
	}
	std::vector<uint8_t>    stable_create_source;
	GpuDirtyReadObservation stable_create_read {};
	bool                    stable_create_registered = false;
	bool                    stable_create_ready      = false;
	const bool stage_stable_create =
	    vaddr_num == 1 && info.check_hash && info.read_only &&
	    (info.type == GpuMemoryObjectType::VertexBuffer || info.type == GpuMemoryObjectType::IndexBuffer) &&
	    GpuMemoryCanUseTransientReadOnlyBuffer(true, size[0], true, true) && GpuDirtyPageTracker::Instance().Enabled();
	if (stage_stable_create)
	{
		auto& tracker = GpuDirtyPageTracker::Instance();
		stable_create_registered = tracker.RegisterRange(vaddr[0], size[0]);
		if (stable_create_registered)
		{
			stable_create_source.resize(static_cast<size_t>(size[0]));
			for (uint32_t attempt = 0; attempt < 2u && !stable_create_ready; ++attempt)
			{
				stable_create_read = tracker.BeginRead(vaddr[0], size[0]);
				if (!stable_create_read.tracked)
				{
					if (tracker.Mode(vaddr[0], size[0]) == GpuDirtyTrackingMode::HashFallback)
					{
						break;
					}
					continue;
				}
				std::memcpy(stable_create_source.data(), reinterpret_cast<const void*>(vaddr[0]), stable_create_source.size());
				stable_create_ready = tracker.ReadObservationIsStable(vaddr[0], size[0], stable_create_read);
			}
			if (stable_create_ready)
			{
				create_vaddr[0] = reinterpret_cast<uint64_t>(stable_create_source.data());
			} else
			{
				(void)tracker.UnregisterRange(vaddr[0], size[0]);
				stable_create_registered = false;
			}
		}
		DebugStatsRecordStableBufferSource(true, size[0], stable_create_ready);
	}

	for (int vi = 0; vi < vaddr_num; vi++)
	{
		uint64_t cur_size = (size[vi] != 0 ? size[vi] : 4096);

		if (info.check_hash)
		{
			hash[vi] = GpuMemoryCalcHash(info.type, reinterpret_cast<const uint8_t*>(create_vaddr[vi]), cur_size);
		} else
		{
			hash[vi] = 0;
		}
	}

	o.object.type = info.type;
	o.object.obj  = nullptr;
	for (int vi = 0; vi < vaddr_num; vi++)
	{
		o.hash[vi] = hash[vi];
	}
	o.cpu_update_time = GpuMemoryGetCurrentTime();
	o.gpu_update_time = o.cpu_update_time;
	o.submit_id       = submit_id;
	o.create_func     = info.GetCreateFunc();

	bool create_from_objects_fell_back_to_cpu = false;
	if (create_from_objects)
	{
		Vector<GpuMemoryObject> objects;
		for (const auto& obj: others)
		{
			auto& o2 = heap.objects[obj.object_id].info;
			RecordUse(&o2, buffer);
			objects.Add(o2.object);
		}
		auto create_func = info.GetCreateFromObjectsFunc();
		EXIT_IF(create_func == nullptr);
		const auto create_start = std::chrono::steady_clock::now();
		o.object.obj            = create_func(ctx, buffer, o.params, scenario, objects, &o.mem);
		const auto create_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - create_start).count();
		create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::CreateFunc, static_cast<uint64_t>(create_ns));
		// Texture CreateFromObjects may leave layout UNDEFINED when no
		// format+extent surface parent is usable. Fall back to guest upload so
		// package tiles are not replaced by transparent AABBs over god-rays.
		if (info.type == GpuMemoryObjectType::Texture && o.object.obj != nullptr)
		{
			auto* tex = static_cast<TextureVulkanImage*>(o.object.obj);
			if (tex->layout == VK_IMAGE_LAYOUT_UNDEFINED)
			{
				auto update = info.GetUpdateFunc();
				EXIT_IF(update == nullptr);
				const auto update_start = std::chrono::steady_clock::now();
				update(ctx, o.params, o.object.obj, vaddr, size, vaddr_num);
				create_from_objects_fell_back_to_cpu = true;
				const auto update_ns =
				    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - update_start).count();
				create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::UpdateFunc, static_cast<uint64_t>(update_ns));
			}
		}
	} else
	{
		EXIT_IF(o.create_func == nullptr);
		const auto create_start = std::chrono::steady_clock::now();
		o.object.obj            = o.create_func(ctx, o.params, create_vaddr, size, vaddr_num, &o.mem);
		const auto create_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - create_start).count();
		create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::CreateFunc, static_cast<uint64_t>(create_ns));
	}

	if (info.type == GpuMemoryObjectType::StorageBuffer && vaddr_num == 1)
	{
		auto* storage = static_cast<StorageVulkanBuffer*>(o.object.obj);
		EXIT_IF(storage == nullptr);
		for (const auto& obj: others)
		{
			const auto& parent = heap.objects[obj.object_id];
			if (parent.info.object.type != GpuMemoryObjectType::DepthStencilBuffer)
			{
				continue;
			}
			const uint64_t htile_addr = parent.info.params[DepthStencilBufferObject::PARAM_HTILE_ADDR];
			const uint64_t htile_size = parent.info.params[DepthStencilBufferObject::PARAM_HTILE_SIZE];
			if (DepthMetaMatchesStorageRange(vaddr[0], size[0], htile_addr, htile_size))
			{
				storage->depth_meta_addr = htile_addr;
				storage->depth_meta_size = htile_size;
				o.depth_meta_bound      = true;
				break;
			}
		}
	}

	o.write_back_func = info.GetWriteBackFunc();
	o.delete_func     = info.GetDeleteFunc();
	o.update_func     = info.GetUpdateFunc();
	o.content_origin = GpuMemoryCreationContentOrigin(info.type, create_from_objects, create_from_objects_fell_back_to_cpu);
	o.content_sequence = NextContentSequence();
	o.use_num         = 1;
	o.use_last_frame  = m_current_frame;
	o.in_use          = true;
	o.read_only       = info.read_only;
	o.check_hash      = info.check_hash;
	RecordUse(&o, buffer);

	int index = 0;

	if (heap.first_free_id != -1)
	{
		index              = heap.first_free_id;
		auto& u            = heap.objects[heap.first_free_id];
		heap.first_free_id = u.next_free_id;
		EXIT_IF(u.info.logical_generation == UINT64_MAX);
		const uint64_t logical_generation = u.info.logical_generation + 1u;
		u.free                            = false;
		u.block                           = CreateBlock(vaddr, size, vaddr_num, heap_id, index);
		u.info                            = o;
		u.info.logical_generation         = logical_generation;
		u.others.Clear();
		u.scenario = scenario;
	} else
	{
		index = static_cast<int>(heap.objects.Size());

		Object h {};
		h.block = CreateBlock(vaddr, size, vaddr_num, heap_id, index);
		h.info  = o;
		h.others.Clear();
		h.scenario = scenario;
		h.free     = false;
		heap.objects.Add(h);
	}

	if (overlap)
	{
		for (const auto& obj: others)
		{
			Link(heap_id, index, obj.object_id, obj.relation, scenario);
		}
	}
	for (int id: retire_after_copy_ids)
	{
		destructors.Add(Free(heap_id, id));
	}

	if (info.check_hash)
	{
		const auto dirty_track_start    = std::chrono::steady_clock::now();
		auto&      created              = heap.objects[index];
		GpuDirtyReadObservation dirty_read[VADDR_BLOCKS_MAX] {};
		bool       tracked              = stable_create_ready || GpuDirtyPageTracker::Instance().Enabled();
		bool       attempted            = stable_create_registered;
		const auto dirty_register_start = std::chrono::steady_clock::now();
		for (int vi = 0; tracked && !stable_create_registered && vi < created.block.vaddr_num; vi++)
		{
			attempted = true;
			tracked   = GpuDirtyPageTracker::Instance().RegisterRange(created.block.vaddr[vi], created.block.size[vi]);
		}
		const auto dirty_register_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dirty_register_start).count();
		create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::DirtyRegister, static_cast<uint64_t>(dirty_register_ns));
		const auto dirty_prepare_start = std::chrono::steady_clock::now();
		if (stable_create_ready)
		{
			dirty_read[0] = stable_create_read;
		}
		for (int vi = 0; tracked && !stable_create_ready && vi < created.block.vaddr_num; vi++)
		{
			dirty_read[vi] = GpuDirtyPageTracker::Instance().BeginRead(created.block.vaddr[vi], created.block.size[vi]);
			tracked        = dirty_read[vi].tracked;
		}
		const auto dirty_prepare_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dirty_prepare_start).count();
		create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::DirtyPrepare, static_cast<uint64_t>(dirty_prepare_ns));
		if (tracked)
		{
			created.info.dirty_registered = true;
			for (int vi = 0; vi < created.block.vaddr_num; vi++)
			{
				created.info.dirty_generation[vi] = dirty_read[vi].generation;
			}
		} else if (attempted)
		{
			for (int vi = 0; vi < created.block.vaddr_num; vi++)
			{
				(void)GpuDirtyPageTracker::Instance().UnregisterRange(created.block.vaddr[vi], created.block.size[vi]);
			}
		}
		const auto dirty_track_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dirty_track_start).count();
		create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::DirtyTrack, static_cast<uint64_t>(dirty_track_ns));
	}

	cache_materialization(index);
	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);

	uint64_t created_bytes = 0;
	for (int vi = 0; vi < vaddr_num; vi++)
	{
		created_bytes += size[vi];
	}
	DebugStatsRecordAlloc(created_bytes);
	if (info.type == GpuMemoryObjectType::Texture || info.type == GpuMemoryObjectType::StorageTexture ||
	    info.type == GpuMemoryObjectType::StorageBuffer)
	{
		if (m_transient_creates_since_retirement != UINT32_MAX)
		{
			m_transient_creates_since_retirement++;
		}
	}

	if (reclaimed_existing)
	{
		create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::ReclaimNew);
	} else if (create_from_objects)
	{
		create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::NewFromObjects);
	} else if (overlap)
	{
		if (record_linked_storage_topology)
		{
			create_stats.SetLinkedTopology(linked_storage_topology);
		}
		create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::NewLinked);
	} else
	{
		create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::NewStandalone);
	}
	return o.object.obj;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
