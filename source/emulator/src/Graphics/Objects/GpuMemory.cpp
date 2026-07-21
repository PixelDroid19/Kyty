#include "Emulator/Graphics/Objects/GpuMemory.h"

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
#include "Emulator/Graphics/GpuMemoryMaterializationCache.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GpuMemoryRangeQueryCache.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
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

constexpr int VADDR_BLOCKS_MAX = 3;

using OverlapType = GpuMemoryOverlapType;

constexpr uint32_t GpuMemoryStatsTypeIndex(GpuMemoryObjectType type)
{
	return static_cast<uint32_t>(type) - static_cast<uint32_t>(GpuMemoryObjectType::VideoOutBuffer);
}

constexpr uint64_t ObjectsRelation(GpuMemoryObjectType b, OverlapType relation, GpuMemoryObjectType a)
{
	return static_cast<uint64_t>(a) * static_cast<uint64_t>(GpuMemoryObjectType::Max) * static_cast<uint64_t>(OverlapType::Max) +
	       static_cast<uint64_t>(b) * static_cast<uint64_t>(OverlapType::Max) + static_cast<uint64_t>(relation);
}

static bool addr_in_block(uint64_t block_addr, uint64_t block_size, uint64_t addr)
{
	return addr >= block_addr && addr < block_addr + block_size;
};

static OverlapType GetOverlapType(uint64_t vaddr_a, uint64_t size_a, uint64_t vaddr_b, uint64_t size_b)
{
	// KYTY_PROFILER_FUNCTION();

	EXIT_IF(size_a == 0 || size_b == 0);

	if (vaddr_a == vaddr_b && size_a == size_b)
	{
		return OverlapType::Equals;
	}

	bool a_b  = addr_in_block(vaddr_a, size_a, vaddr_b);
	bool a_lb = addr_in_block(vaddr_a, size_a, vaddr_b + size_b - 1);
	bool b_a  = addr_in_block(vaddr_b, size_b, vaddr_a);
	bool b_la = addr_in_block(vaddr_b, size_b, vaddr_a + size_a - 1);

	if (a_b && a_lb)
	{
		return OverlapType::Contains;
	}

	if (b_a && b_la)
	{
		return OverlapType::IsContainedWithin;
	}

	if ((a_b && b_la) || (b_a && a_lb))
	{
		return OverlapType::Crosses;
	}

	return OverlapType::None;
}

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
	// GetOverlapType before accepting it. A 1 MiB bucket keeps that exact
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
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
		DbgInit();
	}
	virtual ~GpuMemory() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(GpuMemory);

	bool IsAllocated(uint64_t vaddr, uint64_t size);
	GpuMemoryRangeValidationStatus ValidateAllocatedRange(uint64_t vaddr, uint64_t size);
	void SetAllocatedRange(uint64_t vaddr, uint64_t size);
	void Free(GraphicContext* ctx, uint64_t vaddr, uint64_t size, bool unmap);

	void* CreateObject(uint64_t submit_id, GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
	                   int vaddr_num, const GpuObject& info);
	void  ResetHash(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type);
	void  FrameDone();

	Vector<GpuMemoryObject> FindObjects(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type, bool exact,
	                                    bool only_first, const SubmissionId* submission = nullptr);
	bool QueryOverlaps(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryOverlapSnapshot* out);

	// Sync: GPU -> CPU
	void WriteBackCompletedSubmission(GraphicContext* ctx, SubmissionId submission);
	void WriteBackAllCompleted(GraphicContext* ctx);
	// Write back StorageBuffers that overlap a sample range before CPU detile.
	void WriteBackStorageRange(GraphicContext* ctx, uint64_t vaddr, uint64_t size);

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
		uint64_t                     params[GpuObject::PARAMS_MAX] = {};
		uint64_t                     hash[VADDR_BLOCKS_MAX]        = {};
		uint64_t                     cpu_update_time               = 0;
		uint64_t                     gpu_update_time               = 0;
		uint64_t                     submit_id                     = 0;
		GpuObject::create_func_t     create_func                   = nullptr;
		GpuObject::write_back_func_t write_back_func               = nullptr;
		GpuObject::delete_func_t     delete_func                   = nullptr;
		GpuObject::update_func_t     update_func                   = nullptr;
		uint64_t                     use_last_frame                = 0;
		uint64_t                     use_num                       = 0;
		bool                         in_use                        = false;
		bool                         read_only                     = false;
		bool                         check_hash                    = false;
		bool                         dirty_registered              = false;
		uint64_t                     dirty_generation[VADDR_BLOCKS_MAX] = {};
		GpuSubmissionHighWater       submission_uses;
		// Incarnation of the host Vulkan backing, not a content revision.
		// In-place uploads retain it; an atomic COW swap advances it.
		uint64_t                     backing_generation = 1;
		// Incarnation of this logical slot. Reusing a freed object id advances
		// it so bounded acquisition caches cannot observe an ABA replacement.
		uint64_t                     logical_generation = 1;
		VulkanMemory                 mem;
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
		AllocatedRange range;
		Vector<Object> objects;
		uint64_t       objects_size  = 0;
		int            first_free_id = -1;
		GpuMap1*       objects_map1  = nullptr;
		GpuMap2*       objects_map2  = nullptr;
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
	void                     RequireDetachable(GraphicContext* ctx, int heap_id, int object_id, Vector<Destructor>* destructors,
	                                           const char* operation,
	                                           GpuMemoryObjectType incoming_type = GpuMemoryObjectType::Invalid);
	void                     WriteBackObjectLocked(GraphicContext* ctx, int heap_id, int object_id, Vector<Destructor>* destructors,
	                                               const SubmissionId* publishing_submission = nullptr);
	void                     RecordUse(ObjectInfo* object, SubmissionId submission);
	void                     RecordUse(ObjectInfo* object, CommandBuffer* buffer);
	void                     ScheduleDestructors(GraphicContext* ctx, Vector<Destructor>* destructors);
	void                     ScheduleDestructorsOutsideMutationLocks(GraphicContext* ctx, Vector<Destructor>* destructors);
	void                     VersionBacking(GraphicContext* ctx, int heap_id, int obj_id, Vector<Destructor>* destructors);

	Vector<OverlappedBlock> FindBlocks_slow(int heap_id, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
	                                        bool only_first = false);
	Vector<OverlappedBlock> FindBlocks(int heap_id, const uint64_t* vaddr, const uint64_t* size, int vaddr_num, bool only_first = false);
	bool  FindFast(int heap_id, const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type, bool only_first,
	               int* id);
	Block CreateBlock(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, int heap_id, int obj_id);
	void  DeleteBlock(Block* b, int heap_id, int obj_id);
	void  Link(int heap_id, int id1, int id2, OverlapType rel, GpuMemoryScenario scenario);
	int   GetHeapId(uint64_t vaddr, uint64_t size);

	// Update (CPU -> GPU)
	void Update(uint64_t submit_id, GraphicContext* ctx, int heap_id, int obj_id, Vector<Destructor>* destructors = nullptr);

	bool create_existing(const Vector<OverlappedBlock>& others, const GpuObject& info, int heap_id, const uint64_t* vaddr,
	                     const uint64_t* size, int vaddr_num, int* id, bool* covered_reuse);
	bool create_generate_mips(const Vector<OverlappedBlock>& others, GpuMemoryObjectType type, int heap_id);
	bool create_texture_triplet(const Vector<OverlappedBlock>& others, GpuMemoryObjectType type, int heap_id);
	bool create_maybe_deleted(const Vector<OverlappedBlock>& others, GpuMemoryObjectType type, int heap_id);
	bool create_all_the_same(const Vector<OverlappedBlock>& others, int heap_id);

	[[nodiscard]] String create_dbg_exit(const String& msg, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
	                                     const Vector<OverlappedBlock>& others, GpuMemoryObjectType type);

	Core::Mutex m_mutex;
	// Serializes logical object graph mutations while VersionBacking temporarily
	// releases m_mutex for host allocation/upload work.
	Core::Mutex m_backing_mutation_mutex;

	Vector<Heap> m_heaps;

	uint64_t m_current_frame = 0;

	MaterializationCache m_materialization_cache;

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

	GpuResources() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
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

static GpuMemory*    g_gpu_memory    = nullptr;
static GpuResources* g_gpu_resources = nullptr;

uint32_t GpuResources::AddOwner(const String& name)
{
	Core::LockGuard lock(m_mutex);

	Owner n;
	n.name = name;
	n.free = false;

	uint32_t index = 0;
	for (auto& b: m_owners)
	{
		if (b.free)
		{
			b = n;
			return index;
		}
		index++;
	}

	m_owners.Add(n);

	return index;
}

uint32_t GpuResources::AddResource(uint32_t owner_handle, uint64_t memory, size_t size, const String& name, uint32_t type,
                                   uint64_t user_data)
{
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(!m_owners.IndexValid(owner_handle));
	EXIT_NOT_IMPLEMENTED(memory == 0);

	Info info;
	info.owner     = owner_handle;
	info.memory    = memory;
	info.free      = false;
	info.name      = name;
	info.size      = size;
	info.type      = type;
	info.user_data = user_data;

	uint32_t index = 0;
	for (auto& i: m_infos)
	{
		if (i.free)
		{
			i = info;
			return index;
		}
		index++;
	}

	m_infos.Add(info);

	return index;
}

void GpuResources::DeleteOwner(uint32_t owner_handle)
{
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(!m_owners.IndexValid(owner_handle));

	for (auto& i: m_infos)
	{
		if (!i.free && i.owner == owner_handle)
		{
			i.free = true;
		}
	}

	EXIT_NOT_IMPLEMENTED(m_owners[owner_handle].free);

	m_owners[owner_handle].free = true;
}

void GpuResources::DeleteResources(uint32_t owner_handle)
{
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(!m_owners.IndexValid(owner_handle));

	for (auto& i: m_infos)
	{
		if (!i.free && i.owner == owner_handle)
		{
			i.free = true;
		}
	}
}

void GpuResources::DeleteResource(uint32_t resource_handle)
{
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(!m_infos.IndexValid(resource_handle));

	EXIT_NOT_IMPLEMENTED(m_infos[resource_handle].free);

	m_infos[resource_handle].free = true;
}

bool GpuResources::FindInfo(uint64_t memory, Info* dst)
{
	EXIT_IF(dst == nullptr);

	Core::LockGuard lock(m_mutex);

	// NOLINTNEXTLINE(readability-use-anyofallof)
	for (const auto& i: m_infos)
	{
		if (!i.free && memory >= i.memory && memory < i.memory + i.size)
		{
			*dst = i;
			return true;
		}
	}

	return false;
}

void GpuMemory::SetAllocatedRange(uint64_t vaddr, uint64_t size)
{
	EXIT_IF(size == 0);

	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	EXIT_NOT_IMPLEMENTED(IsAllocated(vaddr, size));

	Core::LockGuard lock(m_mutex);

	Heap h;
	h.range.vaddr  = vaddr;
	h.range.size   = size;
	h.objects_map1 = new GpuMap1;
	h.objects_map2 = new GpuMap2;
	h.overlap_cache = new OverlapQueryCache;

	m_heaps.Add(h);
}

bool GpuMemory::IsAllocated(uint64_t vaddr, uint64_t size)
{
	EXIT_IF(size == 0);

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

static uint64_t calc_hash(GpuMemoryObjectType type, const uint8_t* buf, uint64_t size)
{
	KYTY_PROFILER_FUNCTION();

	if (size == 0 || buf == nullptr)
	{
		return 0;
	}
	const auto start  = std::chrono::steady_clock::now();
	const auto result = XXH3_64bits(buf, size);
	const auto elapsed =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
	DebugStatsRecordGpuMemoryHash(GpuMemoryStatsTypeIndex(type), size, static_cast<uint64_t>(elapsed));
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::Hash, static_cast<uint64_t>(elapsed), size);
	return result;
}

static uint64_t get_current_time()
{
	static std::atomic_uint64_t t(0);
	return ++t;
}

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

void GpuMemory::VersionBacking(GraphicContext* ctx, int heap_id, int obj_id, Vector<Destructor>* destructors)
{
	EXIT_IF(destructors == nullptr);

	auto& heap = m_heaps[heap_id];
	auto& h    = heap.objects[obj_id];
	EXIT_IF(h.free);
	auto& o = h.info;

	if (o.object.obj == nullptr || o.create_func == nullptr || o.delete_func == nullptr)
	{
		EXIT("GpuMemory backing version unsupported for type=%s: object=%s create=%s delete=%s\n",
		     Core::EnumName(o.object.type).C_Str(), o.object.obj != nullptr ? "set" : "null",
		     o.create_func != nullptr ? "set" : "null", o.delete_func != nullptr ? "set" : "null");
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
		vaddr[i] = h.block.vaddr[i];
		size[i]  = h.block.size[i];
	}
	const int vaddr_num = h.block.vaddr_num;

	// Host creation/upload may block on its private transfer fence. Keep the
	// logical object stable with m_backing_mutation_mutex, but never hold the
	// global GpuMemory mutex across that work.
	m_mutex.Unlock();
	VulkanMemory new_memory {};
	const auto create_start = std::chrono::steady_clock::now();
	void* const new_object  = create_func(ctx, params, vaddr, size, vaddr_num, &new_memory);
	const auto create_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - create_start).count();
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::CreateFunc,
	                                               static_cast<uint64_t>(create_ns));
	m_mutex.Lock();

	auto& current_heap = m_heaps[heap_id];
	auto& current      = current_heap.objects[obj_id];
	const bool valid = !current.free && current.info.object.type == object_type && current.info.object.obj == old_object &&
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

	auto& current_info = current.info;
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
		bool                    hash_tracked[VADDR_BLOCKS_MAX] = {};
		bool                    tracker_ready = o.check_hash && o.dirty_registered;

		for (int vi = 0; vi < h.block.vaddr_num; vi++)
		{
			EXIT_IF(h.block.size[vi] == 0);

			bool clean = false;
			if (tracker_ready)
			{
				dirty_read[vi] = GpuDirtyPageTracker::Instance().BeginRead(h.block.vaddr[vi], h.block.size[vi]);
				if (dirty_read[vi].tracked)
				{
					clean =
					    !GpuDirtyPageTracker::Instance().ChangedSince(h.block.vaddr[vi], h.block.size[vi], o.dirty_generation[vi]);
				}
			}
			if (clean)
			{
				hash[vi] = o.hash[vi];
			} else if (o.check_hash)
			{
				hash[vi] = calc_hash(o.object.type, reinterpret_cast<const uint8_t*>(h.block.vaddr[vi]), h.block.size[vi]);
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
				printf("Update (CPU -> GPU): type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n",
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

		const bool pending_uses = !m_deferred_deletions.AreDependenciesComplete(o.submission_uses.Dependencies());
		const bool write_back_capable = o.write_back_func != nullptr && !o.read_only;
		const auto mutation =
		    GpuMemoryChooseMutationAction(need_update, surface_parent, pending_uses, write_back_capable);

		if (mutation == GpuMemoryMutationAction::RejectWriteBackConflict)
		{
			const auto& dependencies = o.submission_uses.Dependencies();
			const auto  pending = std::find_if(dependencies.begin(), dependencies.end(),
			                                  [this](const auto& dependency)
			                                  {
				                                  return !m_deferred_deletions.AreDependenciesComplete({dependency});
			                                  });
			EXIT_IF(pending == dependencies.end());
			EXIT("GpuMemory cannot version an in-flight write-back backing: type=%s vaddr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " queue=%" PRIu32 " sequence=%" PRIu64 "\n",
			     Core::EnumName(o.object.type).C_Str(), h.block.vaddr[0], h.block.size[0], pending->queue.Value(),
			     pending->sequence);
		}
		if (mutation == GpuMemoryMutationAction::VersionBacking)
		{
			VersionBacking(ctx, heap_id, obj_id, destructors);
		} else if (mutation == GpuMemoryMutationAction::UpdateInPlace)
		{
			const auto update_start = std::chrono::steady_clock::now();
			o.update_func(ctx, o.params, o.object.obj, h.block.vaddr, h.block.size, h.block.vaddr_num);
			const auto update_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - update_start).count();
			DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::UpdateFunc,
			                                               static_cast<uint64_t>(update_ns));
		}

		// VersionBacking may have released m_mutex, so reacquire the current
		// logical object instead of retaining a stale reference.
		auto& updated = m_heaps[heap_id].objects[obj_id].info;
		for (int vi = 0; vi < h.block.vaddr_num; vi++)
		{
			updated.hash[vi] = hash[vi];
		}
		updated.gpu_update_time = get_current_time();
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
                                const uint64_t* size, int vaddr_num, int* id, bool* covered_reuse)
{
	EXIT_IF(vaddr == nullptr || size == nullptr || id == nullptr || covered_reuse == nullptr);

	auto& heap = m_heaps[heap_id];

	uint64_t               max_gpu_update_time = 0;
	const OverlappedBlock* latest_block        = nullptr;
	int                    reusable_index_id   = -1;
	uint64_t               reusable_index_size = UINT64_MAX;
	*covered_reuse                              = false;

	for (const auto& obj: others)
	{
		auto& h = heap.objects[obj.object_id];
		EXIT_IF(h.free);
		auto& o = h.info;

		if (h.scenario == GpuMemoryScenario::Common && obj.relation == OverlapType::Equals && o.object.type == info.type &&
		    info.Equal(o.params))
		{
			*id = obj.object_id;
			return true;
		}

		if (vaddr_num == 1 && h.block.vaddr_num == 1 && h.scenario == GpuMemoryScenario::Common &&
		    o.object.type == GpuMemoryObjectType::IndexBuffer && info.type == GpuMemoryObjectType::IndexBuffer &&
		    info.Equal(o.params) &&
		    GpuMemoryCanReuseIndexBacking(h.block.vaddr[0], h.block.size[0], vaddr[0], size[0]) &&
		    h.block.size[0] < reusable_index_size)
		{
			reusable_index_id   = obj.object_id;
			reusable_index_size = h.block.size[0];
		}

		if (o.gpu_update_time > max_gpu_update_time)
		{
			max_gpu_update_time = o.gpu_update_time;
			latest_block        = &obj;
		}
	}

	if (reusable_index_id >= 0)
	{
		*id            = reusable_index_id;
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
	printf("%s\n", str.C_Str());
	return str;
}

void GpuMemory::RecordUse(ObjectInfo* object, SubmissionId submission)
{
	EXIT_IF(object == nullptr);
	EXIT_NOT_IMPLEMENTED(object->submission_uses.RecordUse(submission) != GpuDeferredDeletionResult::Success);
}

void GpuMemory::RecordUse(ObjectInfo* object, CommandBuffer* buffer)
{
	EXIT_IF(object == nullptr);
	if (buffer == nullptr)
	{
		return;
	}

	SubmissionId submission;
	EXIT_NOT_IMPLEMENTED(!buffer->GetSubmissionId(&submission));
	RecordUse(object, submission);
}

void GpuMemory::ScheduleDestructors(GraphicContext* ctx, Vector<Destructor>* destructors)
{
	EXIT_IF(ctx == nullptr || destructors == nullptr);

	for (auto& destructor: *destructors)
	{
		EXIT_IF(destructor.delete_func == nullptr || destructor.obj == nullptr);

		auto delete_func = destructor.delete_func;
		auto* object     = destructor.obj;
		auto  memory     = destructor.mem;
		const auto result = m_deferred_deletions.Enqueue(
		    destructor.submission_uses.Dependencies(),
		    [ctx, delete_func, object, memory]() mutable { delete_func(ctx, object, &memory); });
		EXIT_NOT_IMPLEMENTED(result != GpuDeferredDeletionResult::Success);
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
	EXIT_NOT_IMPLEMENTED(m_deferred_deletions.CompleteSubmission(submission) != GpuDeferredDeletionResult::Success);
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
	DebugStatsGpuMemoryCreateTrace create_stats(GpuMemoryStatsTypeIndex(info.type), requested_bytes,
	                                           static_cast<uint32_t>(vaddr_num));
	const auto backing_lock_start = std::chrono::steady_clock::now();
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	const auto backing_lock_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - backing_lock_start).count();
	create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::BackingLockWait, static_cast<uint64_t>(backing_lock_ns));
	const auto registry_lock_start = std::chrono::steady_clock::now();
	Core::LockGuard lock(m_mutex);
	const auto registry_lock_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - registry_lock_start).count();
	create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::RegistryLockWait, static_cast<uint64_t>(registry_lock_ns));
	const auto classification_start = std::chrono::steady_clock::now();
	bool       classification_done  = false;
	const auto finish_classification = [&](uint32_t candidates = 0, uint32_t relation_mask = 0, uint32_t reclaimed = 0,
	                                      bool from_objects = false)
	{
		if (classification_done)
		{
			return;
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - classification_start)
		                         .count();
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
			h.range.vaddr  = cover_start;
			h.range.size   = cover_size;
			h.objects_map1 = new GpuMap1;
			h.objects_map2 = new GpuMap2;
			h.overlap_cache = new OverlapQueryCache;
			m_heaps.Add(h);
		}
		heap_id = GetHeapId(vaddr[0], size[0]);
	}

	if (heap_id < 0)
	{
		Vector<OverlappedBlock> no_parents;
		EXIT("%s\n", create_dbg_exit(U"unallocated gpu object range", vaddr, size, vaddr_num, no_parents, info.type).C_Str());
	}

	auto& heap = m_heaps[heap_id];

	GpuMemoryMaterializationKey materialization_key;
	if (buffer != nullptr)
	{
		SubmissionId submission;
		EXIT_NOT_IMPLEMENTED(!buffer->GetSubmissionId(&submission));
		materialization_key = GpuMemoryMaterializationKey::Create(
		    submit_id, submission.queue.Value(), submission.sequence, vaddr, size, vaddr_num,
		    static_cast<uint32_t>(info.type), info.params, GpuObject::PARAMS_MAX, info.check_hash, info.read_only);

		Materialization cached;
		if (m_materialization_cache.Lookup(materialization_key, &cached) && cached.heap_id == heap_id && cached.object_id >= 0 &&
		    static_cast<uint32_t>(cached.object_id) < heap.objects.Size())
		{
			auto& h = heap.objects[cached.object_id];
			auto& o = h.info;
			const bool same_callbacks =
			    cached.create_func == info.GetCreateFunc() &&
			    cached.create_from_objects_func == info.GetCreateFromObjectsFunc() &&
			    cached.write_back_func == info.GetWriteBackFunc() && cached.delete_func == info.GetDeleteFunc() &&
			    cached.update_func == info.GetUpdateFunc();
			if (!h.free && h.scenario == GpuMemoryScenario::Common && o.logical_generation == cached.logical_generation &&
			    o.object.type == info.type && o.submit_id == submit_id && o.in_use && o.check_hash == info.check_hash &&
			    info.Equal(o.params) && same_callbacks)
			{
				o.use_num++;
				o.use_last_frame = m_current_frame;
				o.read_only      = GpuMemoryMergeReadOnlyUse(o.in_use, o.read_only, info.read_only);
				o.in_use         = true;
				o.check_hash     = info.check_hash;
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
		m_materialization_cache.Store(
		    materialization_key,
		    Materialization {heap_id, object_id, o.logical_generation, info.GetCreateFunc(), info.GetCreateFromObjectsFunc(),
		                     info.GetWriteBackFunc(), info.GetDeleteFunc(), info.GetUpdateFunc()});
	};

	bool overlap             = false;
	bool delete_all          = false;
	bool create_from_objects = false;
	// VertexBuffer ids to free when a multi-parent Texture mixes reclaim with
	// surface-link parents (see multi_texture_mixed).
	Vector<int> texture_reclaim_vertex_ids;

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
			o.use_last_frame = m_current_frame;
			o.read_only      = GpuMemoryMergeReadOnlyUse(o.in_use, o.read_only, info.read_only);
			o.in_use         = true;
			o.check_hash     = info.check_hash;
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
		int existing_id = -1;
		bool covered_reuse = false;
		if (create_existing(others, info, heap_id, vaddr, size, vaddr_num, &existing_id, &covered_reuse))
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
			o.use_last_frame = m_current_frame;
			o.read_only      = GpuMemoryMergeReadOnlyUse(o.in_use, o.read_only, info.read_only);
			o.in_use         = true;
			o.check_hash     = info.check_hash;
			RecordUse(&o, buffer);

			void* const result = o.object.obj;
			cache_materialization(existing_id);
			ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
			create_stats.Complete(covered_reuse ? DebugStatsGpuMemoryCreateOutcome::CoveredReuse
			                                   : DebugStatsGpuMemoryCreateOutcome::ExactReuse);
			return result;
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
			} else if (GpuMemoryAllowsTextureStorageAlias(o.object.type, obj.relation, info.type))
			{
				overlap = true;
			} else if (GpuMemoryAllowsTextureContainedInSurface(o.object.type, obj.relation, info.type))
			{
				// Incoming Texture under a live RT/ST/SB/Texture surface: link and,
				// when the parent holds a Vulkan image, copy from it instead of
				// CPU-detiling empty GPU-owned guest memory.
				overlap = true;
				if (o.object.type == GpuMemoryObjectType::RenderTexture ||
				    o.object.type == GpuMemoryObjectType::StorageTexture)
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
			} else if (GpuMemoryAllowsStorageSurfaceShare(o.object.type, obj.relation, info.type))
			{
				// Single-parent RT/DS/Texture/SB share with an incoming StorageBuffer
				// (captured DepthStencilBuffer Crosses StorageBuffer 0x8000).
				overlap = true;
			} else
			switch (ObjectsRelation(o.object.type, obj.relation, info.type))
			{
				case ObjectsRelation(GpuMemoryObjectType::VideoOutBuffer, OverlapType::Equals, GpuMemoryObjectType::StorageBuffer):
				// Observed Gen5 alias: Texture 0x100 created at the base of an
				// active VertexBuffer 0x580 (relation Contains). Keep both views
				// linked rather than deleting the heavily-used vertex buffer.
				case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Contains, GpuMemoryObjectType::Texture):
				case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Equals, GpuMemoryObjectType::Texture):
				{
					overlap = true;
					break;
				}
				case ObjectsRelation(GpuMemoryObjectType::DepthStencilBuffer, OverlapType::Contains,
				                     GpuMemoryObjectType::DepthStencilBuffer):
				case ObjectsRelation(GpuMemoryObjectType::IndexBuffer, OverlapType::Crosses, GpuMemoryObjectType::IndexBuffer):
				case ObjectsRelation(GpuMemoryObjectType::IndexBuffer, OverlapType::Contains, GpuMemoryObjectType::IndexBuffer):
				case ObjectsRelation(GpuMemoryObjectType::IndexBuffer, OverlapType::IsContainedWithin, GpuMemoryObjectType::IndexBuffer):
				case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Crosses, GpuMemoryObjectType::VertexBuffer):
				case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Contains, GpuMemoryObjectType::VertexBuffer):
				case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::IsContainedWithin, GpuMemoryObjectType::VertexBuffer):
					// Gen5 alias observed when a storage view crosses an active vertex
					// allocation. Keep both resource views linked so the storage access
					// sees the same guest memory without reclaiming the vertex object.
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Crosses, GpuMemoryObjectType::StorageBuffer):
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::IsContainedWithin, GpuMemoryObjectType::StorageBuffer):
					// Existing VertexBuffer fully contains a new StorageBuffer view
					// (relation Contains). Reclaim the vertex object so the storage
					// view owns the range; multi-parent path links instead when a
					// Texture alias coexists.
					case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Contains, GpuMemoryObjectType::StorageBuffer):
					// Large Texture superseding VertexBuffers that live inside its
				// address range (observed 1 MiB texture over multiple VBs).
				case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::IsContainedWithin, GpuMemoryObjectType::Texture):
				case ObjectsRelation(GpuMemoryObjectType::VertexBuffer, OverlapType::Crosses, GpuMemoryObjectType::Texture):
				case ObjectsRelation(GpuMemoryObjectType::Texture, OverlapType::Crosses, GpuMemoryObjectType::Texture):
				case ObjectsRelation(GpuMemoryObjectType::Texture, OverlapType::Contains, GpuMemoryObjectType::Texture):
				case ObjectsRelation(GpuMemoryObjectType::Texture, OverlapType::IsContainedWithin, GpuMemoryObjectType::Texture):
				{
					delete_all = true;
					break;
				}
				case ObjectsRelation(GpuMemoryObjectType::StorageTexture, OverlapType::Equals, GpuMemoryObjectType::Texture):
				{
					overlap             = true;
					create_from_objects = true;
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
					     obj.relation != OverlapType::Contains))
					{
						multi_vertex_storage_alias = false;
						break;
					}
				}
			}

			// Multi-parent StorageBuffer where every parent is either an observed
			// Texture↔Storage alias, VertexBuffer storage-share, or surface share
			// (including IsContainedWithin). create_all_the_same rejects mixed
			// parent types; this policy links them.
			bool multi_mixed_storage_alias = (info.type == GpuMemoryObjectType::StorageBuffer && !others.IsEmpty());
			if (multi_mixed_storage_alias)
			{
				for (const auto& obj: others)
				{
					const auto& h = heap.objects[obj.object_id];
					EXIT_IF(h.free);
					const auto& o = h.info;
					const bool texture_alias =
					    GpuMemoryAllowsTextureStorageAlias(o.object.type, obj.relation, info.type);
					const bool vertex_share = GpuMemoryAllowsVertexStorageShare(o.object.type, obj.relation, info.type);
					const bool surface_share =
					    GpuMemoryAllowsStorageSurfaceShare(o.object.type, obj.relation, info.type);
					if (!texture_alias && !vertex_share && !surface_share)
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
			bool multi_vertex_mixed = (info.type == GpuMemoryObjectType::VertexBuffer && !others.IsEmpty());
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
			bool multi_index_mixed = (info.type == GpuMemoryObjectType::IndexBuffer && !others.IsEmpty());
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
					const bool surface = o.object.type == GpuMemoryObjectType::Texture ||
					                     o.object.type == GpuMemoryObjectType::StorageBuffer ||
					                     o.object.type == GpuMemoryObjectType::StorageTexture ||
					                     o.object.type == GpuMemoryObjectType::RenderTexture ||
					                     o.object.type == GpuMemoryObjectType::VertexBuffer;
					const bool rel_ok  = obj.relation == OverlapType::Crosses || obj.relation == OverlapType::Contains ||
					                    obj.relation == OverlapType::IsContainedWithin || obj.relation == OverlapType::Equals;
					if (!surface || !rel_ok)
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
						texture_reclaim_vertex_ids.Add(obj.object_id);
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
					}
				}
			}

			if (multi_ro_storage_share || multi_vertex_storage_alias || multi_mixed_storage_alias || multi_vertex_in_surface ||
			    multi_render_target_alias)
			{
				overlap = true;
			} else if (multi_texture_reclaim || multi_depth_stencil_reclaim)
			{
				delete_all = true;
			} else if (multi_texture_mixed)
			{
				// Drop reclaimed VBs from the link set; free them after create.
				if (texture_reclaim_vertex_ids.Size() == others.Size())
				{
					delete_all = true;
				} else
				{
					Vector<OverlappedBlock> keep;
					for (const auto& obj: others)
					{
						bool reclaim = false;
						for (int id: texture_reclaim_vertex_ids)
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
					// Surface parents hold live GPU images; copy from them instead
					// of tile-27-uploading empty CPU guest memory (opaque-black
					// props when the sample range was GPU-written).
					for (const auto& obj: others)
					{
						const auto parent_type = heap.objects[obj.object_id].info.object.type;
						if (parent_type == GpuMemoryObjectType::RenderTexture ||
						    parent_type == GpuMemoryObjectType::StorageTexture)
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
					// Reuse texture_reclaim_vertex_ids free path after create.
					texture_reclaim_vertex_ids = vertex_reclaim_vertex_ids;
					others                     = keep;
					overlap                    = true;
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
					texture_reclaim_vertex_ids = index_reclaim_index_ids;
					others                     = keep;
					overlap                    = true;
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
					std::fprintf(stderr, "GpuMemory !create_all_the_same: new type=%s parents=%u\n",
					             Core::EnumName(info.type).C_Str(), static_cast<unsigned>(others.Size()));
					for (int vi = 0; vi < vaddr_num; vi++)
					{
						std::fprintf(stderr, "  new range[%d]: vaddr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vi, vaddr[vi], size[vi]);
					}
					for (const auto& d: others)
					{
						const auto& oh = heap.objects[d.object_id];
						const auto& oi = oh.info;
						std::fprintf(stderr,
						             "  parent id=%d type=%s rel=%s read_only=%d vaddr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
						             d.object_id, Core::EnumName(oi.object.type).C_Str(), Core::EnumName(d.relation).C_Str(),
						             oi.read_only ? 1 : 0, oh.block.vaddr[0], oh.block.size[0]);
					}
					std::fflush(stderr);
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

	const bool reclaimed_existing = delete_all || !texture_reclaim_vertex_ids.IsEmpty();
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
	} else if (!texture_reclaim_vertex_ids.IsEmpty())
	{
		// Selective free from multi_texture_mixed (VertexBuffers only).
		for (int id: texture_reclaim_vertex_ids)
		{
			RequireDetachable(ctx, heap_id, id, &destructors, "create_selective_reclaim", info.type);
		}
		for (int id: texture_reclaim_vertex_ids)
		{
			destructors.Add(Free(heap_id, id));
		}
	}

	for (int vi = 0; vi < vaddr_num; vi++)
	{
		EXIT_NOT_IMPLEMENTED(!IsAllocated(vaddr[vi], size[vi]));
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
	    delete_all ? others.Size() : static_cast<uint32_t>(texture_reclaim_vertex_ids.Size());
	finish_classification(others.Size(), relation_mask, reclaimed_count, create_from_objects);

	ObjectInfo o {};

	for (int i = 0; i < GpuObject::PARAMS_MAX; i++)
	{
		o.params[i] = info.params[i];
	}

	uint64_t hash[VADDR_BLOCKS_MAX] = {};

	for (int vi = 0; vi < vaddr_num; vi++)
	{
		EXIT_IF(size[vi] == 0);

		if (info.check_hash)
		{
			hash[vi] = calc_hash(info.type, reinterpret_cast<const uint8_t*>(vaddr[vi]), size[vi]);
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
	o.cpu_update_time = get_current_time();
	o.gpu_update_time = o.cpu_update_time;
	o.submit_id       = submit_id;
	o.create_func     = info.GetCreateFunc();

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
		o.object.obj = create_func(ctx, buffer, o.params, scenario, objects, &o.mem);
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
				const auto update_ns =
				    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - update_start).count();
				create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::UpdateFunc, static_cast<uint64_t>(update_ns));
			}
		}
	} else
	{
		EXIT_IF(o.create_func == nullptr);
		const auto create_start = std::chrono::steady_clock::now();
		o.object.obj = o.create_func(ctx, o.params, vaddr, size, vaddr_num, &o.mem);
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
				break;
			}
		}
	}

	o.write_back_func = info.GetWriteBackFunc();
	o.delete_func     = info.GetDeleteFunc();
	o.update_func     = info.GetUpdateFunc();
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
		u.free             = false;
		u.block            = CreateBlock(vaddr, size, vaddr_num, heap_id, index);
		u.info             = o;
		u.info.logical_generation = logical_generation;
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

		// Evidence: multi-parent alias graphs break WriteBack's single-Equals
		// parent contract. Log the producer link set when a write-back-capable
		// object ends up with more than one parent. Always go to stderr so Silent
		// PrintfDirection runs still capture this evidence.
		const auto& created = heap.objects[index];
		if (created.info.write_back_func != nullptr && !created.info.read_only && created.others.Size() > 1)
		{
			const auto type_name = Core::EnumName(created.info.object.type);
			std::fprintf(stderr, "GpuMemory CreateObject multi-parent write-back link:\n");
			std::fprintf(stderr, "\t new: heap=%d id=%d type=%s read_only=%s others=%u\n", heap_id, index, type_name.C_Str(),
			             (created.info.read_only ? "true" : "false"), static_cast<unsigned>(created.others.Size()));
			for (int vi = 0; vi < created.block.vaddr_num; vi++)
			{
				std::fprintf(stderr, "\t new range[%d]: vaddr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vi, created.block.vaddr[vi],
				             created.block.size[vi]);
			}
			for (uint32_t oi = 0; oi < created.others.Size(); oi++)
			{
				const auto& other  = created.others.At(static_cast<int>(oi));
				const auto& parent = heap.objects[other.object_id];
				std::fprintf(stderr, "\t parent[%u]: id=%d type=%s relation=%s read_only=%s\n", oi, other.object_id,
				             Core::EnumName(parent.info.object.type).C_Str(), Core::EnumName(other.relation).C_Str(),
				             (parent.info.read_only ? "true" : "false"));
				if (!parent.free)
				{
					for (int vi = 0; vi < parent.block.vaddr_num; vi++)
					{
						std::fprintf(stderr, "\t parent[%u] range[%d]: vaddr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", oi, vi,
						             parent.block.vaddr[vi], parent.block.size[vi]);
					}
				}
			}
			std::fflush(stderr);
		}
	}

	if (info.check_hash)
	{
		const auto dirty_track_start = std::chrono::steady_clock::now();
		auto& created  = heap.objects[index];
		bool  tracked  = GpuDirtyPageTracker::Instance().Enabled();
		bool  attempted = false;
		const auto dirty_register_start = std::chrono::steady_clock::now();
		for (int vi = 0; tracked && vi < created.block.vaddr_num; vi++)
		{
			attempted = true;
			tracked = GpuDirtyPageTracker::Instance().RegisterRange(created.block.vaddr[vi], created.block.size[vi]);
		}
		const auto dirty_register_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dirty_register_start).count();
		create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::DirtyRegister, static_cast<uint64_t>(dirty_register_ns));
		const auto dirty_prepare_start = std::chrono::steady_clock::now();
		for (int vi = 0; tracked && vi < created.block.vaddr_num; vi++)
		{
			tracked = GpuDirtyPageTracker::Instance().PrepareForRead(created.block.vaddr[vi], created.block.size[vi]);
		}
		const auto dirty_prepare_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dirty_prepare_start).count();
		create_stats.AddPhase(DebugStatsGpuMemoryCreatePhase::DirtyPrepare, static_cast<uint64_t>(dirty_prepare_ns));
		if (tracked)
		{
			created.info.dirty_registered = true;
			for (int vi = 0; vi < created.block.vaddr_num; vi++)
			{
				created.info.dirty_generation[vi] =
				    GpuDirtyPageTracker::Instance().SnapshotGeneration(created.block.vaddr[vi], created.block.size[vi]);
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

	if (reclaimed_existing)
	{
		create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::ReclaimNew);
	} else if (create_from_objects)
	{
		create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::NewFromObjects);
	} else if (overlap)
	{
		create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::NewLinked);
	} else
	{
		create_stats.Complete(DebugStatsGpuMemoryCreateOutcome::NewStandalone);
	}
	return o.object.obj;
}

Vector<GpuMemoryObject> GpuMemory::FindObjects(const uint64_t* vaddr, const uint64_t* size, int vaddr_num, GpuMemoryObjectType type,
                                               bool exact, bool only_first, const SubmissionId* submission)
{
	KYTY_PROFILER_BLOCK("GpuMemory::FindObjects", profiler::colors::Green200);

	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num > VADDR_BLOCKS_MAX || vaddr_num <= 0);

	Core::LockGuard lock(m_mutex);

	Vector<GpuMemoryObject> ret;

	int heap_id = GetHeapId(vaddr[0], size[0]);

	EXIT_NOT_IMPLEMENTED(heap_id < 0);

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
	{
		return a <= b ? b - a < a_size : a - b < b_size;
	};
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
				entry           = &out->entries[out->entry_count++];
				entry->type     = stored.info.object.type;
				entry->relation = object.relation;
				entry->exact    = object.relation == GpuMemoryOverlapType::Equals;
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

	int heap_id = GetHeapId(vaddr[0], size[0]);

	EXIT_NOT_IMPLEMENTED(heap_id < 0);

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
				printf("ResetHash: type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 ", old_hash = 0x%016" PRIx64
				       ", new_hash = 0x%016" PRIx64 "\n",
				       Core::EnumName(o.object.type).C_Str(), vaddr[vi], size[vi], o.hash[vi], new_hash);
			}
			o.gpu_update_time = get_current_time();

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
				EXIT_NOT_IMPLEMENTED(obj.relation != OverlapType::Equals);

				for (int vi = 0; vi < vaddr_num; vi++)
				{
					printf("ResetHash: type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 ", old_hash = 0x%016" PRIx64
					       ", new_hash = 0x%016" PRIx64 "\n",
					       Core::EnumName(o.object.type).C_Str(), vaddr[vi], size[vi], o.hash[vi], new_hash);
				}
				o.gpu_update_time = get_current_time();
			}
		}
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
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

	printf("Release gpu objects:\n");
	printf("\t gpu_vaddr = 0x%016" PRIx64 "\n", vaddr);
	printf("\t size   = 0x%016" PRIx64 "\n", size);

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
			case OverlapType::Crosses:
				RequireDetachable(ctx, heap_id, obj.object_id, &destructors, "range_free");
				break;
			default: GpuMemoryDbgDump(); EXIT("unknown obj.relation: %s\n", Core::EnumName(obj.relation).C_Str());
		}
	}
	for (const auto& obj: object_ids)
	{
		destructors.Add(Free(heap_id, obj.object_id));
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

void GpuMemory::RequireDetachable(GraphicContext* ctx, int heap_id, int object_id, Vector<Destructor>* destructors,
                                  const char* operation, GpuMemoryObjectType incoming_type)
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

		std::fprintf(stderr,
		             "GpuMemory detach blocked: operation=%s incoming=%s type=%s heap=%d id=%d generation=%" PRIu64
		             " ranges=%d dependencies=%zu\n",
		             operation, Core::EnumName(incoming_type).C_Str(), Core::EnumName(object.object.type).C_Str(), heap_id, object_id,
		             object.backing_generation, h.block.vaddr_num, dependencies.size());
		for (int vi = 0; vi < h.block.vaddr_num; vi++)
		{
			std::fprintf(stderr, "  range[%d]=0x%016" PRIx64 "+0x%016" PRIx64 "\n", vi, h.block.vaddr[vi], h.block.size[vi]);
		}
		for (const auto& dependency: dependencies)
		{
			std::fprintf(stderr, "  dependency queue=%" PRIu32 " sequence=%" PRIu64 " complete=%d\n", dependency.queue.Value(),
			             dependency.sequence, m_deferred_deletions.AreDependenciesComplete({dependency}) ? 1 : 0);
		}
		std::fflush(stderr);
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
				printf("Delete: type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n", Core::EnumName(o.object.type).C_Str(),
				       block.vaddr[vi], block.size[vi]);
			}
		}
		// o.delete_func(ctx, o.object.obj, &o.mem);
		ret.delete_func = o.delete_func;
		ret.obj         = o.object.obj;
		ret.type            = o.object.type;
		ret.submission_uses = o.submission_uses;
		ret.mem             = o.mem;
	}

	// Drop bidirectional alias links before recycling the slot. Multi-parent
	// reclaim (VB/Texture delete_all) otherwise leaves free object_ids in peer
	// others lists; WriteBack then EXIT_IF(parent.free) on those dangling links
	// (captured dual-strict after GPU-owned RT WriteBack skip).
	for (const auto& other: h.others)
	{
		auto& peer = heap.objects[other.object_id];
		if (peer.free)
		{
			continue;
		}
		Vector<OverlappedBlock> keep;
		for (const auto& e: peer.others)
		{
			if (e.object_id != object_id)
			{
				keep.Add(e);
			}
		}
		peer.others = keep;
	}
	h.others.Clear();

	h.free             = true;
	h.next_free_id     = heap.first_free_id;
	heap.first_free_id = object_id;
	DeleteBlock(&h.block, heap_id, object_id);

	return ret;
}

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
			if (b.info.object.type == type)
			{
				bool equal = true;
				if (b.block.vaddr_num == 1 || only_first)
				{
					if (GetOverlapType(b.block.vaddr[0], b.block.size[0], vaddr[0], size[0]) != OverlapType::Equals)
					{
						equal = false;
					}
				} else
				{
					for (int i = 0; i < vaddr_num; i++)
					{
						if (GetOverlapType(b.block.vaddr[i], b.block.size[i], vaddr[i], size[i]) != OverlapType::Equals)
						{
							equal = false;
							break;
						}
					}
				}
				if (equal)
				{
					*id = obj_id;
					return true;
				}
			}
		}
	}

	return false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Vector<GpuMemory::OverlappedBlock> GpuMemory::FindBlocks_slow(int heap_id, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                                                              bool only_first)
{
	KYTY_PROFILER_BLOCK("GpuMemory::FindBlocks", profiler::colors::Green100);

	auto& heap = m_heaps[heap_id];

	EXIT_IF(vaddr_num <= 0 || vaddr_num > VADDR_BLOCKS_MAX);
	EXIT_IF(vaddr == nullptr || size == nullptr);
	EXIT_IF(only_first && vaddr_num != 1);

	Vector<GpuMemory::OverlappedBlock> ret;

	// TODO(): implement interval-tree

	if (vaddr_num != 1)
	{
		int index = 0;
		for (const auto& b: heap.objects)
		{
			if (!b.free)
			{
				bool equal = b.block.vaddr_num == vaddr_num;
				for (int i = 0; equal && i < vaddr_num; i++)
				{
					if (GetOverlapType(b.block.vaddr[i], b.block.size[i], vaddr[i], size[i]) != OverlapType::Equals)
					{
						equal = false;
						break;
					}
				}
				if (equal)
				{
					ret.Add({OverlapType::Equals, index});
				} else
				{
					bool cross = false;
					for (int i = 0; i < vaddr_num; i++)
					{
						for (int j = 0; j < b.block.vaddr_num; j++)
						{
							if (GetOverlapType(b.block.vaddr[j], b.block.size[j], vaddr[i], size[i]) != OverlapType::None)
							{
								cross = true;
								break;
							}
						}
						if (cross)
						{
							break;
						}
					}
					if (cross)
					{
						ret.Add({OverlapType::Crosses, index});
					}
				}
			}
			index++;
		}
	} else
	{
		int index = 0;
		for (const auto& b: heap.objects)
		{
			if (!b.free)
			{
				if (b.block.vaddr_num == 1 || only_first)
				{
					auto type = GetOverlapType(b.block.vaddr[0], b.block.size[0], vaddr[0], size[0]);
					if (type != OverlapType::None)
					{
						ret.Add({type, index});
					}
				} else
				{
					for (int i = 0; i < b.block.vaddr_num; i++)
					{
						if (GetOverlapType(b.block.vaddr[i], b.block.size[i], vaddr[0], size[0]) != OverlapType::None)
						{
							ret.Add({OverlapType::Crosses, index});
							break;
						}
					}
				}
			}
			index++;
		}
	}

	//	printf("FindBlocks:\n");
	//	for (int vi = 0; vi < vaddr_num; vi++)
	//	{
	//		printf("\t vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n", vaddr[vi], size[vi]);
	//	}
	//	for (const auto& d: ret)
	//	{
	//		printf("\t id = %d, rel = %s\n", d.object_id, Core::EnumName(d.relation).C_Str());
	//		const auto& b = m_objects[d.object_id];
	//		for (int vi = 0; vi < b.block.vaddr_num; vi++)
	//		{
	//			printf("\t\t vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n", b.block.vaddr[vi], b.block.size[vi]);
	//		}
	//	}

	return ret;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
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
	const auto query = GpuMemoryRangeQueryKey::Create(vaddr, size, vaddr_num, only_first);
	EXIT_IF(!query.Valid());
	if (heap.overlap_cache->Lookup(query, &ret))
	{
		return ret;
	}

	// TODO(): implement interval-tree

	if (vaddr_num != 1)
	{
		for (int index: heap.objects_map2->FindAll(vaddr, size, vaddr_num))
		{
			const auto& b = heap.objects[index];
			if (!b.free)
			{
				bool equal = b.block.vaddr_num == vaddr_num;
				for (int i = 0; equal && i < vaddr_num; i++)
				{
					if (GetOverlapType(b.block.vaddr[i], b.block.size[i], vaddr[i], size[i]) != OverlapType::Equals)
					{
						equal = false;
						break;
					}
				}
				if (equal)
				{
					ret.Add({OverlapType::Equals, index});
				} else
				{
					bool cross = false;
					for (int i = 0; i < vaddr_num; i++)
					{
						for (int j = 0; j < b.block.vaddr_num; j++)
						{
							if (GetOverlapType(b.block.vaddr[j], b.block.size[j], vaddr[i], size[i]) != OverlapType::None)
							{
								cross = true;
								break;
							}
						}
						if (cross)
						{
							break;
						}
					}
					if (cross)
					{
						ret.Add({OverlapType::Crosses, index});
					}
				}
			}
		}
	} else
	{
		for (int index: heap.objects_map2->FindAll(vaddr[0], size[0]))
		{
			const auto& b = heap.objects[index];
			if (!b.free)
			{
				if (b.block.vaddr_num == 1 || only_first)
				{
					auto type = GetOverlapType(b.block.vaddr[0], b.block.size[0], vaddr[0], size[0]);
					if (type != OverlapType::None)
					{
						ret.Add({type, index});
					}
				} else
				{
					for (int i = 0; i < b.block.vaddr_num; i++)
					{
						if (GetOverlapType(b.block.vaddr[i], b.block.size[i], vaddr[0], size[0]) != OverlapType::None)
						{
							ret.Add({OverlapType::Crosses, index});
							break;
						}
					}
				}
			}
		}
	}

	{
		KYTY_PROFILER_BLOCK("sort");
		ret.Sort([](auto& b1, auto& b2) { return b1.object_id < b2.object_id; });
	}
	heap.overlap_cache->Store(query, ret);

	//
	//	printf("FindBlocks:\n");
	//	for (int vi = 0; vi < vaddr_num; vi++)
	//	{
	//		printf("\t vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n", vaddr[vi], size[vi]);
	//	}
	//	for (const auto& d: ret)
	//	{
	//		printf("\t id = %d, rel = %s\n", d.object_id, Core::EnumName(d.relation).C_Str());
	//		const auto& b = m_objects[d.object_id];
	//		for (int vi = 0; vi < b.block.vaddr_num; vi++)
	//		{
	//			printf("\t\t vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64 "\n", b.block.vaddr[vi], b.block.size[vi]);
	//		}
	//	}

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

void GpuMemory::FrameDone()
{
	Core::LockGuard lock(m_mutex);

	m_current_frame++;
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
	// full hash propagation.
	constexpr uint32_t kMaxWriteBackParents = 64;
	EXIT_NOT_IMPLEMENTED(h.others.Size() > kMaxWriteBackParents);
	GpuMemoryOverlapType parent_rels[kMaxWriteBackParents] {};
	for (uint32_t oi = 0; oi < h.others.Size(); oi++)
	{
		parent_rels[oi] = h.others.At(static_cast<int>(oi)).relation;
	}
	bool     recompute_self   = true;
	uint32_t equals_count     = 0;
	uint32_t invalidate_count = 0;
	if (!GpuMemoryWriteBackClassifyParents(parent_rels, static_cast<uint32_t>(h.others.Size()), &recompute_self, &equals_count,
	                                       &invalidate_count))
	{
		std::fprintf(stderr, "GpuMemory WriteBack unsupported parent relation in alias topology:\n");
		std::fprintf(stderr, "\t self: heap=%d id=%d type=%s others=%u\n", heap_id, object_id,
		             Core::EnumName(o.object.type).C_Str(), static_cast<unsigned>(h.others.Size()));
		for (uint32_t oi = 0; oi < h.others.Size(); oi++)
		{
			const auto& other = h.others.At(static_cast<int>(oi));
			std::fprintf(stderr, "\t other[%u]: id=%d relation=%s type=%s\n", oi, other.object_id,
			             Core::EnumName(other.relation).C_Str(),
			             Core::EnumName(heap.objects[other.object_id].info.object.type).C_Str());
		}
		std::fflush(stderr);
		EXIT("WriteBack unsupported parent relation\n");
	}

	GpuWritebackResult writeback_result;
	{
		const auto writeback_start = std::chrono::steady_clock::now();
		writeback_result = o.write_back_func(ctx, o.params, o.object.obj, block.vaddr, block.size, block.vaddr_num);
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
	o.cpu_update_time = get_current_time();

	// Invalidate or propagate each parent according to its relation.
	// GPU-owned tiled RTs cannot be reconstructed from guest bytes.
	for (uint32_t oi = 0; oi < h.others.Size(); oi++)
	{
		const auto& other = h.others.At(static_cast<int>(oi));
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
				new_hash = calc_hash(o.object.type, reinterpret_cast<const uint8_t*>(block.vaddr[vi]), block.size[vi]);
			}
			printf("WriteBack (GPU -> CPU): type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64
			       ", old_hash = 0x%016" PRIx64 ", new_hash = 0x%016" PRIx64 ", equals=%u invalidate=%u\n",
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
				printf("WriteBack (GPU -> CPU): type = %s, vaddr = 0x%016" PRIx64 ", size = 0x%016" PRIx64
				       ", old_hash = 0x%016" PRIx64 ", new_hash = 0x%016" PRIx64 ", equals=%u invalidate=%u\n",
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
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
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
							EXIT("GpuMemory write-back crossed a later same-queue use: type=%s completing=%" PRIu64
							     " latest=%" PRIu64 " queue=%" PRIu32 "\n",
							     Core::EnumName(o.object.type).C_Str(), submission.sequence, queue_use.sequence,
							     submission.queue.Value());
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
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
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
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
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

	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
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
	Core::LockGuard backing_lock(m_backing_mutation_mutex);
	Core::LockGuard lock(m_mutex);
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

void GpuMemory::DbgInit()
{
	EXIT_IF(!m_db.IsInvalid());
	[[maybe_unused]] bool create = m_db.CreateInMemory();
	EXIT_IF(!create);
	if (!m_db.IsInvalid())
	{
		m_db.Exec(R"(
CREATE TABLE [objects](
  [dump_id] INT, 
  [heap_id] INTEGER, 
  [id] INTEGER, 
  [vaddr] TEXT, 
  [size] TEXT, 
  [vaddr2] TEXT, 
  [size2] TEXT, 
  [vaddr3] TEXT, 
  [size3] TEXT, 
  [obj] TEXT, 
  [type] TEXT, 
  [param0] INT64, 
  [param1] INT64, 
  [param2] INT64, 
  [param3] INT64, 
  [param4] INT64, 
  [param5] INT64, 
  [param6] INT64, 
  [param7] INT64, 
  [scenario] TEXT, 
  [others] TEXT, 
  [hash] TEXT, 
  [hash2] TEXT, 
  [hash3] TEXT, 
  [gpu_update_time] INT64, 
  [cpu_update_time] INT64, 
  [submit_id] INT64, 
  [write_back_func] TEXT, 
  [delete_func] TEXT, 
  [update_func] TEXT, 
  [use_last_frame] INT64, 
  [use_num] INT64, 
  [in_use] BOOL, 
  [read_only] BOOL, 
  [check_hash] BOOL, 
  [vk_mem_size] TEXT, 
  [vk_mem_alignment] TEXT, 
  [vk_mem_memoryTypeBits] INT, 
  [vk_mem_property] INT, 
  [vk_mem_memory] TEXT, 
  [vk_mem_offset] INT64, 
  [vk_mem_type] INT, 
  [vk_mem_unique_id] INT64, 
  PRIMARY KEY([heap_id], [id]), 
  UNIQUE([heap_id], [id])) WITHOUT ROWID;

CREATE TABLE [ranges](
  [dump_id] INT, 
  [vaddr] TEXT, 
  [size] TEXT);
)");

		m_db_add_range  = m_db.Prepare("insert into ranges(dump_id, vaddr, size) values(:dump_id, :vaddr, :size)");
		m_db_add_object = m_db.Prepare(
		    "insert into objects(dump_id, heap_id, id, vaddr, vaddr2, vaddr3, size, size2, size3, obj, param0, param1, param2, param3, "
		    "param4, "
		    "param5, param6, param7, type, hash, hash2, "
		    "hash3, gpu_update_time, cpu_update_time, submit_id, scenario, others, write_back_func, delete_func, update_func, "
		    "use_last_frame, "
		    "use_num, in_use, read_only, "
		    "check_hash, vk_mem_size, "
		    "vk_mem_alignment, vk_mem_memoryTypeBits, vk_mem_property, vk_mem_memory, vk_mem_offset, vk_mem_type, vk_mem_unique_id) "
		    "values(:dump_id, :heap_id, :id, :vaddr, :vaddr2, :vaddr3, :size, :size2, :size3, :obj, :param0, :param1, :param2, :param3, "
		    ":param4, "
		    ":param5, "
		    ":param6, :param7, :type, :hash, "
		    ":hash2, :hash3, :gpu_update_time, :cpu_update_time, :submit_id, :scenario, :others, :write_back_func, :delete_func, "
		    ":update_func, "
		    ":use_last_frame, :use_num, :in_use, "
		    ":read_only, :check_hash, "
		    ":vk_mem_size, :vk_mem_alignment, :vk_mem_memoryTypeBits, :vk_mem_property, :vk_mem_memory, :vk_mem_offset, :vk_mem_type, "
		    ":vk_mem_unique_id)");
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void GpuMemory::DbgDbDump()
{
	KYTY_PROFILER_FUNCTION();

	Core::LockGuard lock(m_mutex);

	static int dump_id = 0;

	auto hex = [](auto u)
	{
		auto u64 = reinterpret_cast<uint64_t>(u);
		return (u64 == 0 ? U"0" : String::FromPrintf("0x%010" PRIx64, u64));
	};

	auto id = [](int id1, int id2) { return id1 * 1000000 + id2; };

	if (!m_db.IsInvalid())
	{
		m_db.Exec("BEGIN TRANSACTION");

		m_db.Exec("delete from ranges");

		int heap_id = 0;
		for (const auto& heap: m_heaps)
		{
			m_db_add_range->Reset();
			m_db_add_range->BindInt(":dump_id", dump_id);
			m_db_add_range->BindString(":vaddr", hex(heap.range.vaddr));
			m_db_add_range->BindString(":size", hex(heap.range.size));
			m_db_add_range->Step();

			int index = 0;
			for (const auto& r: heap.objects)
			{
				if (!r.free)
				{
					m_db_add_object->Reset();
					m_db_add_object->BindInt(":dump_id", dump_id);
					m_db_add_object->BindInt(":heap_id", heap_id);
					m_db_add_object->BindInt(":id", id(dump_id, index));
					(r.block.vaddr_num > 0 ? m_db_add_object->BindString(":vaddr", hex(r.block.vaddr[0]))
					                       : m_db_add_object->BindNull(":vaddr"));
					(r.block.vaddr_num > 1 ? m_db_add_object->BindString(":vaddr2", hex(r.block.vaddr[1]))
					                       : m_db_add_object->BindNull(":vaddr2"));
					(r.block.vaddr_num > 2 ? m_db_add_object->BindString(":vaddr3", hex(r.block.vaddr[2]))
					                       : m_db_add_object->BindNull(":vaddr3"));
					(r.block.vaddr_num > 0 ? m_db_add_object->BindString(":size", hex(r.block.size[0]))
					                       : m_db_add_object->BindNull(":size"));
					(r.block.vaddr_num > 1 ? m_db_add_object->BindString(":size2", hex(r.block.size[1]))
					                       : m_db_add_object->BindNull(":size2"));
					(r.block.vaddr_num > 2 ? m_db_add_object->BindString(":size3", hex(r.block.size[2]))
					                       : m_db_add_object->BindNull(":size3"));
					m_db_add_object->BindString(":obj", hex(r.info.object.obj));
					int param0_index = m_db_add_object->GetIndex(":param0");
					for (int i = 0; i < GpuObject::PARAMS_MAX; i++)
					{
						m_db_add_object->BindInt64(param0_index + i, static_cast<int64_t>(r.info.params[i]));
					}
					m_db_add_object->BindString(":type", Core::EnumName(r.info.object.type).C_Str());
					int hash0_index = m_db_add_object->GetIndex(":hash");
					for (int i = 0; i < VADDR_BLOCKS_MAX; i++)
					{
						m_db_add_object->BindString(hash0_index + i, hex(r.info.hash[i]));
					}
					m_db_add_object->BindString(":write_back_func", hex(r.info.write_back_func));
					m_db_add_object->BindString(":delete_func", hex(r.info.delete_func));
					m_db_add_object->BindString(":update_func", hex(r.info.update_func));
					m_db_add_object->BindInt64(":use_last_frame", static_cast<int64_t>(r.info.use_last_frame));
					m_db_add_object->BindInt64(":use_num", static_cast<int64_t>(r.info.use_num));
					m_db_add_object->BindInt(":in_use", static_cast<int>(r.info.in_use));
					m_db_add_object->BindInt(":read_only", static_cast<int>(r.info.read_only));
					m_db_add_object->BindInt(":check_hash", static_cast<int>(r.info.check_hash));
					m_db_add_object->BindString(":vk_mem_size", hex(r.info.mem.requirements.size));
					m_db_add_object->BindString(":vk_mem_alignment", hex(r.info.mem.requirements.alignment));
					m_db_add_object->BindInt(":vk_mem_memoryTypeBits", static_cast<int>(r.info.mem.requirements.memoryTypeBits));
					m_db_add_object->BindInt(":vk_mem_property", static_cast<int>(r.info.mem.property));
					m_db_add_object->BindString(":vk_mem_memory", hex(r.info.mem.memory));
					m_db_add_object->BindInt64(":vk_mem_offset", static_cast<int64_t>(r.info.mem.offset));
					m_db_add_object->BindInt(":vk_mem_type", static_cast<int>(r.info.mem.type));
					m_db_add_object->BindInt64(":vk_mem_unique_id", static_cast<int64_t>(r.info.mem.unique_id));
					m_db_add_object->BindString(":scenario", Core::EnumName(r.scenario).C_Str());
					m_db_add_object->BindInt64(":gpu_update_time", static_cast<int64_t>(r.info.gpu_update_time));
					m_db_add_object->BindInt64(":cpu_update_time", static_cast<int64_t>(r.info.cpu_update_time));
					m_db_add_object->BindInt64(":submit_id", static_cast<int64_t>(r.info.submit_id));

					if (r.others.Size() > 0)
					{
						Core::StringList others;
						for (const auto& s: r.others)
						{
							others.Add(String::FromPrintf("[%s,%d]", Core::EnumName(s.relation).C_Str(), id(dump_id, s.object_id)));
						}
						m_db_add_object->BindString(":others", others.Concat(U','));
					} else
					{
						m_db_add_object->BindNull(":others");
					}

					m_db_add_object->Step();
				}
				index++;
			}
			heap_id++;
		}

		m_db.Exec("END TRANSACTION");
	}

	dump_id++;
}

void GpuMemory::DbgDbSave(const String& file_name)
{
	KYTY_PROFILER_FUNCTION();

	Core::LockGuard lock(m_mutex);

	if (!m_db.IsInvalid())
	{
		Core::Database::Connection db;
		if (!db.Create(file_name) && !db.Open(file_name, Core::Database::Connection::Mode::ReadWrite))
		{
			printf("Can't open file: %s\n", file_name.C_Str());
			return;
		}
		m_db.CopyTo(&db);
		db.Close();
	}
}

struct VulkanMemoryStat
{
	std::atomic_uint64_t allocated[VK_MAX_MEMORY_TYPES];
	std::atomic_uint64_t count[VK_MAX_MEMORY_TYPES];
};

static VulkanMemoryStat* g_mem_stat = nullptr;

void GpuMemoryInit()
{
	EXIT_IF(g_gpu_memory != nullptr);
	EXIT_IF(g_gpu_resources != nullptr);

	g_gpu_memory    = new GpuMemory;
	g_gpu_resources = new GpuResources;

	g_mem_stat = new VulkanMemoryStat;

	for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; i++)
	{
		g_mem_stat->allocated[i] = 0;
		g_mem_stat->count[i]     = 0;
	}
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

Vector<GpuMemoryObject> GpuMemoryFindObjectsForSubmission(SubmissionId submission, uint64_t vaddr, uint64_t size,
                                                          GpuMemoryObjectType type, bool exact, bool only_first)
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

Vector<GpuMemoryObject> GpuMemoryFindObjectsForSubmission(CommandBuffer* buffer, uint64_t vaddr, uint64_t size,
                                                          GpuMemoryObjectType type, bool exact, bool only_first)
{
	EXIT_IF(buffer == nullptr);
	SubmissionId submission;
	EXIT_NOT_IMPLEMENTED(!buffer->GetSubmissionId(&submission));
	return GpuMemoryFindObjectsForSubmission(submission, vaddr, size, type, exact, only_first);
}

Vector<GpuMemoryObject> GpuMemoryFindObjectsForSubmission(CommandBuffer* buffer, const uint64_t* vaddr, const uint64_t* size,
                                                          int vaddr_num, GpuMemoryObjectType type, bool exact, bool only_first)
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

	g_gpu_memory->FrameDone();
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

bool VulkanAllocate(GraphicContext* ctx, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	static std::atomic_uint64_t seq = 0;

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(mem->memory != nullptr);
	EXIT_IF(mem->requirements.size == 0);

	VkPhysicalDeviceMemoryProperties memory_properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &memory_properties);

	uint32_t index = 0;
	for (; index < memory_properties.memoryTypeCount; index++)
	{
		if ((mem->requirements.memoryTypeBits & (static_cast<uint32_t>(1) << index)) != 0 &&
		    (memory_properties.memoryTypes[index].propertyFlags & mem->property) == mem->property)
		{
			break;
		}
	}

	mem->type   = index;
	mem->offset = 0;

	VkMemoryAllocateInfo alloc_info {};
	alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext           = nullptr;
	alloc_info.allocationSize  = mem->requirements.size;
	alloc_info.memoryTypeIndex = index;

	mem->unique_id = ++seq;

	const auto allocate_start = std::chrono::steady_clock::now();
	auto result = vkAllocateMemory(ctx->device, &alloc_info, nullptr, &mem->memory);
	const auto allocate_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - allocate_start).count();
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::VulkanAllocate,
	                                               static_cast<uint64_t>(allocate_ns), mem->requirements.size);

	if (result == VK_SUCCESS)
	{
		g_mem_stat->allocated[index] += mem->requirements.size;
		g_mem_stat->count[index]++;
		return true;
	}

	Core::StringList stat;
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
	{
		uint64_t allocated = g_mem_stat->allocated[i];
		uint64_t count     = g_mem_stat->count[i];
		stat.Add(String::FromPrintf("%u, %" PRIu64 ", %" PRIu64 "", i, count, allocated));
	}
	g_gpu_memory->DbgDbDump();
	g_gpu_memory->DbgDbSave(U"_gpu_memory.db");
	EXIT("size = %" PRIu64 ", index = %u, error: %s:%s\n", mem->requirements.size, index, string_VkResult(result),
	     stat.Concat(U'\n').C_Str());

	return false;
}

void VulkanFree(GraphicContext* ctx, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);

	vkFreeMemory(ctx->device, mem->memory, nullptr);

	g_mem_stat->allocated[mem->type] -= mem->requirements.size;
	g_mem_stat->count[mem->type]--;

	mem->memory = nullptr;
}

void VulkanMapMemory(GraphicContext* ctx, VulkanMemory* mem, void** data)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(data == nullptr);

	vkMapMemory(ctx->device, mem->memory, mem->offset, mem->requirements.size, 0, data);
}

void VulkanUnmapMemory(GraphicContext* ctx, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);

	vkUnmapMemory(ctx->device, mem->memory);
}

void VulkanBindImageMemory(GraphicContext* ctx, VulkanImage* image, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(image == nullptr);

	const auto bind_start = std::chrono::steady_clock::now();
	vkBindImageMemory(ctx->device, image->image, mem->memory, mem->offset);
	const auto bind_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - bind_start).count();
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::VulkanBind, static_cast<uint64_t>(bind_ns));
}

void VulkanBindBufferMemory(GraphicContext* ctx, VulkanBuffer* buffer, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(buffer == nullptr);

	const auto bind_start = std::chrono::steady_clock::now();
	vkBindBufferMemory(ctx->device, buffer->buffer, mem->memory, mem->offset);
	const auto bind_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - bind_start).count();
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::VulkanBind, static_cast<uint64_t>(bind_ns));
}

void GpuMemoryRegisterOwner(uint32_t* owner_handle, const char* name)
{
	EXIT_IF(g_gpu_resources == nullptr);
	EXIT_IF(owner_handle == nullptr);
	EXIT_IF(name == nullptr);

	*owner_handle = g_gpu_resources->AddOwner(String::FromUtf8(name));
}

void GpuMemoryRegisterResource(uint32_t* resource_handle, uint32_t owner_handle, const void* memory, size_t size, const char* name,
                               uint32_t type, uint64_t user_data)
{
	EXIT_IF(g_gpu_resources == nullptr);
	EXIT_IF(resource_handle == nullptr);
	EXIT_IF(name == nullptr);

	*resource_handle =
	    g_gpu_resources->AddResource(owner_handle, reinterpret_cast<uint64_t>(memory), size, String::FromUtf8(name), type, user_data);
}

void GpuMemoryUnregisterAllResourcesForOwner(uint32_t owner_handle)
{
	EXIT_IF(g_gpu_resources == nullptr);

	g_gpu_resources->DeleteResources(owner_handle);
}

void GpuMemoryUnregisterOwnerAndResources(uint32_t owner_handle)
{
	EXIT_IF(g_gpu_resources == nullptr);

	g_gpu_resources->DeleteOwner(owner_handle);
}

void GpuMemoryUnregisterResource(uint32_t resource_handle)
{
	EXIT_IF(g_gpu_resources == nullptr);

	g_gpu_resources->DeleteResource(resource_handle);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
