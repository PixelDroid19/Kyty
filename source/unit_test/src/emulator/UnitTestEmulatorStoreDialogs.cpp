#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Dialog.h"
#include "Emulator/Log.h"

#include <cstring>

UT_BEGIN(EmulatorStoreDialogs);

namespace {

using namespace Kyty;
using namespace Libs::Dialog;

void EnsureLog()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

} // namespace

TEST(EmulatorStoreDialogs, CommerceDialogFinishesCanceledAndReturnsTheUserData)
{
	using namespace NpCommerceDialog;
	EnsureLog();

	NpCommerceDialogParam param {};
	param.user_data = 0x1122334455667788ull;
	EXPECT_EQ(NpCommerceDialogOpen(&param), CommonDialog::ERROR_NOT_INITIALIZED);
	ASSERT_EQ(NpCommerceDialogInitialize(), 0);
	EXPECT_EQ(NpCommerceDialogInitialize(), CommonDialog::ERROR_ALREADY_INITIALIZED);

	NpCommerceDialogResult result {};
	EXPECT_EQ(NpCommerceDialogGetResult(&result), CommonDialog::ERROR_NOT_FINISHED);
	EXPECT_EQ(NpCommerceDialogOpen(nullptr), CommonDialog::ERROR_ARG_NULL);
	ASSERT_EQ(NpCommerceDialogOpen(&param), 0);
	EXPECT_EQ(NpCommerceDialogUpdateStatus(), CommonDialog::STATUS_FINISHED);

	std::memset(&result, 0xff, sizeof(result));
	EXPECT_EQ(NpCommerceDialogGetResult(&result), RESULT_USER_CANCELED);
	EXPECT_EQ(result.result, RESULT_USER_CANCELED);
	EXPECT_EQ(result.authorized, 0u);
	EXPECT_EQ(result.user_data, param.user_data);

	NpCommerceDialogParam2 param2 {};
	param2.user_data = 7;
	ASSERT_EQ(NpCommerceDialogOpen2(&param2), 0);
	EXPECT_EQ(NpCommerceDialogGetResult(&result), RESULT_USER_CANCELED);
	EXPECT_EQ(result.user_data, 7u);

	ASSERT_EQ(NpCommerceDialogTerminate(), 0);
	EXPECT_EQ(NpCommerceDialogTerminate(), CommonDialog::ERROR_NOT_INITIALIZED);
	EXPECT_EQ(NpCommerceDialogGetResult(&result), CommonDialog::ERROR_NOT_INITIALIZED);
}

TEST(EmulatorStoreDialogs, BrowserDialogFinishesAsClosedByTheUser)
{
	using namespace WebBrowserDialog;
	EnsureLog();

	const uint8_t param[64] {};
	EXPECT_EQ(WebBrowserDialogOpen(param), CommonDialog::ERROR_NOT_INITIALIZED);
	ASSERT_EQ(WebBrowserDialogInitialize(), 0);
	EXPECT_EQ(WebBrowserDialogInitialize(), CommonDialog::ERROR_ALREADY_INITIALIZED);
	EXPECT_EQ(WebBrowserDialogGetStatus(), CommonDialog::STATUS_INITIALIZED);

	WebBrowserDialogResult result {};
	EXPECT_EQ(WebBrowserDialogGetResult(&result), CommonDialog::ERROR_NOT_FINISHED);
	ASSERT_EQ(WebBrowserDialogOpen(param), 0);
	EXPECT_EQ(WebBrowserDialogUpdateStatus(), CommonDialog::STATUS_FINISHED);
	ASSERT_EQ(WebBrowserDialogGetResult(&result), 0);
	EXPECT_EQ(result.result, RESULT_USER_CANCELED);
	EXPECT_EQ(WebBrowserDialogClose(), CommonDialog::ERROR_NOT_RUNNING);

	ASSERT_EQ(WebBrowserDialogOpenForPredeterminedContent(param), 0);
	EXPECT_EQ(WebBrowserDialogGetStatus(), CommonDialog::STATUS_FINISHED);
	ASSERT_EQ(WebBrowserDialogTerminate(), 0);
	EXPECT_EQ(WebBrowserDialogTerminate(), CommonDialog::ERROR_NOT_INITIALIZED);
}

UT_END();
