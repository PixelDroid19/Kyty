#ifndef EMULATOR_SRC_GRAPHICS_OBJECTS_GPUMEMORYINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_OBJECTS_GPUMEMORYINTERNAL_H_

// GpuMemory module map (edit the smallest file that owns the contract):
//
//   GpuMemory.cpp            init, allocated ranges, public C wrappers, Free
//   GpuMemoryCreate.cpp      CreateObject + create_* policies + Update/Version
//   GpuMemoryFind.cpp        FindObjects / FindBlocks / QueryOverlaps / maps
//   GpuMemoryWriteback.cpp   WriteBack / Flush / FrameDone
//   GpuMemoryVulkan.cpp      VulkanAllocate/Free/Map/Bind + memory stats
//   GpuMemoryResources.cpp   GpuResources + Register/Unregister public API
//   GpuMemoryDbg.cpp         sqlite debug dump helpers
//   GpuMemoryInternal.h      GpuMemory / GpuResources private types + helpers
//
// Texture↔StorageTexture Equals alias policy lives in GpuMemory.h
// (GpuMemoryAllowsTextureStorageAlias) and is applied in GpuMemoryCreate.cpp.
// Pending StorageBuffer↔DepthStencil create-time link uses
// GpuMemoryShouldLinkPendingDepthStencilStorage; WriteBack invalidates the
// linked depth parent through the ordinary parent walk.

#include "Emulator/Graphics/Objects/GpuMemory.h"

#include "Kyty/Core/Database.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GpuDeferredDeletionQueue.h"
#include "Emulator/Graphics/GpuMemoryMaterializationCache.h"
#include "Emulator/Graphics/GpuMemoryRangeQueryCache.h"
#include "Emulator/Graphics/GraphicContext.h"

#include <set>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

constexpr int VADDR_BLOCKS_MAX = GPU_MEMORY_RANGE_SET_MAX;

enum class DebugStatsGpuMemoryLinkedTopology: uint8_t;

using OverlapType = GpuMemoryOverlapType;

enum class GpuMemoryRangeReleaseMode : uint8_t
{
	ObjectsOnly,
	PhysicalLifetime,
	Unmap,
};

constexpr uint32_t GpuMemoryStatsTypeIndex(GpuMemoryObjectType type)
{
	return static_cast<uint32_t>(type) - static_cast<uint32_t>(GpuMemoryObjectType::VideoOutBuffer);
}

constexpr uint64_t ObjectsRelation(GpuMemoryObjectType b, OverlapType relation, GpuMemoryObjectType a)
{
	return static_cast<uint64_t>(a) * static_cast<uint64_t>(GpuMemoryObjectType::Max) * static_cast<uint64_t>(OverlapType::Max) +
	       static_cast<uint64_t>(b) * static_cast<uint64_t>(OverlapType::Max) + static_cast<uint64_t>(relation);
}

// Content hash / wall-clock helpers shared by Create, Update, WriteBack.
uint64_t GpuMemoryCalcHash(GpuMemoryObjectType type, const uint8_t* buf, uint64_t size);
uint64_t GpuMemoryGetCurrentTime();

class GpuMap1
{
public:
	GpuMap1()  = default;
	~GpuMap1() = default;

	KYTY_CLASS_NO_COPY(GpuMap1);

	void Insert(uint64_t vaddr, int id)
	{
		auto& ids = m_map[vaddr];
		if (!ids.Contains(id))
		{
			ids.Add(id);
		}
	}

	void Erase(uint64_t vaddr, int id)
	{
		auto& ids = m_map[vaddr];
		ids.Remove(id);
		if (ids.IsEmpty())
		{
			m_map.Remove(vaddr);
		}
	}

	[[nodiscard]] Vector<int> FindAll(uint64_t vaddr) const { return m_map.Get(vaddr); }

	[[nodiscard]] bool IsEmpty() const
	{
		int num = 0;
		m_map.ForEach(
		    [](auto /*key*/, auto value, void* arg)
		    {
			    (*static_cast<int*>(arg)) += value->Size();
			    return true;
		    },
		    &num);
		return num == 0;
	}

private:
	Core::Hashmap<uint64_t, Vector<int>> m_map;
};

class GpuMap2
{
public:
	GpuMap2()  = default;
	~GpuMap2() = default;

	KYTY_CLASS_NO_COPY(GpuMap2);

	void Insert(uint64_t vaddr, uint64_t size, int id)
	{
		EXIT_IF(size == 0);
		auto first_page = CalcPageId(vaddr);
		auto last_page  = CalcPageId(vaddr + size - 1);
		EXIT_IF(last_page < first_page);
		for (auto page = first_page; page <= last_page; page++)
		{
			auto& ids = m_map[page];
			if (!ids.Contains(id))
			{
				ids.Add(id);
			}
		}
	}

	void Erase(uint64_t vaddr, uint64_t size, int id)
	{
		EXIT_IF(size == 0);
		auto first_page = CalcPageId(vaddr);
		auto last_page  = CalcPageId(vaddr + size - 1);
		EXIT_IF(last_page < first_page);
		for (auto page = first_page; page <= last_page; page++)
		{
			auto& ids = m_map[page];
			ids.Remove(id);
			if (ids.IsEmpty())
			{
				m_map.Remove(page);
			}
		}
	}

	[[nodiscard]] Vector<int> FindAll(uint64_t vaddr, uint64_t size) const
	{
		Vector<int> ret;
		EXIT_IF(size == 0);
		auto first_page = CalcPageId(vaddr);
		auto last_page  = CalcPageId(vaddr + size - 1);
		EXIT_IF(last_page < first_page);
		if (first_page == last_page)
		{
			// Insert keeps each bucket unique. Vector copies share immutable data
			// until mutation, so this preserves a stable result without rebuilding it.
			return m_map.Get(first_page);
		}
		for (auto page = first_page; page <= last_page; page++)
		{
			for (int id: m_map.Get(page))
			{
				if (!ret.Contains(id))
				{
					ret.Add(id);
				}
			}
		}
		return ret;
	}

	[[nodiscard]] Vector<int> FindAll(const uint64_t* vaddr, const uint64_t* size, int vaddr_num) const
	{
		EXIT_IF(vaddr == nullptr);
		EXIT_IF(size == nullptr);
		Vector<int> ret;
		for (int i = 0; i < vaddr_num; i++)
		{
			EXIT_IF(size[i] == 0);
			auto first_page = CalcPageId(vaddr[i]);
			auto last_page  = CalcPageId(vaddr[i] + size[i] - 1);
			EXIT_IF(last_page < first_page);
			for (auto page = first_page; page <= last_page; page++)
			{
				for (int id: m_map.Get(page))
				{
					if (!ret.Contains(id))
					{
						ret.Add(id);
					}
				}
			}
		}
		return ret;
	}

	// Diagnostic-only candidate walk. Both the address buckets and unique
	// candidates are capped so an opt-in trace cannot enumerate an arbitrary
	// alias graph or populate the shared overlap cache. Returning false means
	// the caller must report a truncated point-in-time result.
	template <typename Visitor>
	[[nodiscard]] bool VisitCandidatesBounded(uint64_t vaddr, uint64_t size, uint32_t max_pages, uint32_t max_candidates,
	                                          Visitor&& visitor) const
	{
		EXIT_IF(size == 0 || max_pages == 0 || max_candidates == 0);
		const auto first_page = CalcPageId(vaddr);
		const auto last_page  = CalcPageId(vaddr + size - 1);
		EXIT_IF(last_page < first_page);

		Vector<int> visited;
		uint32_t    page_count = 0;
		auto        page       = first_page;
		for (;;)
		{
			if (page_count == max_pages)
			{
				return false;
			}
			page_count++;
			for (int id: m_map.Get(page))
			{
				if (visited.Contains(id))
				{
					continue;
				}
				if (visited.Size() == max_candidates)
				{
					return false;
				}
				visited.Add(id);
				if (!visitor(id))
				{
					return false;
				}
			}
			if (page == last_page)
			{
				return true;
			}
			page++;
		}
	}

	[[nodiscard]] bool IsEmpty() const
	{
		int num = 0;
		m_map.ForEach(
		    [](auto /*key*/, auto value, void* arg)
		    {
			    (*static_cast<int*>(arg)) += value->Size();
			    return true;
		    },
		    &num);
		return num == 0;
	}

private:
	// This index only produces candidates; FindBlocks validates each range with
	// GpuMemoryClassifyRangeSets before accepting it. A 1 MiB bucket keeps that exact
	// contract while avoiding thousands of 16 KiB probes for large descriptors.
	static constexpr uint32_t PAGE_BITS = 20u;

	static uint32_t CalcPageId(uint64_t vaddr)
	{
		EXIT_IF((vaddr >> (PAGE_BITS + 32u)) != 0);
		return static_cast<uint32_t>(vaddr >> PAGE_BITS);
	}
	Core::Hashmap<uint32_t, Vector<int>> m_map;
};

class GpuMemory
{
public:
	GpuMemory()
	{
		if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); }
		DbgInit();
	}
	virtual ~GpuMemory() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(GpuMemory);

	bool                           IsAllocated(uint64_t vaddr, uint64_t size);
	GpuMemoryRangeValidationStatus ValidateAllocatedRange(uint64_t vaddr, uint64_t size);
	[[nodiscard]] uint64_t         GetAllocatedRangePrefix(uint64_t vaddr, uint64_t maximum_size);
	void                           SetAllocatedRange(uint64_t vaddr, uint64_t size);
	void                           Free(GraphicContext* ctx, uint64_t vaddr, uint64_t size, GpuMemoryRangeReleaseMode mode);

	void* CreateObject(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
	                   int vaddr_num, const GpuObject& info);
	void  ResetHash(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type);
	void  FrameDone(GraphicContext* ctx);

	Vector<GpuMemoryObject> FindObjects(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type, bool exact,
	                                    bool only_first, const SubmissionId* submission = nullptr);
	bool                    QueryOverlaps(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryOverlapSnapshot* out);
	bool                    QueryRangeProvenance(uint64_t vaddr, uint64_t size, GpuMemoryRangeProvenance* out);
	bool                    CanSnapshotReadOnlyBuffer(uint64_t vaddr, uint64_t size);
	bool                    CaptureSnapshotReadOnlyBuffer(uint64_t vaddr, uint64_t size, void* dst, uint64_t* validation_ns,
	                                                      uint64_t* copy_ns);
	bool                    CompareSnapshotReadOnlyBuffer(uint64_t vaddr, uint64_t size, const void* snapshot, bool* matches,
	                                                      uint64_t* validation_ns, uint64_t* compare_ns);

	// Sync: GPU -> CPU
	void WriteBackCompletedSubmission(GraphicContext* ctx, SubmissionId submission);
	void WriteBackAllCompleted(GraphicContext* ctx);
	void               WriteBackStorageRanges(GraphicContext* ctx, const GpuMemoryGuestRanges& ranges, GpuQueueId consumer);
	[[nodiscard]] bool PendingStorageWriteBack(const GpuMemoryGuestRanges& ranges, GpuQueueId consumer, SubmissionId* dependency);
	[[nodiscard]] bool FindExactWritableStorage(uint64_t vaddr, uint64_t size, GpuMemoryStorageWriteIdentity* identity);
	[[nodiscard]] bool MarkStorageGuestPublished(const GpuMemoryStorageWriteIdentity& identity, GpuQueueId queue,
	                                             const GpuWritebackPageCache::UniformWords* uniform_words);

	// Sync: CPU -> GPU
	void Flush(GraphicContext* ctx, uint64_t vaddr, uint64_t size);
	void FlushAll(GraphicContext* ctx);

	void DbgInit();
	void DbgDbDump();
	void DbgDbSave(const String& file_name);
	void CompleteSubmission(SubmissionId submission);

private:
	static constexpr int OBJ_OVERLAPS_MAX = 2;

	struct AllocatedRange
	{
		uint64_t vaddr = 0;
		uint64_t size  = 0;
	};

	struct ObjectInfo
	{
		GpuMemoryObject              object;
		uint64_t                     params[GpuObject::PARAMS_MAX]      = {};
		uint64_t                     hash[VADDR_BLOCKS_MAX]             = {};
		uint64_t                     cpu_update_time                    = 0;
		uint64_t                     gpu_update_time                    = 0;
		uint64_t                     submit_id                          = 0;
		GpuObject::create_func_t     create_func                        = nullptr;
		GpuObject::write_back_func_t write_back_func                    = nullptr;
		GpuObject::delete_func_t     delete_func                        = nullptr;
		GpuObject::update_func_t     update_func                        = nullptr;
		uint64_t                     use_last_frame                     = 0;
		uint64_t                     use_num                            = 0;
		bool                         in_use                             = false;
		bool                         read_only                          = false;
		bool                         check_hash                         = false;
		bool                         dirty_registered                   = false;
		bool                         depth_meta_bound                   = false;
		uint64_t                     dirty_generation[VADDR_BLOCKS_MAX] = {};
		uint64_t                     content_sequence                  = 0;
		GpuMemoryContentOrigin       content_origin                    = GpuMemoryContentOrigin::Unknown;
		GpuSubmissionHighWater       submission_uses;
		// Writable uses since creation, and the count a device-side publication
		// to the guest device-address view covered (0: none) with its queue.
		uint64_t                            write_uses                 = 0;
		// Logical times (GpuMemoryGetCurrentTime) of the latest writable use and
		// of the latest device write recorded into this object.
		uint64_t                            write_time                 = 0;
		uint64_t                            device_write_time          = 0;
		uint64_t                            guest_published_write_uses = 0;
		uint32_t                            guest_published_queue      = 0;
		bool                                guest_published_uniform    = false;
		GpuWritebackPageCache::UniformWords guest_published_words {};
		// Incarnation of the host Vulkan backing, not a content revision.
		// In-place uploads retain it; an atomic COW swap advances it.
		uint64_t backing_generation = 1;
		// Incarnation of this logical slot. Reusing a freed object id advances
		// it so bounded acquisition caches cannot observe an ABA replacement.
		uint64_t     logical_generation = 1;
		VulkanMemory mem;
	};

	struct OverlappedBlock
	{
		OverlapType relation  = OverlapType::None;
		int         object_id = -1;
	};

	using OverlapQueryCache = GpuMemoryRangeQueryCache<Vector<OverlappedBlock>, 4096>;

	struct Materialization
	{
		int                                   heap_id                  = -1;
		int                                   object_id                = -1;
		uint64_t                              logical_generation       = 0;
		GpuObject::create_func_t              create_func              = nullptr;
		GpuObject::create_from_objects_func_t create_from_objects_func = nullptr;
		GpuObject::write_back_func_t          write_back_func          = nullptr;
		GpuObject::delete_func_t              delete_func              = nullptr;
		GpuObject::update_func_t              update_func              = nullptr;
	};

	using MaterializationCache = GpuMemoryMaterializationCache<Materialization, 2048>;
	using AllocatedValidationCache = GpuMemoryRangeQueryCache<GpuMemoryRangeValidationStatus, 4096>;
	using AllocatedPrefixCache     = GpuMemoryRangeQueryCache<uint64_t, 4096>;
	using OverlapSnapshotCache     = GpuMemoryRangeQueryCache<GpuMemoryOverlapSnapshot, 4096>;

	struct Block
	{
		uint64_t vaddr[VADDR_BLOCKS_MAX] = {};
		uint64_t size[VADDR_BLOCKS_MAX]  = {};
		int      vaddr_num               = 0;
	};

	struct Object
	{
		Block                   block;
		ObjectInfo              info;
		Vector<OverlappedBlock> others;
		GpuMemoryScenario       scenario     = GpuMemoryScenario::Common;
		bool                    free         = true;
		int                     next_free_id = -1;
	};

	struct Heap
	{
		AllocatedRange     range;
		Vector<Object>     objects;
		uint64_t           objects_size  = 0;
		int                first_free_id = -1;
		GpuMap1*           objects_map1  = nullptr;
		GpuMap2*           objects_map2  = nullptr;
		OverlapQueryCache* overlap_cache = nullptr;
	};

	struct Destructor
	{
		void*                    obj         = nullptr;
		GpuObject::delete_func_t delete_func = nullptr;
		GpuMemoryObjectType      type        = GpuMemoryObjectType::Invalid;
		GpuSubmissionHighWater   submission_uses;
		VulkanMemory             mem;
	};

	[[nodiscard]] Destructor Free(int heap_id, int object_id);
	void RequireDetachable(GraphicContext* ctx, int heap_id, int object_id, Vector<Destructor>* destructors, const char* operation,
	                       GpuMemoryObjectType incoming_type = GpuMemoryObjectType::Invalid);
	void WriteBackObjectLocked(GraphicContext* ctx, int heap_id, int object_id, Vector<Destructor>* destructors,
	                           const SubmissionId* publishing_submission = nullptr);
	void RecordUse(ObjectInfo* object, SubmissionId submission);
	void RecordUse(ObjectInfo* object, CommandBuffer* buffer);
	void ScheduleDestructors(GraphicContext* ctx, Vector<Destructor>* destructors);
	void ScheduleDestructorsOutsideMutationLocks(GraphicContext* ctx, Vector<Destructor>* destructors);
	void VersionBacking(GraphicContext* ctx, int heap_id, int obj_id, Vector<Destructor>* destructors,
	                    const uint64_t* source_vaddr = nullptr);

	Vector<OverlappedBlock> FindBlocks(int heap_id, const uint64_t* vaddr, const uint64_t* size, int vaddr_num, bool only_first = false);
	bool  FindFast(int heap_id, const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type, bool only_first,
	               int* id);
	Block CreateBlock(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, int heap_id, int obj_id);
	void  DeleteBlock(Block* b, int heap_id, int obj_id);
	void  Link(int heap_id, int id1, int id2, OverlapType rel, GpuMemoryScenario scenario);
	[[nodiscard]] uint64_t NextContentSequence();
	[[nodiscard]] DebugStatsGpuMemoryLinkedTopology ClassifyLinkedStorageTopology(
	    int heap_id, const Vector<OverlappedBlock>& parents, const GpuObject& incoming) const;
	[[nodiscard]] int GetHeapId(uint64_t vaddr, uint64_t size) const;
	[[nodiscard]] int HeapAt(uint64_t address) const;
	void              HeapsIntersecting(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, std::vector<int>* out) const;
	void              RebuildHeapIndex();
	void              ForgetHeapStorageObjects(int removed_heap_id);
	// (heap id, object id) of each in-use writable StorageBuffer overlapping a range
	// whose writes are not published to the guest device-address view on `consumer`.
	// The ranges are sorted by address and disjoint.
	[[nodiscard]] std::vector<std::pair<int, int>> CollectWritableStorage(const GpuMemoryGuestRanges& ranges, GpuQueueId consumer) const;
	GpuMemoryRangeValidationStatus ValidateAllocatedRangeLocked(uint64_t vaddr, uint64_t size,
	                                                            const GpuMemoryRangeQueryKey& query);
	bool QueryOverlapsLocked(const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
	                         const GpuMemoryRangeQueryKey& query, GpuMemoryOverlapSnapshot* out);

	// Update (CPU -> GPU)
	void Update(uint64_t submit_id, GraphicContext* ctx, int heap_id, int obj_id, Vector<Destructor>* destructors = nullptr);

	bool create_existing(const Vector<OverlappedBlock>& others, const GpuObject& info, int heap_id, const uint64_t* vaddr,
	                     const uint64_t* size, int vaddr_num, int* id, bool* covered_reuse, int* stale_reuse_id);
	bool create_generate_mips(const Vector<OverlappedBlock>& others, GpuMemoryObjectType type, int heap_id);
	bool create_texture_triplet(const Vector<OverlappedBlock>& others, GpuMemoryObjectType type, int heap_id);
	bool create_cpu_texture_storage_alias(const Vector<OverlappedBlock>& others, const GpuObject& info, int heap_id,
	                                     const uint64_t* vaddr, const uint64_t* size, int vaddr_num) const;
	bool create_maybe_deleted(const Vector<OverlappedBlock>& others, GpuMemoryObjectType type, int heap_id);
	bool create_all_the_same(const Vector<OverlappedBlock>& others, int heap_id);

	[[nodiscard]] String create_dbg_exit(const String& msg, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
	                                     const Vector<OverlappedBlock>& others, GpuMemoryObjectType type);
	// One line per overlapping parent (type, relation, scenario, parent count, range, raw parameters)
	// plus the parameters of the object being created: what a refused overlap needs to be classified.
	[[nodiscard]] String create_dbg_parents(int heap_id, const Vector<OverlappedBlock>& others, const GpuObject& info);

	Core::Mutex m_mutex;
	// Serializes logical object graph mutations while VersionBacking temporarily
	// releases m_mutex for host allocation/upload work.
	Core::Mutex m_backing_mutation_mutex;

	Vector<Heap> m_heaps;
	// [begin, end) address spans, sorted and disjoint, each naming the lowest heap
	// index that covers it: GetHeapId's first-match answer by binary search.
	// Every covering heap, ascending, is m_heap_index_open[open_first, +open_count).
	struct HeapSpan
	{
		uint64_t begin      = 0;
		uint64_t end        = 0;
		int      heap_id    = -1;
		uint32_t open_first = 0;
		uint32_t open_count = 0;
	};
	std::vector<HeapSpan> m_heap_index;
	std::vector<int>      m_heap_index_open;
	// (heap id, object id) of every live StorageBuffer object with a write-back:
	// the only objects a device-address write-back or its wait can touch.
	std::set<std::pair<int, int>> m_storage_objects;
	// The in-use, writable subset of m_storage_objects: the only candidates of
	// the per-draw pending-write scans, which titles with thousands of live
	// storage buffers otherwise pay in full for every device-address draw.
	std::set<std::pair<int, int>> m_writable_storage;
	// (heap id, object id) of every live object whose GPU writes are not yet
	// written back (in use, writable, with a write-back): the only candidates
	// of a completed-submission write-back, which otherwise visits every object
	// of every heap on each completion.
	std::set<std::pair<int, int>> m_pending_write_back;
	void                          SyncWriteBackIndexes(int heap_id, int object_id);

	uint64_t m_current_frame                      = 0;
	uint64_t m_content_sequence                   = 0;
	uint32_t m_transient_creates_since_retirement = 0;
	uint32_t m_retirement_heap_cursor            = 0;
	uint32_t m_retirement_object_cursor          = 0;

	MaterializationCache m_materialization_cache;
	AllocatedValidationCache m_allocated_validation_cache;
	AllocatedPrefixCache     m_allocated_prefix_cache;
	OverlapSnapshotCache     m_overlap_snapshot_cache;

	GpuDeferredDeletionQueue m_deferred_deletions;

	Core::Database::Connection m_db;
	Core::Database::Statement* m_db_add_range  = nullptr;
	Core::Database::Statement* m_db_add_object = nullptr;
};

class GpuResources
{
public:
	struct Info
	{
		uint32_t owner  = 0;
		bool     free   = true;
		uint64_t memory = 0;
		size_t   size   = 0;
		String   name;
		uint32_t type      = 0;
		uint64_t user_data = 0;
	};

	GpuResources() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !Core::Thread::IsMainThread() condition ignored (continuing)\n"); } }
	virtual ~GpuResources() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(GpuResources);

	uint32_t AddOwner(const String& name);
	uint32_t AddResource(uint32_t owner_handle, uint64_t memory, size_t size, const String& name, uint32_t type, uint64_t user_data);
	void     DeleteOwner(uint32_t owner_handle);
	void     DeleteResources(uint32_t owner_handle);
	void     DeleteResource(uint32_t resource_handle);

	bool FindInfo(uint64_t memory, Info* dst);

private:
	struct Owner
	{
		String name;
		bool   free = true;
	};

	Core::Mutex m_mutex;

	Vector<Owner> m_owners;
	Vector<Info>  m_infos;
};

extern GpuMemory*    g_gpu_memory;
extern GpuResources* g_gpu_resources;

// Vulkan host-memory accounting (defined in GpuMemoryVulkan.cpp).
void GpuMemoryVulkanStatsInit();


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_SRC_GRAPHICS_OBJECTS_GPUMEMORYINTERNAL_H_
