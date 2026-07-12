#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Dialog.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/SaveData.h"
#include "Emulator/Log.h"

#include <cstring>

UT_BEGIN(EmulatorSaveData);

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

TEST(EmulatorSaveData, GetEventResultReportsEmptyQueue)
{
	using namespace Libs::SaveData;

	uint8_t event[128] = {};
	EXPECT_EQ(SaveDataGetEventResult(nullptr, nullptr), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
	EXPECT_EQ(SaveDataGetEventResult(nullptr, event), Libs::SaveData::SAVE_DATA_ERROR_NOT_FOUND);
}

TEST(EmulatorSaveData, DeleteTransactionResourceTracksCreateHandles)
{
	using namespace Libs::SaveData;

	SaveDataMountPoint mount {};
	std::memcpy(mount.data, "/savedata0", 11);

	EXPECT_EQ(SaveDataDeleteTransactionResource(0, &mount), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);
	EXPECT_EQ(SaveDataDeleteTransactionResource(-1, &mount), Libs::SaveData::SAVE_DATA_ERROR_PARAMETER);

	// Unknown handle: guest-visible NOT_FOUND (not success).
	EXPECT_EQ(SaveDataDeleteTransactionResource(99999, &mount), Libs::SaveData::SAVE_DATA_ERROR_NOT_FOUND);

	const int32_t resource = SaveDataCreateTransactionResource(1);
	EXPECT_GT(resource, 0);
	EXPECT_EQ(SaveDataDeleteTransactionResource(resource, &mount), 0);
	// Double-delete must not invent success.
	EXPECT_EQ(SaveDataDeleteTransactionResource(resource, &mount), Libs::SaveData::SAVE_DATA_ERROR_NOT_FOUND);
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
	// Host `_SaveData` may or may not exist; contract is success with set_num
	// capped by dir_names_num and hit_num >= set_num.
	EXPECT_EQ(SaveDataDirNameSearch(&cond, &result), 0);
	EXPECT_LE(result.set_num, result.dir_names_num);
	EXPECT_GE(result.hit_num, result.set_num);

	// Filter for a name that cannot exist: exact match yields empty hits.
	SaveDataDirName filter {};
	std::memcpy(filter.data, "__kyty_no_such_save__", 21);
	cond.dir_name    = &filter;
	result.hit_num   = 99;
	result.set_num   = 99;
	EXPECT_EQ(SaveDataDirNameSearch(&cond, &result), 0);
	EXPECT_EQ(result.hit_num, 0u);
	EXPECT_EQ(result.set_num, 0u);
}

UT_END();
