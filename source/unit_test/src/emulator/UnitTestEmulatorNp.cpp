#include "Emulator/Libs/Np.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Config.h"
#include "Emulator/Log.h"
#include "Kyty/UnitTest.h"

#include <cstring>

UT_BEGIN(EmulatorNp);

using namespace Libs;

namespace {

void EnsureLog()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

} // namespace

TEST(EmulatorNp, ResolvesSessionSignalingInitialize)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"ysmw6J-P8Ak";
	query.library              = U"NpSessionSignaling";
	query.library_version      = 1;
	query.module               = U"NpSessionSignaling";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	EXPECT_NE(symbols.Find(query), nullptr);
}

TEST(EmulatorNp, ResolvesAlternateStateCallbackExport)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"qQJfO8HAiaY";
	query.library              = U"NpManager";
	query.library_version      = 1;
	query.module               = U"NpManager";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	EXPECT_NE(symbols.Find(query), nullptr);
}

TEST(EmulatorNp, ResolvesCppWebApiIntrusivePointerArrow)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNpCppWebApi_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"KJTzMXmYY+U";
	query.library              = U"NpCppWebApi";
	query.library_version      = 1;
	query.module               = U"NpCppWebApi";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.Find(query);
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	int   value = 0;
	void* slot  = &value;
	using OperatorArrow = KYTY_SYSV_ABI void* (*)(const void* self);
	auto* fn            = reinterpret_cast<OperatorArrow>(rec->vaddr);
	EXPECT_EQ(fn(&slot), &value);
}

TEST(EmulatorNp, ReportsSignedOutForAccountIdLookup)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNpManager_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"VgYczPGB5ss";
	query.library              = U"NpManager";
	query.library_version      = 1;
	query.module               = U"NpManager";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.Find(query);
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using GetUserIdByAccountId = KYTY_SYSV_ABI int (*)(uint64_t account_id, int32_t* user_id);
	auto* fn                   = reinterpret_cast<GetUserIdByAccountId>(rec->vaddr);
	int32_t user_id            = -1;
	EXPECT_EQ(fn(0, &user_id), static_cast<int>(0x80550003u));
	EXPECT_EQ(fn(1, nullptr), static_cast<int>(0x80550003u));
	EXPECT_EQ(fn(1, &user_id), static_cast<int>(0x80550006u));
	EXPECT_EQ(user_id, -1);
}

TEST(EmulatorNp, ReportsNoProfileDialogWhenItHasNotBeenOpened)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNpProfileDialog_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"haVZE9FgKqE";
	query.library              = U"NpProfileDialog";
	query.library_version      = 1;
	query.module               = U"NpProfileDialog";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.Find(query);
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using UpdateStatus = KYTY_SYSV_ABI int (*)();
	auto* fn           = reinterpret_cast<UpdateStatus>(rec->vaddr);
	EXPECT_EQ(fn(), 0);
}

TEST(EmulatorNp, ResolvesToolkitFriendsRequestAsStructuredUnsupportedOperation)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNpToolkit2_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"7zsee5IHLis";
	query.library              = U"NpToolkit2";
	query.library_version      = 1;
	query.module               = U"NpToolkit2";
	query.module_version_major = 0;
	query.module_version_minor = 0;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.Find(query);
	ASSERT_NE(rec, nullptr);
	EXPECT_NE(rec->vaddr, 0u);
}

TEST(EmulatorNp, ReportsMissingWebApiResponseHeaderForUnknownRequest)
{
	EnsureLog();
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"HwP3aM+c85c";
	query.library              = U"NpWebApi2";
	query.library_version      = 1;
	query.module               = U"NpWebApi2";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.Find(query);
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using GetHeaderLength = KYTY_SYSV_ABI int (*)(int64_t request_id, const char* field_name, size_t* value_length);
	auto* fn              = reinterpret_cast<GetHeaderLength>(rec->vaddr);
	size_t value_length   = 99;
	EXPECT_EQ(fn(1, nullptr, &value_length), static_cast<int>(0x80553402u));
	EXPECT_EQ(fn(1, "Content-Type", nullptr), static_cast<int>(0x80553402u));
	EXPECT_EQ(fn(1, "Content-Type", &value_length), static_cast<int>(0x80553406u));
	EXPECT_EQ(value_length, 99u);
}

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

// Astro after PlayGo: ObjectSetArray with null value allocates array via value_ptr.
TEST(EmulatorNp, ObjectSetArrayAllocatesWhenValueNull)
{
	using namespace NpUniversalDataSystem;

	Event*               event      = nullptr;
	EventPropertyObject* properties = nullptr;
	ASSERT_EQ(CreateEvent("analytics.boot", 0, &event, &properties), 0);

	EventPropertyArray* array = nullptr;
	EXPECT_LT(EventPropertyObjectSetArray(nullptr, "items", nullptr, &array), 0);
	EXPECT_LT(EventPropertyObjectSetArray(properties, nullptr, nullptr, &array), 0);
	EXPECT_EQ(EventPropertyObjectSetArray(properties, "items", nullptr, &array), 0);
	ASSERT_NE(array, nullptr);
	EXPECT_EQ(EventPropertyArraySetString(array, "entry"), 0);
	EXPECT_EQ(EventPropertyArraySetInt32(array, 1), 0);
	EXPECT_EQ(EventPropertyArraySetUInt64(array, 42ull), 0);
	EXPECT_EQ(DestroyEventPropertyArray(array), 0);
	EXPECT_EQ(DestroyEvent(event), 0);

	EventPropertyArray* created = nullptr;
	EXPECT_EQ(CreateEventPropertyArray(&created), 0);
	ASSERT_NE(created, nullptr);
	EXPECT_EQ(DestroyEventPropertyArray(created), 0);
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
	std::memcpy(label.data, "TEST_ENTITLEMENT", 16);

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
	std::memcpy(label.data, "TEST_ENTITLEMENT", 16);

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
	std::memcpy(label.data, "TEST_ENTITLEMENT", 16);

	// Sentinel must survive: missing entitlement must not write a fabricated record.
	const auto before = info;
	EXPECT_EQ(GetAddcontEntitlementInfo(0, &label, &info), ERROR_NO_ENTITLEMENT);
	EXPECT_EQ(std::memcmp(&info, &before, sizeof(info)), 0);
}

TEST(EmulatorNp, GetAddcontEntitlementInfoListReportsNoLocalEntitlements)
{
	using namespace NpEntitlementAccess;

	uint8_t init_parameters[0x40] = {};
	uint8_t boot_parameters[0x20] = {};
	ASSERT_EQ(Initialize(init_parameters, boot_parameters), 0);

	uint32_t hit_count = UINT32_MAX;
	EXPECT_EQ(GetAddcontEntitlementInfoList(0, nullptr, 0, &hit_count), 0);
	EXPECT_EQ(hit_count, 0u);
	EXPECT_EQ(GetAddcontEntitlementInfoList(0, nullptr, 0, nullptr), ERROR_PARAMETER);
}

UT_END();
