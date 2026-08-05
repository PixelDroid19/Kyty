#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Dialog.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/SaveData.h"
#include "Emulator/Libs/SaveDataMemoryStore.h"
#include "Emulator/Libs/SaveDataPaths.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

UT_BEGIN(EmulatorSaveData);

TEST(EmulatorSaveData, BuildsPortablePerTitleSaveRoots)
{
	const auto root = Libs::SaveData::SaveDataBuildTitleRoot(std::filesystem::path("/portable/user/savedata"), "ppsa-12345");
	EXPECT_EQ(root, std::filesystem::path("/portable/user/savedata/PPSA-12345"));
	EXPECT_EQ(Libs::SaveData::SaveDataBuildTitleRoot(std::filesystem::path("/portable/user/savedata"), nullptr),
	          std::filesystem::path("/portable/user/savedata/UNKNOWN"));
	EXPECT_EQ(Libs::SaveData::SaveDataBuildMemoryPath(root, 7, 3),
	          std::filesystem::path("/portable/user/savedata/PPSA-12345/memory/user-7/slot-3.bin"));
	EXPECT_TRUE(Libs::SaveData::SaveDataBuildMemoryPath(root, -1, 3).empty());
	EXPECT_TRUE(Libs::SaveData::SaveDataBuildMemoryPath(std::filesystem::path("relative"), 7, 3).empty());
}

TEST(EmulatorSaveData, RejectsSaveSlotPathTraversal)
{
	using Libs::SaveData::SaveDataDirectoryNameValid;
	EXPECT_TRUE(SaveDataDirectoryNameValid("slot-01"));
	EXPECT_FALSE(SaveDataDirectoryNameValid(nullptr));
	EXPECT_FALSE(SaveDataDirectoryNameValid(""));
	EXPECT_FALSE(SaveDataDirectoryNameValid(".."));
	EXPECT_FALSE(SaveDataDirectoryNameValid("../escape"));
	EXPECT_FALSE(SaveDataDirectoryNameValid("nested/slot"));
	EXPECT_FALSE(SaveDataDirectoryNameValid("windows\\slot"));
}

namespace {

class ScopedSaveDataMemoryDirectory final
{
public:
	ScopedSaveDataMemoryDirectory()
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		path             = std::filesystem::temp_directory_path() / ("kyty-savedata-memory-test-" + std::to_string(nonce));
	}

	~ScopedSaveDataMemoryDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path path;
};

class ScopedSaveDataRoot final
{
public:
	explicit ScopedSaveDataRoot(const std::filesystem::path& root)
	{
		if (const char* previous = std::getenv("KYTY_SAVEDATA_DIR"); previous != nullptr)
		{
			m_previous     = previous;
			m_had_previous = true;
		}
#if defined(_WIN32)
		_putenv_s("KYTY_SAVEDATA_DIR", root.string().c_str());
#else
		setenv("KYTY_SAVEDATA_DIR", root.string().c_str(), 1);
#endif
	}

	~ScopedSaveDataRoot()
	{
#if defined(_WIN32)
		_putenv_s("KYTY_SAVEDATA_DIR", m_had_previous ? m_previous.c_str() : "");
#else
		if (m_had_previous)
		{
			setenv("KYTY_SAVEDATA_DIR", m_previous.c_str(), 1);
		} else
		{
			unsetenv("KYTY_SAVEDATA_DIR");
		}
#endif
	}

private:
	std::string m_previous;
	bool        m_had_previous = false;
};

Kyty::Loader::SymbolResolve SaveDataNativeFunc(const char16_t* nid)
{
	Kyty::Loader::SymbolResolve sr {};
	sr.name                 = nid;
	sr.library              = U"SaveData_native";
	sr.library_version      = 1;
	sr.module               = U"SaveData_native";
	sr.module_version_major = 1;
	sr.module_version_minor = 1;
	sr.type                 = Kyty::Loader::SymbolType::Func;
	return sr;
}

Kyty::Loader::SymbolResolve SaveDataFunc(const char16_t* nid)
{
	auto sr    = SaveDataNativeFunc(nid);
	sr.library = U"SaveData";
	sr.module  = U"SaveData";
	return sr;
}

void EnsureLogSubsystem()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

void EnsureFileSystemSubsystem()
{
	static bool initialized = false;
	if (!initialized)
	{
		EnsureLogSubsystem();
		Kernel::FileSystem::FileSystemSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		initialized = true;
	}
}

struct SaveDataMount2Layout
{
	int32_t                                user_id;
	int32_t                                pad;
	const Libs::SaveData::SaveDataDirName* dir_name;
	uint64_t                               blocks;
	uint32_t                               mount_mode;
	uint8_t                                reserved[32];
	int32_t                                pad2;
};

struct SaveDataMountResultLayout
{
	Libs::SaveData::SaveDataMountPoint mount_point;
	uint64_t                           required_blocks;
	uint32_t                           unused;
	uint32_t                           mount_status;
	uint8_t                            reserved[28];
	int32_t                            pad;
};

struct SaveDataMemoryDataLayout
{
	void*   buf;
	size_t  buf_size;
	int64_t offset;
	uint8_t reserved[40];
};

struct SaveDataMemoryGet2Layout
{
	int32_t                   user_id;
	uint8_t                   padding[4];
	SaveDataMemoryDataLayout* data;
	void*                     param;
	void*                     icon;
	uint32_t                  slot_id;
	uint8_t                   reserved[28];
};

struct SaveDataMemorySetup2Layout
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

struct SaveDataMemorySetupResultLayout
{
	size_t  existed_memory_size;
	uint8_t reserved[16];
};

struct SaveDataMemorySet2Layout
{
	int32_t                         user_id;
	uint8_t                         padding[4];
	const SaveDataMemoryDataLayout* data;
	const void*                     param;
	const void*                     icon;
	uint32_t                        data_num;
	uint32_t                        slot_id;
	uint8_t                         reserved[24];
};

struct SaveDataMemorySyncLayout
{
	int32_t  user_id;
	uint32_t slot_id;
};

static_assert(sizeof(SaveDataMemoryDataLayout) == 64);
static_assert(sizeof(SaveDataMemoryGet2Layout) == 64);
static_assert(sizeof(SaveDataMemorySetup2Layout) == 64);
static_assert(sizeof(SaveDataMemorySet2Layout) == 64);
static_assert(sizeof(SaveDataMemorySyncLayout) == 8);

} // namespace

TEST(EmulatorSaveData, SaveDataMemoryPersistsAndKeepsSlotsIsolated)
{
	using namespace Libs::SaveData;

	ScopedSaveDataMemoryDirectory directory;
	const auto                    title_root = SaveDataBuildTitleRoot(directory.path, "PPSA-90001");
	const auto                    first_path = SaveDataBuildMemoryPath(title_root, 1, 0);
	const auto                    other_path = SaveDataBuildMemoryPath(title_root, 1, 1);

	SaveDataMemoryStore first_process;
	size_t              existed_size = 99;
	EXPECT_EQ(first_process.Setup(first_path, 8, &existed_size), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(existed_size, 0u);
	const std::array<uint8_t, 3> payload {0x21, 0x43, 0x65};
	EXPECT_EQ(first_process.Write(first_path, payload.data(), payload.size(), 2), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(first_process.Sync(first_path), SaveDataMemoryStoreResult::Success);

	SaveDataMemoryStore second_process;
	EXPECT_EQ(second_process.Setup(first_path, 8, &existed_size), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(existed_size, 8u);
	std::array<uint8_t, 8> restored {};
	EXPECT_EQ(second_process.Read(first_path, restored.data(), restored.size(), 0), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(restored, (std::array<uint8_t, 8> {0, 0, 0x21, 0x43, 0x65, 0, 0, 0}));

	EXPECT_EQ(second_process.Setup(other_path, 8, &existed_size), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(existed_size, 0u);
	std::array<uint8_t, 8> isolated {};
	EXPECT_EQ(second_process.Read(other_path, isolated.data(), isolated.size(), 0), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(isolated, (std::array<uint8_t, 8> {}));
}

TEST(EmulatorSaveData, SaveDataMemoryRequiresSetupAndExactRanges)
{
	using namespace Libs::SaveData;

	ScopedSaveDataMemoryDirectory directory;
	const auto                    path = SaveDataBuildMemoryPath(SaveDataBuildTitleRoot(directory.path, "PPSA-90002"), 2, 4);
	SaveDataMemoryStore           store;
	uint8_t                       byte         = 0x7f;
	size_t                        existed_size = 0;

	EXPECT_EQ(store.Read(path, &byte, 1, 0), SaveDataMemoryStoreResult::NotReady);
	EXPECT_EQ(store.Write(path, &byte, 1, 0), SaveDataMemoryStoreResult::NotReady);
	EXPECT_EQ(store.Sync(path), SaveDataMemoryStoreResult::NotReady);
	EXPECT_EQ(store.Setup(path, SaveDataMemoryStore::MaximumSlotBytes() + 1, &existed_size), SaveDataMemoryStoreResult::CapacityExceeded);
	EXPECT_EQ(store.Setup(path, 4, &existed_size), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(store.Read(path, &byte, 1, -1), SaveDataMemoryStoreResult::InvalidArgument);
	EXPECT_EQ(store.Read(path, &byte, 2, 3), SaveDataMemoryStoreResult::InvalidArgument);
	EXPECT_EQ(store.Write(path, &byte, 2, 3), SaveDataMemoryStoreResult::InvalidArgument);
	const std::array<SaveDataMemoryWriteRange, 2> invalid_batch {{{&byte, 1, 0}, {&byte, 2, 3}}};
	EXPECT_EQ(store.WriteRanges(path, invalid_batch.data(), invalid_batch.size()), SaveDataMemoryStoreResult::InvalidArgument);
	std::array<uint8_t, 4> unchanged {};
	EXPECT_EQ(store.Read(path, unchanged.data(), unchanged.size(), 0), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(unchanged, (std::array<uint8_t, 4> {}));
	EXPECT_EQ(store.Read(path, nullptr, 0, 4), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(store.Write(path, nullptr, 0, 4), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(store.Setup(std::filesystem::path("relative/slot.bin"), 4, &existed_size), SaveDataMemoryStoreResult::InvalidArgument);
}

TEST(EmulatorSaveData, SaveDataMemoryHleUsesExactSlotAndPersistsBeforeEvent)
{
	using Setup = int(KYTY_SYSV_ABI*)(void*, void*);
	using Get   = int(KYTY_SYSV_ABI*)(void*);
	using Set   = int(KYTY_SYSV_ABI*)(void*);
	using Sync  = int(KYTY_SYSV_ABI*)(const SaveDataMemorySyncLayout*);

	using namespace Libs::SaveData;
	EnsureLogSubsystem();
	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSaveData_1", &symbols));
	const auto* setup_record = symbols.Find(SaveDataFunc(u"oQySEUfgXRA"));
	const auto* get_record   = symbols.Find(SaveDataFunc(u"QwOO7vegnV8"));
	const auto* set_record   = symbols.Find(SaveDataFunc(u"cduy9v4YmT4"));
	const auto* sync_record  = symbols.Find(SaveDataFunc(u"wiT9jeC7xPw"));
	ASSERT_NE(setup_record, nullptr);
	ASSERT_NE(get_record, nullptr);
	ASSERT_NE(set_record, nullptr);
	ASSERT_NE(sync_record, nullptr);
	auto* setup = reinterpret_cast<Setup>(setup_record->vaddr);
	auto* get   = reinterpret_cast<Get>(get_record->vaddr);
	auto* set   = reinterpret_cast<Set>(set_record->vaddr);
	auto* sync  = reinterpret_cast<Sync>(sync_record->vaddr);

	ScopedSaveDataMemoryDirectory directory;
	ScopedSaveDataRoot            save_root(directory.path);
	constexpr int32_t             k_user = 1701;
	constexpr uint32_t            k_slot = 23;
	SaveDataMemoryGet2Layout      before_setup {};
	before_setup.user_id = k_user;
	before_setup.slot_id = k_slot;
	EXPECT_EQ(get(&before_setup), Libs::SaveData::SAVE_DATA_ERROR_MEMORY_NOT_READY);

	SaveDataMemorySetup2Layout setup_param {};
	setup_param.user_id     = k_user;
	setup_param.memory_size = 8;
	setup_param.slot_id     = k_slot;
	SaveDataMemorySetupResultLayout setup_result {};
	EXPECT_EQ(setup(&setup_param, &setup_result), 0);
	EXPECT_EQ(setup_result.existed_memory_size, 0u);

	std::array<uint8_t, 3>   payload {0x91, 0x82, 0x73};
	SaveDataMemoryDataLayout write_data {};
	write_data.buf      = payload.data();
	write_data.buf_size = payload.size();
	write_data.offset   = 1;
	SaveDataMemorySet2Layout set_param {};
	set_param.user_id  = k_user;
	set_param.data     = &write_data;
	set_param.data_num = 1;
	set_param.slot_id  = k_slot;
	EXPECT_EQ(set(&set_param), 0);

	std::array<uint8_t, 8>   round_trip {};
	SaveDataMemoryDataLayout read_data {};
	read_data.buf      = round_trip.data();
	read_data.buf_size = round_trip.size();
	SaveDataMemoryGet2Layout get_param {};
	get_param.user_id = k_user;
	get_param.data    = &read_data;
	get_param.slot_id = k_slot;
	EXPECT_EQ(get(&get_param), 0);
	EXPECT_EQ(round_trip, (std::array<uint8_t, 8> {0, 0x91, 0x82, 0x73, 0, 0, 0, 0}));

	SaveDataMemorySyncLayout sync_param {k_user, k_slot};
	EXPECT_EQ(sync(&sync_param), 0);
	uint8_t event[0x60] = {};
	EXPECT_EQ(SaveDataGetEventResult(nullptr, event), 0);
	EXPECT_EQ(*reinterpret_cast<const uint32_t*>(event), 3u);

	const auto          backing_path = SaveDataBuildMemoryPath(SaveDataBuildTitleRoot(directory.path, nullptr), k_user, k_slot);
	SaveDataMemoryStore restarted;
	size_t              existed_size = 0;
	EXPECT_EQ(restarted.Setup(backing_path, 8, &existed_size), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(existed_size, 8u);
	std::array<uint8_t, 8> persisted {};
	EXPECT_EQ(restarted.Read(backing_path, persisted.data(), persisted.size(), 0), SaveDataMemoryStoreResult::Success);
	EXPECT_EQ(persisted, round_trip);
}

TEST(EmulatorSaveData, StandardAndNativeLibrariesShareOneExactExportContract)
{
	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSaveData_1", &symbols));

	const char16_t* shared_nids[] = {
	    u"ZkZhskCPXFw", u"l1NmDeDpNGU", u"TywrFKCoLGY", u"gjRZNnw0JPE", u"32HQAQdwM2o", u"0z45PIH+SNI", u"ZP4e7rlzOUk",
	    u"BMR4F-Uek3E", u"85zul--eGXs", u"65VH0Qaaz6s", u"dyIhnXq-0SM", u"j8xKtiFj0SY", u"ie7qhZ4X0Cc", u"c88Yy54Mx0w",
	    u"oQySEUfgXRA", u"QwOO7vegnV8", u"cduy9v4YmT4", u"wiT9jeC7xPw", u"ANmSWUiyyGQ", u"Wz-4JZfeO9g", u"yKDy8S5yLA0",
	    u"dQ2GohUHXzk", u"ieP6jP138Qo", u"XgvSuIdnMlw", u"lJUQuaKqoKY", u"WAzWTZm1H+I", u"RjMlsR8EXrw",
	};
	for (const auto* nid: shared_nids)
	{
		const auto* standard = symbols.Find(SaveDataFunc(nid));
		const auto* native   = symbols.Find(SaveDataNativeFunc(nid));
		ASSERT_NE(standard, nullptr) << String::FromUtf16(nid).C_Str();
		ASSERT_NE(native, nullptr) << String::FromUtf16(nid).C_Str();
		EXPECT_EQ(standard->vaddr, native->vaddr) << String::FromUtf16(nid).C_Str();
	}

	EXPECT_EQ(symbols.Find(SaveDataFunc(u"sDCBrmc61XU")), nullptr);
	EXPECT_EQ(symbols.Find(SaveDataFunc(u"uW4vfTwMQVo")), nullptr);
	EXPECT_NE(symbols.Find(SaveDataNativeFunc(u"sDCBrmc61XU")), nullptr);
	EXPECT_NE(symbols.Find(SaveDataNativeFunc(u"uW4vfTwMQVo")), nullptr);
}

TEST(EmulatorSaveData, CreatesTransactionResourceThroughReturnValue)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	const auto first  = Libs::SaveData::SaveDataCreateTransactionResource(0);
	const auto second = Libs::SaveData::SaveDataCreateTransactionResource(0);

	EXPECT_GT(first, 0);
	EXPECT_GT(second, first);
}

TEST(EmulatorSaveData, SaveDataDialogInitializeRequiresCommonDialog)
{
	using namespace Libs::Dialog;

	// Alphabetically early within the suite when process is fresh: common dialog
	// may already be initialized by other suites in the same process. Exercise
	// the documented contract that Initialize succeeds once system init is up.
	EXPECT_EQ(CommonDialog::CommonDialogInitialize(), 0);
	// Second call is already-system-initialized.
	EXPECT_EQ(CommonDialog::CommonDialogInitialize(), CommonDialog::ERROR_ALREADY_SYSTEM_INITIALIZED);

	EXPECT_EQ(SaveDataDialog::SaveDataDialogInitialize(), 0);
	EXPECT_EQ(SaveDataDialog::SaveDataDialogGetStatus(), CommonDialog::STATUS_INITIALIZED);
	EXPECT_EQ(SaveDataDialog::SaveDataDialogUpdateStatus(), CommonDialog::STATUS_INITIALIZED);
	EXPECT_EQ(SaveDataDialog::SaveDataDialogInitialize(), CommonDialog::ERROR_ALREADY_INITIALIZED);

	EXPECT_EQ(SaveDataDialog::SaveDataDialogOpen(nullptr), CommonDialog::ERROR_ARG_NULL);

	SaveDataDialog::SaveDataDialogParam param {};
	param.base_size = 0x30;
	param.mode      = 4; // ERROR_CODE mode observed at the frontier
	param.size      = 0x98;
	param.disp_type = 1;
	EXPECT_EQ(SaveDataDialog::SaveDataDialogOpen(&param), 0);
	EXPECT_EQ(SaveDataDialog::SaveDataDialogUpdateStatus(), CommonDialog::STATUS_FINISHED);

	EXPECT_EQ(SaveDataDialog::SaveDataDialogTerminate(), 0);
	EXPECT_EQ(SaveDataDialog::SaveDataDialogUpdateStatus(), CommonDialog::STATUS_NONE);
	EXPECT_EQ(SaveDataDialog::SaveDataDialogTerminate(), CommonDialog::ERROR_NOT_INITIALIZED);
}

TEST(EmulatorSaveData, GetMountInfoValidatesAndReportsCapacity)
{
	using namespace Libs::SaveData;

	SaveDataMountInfo info {};
	EXPECT_EQ(SaveDataGetMountInfo(nullptr, &info), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
	EXPECT_EQ(SaveDataGetMountInfo(reinterpret_cast<const SaveDataMountPoint*>("/savedata0"), nullptr),
	          Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);

	SaveDataMountPoint mount {};
	std::memcpy(mount.data, "/savedata0", 11);
	EXPECT_EQ(SaveDataGetMountInfo(&mount, &info), 0);
	EXPECT_GT(info.blocks, 0u);
	EXPECT_GT(info.free_blocks, 0u);
	EXPECT_LE(info.free_blocks, info.blocks);
}

TEST(EmulatorSaveData, Mount2CreatesMissingDirectoryForCreateIfMissingMode)
{
	using Mount2 = int(KYTY_SYSV_ABI*)(const SaveDataMount2Layout*, SaveDataMountResultLayout*);

	EnsureLogSubsystem();
	EnsureFileSystemSubsystem();
	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSaveData_1", &symbols));
	const auto* record = symbols.Find(SaveDataNativeFunc(u"0z45PIH+SNI"));
	ASSERT_NE(record, nullptr);

	auto* mount2 = reinterpret_cast<Mount2>(record->vaddr);
	ASSERT_NE(mount2, nullptr);

	constexpr char     kDirectory[] = "ut-mount2-cim";
	const auto         save_root    = std::filesystem::temp_directory_path() / "kyty-savedata-mount2-test";
	ScopedSaveDataRoot scoped_root(save_root);
	const auto         host_path      = Libs::SaveData::SaveDataBuildTitleRoot(save_root, nullptr) / kDirectory;
	const auto         host_utf8      = host_path.u8string();
	const String       host_directory = String::FromUtf8(host_utf8.c_str());
	Core::File::DeleteDirectories(host_directory);

	Libs::SaveData::SaveDataDirName dir_name {};
	std::memcpy(dir_name.data, kDirectory, sizeof(kDirectory));
	SaveDataMount2Layout mount {};
	mount.user_id    = 0;
	mount.dir_name   = &dir_name;
	mount.mount_mode = 0x30;
	EXPECT_EXIT(
	    {
		    SaveDataMountResultLayout result {};
		    const int                 mount_result = mount2(&mount, &result);
		    if (mount_result != 0 || result.mount_status != 1u || std::strcmp(result.mount_point.data, "/savedata0") != 0)
		    {
			    std::_Exit(1);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
	EXPECT_TRUE(Core::File::IsDirectoryExisting(host_directory));

	Core::File::DeleteDirectories(host_directory);
}

TEST(EmulatorSaveData, GetEventResultReportsEmptyQueue)
{
	using namespace Libs::SaveData;

	uint8_t event[128] = {};
	EXPECT_EQ(SaveDataGetEventResult(nullptr, nullptr), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
	EXPECT_EQ(SaveDataGetEventResult(nullptr, event), Libs::SaveData::SAVE_DATA_ERROR_NOT_FOUND);
}

TEST(EmulatorSaveData, CommitNativeNidValidatesPointerAndSucceeds)
{
	using CommitFn = int(KYTY_SYSV_ABI*)(const void*);

	EnsureLogSubsystem();

	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSaveData_1", &symbols));
	const Kyty::Loader::SymbolRecord* commit = symbols.Find(SaveDataNativeFunc(u"ie7qhZ4X0Cc"));
	ASSERT_NE(commit, nullptr);
	auto* commit_fn = reinterpret_cast<CommitFn>(commit->vaddr);
	ASSERT_NE(commit_fn, nullptr);

	uint8_t commit_param[32] = {};
	EXPECT_EQ(commit_fn(nullptr), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
	EXPECT_EQ(commit_fn(commit_param), 0);
}

TEST(EmulatorSaveData, DeleteTransactionResourceTracksCreateHandles)
{
	using namespace Libs::SaveData;

	EXPECT_EQ(SaveDataDeleteTransactionResource(0), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
	EXPECT_EQ(SaveDataDeleteTransactionResource(-1), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);

	// Unknown handle: guest-visible NOT_FOUND (not success).
	EXPECT_EQ(SaveDataDeleteTransactionResource(99999), Libs::SaveData::SAVE_DATA_ERROR_NOT_FOUND);

	const int32_t resource = SaveDataCreateTransactionResource(1);
	EXPECT_GT(resource, 0);
	EXPECT_EQ(SaveDataDeleteTransactionResource(resource), 0);
	// Double-delete must not invent success.
	EXPECT_EQ(SaveDataDeleteTransactionResource(resource), Libs::SaveData::SAVE_DATA_ERROR_NOT_FOUND);
}

// sceSaveDataTransferringMount rejects null mount/result at the HLE boundary.
TEST(EmulatorSaveData, TransferringMountRejectsNull)
{
	using namespace Libs::SaveData;

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	EXPECT_EQ(SaveDataTransferringMount(nullptr, nullptr), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
}

TEST(EmulatorSaveData, DirNameSearchValidatesAndReportsHits)
{
	using namespace Libs::SaveData;

	SaveDataDirNameSearchResult result {};
	EXPECT_EQ(SaveDataDirNameSearch(nullptr, &result), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
	EXPECT_EQ(SaveDataDirNameSearch(reinterpret_cast<const SaveDataDirNameSearchCond*>(0x1), nullptr),
	          Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);

	SaveDataDirNameSearchCond cond {};
	cond.user_id = 1;
	cond.key     = 0;
	cond.order   = 0;
	// Invalid sort key/order must fail with PARAMETER.
	cond.key = 99;
	EXPECT_EQ(SaveDataDirNameSearch(&cond, &result), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
	cond.key   = 0;
	cond.order = 9;
	EXPECT_EQ(SaveDataDirNameSearch(&cond, &result), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
	cond.order = 0;

	SaveDataDirName names[4] = {};
	result.dir_names         = names;
	result.dir_names_num     = 4;
	// The portable per-title root may or may not exist; contract is success with set_num
	// capped by dir_names_num and hit_num >= set_num.
	EXPECT_EQ(SaveDataDirNameSearch(&cond, &result), 0);
	EXPECT_LE(result.set_num, result.dir_names_num);
	EXPECT_GE(result.hit_num, result.set_num);

	// Filter for a name that cannot exist: exact match yields empty hits.
	SaveDataDirName filter {};
	std::memcpy(filter.data, "__kyty_no_such_save__", 21);
	cond.dir_name  = &filter;
	result.hit_num = 99;
	result.set_num = 99;
	EXPECT_EQ(SaveDataDirNameSearch(&cond, &result), 0);
	EXPECT_EQ(result.hit_num, 0u);
	EXPECT_EQ(result.set_num, 0u);
}

UT_END();
