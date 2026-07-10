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
	EXPECT_EQ(NpTrophy2::RegisterContext(trophy_context, trophy_handle, 0), 0);
	EXPECT_LT(NpTrophy2::RegisterContext(0, trophy_handle, 0), 0);
}

TEST(EmulatorNp, OwnsLocalUniversalDataEvents)
{
	using namespace NpUniversalDataSystem;

	Event*               event      = nullptr;
	EventPropertyObject* properties = nullptr;

	EXPECT_LT(CreateEvent(nullptr, 0, &event, &properties), 0);
	EXPECT_LT(CreateEvent("runtime.event", 0, nullptr, &properties), 0);
	EXPECT_EQ(CreateEvent("runtime.event", 0, &event, &properties), 0);
	ASSERT_NE(event, nullptr);
	ASSERT_NE(properties, nullptr);
	EXPECT_EQ(EventPropertyObjectSetInt32(properties, "count", 3), 0);
	EXPECT_EQ(EventPropertyObjectSetString(properties, "state", "ready"), 0);
	EXPECT_EQ(PostEvent(1, 1, event, 0), 0);
	EXPECT_EQ(DestroyEvent(event), 0);
}

TEST(EmulatorNp, InitializesGameIntentIdempotently)
{
	EXPECT_EQ(NpGameIntent::Initialize(), 0);
	EXPECT_EQ(NpGameIntent::Initialize(), 0);
}

TEST(EmulatorNp, InitializesEntitlementAccessWithCleanBootState)
{
	uint8_t init_parameters[0x40] = {};
	uint8_t boot_parameters[0x20];
	memset(boot_parameters, 0xa5, sizeof(boot_parameters));

	EXPECT_EQ(NpEntitlementAccess::Initialize(init_parameters, boot_parameters), 0);
	for (auto value: boot_parameters)
	{
		EXPECT_EQ(value, 0);
	}
}

UT_END();
