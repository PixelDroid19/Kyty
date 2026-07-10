#include "Emulator/Libs/Np.h"
#include "Kyty/UnitTest.h"

UT_BEGIN(EmulatorNp);

using namespace Libs;

TEST(EmulatorNp, ValidatesAndCreatesStableHandles)
{
	uint8_t parameters[16] = {};
	int32_t context        = 0;
	int32_t first_handle   = 0;
	int32_t second_handle  = 0;

	EXPECT_LT(NpUniversalDataSystem::Initialize(nullptr), 0);
	EXPECT_EQ(NpUniversalDataSystem::Initialize(parameters), 0);
	EXPECT_EQ(NpUniversalDataSystem::CreateContext(&context), 0);
	EXPECT_EQ(context, 1);
	EXPECT_EQ(NpUniversalDataSystem::CreateHandle(&first_handle, nullptr), 0);
	EXPECT_EQ(NpUniversalDataSystem::CreateHandle(&second_handle, nullptr), 0);
	EXPECT_GT(first_handle, 0);
	EXPECT_GT(second_handle, first_handle);

	int32_t trophy_context = 0;
	EXPECT_EQ(NpTrophy2::CreateContext(&trophy_context, 1, 0, 0), 0);
	EXPECT_GT(trophy_context, 0);
	EXPECT_LT(NpTrophy2::CreateContext(nullptr, 1, 0, 0), 0);
	int32_t trophy_handle = 0;
	EXPECT_EQ(NpTrophy2::CreateHandle(&trophy_handle), 0);
	EXPECT_GT(trophy_handle, 0);
}

UT_END();
