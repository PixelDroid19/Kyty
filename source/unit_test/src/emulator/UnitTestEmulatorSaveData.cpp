#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/SaveData.h"
#include "Emulator/Log.h"

UT_BEGIN(EmulatorSaveData);

TEST(EmulatorSaveData, CreatesTransactionResourceThroughReturnValue)
{
	Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	const auto first  = Libs::SaveData::SaveDataCreateTransactionResource(0);
	const auto second = Libs::SaveData::SaveDataCreateTransactionResource(0);

	EXPECT_GT(first, 0);
	EXPECT_GT(second, first);
}

UT_END();
