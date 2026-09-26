#include "Kyty/UnitTest.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <cstring>

UT_BEGIN(EmulatorSymbolDatabase);

namespace {

Loader::SymbolResolve Resolve(const char16_t* nid, Loader::SymbolType type)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = type;
	return query;
}

Loader::SymbolResolve ResolveFor(const char16_t* nid, Loader::SymbolType type, const char32_t* library, const char32_t* module)
{
	auto query    = Resolve(nid, type);
	query.library = library;
	query.module  = module;
	return query;
}

} // namespace

TEST(EmulatorSymbolDatabase, FindRequiresTheCompleteCanonicalIdentity)
{
	Loader::SymbolDatabase symbols;
	symbols.Add(Resolve(u"same-nid", Loader::SymbolType::Object), 0x1000);

	EXPECT_EQ(symbols.Find(Resolve(u"same-nid", Loader::SymbolType::Func)), nullptr);
	EXPECT_EQ(symbols.Find(ResolveFor(u"same-nid", Loader::SymbolType::Object, U"AudioOut2", U"AudioOut")), nullptr);
	ASSERT_NE(symbols.Find(Resolve(u"same-nid", Loader::SymbolType::Object)), nullptr);
}

TEST(EmulatorSymbolDatabase, CanonicalIdentityDoesNotRewriteLibraryNames)
{
	const auto agc       = ResolveFor(u"same-nid", Loader::SymbolType::Func, U"Agc", U"Agc");
	const auto graphics5 = ResolveFor(u"same-nid", Loader::SymbolType::Func, U"Graphics5", U"Graphics5");
	EXPECT_NE(Loader::SymbolDatabase::GenerateName(agc), Loader::SymbolDatabase::GenerateName(graphics5));

	const auto gnm_driver      = ResolveFor(u"same-nid", Loader::SymbolType::Func, U"GnmDriver", U"GnmDriver");
	const auto graphics_driver = ResolveFor(u"same-nid", Loader::SymbolType::Func, U"GraphicsDriver", U"GraphicsDriver");
	EXPECT_NE(Loader::SymbolDatabase::GenerateName(gnm_driver), Loader::SymbolDatabase::GenerateName(graphics_driver));
}

TEST(EmulatorSymbolDatabase, InitAllRegistersEachCanonicalSymbolOnce)
{
	Loader::SymbolDatabase symbols;
	Libs::InitAll(&symbols);

	for (uint32_t first_index = 0; first_index < symbols.SymbolCount(); first_index++)
	{
		const auto* first = symbols.SymbolAt(first_index);
		ASSERT_NE(first, nullptr);
		for (uint32_t second_index = first_index + 1; second_index < symbols.SymbolCount(); second_index++)
		{
			const auto* second = symbols.SymbolAt(second_index);
			ASSERT_NE(second, nullptr);
			ASSERT_NE(first->name, second->name) << first->name.C_Str();
		}
	}
}

TEST(EmulatorSymbolDatabase, AudioOut2RegistersOnlyIdentifiedExports)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libAudio_1", &symbols));

	const char16_t* unresolved_nids[] = {u"TUuiYS2kE8s", u"jbz9I9vkqkk", u"3BytPOQgVKc", u"Ec63y59l9tw", u"fYapWA9xVmA",
	                                     u"Bagshr7OQ6Q", u"Gz1rmUZpROM", u"sysY2FHYff4"};
	for (const auto* nid: unresolved_nids)
	{
		EXPECT_EQ(symbols.Find(ResolveFor(nid, Loader::SymbolType::Func, U"AudioOut2", U"AudioOut")), nullptr);
	}
	// Identified speaker-info export: registered under AudioOut2_v1/AudioOut.
	EXPECT_NE(symbols.Find(ResolveFor(u"DImz2Ft9E2g", Loader::SymbolType::Func, U"AudioOut2", U"AudioOut")), nullptr);
}

TEST(EmulatorSymbolDatabase, TextToSpeechStatusUsesGuestOutputContract)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSceTextToSpeech2_1", &symbols));

	const auto* record = symbols.FindByNid(U"08JSg9p6bgQ", Loader::SymbolType::Func);
	ASSERT_NE(record, nullptr);
	const auto* initialize_record = symbols.FindByNid(U"UOjiprYwVNw", Loader::SymbolType::Func);
	const auto* open_record = symbols.FindByNid(U"X0HZNbSiqyg", Loader::SymbolType::Func);
	const auto* terminate_record = symbols.FindByNid(U"SoWHuVW0gpU", Loader::SymbolType::Func);
	ASSERT_NE(initialize_record, nullptr);
	ASSERT_NE(open_record, nullptr);
	ASSERT_NE(terminate_record, nullptr);
	using GetSpeechStatus = KYTY_SYSV_ABI int (*)(int32_t* status);
	using Initialize = KYTY_SYSV_ABI int (*)();
	using Open = KYTY_SYSV_ABI int (*)(const uint32_t* parameters);
	using Terminate = KYTY_SYSV_ABI int (*)();
	auto* get_speech_status = reinterpret_cast<GetSpeechStatus>(record->vaddr);
	auto* initialize = reinterpret_cast<Initialize>(initialize_record->vaddr);
	auto* open = reinterpret_cast<Open>(open_record->vaddr);
	auto* terminate = reinterpret_cast<Terminate>(terminate_record->vaddr);

	const uint64_t address = Core::VirtualMemory::Alloc(0, 0x1000, Core::VirtualMemory::Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	auto* status = reinterpret_cast<int32_t*>(address);
	auto* parameters = reinterpret_cast<uint32_t*>(address + sizeof(int32_t));
	parameters[0] = 0;
	parameters[1] = 0;
	ASSERT_EQ(initialize(), 0);
	ASSERT_EQ(open(parameters), 0);
	*status = -1;
	EXPECT_EQ(get_speech_status(status), 0);
	EXPECT_EQ(*status, 0);
	EXPECT_EQ(get_speech_status(nullptr), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	*status = -1;
	ASSERT_TRUE(Core::VirtualMemory::ProtectGuest(address, 0x1000, Core::VirtualMemory::Mode::Read));
	EXPECT_EQ(get_speech_status(status), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(*status, -1);
	ASSERT_EQ(terminate(), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(address));
}

TEST(EmulatorSymbolDatabase, AddAliasesRegistersEveryNidWithOneHandler)
{
	Loader::SymbolDatabase symbols;
	symbols.AddAliases(Resolve(u"unused", Loader::SymbolType::Func), {"nid-a", "nid-b"}, 0x12345678, U"test_handler");

	const auto* first = symbols.Find(Resolve(u"nid-a", Loader::SymbolType::Func));
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(first->vaddr, 0x12345678u);

	const auto* second = symbols.Find(Resolve(u"nid-b", Loader::SymbolType::Func));
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(second->vaddr, 0x12345678u);

	EXPECT_EQ(symbols.Find(Resolve(u"nid-a", Loader::SymbolType::Object)), nullptr);
}

TEST(EmulatorSymbolDatabase, NeutralHleRegistryPreservesCanonicalIdentity)
{
	Loader::SymbolDatabase symbols;
	Kyty::Hle::HleSymbolResolve export_symbol {};
	export_symbol.name                 = U"neutral-nid";
	export_symbol.library              = U"lib-neutral";
	export_symbol.library_version      = 1;
	export_symbol.module               = U"module-neutral";
	export_symbol.module_version_major = 2;
	export_symbol.module_version_minor = 3;
	export_symbol.type                 = Kyty::Hle::HleSymbolType::Func;

	symbols.AddHle(export_symbol, 0x87654321, U"neutral_handler");

	Loader::SymbolResolve query {};
	query.name                 = U"neutral-nid";
	query.library              = U"lib-neutral";
	query.library_version      = 1;
	query.module               = U"module-neutral";
	query.module_version_major = 2;
	query.module_version_minor = 3;
	query.type                 = Loader::SymbolType::Func;

	const auto* record = symbols.Find(query);
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->vaddr, 0x87654321u);
	EXPECT_EQ(record->dbg_name, U"neutral_handler");

	export_symbol.name = U"";
	symbols.AddHleAliases(export_symbol, {"neutral-alias-a", "neutral-alias-b"}, 0x87654321, U"neutral_handler");

	query.name = U"neutral-alias-a";
	EXPECT_NE(symbols.Find(query), nullptr);
	query.name = U"neutral-alias-b";
	EXPECT_NE(symbols.Find(query), nullptr);
}

TEST(EmulatorSymbolDatabase, PlatformPrivacyResolvesUnderItsOwnLibraryScope)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libUserService_1", &symbols));
	// The guest imports this NID under library UserServicePlatformPrivacyWs1_v1
	// of module UserService_v1.1; the plain UserService library must not claim it.
	ASSERT_NE(symbols.Find(ResolveFor(u"D-CzAxQL0XI", Loader::SymbolType::Func, U"UserServicePlatformPrivacyWs1", U"UserService")),
	          nullptr);
	EXPECT_EQ(symbols.Find(ResolveFor(u"D-CzAxQL0XI", Loader::SymbolType::Func, U"UserService", U"UserService")), nullptr);
}

TEST(EmulatorSymbolDatabase, PlatformPrivacyWritesOnlyTheGuestSetting)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libUserService_1", &symbols));
	const auto* record = symbols.Find(ResolveFor(u"D-CzAxQL0XI", Loader::SymbolType::Func, U"UserServicePlatformPrivacyWs1", U"UserService"));
	ASSERT_NE(record, nullptr);
	using GetSetting = KYTY_SYSV_ABI int (*)(int, int32_t*);
	auto* get_setting = reinterpret_cast<GetSetting>(record->vaddr);

	const uint64_t address = Core::VirtualMemory::Alloc(0, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	auto*        words = reinterpret_cast<int32_t*>(address);
	words[0]           = -1;
	words[1]           = 0x12345678;
	const int32_t kept = words[1];
	// The id comes from GetInitialUser (which yields 1); any other id is offline.
	EXPECT_EQ(get_setting(2, words), Libs::UserService::USER_SERVICE_ERROR_NOT_LOGGED_IN);
	EXPECT_EQ(words[0], -1);
	EXPECT_EQ(get_setting(1, nullptr), Libs::UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT);
	// The caller treats *value_out != 0 as "enabled": report the offline default.
	EXPECT_EQ(get_setting(1, words), 0);
	EXPECT_EQ(words[0], 0);
	EXPECT_EQ(words[1], kept);
	EXPECT_TRUE(Core::VirtualMemory::Free(address));

	int32_t host_output = -1;
	EXPECT_EQ(get_setting(1, &host_output), Libs::UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT);
	EXPECT_EQ(host_output, -1);
}

TEST(EmulatorSymbolDatabase, PlatformPrivacyRejectsUnwritableOutputsWithoutFaultingOrPartialWrites)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libUserService_1", &symbols));
	const auto* record = symbols.Find(ResolveFor(u"D-CzAxQL0XI", Loader::SymbolType::Func, U"UserServicePlatformPrivacyWs1", U"UserService"));
	ASSERT_NE(record, nullptr);
	using GetSetting = KYTY_SYSV_ABI int (*)(int, int32_t*);
	auto* get_setting = reinterpret_cast<GetSetting>(record->vaddr);

	for (int output_case = 0; output_case < 3; ++output_case)
	{
		SCOPED_TRACE(output_case);
		ASSERT_EXIT(
		    {
			    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
			    const uint64_t page     = Core::VirtualMemory::GetPageSize();
			    const uint64_t address  = Core::VirtualMemory::Alloc(0, page * 2, Core::VirtualMemory::Mode::ReadWrite);
			    if (address == 0) { std::_Exit(2); }
			    auto*         bytes = reinterpret_cast<uint8_t*>(address);
			    std::memset(bytes, 0xa5, static_cast<size_t>(page * 2));
			    int32_t* output = reinterpret_cast<int32_t*>(uintptr_t {1});
			    if (output_case == 1)
			    {
				    output = reinterpret_cast<int32_t*>(address);
				    if (!Core::VirtualMemory::ProtectGuest(address, page, Core::VirtualMemory::Mode::Read)) { std::_Exit(3); }
			    }
			    if (output_case == 2)
			    {
				    output = reinterpret_cast<int32_t*>(address + page - 2);
				    if (!Core::VirtualMemory::ProtectGuest(address + page, page, Core::VirtualMemory::Mode::NoAccess)) { std::_Exit(3); }
			    }
			    const int result = get_setting(1, output);
			    if (result != Libs::UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT) { std::_Exit(4); }
			    for (uint64_t i = 0; i < page; ++i)
			    {
				    if (bytes[i] != 0xa5) { std::_Exit(5); }
			    }
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

UT_END();
