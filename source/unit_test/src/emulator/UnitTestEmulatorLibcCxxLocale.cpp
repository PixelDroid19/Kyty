#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/CxxLocale.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/ProcessEnvironment.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <cstdio>
#include <clocale>
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

void EnsurePthread()
{
	if (!Libs::LibKernel::PthreadIsInitialized())
	{
		Libs::LibKernel::PthreadSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
}

using ExecuteOnceCallback = KYTY_SYSV_ABI int (*)(void*, void*, void**);

struct ExecuteOnceContext
{
	int  invocations = 0;
	bool succeed     = true;
};

int KYTY_SYSV_ABI ExecuteOnceCallbackImpl(void* first, void* context, void** result)
{
	EXPECT_EQ(first, nullptr);

	auto* state = static_cast<ExecuteOnceContext*>(context);
	EXPECT_NE(state, nullptr);
	EXPECT_NE(result, nullptr);

	state->invocations++;
	*result = state;
	return state->succeed ? 1 : 0;
}

const Loader::SymbolRecord* ResolveLibcFunction(Loader::SymbolDatabase* symbols, const char16_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return symbols->FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
}

} // namespace

TEST(EmulatorLibcCxxLocale, ExecuteOnceRunsCallbackOnceAndRetriesFailure)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"DiGVep5yB5w");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using ExecuteOnce = KYTY_SYSV_ABI int (*)(int* flag, ExecuteOnceCallback callback, void* context);
	auto* execute_once = reinterpret_cast<ExecuteOnce>(rec->vaddr);

	int                flag = 0;
	ExecuteOnceContext context {};

	EXPECT_EQ(execute_once(&flag, ExecuteOnceCallbackImpl, &context), 1);
	EXPECT_EQ(flag, 1);
	EXPECT_EQ(context.invocations, 1);
	EXPECT_EQ(execute_once(&flag, ExecuteOnceCallbackImpl, &context), 1);
	EXPECT_EQ(context.invocations, 1);

	flag            = 0;
	context.succeed = false;
	EXPECT_EQ(execute_once(&flag, ExecuteOnceCallbackImpl, &context), 0);
	EXPECT_EQ(flag, 0);
	EXPECT_EQ(context.invocations, 2);
	context.succeed = true;
	EXPECT_EQ(execute_once(&flag, ExecuteOnceCallbackImpl, &context), 1);
	EXPECT_EQ(flag, 1);
	EXPECT_EQ(context.invocations, 3);
}

TEST(EmulatorLibcCxxLocale, SetlocaleExposesTheStandardLocaleContract)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"PtsB1Q9wsFA");
	ASSERT_NE(rec, nullptr);
	using Setlocale = KYTY_SYSV_ABI char* (*)(int category, const char* locale);
	auto* setlocale = reinterpret_cast<Setlocale>(rec->vaddr);
	ASSERT_NE(setlocale, nullptr);

	const char* current = setlocale(LC_ALL, nullptr);
	ASSERT_NE(current, nullptr);
	EXPECT_NE(current[0], '\0');
}

TEST(EmulatorLibcCxxLocale, MtxInitUsesGuestPthreadStorage)
{
	EnsureLog();
	EnsurePthread();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"YaHc3GS7y7g");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);
	const auto* lock_rec = ResolveLibcFunction(&symbols, u"iS4aWbUonl0");
	ASSERT_NE(lock_rec, nullptr);
	ASSERT_NE(lock_rec->vaddr, 0u);
	const auto* unlock_rec = ResolveLibcFunction(&symbols, u"gTuXQwP9rrs");
	ASSERT_NE(unlock_rec, nullptr);
	ASSERT_NE(unlock_rec->vaddr, 0u);

	using MtxInit = KYTY_SYSV_ABI int (*)(Libs::LibKernel::PthreadMutex* mutex, int type);
	using MtxLock = KYTY_SYSV_ABI int (*)(Libs::LibKernel::PthreadMutex* mutex);
	auto* mtx_init = reinterpret_cast<MtxInit>(rec->vaddr);
	auto* mtx_lock = reinterpret_cast<MtxLock>(lock_rec->vaddr);
	auto* mtx_unlock = reinterpret_cast<MtxLock>(unlock_rec->vaddr);

	Libs::LibKernel::PthreadMutex mutex = nullptr;
	EXPECT_EQ(mtx_init(&mutex, 2), 0);
	ASSERT_NE(mutex, nullptr);
	EXPECT_EQ(mtx_lock(&mutex), 0);
	EXPECT_EQ(mtx_unlock(&mutex), 0);
	EXPECT_EQ(Libs::LibKernel::PthreadMutexDestroy(&mutex), OK);
}

TEST(EmulatorLibcCxxLocale, CndBroadcastUsesGuestPthreadStorage)
{
	EnsureLog();
	EnsurePthread();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* init_rec = ResolveLibcFunction(&symbols, u"SreZybSRWpU");
	ASSERT_NE(init_rec, nullptr);
	ASSERT_NE(init_rec->vaddr, 0u);
	const auto* broadcast_rec = ResolveLibcFunction(&symbols, u"VsP3daJgmVA");
	ASSERT_NE(broadcast_rec, nullptr);
	ASSERT_NE(broadcast_rec->vaddr, 0u);

	using CndOperation = KYTY_SYSV_ABI int (*)(Libs::LibKernel::PthreadCond* cond);
	auto* cnd_init      = reinterpret_cast<CndOperation>(init_rec->vaddr);
	auto* cnd_broadcast = reinterpret_cast<CndOperation>(broadcast_rec->vaddr);

	Libs::LibKernel::PthreadCond cond = nullptr;
	EXPECT_EQ(cnd_init(&cond), 0);
	ASSERT_NE(cond, nullptr);
	EXPECT_EQ(cnd_broadcast(&cond), 0);
	EXPECT_EQ(Libs::LibKernel::PthreadCondDestroy(&cond), OK);
}

TEST(EmulatorLibcCxxLocale, FlushesCapturedStandardErrorStream)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"MUjC4lbHrK4");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Flush = KYTY_SYSV_ABI int (*)(FILE* stream);
	auto* flush = reinterpret_cast<Flush>(rec->vaddr);

	EXPECT_EQ(flush(stderr), 0);
}

TEST(EmulatorLibcCxxLocale, FilenoReturnsHostDescriptorForGuestStream)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"Fm-dmyywH9Q");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Fileno = KYTY_SYSV_ABI int (*)(FILE* stream);
	auto* fileno_guest = reinterpret_cast<Fileno>(rec->vaddr);

	FILE* stream = std::tmpfile();
	ASSERT_NE(stream, nullptr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	EXPECT_EQ(fileno_guest(stream), ::_fileno(stream));
#else
	EXPECT_EQ(fileno_guest(stream), ::fileno(stream));
#endif
	EXPECT_EQ(fileno_guest(nullptr), -1);
	EXPECT_EQ(std::fclose(stream), 0);
}

TEST(EmulatorLibcCxxLocale, DecrementExceptionRefcountAcceptsNullException)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"MQFPAqQPt1s");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using DecrementExceptionRefcount = KYTY_SYSV_ABI void (*)(void* exception);
	auto* decrement = reinterpret_cast<DecrementExceptionRefcount>(rec->vaddr);
	decrement(nullptr);
}

TEST(EmulatorLibcCxxLocale, InitEnvCapturesBoundedProcessArguments)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"bzQExy189ZI");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using InitEnv = KYTY_SYSV_ABI void (*)(const Libs::ProcessEnvironment::InitParameters* parameters);
	auto* init_env = reinterpret_cast<InitEnv>(rec->vaddr);

	Libs::ProcessEnvironment::InitParameters parameters {};
	parameters.argc    = 1;
	parameters.argv[0] = "guest-program";
	init_env(&parameters);

	const auto arguments = Libs::ProcessEnvironment::GetArguments();
	EXPECT_EQ(arguments.argc, 1);
	EXPECT_STREQ(arguments.argv[0], "guest-program");
	EXPECT_EQ(arguments.argv[1], nullptr);
}

TEST(EmulatorLibcCxxLocale, GetenvExposesHostProcessConfiguration)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"smbQukfxYJM");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Getenv = KYTY_SYSV_ABI char* (*)(const char* name);
	auto* getenv = reinterpret_cast<Getenv>(rec->vaddr);

	EXPECT_EQ(getenv("PATH"), ::getenv("PATH"));
}

TEST(EmulatorLibcCxxLocale, CeilDoubleMatchesLibcContract)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"gacfOmO8hNs");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Ceil = KYTY_SYSV_ABI double (*)(double);
	auto* ceil_fn = reinterpret_cast<Ceil>(rec->vaddr);

	EXPECT_DOUBLE_EQ(ceil_fn(1.1), 2.0);
	EXPECT_DOUBLE_EQ(ceil_fn(-1.1), -1.0);
	EXPECT_DOUBLE_EQ(ceil_fn(2.0), 2.0);
}

TEST(EmulatorLibcCxxLocale, Udivti3DividesGuestUnsigned128BitValues)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));
	const auto* rec = ResolveLibcFunction(&symbols, u"802pFCwC9w0");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Udivti3 = KYTY_SYSV_ABI unsigned __int128 (*)(unsigned __int128 numerator, unsigned __int128 denominator);
	auto* udivti3 = reinterpret_cast<Udivti3>(rec->vaddr);

	const unsigned __int128 numerator = (static_cast<unsigned __int128>(9) << 80u) + 77u;
	const unsigned __int128 denominator = 9u;
	EXPECT_EQ(udivti3(numerator, denominator), numerator / denominator);
}

TEST(EmulatorLibcCxxLocale, VswprintfFormatsGuestUtf16SignedInteger)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"u0XOsuOmOzc");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Vswprintf = KYTY_SYSV_ABI int (*)(uint16_t* output, size_t output_count, const uint16_t* format, Libs::VaList* arguments);
	auto* vswprintf = reinterpret_cast<Vswprintf>(rec->vaddr);

	uint16_t              output[64] = {};
	const char16_t        format[]   = u"PlatformMisc::RequestExit(%i)";
	Libs::VaRegSave       registers {};
	Libs::VaList          arguments {};
	registers.gp[0]               = 8;
	arguments.gp_offset           = 0;
	arguments.fp_offset           = offsetof(Libs::VaRegSave, fp);
	arguments.reg_save_area       = &registers;

	const char16_t expected[] = u"PlatformMisc::RequestExit(8)";
	EXPECT_EQ(vswprintf(output, std::size(output), reinterpret_cast<const uint16_t*>(format), &arguments),
	          static_cast<int>(std::size(expected) - 1));
	EXPECT_EQ(std::char_traits<char16_t>::compare(reinterpret_cast<const char16_t*>(output), expected, std::size(expected)), 0);
}

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

TEST(EmulatorLibcCxxLocale, ResolvesBaseExceptionDoraiseAsVoidFunction)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"tyHd3P7oDrU";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Doraise = KYTY_SYSV_ABI void (*)(const void* self);
	auto* fn      = reinterpret_cast<Doraise>(rec->vaddr);
	fn(reinterpret_cast<const void*>(0x840000000));
}

TEST(EmulatorLibcCxxLocale, NothrowNewOverloadsResolveAndUseLibcAllocationOwnership)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	auto resolve = [&](const char16_t* nid) -> const Loader::SymbolRecord*
	{
		Loader::SymbolResolve query {};
		query.name                 = nid;
		query.library              = U"libc";
		query.library_version      = 1;
		query.module               = U"libc";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		return symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	};

	const auto* new_rec          = resolve(u"ryUxD-60bKM");
	const auto* new_array_rec    = resolve(u"Jh5qUcwiSEk");
	const auto* delete_rec       = resolve(u"z+P+xCnWLBk");
	const auto* delete_array_rec = resolve(u"MLWl90SFWNE");
	ASSERT_NE(new_rec, nullptr);
	ASSERT_NE(new_array_rec, nullptr);
	ASSERT_NE(delete_rec, nullptr);
	ASSERT_NE(delete_array_rec, nullptr);

	using NothrowNew      = KYTY_SYSV_ABI void* (*)(size_t size, const void* nothrow_tag);
	using Delete          = KYTY_SYSV_ABI void (*)(void* ptr);
	auto* new_fn          = reinterpret_cast<NothrowNew>(new_rec->vaddr);
	auto* new_array_fn    = reinterpret_cast<NothrowNew>(new_array_rec->vaddr);
	auto* delete_fn       = reinterpret_cast<Delete>(delete_rec->vaddr);
	auto* delete_array_fn = reinterpret_cast<Delete>(delete_array_rec->vaddr);

	void* ptr = new_fn(0x40, reinterpret_cast<const void*>(0x840000000));
	ASSERT_NE(ptr, nullptr);
	delete_fn(ptr);

	void* array_ptr = new_array_fn(0x40000, reinterpret_cast<const void*>(0x840000000));
	ASSERT_NE(array_ptr, nullptr);
	delete_array_fn(array_ptr);
}

TEST(EmulatorLibcCxxLocale, StrerrorRResolvesAndCopiesIntoGuestBuffer)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"RBcs3uut1TA");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using StrerrorR = KYTY_SYSV_ABI int (*)(int error, char* destination, size_t size);
	auto* strerror_r = reinterpret_cast<StrerrorR>(rec->vaddr);

	char destination[64] {};
	EXPECT_EQ(strerror_r(Libs::Posix::POSIX_ENOENT, destination, sizeof(destination)), 0);
	EXPECT_NE(destination[0], '\0');

	char short_destination[2] = {'x', 'x'};
	EXPECT_EQ(strerror_r(Libs::Posix::POSIX_ENOENT, short_destination, sizeof(short_destination)), Libs::Posix::POSIX_ERANGE);
	EXPECT_EQ(short_destination[sizeof(short_destination) - 1], '\0');
	EXPECT_EQ(strerror_r(Libs::Posix::POSIX_ENOENT, nullptr, sizeof(destination)), Libs::Posix::POSIX_EINVAL);
}

UT_END();
