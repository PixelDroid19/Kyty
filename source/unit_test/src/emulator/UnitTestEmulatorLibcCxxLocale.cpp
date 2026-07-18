#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/CxxLocale.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <cstring>

UT_BEGIN(EmulatorLibcCxxLocale);

using Kyty::Libs::LibC::CxxLocaleLayout;
using Kyty::Libs::LibC::CxxLocimpFacetLookupOk;
using Kyty::Libs::LibC::CxxLocimpLayout;
using Kyty::Libs::LibC::CxxSiTypeInfoLayout;
using Kyty::Libs::LibC::kCxxCtypeCharId;
using Kyty::Libs::LibC::kCxxLocimpFacetCount;

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

TEST(EmulatorLibcCxxLocale, ClassicLocimpFacetLookupMatchesGuestUseFacetLayout)
{
	void* facets[kCxxLocimpFacetCount] = {nullptr, reinterpret_cast<void*>(0x1)};
	CxxLocimpLayout locimp {};
	locimp.facet_vec   = facets;
	locimp.facet_count = kCxxLocimpFacetCount;

	EXPECT_TRUE(CxxLocimpFacetLookupOk(locimp, kCxxCtypeCharId));
	EXPECT_FALSE(CxxLocimpFacetLookupOk(locimp, 0));
	facets[1] = nullptr;
	EXPECT_FALSE(CxxLocimpFacetLookupOk(locimp, kCxxCtypeCharId));

	CxxLocaleLayout locale {};
	locale.ptr = &locimp;
	EXPECT_EQ(locale.ptr, &locimp);
}

// NID 5BIbzIuDxTQ = _ZTISt12domain_error; oAidKrxuUv0 = _ZTVSt12domain_error
// (ps5_names / Ps5Nid). Strict guest EXIT was Unpatched non-Func Object import.
TEST(EmulatorLibcCxxLocale, ResolvesDomainErrorTypeInfoObject)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	auto resolve_object = [&](const char16_t* nid) -> const Loader::SymbolRecord* {
		Loader::SymbolResolve query {};
		query.name                 = nid;
		query.library              = U"libc";
		query.library_version      = 1;
		query.module               = U"libc";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Object;
		return symbols.Find(query);
	};

	const auto* ti_rec = resolve_object(u"5BIbzIuDxTQ");
	ASSERT_NE(ti_rec, nullptr);
	ASSERT_NE(ti_rec->vaddr, 0u);
	const auto* ti = reinterpret_cast<const CxxSiTypeInfoLayout*>(ti_rec->vaddr);
	ASSERT_NE(ti->vtable, nullptr);
	ASSERT_NE(ti->name, nullptr);
	EXPECT_STREQ(ti->name, "St12domain_error");

	Loader::SymbolResolve wcmp {};
	wcmp.name                 = U"QJ5xVfKkni0";
	wcmp.library              = U"libc";
	wcmp.library_version      = 1;
	wcmp.module               = U"libc";
	wcmp.module_version_major = 1;
	wcmp.module_version_minor = 1;
	wcmp.type                 = Loader::SymbolType::Func;
	ASSERT_NE(symbols.Find(wcmp), nullptr);

	const char16_t* object_nids[] = {
	    u"oAidKrxuUv0", // _ZTVSt12domain_error
	    u"udTM6Nxx-Ng", // _ZTVSt11logic_error
	    u"n2kx+OmFUis", // _ZTISt9exception
	    u"dKjhNUf9FBc", // _ZTISt12out_of_range
	    u"bLPn1gfqSW8", // _ZTISt13runtime_error
	    u"XZzWt0ygWdw", // _ZTISt16invalid_argument
	    u"qOD-ksTkE08", // _ZTISt8bad_cast
	    u"BJCgW9-OxLA", // _ZTISt8ios_base
	    u"sBCTjFk7Gi4", // _ZTINSt8ios_base7failureE
	    u"n+aUKkC-3sI", // _ZTVSt12out_of_range
	    u"-L+-8F0+gBc", // _ZTVSt13runtime_error
	    u"keXoyW-rV-0", // _ZTVSt16invalid_argument
	    u"Bq8m04PN1zw", // _ZTVSt12system_error
	    u"tVHE+C8vGXk", // _ZTVSt8bad_cast
	    u"yLE5H3058Ao", // _ZTVNSt8ios_base7failureE
	    u"1kZFcktOm+s", // _ZTVSt7num_put<char,...>
	    u"E14mW8pVpoE", // num_put<char>::id
	    u"VmqsS6auJzo", // ctype<wchar_t>::id
	    u"irGo1yaJ-vM", // collate<wchar_t>::id
	};
	for (const char16_t* nid: object_nids)
	{
		const auto* rec = resolve_object(nid);
		ASSERT_NE(rec, nullptr) << "missing Object NID";
		ASSERT_NE(rec->vaddr, 0u);
	}
}

UT_END();
