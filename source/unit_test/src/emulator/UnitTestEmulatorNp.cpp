#include "Emulator/Libs/Np.h"
#include "Kyty/UnitTest.h"

#include <cstring>

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
	// RegisterUnlockCallback accepts any callback pointer (including null) for HLE.
	EXPECT_EQ(NpTrophy2::RegisterUnlockCallback(nullptr, nullptr), 0);
	EXPECT_EQ(NpTrophy2::RegisterUnlockCallback(reinterpret_cast<void*>(0x1), nullptr), 0);
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

// Runs alphabetically before Initializes* so the module is still uninitialized.
TEST(EmulatorNp, GetAddcontEntitlementInfoRejectsBeforeInitialize)
{
	using namespace NpEntitlementAccess;

	UnifiedEntitlementLabel label {};
	AddcontEntitlementInfo  info {};
	std::memcpy(label.data, "DEADCELLSBADSEED", 16);

	EXPECT_EQ(GetAddcontEntitlementInfo(0, &label, &info), ERROR_NOT_INITIALIZED);
}

TEST(EmulatorNp, InitializesEntitlementAccessWithCleanBootState)
{
	using namespace NpEntitlementAccess;

	uint8_t init_parameters[0x40] = {};
	uint8_t boot_parameters[0x20];
	memset(boot_parameters, 0xa5, sizeof(boot_parameters));

	EXPECT_EQ(Initialize(nullptr, boot_parameters), ERROR_PARAMETER);
	EXPECT_EQ(Initialize(init_parameters, nullptr), ERROR_PARAMETER);
	EXPECT_EQ(Initialize(init_parameters, boot_parameters), 0);
	for (auto value: boot_parameters)
	{
		EXPECT_EQ(value, 0);
	}
}

TEST(EmulatorNp, GetAddcontEntitlementInfoValidatesArguments)
{
	using namespace NpEntitlementAccess;

	uint8_t init_parameters[0x40] = {};
	uint8_t boot_parameters[0x20] = {};
	ASSERT_EQ(Initialize(init_parameters, boot_parameters), 0);

	UnifiedEntitlementLabel label {};
	AddcontEntitlementInfo  info {};
	std::memcpy(label.data, "DEADCELLSBADSEED", 16);

	EXPECT_EQ(GetAddcontEntitlementInfo(0, nullptr, &info), ERROR_PARAMETER);
	EXPECT_EQ(GetAddcontEntitlementInfo(0, &label, nullptr), ERROR_PARAMETER);

	UnifiedEntitlementLabel empty {};
	EXPECT_EQ(GetAddcontEntitlementInfo(0, &empty, &info), ERROR_PARAMETER);

	UnifiedEntitlementLabel no_nul {};
	std::memset(no_nul.data, 'A', sizeof(no_nul.data));
	EXPECT_EQ(GetAddcontEntitlementInfo(0, &no_nul, &info), ERROR_PARAMETER);

	UnifiedEntitlementLabel bad_pad = label;
	bad_pad.padding[0]             = 1;
	EXPECT_EQ(GetAddcontEntitlementInfo(0, &bad_pad, &info), ERROR_PARAMETER);
}

TEST(EmulatorNp, GetAddcontEntitlementInfoReportsMissingEntitlement)
{
	using namespace NpEntitlementAccess;

	uint8_t init_parameters[0x40] = {};
	uint8_t boot_parameters[0x20] = {};
	ASSERT_EQ(Initialize(init_parameters, boot_parameters), 0);

	UnifiedEntitlementLabel label {};
	AddcontEntitlementInfo  info {};
	std::memset(&info, 0xa5, sizeof(info));
	std::memcpy(label.data, "DEADCELLSBADSEED", 16);

	// Sentinel must survive: missing entitlement must not write a fabricated record.
	const auto before = info;
	EXPECT_EQ(GetAddcontEntitlementInfo(0, &label, &info), ERROR_NO_ENTITLEMENT);
	EXPECT_EQ(std::memcmp(&info, &before, sizeof(info)), 0);
}

UT_END();
