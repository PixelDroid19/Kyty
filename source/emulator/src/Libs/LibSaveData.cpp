#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/SaveData.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("SaveData", 1, "SaveData", 1, 1);

namespace SaveData {

// TODO(): specify dir at launcher
static constexpr char32_t    SAVE_DATA_DIR[]   = U"_SaveData";
static constexpr char32_t    SAVE_DATA_POINT[] = U"/savedata0";
static std::atomic<uint32_t> g_next_transaction_resource {0};
static std::mutex              g_transaction_mutex;
static std::unordered_set<int32_t> g_transaction_resources;

// SaveDataMountPoint / SaveDataMountInfo / SaveDataDirName* search types
// are declared in SaveData.h for tests and HLE registration.

struct SaveDataMount
{
	int         user_id;
	int         pad;
	const char* title_id;
	const char* dir_name;
	const char* fingerprint;
	uint64_t    blocks;
	uint32_t    mount_mode;
	uint8_t     reserved[32];
};

struct SaveDataMount2
{
	int                     user_id;
	int                     pad;
	const SaveDataDirName*  dir_name;
	uint64_t                blocks;
	uint32_t                mount_mode;
	uint8_t                 reserved[32];
	int                     pad2;
};

struct SaveDataMount3
{
	int32_t                user_id;
	int32_t                pad;
	const SaveDataDirName* dir_name;
	uint64_t               blocks;
	uint64_t               system_blocks;
	uint32_t               mount_mode;
	uint32_t               resource;
	uint32_t               mode;
};

struct SaveDataMountResult
{
	SaveDataMountPoint mount_point;
	uint64_t           required_blocks;
	uint32_t           unused;
	uint32_t           mount_status;
	uint8_t            reserved[28];
	int                pad;
};

struct SaveDataParam
{
	char     title[128];
	char     sub_title[128];
	char     detail[1024];
	uint32_t user_param;
	int      pad;
	int64_t  mtime;
	uint8_t  reserved[32];
};

struct SaveDataIcon
{
	void*   buf;
	size_t  buf_size;
	size_t  data_size;
	uint8_t reserved[32];
};

int KYTY_SYSV_ABI SaveDataInitialize(const void* /*init*/)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(init != nullptr);

	return OK;
}

int KYTY_SYSV_ABI SaveDataInitialize2(const void* /*init*/)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(init != nullptr);

	return OK;
}

int KYTY_SYSV_ABI SaveDataInitialize3(const void* /*init*/)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(init != nullptr);

	return OK;
}

// Gen5 ABI (NID gjRZNnw0JPE): rdi=user_id; return value is the resource id (>0).
// Call site: test eax,eax / js error / mov [global], eax — so 0 would be stored
// as a bogus handle. Negative values are errors (INVALID_LOGIN_USER etc.).
int KYTY_SYSV_ABI SaveDataCreateTransactionResource(int32_t user_id)
{
	PRINT_NAME();

	printf("\t user_id  = %d\n", user_id);

	if (user_id < 0)
	{
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
	}

	const int32_t id = static_cast<int32_t>(g_next_transaction_resource.fetch_add(1, std::memory_order_relaxed) + 1);
	{
		std::lock_guard<std::mutex> lock(g_transaction_mutex);
		g_transaction_resources.insert(id);
	}
	printf("\t resource = %d\n", id);
	return id;
}

// NID uW4vfTwMQVo (SaveData_native). Observed SysV: rdi=resource-or-user (1),
// rsi=MountPoint* "/savedata0" after Mount3/GetMountInfo/file create.
// Bound as transaction-resource release: resource handles come from
// CreateTransactionResource (first id is 1). Mount pointer is validated when
// provided so a null second arg still fails cleanly.
int KYTY_SYSV_ABI SaveDataDeleteTransactionResource(int32_t resource, const SaveDataMountPoint* mount_point)
{
	PRINT_NAME();

	printf("\t resource    = %d\n", resource);
	printf("\t mount_point = %s\n", (mount_point != nullptr ? mount_point->data : "(null)"));

	if (resource <= 0)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	std::lock_guard<std::mutex> lock(g_transaction_mutex);
	const auto it = g_transaction_resources.find(resource);
	if (it == g_transaction_resources.end())
	{
		return SAVE_DATA_ERROR_NOT_FOUND;
	}
	g_transaction_resources.erase(it);
	return OK;
}

// NID dyIhnXq-0SM — sceSaveDataDirNameSearch (public NID tables; SaveData_native
// Gen5 import after CreateTransactionResource + 64 KiB direct-memory map).
// Lists host directories under `_SaveData` that match an optional name filter.
// Empty host root yields hit_num/set_num = 0 with OK (first-boot path).
int KYTY_SYSV_ABI SaveDataDirNameSearch(const SaveDataDirNameSearchCond* cond, SaveDataDirNameSearchResult* result)
{
	PRINT_NAME();

	if (cond == nullptr || result == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	// Sort enums: DIRNAME=0 … FREE_BLOCKS=5; order ASCENT=0 / DESCENT=1.
	if (cond->key > 5u || cond->order > 1u)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	printf("\t user_id       = %d\n", cond->user_id);
	printf("\t title_id      = %s\n", (cond->title_id != nullptr ? cond->title_id->data : "(null)"));
	printf("\t dir_name_pat  = %s\n", (cond->dir_name != nullptr ? cond->dir_name->data : "(null)"));
	printf("\t key/order     = %u/%u\n", cond->key, cond->order);
	printf("\t dir_names_num = %u\n", result->dir_names_num);

	result->hit_num = 0;
	result->set_num = 0;

	const String root = SAVE_DATA_DIR;
	if (!Core::File::IsDirectoryExisting(root))
	{
		return OK;
	}

	const char* filter = (cond->dir_name != nullptr ? cond->dir_name->data : nullptr);
	const bool  filter_active = (filter != nullptr && filter[0] != '\0');

	const auto entries = Core::File::GetDirEntries(root);
	uint32_t   written = 0;

	for (const auto& entry: entries)
	{
		if (entry.is_file)
		{
			continue;
		}

		const String name = entry.name;
		if (name.IsEmpty() || name == U"." || name == U".." || name.StartsWith(U"sce_"))
		{
			continue;
		}

		// utf8_str() owns the buffer; C_Str macro would dangle after the expression.
		const auto  name_utf8 = name.utf8_str();
		const char* name_c    = name_utf8.GetData();
		if (name_c == nullptr)
		{
			continue;
		}
		if (filter_active)
		{
			// Exact match; wildcard filter not required for first-boot enumeration.
			if (std::strcmp(name_c, filter) != 0)
			{
				continue;
			}
		}

		result->hit_num++;
		if (result->dir_names != nullptr && written < result->dir_names_num)
		{
			std::memset(&result->dir_names[written], 0, sizeof(SaveDataDirName));
			std::snprintf(result->dir_names[written].data, sizeof(result->dir_names[written].data), "%s", name_c);
			if (result->infos != nullptr)
			{
				result->infos[written].blocks      = 100000;
				result->infos[written].free_blocks = 100000;
			}
			written++;
		}
	}

	result->set_num = written;
	printf("\t hit_num = %u set_num = %u\n", result->hit_num, result->set_num);
	return OK;
}

int KYTY_SYSV_ABI SaveDataMount(const SaveDataMount* mount, SaveDataMountResult* mount_result)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(mount == nullptr);
	EXIT_NOT_IMPLEMENTED(mount_result == nullptr);

	printf("\t user_id     = %d\n", mount->user_id);
	printf("\t title_id    = %s\n", mount->title_id);
	printf("\t dir_name    = %s\n", mount->dir_name);
	printf("\t fingerprint = %s\n", mount->fingerprint);
	printf("\t blocks      = %" PRIu64 "\n", mount->blocks);
	printf("\t mount_mode  = %" PRIu32 "\n", mount->mount_mode);

	String mount_dir   = String(SAVE_DATA_DIR) + U"/" + String::FromUtf8(mount->dir_name);
	String mount_point = SAVE_DATA_POINT;
	bool   create      = (mount->mount_mode == 6 || mount->mount_mode == 22);
	bool   open        = (mount->mount_mode == 1 || mount->mount_mode == 2);

	if (!create && !open)
	{
		EXIT("unknown mount mode: %u", mount->mount_mode);
	}

	if (open)
	{
		EXIT_NOT_IMPLEMENTED(create);

		if (!Core::File::IsDirectoryExisting(mount_dir))
		{
			return SAVE_DATA_ERROR_NOT_FOUND;
		}

		LibKernel::FileSystem::Mount(mount_dir, mount_point);

		mount_result->mount_status = 0;
	}

	if (create)
	{
		EXIT_NOT_IMPLEMENTED(open);

		if (Core::File::IsDirectoryExisting(mount_dir))
		{
			return SAVE_DATA_ERROR_EXISTS;
		}

		Core::File::CreateDirectories(mount_dir);

		EXIT_NOT_IMPLEMENTED((!Core::File::IsDirectoryExisting(mount_dir)));

		LibKernel::FileSystem::Mount(mount_dir, mount_point);

		mount_result->mount_status = 1;
	}

	int s = snprintf(mount_result->mount_point.data, 16, "%s", mount_point.C_Str());

	EXIT_NOT_IMPLEMENTED(s >= 16);

	mount_result->required_blocks = 0;

	return OK;
}

int KYTY_SYSV_ABI SaveDataMount2(const SaveDataMount2* mount, SaveDataMountResult* mount_result)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(mount == nullptr);
	EXIT_NOT_IMPLEMENTED(mount_result == nullptr);

	printf("\t user_id    = %d\n", mount->user_id);
	printf("\t dir_name   = %s\n", mount->dir_name->data);
	printf("\t blocks     = %" PRIu64 "\n", mount->blocks);
	printf("\t mount_mode = %" PRIu32 "\n", mount->mount_mode);

	String mount_dir   = String(SAVE_DATA_DIR) + U"/" + String::FromUtf8(mount->dir_name->data);
	String mount_point = SAVE_DATA_POINT;
	bool   create      = (mount->mount_mode == 6 || mount->mount_mode == 22);
	bool   open        = (mount->mount_mode == 1 || mount->mount_mode == 2);

	if (!create && !open)
	{
		EXIT("unknown mount mode: %u", mount->mount_mode);
	}

	if (open)
	{
		EXIT_NOT_IMPLEMENTED(create);

		if (!Core::File::IsDirectoryExisting(mount_dir))
		{
			return SAVE_DATA_ERROR_NOT_FOUND;
		}

		LibKernel::FileSystem::Mount(mount_dir, mount_point);

		mount_result->mount_status = 0;
	}

	if (create)
	{
		EXIT_NOT_IMPLEMENTED(open);

		if (Core::File::IsDirectoryExisting(mount_dir))
		{
			return SAVE_DATA_ERROR_EXISTS;
		}

		Core::File::CreateDirectories(mount_dir);

		EXIT_NOT_IMPLEMENTED((!Core::File::IsDirectoryExisting(mount_dir)));

		LibKernel::FileSystem::Mount(mount_dir, mount_point);

		mount_result->mount_status = 1;
	}

	int s = snprintf(mount_result->mount_point.data, 16, "%s", mount_point.C_Str());

	EXIT_NOT_IMPLEMENTED(s >= 16);

	mount_result->required_blocks = 0;

	return OK;
}

int KYTY_SYSV_ABI SaveDataMount3(const SaveDataMount3* mount, SaveDataMountResult* mount_result)
{
	PRINT_NAME();

	if (mount == nullptr || mount_result == nullptr || mount->dir_name == nullptr || mount->user_id < 0)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	String dir_name = String::FromUtf8(mount->dir_name->data);
	if (dir_name.IsEmpty())
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	String     mount_dir         = String(SAVE_DATA_DIR) + U"/" + dir_name;
	String     mount_point       = SAVE_DATA_POINT;
	const bool existed           = Core::File::IsDirectoryExisting(mount_dir);
	const bool create            = (mount->mount_mode & 0x04u) != 0;
	const bool create_if_missing = (mount->mount_mode & 0x20u) != 0;

	if (!existed && !create && !create_if_missing)
	{
		return SAVE_DATA_ERROR_NOT_FOUND;
	}
	if (existed && create)
	{
		return SAVE_DATA_ERROR_EXISTS;
	}
	if (!existed)
	{
		Core::File::CreateDirectories(mount_dir);
		if (!Core::File::IsDirectoryExisting(mount_dir))
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
	}

	LibKernel::FileSystem::Mount(mount_dir, mount_point);
	memset(mount_result, 0, sizeof(*mount_result));
	const int written = snprintf(mount_result->mount_point.data, sizeof(mount_result->mount_point.data), "%s", mount_point.C_Str());
	if (written < 0 || written >= static_cast<int>(sizeof(mount_result->mount_point.data)))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	mount_result->mount_status = (create_if_missing && !existed ? 1u : 0u);
	return OK;
}

// sceSaveDataTransferringMount — NID WAzWTZm1H+I / RjMlsR8EXrw (SaveData_native).
// Observed Astro after PlayGo on thread 10: (mount*, result*). Creates host dir
// and mounts at /savedata0 when missing so first-boot transfer continues.
struct SaveDataTransferringMountParam
{
	int32_t                user_id;
	const SaveDataTitleId* title_id;
	const SaveDataDirName* dir_name;
	const void*            fingerprint;
	uint8_t                reserved[32];
};

int KYTY_SYSV_ABI SaveDataTransferringMount(const SaveDataTransferringMountParam* mount, SaveDataMountResult* mount_result)
{
	PRINT_NAME();
	if (mount == nullptr || mount_result == nullptr || mount->dir_name == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	if (mount->user_id < 0)
	{
		return SAVE_DATA_ERROR_INVALID_LOGIN_USER;
	}

	String dir_name = String::FromUtf8(mount->dir_name->data);
	if (dir_name.IsEmpty())
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	printf("\t user_id  = %d\n", mount->user_id);
	printf("\t title_id = %s\n", mount->title_id != nullptr ? mount->title_id->data : "<null>");
	printf("\t dir_name = %s\n", mount->dir_name->data);

	// Layout mirrors Mount3 host path: _SaveData/<dir_name> → /savedata0.
	String       mount_dir   = String(SAVE_DATA_DIR) + U"/" + dir_name;
	String       mount_point = SAVE_DATA_POINT;
	const bool   existed     = Core::File::IsDirectoryExisting(mount_dir);
	if (!existed)
	{
		Core::File::CreateDirectories(mount_dir);
		if (!Core::File::IsDirectoryExisting(mount_dir))
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
	}

	LibKernel::FileSystem::Mount(mount_dir, mount_point);
	std::memset(mount_result, 0, sizeof(*mount_result));
	const int written =
	    snprintf(mount_result->mount_point.data, sizeof(mount_result->mount_point.data), "%s", mount_point.C_Str());
	if (written < 0 || written >= static_cast<int>(sizeof(mount_result->mount_point.data)))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	mount_result->required_blocks = 0;
	mount_result->mount_status    = (existed ? 0u : 1u);
	return OK;
}

int KYTY_SYSV_ABI SaveDataUmount(const SaveDataMountPoint* mount_point)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(mount_point == nullptr);

	printf("\t mount_point = %s\n", mount_point->data);

	LibKernel::FileSystem::Umount(String::FromUtf8(mount_point->data));

	return OK;
}

int KYTY_SYSV_ABI SaveDataSetParam(const SaveDataMountPoint* mount_point, uint32_t param_type, const void* param_buf, size_t param_buf_size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(mount_point == nullptr);

	printf("\t mount_point    = %s\n", mount_point->data);
	printf("\t param_type     = %u\n", param_type);
	printf("\t param_buf_size = %" PRIu64 "\n", param_buf_size);

	if (param_type == 0)
	{
		const auto* p = static_cast<const SaveDataParam*>(param_buf);

		printf("\t title      = %s\n", p->title);
		printf("\t sub_title  = %s\n", p->sub_title);
		printf("\t detail     = %s\n", p->detail);
		printf("\t user_param = %u\n", p->user_param);
	} else
	{
		KYTY_NOT_IMPLEMENTED;
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataGetMountInfo(const SaveDataMountPoint* mount_point, SaveDataMountInfo* info)
{
	PRINT_NAME();

	if (mount_point == nullptr || info == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	printf("\t mount_point = %s\n", mount_point->data);

	// Mounted save capacity reported to the guest. Values are large enough for
	// typical title save slots; free_blocks tracks remaining capacity.
	info->blocks      = 100000;
	info->free_blocks = 100000;

	return OK;
}

// sceSaveDataGetEventResult — polled after async save operations.
// When the HLE event queue is empty, return NOT_FOUND (no pending event).
int KYTY_SYSV_ABI SaveDataGetEventResult(const void* /*event_param*/, void* event)
{
	PRINT_NAME();

	if (event == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	return SAVE_DATA_ERROR_NOT_FOUND;
}

// sceSaveDataCommit — completes a save transaction descriptor supplied by the guest.
// The observed caller only requires synchronous success after a non-null descriptor.
int KYTY_SYSV_ABI SaveDataCommit(const void* commit_param)
{
	PRINT_NAME();

	printf("\t commit_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(commit_param));

	if (commit_param == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	return OK;
}

int KYTY_SYSV_ABI SaveDataSaveIcon(const SaveDataMountPoint* mount_point, const SaveDataIcon* icon)
{
	EXIT_NOT_IMPLEMENTED(mount_point == nullptr);
	EXIT_NOT_IMPLEMENTED(icon == nullptr);

	printf("\t buf       = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(icon->buf));
	printf("\t buf_size  = %" PRIu64 "\n", icon->buf_size);
	printf("\t data_size = %" PRIu64 "\n", icon->data_size);

	return OK;
}

// sceSaveData*SaveDataMemory2 guest param layouts (Gen5). First-boot Get must
// not return NOT_FOUND — Astro SaveMemory.cpp asserts on 0x809F0008.
struct SaveDataMemoryData
{
	void*   buf;
	size_t  buf_size;
	int64_t offset;
	uint8_t reserved[40];
};

struct SaveDataMemoryGet2
{
	int32_t             user_id;
	uint8_t             padding[4];
	SaveDataMemoryData* data;
	void*               param;
	void*               icon;
	uint32_t            slot_id;
	uint8_t             reserved[28];
};

struct SaveDataMemorySetup2
{
	uint32_t    option;
	int32_t     user_id;
	size_t      memory_size;
	size_t      icon_memory_size;
	const void* init_param;
	const void* init_icon;
	uint32_t    slot_id;
	uint8_t     reserved[20];
};

struct SaveDataMemorySetupResult
{
	size_t  existed_memory_size;
	uint8_t reserved[16];
};

struct SaveDataMemorySet2
{
	int32_t                   user_id;
	uint8_t                   padding[4];
	const SaveDataMemoryData* data;
	const void*               param;
	const void*               icon;
	uint32_t                  data_num;
	uint32_t                  slot_id;
	uint8_t                   reserved[24];
};

// In-process save-memory slot (zero-filled until Set writes).
static std::mutex           g_save_memory_mutex;
static std::vector<uint8_t> g_save_data_memory(0x10000);

// sceSaveDataSetupSaveDataMemory2 (NID oQySEUfgXRA)
int KYTY_SYSV_ABI SaveDataSetupSaveDataMemory2(void* setup_param, void* result_out)
{
	PRINT_NAME();
	printf("\t setup_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(setup_param));
	printf("\t result_out  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result_out));
	if (setup_param == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	const auto* setup = static_cast<const SaveDataMemorySetup2*>(setup_param);
	printf("\t option=%#x user_id=%d memory_size=%" PRIu64 " slot=%u\n", setup->option, setup->user_id,
	       static_cast<uint64_t>(setup->memory_size), setup->slot_id);

	{
		std::lock_guard lock(g_save_memory_mutex);
		if (setup->memory_size > g_save_data_memory.size())
		{
			g_save_data_memory.resize(setup->memory_size, 0);
		}
		if (result_out != nullptr)
		{
			auto* result                  = static_cast<SaveDataMemorySetupResult*>(result_out);
			result->existed_memory_size   = g_save_data_memory.size();
			std::memset(result->reserved, 0, sizeof(result->reserved));
		}
	}
	return OK;
}

// sceSaveDataGetSaveDataMemory2 (NID QwOO7vegnV8)
// First boot: return OK with zeros (NOT_FOUND aborts Astro SaveMemory.cpp:118).
int KYTY_SYSV_ABI SaveDataGetSaveDataMemory2(void* get_param)
{
	PRINT_NAME();
	printf("\t get_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(get_param));
	if (get_param == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	auto* get = static_cast<SaveDataMemoryGet2*>(get_param);
	printf("\t user_id=%d data=%p slot=%u\n", get->user_id, static_cast<void*>(get->data), get->slot_id);

	std::lock_guard lock(g_save_memory_mutex);
	if (get->data != nullptr)
	{
		auto* data = get->data;
		if (data->buf == nullptr || data->offset < 0)
		{
			return SAVE_DATA_ERROR_PARAMETER;
		}
		const auto offset = static_cast<size_t>(data->offset);
		if (offset + data->buf_size > g_save_data_memory.size())
		{
			g_save_data_memory.resize(offset + data->buf_size, 0);
		}
		std::memcpy(data->buf, g_save_data_memory.data() + offset, data->buf_size);
	}
	if (get->param != nullptr)
	{
		// Param blob size is title-defined; clear a conservative 0x80 prefix.
		std::memset(get->param, 0, 0x80);
	}
	return OK;
}

// sceSaveDataSetSaveDataMemory2 (NID cduy9v4YmT4)
int KYTY_SYSV_ABI SaveDataSetSaveDataMemory2(void* set_param)
{
	PRINT_NAME();
	printf("\t set_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(set_param));
	if (set_param == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	const auto* set = static_cast<const SaveDataMemorySet2*>(set_param);
	printf("\t user_id=%d data=%p data_num=%u slot=%u\n", set->user_id, static_cast<const void*>(set->data),
	       set->data_num, set->slot_id);

	std::lock_guard lock(g_save_memory_mutex);
	const uint32_t  data_num = (set->data_num == 0 ? 1u : set->data_num);
	if (set->data != nullptr)
	{
		for (uint32_t i = 0; i < data_num; ++i)
		{
			const auto& data = set->data[i];
			if (data.buf == nullptr || data.offset < 0)
			{
				return SAVE_DATA_ERROR_PARAMETER;
			}
			const auto offset = static_cast<size_t>(data.offset);
			if (offset + data.buf_size > g_save_data_memory.size())
			{
				g_save_data_memory.resize(offset + data.buf_size, 0);
			}
			std::memcpy(g_save_data_memory.data() + offset, data.buf, data.buf_size);
		}
	}
	return OK;
}

} // namespace SaveData

LIB_DEFINE(InitSaveData_1)
{
	LIB_FUNC("ZkZhskCPXFw", SaveData::SaveDataInitialize);
	LIB_FUNC("l1NmDeDpNGU", SaveData::SaveDataInitialize2);
	LIB_FUNC("TywrFKCoLGY", SaveData::SaveDataInitialize3);
	LIB_FUNC("gjRZNnw0JPE", SaveData::SaveDataCreateTransactionResource);
	LIB_FUNC("32HQAQdwM2o", SaveData::SaveDataMount);
	LIB_FUNC("0z45PIH+SNI", SaveData::SaveDataMount2);
	LIB_FUNC("ZP4e7rlzOUk", SaveData::SaveDataMount3);
	LIB_FUNC("BMR4F-Uek3E", SaveData::SaveDataUmount);
	LIB_FUNC("85zul--eGXs", SaveData::SaveDataSetParam);
	LIB_FUNC("65VH0Qaaz6s", SaveData::SaveDataGetMountInfo);
	// sceSaveDataDirNameSearch
	LIB_FUNC("dyIhnXq-0SM", SaveData::SaveDataDirNameSearch);
	// sceSaveDataGetEventResult
	LIB_FUNC("j8xKtiFj0SY", SaveData::SaveDataGetEventResult);
	LIB_FUNC("ie7qhZ4X0Cc", SaveData::SaveDataCommit);
	LIB_FUNC("c88Yy54Mx0w", SaveData::SaveDataSaveIcon);
	// Memory2 APIs (Gen5 SaveData memory slots).
	LIB_FUNC("oQySEUfgXRA", SaveData::SaveDataSetupSaveDataMemory2);
	LIB_FUNC("QwOO7vegnV8", SaveData::SaveDataGetSaveDataMemory2);
	LIB_FUNC("cduy9v4YmT4", SaveData::SaveDataSetSaveDataMemory2);
	// TransferringMount also appears on SaveData_v1 tables.
	LIB_FUNC("WAzWTZm1H+I", SaveData::SaveDataTransferringMount);
	LIB_FUNC("RjMlsR8EXrw", SaveData::SaveDataTransferringMount);
}

namespace SaveDataNative {

LIB_VERSION("SaveData_native", 1, "SaveData_native", 1, 1);

LIB_DEFINE(InitSaveDataNative_1)
{
	LIB_FUNC("ZkZhskCPXFw", SaveData::SaveDataInitialize);
	LIB_FUNC("l1NmDeDpNGU", SaveData::SaveDataInitialize2);
	LIB_FUNC("TywrFKCoLGY", SaveData::SaveDataInitialize3);
	LIB_FUNC("gjRZNnw0JPE", SaveData::SaveDataCreateTransactionResource);
	LIB_FUNC("32HQAQdwM2o", SaveData::SaveDataMount);
	LIB_FUNC("0z45PIH+SNI", SaveData::SaveDataMount2);
	LIB_FUNC("ZP4e7rlzOUk", SaveData::SaveDataMount3);
	LIB_FUNC("BMR4F-Uek3E", SaveData::SaveDataUmount);
	LIB_FUNC("85zul--eGXs", SaveData::SaveDataSetParam);
	LIB_FUNC("65VH0Qaaz6s", SaveData::SaveDataGetMountInfo);
	// sceSaveDataGetEventResult
	LIB_FUNC("j8xKtiFj0SY", SaveData::SaveDataGetEventResult);
	LIB_FUNC("ie7qhZ4X0Cc", SaveData::SaveDataCommit);
	// Observed Gen5 import (SaveData_native): SysV (MountPoint*, info*).
	// Public PS4 tables list GetMountInfo as 65VH0Qaaz6s; this title does not
	// import that NID. Call-site capture shows rdi="/savedata0" and rsi as a
	// guest buffer after Mount3 — same shape as GetMountInfo. Bound to the
	// shared implementation until a distinct name/layout is evidenced.
	LIB_FUNC("sDCBrmc61XU", SaveData::SaveDataGetMountInfo);
	// NID uW4vfTwMQVo: SysV rdi=1 (first CreateTransactionResource id / user),
	// rsi=MountPoint* "/savedata0". Treated as transaction-resource release.
	LIB_FUNC("uW4vfTwMQVo", SaveData::SaveDataDeleteTransactionResource);
	// NID dyIhnXq-0SM: sceSaveDataDirNameSearch (boot save enumeration).
	LIB_FUNC("dyIhnXq-0SM", SaveData::SaveDataDirNameSearch);
	LIB_FUNC("c88Yy54Mx0w", SaveData::SaveDataSaveIcon);
	LIB_FUNC("oQySEUfgXRA", SaveData::SaveDataSetupSaveDataMemory2);
	LIB_FUNC("QwOO7vegnV8", SaveData::SaveDataGetSaveDataMemory2);
	// sceSaveDataTransferringMount — NID WAzWTZm1H+I (Astro after PlayGo).
	LIB_FUNC("WAzWTZm1H+I", SaveData::SaveDataTransferringMount);
	LIB_FUNC("RjMlsR8EXrw", SaveData::SaveDataTransferringMount);
	LIB_FUNC("cduy9v4YmT4", SaveData::SaveDataSetSaveDataMemory2);
}

} // namespace SaveDataNative

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
