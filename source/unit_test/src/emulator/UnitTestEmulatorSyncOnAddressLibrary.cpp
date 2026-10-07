#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

UT_BEGIN(EmulatorSyncOnAddressLibrary);

namespace {

Loader::SymbolResolve ResolveFor(const char16_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"libkernel_sync_on_address";
	query.library_version      = 1;
	query.module               = U"libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return query;
}

} // namespace

// A title imports the wait/wake pair as Hc4CaR6JBL0/q2y-wDIVWZA[libkernel_sync_on_address_v1][libkernel_v1.1];
// both must resolve in that library of the libkernel module.
TEST(EmulatorSyncOnAddressLibrary, WaitAndWakeResolveInTheirOwnLibrary)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_sync_on_address_1", &symbols));
	const auto* wait = symbols.Find(ResolveFor(u"Hc4CaR6JBL0"));
	const auto* wake = symbols.Find(ResolveFor(u"q2y-wDIVWZA"));
	ASSERT_NE(wait, nullptr);
	ASSERT_NE(wake, nullptr);
	EXPECT_NE(wait->vaddr, 0u);
	EXPECT_NE(wake->vaddr, 0u);
	EXPECT_NE(wait->vaddr, wake->vaddr);
}

UT_END();
