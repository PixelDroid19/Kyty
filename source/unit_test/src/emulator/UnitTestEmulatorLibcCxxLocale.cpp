#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/CxxLocale.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/ProcessEnvironment.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Loader/GuestCall.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Kernel/PthreadInternal.h"

#include <atomic>
#include <array>
#include <cstdio>
#include <cmath>
#include <clocale>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

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
	if (!Kernel::PthreadIsInitialized())
	{
		Kernel::PthreadSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
}

using ExecuteOnceCallback = KYTY_SYSV_ABI int (*)(void*, void*, void**);
using CxaDestructor       = KYTY_SYSV_ABI void (*)(void*);
using CxaAtexit           = KYTY_SYSV_ABI int (*)(CxaDestructor, void*, void*);
using CxaFinalize         = KYTY_SYSV_ABI void (*)(void*);

struct TestIstreambuf
{
	void**      vtable = nullptr;
	const char* current = nullptr;
};

int KYTY_SYSV_ABI TestIstreamUnderflow(TestIstreambuf* self)
{
	return self != nullptr && self->current != nullptr && *self->current != '\0' ? static_cast<unsigned char>(*self->current) : -1;
}

int KYTY_SYSV_ABI TestIstreamUflow(TestIstreambuf* self)
{
	const int result = TestIstreamUnderflow(self);
	if (result >= 0)
	{
		++self->current;
	}
	return result;
}

void* KYTY_SYSV_ABI CompleteDetachedThread(void* arg)
{
	auto* complete = static_cast<std::atomic_bool*>(arg);
	complete->store(true, std::memory_order_release);
	return nullptr;
}

void* KYTY_SYSV_ABI HoldPthreadUntilReleased(void* arg)
{
	auto* release = static_cast<std::atomic_bool*>(arg);
	while (!release->load(std::memory_order_acquire)) {}
	return nullptr;
}

struct PthreadKeyEntryContext
{
	Kernel::PthreadKey key        = -1;
	void*              value      = nullptr;
	std::atomic_int    set_result = Libs::LibKernel::KERNEL_ERROR_EINVAL;
	std::atomic_int    reregister_result = Libs::LibKernel::KERNEL_ERROR_EINVAL;
	std::atomic_int    worker_thread_id {-1};
	std::atomic_int    destructor_thread_id {-1};
	std::atomic_uintptr_t destructor_rsp {0};
	bool               reregister_in_destructor = false;
};

std::atomic_int* g_pthread_key_destructor_calls = nullptr;
PthreadKeyEntryContext* g_pthread_key_destructor_context = nullptr;

class ScopedPthreadKeyDestructorCounter
{
public:
	explicit ScopedPthreadKeyDestructorCounter(std::atomic_int* counter, PthreadKeyEntryContext* context = nullptr)
	{
		g_pthread_key_destructor_calls   = counter;
		g_pthread_key_destructor_context = context;
	}
	~ScopedPthreadKeyDestructorCounter()
	{
		g_pthread_key_destructor_context = nullptr;
		g_pthread_key_destructor_calls   = nullptr;
	}

	KYTY_CLASS_NO_COPY(ScopedPthreadKeyDestructorCounter);
};

void KYTY_SYSV_ABI CountPthreadKeyDestructor(void* /*value*/)
{
	if (g_pthread_key_destructor_calls != nullptr)
	{
		const int count = g_pthread_key_destructor_calls->fetch_add(1, std::memory_order_relaxed) + 1;
		if (g_pthread_key_destructor_context != nullptr)
		{
			uintptr_t rsp = 0;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
			asm volatile("movq %%rsp, %0" : "=r"(rsp));
#endif
			g_pthread_key_destructor_context->destructor_rsp.store(rsp, std::memory_order_release);
			g_pthread_key_destructor_context->destructor_thread_id.store(Kernel::PthreadGetthreadid(),
			                                                            std::memory_order_release);
		}
		if (g_pthread_key_destructor_context != nullptr && g_pthread_key_destructor_context->reregister_in_destructor &&
		    count < 4)
		{
			g_pthread_key_destructor_context->reregister_result.store(
			    Kernel::PthreadSetspecific(g_pthread_key_destructor_context->key, g_pthread_key_destructor_context->value),
			    std::memory_order_release);
		}
	}
}

struct CxaCallbackContext
{
	std::vector<int>* events             = nullptr;
	std::mutex*       events_mutex       = nullptr;
	int               value              = 0;
	CxaAtexit         atexit             = nullptr;
	CxaFinalize       finalize            = nullptr;
	CxaCallbackContext* reentrant_context = nullptr;
	void*             reentrant_dso      = nullptr;
	std::atomic_bool  reentered {false};
};

void KYTY_SYSV_ABI RecordCxaDestructor(void* arg)
{
	auto* context = static_cast<CxaCallbackContext*>(arg);
	if (context == nullptr)
	{
		return;
	}

	{
		std::lock_guard lock(*context->events_mutex);
		context->events->push_back(context->value);
	}

	if (context->reentrant_context != nullptr && !context->reentered.exchange(true, std::memory_order_acq_rel))
	{
		(void)context->atexit(RecordCxaDestructor, context->reentrant_context, context->reentrant_dso);
		context->finalize(context->reentrant_dso);
	}
}

struct CxaCountContext
{
	std::atomic_int* count = nullptr;
};

void KYTY_SYSV_ABI CountCxaDestructor(void* arg)
{
	auto* context = static_cast<CxaCountContext*>(arg);
	if (context != nullptr && context->count != nullptr)
	{
		context->count->fetch_add(1, std::memory_order_relaxed);
	}
}

void* KYTY_SYSV_ABI SetPthreadKeySpecific(void* arg)
{
	auto* context = static_cast<PthreadKeyEntryContext*>(arg);
	context->worker_thread_id.store(Kernel::PthreadGetthreadid(), std::memory_order_release);
	context->set_result.store(Kernel::PthreadSetspecific(context->key, context->value), std::memory_order_release);
	return nullptr;
}

uint64_t KYTY_SYSV_ABI InvokeHostPthreadEntryOnStack(uint64_t target, uint64_t arg0, uint64_t /*arg1*/, uint64_t /*arg2*/,
                                                      void* /*stack_top*/)
{
	auto* entry = reinterpret_cast<Kernel::pthread_entry_func_t>(target);
	return reinterpret_cast<uint64_t>(entry(reinterpret_cast<void*>(arg0)));
}

class ScopedPthreadEntryInvoker
{
public:
	ScopedPthreadEntryInvoker() { Emulator::GuestRuntimePort::Install({nullptr, nullptr, nullptr, InvokeHostPthreadEntryOnStack}); }
	~ScopedPthreadEntryInvoker() { Emulator::GuestRuntimePort::Install({}); }

	KYTY_CLASS_NO_COPY(ScopedPthreadEntryInvoker);
};

class ScopedPthreadGuestCallInvoker
{
public:
	ScopedPthreadGuestCallInvoker()
	{
		Emulator::GuestRuntimePort::Install({nullptr, nullptr, nullptr, Loader::GuestCall::InvokeOnStack});
	}
	~ScopedPthreadGuestCallInvoker() { Emulator::GuestRuntimePort::Install({}); }

	KYTY_CLASS_NO_COPY(ScopedPthreadGuestCallInvoker);
};

struct ExecuteOnceContext
{
	void* expected_flag = nullptr;
	int   invocations   = 0;
	bool  succeed       = true;
};

int KYTY_SYSV_ABI ExecuteOnceCallbackImpl(void* first, void* context, void** result)
{
	auto* state = static_cast<ExecuteOnceContext*>(context);
	EXPECT_NE(state, nullptr);
	EXPECT_NE(result, nullptr);
	EXPECT_EQ(first, state->expected_flag);

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

std::pair<CxaAtexit, CxaFinalize> ResolveCxaFunctions()
{
	Loader::SymbolDatabase symbols;
	EXPECT_TRUE(Libs::Init(U"libc_1", &symbols));
	const auto* atexit_record   = ResolveLibcFunction(&symbols, u"tsvEmnenz48");
	const auto* finalize_record = ResolveLibcFunction(&symbols, u"H2e8t5ScQGc");
	EXPECT_NE(atexit_record, nullptr);
	EXPECT_NE(finalize_record, nullptr);
	return {atexit_record != nullptr ? reinterpret_cast<CxaAtexit>(atexit_record->vaddr) : nullptr,
	        finalize_record != nullptr ? reinterpret_cast<CxaFinalize>(finalize_record->vaddr) : nullptr};
}

} // namespace

TEST(EmulatorLibcCxxLocale, ExecuteOnceUsesPs5FlagStatesAndRetriesFailure)
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
	context.expected_flag = &flag;

	EXPECT_EQ(execute_once(&flag, ExecuteOnceCallbackImpl, &context), 0);
	EXPECT_EQ(flag, 2);
	EXPECT_EQ(context.invocations, 1);
	EXPECT_EQ(execute_once(&flag, ExecuteOnceCallbackImpl, &context), 0);
	EXPECT_EQ(context.invocations, 1);

	flag            = 0;
	context.succeed = false;
	EXPECT_NE(execute_once(&flag, ExecuteOnceCallbackImpl, &context), 0);
	EXPECT_EQ(flag, 0);
	EXPECT_EQ(context.invocations, 2);
	context.succeed = true;
	EXPECT_EQ(execute_once(&flag, ExecuteOnceCallbackImpl, &context), 0);
	EXPECT_EQ(flag, 2);
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

	using MtxInit = KYTY_SYSV_ABI int (*)(Kernel::PthreadMutex* mutex, int type);
	using MtxLock = KYTY_SYSV_ABI int (*)(Kernel::PthreadMutex* mutex);
	auto* mtx_init = reinterpret_cast<MtxInit>(rec->vaddr);
	auto* mtx_lock = reinterpret_cast<MtxLock>(lock_rec->vaddr);
	auto* mtx_unlock = reinterpret_cast<MtxLock>(unlock_rec->vaddr);

	Kernel::PthreadMutex mutex = nullptr;
	EXPECT_EQ(mtx_init(&mutex, 2), 0);
	ASSERT_NE(mutex, nullptr);
	EXPECT_EQ(mtx_lock(&mutex), 0);
	EXPECT_EQ(mtx_unlock(&mutex), 0);
	EXPECT_EQ(Kernel::PthreadMutexDestroy(&mutex), OK);
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

	using CndOperation = KYTY_SYSV_ABI int (*)(Kernel::PthreadCond* cond);
	auto* cnd_init      = reinterpret_cast<CndOperation>(init_rec->vaddr);
	auto* cnd_broadcast = reinterpret_cast<CndOperation>(broadcast_rec->vaddr);

	Kernel::PthreadCond cond = nullptr;
	EXPECT_EQ(cnd_init(&cond), 0);
	ASSERT_NE(cond, nullptr);
	EXPECT_EQ(cnd_broadcast(&cond), 0);
	EXPECT_EQ(Kernel::PthreadCondDestroy(&cond), OK);
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
	    u"eVFYZnYNDo0", // codecvt<char, char, mbstate_t>::id
	    u"aK1Ymf-NhAs", // codecvt<char, char, mbstate_t> vtable
	    u"HIhqigNaOns", // _Inf
	    u"VmqsS6auJzo", // ctype<wchar_t>::id
	    u"irGo1yaJ-vM", // collate<wchar_t>::id
	};
	for (const char16_t* nid: object_nids)
	{
		const auto* rec = resolve_object(nid);
		ASSERT_NE(rec, nullptr) << "missing Object NID";
		ASSERT_NE(rec->vaddr, 0u);
	}

	const auto* inf_rec = resolve_object(u"HIhqigNaOns");
	ASSERT_NE(inf_rec, nullptr);
	const auto inf = *reinterpret_cast<const double*>(inf_rec->vaddr);
	EXPECT_TRUE(std::isinf(inf));
	EXPECT_GT(inf, 0.0);
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

TEST(EmulatorLibcCxxLocale, ResolvesGxxPersonalityAndContinuesUnwind)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"XwLA5cTHjt4");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using GxxPersonality = KYTY_SYSV_ABI int (*)(int, int, uint64_t, void*, void*);
	auto* fn              = reinterpret_cast<GxxPersonality>(rec->vaddr);
	EXPECT_EQ(fn(1, 0, 0, nullptr, nullptr), 8);
}

TEST(EmulatorLibcCxxLocale, ResolvesIosBaseFailureCompleteDestructor)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"N2f485TmJms");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Destructor = KYTY_SYSV_ABI void (*)(void*);
	auto* fn          = reinterpret_cast<Destructor>(rec->vaddr);
	fn(nullptr);
}

TEST(EmulatorLibcCxxLocale, ResolvesBadCastCompleteDestructor)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	const auto* rec = ResolveLibcFunction(&symbols, u"47RvLSo2HN8");
	ASSERT_NE(rec, nullptr);
	ASSERT_NE(rec->vaddr, 0u);

	using Destructor = KYTY_SYSV_ABI void (*)(void*);
	auto* fn          = reinterpret_cast<Destructor>(rec->vaddr);
	fn(nullptr);
}

TEST(EmulatorLibcCxxLocale, CxaFinalizeNullRunsAllRegisteredDsosInReverseOrder)
{
	EnsureLog();
	const auto [atexit, finalize] = ResolveCxaFunctions();
	ASSERT_NE(atexit, nullptr);
	ASSERT_NE(finalize, nullptr);
	finalize(nullptr);

	int              dso_a = 0;
	int              dso_b = 0;
	std::vector<int> events;
	std::mutex       events_mutex;
	CxaCallbackContext first {&events, &events_mutex, 1};
	CxaCallbackContext second {&events, &events_mutex, 2};
	CxaCallbackContext third {&events, &events_mutex, 3};

	ASSERT_EQ(atexit(RecordCxaDestructor, &first, &dso_a), 0);
	ASSERT_EQ(atexit(RecordCxaDestructor, &second, &dso_b), 0);
	ASSERT_EQ(atexit(RecordCxaDestructor, &third, &dso_a), 0);
	finalize(nullptr);
	EXPECT_EQ(events, (std::vector<int> {3, 2, 1}));

	// Claimed callbacks stay cleared on subsequent all-DSO finalization.
	finalize(nullptr);
	EXPECT_EQ(events, (std::vector<int> {3, 2, 1}));
}

TEST(EmulatorLibcCxxLocale, CxaFinalizeFiltersDsoWithoutSkippingRemainingCallbacks)
{
	EnsureLog();
	const auto [atexit, finalize] = ResolveCxaFunctions();
	ASSERT_NE(atexit, nullptr);
	ASSERT_NE(finalize, nullptr);
	finalize(nullptr);

	int              dso_a = 0;
	int              dso_b = 0;
	std::vector<int> events;
	std::mutex       events_mutex;
	CxaCallbackContext first {&events, &events_mutex, 1};
	CxaCallbackContext second {&events, &events_mutex, 2};
	CxaCallbackContext third {&events, &events_mutex, 3};

	ASSERT_EQ(atexit(RecordCxaDestructor, &first, &dso_a), 0);
	ASSERT_EQ(atexit(RecordCxaDestructor, &second, &dso_b), 0);
	ASSERT_EQ(atexit(RecordCxaDestructor, &third, &dso_a), 0);
	finalize(&dso_a);
	EXPECT_EQ(events, (std::vector<int> {3, 1}));

	finalize(nullptr);
	EXPECT_EQ(events, (std::vector<int> {3, 1, 2}));
}

TEST(EmulatorLibcCxxLocale, CxaFinalizeAllowsDestructorReentry)
{
	EnsureLog();
	const auto [atexit, finalize] = ResolveCxaFunctions();
	ASSERT_NE(atexit, nullptr);
	ASSERT_NE(finalize, nullptr);
	finalize(nullptr);

	int              dso_a = 0;
	int              dso_b = 0;
	std::vector<int> events;
	std::mutex       events_mutex;
	CxaCallbackContext nested {&events, &events_mutex, 3};
	CxaCallbackContext first {&events, &events_mutex, 1};
	CxaCallbackContext reentrant {&events, &events_mutex, 2, atexit, finalize, &nested, &dso_b};

	ASSERT_EQ(atexit(RecordCxaDestructor, &first, &dso_a), 0);
	ASSERT_EQ(atexit(RecordCxaDestructor, &reentrant, &dso_a), 0);
	finalize(&dso_a);
	EXPECT_TRUE(reentrant.reentered.load(std::memory_order_acquire));
	EXPECT_EQ(events, (std::vector<int> {2, 3, 1}));
}

TEST(EmulatorLibcCxxLocale, CxaAtexitSynchronizesConcurrentRegistration)
{
	EnsureLog();
	const auto [atexit, finalize] = ResolveCxaFunctions();
	ASSERT_NE(atexit, nullptr);
	ASSERT_NE(finalize, nullptr);
	finalize(nullptr);

	int              dso = 0;
	std::atomic_int  callbacks {0};
	std::atomic_int  registration_failures {0};
	CxaCountContext  context {&callbacks};
	constexpr int    worker_count       = 8;
	constexpr int    registrations_each = 32;
	std::vector<std::thread> workers;
	workers.reserve(worker_count);
	for (int worker = 0; worker < worker_count; worker++)
	{
		workers.emplace_back([&] {
			for (int registration = 0; registration < registrations_each; registration++)
			{
				if (atexit(CountCxaDestructor, &context, &dso) != 0)
				{
					registration_failures.fetch_add(1, std::memory_order_relaxed);
				}
			}
		});
	}
	for (auto& worker: workers)
	{
		worker.join();
	}

	EXPECT_EQ(registration_failures.load(std::memory_order_relaxed), 0);
	finalize(&dso);
	EXPECT_EQ(callbacks.load(std::memory_order_relaxed), worker_count * registrations_each);
	finalize(&dso);
	EXPECT_EQ(callbacks.load(std::memory_order_relaxed), worker_count * registrations_each);
}

TEST(EmulatorLibcCxxLocale, CtypeCaseTablesInitializeSafelyUnderConcurrentFirstUse)
{
	EnsureLog();
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));
	const auto* upper_record = ResolveLibcFunction(&symbols, u"rcQCUr0EaRU");
	const auto* lower_record = ResolveLibcFunction(&symbols, u"1uJgoVq3bQU");
	ASSERT_NE(upper_record, nullptr);
	ASSERT_NE(lower_record, nullptr);
	using GetCaseTable = KYTY_SYSV_ABI const short* (*)();
	auto* upper = reinterpret_cast<GetCaseTable>(upper_record->vaddr);
	auto* lower = reinterpret_cast<GetCaseTable>(lower_record->vaddr);

	constexpr size_t worker_count = 16;
	std::array<const short*, worker_count> uppers {};
	std::array<const short*, worker_count> lowers {};
	std::atomic_size_t                     ready {0};
	std::atomic_bool                       start {false};
	std::vector<std::thread>               workers;
	workers.reserve(worker_count);
	for (size_t worker = 0; worker < worker_count; worker++)
	{
		workers.emplace_back([&, worker] {
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			uppers[worker] = upper();
			lowers[worker] = lower();
		});
	}
	while (ready.load(std::memory_order_acquire) != worker_count)
	{
		std::this_thread::yield();
	}
	start.store(true, std::memory_order_release);
	for (auto& worker: workers)
	{
		worker.join();
	}

	for (size_t worker = 0; worker < worker_count; worker++)
	{
		EXPECT_EQ(uppers[worker], uppers[0]);
		EXPECT_EQ(lowers[worker], lowers[0]);
	}
	EXPECT_EQ(uppers[0][-1], 0);
	EXPECT_EQ(lowers[0][-1], 0);
	EXPECT_EQ(uppers[0][static_cast<unsigned char>('a')], 'A');
	EXPECT_EQ(lowers[0][static_cast<unsigned char>('A')], 'a');
}

// Regression: FAULTR at INVALID_MEMORY+0x20 because weak objects
// _ZTIi / _ZTIv / _ZTV num_get<char> were unresolved sentinels.
TEST(EmulatorLibcCxxLocale, ThrdDetachRejectsGuestJoinWhileInternalReaperOwnsCollection)
{
	EnsureLog();
	EnsurePthread();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));
	const auto* rec = ResolveLibcFunction(&symbols, u"L7f7zYwBvZA");
	ASSERT_NE(rec, nullptr);
	using ThrdDetach = int(KYTY_SYSV_ABI*)(Kernel::Pthread);
	auto* detach = reinterpret_cast<ThrdDetach>(rec->vaddr);

	ScopedPthreadEntryInvoker entry_invoker;
	std::atomic_bool complete {false};
	Kernel::Pthread  thread = nullptr;
	ASSERT_EQ(Kernel::PthreadCreate(&thread, nullptr, CompleteDetachedThread, &complete, "detach-abi-test"), OK);
	ASSERT_NE(thread, nullptr);
	EXPECT_EQ(detach(thread), 0);
	EXPECT_NE(detach(nullptr), 0);
	EXPECT_EQ(Kernel::PthreadJoin(thread, nullptr), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	while (!complete.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
}

TEST(EmulatorLibcCxxLocale, PthreadSetcanceltypeRejectsInvalidGuestType)
{
	EnsureLog();
	EnsurePthread();

	EXPECT_EQ(Kernel::PthreadSetcanceltype(-1, nullptr), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(Libs::Posix::pthread_setcanceltype(-1, nullptr), Libs::Posix::POSIX_EINVAL);
}

TEST(EmulatorLibcCxxLocale, PthreadSetcancelstateRejectsInvalidAndAllowsNullOldState)
{
	EnsureLog();
	EnsurePthread();

	EXPECT_EQ(Kernel::PthreadSetcancelstate(-1, nullptr), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(Kernel::PthreadSetcancelstate(1, nullptr), OK);
	int old_state = -1;
	EXPECT_EQ(Kernel::PthreadSetcancelstate(0, &old_state), OK);
	EXPECT_EQ(old_state, 1);
	EXPECT_EQ(Kernel::PthreadSetcancelstate(1, nullptr), OK);
}

TEST(EmulatorLibcCxxLocale, PthreadSchedparamKeepsGuestPolicyAndPriority)
{
	EnsureLog();
	EnsurePthread();

	ScopedPthreadEntryInvoker entry_invoker;
	std::atomic_bool release {false};
	Kernel::Pthread  thread = nullptr;
	ASSERT_EQ(Kernel::PthreadCreate(&thread, nullptr, HoldPthreadUntilReleased, &release, "schedparam-abi-test"), OK);
	ASSERT_NE(thread, nullptr);

	int                      policy = -1;
	Kernel::KernelSchedParam reported {};
	EXPECT_EQ(Kernel::PthreadGetschedparam(thread, &policy, &reported), OK);
	EXPECT_EQ(policy, 1);
	EXPECT_EQ(reported.sched_priority, 700);

	Kernel::KernelSchedParam param {};
	param.sched_priority = 767;
	EXPECT_EQ(Kernel::PthreadSetschedparam(thread, 3, &param), OK);
	EXPECT_EQ(Kernel::PthreadGetschedparam(thread, &policy, &reported), OK);
	EXPECT_EQ(policy, 3);
	EXPECT_EQ(reported.sched_priority, 767);
	Kernel::PthreadAttr live_attr = nullptr;
	ASSERT_EQ(Kernel::PthreadAttrInit(&live_attr), OK);
	ASSERT_EQ(Kernel::PthreadAttrGet(thread, &live_attr), OK);
	EXPECT_EQ(Kernel::PthreadAttrGetschedpolicy(&live_attr, &policy), OK);
	EXPECT_EQ(policy, 3);
	EXPECT_EQ(Kernel::PthreadAttrGetschedparam(&live_attr, &reported), OK);
	EXPECT_EQ(reported.sched_priority, 767);

	EXPECT_EQ(Kernel::PthreadSetprio(thread, 700), OK);
	int priority = -1;
	EXPECT_EQ(Kernel::PthreadGetprio(thread, &priority), OK);
	EXPECT_EQ(priority, 700);
	ASSERT_EQ(Kernel::PthreadAttrGet(thread, &live_attr), OK);
	EXPECT_EQ(Kernel::PthreadAttrGetschedpolicy(&live_attr, &policy), OK);
	EXPECT_EQ(policy, 3);
	EXPECT_EQ(Kernel::PthreadAttrGetschedparam(&live_attr, &reported), OK);
	EXPECT_EQ(reported.sched_priority, 700);

	param.sched_priority = 700;
	EXPECT_EQ(Kernel::PthreadSetschedparam(thread, 2, &param), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(Kernel::PthreadGetschedparam(thread, &policy, &reported), OK);
	EXPECT_EQ(policy, 3);
	EXPECT_EQ(reported.sched_priority, 700);

	param.sched_priority = 255;
	EXPECT_EQ(Kernel::PthreadSetschedparam(thread, 1, &param), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(Kernel::PthreadSetprio(thread, 768), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(Kernel::PthreadGetschedparam(thread, &policy, &reported), OK);
	EXPECT_EQ(policy, 3);
	EXPECT_EQ(reported.sched_priority, 700);

	release.store(true, std::memory_order_release);
	EXPECT_EQ(Kernel::PthreadJoin(thread, nullptr), OK);
	EXPECT_EQ(Kernel::PthreadAttrDestroy(&live_attr), OK);
}

TEST(EmulatorLibcCxxLocale, PthreadAttrSchedparamPreservesGuestValues)
{
	EnsureLog();
	EnsurePthread();

	Kernel::PthreadAttr attr = nullptr;
	ASSERT_EQ(Kernel::PthreadAttrInit(&attr), OK);
	ASSERT_NE(attr, nullptr);

	Kernel::KernelSchedParam param {};
	param.sched_priority = 256;
	EXPECT_EQ(Kernel::PthreadAttrSetschedparam(&attr, &param), OK);
	EXPECT_EQ(Kernel::PthreadAttrSetschedpolicy(&attr, 3), OK);

	int                      policy = -1;
	Kernel::KernelSchedParam reported {};
	EXPECT_EQ(Kernel::PthreadAttrGetschedparam(&attr, &reported), OK);
	EXPECT_EQ(reported.sched_priority, 256);
	EXPECT_EQ(Kernel::PthreadAttrGetschedpolicy(&attr, &policy), OK);
	EXPECT_EQ(policy, 3);

	param.sched_priority = 255;
	EXPECT_EQ(Kernel::PthreadAttrSetschedparam(&attr, &param), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(Kernel::PthreadAttrSetschedpolicy(&attr, 2), Libs::LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(Kernel::PthreadAttrGetschedparam(&attr, &reported), OK);
	EXPECT_EQ(reported.sched_priority, 256);
	EXPECT_EQ(Kernel::PthreadAttrGetschedpolicy(&attr, &policy), OK);
	EXPECT_EQ(policy, 3);

	ScopedPthreadEntryInvoker entry_invoker;
	std::atomic_bool          release {false};
	Kernel::Pthread           thread = nullptr;
	ASSERT_EQ(Kernel::PthreadCreate(&thread, &attr, HoldPthreadUntilReleased, &release, "attr-schedparam-abi-test"), OK);
	ASSERT_NE(thread, nullptr);
	EXPECT_EQ(Kernel::PthreadGetschedparam(thread, &policy, &reported), OK);
	EXPECT_EQ(policy, 3);
	EXPECT_EQ(reported.sched_priority, 256);

	release.store(true, std::memory_order_release);
	EXPECT_EQ(Kernel::PthreadJoin(thread, nullptr), OK);
	EXPECT_EQ(Kernel::PthreadAttrDestroy(&attr), OK);
}

TEST(EmulatorLibcCxxLocale, PthreadKeyDestructorRunsFourPassesOnExitingThread)
{
	EnsureLog();
	EnsurePthread();

	ScopedPthreadGuestCallInvoker guest_call_invoker;
	std::atomic_int           destructor_calls {0};

	Kernel::PthreadKey key = -1;
	ASSERT_EQ(Kernel::PthreadKeyCreate(&key, CountPthreadKeyDestructor), OK);

	PthreadKeyEntryContext context {};
	context.key   = key;
	context.value = &context;
	context.reregister_in_destructor = true;
	ScopedPthreadKeyDestructorCounter destructor_counter(&destructor_calls, &context);

	std::vector<uint8_t> guest_stack(0x8000);
	Kernel::PthreadAttr   attr = nullptr;
	ASSERT_EQ(Kernel::PthreadAttrInit(&attr), OK);
	ASSERT_EQ(Kernel::PthreadAttrSetstack(&attr, guest_stack.data(), guest_stack.size()), OK);

	Kernel::Pthread thread = nullptr;
	ASSERT_EQ(Kernel::PthreadCreate(&thread, &attr, SetPthreadKeySpecific, &context, "key-destructor-test"), OK);
	ASSERT_NE(thread, nullptr);
	EXPECT_EQ(Kernel::PthreadJoin(thread, nullptr), OK);
	EXPECT_EQ(context.set_result.load(std::memory_order_acquire), OK);
	EXPECT_EQ(destructor_calls.load(std::memory_order_relaxed), 4);
	EXPECT_EQ(context.reregister_result.load(std::memory_order_acquire), OK);
	EXPECT_EQ(context.destructor_thread_id.load(std::memory_order_acquire),
	          context.worker_thread_id.load(std::memory_order_acquire));
	const auto stack_base = reinterpret_cast<uintptr_t>(guest_stack.data());
	const auto stack_end  = stack_base + guest_stack.size();
	const auto destructor_rsp = context.destructor_rsp.load(std::memory_order_acquire);
	EXPECT_GE(destructor_rsp, stack_base);
	EXPECT_LT(destructor_rsp, stack_end);
	ASSERT_NE(Kernel::g_pthread_context, nullptr);
	EXPECT_EQ(Kernel::g_pthread_context->GetPthreadKeys()->CountSpecificValuesForThread(
	              context.worker_thread_id.load(std::memory_order_acquire)),
	          0u);
	EXPECT_EQ(Kernel::PthreadAttrDestroy(&attr), OK);
	EXPECT_EQ(Kernel::PthreadKeyDelete(key), OK);
}

TEST(EmulatorLibcCxxLocale, PthreadKeyDestructReclaimsRetiredThreadEntries)
{
	EnsureLog();
	EnsurePthread();

	ScopedPthreadGuestCallInvoker guest_call_invoker;
	Kernel::PthreadKey            key = -1;
	ASSERT_EQ(Kernel::PthreadKeyCreate(&key, CountPthreadKeyDestructor), OK);

	std::vector<uint8_t> guest_stack(0x8000);
	Kernel::PthreadAttr   attr = nullptr;
	ASSERT_EQ(Kernel::PthreadAttrInit(&attr), OK);
	ASSERT_EQ(Kernel::PthreadAttrSetstack(&attr, guest_stack.data(), guest_stack.size()), OK);

	for (int worker = 0; worker < 3; worker++)
	{
		PthreadKeyEntryContext context {};
		context.key   = key;
		context.value = (worker == 1 ? nullptr : &context);
		std::atomic_int destructor_calls {0};
		ScopedPthreadKeyDestructorCounter destructor_counter(&destructor_calls, &context);

		Kernel::Pthread thread = nullptr;
		ASSERT_EQ(Kernel::PthreadCreate(&thread, &attr, SetPthreadKeySpecific, &context, "key-retire-test"), OK);
		ASSERT_NE(thread, nullptr);
		EXPECT_EQ(Kernel::PthreadJoin(thread, nullptr), OK);
		EXPECT_EQ(context.set_result.load(std::memory_order_acquire), OK);
		EXPECT_EQ(destructor_calls.load(std::memory_order_relaxed), worker == 1 ? 0 : 1);
		ASSERT_NE(Kernel::g_pthread_context, nullptr);
		EXPECT_EQ(Kernel::g_pthread_context->GetPthreadKeys()->CountSpecificValuesForThread(
		              context.worker_thread_id.load(std::memory_order_acquire)),
		          0u);
	}

	EXPECT_EQ(Kernel::PthreadAttrDestroy(&attr), OK);
	EXPECT_EQ(Kernel::PthreadKeyDelete(key), OK);
}

TEST(EmulatorLibcCxxLocale, NumGetParsesUnsignedLongLongAndLeavesDelimiter)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));
	Loader::SymbolResolve query {};
	query.name                 = U"KfcTPbeaOqg";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Object;
	const auto* rec = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(rec, nullptr);

	void* stream_vtable[9] {};
	stream_vtable[7] = reinterpret_cast<void*>(&TestIstreamUnderflow);
	stream_vtable[8] = reinterpret_cast<void*>(&TestIstreamUflow);
	const char input[] = "100%";
	TestIstreambuf stream {stream_vtable, input};
	struct Iterator
	{
		void*         streambuf;
		std::uint64_t failed;
	};
	struct Ios
	{
		std::byte      reserved[0x18];
		std::uint32_t flags;
		std::int32_t  precision;
		std::int32_t  width;
	} ios {};

	auto** vtable_object = reinterpret_cast<void**>(rec->vaddr);
	using DoGet = Iterator(KYTY_SYSV_ABI*)(const void*, Iterator, Iterator, void*, std::uint32_t*, std::uint64_t*);
	auto* do_get = reinterpret_cast<DoGet>(vtable_object[13]);
	ASSERT_NE(do_get, nullptr);
	std::uint32_t state = 0;
	std::uint64_t value = 0;
	Iterator first {&stream, 0};
	Iterator last {nullptr, 1};
	const Iterator result = do_get(vtable_object, first, last, &ios, &state, &value);

	EXPECT_EQ(result.streambuf, &stream);
	EXPECT_EQ(result.failed, 0u);
	EXPECT_EQ(state, 0u);
	EXPECT_EQ(value, 100u);
	EXPECT_EQ(*stream.current, '%');
}

TEST(EmulatorLibcCxxLocale, ResolvesFundamentalTypeinfoAndNumGetVtable)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	auto resolve_obj = [&](const char16_t* nid) -> const Loader::SymbolRecord*
	{
		Loader::SymbolResolve query {};
		query.name                 = nid;
		query.library              = U"libc";
		query.library_version      = 1;
		query.module               = U"libc";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Object;
		return symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	};

	const auto* ti_int   = resolve_obj(u"St4apgcBNfo"); // _ZTIi
	const auto* ti_void  = resolve_obj(u"JrUnjJ-PCTg"); // _ZTIv
	const auto* num_get  = resolve_obj(u"KfcTPbeaOqg"); // _ZTV num_get<char>
	ASSERT_NE(ti_int, nullptr);
	ASSERT_NE(ti_void, nullptr);
	ASSERT_NE(num_get, nullptr);
	ASSERT_NE(ti_int->vaddr, 0u);
	ASSERT_NE(ti_void->vaddr, 0u);
	ASSERT_NE(num_get->vaddr, 0u);
	// Must not be the NoAccess weak-Object sentinel page.
	constexpr uint64_t kInvalidMemorySentinel = 0x840000000ull;
	EXPECT_NE(ti_int->vaddr, kInvalidMemorySentinel);
	EXPECT_NE(ti_void->vaddr, kInvalidMemorySentinel);
	EXPECT_NE(num_get->vaddr, kInvalidMemorySentinel);
	// First typeinfo field is a vtable pointer — must be a readable host address.
	const auto* int_words = reinterpret_cast<const void* const*>(ti_int->vaddr);
	EXPECT_NE(int_words[0], nullptr);
	const auto* void_words = reinterpret_cast<const void* const*>(ti_void->vaddr);
	EXPECT_NE(void_words[0], nullptr);
	const auto* num_get_words = reinterpret_cast<const void* const*>(num_get->vaddr);
	// vtable object: [0]=offset-to-top, [1]=typeinfo*
	EXPECT_NE(num_get_words[1], nullptr);
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
