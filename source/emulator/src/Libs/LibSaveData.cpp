#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Host/Platform.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/LibraryRegistration.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/SaveData.h"
#include "Emulator/Libs/SaveDataMemoryStore.h"
#include "Emulator/Libs/SaveDataMountCoordinator.h"
#include "Emulator/Libs/SaveDataPaths.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Loader/SystemContent.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("SaveData", 1, "SaveData", 1, 1);

namespace SaveData {

static std::atomic<uint32_t>       g_next_transaction_resource {0};
static std::mutex                  g_transaction_mutex;
static std::unordered_set<int32_t> g_transaction_resources;
static std::mutex                  g_mount_mutex;
static SaveDataMountCoordinator    g_mount_coordinator;

static std::filesystem::path ResolveSaveDataRoot(const char* title_id)
{
	std::filesystem::path root;
	if (const char* configured = std::getenv("KYTY_SAVEDATA_DIR"); configured != nullptr && configured[0] != '\0')
	{
		root = std::filesystem::u8path(configured);
		if (!root.is_absolute())
		{
			return {};
		}
	} else
	{
		const auto base_path = Kyty::Emulator::Host::ApplicationBasePath();
		if (base_path.empty())
		{
			return {};
		}
		root = std::filesystem::u8path(base_path) / "user" / "savedata";
	}

	if (title_id != nullptr && title_id[0] != '\0')
	{
		return SaveDataBuildTitleRoot(root.lexically_normal(), title_id);
	}

	String metadata_title;
	String metadata_version;
	if (!Loader::SystemContentGetMetadata(&metadata_title, &metadata_version) || metadata_title.IsEmpty())
	{
		return {};
	}

	const auto title_utf8 = metadata_title.utf8_str();
	return SaveDataBuildTitleRoot(root.lexically_normal(), title_utf8.GetData());
}

static bool ResolveSaveDataSlot(const char* title_id, const char* directory_name, String* out)
{
	if (!SaveDataDirectoryNameValid(directory_name) || out == nullptr)
	{
		return false;
	}
	const auto root = ResolveSaveDataRoot(title_id);
	if (root.empty())
	{
		return false;
	}
	const auto path = (root / std::filesystem::u8path(directory_name)).lexically_normal().u8string();
	*out            = String::FromUtf8(path.c_str());
	return !out->IsEmpty();
}

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
	int                    user_id;
	int                    pad;
	const SaveDataDirName* dir_name;
	uint64_t               blocks;
	uint32_t               mount_mode;
	uint8_t                reserved[32];
	int                    pad2;
};

struct SaveDataMount3
{
	int32_t                user_id;
	int32_t                pad;
	const SaveDataDirName* dir_name;
	uint64_t               blocks;
	uint64_t               system_blocks;
	uint32_t               mount_mode;
	int32_t                pad2;
	int32_t                resource;
	uint8_t                reserved[32];
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

static_assert(sizeof(SaveDataMount3) == 80);
static_assert(offsetof(SaveDataMount3, dir_name) == 8);
static_assert(offsetof(SaveDataMount3, mount_mode) == 32);
static_assert(offsetof(SaveDataMount3, resource) == 40);
static_assert(sizeof(SaveDataMountResult) == 64);
static_assert(offsetof(SaveDataMountResult, required_blocks) == 16);
static_assert(offsetof(SaveDataMountResult, mount_status) == 28);

static int MountSaveDataDirectory(const char* directory_name, const String& mount_dir, uint32_t mount_mode,
                                  SaveDataMountResult* mount_result)
{
	if (directory_name == nullptr || mount_result == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	std::lock_guard lock(g_mount_mutex);
	const auto      acquisition = g_mount_coordinator.Acquire(directory_name);
	if (acquisition.result == SaveDataMountCoordinator::AcquireResult::AlreadyMounted)
	{
		return SAVE_DATA_ERROR_BUSY;
	}
	if (acquisition.result == SaveDataMountCoordinator::AcquireResult::Full)
	{
		return SAVE_DATA_ERROR_MOUNT_FULL;
	}

	const bool create            = (mount_mode & 0x04u) != 0;
	const bool create_if_missing = (mount_mode & 0x20u) != 0;
	const bool open              = !create && !create_if_missing && (mount_mode & 0x03u) != 0;
	if (!create && !create_if_missing && !open)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	const bool existed = Core::File::IsDirectoryExisting(mount_dir);
	if (open && !existed)
	{
		return SAVE_DATA_ERROR_NOT_FOUND;
	}
	if (create && existed)
	{
		return SAVE_DATA_ERROR_EXISTS;
	}

	bool created = false;
	if (!existed)
	{
		Core::File::CreateDirectories(mount_dir);
		if (!Core::File::IsDirectoryExisting(mount_dir))
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
		created = true;
	}

	const auto   mount_point_utf8 = SaveDataMountCoordinator::MountPoint(acquisition.slot);
	const String mount_point      = String::FromUtf8(mount_point_utf8.c_str());
	LibKernel::FileSystem::Mount(mount_dir, mount_point);
	std::memset(mount_result, 0, sizeof(*mount_result));
	const int written = std::snprintf(mount_result->mount_point.data, sizeof(mount_result->mount_point.data), "%s", mount_point.C_Str());
	if (written < 0 || written >= static_cast<int>(sizeof(mount_result->mount_point.data)))
	{
		LibKernel::FileSystem::Umount(mount_point);
		return SAVE_DATA_ERROR_INTERNAL;
	}

	g_mount_coordinator.Commit(acquisition.slot, directory_name);
	mount_result->mount_status = created ? 1u : 0u;
	return OK;
}

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

// sceSaveDataDeleteTransactionResource (NID lJUQuaKqoKY).
int KYTY_SYSV_ABI SaveDataDeleteTransactionResource(int32_t resource)
{
	PRINT_NAME();

	printf("\t resource = %d\n", resource);

	if (resource <= 0)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	std::lock_guard<std::mutex> lock(g_transaction_mutex);
	const auto                  it = g_transaction_resources.find(resource);
	if (it == g_transaction_resources.end())
	{
		return SAVE_DATA_ERROR_NOT_FOUND;
	}
	g_transaction_resources.erase(it);
	return OK;
}

// NID dyIhnXq-0SM — sceSaveDataDirNameSearch (public NID tables; SaveData_native
// Gen5 import after CreateTransactionResource + 64 KiB direct-memory map).
// Lists host directories under the current title's portable save root.
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

	const auto root_path = ResolveSaveDataRoot(cond->title_id != nullptr ? cond->title_id->data : nullptr);
	if (root_path.empty())
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	const auto   root_utf8 = root_path.u8string();
	const String root      = String::FromUtf8(root_utf8.c_str());
	if (!Core::File::IsDirectoryExisting(root))
	{
		return OK;
	}

	const char* filter        = (cond->dir_name != nullptr ? cond->dir_name->data : nullptr);
	const bool  filter_active = (filter != nullptr && filter[0] != '\0');
	if (filter_active && !SaveDataDirectoryNameValid(filter))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

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

	if (mount == nullptr || mount_result == nullptr || mount->dir_name == nullptr || mount->user_id < 0)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	printf("\t user_id     = %d\n", mount->user_id);
	printf("\t title_id    = %s\n", mount->title_id != nullptr ? mount->title_id : "<current>");
	printf("\t dir_name    = %s\n", mount->dir_name);
	printf("\t fingerprint = %s\n", mount->fingerprint != nullptr ? mount->fingerprint : "<null>");
	printf("\t blocks      = %" PRIu64 "\n", mount->blocks);
	printf("\t mount_mode  = %" PRIu32 "\n", mount->mount_mode);

	String mount_dir;
	if (!ResolveSaveDataSlot(mount->title_id, mount->dir_name, &mount_dir))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	return MountSaveDataDirectory(mount->dir_name, mount_dir, mount->mount_mode, mount_result);
}

int KYTY_SYSV_ABI SaveDataMount2(const SaveDataMount2* mount, SaveDataMountResult* mount_result)
{
	PRINT_NAME();

	if (mount == nullptr || mount_result == nullptr || mount->dir_name == nullptr || mount->user_id < 0)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	printf("\t user_id    = %d\n", mount->user_id);
	printf("\t dir_name   = %s\n", mount->dir_name->data);
	printf("\t blocks     = %" PRIu64 "\n", mount->blocks);
	printf("\t mount_mode = %" PRIu32 "\n", mount->mount_mode);

	String mount_dir;
	if (!ResolveSaveDataSlot(nullptr, mount->dir_name->data, &mount_dir))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	return MountSaveDataDirectory(mount->dir_name->data, mount_dir, mount->mount_mode, mount_result);
}

int KYTY_SYSV_ABI SaveDataMount3(const SaveDataMount3* mount, SaveDataMountResult* mount_result)
{
	PRINT_NAME();

	if (mount == nullptr || mount_result == nullptr || mount->dir_name == nullptr || mount->user_id < 0)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	String mount_dir;
	if (!ResolveSaveDataSlot(nullptr, mount->dir_name->data, &mount_dir))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	return MountSaveDataDirectory(mount->dir_name->data, mount_dir, mount->mount_mode, mount_result);
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

	String mount_dir;
	if (!ResolveSaveDataSlot(mount->title_id != nullptr ? mount->title_id->data : nullptr, mount->dir_name->data, &mount_dir))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	printf("\t user_id  = %d\n", mount->user_id);
	printf("\t title_id = %s\n", mount->title_id != nullptr ? mount->title_id->data : "<null>");
	printf("\t dir_name = %s\n", mount->dir_name->data);

	return MountSaveDataDirectory(mount->dir_name->data, mount_dir, 0x20u, mount_result);
}

static int UnmountSaveDataPoint(const SaveDataMountPoint* mount_point)
{
	if (mount_point == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	std::lock_guard lock(g_mount_mutex);
	const auto      slot = g_mount_coordinator.Find(mount_point->data);
	if (!slot.has_value())
	{
		return SAVE_DATA_ERROR_NOT_MOUNTED;
	}

	LibKernel::FileSystem::Umount(String::FromUtf8(mount_point->data));
	g_mount_coordinator.Release(*slot);
	return OK;
}

int KYTY_SYSV_ABI SaveDataUmount(const SaveDataMountPoint* mount_point)
{
	PRINT_NAME();

	printf("\t mount_point = %s\n", mount_point != nullptr ? mount_point->data : "(null)");
	return UnmountSaveDataPoint(mount_point);
}

constexpr uint32_t SAVE_DATA_UMOUNT_MODE_BACKUP_ASYNC = 1u << 16u;
constexpr uint32_t SAVE_DATA_EVENT_TYPE_UMOUNT_BACKUP_END = 1u;
constexpr uint32_t SAVE_DATA_EVENT_TYPE_COMMIT_BACKUP_END = 4u;

static void EnqueueSaveDataEvent(uint32_t type, int32_t user_id, const char* dir_name, int32_t error_code);

// sceSaveDataUmount2 (NID uW4vfTwMQVo).
int KYTY_SYSV_ABI SaveDataUmount2(uint32_t mode, const SaveDataMountPoint* mount_point)
{
	PRINT_NAME();

	printf("\t mode        = %u\n", mode);
	printf("\t mount_point = %s\n", mount_point != nullptr ? mount_point->data : "(null)");

	const int result = UnmountSaveDataPoint(mount_point);
	if (result != OK)
	{
		return result;
	}
	if ((mode & SAVE_DATA_UMOUNT_MODE_BACKUP_ASYNC) != 0)
	{
		EnqueueSaveDataEvent(SAVE_DATA_EVENT_TYPE_UMOUNT_BACKUP_END, 0, "", 0);
	}
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

static SaveDataMemoryStore g_save_memory_store;

static bool ResolveSaveDataMemorySlot(int32_t user_id, uint32_t slot_id, std::filesystem::path* path)
{
	if (path == nullptr)
	{
		return false;
	}
	const auto title_root = ResolveSaveDataRoot(nullptr);
	*path                 = SaveDataBuildMemoryPath(title_root, user_id, slot_id);
	return !path->empty();
}

static int SaveDataMemoryError(SaveDataMemoryStoreResult result)
{
	switch (result)
	{
		case SaveDataMemoryStoreResult::Success: return OK;
		case SaveDataMemoryStoreResult::InvalidArgument: return SAVE_DATA_ERROR_PARAMETER;
		case SaveDataMemoryStoreResult::NotReady: return SAVE_DATA_ERROR_MEMORY_NOT_READY;
		case SaveDataMemoryStoreResult::CapacityExceeded: return SAVE_DATA_ERROR_OUT_OF_MEMORY;
		case SaveDataMemoryStoreResult::IoError: return SAVE_DATA_ERROR_INTERNAL;
	}
	std::abort();
}

// Async save-data events (SceSaveDataEvent is 0x60 bytes).
struct SaveDataEvent
{
	uint32_t type;
	int32_t  error_code;
	int32_t  user_id;
	uint8_t  reserved[4];
	char     dir_name[32];
	uint8_t  padding[0x60 - 0x30];
};

static std::mutex                g_event_mutex;
static std::deque<SaveDataEvent> g_events;

static void EnqueueSaveDataEvent(uint32_t type, int32_t user_id, const char* dir_name = "", int32_t error_code = 0)
{
	SaveDataEvent event {};
	event.type       = type;
	event.error_code = error_code;
	event.user_id    = user_id;
	if (dir_name != nullptr)
	{
		std::snprintf(event.dir_name, sizeof(event.dir_name), "%s", dir_name);
	}
	std::lock_guard lock(g_event_mutex);
	g_events.push_back(event);
}

// sceSaveDataGetEventResult — polled after async save operations.
int KYTY_SYSV_ABI SaveDataGetEventResult(const void* /*event_param*/, void* event)
{
	PRINT_NAME();

	if (event == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	SaveDataEvent pending {};
	{
		std::lock_guard lock(g_event_mutex);
		if (g_events.empty())
		{
			return SAVE_DATA_ERROR_NOT_FOUND;
		}
		pending = g_events.front();
		g_events.pop_front();
	}

	std::memset(event, 0, 0x60);
	auto* out                                = static_cast<uint8_t*>(event);
	*reinterpret_cast<uint32_t*>(out + 0x00) = pending.type;
	*reinterpret_cast<int32_t*>(out + 0x04)  = pending.error_code;
	*reinterpret_cast<int32_t*>(out + 0x08)  = pending.user_id;
	std::memcpy(out + 0x10, pending.dir_name, sizeof(pending.dir_name));
	return OK;
}

// sceSaveDataSyncSaveDataMemory (NID wiT9jeC7xPw).
struct SaveDataMemorySync
{
	int32_t  user_id;
	uint32_t slot_id;
};
static_assert(sizeof(SaveDataMemorySync) == 8);
static_assert(offsetof(SaveDataMemorySync, slot_id) == 4);

int KYTY_SYSV_ABI SaveDataSyncSaveDataMemory(const SaveDataMemorySync* sync)
{
	PRINT_NAME();

	if (sync == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	std::filesystem::path path;
	if (!ResolveSaveDataMemorySlot(sync->user_id, sync->slot_id, &path))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	printf("\t user_id = %d slot=%u\n", sync->user_id, sync->slot_id);

	const auto result = g_save_memory_store.Sync(path);
	if (result != SaveDataMemoryStoreResult::Success)
	{
		return SaveDataMemoryError(result);
	}

	// Publish completion only after the complete slot has reached its backing file.
	EnqueueSaveDataEvent(3u, sync->user_id);
	return OK;
}

// sceSaveDataGetProgress — 8-byte struct; progress float at +0x00 (1.0 = complete).
int KYTY_SYSV_ABI SaveDataGetProgress(void* progress)
{
	PRINT_NAME();

	if (progress != nullptr)
	{
		std::memset(progress, 0, 8);
		*static_cast<float*>(progress) = 1.0f;
	}
	return OK;
}

int KYTY_SYSV_ABI SaveDataClearProgress()
{
	PRINT_NAME();
	return OK;
}

struct SaveDataPrepareParam
{
	int32_t  resource;
	uint32_t prepare_mode;
	uint8_t  reserved[32];
};

struct SaveDataCommitParam
{
	int32_t  resource;
	uint32_t commit_mode;
	uint8_t  reserved[32];
};

int KYTY_SYSV_ABI SaveDataPrepare(const SaveDataMountPoint* mount_point, const SaveDataPrepareParam* param)
{
	PRINT_NAME();

	if (mount_point == nullptr || param == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	printf("\t mount_point  = %s\n", mount_point->data);
	printf("\t resource     = %d\n", param->resource);
	printf("\t prepare_mode = %u\n", param->prepare_mode);
	return OK;
}

// sceSaveDataCommit — completes a save transaction descriptor supplied by the guest.
int KYTY_SYSV_ABI SaveDataCommit(const SaveDataCommitParam* commit_param)
{
	PRINT_NAME();

	if (commit_param == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	printf("\t resource    = %d\n", commit_param->resource);
	printf("\t commit_mode = %u\n", commit_param->commit_mode);
	if ((commit_param->commit_mode & 1u) != 0)
	{
		EnqueueSaveDataEvent(SAVE_DATA_EVENT_TYPE_COMMIT_BACKUP_END, 0);
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

// sceSaveData*SaveDataMemory2 guest param layouts (Gen5).
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

static_assert(sizeof(SaveDataMemoryData) == 64);
static_assert(offsetof(SaveDataMemoryData, offset) == 16);
static_assert(sizeof(SaveDataMemoryGet2) == 64);
static_assert(offsetof(SaveDataMemoryGet2, slot_id) == 32);
static_assert(sizeof(SaveDataMemorySetup2) == 64);
static_assert(offsetof(SaveDataMemorySetup2, slot_id) == 40);
static_assert(sizeof(SaveDataMemorySet2) == 64);
static_assert(offsetof(SaveDataMemorySet2, data_num) == 32);
static_assert(offsetof(SaveDataMemorySet2, slot_id) == 36);

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

	std::filesystem::path path;
	if (!ResolveSaveDataMemorySlot(setup->user_id, setup->slot_id, &path))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	size_t     existed_size = 0;
	const auto status       = g_save_memory_store.Setup(path, setup->memory_size, &existed_size);
	if (status != SaveDataMemoryStoreResult::Success)
	{
		return SaveDataMemoryError(status);
	}
	if (result_out != nullptr)
	{
		auto* result                = static_cast<SaveDataMemorySetupResult*>(result_out);
		result->existed_memory_size = existed_size;
		std::memset(result->reserved, 0, sizeof(result->reserved));
	}
	return OK;
}

// sceSaveDataGetSaveDataMemory2 (NID QwOO7vegnV8)
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

	std::filesystem::path path;
	if (!ResolveSaveDataMemorySlot(get->user_id, get->slot_id, &path))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	SaveDataMemoryStoreResult status = SaveDataMemoryStoreResult::Success;
	if (get->data != nullptr)
	{
		status = g_save_memory_store.Read(path, get->data->buf, get->data->buf_size, get->data->offset);
	} else
	{
		status = g_save_memory_store.Read(path, nullptr, 0, 0);
	}
	if (status != SaveDataMemoryStoreResult::Success)
	{
		return SaveDataMemoryError(status);
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
	printf("\t user_id=%d data=%p data_num=%u slot=%u\n", set->user_id, static_cast<const void*>(set->data), set->data_num, set->slot_id);

	std::filesystem::path path;
	if (!ResolveSaveDataMemorySlot(set->user_id, set->slot_id, &path) || set->data_num > SaveDataMemoryStore::MaximumWriteRanges() ||
	    (set->data == nullptr && set->data_num != 0))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	std::vector<SaveDataMemoryWriteRange> ranges;
	ranges.reserve(set->data_num);
	for (uint32_t index = 0; index < set->data_num; ++index)
	{
		const auto& data = set->data[index];
		ranges.push_back({data.buf, data.buf_size, data.offset});
	}
	return SaveDataMemoryError(g_save_memory_store.WriteRanges(path, ranges.data(), ranges.size()));
}

int KYTY_SYSV_ABI SaveDataTerminate()
{
	PRINT_NAME();
	return OK;
}

int KYTY_SYSV_ABI SaveDataAbort()
{
	PRINT_NAME();
	return OK;
}

int KYTY_SYSV_ABI SaveDataIsMounted(uint32_t* mounted)
{
	PRINT_NAME();
	if (mounted == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	*mounted = 1;
	return OK;
}

int KYTY_SYSV_ABI SaveDataGetParam(const SaveDataMountPoint* mount_point, uint32_t param_type, void* param_buf, size_t param_buf_size)
{
	PRINT_NAME();
	if (mount_point == nullptr || param_buf == nullptr)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	printf("\t mount_point    = %s\n", mount_point->data);
	printf("\t param_type     = %u\n", param_type);
	printf("\t param_buf_size = %" PRIu64 "\n", static_cast<uint64_t>(param_buf_size));
	std::memset(param_buf, 0, param_buf_size);
	return OK;
}

} // namespace SaveData

namespace {

constexpr LibraryIdentity SAVE_DATA        = {"SaveData", 1, "SaveData", 1, 1};
constexpr LibraryIdentity SAVE_DATA_NATIVE = {"SaveData_native", 1, "SaveData_native", 1, 1};

// This is the single canonical export list shared by SaveData and
// SaveData_native. Native-only exports are registered separately below so an
// exact lookup never succeeds through a compatibility alias or fallback.
void RegisterSaveDataFunctions(Loader::SymbolDatabase* symbols, const LibraryIdentity& identity)
{
	using namespace SaveData;
	RegisterLibraryFunction(symbols, identity, "ZkZhskCPXFw", SaveDataInitialize, U"SaveData::SaveDataInitialize");
	RegisterLibraryFunction(symbols, identity, "l1NmDeDpNGU", SaveDataInitialize2, U"SaveData::SaveDataInitialize2");
	RegisterLibraryFunction(symbols, identity, "TywrFKCoLGY", SaveDataInitialize3, U"SaveData::SaveDataInitialize3");
	RegisterLibraryFunction(symbols, identity, "gjRZNnw0JPE", SaveDataCreateTransactionResource,
	                        U"SaveData::SaveDataCreateTransactionResource");
	RegisterLibraryFunction(symbols, identity, "32HQAQdwM2o", SaveDataMount, U"SaveData::SaveDataMount");
	RegisterLibraryFunction(symbols, identity, "0z45PIH+SNI", SaveDataMount2, U"SaveData::SaveDataMount2");
	RegisterLibraryFunction(symbols, identity, "ZP4e7rlzOUk", SaveDataMount3, U"SaveData::SaveDataMount3");
	RegisterLibraryFunction(symbols, identity, "BMR4F-Uek3E", SaveDataUmount, U"SaveData::SaveDataUmount");
	RegisterLibraryFunction(symbols, identity, "85zul--eGXs", SaveDataSetParam, U"SaveData::SaveDataSetParam");
	RegisterLibraryFunction(symbols, identity, "65VH0Qaaz6s", SaveDataGetMountInfo, U"SaveData::SaveDataGetMountInfo");
	RegisterLibraryFunction(symbols, identity, "dyIhnXq-0SM", SaveDataDirNameSearch, U"SaveData::SaveDataDirNameSearch");
	RegisterLibraryFunction(symbols, identity, "j8xKtiFj0SY", SaveDataGetEventResult, U"SaveData::SaveDataGetEventResult");
	RegisterLibraryFunction(symbols, identity, "ie7qhZ4X0Cc", SaveDataCommit, U"SaveData::SaveDataCommit");
	RegisterLibraryFunction(symbols, identity, "c88Yy54Mx0w", SaveDataSaveIcon, U"SaveData::SaveDataSaveIcon");
	RegisterLibraryFunction(symbols, identity, "oQySEUfgXRA", SaveDataSetupSaveDataMemory2, U"SaveData::SaveDataSetupSaveDataMemory2");
	RegisterLibraryFunction(symbols, identity, "QwOO7vegnV8", SaveDataGetSaveDataMemory2, U"SaveData::SaveDataGetSaveDataMemory2");
	RegisterLibraryFunction(symbols, identity, "cduy9v4YmT4", SaveDataSetSaveDataMemory2, U"SaveData::SaveDataSetSaveDataMemory2");
	RegisterLibraryFunction(symbols, identity, "wiT9jeC7xPw", SaveDataSyncSaveDataMemory, U"SaveData::SaveDataSyncSaveDataMemory");
	RegisterLibraryFunction(symbols, identity, "ANmSWUiyyGQ", SaveDataGetProgress, U"SaveData::SaveDataGetProgress");
	RegisterLibraryFunction(symbols, identity, "Wz-4JZfeO9g", SaveDataClearProgress, U"SaveData::SaveDataClearProgress");
	RegisterLibraryFunction(symbols, identity, "yKDy8S5yLA0", SaveDataTerminate, U"SaveData::SaveDataTerminate");
	RegisterLibraryFunction(symbols, identity, "dQ2GohUHXzk", SaveDataAbort, U"SaveData::SaveDataAbort");
	RegisterLibraryFunction(symbols, identity, "ieP6jP138Qo", SaveDataIsMounted, U"SaveData::SaveDataIsMounted");
	RegisterLibraryFunction(symbols, identity, "XgvSuIdnMlw", SaveDataGetParam, U"SaveData::SaveDataGetParam");
	RegisterLibraryFunction(symbols, identity, "lJUQuaKqoKY", SaveDataDeleteTransactionResource,
	                        U"SaveData::SaveDataDeleteTransactionResource");
	RegisterLibraryFunction(symbols, identity, "WAzWTZm1H+I", SaveDataTransferringMount, U"SaveData::SaveDataTransferringMount");
	RegisterLibraryFunction(symbols, identity, "RjMlsR8EXrw", SaveDataTransferringMount, U"SaveData::SaveDataTransferringMount");
}

void RegisterNativeSaveDataFunctions(Loader::SymbolDatabase* symbols)
{
	RegisterSaveDataFunctions(symbols, SAVE_DATA_NATIVE);
	RegisterLibraryFunction(symbols, SAVE_DATA_NATIVE, "sDCBrmc61XU", SaveData::SaveDataPrepare, U"SaveData::SaveDataPrepare");
	RegisterLibraryFunction(symbols, SAVE_DATA_NATIVE, "uW4vfTwMQVo", SaveData::SaveDataUmount2, U"SaveData::SaveDataUmount2");
	RegisterLibraryFunction(symbols, SAVE_DATA_NATIVE, "X4MYzukPc3g", SaveData::SaveDataDirNameSearch,
	                        U"SaveData::SaveDataDirNameSearch");
}

} // namespace

LIB_DEFINE(InitSaveData_1)
{
	RegisterSaveDataFunctions(s, SAVE_DATA);
}

namespace SaveDataNative {

LIB_VERSION("SaveData_native", 1, "SaveData_native", 1, 1);

LIB_DEFINE(InitSaveDataNative_1)
{
	RegisterNativeSaveDataFunctions(s);
}

} // namespace SaveDataNative

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
