#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/CxxLocale.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <cstring>
#include <cwchar>

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

TEST(EmulatorLibcCxxLocale, Qj5xWideCompareMatchesGuestSixteenBitCodeUnits)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"QJ5xVfKkni0";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Wmemcmp16 = KYTY_SYSV_ABI int (*)(const char16_t* a, const char16_t* b, size_t count);
	auto* fn        = reinterpret_cast<Wmemcmp16>(rec->vaddr);

	const char16_t text[]   = u"||This path";
	const char16_t match[]  = u"||This";
	const char16_t higher[] = u"||Thjt";

	EXPECT_EQ(fn(text, match, 6), 0);
	EXPECT_LT(fn(text, higher, 5), 0);
	EXPECT_GT(fn(higher, text, 5), 0);
}

TEST(EmulatorLibcCxxLocale, Fl3WideCopyMatchesGuestSixteenBitCodeUnits)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"fL3O02ypZFE";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Wmemcpy16 = KYTY_SYSV_ABI char16_t* (*)(char16_t* dst, const char16_t* src, size_t count);
	auto* fn        = reinterpret_cast<Wmemcpy16>(rec->vaddr);

	char16_t dst[]       = u"______";
	const char16_t src[] = u"planet";

	EXPECT_EQ(fn(dst, src, 6), dst);
	EXPECT_EQ(std::memcmp(dst, src, sizeof(src) - sizeof(src[0])), 0);
}

TEST(EmulatorLibcCxxLocale, NineRMmReturnsClassicLocimp)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"9rMML086SEE";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using GetLocimp = KYTY_SYSV_ABI CxxLocimpLayout* (*)();
	auto* fn        = reinterpret_cast<GetLocimp>(rec->vaddr);

	CxxLocimpLayout* locimp = fn();
	ASSERT_NE(locimp, nullptr);
	ASSERT_NE(locimp->vtable, nullptr);
	EXPECT_TRUE(CxxLocimpFacetLookupOk(*locimp, kCxxCtypeCharId));
}

TEST(EmulatorLibcCxxLocale, HqiInitializesTemporaryLocaleInfo)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"hqi8yMOCmG0";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using InitLocaleInfo = KYTY_SYSV_ABI void (*)(void* self, const char* name, std::uint64_t category);
	auto* fn             = reinterpret_cast<InitLocaleInfo>(rec->vaddr);

	alignas(8) std::uint8_t temp[0x40] {};
	fn(temp, "C", 0x17);
}

TEST(EmulatorLibcCxxLocale, P6DestroysTemporaryLocaleInfo)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"p6LrHjIQMdk";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using DestroyLocaleInfo = KYTY_SYSV_ABI void (*)(void* self);
	auto* fn                = reinterpret_cast<DestroyLocaleInfo>(rec->vaddr);

	alignas(8) std::uint8_t temp[0x40] {};
	fn(temp);
}

TEST(EmulatorLibcCxxLocale, QwRegistersLocaleFacet)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"QW2jL1J5rwY";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using RegisterFacet = KYTY_SYSV_ABI void (*)(void* self);
	auto* fn            = reinterpret_cast<RegisterFacet>(rec->vaddr);

	alignas(8) std::uint8_t facet[0x40] {};
	fn(facet);
}

TEST(EmulatorLibcCxxLocale, QxqReturnsMultibyteConversionState)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"QxqK-IdpumU";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using GetState = KYTY_SYSV_ABI std::mbstate_t* (*)();
	auto* fn       = reinterpret_cast<GetState>(rec->vaddr);

	EXPECT_NE(fn(), nullptr);
	EXPECT_EQ(fn(), fn());
}

TEST(EmulatorLibcCxxLocale, Zs94ReturnsWideConversionState)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"zS94yyJRSUs";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using GetState = KYTY_SYSV_ABI std::mbstate_t* (*)();
	auto* fn       = reinterpret_cast<GetState>(rec->vaddr);

	EXPECT_NE(fn(), nullptr);
	EXPECT_EQ(fn(), fn());
}

TEST(EmulatorLibcCxxLocale, StvConvertsAsciiWideCharToMultibyte)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"stv1S3BKfgw";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Wctombx = KYTY_SYSV_ABI int (*)(char* dst, std::uint32_t ch, std::mbstate_t* state, const void* cvtvec);
	auto* fn      = reinterpret_cast<Wctombx>(rec->vaddr);

	char           out[4] {};
	std::mbstate_t state {};
	EXPECT_EQ(fn(out, ']', &state, nullptr), 1);
	EXPECT_EQ(out[0], ']');
}

TEST(EmulatorLibcCxxLocale, Minus9ConvertsSingleByteToAsciiWideChar)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"-9SIhUr4Iuo";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Mbtowcx = KYTY_SYSV_ABI int (*)(std::uint16_t* dst, const char* src, size_t count, std::mbstate_t* state, const void* cvtvec);
	auto* fn      = reinterpret_cast<Mbtowcx>(rec->vaddr);

	const char     src[] = "]";
	std::uint16_t  out   = 0;
	std::mbstate_t state {};
	EXPECT_EQ(fn(&out, src, 1, &state, nullptr), 1);
	EXPECT_EQ(out, static_cast<std::uint16_t>(']'));
}

TEST(EmulatorLibcCxxLocale, ResolvesRegexErrorHelperAsFunctionOnly)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"UWyL6KoR96U";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	query.type = Loader::SymbolType::Object;
	EXPECT_EQ(symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query)), nullptr);
}

UT_END();
