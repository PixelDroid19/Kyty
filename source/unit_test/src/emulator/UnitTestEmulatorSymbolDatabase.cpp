#include "Kyty/UnitTest.h"

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

} // namespace

TEST(EmulatorSymbolDatabase, NidPrefixFallbackDoesNotCrossSymbolTypes)
{
	Loader::SymbolDatabase symbols;
	symbols.Add(Resolve(u"same-nid", Loader::SymbolType::Object), 0x1000);

	EXPECT_EQ(symbols.Find(Resolve(u"same-nid", Loader::SymbolType::Func)), nullptr);
	ASSERT_NE(symbols.Find(Resolve(u"same-nid", Loader::SymbolType::Object)), nullptr);
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

UT_END();
