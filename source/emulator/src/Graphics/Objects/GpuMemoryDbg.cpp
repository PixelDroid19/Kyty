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
			KYTY_LOG_DEBUG("Can't open file: %s\n", file_name.C_Str());
			return;
		}
		m_db.CopyTo(&db);
		db.Close();
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
