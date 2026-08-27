#include "Kyty/UnitTest.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

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
	                                     u"Bagshr7OQ6Q", u"Gz1rmUZpROM", u"sysY2FHYff4", u"DImz2Ft9E2g"};
	for (const auto* nid: unresolved_nids)
	{
		EXPECT_EQ(symbols.Find(ResolveFor(nid, Loader::SymbolType::Func, U"AudioOut2", U"AudioOut")), nullptr);
	}
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

UT_END();
