#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/LinkList.h"
#include "Kyty/Core/MSpace.h"
#include "Kyty/Core/Singleton.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Common.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/ApplicationHeap.h"
#include "Emulator/Libs/CxaDynamicCast.h"
#include "Emulator/Libs/CxxLocale.h"
#include "Emulator/Libs/CxxRtti.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Libs/CxxString.h"
#include "Emulator/Libs/LibCTime.h"
#include "Emulator/Libs/Libs.h"
#include "LibCInternal.h"
#include "Emulator/Libs/Memalign.h"
#include "Emulator/Libs/ProcessEnvironment.h"
#include "Emulator/Libs/Printf.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/GuestCall.h"
#include "Emulator/VideoFrameMemory.h"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <mutex>
#include <limits>
#include <setjmp.h>
#include <string>
#include <strings.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

// Host Itanium C++ ABI / unwinder. Guest code executes natively on the host,
// so guest C++ exceptions are serviced by the real host libstdc++/libgcc
// runtime; each module's .eh_frame is published via __register_frame (see
// RuntimeLinker) so the host unwinder can unwind guest frames. The entry points
// are declared extern "C" directly because <cxxabi.h> does not re-export every
// one of them into abi:: on all toolchains.
#include <exception>
#include <typeinfo>
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <sys/uio.h>
#include <unistd.h>
#endif
#include <unwind.h>

extern "C" {
[[noreturn]] void         __cxa_throw(void* thrown_exception, const std::type_info* tinfo, void (*dest)(void*));
void*                     __cxa_begin_catch(void* exception_object);
void                      __cxa_end_catch();
[[noreturn]] void         __cxa_rethrow();
void*                     __cxa_allocate_exception(size_t thrown_size);
void                      __cxa_free_exception(void* thrown_exception);
const std::type_info*     __cxa_current_exception_type();
void*                     __cxa_get_globals();
void*                     __cxa_get_globals_fast();
void*                     __cxa_allocate_dependent_exception();
void                      __cxa_free_dependent_exception(void* dependent_exception);
int                       __gxx_personality_v0(int version, _Unwind_Action actions, _Unwind_Exception_Class exception_class,
                                               _Unwind_Exception* exception_object, _Unwind_Context* context);
}

// Host libstdc++ Itanium exception-object layouts (stable across GCC releases).
// The four std::exception_ptr helpers (__cxa_current_primary_exception,
// __cxa_rethrow_primary_exception, __cxa_*_exception_refcount) are not present
// in the statically-linked libstdc++, so they are implemented here against the
// same structures the host runtime uses.
struct KytyCxaException
{
	std::type_info*     exceptionType;
	void (*exceptionDestructor)(void*);
	void*               unexpectedHandler;
	void*               terminateHandler;
	KytyCxaException*   nextException;
	int                 handlerCount;
	int                 handlerSwitchValue;
	const unsigned char* actionRecord;
	const unsigned char* languageSpecificData;
	void*               catchTemp;
	void*               adjustedPtr;
	_Unwind_Exception   unwindHeader;
};

struct KytyCxaDependentException
{
	void*               primaryException;
	void (*exceptionDestructor)(void*);
	void*               unexpectedHandler;
	void*               terminateHandler;
	_Unwind_Exception   unwindHeader;
};

struct KytyCxaEhGlobals
{
	KytyCxaException* caughtExceptions;
	unsigned int      uncaughtExceptions;
};

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__GLIBC__)
#include <malloc.h>
#endif

// Orbis uses the FreeBSD amd64 _setjmp layout (72 bytes). UCRT's jmp_buf is
// 256 bytes and has a different layout, so forwarding to host setjmp corrupts
// adjacent guest memory. Save and restore the guest SysV context directly.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
extern "C" KYTY_SYSV_ABI __attribute__((naked)) int kyty_setjmp(void* /*env*/)
{
	__asm__ volatile("movq (%rsp), %rdx\n\t"
	                 "movq %rdx, 0(%rdi)\n\t"
	                 "movq %rbx, 8(%rdi)\n\t"
	                 "movq %rsp, 16(%rdi)\n\t"
	                 "movq %rbp, 24(%rdi)\n\t"
	                 "movq %r12, 32(%rdi)\n\t"
	                 "movq %r13, 40(%rdi)\n\t"
	                 "movq %r14, 48(%rdi)\n\t"
	                 "movq %r15, 56(%rdi)\n\t"
	                 "fnstcw 64(%rdi)\n\t"
	                 "stmxcsr 68(%rdi)\n\t"
	                 "xorl %eax, %eax\n\t"
	                 "ret\n\t");
}

extern "C" KYTY_SYSV_ABI __attribute__((naked)) void kyty_longjmp(void* /*env*/, int /*value*/)
{
	__asm__ volatile("movq %rdi, %rdx\n\t"
	                 "stmxcsr -4(%rsp)\n\t"
	                 "movl 68(%rdx), %eax\n\t"
	                 "andl $0xffffffc0, %eax\n\t"
	                 "movl -4(%rsp), %edi\n\t"
	                 "andl $0x3f, %edi\n\t"
	                 "xorl %eax, %edi\n\t"
	                 "movl %edi, -4(%rsp)\n\t"
	                 "ldmxcsr -4(%rsp)\n\t"
	                 "movl %esi, %eax\n\t"
	                 "movq 0(%rdx), %rcx\n\t"
	                 "movq 8(%rdx), %rbx\n\t"
	                 "movq 16(%rdx), %rsp\n\t"
	                 "movq 24(%rdx), %rbp\n\t"
	                 "movq 32(%rdx), %r12\n\t"
	                 "movq 40(%rdx), %r13\n\t"
	                 "movq 48(%rdx), %r14\n\t"
	                 "movq 56(%rdx), %r15\n\t"
	                 "fldcw 64(%rdx)\n\t"
	                 "testl %eax, %eax\n\t"
	                 "jnz 1f\n\t"
	                 "incl %eax\n\t"
	                 "1:\n\t"
	                 "movq %rcx, (%rsp)\n\t"
	                 "ret\n\t");
}
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace LibC {

LIB_VERSION("libc", 1, "libc", 1, 1);

using Time::GuestTm;
using Time::GuestToHostTm;

// Gen5 libc/libSceLibcInternal "need" flag: non-zero asks the guest CRT to run
// heap/TSD bootstrap. Zero claims "already initialized" and skips that path.
// A title that uses libc's internal mspace before ApplicationHeap create can
// leave the mspace at BSS zero when this stays 0, so operator new
// returns null → bad_alloc → terminate (DebugRaiseException 0xa0020008).
// Keep both LibC and LibcInternal objects in sync.
uint32_t g_need_flag = 1;

struct CxaDestructor
{
	cxa_destructor_func_t destructor_func;
	void*                 destructor_object;
	void*                 module_id;
};

struct CContext
{
	Core::List<CxaDestructor> cxa;
};

// __cxa_atexit may be reached by independently scheduled guest modules. Keep
// registration and claim state together, but never retain this lock while a
// guest destructor runs: destructors are allowed to register/finalize more
// callbacks themselves.
std::mutex g_cxa_mutex;

// A title that gives up after an internal failure often just calls exit(0);
// record who asked, so the run is not mistaken for a host crash or stall.
static KYTY_SYSV_ABI void exit(int code)
{
	KYTY_LOG_WARN("guest exit(%d) from 0x%016" PRIx64 "\n", code, reinterpret_cast<uint64_t>(__builtin_return_address(0)));

	::exit(code);
}

static KYTY_SYSV_ABI void init_env(const ProcessEnvironment::InitParameters* parameters)
{
	PRINT_NAME();

	(void)ProcessEnvironment::Initialize(parameters);
	if (!LibKernel::ApplicationHeap::InitializeProcessHeap(Emulator::GuestRuntimePort::GetProcessParameters()))
	{
		EXIT("libc process allocator metadata or initialization failed: %s\n", LibKernel::ApplicationHeap::ProcessHeapFailureReason());
	}
}

// The C++ runtime uses _Cnd_t as a pointer to the kernel condition object.
// _Cnd_init receives the address of that handle and owns its allocation; use
// the same condition implementation as the public pthread ABI so both paths
// share lifetime and error handling.
int c_thread_sync_result(int result);
static Kernel::KernelUseconds c_abstime_remaining_usec(const Kernel::KernelTimespec* abstime);

KYTY_SYSV_ABI int c_cnd_init(Kernel::PthreadCond* cond)
{
	return c_thread_sync_result(Kernel::PthreadCondInit(cond, nullptr, nullptr));
}

KYTY_SYSV_ABI int c_cnd_init_with_name(Kernel::PthreadCond* cond, const char* name)
{
	return c_thread_sync_result(Kernel::PthreadCondInit(cond, nullptr, name));
}

KYTY_SYSV_ABI int c_cnd_init_with_default_name_override(Kernel::PthreadCond* cond, const char* name)
{
	return c_cnd_init_with_name(cond, name);
}

enum class CThreadResult : int
{
	Success  = 0,
	TimedOut = 2,
	Busy     = 3,
	Error    = 4,
};

int c_thread_sync_result(int result)
{
	switch (result)
	{
		case OK: return static_cast<int>(CThreadResult::Success);
		case LibKernel::KERNEL_ERROR_ETIMEDOUT: return static_cast<int>(CThreadResult::TimedOut);
		case LibKernel::KERNEL_ERROR_EBUSY: return static_cast<int>(CThreadResult::Busy);
		default: return static_cast<int>(CThreadResult::Error);
	}
}

KYTY_SYSV_ABI int c_cnd_broadcast(Kernel::PthreadCond* cond)
{
	return c_thread_sync_result(Kernel::PthreadCondBroadcast(cond));
}

KYTY_SYSV_ABI int c_cnd_signal(Kernel::PthreadCond* cond)
{
	return c_thread_sync_result(Kernel::PthreadCondSignal(cond));
}

KYTY_SYSV_ABI int c_cnd_wait(Kernel::PthreadCond* cond, Kernel::PthreadMutex* mutex)
{
	return c_thread_sync_result(Kernel::PthreadCondWait(cond, mutex));
}

KYTY_SYSV_ABI int c_cnd_timedwait(Kernel::PthreadCond* cond, Kernel::PthreadMutex* mutex,
                                         const Kernel::KernelTimespec* abstime)
{
	return c_thread_sync_result(Kernel::PthreadCondTimedwaitAbsolute(cond, mutex, abstime));
}

KYTY_SYSV_ABI void c_cnd_destroy(Kernel::PthreadCond* cond)
{
	if (cond == nullptr)
	{
		return;
	}

	auto* private_cond = *cond;
	if (private_cond != nullptr && reinterpret_cast<uintptr_t>(private_cond) >= 0x100000)
	{
		(void)Kernel::PthreadCondDestroy(cond);
	}
}

static KYTY_SYSV_ABI int c_pthread_equal(Kernel::Pthread thread1, Kernel::Pthread thread2)
{
	return Kernel::PthreadEqual(thread1, thread2);
}
KYTY_SYSV_ABI int c_fstat(int fd, Kernel::FileSystem::FileStat* sb)
{
	return POSIX_CALL(Kernel::FileSystem::KernelFstat(fd, sb));
}
// The guest wchar_t is 16 bits wide.
static KYTY_SYSV_ABI int c_wcscmp(const uint16_t* s1, const uint16_t* s2)
{
	for (;; s1++, s2++)
	{
		if (*s1 != *s2)
		{
			return *s1 < *s2 ? -1 : 1;
		}
		if (*s1 == 0)
		{
			return 0;
		}
	}
}
static KYTY_SYSV_ABI void c_perror(const char* s)
{
	::perror(s);
}
static KYTY_SYSV_ABI int c_getc(FILE* f)
{
	return c_fgetc(f);
}
static KYTY_SYSV_ABI void c_srand(unsigned int seed)
{
	::srand(seed);
}
// Gen5 libc_v1 rand (Nmtr628eA3A): first Unpatched after Global Heap create.
static KYTY_SYSV_ABI int c_rand()
{
	return ::rand();
}
// drand48 family — 48-bit linear-congruential PRNG, independent state from rand().
static KYTY_SYSV_ABI void c_srand48(long seed)
{
	::srand48(seed);
}
static KYTY_SYSV_ABI double c_drand48()
{
	return ::drand48();
}
static KYTY_SYSV_ABI long c_lrand48()
{
	return ::lrand48();
}
static KYTY_SYSV_ABI long c_mrand48()
{
	return ::mrand48();
}

// --- libc_v1 math/time/stdio imports required by IL2CPP modules -------------
static KYTY_SYSV_ABI double c_difftime(time_t end, time_t beg)
{
	return ::difftime(end, beg);
}
static KYTY_SYSV_ABI double c_logb(double x)
{
	return ::logb(x);
}
static KYTY_SYSV_ABI double c_scalbn(double x, int n)
{
	return ::scalbn(x, n);
}
static KYTY_SYSV_ABI double c_log10(double x)
{
	return ::log10(x);
}
static KYTY_SYSV_ABI double c_log2(double x)
{
	return ::log2(x);
}
static KYTY_SYSV_ABI int c_ungetc(int ch, FILE* f)
{
	return (f != nullptr ? ::ungetc(ch, f) : EOF);
}
static KYTY_SYSV_ABI int c_fgetpos(FILE* f, fpos_t* pos)
{
	return (f != nullptr && pos != nullptr ? ::fgetpos(f, pos) : -1);
}
static KYTY_SYSV_ABI int c_fsetpos(FILE* f, const fpos_t* pos)
{
	return (f != nullptr && pos != nullptr ? ::fsetpos(f, pos) : -1);
}
static KYTY_SYSV_ABI void c_quick_exit(int code)
{
	KYTY_LOG_WARN("guest quick_exit(%d) from 0x%016" PRIx64 "\n", code, reinterpret_cast<uint64_t>(__builtin_return_address(0)));
	::quick_exit(code);
}
// Gen5 libc_v1 strtok (oVkZ8W8-Q8A): host uses strtok_r with a per-thread save pointer.
static KYTY_SYSV_ABI char* c_strtok(char* str, const char* delim)
{
	static thread_local char* save = nullptr;
	return ::strtok_r(str, delim, &save);
}

// C++ operator new/delete, same ownership as libc malloc.
static constexpr uint8_t g_cxx_nothrow = 0;

static KYTY_SYSV_ABI void* cxx_new(size_t size)
{
	return allocate_with_owner(size != 0 ? size : 1);
}
static KYTY_SYSV_ABI void* cxx_new_nothrow(size_t size, const void* /*nothrow_tag*/)
{
	return allocate_with_owner(size != 0 ? size : 1);
}
static KYTY_SYSV_ABI void cxx_delete(void* p)
{
	if (!free_by_owner(p))
	{
		EXIT("ApplicationHeap delete failed\n");
	}
}
static KYTY_SYSV_ABI void cxx_delete_sized(void* p, size_t /*size*/)
{
	cxx_delete(p);
}
static KYTY_SYSV_ABI void cxx_delete_sized_aligned(void* p, size_t /*size*/, size_t /*alignment*/)
{
	cxx_delete(p);
}
static KYTY_SYSV_ABI void* cxx_new_array(size_t size)
{
	return allocate_with_owner(size != 0 ? size : 1);
}
static KYTY_SYSV_ABI void* cxx_new_array_nothrow(size_t size, const void* /*nothrow_tag*/)
{
	return allocate_with_owner(size != 0 ? size : 1);
}
static KYTY_SYSV_ABI void cxx_delete_array(void* p)
{
	if (!free_by_owner(p))
	{
		EXIT("ApplicationHeap delete[] failed\n");
	}
}
static KYTY_SYSV_ABI void cxx_delete_array_sized(void* p, size_t /*size*/)
{
	cxx_delete_array(p);
}

static std::atomic<uint32_t>* c_atomic_4_ref(uint32_t* value)
{
	EXIT_IF(value == nullptr || (reinterpret_cast<uintptr_t>(value) % alignof(uint32_t)) != 0);
	static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
	static_assert(alignof(std::atomic<uint32_t>) <= alignof(uint32_t));
	return reinterpret_cast<std::atomic<uint32_t>*>(value);
}

static KYTY_SYSV_ABI uint32_t c_atomic_load_4(const uint32_t* value)
{
	return c_atomic_4_ref(const_cast<uint32_t*>(value))->load(std::memory_order_seq_cst);
}

static KYTY_SYSV_ABI int c_atomic_compare_exchange_weak_4(uint32_t* value, uint32_t* expected, uint32_t desired)
{
	EXIT_IF(expected == nullptr);
	uint32_t observed = *expected;
	const bool exchanged =
	    c_atomic_4_ref(value)->compare_exchange_weak(observed, desired, std::memory_order_seq_cst, std::memory_order_seq_cst);
	if (!exchanged)
	{
		*expected = observed;
	}
	return exchanged ? 1 : 0;
}

static KYTY_SYSV_ABI uint32_t c_atomic_fetch_add_4(uint32_t* value, uint32_t operand)
{
	return c_atomic_4_ref(value)->fetch_add(operand, std::memory_order_seq_cst);
}

static KYTY_SYSV_ABI uint32_t c_atomic_fetch_sub_4(uint32_t* value, uint32_t operand)
{
	return c_atomic_4_ref(value)->fetch_sub(operand, std::memory_order_seq_cst);
}

static KYTY_SYSV_ABI uint32_t c_thread_hardware_concurrency()
{
	constexpr uint32_t kGuestHardwareThreads = 8;
	return kGuestHardwareThreads;
}

static KYTY_SYSV_ABI int c_thread_join(Kernel::Pthread thread, int* result)
{
	void* joined_value = nullptr;
	if (Kernel::PthreadJoin(thread, &joined_value) != OK)
	{
		return static_cast<int>(CThreadResult::Error);
	}

	if (result != nullptr)
	{
		*result = static_cast<int>(reinterpret_cast<intptr_t>(joined_value));
	}
	return static_cast<int>(CThreadResult::Success);
}

static KYTY_SYSV_ABI void c_thread_yield()
{
	Kernel::PthreadYield();
}

// --- Additional string / memory ---------------------------------------------
static KYTY_SYSV_ABI int c_bcmp(const void* a, const void* b, size_t n)
{
	return ::memcmp(a, b, n);
}
static KYTY_SYSV_ABI char* c_strerror(int e)
{
	return ::strerror(e);
}
static KYTY_SYSV_ABI int c_strerror_r(int e, char* destination, size_t size)
{
	if (destination == nullptr)
	{
		return Posix::POSIX_EINVAL;
	}
	if (size == 0)
	{
		return Posix::POSIX_ERANGE;
	}

	const char* message = ::strerror(e);
	if (message == nullptr)
	{
		destination[0] = '\0';
		return Posix::POSIX_EINVAL;
	}

	const size_t message_size = std::strlen(message);
	const size_t copy_size    = std::min(message_size, size - 1u);
	std::memcpy(destination, message, copy_size);
	destination[copy_size] = '\0';
	return (message_size < size ? 0 : Posix::POSIX_ERANGE);
}
// strncpy_s(dst, dstsz, src, count) -> errno_t (0 on success)
static KYTY_SYSV_ABI int c_strncpy_s(char* d, size_t dn, const char* s, size_t n)
{
	if (d == nullptr || dn == 0)
	{
		return 22; // EINVAL
	}
	size_t i = 0;
	for (; i < n && i + 1 < dn && s != nullptr && s[i] != '\0'; i++)
	{
		d[i] = s[i];
	}
	d[i] = '\0';
	return 0;
}

// --- C locale character tables -----------------------------------------------
// The guest ABI uses fixed classification bits and an EOF entry immediately
// before the byte-indexed table. Keep this data independent of the host locale.
using CtypeTable = std::array<std::uint16_t, 257>;

constexpr CtypeTable MakeCtypeTable()
{
	CtypeTable table {};
	for (int c = 0; c < 128; ++c)
	{
		std::uint16_t mask = 0;
		if (c <= 0x08 || (c >= 0x0e && c <= 0x1f) || c == 0x7f)
		{
			mask |= 0x080;
		}
		if (c >= 0x09 && c <= 0x0d)
		{
			mask |= 0x0c0;
		}
		if (c == '\t')
		{
			mask |= 0x400;
		}
		if (c == ' ')
		{
			mask |= 0x004;
		}
		if ((c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~'))
		{
			mask |= 0x008;
		}
		if (c >= '0' && c <= '9')
		{
			mask |= 0x021;
		}
		if (c >= 'A' && c <= 'Z')
		{
			mask |= static_cast<std::uint16_t>(0x002 | (c <= 'F' ? 0x001 : 0));
		}
		if (c >= 'a' && c <= 'z')
		{
			mask |= static_cast<std::uint16_t>(0x010 | (c <= 'f' ? 0x001 : 0));
		}
		table[static_cast<std::size_t>(c) + 1] = mask;
	}
	return table;
}

constexpr CtypeTable g_c_locale_ctype = MakeCtypeTable();

constexpr char g_c_locale_ampm[] =
    ":AM:PM";
constexpr char g_c_locale_weekdays[] =
    ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday";
constexpr char g_c_locale_months[] =
    ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:Sep:September:Oct:October:Nov:"
    "November:Dec:December";
constexpr char g_c_locale_time_formats[] =
    "|%a %b %e %T %Y|%m/%d/%y|%H:%M:%S|%I:%M:%S %p";
constexpr char g_c_locale_empty[] = "";

// The runtime exposes 21 stable pointers. Repeated entries represent the same
// C-locale data for independent time-format categories.
constexpr std::array<const char*, 21> g_c_locale_times = {
    g_c_locale_ampm,
    g_c_locale_weekdays,
    g_c_locale_weekdays,
    g_c_locale_weekdays,
    g_c_locale_months,
    g_c_locale_months,
    g_c_locale_months,
    g_c_locale_time_formats,
    g_c_locale_time_formats,
    g_c_locale_time_formats,
    g_c_locale_time_formats,
    g_c_locale_time_formats,
    g_c_locale_time_formats,
    g_c_locale_time_formats,
    g_c_locale_time_formats,
    g_c_locale_time_formats,
    g_c_locale_time_formats,
    g_c_locale_empty,
    g_c_locale_empty,
    g_c_locale_empty,
    g_c_locale_empty,
};

static KYTY_SYSV_ABI const unsigned short* c_Getpctype()
{
	return g_c_locale_ctype.data() + 1;
}

static KYTY_SYSV_ABI const char* const* c_Getptimes()
{
	return g_c_locale_times.data();
}

static const std::array<short, 384>& c_Getptoupper_table()
{
	static const std::array<short, 384> table = [] {
		std::array<short, 384> values {};
		for (int c = -1; c < 256; c++)
		{
			values[c + 1] = (c >= 0) ? static_cast<short>(::toupper(c)) : 0;
		}
		return values;
	}();
	return table;
}

static const std::array<short, 384>& c_Getptolower_table()
{
	static const std::array<short, 384> table = [] {
		std::array<short, 384> values {};
		for (int c = -1; c < 256; c++)
		{
			values[c + 1] = (c >= 0) ? static_cast<short>(::tolower(c)) : 0;
		}
		return values;
	}();
	return table;
}

static KYTY_SYSV_ABI const short* c_Getptoupper()
{
	return c_Getptoupper_table().data() + 1;
}

// Gen5 libc_v1 _Getptolower — NID 1uJgoVq3bQU. Same table contract as
// _Getptoupper: short[384] centered so index 0 is EOF (-1). A guest
// Construct VFS lowercases asset names with:
//   table = _Getptolower();  dest[i] = (uint8_t)table[(unsigned char)src[i]];
// Returning a non-table pointer corrupted "data.js" and the project parse hit EOF.
static KYTY_SYSV_ABI const short* c_Getptolower()
{
	return c_Getptolower_table().data() + 1;
}

static KYTY_SYSV_ABI std::mbstate_t* c_Getpmbstate()
{
	static std::mbstate_t state {};
	return &state;
}

static KYTY_SYSV_ABI std::mbstate_t* c_Getpwcstate()
{
	static std::mbstate_t state {};
	return &state;
}

// Gen5 libc_v1 _Getmbcurmax — maximum bytes per multibyte character in the
// current locale. Kyty models the guest locale with the host process locale
// (c_setlocale forwards to the host), so MB_CUR_MAX is the live value.
static KYTY_SYSV_ABI size_t c_Getmbcurmax()
{
	return MB_CUR_MAX;
}

// --- printf / scanf family ---------------------------------------------------
// Every formatted output export converts the guest VaList through Kyty's own
// formatter (Printf::Format). The guest register-save area is never handed to the
// host libc formatter: that walks memory with host assumptions and faults on the
// guest's frame. A large but finite cap stands in for the unbounded sprintf/
// vsprintf buffer contract, which trusts the caller-provided destination.
static constexpr size_t C_UNBOUNDED_FORMAT = 0x10000;

static KYTY_SYSV_ABI int c_snprintf(VA_ARGS)
{
	VA_CONTEXT(ctx);
	char*       s   = VaArg_ptr<char>(&ctx.va_list);
	size_t      n   = VaArg_size_t(&ctx.va_list);
	const char* fmt = VaArg_ptr<const char>(&ctx.va_list);
	return Format(s, n, fmt, &ctx.va_list);
}

static KYTY_SYSV_ABI int c_snprintf_s(VA_ARGS)
{
	VA_CONTEXT(ctx);
	char*       output      = VaArg_ptr<char>(&ctx.va_list);
	size_t      output_size = VaArg_size_t(&ctx.va_list);
	const char* format      = VaArg_ptr<const char>(&ctx.va_list);
	if (output == nullptr || output_size == 0 || format == nullptr)
	{
		return -1;
	}
	return Format(output, output_size, format, &ctx.va_list);
}

static KYTY_SYSV_ABI int c_sprintf(VA_ARGS)
{
	VA_CONTEXT(ctx);
	char*       s   = VaArg_ptr<char>(&ctx.va_list);
	const char* fmt = VaArg_ptr<const char>(&ctx.va_list);
	return Format(s, C_UNBOUNDED_FORMAT, fmt, &ctx.va_list);
}
// Gen5 sprintf_s — NID xEszJVGpybs: buffer, size, format, ...
static KYTY_SYSV_ABI int c_sprintf_s(VA_ARGS)
{
	VA_CONTEXT(ctx);
	char*       s   = VaArg_ptr<char>(&ctx.va_list);
	size_t      n   = VaArg_size_t(&ctx.va_list);
	const char* fmt = VaArg_ptr<const char>(&ctx.va_list);
	if (s == nullptr || n == 0 || fmt == nullptr)
	{
		return -1;
	}
	return Format(s, n, fmt, &ctx.va_list);
}
static KYTY_SYSV_ABI int c_fprintf(VA_ARGS)
{
	VA_CONTEXT(ctx);
	FILE*       f   = VaArg_ptr<FILE>(&ctx.va_list);
	const char* fmt = VaArg_ptr<const char>(&ctx.va_list);

	char      buffer[C_UNBOUNDED_FORMAT];
	const int written = Format(buffer, sizeof(buffer), fmt, &ctx.va_list);

	if (f == nullptr || written < 0)
	{
		return written;
	}
	c_fwrite(buffer, 1, static_cast<size_t>(written), f);
	return written;
}
static KYTY_SYSV_ABI int c_vfprintf(VA_ARGS)
{
	VA_CONTEXT(ctx);
	FILE*       f   = VaArg_ptr<FILE>(&ctx.va_list);
	const char* fmt = VaArg_ptr<const char>(&ctx.va_list);
	if (f == nullptr || fmt == nullptr)
	{
		return -1;
	}
	char      buffer[C_UNBOUNDED_FORMAT];
	const int written = Format(buffer, sizeof(buffer), fmt, &ctx.va_list);
	if (written < 0)
	{
		return written;
	}
	c_fwrite(buffer, 1, static_cast<size_t>(written), f);
	return written;
}
// scanf parses a guest input string into guest output pointers. Kyty has no input
// converter yet; forward to the host, which reads the guest string and writes back
// through the pointer arguments. This is input parsing, not output formatting.
static KYTY_SYSV_ABI int c_sscanf(VA_ARGS)
{
	VA_CONTEXT(ctx);
	const char* s   = VaArg_ptr<const char>(&ctx.va_list);
	const char* fmt = VaArg_ptr<const char>(&ctx.va_list);
	return ::vsscanf(s, fmt, *reinterpret_cast<va_list*>(&ctx.va_list));
}
// Gen5 sscanf_s — NID 24m4Z4bUaoY. Annex K requires rsize after %s/%c/%[ destinations;
// integer formats match sscanf. Forward identically for now; refine if a title
// supplies sized string conversions that mis-parse under host vsscanf.
static KYTY_SYSV_ABI int c_sscanf_s(VA_ARGS)
{
	VA_CONTEXT(ctx);
	const char* s   = VaArg_ptr<const char>(&ctx.va_list);
	const char* fmt = VaArg_ptr<const char>(&ctx.va_list);
	return ::vsscanf(s, fmt, *reinterpret_cast<va_list*>(&ctx.va_list));
}

// Gen5 clock — NID QZP6I9ZZxpE. Observed as seed input XOR rdtscp.
static KYTY_SYSV_ABI int64_t c_clock()
{
	return static_cast<int64_t>(::clock());
}
static KYTY_SYSV_ABI int c_vsprintf(char* s, const char* fmt, VaList* ap)
{
	return Format(s, C_UNBOUNDED_FORMAT, fmt, ap);
}
KYTY_SYSV_ABI int c_vsnprintf(char* s, size_t n, const char* fmt, VaList* ap)
{
	return Format(s, n, fmt, ap);
}
// Gen5 vsprintf_s — NID +qitMEbkSWk: buffer, element count, format, va_list.
static KYTY_SYSV_ABI int c_vsprintf_s(char* s, size_t n, const char* fmt, VaList* ap)
{
	if (s == nullptr || n == 0 || fmt == nullptr)
	{
		return -1;
	}
	return Format(s, n, fmt, ap);
}
static KYTY_SYSV_ABI int c_vsnprintf_s(char* s, size_t dn, size_t count, const char* fmt, VaList* ap)
{
	size_t n = (count + 1 < dn) ? count + 1 : dn;
	return Format(s, n, fmt, ap);
}

// C: vswprintf fails with a negative result when the output and its terminator
// do not fit; callers such as string builders grow the buffer and retry.
static KYTY_SYSV_ABI int c_vswprintf(uint16_t* out, size_t out_count, const uint16_t* wide_format, VaList* ap)
{
	if (out == nullptr || out_count == 0 || wide_format == nullptr || ap == nullptr)
	{
		return -1;
	}
	const int length = FormatWide(out, out_count, wide_format, ap);
	return length < 0 || static_cast<size_t>(length) >= out_count ? -1 : length;
}

// --- stdlib ------------------------------------------------------------------
static KYTY_SYSV_ABI double c_strtod(const char* s, char** e)
{
	return ::strtod(s, e);
}
static KYTY_SYSV_ABI float c_strtof(const char* s, char** e)
{
	return ::strtof(s, e);
}
static KYTY_SYSV_ABI long c_strtol(const char* s, char** e, int b)
{
	return ::strtol(s, e, b);
}
static KYTY_SYSV_ABI unsigned long c_strtoul(const char* s, char** e, int b)
{
	return ::strtoul(s, e, b);
}
static KYTY_SYSV_ABI long long c_strtoll(const char* s, char** e, int b)
{
	return ::strtoll(s, e, b);
}

// Gen5 libc_v1 strtoull — NID 5OqszGpy7Mg.
static KYTY_SYSV_ABI unsigned long long c_strtoull(const char* s, char** e, int b)
{
	return ::strtoull(s, e, b);
}
static KYTY_SYSV_ABI double c_atof(const char* s)
{
	return ::atof(s);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
using GuestQsortCompare = int(KYTY_SYSV_ABI*)(const void*, const void*);
static thread_local GuestQsortCompare g_guest_qsort_compare = nullptr;

static int qsort_compare_bridge(const void* lhs, const void* rhs)
{
	EXIT_IF(g_guest_qsort_compare == nullptr);
	return g_guest_qsort_compare(lhs, rhs);
}
#endif

static KYTY_SYSV_ABI void c_qsort(void* base, size_t n, size_t sz, int(KYTY_SYSV_ABI* cmp)(const void*, const void*))
{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	auto previous         = g_guest_qsort_compare;
	g_guest_qsort_compare = cmp;
	::qsort(base, n, sz, qsort_compare_bridge);
	g_guest_qsort_compare = previous;
#else
	::qsort(base, n, sz, reinterpret_cast<int (*)(const void*, const void*)>(cmp));
#endif
}

static KYTY_SYSV_ABI void* c_bsearch(const void* key, const void* base, size_t count, size_t size,
                                     int(KYTY_SYSV_ABI* compare)(const void*, const void*))
{
	if (base == nullptr || compare == nullptr || count == 0 || size == 0 || count > std::numeric_limits<size_t>::max() / size)
	{
		return nullptr;
	}

	const auto* bytes = static_cast<const uint8_t*>(base);
	size_t first = 0;
	while (count != 0)
	{
		const size_t half = count / 2;
		const size_t middle = first + half;
		const void* element = bytes + middle * size;
		const int relation = compare(key, element);
		if (relation == 0)
		{
			return const_cast<void*>(element);
		}
		if (relation < 0)
		{
			count = half;
			continue;
		}
		first = middle + 1;
		count -= half + 1;
	}

	return nullptr;
}
static KYTY_SYSV_ABI void c_abort()
{
	KYTY_LOG_ERROR("libc::abort() called by guest from 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(__builtin_return_address(0)));
	::abort();
}

namespace {

std::mutex                                       g_static_init_mutex;
std::condition_variable                          g_static_init_cv;
std::unordered_map<const void*, std::thread::id> g_static_init_owner;

} // namespace

// --- C++ runtime -------------------------------------------------------------
// Process-wide static initialization guards. The guard word uses bit 0 for
// completion and the second byte for an initializer in progress. The owner map
// is only populated while an initializer is active; it prevents a guest thread
// from waiting on a guard that it already owns.
static void static_init_claim_locked(const void* key)
{
	g_static_init_owner[key] = std::this_thread::get_id();
}

static KYTY_SYSV_ABI int c_cxa_guard_acquire(uint64_t* g)
{
	if (g == nullptr)
	{
		return 0;
	}
	auto* guard = reinterpret_cast<std::atomic<uint64_t>*>(g);
	for (;;)
	{
		uint64_t val = guard->load(std::memory_order_acquire);
		if ((val & 0x01) != 0)
		{
			return 0;
		}

		std::unique_lock lock(g_static_init_mutex);
		val = guard->load(std::memory_order_acquire);
		if ((val & 0x01) != 0)
		{
			return 0;
		}
		if ((val & 0xFF00) == 0)
		{
			guard->store(val | 0x0100, std::memory_order_release);
			static_init_claim_locked(g);
			return 1;
		}
		const auto owner = g_static_init_owner.find(g);
		if (owner != g_static_init_owner.end() && owner->second == std::this_thread::get_id())
		{
			KYTY_LOG_WARN(FG_BRIGHT_YELLOW "libc: recursive static initialization guard %p skipped by owner thread" DEFAULT "\n",
			       static_cast<void*>(g));
			return 0;
		}
		g_static_init_cv.wait(lock, [guard] { return (guard->load(std::memory_order_acquire) & 0xFF01) != 0x0100; });
	}
}
static KYTY_SYSV_ABI void c_cxa_guard_release(uint64_t* g)
{
	if (g == nullptr)
	{
		return;
	}
	auto* guard = reinterpret_cast<std::atomic<uint64_t>*>(g);
	{
		std::lock_guard lock(g_static_init_mutex);
		const uint64_t  val = guard->load(std::memory_order_relaxed);
		guard->store((val & ~static_cast<uint64_t>(0xFFFF)) | 0x0001, std::memory_order_release);
		g_static_init_owner.erase(g);
	}
	g_static_init_cv.notify_all();
}
static KYTY_SYSV_ABI void c_cxa_guard_abort(uint64_t* g)
{
	if (g == nullptr)
	{
		return;
	}
	auto* guard = reinterpret_cast<std::atomic<uint64_t>*>(g);
	{
		std::lock_guard lock(g_static_init_mutex);
		const uint64_t  val = guard->load(std::memory_order_relaxed);
		guard->store(val & ~static_cast<uint64_t>(0xFFFF), std::memory_order_release);
		g_static_init_owner.erase(g);
	}
	g_static_init_cv.notify_all();
}

using execute_once_callback_t = KYTY_SYSV_ABI int (*)(void*, void*, void**);

KYTY_SYSV_ABI int c_execute_once(int* flag, execute_once_callback_t callback, void* context)
{
	PRINT_NAME();

	if (flag == nullptr || callback == nullptr)
	{
		return 0;
	}

	// PS5 libc's once_flag follows the three-state ABI: zero has not started,
	// one is executing, and two is permanently complete. The callback receives
	// the once flag itself as its first argument.
	constexpr int once_uninitialized = 0;
	constexpr int once_running       = 1;
	constexpr int once_complete      = 2;

	{
		std::unique_lock lock(g_static_init_mutex);
		g_static_init_cv.wait(lock, [flag] {
			return reinterpret_cast<std::atomic<uint32_t>*>(flag)->load(std::memory_order_acquire) != once_running;
		});
		auto* once_flag = reinterpret_cast<std::atomic<uint32_t>*>(flag);
		if (once_flag->load(std::memory_order_acquire) == once_complete)
		{
			return 0;
		}
		once_flag->store(once_running, std::memory_order_release);
		static_init_claim_locked(flag);
	}

	void*     callback_result = nullptr;
	const int result          = callback(flag, context, &callback_result);

	{
		std::lock_guard lock(g_static_init_mutex);
		auto*            once_flag = reinterpret_cast<std::atomic<uint32_t>*>(flag);
		once_flag->store(result != 0 ? once_complete : once_uninitialized, std::memory_order_release);
		g_static_init_owner.erase(flag);
	}
	g_static_init_cv.notify_all();
	return result != 0 ? 0 : LibKernel::KERNEL_ERROR_EAGAIN;
}

struct ThreadAtexitEntry
{
	cxa_destructor_func_t destructor;
	void* object;
	void* dso_handle;
};

thread_local std::vector<ThreadAtexitEntry> g_thread_atexit_entries;
thread_local bool                           g_running_thread_atexit = false;
std::once_flag                              g_thread_atexit_hook_once;

static void run_thread_atexit_destructors(void* guest_stack_top)
{
	EXIT_IF(guest_stack_top == nullptr);
	if (g_running_thread_atexit)
	{
		return;
	}

	g_running_thread_atexit = true;
	while (!g_thread_atexit_entries.empty())
	{
		const auto entry = g_thread_atexit_entries.back();
		g_thread_atexit_entries.pop_back();
		if (entry.destructor != nullptr)
		{
			Loader::GuestCall::InvokeOnStack(reinterpret_cast<uint64_t>(entry.destructor),
			                                 reinterpret_cast<uint64_t>(entry.object), 0, 0, guest_stack_top);
		}
	}
	g_running_thread_atexit = false;
}

KYTY_SYSV_ABI int c_cxa_thread_atexit(cxa_destructor_func_t dtor, void* obj, void* dso_handle)
{
	if (dtor == nullptr)
	{
		return -1;
	}

	std::call_once(g_thread_atexit_hook_once, [] { Kernel::PthreadSetHostThreadDtors(run_thread_atexit_destructors); });
	g_thread_atexit_entries.push_back({dtor, obj, dso_handle});
	return 0;
}

KYTY_SYSV_ABI int c_mtx_init(Kernel::PthreadMutex* mutex, int type)
{
	PRINT_NAME();

	if (mutex == nullptr)
	{
		return static_cast<int>(CThreadResult::Error);
	}

	constexpr int mtx_recursive = 0x100;
	if ((type & mtx_recursive) == 0)
	{
		return c_thread_sync_result(Kernel::PthreadMutexInit(mutex, nullptr, nullptr));
	}

	Kernel::PthreadMutexattr attr = nullptr;
	int result                       = Kernel::PthreadMutexattrInit(&attr);
	if (result == OK)
	{
		result = Kernel::PthreadMutexattrSettype(&attr, 2);
	}
	if (result == OK)
	{
		result = Kernel::PthreadMutexInit(mutex, &attr, nullptr);
	}
	if (attr != nullptr)
	{
		(void)Kernel::PthreadMutexattrDestroy(&attr);
	}

	return c_thread_sync_result(result);
}

KYTY_SYSV_ABI int c_mtx_init_with_name(Kernel::PthreadMutex* mutex, int type, const char* name)
{
	PRINT_NAME();

	if (mutex == nullptr)
	{
		return static_cast<int>(CThreadResult::Error);
	}

	constexpr int mtx_recursive = 0x100;
	if ((type & mtx_recursive) == 0)
	{
		return c_thread_sync_result(Kernel::PthreadMutexInit(mutex, nullptr, name));
	}

	Kernel::PthreadMutexattr attr = nullptr;
	int result                       = Kernel::PthreadMutexattrInit(&attr);
	if (result == OK)
	{
		result = Kernel::PthreadMutexattrSettype(&attr, 2);
	}
	if (result == OK)
	{
		result = Kernel::PthreadMutexInit(mutex, &attr, name);
	}
	if (attr != nullptr)
	{
		(void)Kernel::PthreadMutexattrDestroy(&attr);
	}

	return c_thread_sync_result(result);
}

KYTY_SYSV_ABI int c_mtx_init_with_default_name_override(Kernel::PthreadMutex* mutex, int type, const char* name)
{
	PRINT_NAME();

	return c_mtx_init_with_name(mutex, type, name);
}

KYTY_SYSV_ABI void c_mtx_destroy(Kernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	if (mutex == nullptr)
	{
		return;
	}

	auto* private_mutex = *mutex;
	if (private_mutex != nullptr && reinterpret_cast<uintptr_t>(private_mutex) >= 0x100000)
	{
		(void)Kernel::PthreadMutexDestroy(mutex);
	}
}

KYTY_SYSV_ABI int c_mtx_lock(Kernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return c_thread_sync_result(Kernel::PthreadMutexLock(mutex));
}

KYTY_SYSV_ABI int c_mtx_trylock(Kernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return c_thread_sync_result(Kernel::PthreadMutexTrylock(mutex));
}

static Kernel::KernelUseconds c_abstime_remaining_usec(const Kernel::KernelTimespec* abstime)
{
	Kernel::KernelTimespec now {};
	if (abstime == nullptr || Kernel::KernelClockGettime(0, &now) != OK)
	{
		return 0;
	}

	const int64_t now_us = now.tv_sec * 1000000 + now.tv_nsec / 1000;
	const int64_t abs_us = abstime->tv_sec * 1000000 + abstime->tv_nsec / 1000;
	if (abs_us <= now_us)
	{
		return 0;
	}

	const int64_t delta = abs_us - now_us;
	if (delta > static_cast<int64_t>(UINT32_MAX))
	{
		return UINT32_MAX;
	}
	return static_cast<Kernel::KernelUseconds>(delta);
}

KYTY_SYSV_ABI int c_mtx_timedlock(Kernel::PthreadMutex* mutex, const Kernel::KernelTimespec* abstime)
{
	PRINT_NAME();

	return c_thread_sync_result(Kernel::PthreadMutexTimedlock(mutex, c_abstime_remaining_usec(abstime)));
}

KYTY_SYSV_ABI int c_mtx_unlock(Kernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return c_thread_sync_result(Kernel::PthreadMutexUnlock(mutex));
}

KYTY_SYSV_ABI int c_mtx_current_owns(Kernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return Kernel::PthreadMutexCurrentOwns(mutex) ? 1 : 0;
}

static void c_thread_require(int result, const char* operation)
{
	if (result != static_cast<int>(CThreadResult::Success))
	{
		EXIT("C thread %s failed with result %d\n", operation, result);
	}
}

struct CndThreadExitEntry
{
	Kernel::PthreadCond*  condition;
	Kernel::PthreadMutex* mutex;
	int*                     completed;
};

thread_local std::vector<CndThreadExitEntry> g_cnd_thread_exit_entries;

KYTY_SYSV_ABI void c_cnd_register_at_thread_exit(Kernel::PthreadCond* condition,
                                                        Kernel::PthreadMutex* mutex, int* completed)
{
	EXIT_IF(condition == nullptr || mutex == nullptr);
	g_cnd_thread_exit_entries.push_back({condition, mutex, completed});
}

KYTY_SYSV_ABI void c_cnd_unregister_at_thread_exit(Kernel::PthreadMutex* mutex)
{
	if (mutex == nullptr)
	{
		return;
	}

	const auto first_removed =
	    std::remove_if(g_cnd_thread_exit_entries.begin(), g_cnd_thread_exit_entries.end(),
	                   [mutex](const auto& entry) { return entry.mutex == mutex; });
	g_cnd_thread_exit_entries.erase(first_removed, g_cnd_thread_exit_entries.end());
}

KYTY_SYSV_ABI void c_cnd_do_broadcast_at_thread_exit()
{
	for (auto& entry: g_cnd_thread_exit_entries)
	{
		if (entry.completed != nullptr)
		{
			c_thread_require(c_mtx_lock(entry.mutex), "thread-exit mutex lock");
			*entry.completed = 1;
			c_thread_require(c_cnd_broadcast(entry.condition), "thread-exit condition broadcast");
			c_thread_require(c_mtx_unlock(entry.mutex), "thread-exit mutex unlock");
		}
		else
		{
			c_thread_require(c_mtx_unlock(entry.mutex), "thread-exit transferred mutex unlock");
			c_thread_require(c_cnd_broadcast(entry.condition), "thread-exit condition broadcast");
		}
	}
	g_cnd_thread_exit_entries.clear();
}

static KYTY_SYSV_ABI int64_t c_xtime_get_ticks()
{
	Kernel::KernelTimespec now {};
	if (Kernel::KernelClockGettime(0, &now) != OK)
	{
		return 0;
	}

	// The C++ runtime converts these absolute ticks to nanoseconds by
	// multiplying by 1000 before splitting them into a timespec. Its tick
	// contract is therefore microseconds, not the 100-nanosecond host unit.
	constexpr int64_t ticks_per_second = 1000000;
	return now.tv_sec * ticks_per_second + now.tv_nsec / 1000;
}


static KYTY_SYSV_ABI void c_Xregex_error(int error_type)
{
	KYTY_LOG_WARN("std::regex_error warning: error_type=%d\n", error_type);
}

// The guest C++ runtime calls this after a synchronization primitive reports
// an error. Guest exception unwinding is not available, so preserve the error
// value in the fatal diagnostic instead of returning as though the throw ran.
static KYTY_SYSV_ABI void c_Throw_C_error(int error)
{
	EXIT("C++ runtime error throw requested: code=%d\n", error);
}

struct SceErrorExceptionLayout
{
	void**    vtable;
	uint32_t* shared_message;
};

static KYTY_SYSV_ABI const char* c_error_exception_what(const SceErrorExceptionLayout* self)
{
	EXIT_IF(self == nullptr || self->shared_message == nullptr);
	return reinterpret_cast<const char*>(self->shared_message) + sizeof(uint32_t);
}

// --- C++ locale / RTTI objects (guest Construct string path) -----------------
// Quiet boot AV: mov (%r12),%rdi with r12 = INVALID_MEMORY because weak Object
// Qoo175Ig+-k (_ZSt21_sceLibcClassicLocale) was never registered. The guest
// loads Locimp* from the locale, then looks up ctype<char> by id.
//
// NIDs from aerosoul94/dynlib (public Orbis NID table). Layout from the guest
// use_facet-like body at 0x900134a80 (facet_vec@+0x10, count@+0x18, id compare).

// SysV virtual stubs — slots match MSVC-style vptr offsets used by the title.
static KYTY_SYSV_ABI void* CxxVtableNoop(void* self)
{
	return self;
}

static KYTY_SYSV_ABI void* CxxVtableNull(void* /*self*/)
{
	return nullptr;
}

// ctype facet method at vtable+0x40 with esi=0x20; return non-zero in al.
static KYTY_SYSV_ABI int CxxCtypeFacetQuery(void* /*self*/, int /*mask*/)
{
	return 1;
}

static void* g_locimp_vtable[16] = {
    reinterpret_cast<void*>(&CxxVtableNoop), // +0x00
    reinterpret_cast<void*>(&CxxVtableNoop), // +0x08
    reinterpret_cast<void*>(&CxxVtableNoop), // +0x10  (called on Locimp entry)
    reinterpret_cast<void*>(&CxxVtableNull), // +0x18  (release; null skips delete)
    reinterpret_cast<void*>(&CxxVtableNoop), // +0x20
    reinterpret_cast<void*>(&CxxVtableNoop), // +0x28
    reinterpret_cast<void*>(&CxxVtableNoop), // +0x30
    reinterpret_cast<void*>(&CxxVtableNoop), // +0x38
    reinterpret_cast<void*>(&CxxVtableNoop), // +0x40
};

static void* g_ctype_vtable[16] = {
    reinterpret_cast<void*>(&CxxVtableNoop),      // +0x00
    reinterpret_cast<void*>(&CxxVtableNoop),      // +0x08
    reinterpret_cast<void*>(&CxxVtableNoop),      // +0x10
    reinterpret_cast<void*>(&CxxVtableNoop),      // +0x18
    reinterpret_cast<void*>(&CxxVtableNoop),      // +0x20
    reinterpret_cast<void*>(&CxxVtableNoop),      // +0x28
    reinterpret_cast<void*>(&CxxVtableNoop),      // +0x30
    reinterpret_cast<void*>(&CxxVtableNoop),      // +0x38
    reinterpret_cast<void*>(&CxxCtypeFacetQuery), // +0x40
};

static CxxCtypeFacetLayout g_ctype_facet {g_ctype_vtable, 0, g_c_locale_ctype.data() + 1};

// Facet vector: index 0 unused; ctype at kCxxCtypeCharId (1).
static void* g_classic_facets[kCxxLocimpFacetCount] = {nullptr, &g_ctype_facet};

static CxxLocimpLayout g_classic_locimp {
    g_locimp_vtable,      // vtable
    nullptr,              // reserved_08
    g_classic_facets,     // facet_vec
    kCxxLocimpFacetCount, // facet_count
    0,                    // reserved_20
    0,                    // flag_24
    {0, 0, 0},            // pad
    "C",                  // name
};

// _ZSt21_sceLibcClassicLocale — std::locale object (single Locimp*).
static CxxLocaleLayout g_sce_classic_locale {&g_classic_locimp};

// std::ctype<char>::id and locale::id::_Id_cnt (pre-assigned to match facets).
static std::uint64_t g_ctype_char_id = kCxxCtypeCharId;
// std::codecvt<char, char, mbstate_t>::id starts unassigned. libc assigns a
// locale-facet index lazily, so this must be distinct stable guest storage.
static std::uint64_t g_codecvt_char_id = 0;
static std::uint64_t g_collate_char_id = 5;
static std::uint64_t g_numpunct_char_id = 6;
static std::uint64_t g_num_get_char_id = 7;
static std::uint64_t g_time_get_char_id = 8;
static std::uint64_t g_time_put_char_id = 9;
static std::uint64_t g_codecvt_wchar_id = 10;
static std::uint64_t g_codecvt_char32_id = 11;
// Versioned facets allocate their locale indices lazily in guest code. Keep
// each id in distinct stable storage so registration and caching remain
// independent even when the stripped export does not expose the facet name.
static std::uint64_t g_lazy_locale_facet_id_0 = 0;
static std::uint64_t g_lazy_locale_facet_id_1 = 0;

struct alignas(8) CxxOstreamStorage
{
	std::uint64_t words[13] {};
};

static_assert(sizeof(CxxOstreamStorage) == 104);

static CxxOstreamStorage g_versioned_cerr;
static CxxOstreamStorage g_versioned_clog;
static CxxOstreamStorage g_versioned_cout;

struct alignas(8) CxxClassicOstreamStorage
{
	std::uint64_t words[12] {};
};

static_assert(sizeof(CxxClassicOstreamStorage) == 96);

static CxxClassicOstreamStorage g_classic_cerr;
static CxxClassicOstreamStorage g_classic_cout;

// libc math constant imported by C++ locale initialization.
static const double g_positive_infinity = INFINITY;
static std::uint64_t g_locale_id_2   = 2;
static std::uint64_t g_locale_id_3   = 3;
static std::uint64_t g_locale_id_4   = 4;
static std::uint64_t g_locale_id_5   = 5;
static std::uint64_t g_locale_id_6   = 6;
static std::uint64_t g_locale_id_7   = 7;
static std::int32_t  g_locale_id_cnt = 12;
static std::uint64_t g_dummy_obj_1   = 0;
static std::uint64_t g_dummy_obj_2   = 0;
static std::uint64_t g_dummy_obj_3   = 0;
static std::uint64_t g_dummy_obj_4   = 0;
static std::uint64_t g_dummy_obj_7   = 0;
static std::uint64_t g_dummy_obj_8   = 0;
static std::uint64_t g_dummy_obj_10  = 0;
static std::uint64_t g_dummy_obj_11  = 0;
static std::uint64_t g_dummy_obj_12  = 0;
static std::uint64_t g_dummy_obj_13  = 0;
static std::uint64_t g_dummy_obj_14  = 0;
static std::uint64_t g_dummy_obj_15  = 0;
static std::uint64_t g_dummy_obj_16  = 0;
static std::uint64_t g_dummy_obj_17  = 0;
static std::uint64_t g_dummy_obj_18  = 0;
static std::uint64_t g_dummy_obj_19  = 0;
static std::uint64_t g_dummy_obj_20  = 0;
static std::uint64_t g_dummy_obj_21  = 0;
static std::uint64_t g_dummy_obj_22  = 0;
static std::uint64_t g_dummy_obj_23  = 0;
static std::uint64_t g_dummy_obj_24  = 0;
static std::uint64_t g_dummy_obj_25  = 0;
// Additional facet ids imported as Objects by eboot (linker needs stable addresses).
static std::uint64_t g_ctype_wchar_id   = 2;
static std::uint64_t g_collate_wchar_id = 3;
static std::uint64_t g_num_put_char_id  = 4;

// Itanium type_info vtables: guest type_info objects relocate to these. Slots
// are no-ops so a stray virtual call does not hit INVALID_MEMORY.
static void* g_class_type_info_vtable[8]     = {reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop),
                                                reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop)};
static void* g_si_class_type_info_vtable[8]  = {reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop),
                                                reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop)};
static void* g_vmi_class_type_info_vtable[8] = {reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop),
                                                reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop)};
static void* g_pointer_type_info_vtable[8]   = {reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop),
                                                reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop)};
static void* g_pointer_to_member_type_info_vtable[8] = {reinterpret_cast<void*>(&CxxVtableNoop),
                                                        reinterpret_cast<void*>(&CxxVtableNoop),
                                                        reinterpret_cast<void*>(&CxxVtableNoop),
                                                        reinterpret_cast<void*>(&CxxVtableNoop)};
static void* g_function_type_info_vtable[8]           = {reinterpret_cast<void*>(&CxxVtableNoop),
                                                         reinterpret_cast<void*>(&CxxVtableNoop),
                                                         reinterpret_cast<void*>(&CxxVtableNoop),
                                                         reinterpret_cast<void*>(&CxxVtableNoop)};
static void* g_exception_vtable[8]           = {reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop),
                                                reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop)};

// Itanium __dynamic_cast (NID hMAe+TWS9mQ): rdi=src, rsi=static type_info,
// rdx=destination type_info, rcx=src2dst hint. Guest type_info objects relocate
// to the vtables above, so the guest RTTI is walked for the real result; the
// hint alone is wrong whenever the dynamic type differs from the expected one
// (a service returning either an alias string or the real object).
static KYTY_SYSV_ABI void* cxa_dynamic_cast(void* src, const void* src_type, const void* dst_type, int64_t src2dst)
{
	static const CxaTypeInfoVtables vtables {g_class_type_info_vtable, g_si_class_type_info_vtable, g_vmi_class_type_info_vtable};
	return CxaDynamicCastResolve(src, src_type, dst_type, src2dst, vtables);
}

// Exception / iostream RTTI Objects imported by a guest libc_v1 module.
// NIDs come from the import table; names come from public symbol catalogs.
// Vtable slots are no-ops; type_info uses Itanium __si layout (base null for now).
#define KYTY_CXX_NOOP_VTBL                                                                                                                 \
	{                                                                                                                                      \
		reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop), reinterpret_cast<void*>(&CxxVtableNoop),           \
		    reinterpret_cast<void*>(&CxxVtableNoop)                                                                                        \
	}
static void* g_domain_error_vtable[8]       = KYTY_CXX_NOOP_VTBL;
static void* g_logic_error_vtable[8]        = KYTY_CXX_NOOP_VTBL;
static void* g_out_of_range_vtable[8]       = KYTY_CXX_NOOP_VTBL;
static void* g_runtime_error_vtable[8]      = KYTY_CXX_NOOP_VTBL;
static void* g_invalid_argument_vtable[8]   = KYTY_CXX_NOOP_VTBL;
static void* g_length_error_vtable[8]       = KYTY_CXX_NOOP_VTBL;
static void* g_system_error_vtable[8]       = KYTY_CXX_NOOP_VTBL;
static void* g_future_error_vtable[8]       = KYTY_CXX_NOOP_VTBL;
static void* g_ios_base_vtable[8]           = KYTY_CXX_NOOP_VTBL;
static void* g_ios_failure_vtable[8]        = KYTY_CXX_NOOP_VTBL;
// std::codecvt<char, char, mbstate_t> virtual dispatch. Keep it distinct from
// other facets: it is ABI-compatible storage, but its behavior must not be
// conflated with ctype before a guest conversion call provides evidence.
static void* g_codecvt_char_vtable[8]       = KYTY_CXX_NOOP_VTBL;
#undef KYTY_CXX_NOOP_VTBL

static const char g_ti_name_exception[]         = "St9exception";
static const char g_ti_name_domain_error[]      = "St12domain_error";
static const char g_ti_name_out_of_range[]      = "St12out_of_range";
static const char g_ti_name_runtime_error[]     = "St13runtime_error";
static const char g_ti_name_invalid_argument[]  = "St16invalid_argument";
static const char g_ti_name_length_error[]      = "St12length_error";
static const char g_ti_name_range_error[]       = "St11range_error";
static const char g_ti_name_overflow_error[]    = "St14overflow_error";
static const char g_ti_name_underflow_error[]   = "St15underflow_error";
static const char g_ti_name_future_error[]      = "St12future_error";
static const char g_ti_name_bad_cast[]          = "St8bad_cast";
static const char g_ti_name_bad_alloc[]         = "St9bad_alloc";
static const char g_ti_name_bad_array_new_length[] = "St20bad_array_new_length";
static const char g_ti_name_ios_base[]          = "St8ios_base";
static const char g_ti_name_ios_failure[]       = "NSt8ios_base7failureE";
static const char g_ti_name_num_put_char[]      = "St7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE";

static CxxSiTypeInfoLayout g_typeinfo_exception {g_si_class_type_info_vtable, g_ti_name_exception, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_domain_error {g_si_class_type_info_vtable, g_ti_name_domain_error, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_out_of_range {g_si_class_type_info_vtable, g_ti_name_out_of_range, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_runtime_error {g_si_class_type_info_vtable, g_ti_name_runtime_error, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_invalid_argument {g_si_class_type_info_vtable, g_ti_name_invalid_argument, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_length_error {g_si_class_type_info_vtable, g_ti_name_length_error, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_range_error {g_si_class_type_info_vtable, g_ti_name_range_error, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_overflow_error {g_si_class_type_info_vtable, g_ti_name_overflow_error, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_underflow_error {g_si_class_type_info_vtable, g_ti_name_underflow_error, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_future_error {g_si_class_type_info_vtable, g_ti_name_future_error, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_bad_cast {g_si_class_type_info_vtable, g_ti_name_bad_cast,
                                                reinterpret_cast<const CxxTypeInfoLayout*>(&g_typeinfo_exception)};
static CxxSiTypeInfoLayout g_typeinfo_bad_alloc {g_si_class_type_info_vtable, g_ti_name_bad_alloc,
                                                 reinterpret_cast<const CxxTypeInfoLayout*>(&g_typeinfo_exception)};
static CxxSiTypeInfoLayout g_typeinfo_bad_array_new_length {
    g_si_class_type_info_vtable,
    g_ti_name_bad_array_new_length,
    reinterpret_cast<const CxxTypeInfoLayout*>(&g_typeinfo_bad_alloc),
};
static const char g_ti_name_bad_function_call[]  = "St18bad_function_call";
static CxxSiTypeInfoLayout g_typeinfo_bad_function_call {
    g_si_class_type_info_vtable,
    g_ti_name_bad_function_call,
    reinterpret_cast<const CxxTypeInfoLayout*>(&g_typeinfo_exception),
};
static CxxSiTypeInfoLayout g_typeinfo_ios_base {g_si_class_type_info_vtable, g_ti_name_ios_base, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_ios_failure {g_si_class_type_info_vtable, g_ti_name_ios_failure, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_num_put_char {g_si_class_type_info_vtable, g_ti_name_num_put_char, nullptr};
static const char g_ti_name_num_get_char[] = "St7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE";
static CxxSiTypeInfoLayout g_typeinfo_num_get_char {g_si_class_type_info_vtable, g_ti_name_num_get_char, nullptr};

struct CxxFacetBase;
static KYTY_SYSV_ABI void c_facet_dtor(CxxFacetBase* self);
static KYTY_SYSV_ABI void c_facet_deleting_dtor(CxxFacetBase* self);
static KYTY_SYSV_ABI void c_facet_incref(CxxFacetBase* self);
static KYTY_SYSV_ABI CxxFacetBase* c_facet_decref(CxxFacetBase* self);

// The fields used by num_put are part of the guest ios_base contract. Keep the
// complete prefix opaque: it belongs to the stream implementation and is only
// read by guest code, while these scalar formatting fields are consumed here.
struct alignas(8) CxxIosBaseLayout
{
	std::byte      reserved[0x18];
	std::uint32_t flags;
	std::int32_t  precision;
	std::int32_t  width;
};

static_assert(offsetof(CxxIosBaseLayout, flags) == 0x18);
static_assert(offsetof(CxxIosBaseLayout, precision) == 0x1c);
static_assert(offsetof(CxxIosBaseLayout, width) == 0x20);

constexpr std::uint32_t kCxxIosLeft       = 0x02;
constexpr std::uint32_t kCxxIosRight      = 0x04;
constexpr std::uint32_t kCxxIosInternal   = 0x08;
constexpr std::uint32_t kCxxIosAdjustMask = kCxxIosLeft | kCxxIosRight | kCxxIosInternal;
constexpr std::uint32_t kCxxIosDec        = 0x10;
constexpr std::uint32_t kCxxIosOct        = 0x20;
constexpr std::uint32_t kCxxIosHex        = 0x40;
constexpr std::uint32_t kCxxIosBaseMask   = kCxxIosDec | kCxxIosOct | kCxxIosHex;
constexpr std::uint32_t kCxxIosShowBase   = 0x80;
constexpr std::uint32_t kCxxIosShowPoint  = 0x100;
constexpr std::uint32_t kCxxIosUppercase  = 0x200;
constexpr std::uint32_t kCxxIosShowPos    = 0x400;
constexpr std::uint32_t kCxxIosScientific = 0x800;
constexpr std::uint32_t kCxxIosFixed      = 0x1000;
constexpr std::uint32_t kCxxIosFloatMask  = kCxxIosScientific | kCxxIosFixed;
constexpr std::uint32_t kCxxIosBoolAlpha  = 0x8000;

struct CxxIstreamIterator
{
	void*         streambuf;
	std::uint64_t failed;
};

static_assert(sizeof(CxxIstreamIterator) == 16);

using CxxIstreamRead = int(KYTY_SYSV_ABI*)(void*);

static int CxxIstreamPeek(const CxxIstreamIterator& iterator)
{
	if (iterator.failed != 0 || iterator.streambuf == nullptr)
	{
		return -1;
	}
	auto*** object = reinterpret_cast<void***>(iterator.streambuf);
	return (*object != nullptr && (*object)[7] != nullptr)
	           ? reinterpret_cast<CxxIstreamRead>((*object)[7])(iterator.streambuf)
	           : -1;
}

static int CxxIstreamAdvance(CxxIstreamIterator* iterator)
{
	if (iterator == nullptr || iterator->failed != 0 || iterator->streambuf == nullptr)
	{
		return -1;
	}
	auto*** object = reinterpret_cast<void***>(iterator->streambuf);
	if (*object == nullptr || (*object)[8] == nullptr)
	{
		iterator->failed = 1;
		return -1;
	}
	return reinterpret_cast<CxxIstreamRead>((*object)[8])(iterator->streambuf);
}

constexpr std::uint32_t kCxxIosEofBit  = 0x1;
constexpr std::uint32_t kCxxIosFailBit = 0x2;
constexpr size_t        kCxxNumGetMax  = 128;

// Stage 2 of num_get in the "C" locale: the characters of one number, read
// while they can continue it. *base is the stream's basefield (0 when none is
// set, which detects a 0x or 0 prefix like strtol) and becomes the base found.
static size_t CxxIstreamCollectInteger(CxxIstreamIterator* iterator, int* base, char* buffer, std::uint32_t* state)
{
	size_t length  = 0;
	int    current = CxxIstreamPeek(*iterator);
	const auto take = [&]()
	{
		buffer[length++] = static_cast<char>(current);
		(void)CxxIstreamAdvance(iterator);
		current = CxxIstreamPeek(*iterator);
	};
	if (current == '+' || current == '-')
	{
		take();
	}
	if ((*base == 0 || *base == 16) && current == '0')
	{
		take();
		if (current == 'x' || current == 'X')
		{
			take();
			*base = 16;
		} else if (*base == 0)
		{
			*base = 8;
		}
	}
	if (*base == 0)
	{
		*base = 10;
	}
	for (; current >= 0 && length + 1 < kCxxNumGetMax; take())
	{
		const int digit = std::isdigit(current) != 0 ? current - '0'
		                  : std::isxdigit(current) != 0 ? std::tolower(current) - 'a' + 10
		                                                : 64;
		if (digit >= *base)
		{
			break;
		}
	}
	if (current < 0)
	{
		*state |= kCxxIosEofBit;
	}
	buffer[length] = '\0';
	return length;
}

static size_t CxxIstreamCollectFloat(CxxIstreamIterator* iterator, char* buffer, std::uint32_t* state)
{
	size_t length  = 0;
	int    current = CxxIstreamPeek(*iterator);
	const auto take = [&]()
	{
		buffer[length++] = static_cast<char>(current);
		(void)CxxIstreamAdvance(iterator);
		current = CxxIstreamPeek(*iterator);
	};
	const auto digits = [&]()
	{
		while (current >= '0' && current <= '9' && length + 1 < kCxxNumGetMax)
		{
			take();
		}
	};
	if (current == '+' || current == '-')
	{
		take();
	}
	digits();
	if (current == '.' && length + 1 < kCxxNumGetMax)
	{
		take();
		digits();
	}
	if ((current == 'e' || current == 'E') && length + 2 < kCxxNumGetMax)
	{
		take();
		if (current == '+' || current == '-')
		{
			take();
		}
		digits();
	}
	if (current < 0)
	{
		*state |= kCxxIosEofBit;
	}
	buffer[length] = '\0';
	return length;
}

static int CxxNumGetBase(const CxxIosBaseLayout* ios_base)
{
	switch (ios_base->flags & kCxxIosBaseMask)
	{
		case kCxxIosDec: return 10;
		case kCxxIosOct: return 8;
		case kCxxIosHex: return 16;
		default: return 0;
	}
}

// Integers convert through strtoll/strtoull as the standard specifies; a value
// outside the target type stores its nearest limit and sets failbit.
template <typename Int>
static CxxIstreamIterator CxxNumGetInteger(CxxIstreamIterator iterator, const CxxIosBaseLayout* ios_base, std::uint32_t* state,
	                                       Int* value)
{
	if (ios_base == nullptr || state == nullptr || value == nullptr)
	{
		if (state != nullptr) { *state |= kCxxIosFailBit; }
		return iterator;
	}
	char       buffer[kCxxNumGetMax];
	int        base   = CxxNumGetBase(ios_base);
	const auto length = CxxIstreamCollectInteger(&iterator, &base, buffer, state);
	char*      end    = nullptr;
	errno             = 0;
	if constexpr (std::is_signed_v<Int>)
	{
		const long long parsed = std::strtoll(buffer, &end, base);
		const bool range = errno != ERANGE && parsed >= std::numeric_limits<Int>::min() && parsed <= std::numeric_limits<Int>::max();
		*value           = range ? static_cast<Int>(parsed) : (parsed < 0 ? std::numeric_limits<Int>::min() : std::numeric_limits<Int>::max());
		if (length == 0 || end != buffer + length || !range) { *state |= kCxxIosFailBit; }
	} else
	{
		const unsigned long long parsed = std::strtoull(buffer, &end, base);
		const bool range = errno != ERANGE && parsed <= std::numeric_limits<Int>::max();
		*value           = range ? static_cast<Int>(parsed) : std::numeric_limits<Int>::max();
		if (length == 0 || end != buffer + length || !range) { *state |= kCxxIosFailBit; }
	}
	if (length == 0)
	{
		*value = 0;
	}
	return iterator;
}

template <typename Float>
static CxxIstreamIterator CxxNumGetFloat(CxxIstreamIterator iterator, std::uint32_t* state, Float* value)
{
	if (state == nullptr || value == nullptr)
	{
		if (state != nullptr) { *state |= kCxxIosFailBit; }
		return iterator;
	}
	char       buffer[kCxxNumGetMax];
	const auto length = CxxIstreamCollectFloat(&iterator, buffer, state);
	char*      end    = nullptr;
	errno             = 0;
	Float      parsed = 0;
	if constexpr (std::is_same_v<Float, float>)
	{
		parsed = std::strtof(buffer, &end);
	} else if constexpr (std::is_same_v<Float, double>)
	{
		parsed = std::strtod(buffer, &end);
	} else
	{
		parsed = std::strtold(buffer, &end);
	}
	*value = length == 0 ? Float {} : parsed;
	if (length == 0 || end != buffer + length || errno == ERANGE)
	{
		*state |= kCxxIosFailBit;
	}
	return iterator;
}

#define KYTY_CXX_NUM_GET(name, type, body)                                                                                                 \
	static KYTY_SYSV_ABI CxxIstreamIterator name(const CxxFacetBase* /*self*/, CxxIstreamIterator iterator,                              \
	                                             CxxIstreamIterator /*last*/, CxxIosBaseLayout* ios_base, std::uint32_t* state,          \
	                                             type* value)                                                                             \
	{                                                                                                                                      \
		return body;                                                                                                                   \
	}

KYTY_CXX_NUM_GET(c_num_get_do_get_ushort, std::uint16_t, CxxNumGetInteger(iterator, ios_base, state, value))
KYTY_CXX_NUM_GET(c_num_get_do_get_uint, std::uint32_t, CxxNumGetInteger(iterator, ios_base, state, value))
KYTY_CXX_NUM_GET(c_num_get_do_get_long, std::int64_t, CxxNumGetInteger(iterator, ios_base, state, value))
KYTY_CXX_NUM_GET(c_num_get_do_get_ulong, std::uint64_t, CxxNumGetInteger(iterator, ios_base, state, value))
KYTY_CXX_NUM_GET(c_num_get_do_get_float, float, CxxNumGetFloat(iterator, state, value))
KYTY_CXX_NUM_GET(c_num_get_do_get_double, double, CxxNumGetFloat(iterator, state, value))
KYTY_CXX_NUM_GET(c_num_get_do_get_long_double, long double, CxxNumGetFloat(iterator, state, value))
#undef KYTY_CXX_NUM_GET

// Without boolalpha a bool reads as a long that must be 0 or 1.
static KYTY_SYSV_ABI CxxIstreamIterator c_num_get_do_get_bool(const CxxFacetBase* /*self*/, CxxIstreamIterator iterator,
	                                                          CxxIstreamIterator /*last*/, CxxIosBaseLayout* ios_base,
	                                                          std::uint32_t* state, bool* value)
{
	if (ios_base == nullptr || state == nullptr || value == nullptr)
	{
		if (state != nullptr) { *state |= kCxxIosFailBit; }
		return iterator;
	}
	if ((ios_base->flags & kCxxIosBoolAlpha) == 0)
	{
		std::int64_t number = 0;
		iterator            = CxxNumGetInteger(iterator, ios_base, state, &number);
		if ((*state & kCxxIosFailBit) == 0 && number != 0 && number != 1)
		{
			*state |= kCxxIosFailBit;
		}
		*value = number != 0;
		return iterator;
	}
	// "true" or "false" in the "C" locale: read while the input still matches one of them.
	char   word[6] = {};
	size_t length  = 0;
	for (int current = CxxIstreamPeek(iterator); current >= 0 && length < 5; current = CxxIstreamPeek(iterator))
	{
		word[length] = static_cast<char>(current);
		if (std::strncmp(word, "true", length + 1) != 0 && std::strncmp(word, "false", length + 1) != 0)
		{
			break;
		}
		length++;
		(void)CxxIstreamAdvance(&iterator);
		if (std::strcmp(word, "true") == 0 || std::strcmp(word, "false") == 0)
		{
			break;
		}
	}
	word[length] = '\0';
	*value       = std::strcmp(word, "true") == 0;
	if (!*value && std::strcmp(word, "false") != 0)
	{
		*state |= kCxxIosFailBit;
	}
	if (CxxIstreamPeek(iterator) < 0)
	{
		*state |= kCxxIosEofBit;
	}
	return iterator;
}

// A pointer reads as the hexadecimal integer num_put writes for it.
static KYTY_SYSV_ABI CxxIstreamIterator c_num_get_do_get_pointer(const CxxFacetBase* /*self*/, CxxIstreamIterator iterator,
	                                                             CxxIstreamIterator /*last*/, CxxIosBaseLayout* ios_base,
	                                                             std::uint32_t* state, void** value)
{
	if (ios_base == nullptr || state == nullptr || value == nullptr)
	{
		if (state != nullptr) { *state |= kCxxIosFailBit; }
		return iterator;
	}
	CxxIosBaseLayout hex_base = *ios_base;
	hex_base.flags            = (hex_base.flags & ~kCxxIosBaseMask) | kCxxIosHex;
	std::uint64_t address     = 0;
	iterator                  = CxxNumGetInteger(iterator, &hex_base, state, &address);
	*value                    = reinterpret_cast<void*>(address);
	return iterator;
}

// Itanium vtable object: offset-to-top, RTTI, two destructors, facet lifetime,
// then the narrow-character extraction overloads in the guest library's order:
// bool, unsigned short, unsigned int, long, unsigned long, long long,
// unsigned long long, float, double, long double, void*.
static void* g_num_get_char_vtable[] = {
    nullptr,
    &g_typeinfo_num_get_char,
    reinterpret_cast<void*>(&c_facet_dtor),
    reinterpret_cast<void*>(&c_facet_deleting_dtor),
    reinterpret_cast<void*>(&c_facet_incref),
    reinterpret_cast<void*>(&c_facet_decref),
    reinterpret_cast<void*>(&c_num_get_do_get_bool),
    reinterpret_cast<void*>(&c_num_get_do_get_ushort),
    reinterpret_cast<void*>(&c_num_get_do_get_uint),
    reinterpret_cast<void*>(&c_num_get_do_get_long),
    reinterpret_cast<void*>(&c_num_get_do_get_ulong),
    reinterpret_cast<void*>(&c_num_get_do_get_long),
    reinterpret_cast<void*>(&c_num_get_do_get_ulong),
    reinterpret_cast<void*>(&c_num_get_do_get_float),
    reinterpret_cast<void*>(&c_num_get_do_get_double),
    reinterpret_cast<void*>(&c_num_get_do_get_long_double),
    reinterpret_cast<void*>(&c_num_get_do_get_pointer),
};

static_assert(std::size(g_num_get_char_vtable) == 17);

struct alignas(8) CxxFacetBase
{
	void**        vtable;
	std::uint32_t references;
	std::uint32_t reserved;
};

struct CxxOstreamIterator
{
	std::uint64_t failed;
	void*         streambuf;
};

static_assert(sizeof(CxxOstreamIterator) == 16);

constexpr std::int32_t  kCxxNumPutMaxWidth = 1 << 20;
constexpr std::int32_t  kCxxNumPutMaxPrecision = 512;

// std::setw(int) returns the ABI's two-register smanip {apply, arg} (rax:rdx).
// Observed guest use after PLT resolution:
//   call setw(N) → mov %edx,%esi → call *%rax with rdi already adjusted to the
//   ios_base subobject. Returning only N in eax made call *%rax jump to a
//   near-null address (RIP=N) and FatalFault with rc=139.
struct CxxIosSmanipInt
{
	void(KYTY_SYSV_ABI *apply)(CxxIosBaseLayout* ios, int arg);
	int arg;
};

static_assert(sizeof(CxxIosSmanipInt) == 16);
static_assert(offsetof(CxxIosSmanipInt, apply) == 0);
static_assert(offsetof(CxxIosSmanipInt, arg) == 8);

static KYTY_SYSV_ABI void c_setw_apply(CxxIosBaseLayout* ios, int width)
{
	if (ios == nullptr)
	{
		return;
	}
	ios->width = width;
}

static KYTY_SYSV_ABI CxxIosSmanipInt c_setw(int width)
{
	return CxxIosSmanipInt {&c_setw_apply, width};
}

static KYTY_SYSV_ABI void c_setprecision_apply(CxxIosBaseLayout* ios, int precision)
{
	if (ios == nullptr)
	{
		return;
	}
	ios->precision = precision;
}

static KYTY_SYSV_ABI CxxIosSmanipInt c_setprecision(int precision)
{
	return CxxIosSmanipInt {&c_setprecision_apply, precision};
}

static KYTY_SYSV_ABI void c_facet_dtor(CxxFacetBase* /*self*/) {}

static KYTY_SYSV_ABI void c_facet_deleting_dtor(CxxFacetBase* self)
{
	cxx_delete(self);
}

static KYTY_SYSV_ABI void c_facet_incref(CxxFacetBase* self)
{
	EXIT_IF(self == nullptr);
	__atomic_add_fetch(&self->references, 1u, __ATOMIC_RELAXED);
}

static KYTY_SYSV_ABI CxxFacetBase* c_facet_decref(CxxFacetBase* self)
{
	EXIT_IF(self == nullptr);
	const std::uint32_t previous = __atomic_fetch_sub(&self->references, 1u, __ATOMIC_ACQ_REL);
	EXIT_IF(previous == 0);
	return previous == 1 ? self : nullptr;
}

static CxxOstreamIterator CxxOstreamWrite(CxxOstreamIterator iterator, const char* data, size_t size)
{
	if (iterator.failed != 0 || iterator.streambuf == nullptr || data == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}

	auto** streambuf_vtable = *static_cast<void***>(iterator.streambuf);
	EXIT_IF(streambuf_vtable == nullptr || streambuf_vtable[4] == nullptr);
	const std::uint64_t overflow = reinterpret_cast<std::uint64_t>(streambuf_vtable[4]);
	for (size_t index = 0; index < size; ++index)
	{
		const std::uint64_t result =
		    Loader::GuestCall::Invoke(overflow, reinterpret_cast<std::uint64_t>(iterator.streambuf),
		                              static_cast<unsigned char>(data[index]), 0);
		if (static_cast<std::int32_t>(result) == -1)
		{
			iterator.failed = 1;
			break;
		}
	}
	return iterator;
}

static CxxOstreamIterator CxxOstreamWriteRepeated(CxxOstreamIterator iterator, char character, size_t count)
{
	char padding[64];
	::memset(padding, character, sizeof(padding));
	while (count != 0 && iterator.failed == 0)
	{
		const size_t chunk = std::min(count, sizeof(padding));
		iterator          = CxxOstreamWrite(iterator, padding, chunk);
		count -= chunk;
	}
	return iterator;
}

static CxxOstreamIterator CxxOstreamWriteFormatted(CxxOstreamIterator iterator, CxxIosBaseLayout* ios_base, char fill,
                                                    const char* output, size_t output_size, size_t prefix_size)
{
	if (ios_base == nullptr || output == nullptr || prefix_size > output_size)
	{
		iterator.failed = 1;
		return iterator;
	}

	const std::int32_t configured_width = ios_base->width;
	ios_base->width                     = 0;
	if (configured_width > kCxxNumPutMaxWidth)
	{
		iterator.failed = 1;
		return iterator;
	}

	const size_t padding = configured_width > 0 && static_cast<size_t>(configured_width) > output_size
	                           ? static_cast<size_t>(configured_width) - output_size
	                           : 0;
	const std::uint32_t adjustment = ios_base->flags & kCxxIosAdjustMask;
	if (adjustment != kCxxIosLeft && adjustment != kCxxIosInternal)
	{
		iterator = CxxOstreamWriteRepeated(iterator, fill, padding);
	}

	if (prefix_size != 0)
	{
		iterator = CxxOstreamWrite(iterator, output, prefix_size);
	}
	if (adjustment == kCxxIosInternal)
	{
		iterator = CxxOstreamWriteRepeated(iterator, fill, padding);
	}
	iterator = CxxOstreamWrite(iterator, output + prefix_size, output_size - prefix_size);
	if (adjustment == kCxxIosLeft)
	{
		iterator = CxxOstreamWriteRepeated(iterator, fill, padding);
	}
	return iterator;
}

static int CxxNumPutBase(std::uint32_t flags)
{
	switch (flags & kCxxIosBaseMask)
	{
		case kCxxIosOct: return 8;
		case kCxxIosHex: return 16;
		default: return 10;
	}
}

static CxxOstreamIterator CxxNumPutUnsigned(CxxOstreamIterator iterator, CxxIosBaseLayout* ios_base, char fill,
	                                         std::uint64_t value, bool force_hex_prefix)
{
	if (ios_base == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}

	const std::uint32_t flags = ios_base->flags;
	const int           base  = force_hex_prefix ? 16 : CxxNumPutBase(flags);
	char                digits[65];
	const auto [end, error] = std::to_chars(std::begin(digits), std::end(digits), value, base);
	if (error != std::errc {})
	{
		iterator.failed = 1;
		return iterator;
	}

	char   output[68];
	size_t prefix_size = 0;
	if ((force_hex_prefix || ((flags & kCxxIosShowBase) != 0 && value != 0)) && base == 16)
	{
		output[prefix_size++] = '0';
		output[prefix_size++] = (flags & kCxxIosUppercase) != 0 ? 'X' : 'x';
	} else if ((flags & kCxxIosShowBase) != 0 && value != 0 && base == 8)
	{
		output[prefix_size++] = '0';
	}

	const size_t digit_count = static_cast<size_t>(end - std::begin(digits));
	::memcpy(output + prefix_size, digits, digit_count);
	if ((flags & kCxxIosUppercase) != 0)
	{
		for (size_t index = prefix_size; index < prefix_size + digit_count; ++index)
		{
			if (output[index] >= 'a' && output[index] <= 'f')
			{
				output[index] = static_cast<char>(output[index] - ('a' - 'A'));
			}
		}
	}
	return CxxOstreamWriteFormatted(iterator, ios_base, fill, output, prefix_size + digit_count, prefix_size);
}

static CxxOstreamIterator CxxNumPutSigned(CxxOstreamIterator iterator, CxxIosBaseLayout* ios_base, char fill,
	                                       std::int64_t value)
{
	if (ios_base == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}
	if (CxxNumPutBase(ios_base->flags) != 10)
	{
		return CxxNumPutUnsigned(iterator, ios_base, fill, static_cast<std::uint64_t>(value), false);
	}

	char output[32];
	auto [end, error] = std::to_chars(std::begin(output), std::end(output), value);
	if (error != std::errc {})
	{
		iterator.failed = 1;
		return iterator;
	}

	size_t output_size = static_cast<size_t>(end - std::begin(output));
	size_t prefix_size = output_size != 0 && output[0] == '-' ? 1 : 0;
	if (prefix_size == 0 && (ios_base->flags & kCxxIosShowPos) != 0)
	{
		::memmove(output + 1, output, output_size);
		output[0] = '+';
		++output_size;
		prefix_size = 1;
	}
	return CxxOstreamWriteFormatted(iterator, ios_base, fill, output, output_size, prefix_size);
}

template <typename Float>
static CxxOstreamIterator CxxNumPutFloat(CxxOstreamIterator iterator, CxxIosBaseLayout* ios_base, char fill, Float value)
{
	static_assert(std::is_same_v<Float, double> || std::is_same_v<Float, long double>);
	if (ios_base == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}

	const std::uint32_t flags = ios_base->flags;
	const std::int32_t precision = ios_base->precision < 0 ? 6 : ios_base->precision;
	if (precision > kCxxNumPutMaxPrecision)
	{
		ios_base->width = 0;
		iterator.failed  = 1;
		return iterator;
	}

	char* format = nullptr;
	char  format_buffer[8];
	format = format_buffer;
	*format++ = '%';
	if ((flags & kCxxIosShowPos) != 0)
	{
		*format++ = '+';
	}
	if ((flags & kCxxIosShowPoint) != 0)
	{
		*format++ = '#';
	}
	// hexfloat prints every significant digit: the guest formatter passes no precision.
	const bool hexfloat = (flags & kCxxIosFloatMask) == kCxxIosFloatMask;
	if (!hexfloat)
	{
		*format++ = '.';
		*format++ = '*';
	}
	if constexpr (std::is_same_v<Float, long double>)
	{
		*format++ = 'L';
	}

	switch (flags & kCxxIosFloatMask)
	{
		case kCxxIosFixed: *format++ = 'f'; break;
		case kCxxIosScientific: *format++ = (flags & kCxxIosUppercase) != 0 ? 'E' : 'e'; break;
		case kCxxIosFloatMask: *format++ = (flags & kCxxIosUppercase) != 0 ? 'A' : 'a'; break;
		default: *format++ = (flags & kCxxIosUppercase) != 0 ? 'G' : 'g'; break;
	}
	*format = '\0';

	char output[1024];
	const int output_size = hexfloat ? ::snprintf(output, sizeof(output), format_buffer, value)
	                                 : ::snprintf(output, sizeof(output), format_buffer, precision, value);
	if (output_size < 0 || static_cast<size_t>(output_size) >= sizeof(output))
	{
		ios_base->width = 0;
		iterator.failed  = 1;
		return iterator;
	}

	size_t prefix_size = output_size != 0 && (output[0] == '-' || output[0] == '+') ? 1 : 0;
	return CxxOstreamWriteFormatted(iterator, ios_base, fill, output, static_cast<size_t>(output_size), prefix_size);
}

static KYTY_SYSV_ABI CxxOstreamIterator c_num_put_do_put_bool(const CxxFacetBase* self, CxxOstreamIterator iterator,
	                                                            CxxIosBaseLayout* ios_base, char fill, bool value)
{
	if (self == nullptr || ios_base == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}
	if ((ios_base->flags & kCxxIosBoolAlpha) == 0)
	{
		const char numeric = value ? '1' : '0';
		return CxxOstreamWriteFormatted(iterator, ios_base, fill, &numeric, 1, 0);
	}
	const char* text = value ? "true" : "false";
	return CxxOstreamWriteFormatted(iterator, ios_base, fill, text, ::strlen(text), 0);
}

static KYTY_SYSV_ABI CxxOstreamIterator c_num_put_do_put_long(const CxxFacetBase* self, CxxOstreamIterator iterator,
	                                                            CxxIosBaseLayout* ios_base, char fill, std::int64_t value)
{
	if (self == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}
	return CxxNumPutSigned(iterator, ios_base, fill, value);
}

static KYTY_SYSV_ABI CxxOstreamIterator c_num_put_do_put_ulong(const CxxFacetBase* self, CxxOstreamIterator iterator,
	                                                             CxxIosBaseLayout* ios_base, char fill, std::uint64_t value)
{
	if (self == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}
	return CxxNumPutUnsigned(iterator, ios_base, fill, value, false);
}

static KYTY_SYSV_ABI CxxOstreamIterator c_num_put_do_put_double(const CxxFacetBase* self, CxxOstreamIterator iterator,
	                                                              CxxIosBaseLayout* ios_base, char fill, double value)
{
	if (self == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}
	return CxxNumPutFloat(iterator, ios_base, fill, value);
}

static KYTY_SYSV_ABI CxxOstreamIterator c_num_put_do_put_long_double(const CxxFacetBase* self, CxxOstreamIterator iterator,
	                                                                   CxxIosBaseLayout* ios_base, char fill, long double value)
{
	if (self == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}
	return CxxNumPutFloat(iterator, ios_base, fill, value);
}

static KYTY_SYSV_ABI CxxOstreamIterator c_num_put_do_put_pointer(const CxxFacetBase* self, CxxOstreamIterator iterator,
	                                                               CxxIosBaseLayout* ios_base, char fill, const void* value)
{
	if (self == nullptr)
	{
		iterator.failed = 1;
		return iterator;
	}
	return CxxNumPutUnsigned(iterator, ios_base, fill, reinterpret_cast<std::uintptr_t>(value), true);
}

static KYTY_SYSV_ABI CxxOstreamIterator c_num_put_do_put_long_long(const CxxFacetBase* self, CxxOstreamIterator iterator,
	                                                                 CxxIosBaseLayout* ios_base, char fill, std::int64_t value)
{
	return c_num_put_do_put_long(self, iterator, ios_base, fill, value);
}

static KYTY_SYSV_ABI CxxOstreamIterator c_num_put_do_put_ulong_long(const CxxFacetBase* self, CxxOstreamIterator iterator,
	                                                                  CxxIosBaseLayout* ios_base, char fill, std::uint64_t value)
{
	return c_num_put_do_put_ulong(self, iterator, ios_base, fill, value);
}

// Itanium vtable object: offset-to-top, RTTI, two destructors, facet lifetime,
// then the insertion overloads in the guest library's order: bool, long,
// unsigned long, long long, unsigned long long, double, long double, void*.
// Every title's bundled libc agrees; a double inserted through another order
// printed an integer register as a pointer.
static void* g_num_put_char_vtable[] = {
    nullptr,
    &g_typeinfo_num_put_char,
    reinterpret_cast<void*>(&c_facet_dtor),
    reinterpret_cast<void*>(&c_facet_deleting_dtor),
    reinterpret_cast<void*>(&c_facet_incref),
    reinterpret_cast<void*>(&c_facet_decref),
    reinterpret_cast<void*>(&c_num_put_do_put_bool),
    reinterpret_cast<void*>(&c_num_put_do_put_long),
    reinterpret_cast<void*>(&c_num_put_do_put_ulong),
    reinterpret_cast<void*>(&c_num_put_do_put_long_long),
    reinterpret_cast<void*>(&c_num_put_do_put_ulong_long),
    reinterpret_cast<void*>(&c_num_put_do_put_double),
    reinterpret_cast<void*>(&c_num_put_do_put_long_double),
    reinterpret_cast<void*>(&c_num_put_do_put_pointer),
};

static_assert(std::size(g_num_put_char_vtable) == 14);

static KYTY_SYSV_ABI CxxOstreamIterator c_time_put_do_put(const CxxFacetBase* /*self*/, CxxOstreamIterator iterator,
                                                          void* /*ios_base*/, char /*fill*/, const GuestTm* guest_time,
                                                          char format, char modifier)
{
	if (iterator.failed != 0 || iterator.streambuf == nullptr || guest_time == nullptr || format == '\0')
	{
		iterator.failed = 1;
		return iterator;
	}

	char format_string[4] = {'%', format, '\0', '\0'};
	if (modifier == 'E' || modifier == 'O')
	{
		format_string[1] = modifier;
		format_string[2] = format;
	}

	const std::tm host_time = GuestToHostTm(*guest_time);
	char          output[256] {};
	const size_t  output_size = std::strftime(output, sizeof(output), format_string, &host_time);
	if (output_size == 0)
	{
		iterator.failed = 1;
		return iterator;
	}

	return CxxOstreamWrite(iterator, output, output_size);
}

static KYTY_SYSV_ABI CxxOstreamIterator c_time_put_put(const CxxFacetBase* self, CxxOstreamIterator iterator, void* ios_base,
                                                       char fill, const GuestTm* guest_time, const char* first, const char* last)
{
	if (self == nullptr || first == nullptr || last == nullptr || first > last)
	{
		iterator.failed = 1;
		return iterator;
	}

	while (first != last && iterator.failed == 0)
	{
		if (*first != '%')
		{
			iterator = CxxOstreamWrite(iterator, first, 1);
			++first;
			continue;
		}

		++first;
		if (first == last)
		{
			const char percent = '%';
			return CxxOstreamWrite(iterator, &percent, 1);
		}

		char modifier = '\0';
		if (*first == 'E' || *first == 'O')
		{
			modifier = *first++;
			if (first == last)
			{
				const char incomplete[] = {'%', modifier};
				return CxxOstreamWrite(iterator, incomplete, sizeof(incomplete));
			}
		}

		iterator = c_time_put_do_put(self, iterator, ios_base, fill, guest_time, *first, modifier);
		++first;
	}
	return iterator;
}

static const char g_ti_name_time_put_char[] = "St8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE";
static CxxSiTypeInfoLayout g_typeinfo_time_put_char {g_si_class_type_info_vtable, g_ti_name_time_put_char, nullptr};

// Itanium vtable object: offset-to-top, type_info, destructors, facet ownership,
// and the narrow-character formatting virtual.
static void* g_time_put_char_vtable[] = {
    nullptr,
    &g_typeinfo_time_put_char,
    reinterpret_cast<void*>(&c_facet_dtor),
    reinterpret_cast<void*>(&c_facet_deleting_dtor),
    reinterpret_cast<void*>(&c_facet_incref),
    reinterpret_cast<void*>(&c_facet_decref),
    reinterpret_cast<void*>(&c_time_put_do_put),
};

struct alignas(8) CxxThreadPad
{
	void**                  vtable;
	Kernel::PthreadCond  condition;
	Kernel::PthreadMutex mutex;
	bool                    launched;
	std::uint8_t            padding[7] {};
};

static_assert(sizeof(CxxThreadPad) == 32);
static_assert(offsetof(CxxThreadPad, condition) == 8);
static_assert(offsetof(CxxThreadPad, mutex) == 16);
static_assert(offsetof(CxxThreadPad, launched) == 24);

static const char       g_ti_name_thread_pad[] = "St4_Pad";
static CxxTypeInfoLayout g_typeinfo_thread_pad {g_class_type_info_vtable, g_ti_name_thread_pad};

static KYTY_SYSV_ABI void c_thread_pad_pure_virtual(CxxThreadPad* /*self*/)
{
	EXIT("std::_Pad virtual entry invoked before derived construction completed\n");
}

static void* g_thread_pad_vtable[] = {
    nullptr,
    &g_typeinfo_thread_pad,
    reinterpret_cast<void*>(&c_thread_pad_pure_virtual),
};

static void c_thread_pad_construct(CxxThreadPad* self, const char* name)
{
	EXIT_IF(self == nullptr);

	self->vtable   = g_thread_pad_vtable + 2;
	self->condition = nullptr;
	self->mutex     = nullptr;
	self->launched  = false;

	const int condition_result = c_cnd_init_with_name(&self->condition, name);
	c_thread_require(condition_result, "std::_Pad condition initialization");

	const int mutex_result = c_mtx_init_with_name(&self->mutex, 1, name);
	if (mutex_result != static_cast<int>(CThreadResult::Success))
	{
		c_cnd_destroy(&self->condition);
		c_thread_require(mutex_result, "std::_Pad mutex initialization");
	}

	c_thread_require(c_mtx_lock(&self->mutex), "std::_Pad initial mutex lock");
}

static KYTY_SYSV_ABI void c_thread_pad_ctor(CxxThreadPad* self)
{
	c_thread_pad_construct(self, "Thr");
}

static KYTY_SYSV_ABI void c_thread_pad_named_ctor(CxxThreadPad* self, const char* name)
{
	c_thread_pad_construct(self, name);
}

static KYTY_SYSV_ABI void c_thread_pad_dtor(CxxThreadPad* self)
{
	EXIT_IF(self == nullptr);

	self->vtable = g_thread_pad_vtable + 2;
	c_thread_require(c_mtx_unlock(&self->mutex), "std::_Pad destructor mutex unlock");
	c_mtx_destroy(&self->mutex);
	c_cnd_destroy(&self->condition);
}

static KYTY_SYSV_ABI void c_thread_pad_release(CxxThreadPad* self)
{
	EXIT_IF(self == nullptr);

	c_thread_require(c_mtx_lock(&self->mutex), "std::_Pad release mutex lock");
	self->launched = true;
	c_thread_require(c_cnd_signal(&self->condition), "std::_Pad release condition signal");
	c_thread_require(c_mtx_unlock(&self->mutex), "std::_Pad release mutex unlock");
}

static KYTY_SYSV_ABI void* c_thread_pad_call_func(void* argument)
{
	auto* self = static_cast<CxxThreadPad*>(argument);
	EXIT_IF(self == nullptr || self->vtable == nullptr || self->vtable[0] == nullptr);

	Loader::GuestCall::Invoke(reinterpret_cast<std::uint64_t>(self->vtable[0]), reinterpret_cast<std::uint64_t>(self), 0, 0);
	c_cnd_do_broadcast_at_thread_exit();
	return nullptr;
}

static void c_thread_pad_launch(CxxThreadPad* self, Kernel::Pthread* thread, const Kernel::PthreadAttr* attr,
                                const char* name)
{
	EXIT_IF(self == nullptr || thread == nullptr);

	const int result = Kernel::PthreadCreate(thread, attr, c_thread_pad_call_func, self, name != nullptr ? name : "");
	if (result != OK)
	{
		EXIT("std::_Pad thread creation failed with kernel result 0x%x\n", static_cast<unsigned int>(result));
	}

	while (!self->launched)
	{
		c_thread_require(c_cnd_wait(&self->condition, &self->mutex), "std::_Pad launch condition wait");
	}
}

static KYTY_SYSV_ABI void c_thread_pad_launch(CxxThreadPad* self, Kernel::Pthread* thread)
{
	c_thread_pad_launch(self, thread, nullptr, "");
}

static KYTY_SYSV_ABI void c_thread_pad_named_launch(CxxThreadPad* self, const char* name, Kernel::Pthread* thread)
{
	c_thread_pad_launch(self, thread, nullptr, name);
}

static KYTY_SYSV_ABI void c_thread_pad_attr_launch(CxxThreadPad* self, const Kernel::PthreadAttr* attr,
                                                   Kernel::Pthread* thread)
{
	c_thread_pad_launch(self, thread, attr, "");
}

static KYTY_SYSV_ABI void c_thread_pad_named_attr_launch(CxxThreadPad* self, const char* name,
                                                         const Kernel::PthreadAttr* attr, Kernel::Pthread* thread)
{
	c_thread_pad_launch(self, thread, attr, name);
}

static KYTY_SYSV_ABI void c_bad_alloc_dtor(void* /*self*/) {}

static KYTY_SYSV_ABI void c_bad_cast_dtor(void* /*self*/) {}

static KYTY_SYSV_ABI void c_bad_cast_deleting_dtor(void* self)
{
	cxx_delete_sized(self, sizeof(void*));
}

static KYTY_SYSV_ABI const char* c_bad_cast_what(const void* /*self*/)
{
	return "bad cast";
}

static KYTY_SYSV_ABI void c_bad_cast_doraise(const void* /*self*/)
{
	EXIT("std::bad_cast::_Doraise requires guest exception unwinding\n");
}

static KYTY_SYSV_ABI void c_bad_alloc_deleting_dtor(void* self)
{
	cxx_delete_sized(self, sizeof(void*));
}

static KYTY_SYSV_ABI const char* c_bad_alloc_what(const void* /*self*/)
{
	return "std::bad_alloc";
}

static KYTY_SYSV_ABI void c_bad_array_new_length_dtor(void* /*self*/) {}

static KYTY_SYSV_ABI void c_bad_array_new_length_deleting_dtor(void* self)
{
	cxx_delete_sized(self, sizeof(void*));
}

static KYTY_SYSV_ABI const char* c_bad_array_new_length_what(const void* /*self*/)
{
	return "bad allocation";
}

static KYTY_SYSV_ABI void c_bad_alloc_doraise(const void* /*self*/)
{
	EXIT("std::bad_alloc::_Doraise requires guest exception unwinding\n");
}

// Itanium ABI vtable groups begin with offset-to-top and type_info. Guest
// relocations select the address point at slot two for object vptrs.
static void* g_bad_cast_vtable[] = {
    nullptr,
    &g_typeinfo_bad_cast,
    reinterpret_cast<void*>(&c_bad_cast_dtor),
    reinterpret_cast<void*>(&c_bad_cast_deleting_dtor),
    reinterpret_cast<void*>(&c_bad_cast_what),
    reinterpret_cast<void*>(&c_bad_cast_doraise),
};

static void* g_bad_alloc_vtable[] = {
    nullptr,
    &g_typeinfo_bad_alloc,
    reinterpret_cast<void*>(&c_bad_alloc_dtor),
    reinterpret_cast<void*>(&c_bad_alloc_deleting_dtor),
    reinterpret_cast<void*>(&c_bad_alloc_what),
    reinterpret_cast<void*>(&c_bad_alloc_doraise),
};

static void* g_bad_array_new_length_vtable[] = {
    nullptr,
    &g_typeinfo_bad_array_new_length,
    reinterpret_cast<void*>(&c_bad_array_new_length_dtor),
    reinterpret_cast<void*>(&c_bad_array_new_length_deleting_dtor),
    reinterpret_cast<void*>(&c_bad_array_new_length_what),
    reinterpret_cast<void*>(&c_bad_alloc_doraise),
};

struct CxxErrorCategoryLayout;

struct alignas(8) CxxErrorCodeLayout
{
	int32_t                       value;
	uint32_t                      padding;
	const CxxErrorCategoryLayout* category;
};

using CxxErrorConditionLayout = CxxErrorCodeLayout;

struct alignas(8) CxxErrorCategoryLayout
{
	void**      vtable;
	const char* name;
};

static KYTY_SYSV_ABI void c_error_category_dtor(CxxErrorCategoryLayout* /*self*/) {}
static KYTY_SYSV_ABI void c_system_error_dtor(void* /*self*/) {}

static KYTY_SYSV_ABI const char* c_error_category_name(const CxxErrorCategoryLayout* self)
{
	return self != nullptr && self->name != nullptr ? self->name : "unknown";
}

static CxxStringLayout* CxxStringConstruct(CxxStringLayout* result, const std::string& value)
{
	EXIT_IF(result == nullptr);

	*result          = {};
	result->capacity = sizeof(result->storage.inline_data) - 1;
	result->size     = value.size();

	char* destination = result->storage.inline_data;
	if (value.size() > result->capacity)
	{
		EXIT_IF(value.size() == SIZE_MAX);
		destination = static_cast<char*>(allocate_with_owner(value.size() + 1));
		EXIT_IF(destination == nullptr);
		result->storage.allocated.data = destination;
		result->capacity               = value.size();
	}

	::memcpy(destination, value.c_str(), value.size() + 1);
	return result;
}

// std::future_category() messages of the guest's C++ library for the future_errc
// values 1-4 (broken_promise, future_already_retrieved, promise_already_satisfied,
// no_state); other values take the errno text, as there.
static const char* const g_future_error_messages[] = {"broken promise", "future already retrieved", "promise already satisfied",
                                                      "no state"};

static KYTY_SYSV_ABI CxxStringLayout* c_error_category_message(CxxStringLayout* result, const CxxErrorCategoryLayout* self,
                                                               int32_t value)
{
	const bool future = self != nullptr && self->name != nullptr && ::strcmp(self->name, "future") == 0;
	if (future && value >= 1 && value <= 4)
	{
		return CxxStringConstruct(result, g_future_error_messages[value - 1]);
	}
	const bool system = self != nullptr && self->name != nullptr && ::strcmp(self->name, "system") == 0;
	const auto& category = system ? std::system_category() : std::generic_category();
	return CxxStringConstruct(result, std::error_code(value, category).message());
}

static KYTY_SYSV_ABI CxxErrorConditionLayout c_error_category_default_error_condition(const CxxErrorCategoryLayout* self, int32_t value)
{
	return {value, 0, self};
}

static KYTY_SYSV_ABI int c_error_category_equivalent_condition(const CxxErrorCategoryLayout* self, int32_t value,
                                                              const CxxErrorConditionLayout* condition)
{
	if (condition == nullptr)
	{
		return 0;
	}

	const auto expected = c_error_category_default_error_condition(self, value);
	return condition->value == expected.value && condition->category == expected.category ? 1 : 0;
}

static KYTY_SYSV_ABI int c_error_category_equivalent_code(const CxxErrorCategoryLayout* self, const CxxErrorCodeLayout* code,
                                                          int32_t condition)
{
	return code != nullptr && code->value == condition && code->category == self ? 1 : 0;
}

static void* g_error_category_vtable[8] = {
    reinterpret_cast<void*>(&c_error_category_dtor),
    reinterpret_cast<void*>(&c_error_category_dtor),
    reinterpret_cast<void*>(&c_error_category_name),
    reinterpret_cast<void*>(&c_error_category_message),
    reinterpret_cast<void*>(&c_error_category_default_error_condition),
    reinterpret_cast<void*>(&c_error_category_equivalent_condition),
    reinterpret_cast<void*>(&c_error_category_equivalent_code),
    reinterpret_cast<void*>(&CxxVtableNoop),
};

static CxxErrorCategoryLayout g_generic_error_category {g_error_category_vtable, "generic"};
static CxxErrorCategoryLayout g_system_error_category {g_error_category_vtable, "system"};
static CxxErrorCategoryLayout g_future_error_category {g_error_category_vtable, "future"};

static KYTY_SYSV_ABI const CxxErrorCategoryLayout* c_generic_category()
{
	return &g_generic_error_category;
}

static KYTY_SYSV_ABI const CxxErrorCategoryLayout* c_system_category()
{
	return &g_system_error_category;
}

static KYTY_SYSV_ABI const CxxErrorCategoryLayout* c_future_category()
{
	return &g_future_error_category;
}

static const char g_ti_name_error_category[] = "St14error_category";
static CxxTypeInfoLayout g_typeinfo_error_category {g_class_type_info_vtable, g_ti_name_error_category};

// streamoff sentinel and fpz (common libc++/MSVC objects; zero-safe).
static std::int64_t g_bad_off = -1;
static std::uint8_t g_fpz[16] {};

// The shared_ptr control block uses this process-wide lock to serialize
// reference-count ownership changes made by different guest threads.
static Core::Mutex g_shared_ptr_spin_lock;

static KYTY_SYSV_ABI void c_Lock_shared_ptr_spin_lock()
{
	g_shared_ptr_spin_lock.Lock();
}

static KYTY_SYSV_ABI void c_Unlock_shared_ptr_spin_lock()
{
	g_shared_ptr_spin_lock.Unlock();
}

// _Locksyslock / _Unlocksyslock — CRT global lock; no-op is correct while HLE
// is single-threaded for these paths. Arg is lock index (guest passes 0).
static KYTY_SYSV_ABI void c_Locksyslock(int /*index*/) {}
static KYTY_SYSV_ABI void c_Unlocksyslock(int /*index*/) {}

// _Lockfilelock / _Unlockfilelock — per-FILE stdio lock. Guest FILE* is a host
// FILE*, so map to flockfile/funlockfile to keep the host lock consistent.
static KYTY_SYSV_ABI void c_Lockfilelock(FILE* f)
{
	if (f != nullptr)
	{
		::flockfile(f);
	}
}
static KYTY_SYSV_ABI void c_Unlockfilelock(FILE* f)
{
	if (f != nullptr)
	{
		::funlockfile(f);
	}
}

// std::_Fiopen — open a FILE* for fstream from an ios_base::openmode bitmask.
// Dinkumware openmode bits: in=0x01 out=0x02 ate=0x04 app=0x08 trunc=0x10
// binary=0x20 _Nocreate=0x40 _Noreplace=0x80.
static KYTY_SYSV_ABI FILE* c_Fiopen(const char* name, int mode, int /*prot*/)
{
	if (name == nullptr)
	{
		return nullptr;
	}
	const bool in     = (mode & 0x01) != 0;
	const bool out    = (mode & 0x02) != 0;
	const bool app    = (mode & 0x08) != 0;
	const bool trunc  = (mode & 0x10) != 0;
	const bool binary = (mode & 0x20) != 0;
	char        buf[8];
	int         i = 0;
	buf[i++] = (app ? 'a' : (out || trunc) && !(in && !trunc && !out) ? 'w' : 'r');
	if ((in && (out || trunc)) || (in && app))
	{
		buf[i++] = '+';
	}
	if (binary)
	{
		buf[i++] = 'b';
	}
	buf[i] = '\0';
	return ::fopen(name, buf);
}

// std::exception / std::runtime_error destructors. The object storage is
// guest-owned; a no-op dtor leaks the (rare) embedded message but is safe.
static KYTY_SYSV_ABI void c_std_exception_dtor(void* /*self*/) {}
static KYTY_SYSV_ABI void c_std_runtime_error_dtor(void* /*self*/) {}

// std::iostream_category() — on libstdc++ this is the same object as
// generic_category(), so return the shared generic singleton.
static KYTY_SYSV_ABI const CxxErrorCategoryLayout* c_iostream_category()
{
	return &g_generic_error_category;
}

// std::_Xbad_alloc / _Xbad_function_call — throw helpers. Now that guest
// exception unwinding works, construct a minimal exception object and throw it
// with the matching guest typeinfo so a guest catch(...)/catch(bad_alloc&) can
// handle it. The object vptr uses the existing Itanium vtable address point so
// a virtual what() call resolves to the real message.
static KYTY_SYSV_ABI const char* c_bad_function_call_what(const void* /*self*/)
{
	return "std::bad_function_call";
}
static void* g_bad_function_call_vtable[] = {
    nullptr,
    &g_typeinfo_bad_function_call,
    reinterpret_cast<void*>(&CxxVtableNoop), // dtor — no embedded storage to free
    reinterpret_cast<void*>(&CxxVtableNoop), // deleting dtor
    reinterpret_cast<void*>(&c_bad_function_call_what),
    reinterpret_cast<void*>(&CxxVtableNoop), // _Doraise
};

// std::logic_error-derived exceptions raised by the runtime's _X helpers. The
// object holds its vptr and its message (stored inline after it); what()
// returns the message. Catch clauses match on the thrown type_info.
static KYTY_SYSV_ABI const char* c_logic_error_what(const void* self)
{
	return static_cast<const char* const*>(self)[1];
}
static void* g_logic_error_object_vtable[] = {
    nullptr,
    nullptr,
    reinterpret_cast<void*>(&CxxVtableNoop), // dtor — the message lives in the exception allocation
    reinterpret_cast<void*>(&CxxVtableNoop), // deleting dtor
    reinterpret_cast<void*>(&c_logic_error_what),
    reinterpret_cast<void*>(&CxxVtableNoop),
};

[[noreturn]] static void ThrowLogicError(const void* type_info, const char* msg)
{
	const char*  text   = msg != nullptr ? msg : "";
	const size_t length = std::strlen(text) + 1;
	auto**       obj    = static_cast<void**>(__cxa_allocate_exception(2 * sizeof(void*) + length));
	auto*        copy   = reinterpret_cast<char*>(obj + 2);
	std::memcpy(copy, text, length);
	obj[0] = &g_logic_error_object_vtable[2]; // Itanium vtable address point
	obj[1] = copy;
	__cxa_throw(obj, static_cast<const std::type_info*>(type_info), nullptr);
}

static KYTY_SYSV_ABI void c_Xout_of_range(const char* msg)
{
	ThrowLogicError(&g_typeinfo_out_of_range, msg);
}
static KYTY_SYSV_ABI void c_Xlength_error(const char* msg)
{
	ThrowLogicError(&g_typeinfo_length_error, msg);
}
static KYTY_SYSV_ABI void c_Xinvalid_argument(const char* msg)
{
	ThrowLogicError(&g_typeinfo_invalid_argument, msg);
}

// Dinkumware wctrans_t values: 1 folds to lower case, 2 to upper case. The
// guest runs in the "C" locale, which maps only the ASCII letters.
static KYTY_SYSV_ABI uint32_t c_Towctrans(uint32_t character, int transform)
{
	if (transform != 1 && transform != 2)
	{
		EXIT("_Towctrans: unknown transform %d\n", transform);
	}
	if (transform == 1 && character >= 'A' && character <= 'Z')
	{
		return character + ('a' - 'A');
	}
	if (transform == 2 && character >= 'a' && character <= 'z')
	{
		return character - ('a' - 'A');
	}
	return character;
}

static KYTY_SYSV_ABI void c_Xbad_alloc()
{
	auto** obj = static_cast<void**>(__cxa_allocate_exception(8));
	obj[0]     = &g_bad_alloc_vtable[2]; // Itanium vtable address point
	__cxa_throw(obj, reinterpret_cast<const std::type_info*>(&g_typeinfo_bad_alloc), nullptr);
}
static KYTY_SYSV_ABI void c_Xbad_function_call()
{
	auto** obj = static_cast<void**>(__cxa_allocate_exception(8));
	obj[0]     = &g_bad_function_call_vtable[2];
	__cxa_throw(obj, reinterpret_cast<const std::type_info*>(&g_typeinfo_bad_function_call), nullptr);
}

// _Dtest — Dinkumware ctype mask table pointer (same table _Getpctype returns).
static const unsigned short* g_dtest_table = g_c_locale_ctype.data() + 1;

// std::locale::_Getgloballocale — return classic Locimp.
static KYTY_SYSV_ABI void* c_locale_Getgloballocale()
{
	return &g_classic_locimp;
}

static KYTY_SYSV_ABI void* c_locale_CreateClassicLocimp()
{
	return &g_classic_locimp;
}

static KYTY_SYSV_ABI const CxxLocaleLayout* c_locale_classic()
{
	return &g_sce_classic_locale;
}

static KYTY_SYSV_ABI void c_locale_InitTemporaryInfo(void* /*self*/, const char* /*name*/, std::uint64_t /*category*/)
{
	// Captured caller only needs the temporary to be accepted before passing it
	// back to libc cleanup HLE; no guest-visible fields are read at this site.
}

static KYTY_SYSV_ABI void c_locale_DestroyTemporaryInfo(void* /*self*/) {}

static KYTY_SYSV_ABI void c_locale_RegisterFacet(void* /*self*/) {}

static KYTY_SYSV_ABI void c_cxa_pure_virtual()
{
	EXIT("__cxa_pure_virtual\n");
}

// std::exception::_Doraise() is the base virtual hook. It intentionally does
// nothing; derived exception types override it when they need to raise.
static KYTY_SYSV_ABI void c_exception_doraise(const void* /*self*/) {}

// std::uncaught_exception() — returns non-zero while an exception is in flight.
// Forward to the host so destructors observe the real state during a guest
// unwind (the host runtime tracks the count in its own TLS).
static KYTY_SYSV_ABI int c_uncaught_exception()
{
	return std::uncaught_exception() ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Guest C++ exception personality (Itanium __gxx_personality_v0).
//
// The host libgcc unwinder performs the mechanical DWARF frame walk over the
// guest's registered .eh_frame; that part is ABI-neutral. The *personality*,
// however, must interpret the guest's LSDA (call-site/action/type tables) and
// decide which catch clause matches. The host __gxx_personality_v0 cannot be
// used because it matches types by calling __do_catch/__do_upcast on the
// caught/thrown type_info, and the guest (PS5 toolchain) type_info vtable
// layout differs from host __cxxabiv1 — dispatching a host virtual on a guest
// vtable jumps to a null slot.
//
// Instead we parse the LSDA ourselves and match the catch type against the
// thrown type by pointer equality, falling back to comparing the mangled
// type_info name field (offset +8). That covers the dominant IL2CPP pattern
// (catch(Il2CppExceptionWrapper&) catching Il2CppExceptionWrapper) and
// catch(...) without ever dispatching on a guest vtable.
// ---------------------------------------------------------------------------

// Defined later in this translation unit; used by GuestTypeMatch to probe
// candidate base-class type_info pointers without trusting guest data.
static size_t CxaSafeRead(void* dst, const void* src, size_t n);

namespace {

constexpr uint8_t kDwEhPeOmit    = 0xff;
constexpr uint8_t kDwEhPeUleb128 = 0x01;
constexpr uint8_t kDwEhPeUdata2  = 0x02;
constexpr uint8_t kDwEhPeUdata4  = 0x03;
constexpr uint8_t kDwEhPeUdata8  = 0x04;
constexpr uint8_t kDwEhPeSleb128 = 0x09;
constexpr uint8_t kDwEhPeSdata2  = 0x0a;
constexpr uint8_t kDwEhPeSdata4  = 0x0b;
constexpr uint8_t kDwEhPeSdata8  = 0x0c;
constexpr uint8_t kDwEhPePcrel   = 0x10;
constexpr uint8_t kDwEhPeTextrel = 0x20;
constexpr uint8_t kDwEhPeDatarel = 0x30;
constexpr uint8_t kDwEhPeFuncrel = 0x40;
constexpr uint8_t kDwEhPeAligned = 0x50;
constexpr uint8_t kDwEhPeIndirect = 0x80;

struct GuestLsdaInfo
{
	uint64_t start;
	uint64_t lpstart;
	uint64_t ttype_base;
	const uint8_t* ttype;
	const uint8_t* action_table;
	uint8_t        ttype_encoding;
	uint8_t        call_site_encoding;
};

const uint8_t* GuestReadUleb128(const uint8_t* p, uint64_t* val)
{
	uint64_t result = 0;
	uint32_t shift  = 0;
	uint8_t  byte;
	do
	{
		byte = *p++;
		result |= (static_cast<uint64_t>(byte) & 0x7f) << shift;
		shift += 7;
	} while (byte & 0x80);
	*val = result;
	return p;
}

const uint8_t* GuestReadSleb128(const uint8_t* p, int64_t* val)
{
	uint64_t result = 0;
	uint32_t shift  = 0;
	uint8_t  byte;
	do
	{
		byte = *p++;
		result |= (static_cast<uint64_t>(byte) & 0x7f) << shift;
		shift += 7;
	} while (byte & 0x80);
	if (shift < 64 && (byte & 0x40) != 0)
	{
		result |= static_cast<uint64_t>(-(static_cast<int64_t>(1) << shift));
	}
	*val = static_cast<int64_t>(result);
	return p;
}

uint64_t GuestEhBaseOfEncoded(uint8_t encoding, _Unwind_Context* context)
{
	switch (encoding & 0x70)
	{
		case 0x00:
		case kDwEhPePcrel:
		case kDwEhPeAligned: return 0;
		case kDwEhPeTextrel: return _Unwind_GetTextRelBase(context);
		case kDwEhPeDatarel: return _Unwind_GetDataRelBase(context);
		case kDwEhPeFuncrel: return _Unwind_GetRegionStart(context);
		default: return 0;
	}
}

const uint8_t* GuestReadEncodedValue(uint8_t encoding, uint64_t base, const uint8_t* p, uint64_t* val)
{
	uint64_t result = 0;
	const auto* u   = p;
	switch (encoding & 0x0f)
	{
		case 0x00: // absptr
			std::memcpy(&result, u, sizeof(void*));
			p += sizeof(void*);
			break;
		case kDwEhPeUleb128:
		{
			uint64_t tmp = 0;
			p            = GuestReadUleb128(p, &tmp);
			result       = tmp;
			break;
		}
		case kDwEhPeSleb128:
		{
			int64_t tmp = 0;
			p           = GuestReadSleb128(p, &tmp);
			result      = static_cast<uint64_t>(tmp);
			break;
		}
		case kDwEhPeUdata2:
		{
			uint16_t t = 0;
			std::memcpy(&t, u, 2);
			result = t;
			p += 2;
			break;
		}
		case kDwEhPeUdata4:
		{
			uint32_t t = 0;
			std::memcpy(&t, u, 4);
			result = t;
			p += 4;
			break;
		}
		case kDwEhPeUdata8:
		{
			uint64_t t = 0;
			std::memcpy(&t, u, 8);
			result = t;
			p += 8;
			break;
		}
		case kDwEhPeSdata2:
		{
			int16_t t = 0;
			std::memcpy(&t, u, 2);
			result = static_cast<uint64_t>(static_cast<int64_t>(t));
			p += 2;
			break;
		}
		case kDwEhPeSdata4:
		{
			int32_t t = 0;
			std::memcpy(&t, u, 4);
			result = static_cast<uint64_t>(static_cast<int64_t>(t));
			p += 4;
			break;
		}
		case kDwEhPeSdata8:
		{
			int64_t t = 0;
			std::memcpy(&t, u, 8);
			result = static_cast<uint64_t>(t);
			p += 8;
			break;
		}
		default:
			*val = 0;
			return p;
	}
	if (result != 0)
	{
		result += ((encoding & 0x70) == kDwEhPePcrel) ? reinterpret_cast<uint64_t>(u) : base;
		if (encoding & kDwEhPeIndirect)
		{
			result = *reinterpret_cast<uint64_t*>(result);
		}
	}
	*val = result;
	return p;
}

unsigned GuestSizeOfEncodedValue(uint8_t encoding)
{
	switch (encoding & 0x07)
	{
		case 0x00: return sizeof(void*);
		case kDwEhPeUdata2: return 2;
		case kDwEhPeUdata4: return 4;
		case kDwEhPeUdata8: return 8;
		default: return 0;
	}
}

const uint8_t* GuestParseLsdaHeader(_Unwind_Context* context, const uint8_t* p, GuestLsdaInfo* info)
{
	info->start = (context != nullptr) ? _Unwind_GetRegionStart(context) : 0;

	const uint8_t lpstart_encoding = *p++;
	if (lpstart_encoding != kDwEhPeOmit)
	{
		p = GuestReadEncodedValue(lpstart_encoding, GuestEhBaseOfEncoded(lpstart_encoding, context), p, &info->lpstart);
	} else
	{
		info->lpstart = info->start;
	}

	info->ttype_encoding = *p++;
	if (info->ttype_encoding != kDwEhPeOmit)
	{
		uint64_t tmp = 0;
		p            = GuestReadUleb128(p, &tmp);
		info->ttype  = p + tmp;
	} else
	{
		info->ttype = nullptr;
	}

	info->call_site_encoding = *p++;
	uint64_t tmp             = 0;
	p                        = GuestReadUleb128(p, &tmp);
	info->action_table       = p + tmp;

	return p;
}

// Read a pointer-sized field from a possibly-guest address; returns the stored
// pointer or nullptr when the source is not readable.
const void* GuestSafePtr(const void* addr)
{
	const void* v = nullptr;
	return (CxaSafeRead(&v, addr, sizeof(v)) == sizeof(v) ? v : nullptr);
}

// A guest type_info is "[vptr][name]"; `ti` is a plausible type_info when it is
// readable and its +8 name pointer addresses readable, non-empty bytes.
bool GuestTypeInfoPlausible(const void* ti)
{
	if (ti == nullptr)
	{
		return false;
	}
	const auto* name = static_cast<const char*>(GuestSafePtr(static_cast<const uint8_t*>(ti) + 8));
	char        first = 0;
	return name != nullptr && CxaSafeRead(&first, name, 1) == 1 && first != 0;
}

// Compare two type_info objects by their +8 mangled names via bounded safe
// reads (never a raw strcmp on unvalidated guest pointers).
bool GuestTypeNameMatch(const void* a, const void* b)
{
	const auto* na = static_cast<const char*>(GuestSafePtr(static_cast<const uint8_t*>(a) + 8));
	const auto* nb = static_cast<const char*>(GuestSafePtr(static_cast<const uint8_t*>(b) + 8));
	if (na == nullptr || nb == nullptr)
	{
		return false;
	}
	char   ba[96];
	char   bb[96];
	size_t ra = CxaSafeRead(ba, na, sizeof(ba) - 1);
	size_t rb = CxaSafeRead(bb, nb, sizeof(bb) - 1);
	if (ra == 0 || rb == 0)
	{
		return false;
	}
	ba[ra] = '\0';
	bb[rb] = '\0';
	if (std::memchr(ba, '\0', ra) == nullptr || std::memchr(bb, '\0', rb) == nullptr)
	{
		return false;
	}
	return std::strcmp(ba, bb) == 0;
}

// Match a caught type against the thrown type without ever dispatching on a
// guest type_info vtable: pointer equality, then mangled-name equality, then a
// bounded walk over the thrown type's base classes so catch(Base&) matches a
// thrown Derived. __si_class_type_info keeps a single base type_info* at +0x10;
// __vmi_class_type_info keeps {__flags,__base_count} at +0x10 and an array of
// __base_class_type_info {base_type*, offset_flags} starting at +0x18.
bool GuestTypeMatchDepth(const void* catch_type, const void* throw_type, int depth)
{
	if (catch_type == nullptr)
	{
		return true; // catch(...) — the type table entry is null
	}
	if (throw_type == nullptr || depth > 8 || !GuestTypeInfoPlausible(throw_type))
	{
		return false;
	}
	if (catch_type == throw_type || GuestTypeNameMatch(catch_type, throw_type))
	{
		return true;
	}
	const auto* t = static_cast<const uint8_t*>(throw_type);
	if (const void* si_base = GuestSafePtr(t + 0x10);
	    si_base != nullptr && si_base != throw_type && GuestTypeInfoPlausible(si_base) &&
	    GuestTypeMatchDepth(catch_type, si_base, depth + 1))
	{
		return true;
	}
	uint32_t base_count = 0;
	if (CxaSafeRead(&base_count, t + 0x14, sizeof(base_count)) == sizeof(base_count) && base_count > 0 &&
	    base_count < 64)
	{
		const uint8_t* arr = t + 0x18;
		for (uint32_t i = 0; i < base_count; i++)
		{
			const void* b = GuestSafePtr(arr + i * 16);
			if (b != nullptr && GuestTypeInfoPlausible(b) && GuestTypeMatchDepth(catch_type, b, depth + 1))
			{
				return true;
			}
		}
	}
	return false;
}

bool GuestTypeMatch(const void* catch_type, const void* throw_type)
{
	return GuestTypeMatchDepth(catch_type, throw_type, 0);
}

} // namespace

// Itanium ABI __gxx_personality_v0. Registered as the CIE personality for guest
// frames; invoked by the host libgcc unwinder once per guest frame, in both the
// search and cleanup phases.
static KYTY_SYSV_ABI int c_gxx_personality_v0(int version, int actions, uint64_t exception_class,
                                              void* exception_object, void* context_ptr)
{
	auto* context  = static_cast<_Unwind_Context*>(context_ptr);
	auto* ue_header = static_cast<_Unwind_Exception*>(exception_object);

	if (version != 1 || ue_header == nullptr || context == nullptr)
	{
		return _URC_FATAL_PHASE1_ERROR;
	}
	if ((actions & _UA_FORCE_UNWIND) != 0)
	{
		// Forced unwind (pthread_exit etc.) has no catch semantics here.
		return _URC_CONTINUE_UNWIND;
	}

	const uint8_t* lsda = static_cast<const uint8_t*>(_Unwind_GetLanguageSpecificData(context));
	if (lsda == nullptr)
	{
		return _URC_CONTINUE_UNWIND;
	}

	GuestLsdaInfo info {};
	const uint8_t* p = GuestParseLsdaHeader(context, lsda, &info);
	info.ttype_base  = GuestEhBaseOfEncoded(info.ttype_encoding, context);

	int       ip_before_insn = 0;
	uint64_t  ip             = _Unwind_GetIPInfo(context, &ip_before_insn);
	if (!ip_before_insn)
	{
		ip--;
	}

	uint64_t        landing_pad         = 0;
	const uint8_t*  action_record       = nullptr;
	const void*     throw_type_dbg      = nullptr;
	int             handler_switch      = 0;
	const int       found_nothing       = 0;
	const int       found_terminate     = 1;
	const int       found_cleanup       = 2;
	const int       found_handler       = 3;
	int             found_type          = found_terminate;
	bool            found_something     = false;

	// Scan the call-site table for the entry covering the throwing IP.
	while (p < info.action_table)
	{
		uint64_t cs_start = 0;
		uint64_t cs_len   = 0;
		uint64_t cs_lp    = 0;
		uint64_t cs_action = 0;
		p = GuestReadEncodedValue(info.call_site_encoding, 0, p, &cs_start);
		p = GuestReadEncodedValue(info.call_site_encoding, 0, p, &cs_len);
		p = GuestReadEncodedValue(info.call_site_encoding, 0, p, &cs_lp);
		p = GuestReadUleb128(p, &cs_action);

		if (ip < info.start + cs_start)
		{
			break; // sorted table — past the throw site
		}
		if (ip < info.start + cs_start + cs_len)
		{
			if (cs_lp != 0)
			{
				landing_pad = info.lpstart + cs_lp;
			}
			if (cs_action != 0)
			{
				action_record = info.action_table + cs_action - 1;
			}
			found_something = true;
			break;
		}
	}

	if (!found_something)
	{
		found_type = found_terminate;
	} else if (landing_pad == 0)
	{
		found_type = found_nothing;
	} else if (action_record == nullptr)
	{
		found_type = found_cleanup;
	} else
	{
		// Catch handler or exception spec. Resolve the thrown type from the
		// __cxa_exception header that precedes the unwind header.
		auto*        cxa         = reinterpret_cast<KytyCxaException*>(reinterpret_cast<uint8_t*>(ue_header) -
                                                                 offsetof(KytyCxaException, unwindHeader));
		const void*  throw_type  = cxa->exceptionType;
		throw_type_dbg           = throw_type;
		bool         saw_cleanup = false;
		bool         saw_handler = false;

		while (true)
		{
			const uint8_t* ap = action_record;
			int64_t        ar_filter = 0;
			int64_t        ar_disp   = 0;
			ap = GuestReadSleb128(ap, &ar_filter);
			GuestReadSleb128(ap, &ar_disp);

			if (ar_filter == 0)
			{
				saw_cleanup = true;
			} else if ((actions & _UA_CLEANUP_PHASE) != 0 && (actions & _UA_HANDLER_FRAME) == 0)
			{
				// During phase-2 cleanup of a non-handler frame, handler
				// entries don't re-match.
			} else if (ar_filter > 0)
			{
				// Positive filter: catch handler. TType entries index the
				// type table backward from its base.
				const uint8_t* e  = info.ttype - static_cast<size_t>(ar_filter) * GuestSizeOfEncodedValue(info.ttype_encoding);
				uint64_t       tp = 0;
				GuestReadEncodedValue(info.ttype_encoding, info.ttype_base, e, &tp);
				const void* catch_type = reinterpret_cast<const void*>(tp);
				if (GuestTypeMatch(catch_type, throw_type))
				{
					saw_handler    = true;
					handler_switch = static_cast<int>(ar_filter);
					break;
				}
			} else
			{
				// Negative filter: exception specification. We cannot evaluate
				// the guest spec safely; treat an empty spec as terminate, else
				// accept it as a handler so unwinding can proceed.
				const uint8_t* e  = info.ttype - static_cast<size_t>(-ar_filter) - 1;
				uint64_t       first = 0;
				const uint8_t* e2 = GuestReadUleb128(e, &first);
				(void)e2;
				if (first == 0)
				{
					found_type = found_terminate;
					goto decided;
				}
				saw_handler    = true;
				handler_switch = static_cast<int>(ar_filter);
				break;
			}

			if (ar_disp == 0)
			{
				break;
			}
			action_record = ap + ar_disp;
		}

		if (saw_handler)
		{
			found_type = found_handler;
		} else
		{
			found_type = saw_cleanup ? found_cleanup : found_nothing;
		}
	}

decided:
	static const bool dbg = [] { const char* e = std::getenv("KYTY_EH_TRACE"); return e != nullptr && e[0] == '1'; }();
	if (dbg)
	{
		fprintf(stderr, "EH persona v%d act=%x ip=%llx lp=%llx ft=%d sw=%d tt=%p\n", version, actions,
		        (unsigned long long)ip, (unsigned long long)landing_pad, found_type, handler_switch,
		        (void*)throw_type_dbg);
	}
	if (found_type == found_nothing)
	{
		return _URC_CONTINUE_UNWIND;
	}

	if ((actions & _UA_SEARCH_PHASE) != 0)
	{
		if (found_type == found_cleanup)
		{
			return _URC_CONTINUE_UNWIND;
		}
		// Cache the caught-exception state in the __cxa_exception header, exactly
		// as save_caught_exception does — __cxa_begin_catch returns adjustedPtr,
		// and the handler frame is re-entered in phase 2 to run the catch.
		auto* cxa = reinterpret_cast<KytyCxaException*>(reinterpret_cast<uint8_t*>(ue_header) -
		                                              offsetof(KytyCxaException, unwindHeader));
		cxa->handlerSwitchValue   = handler_switch;
		cxa->actionRecord         = action_record;
		cxa->languageSpecificData = lsda;
		cxa->adjustedPtr          = reinterpret_cast<uint8_t*>(ue_header) + sizeof(_Unwind_Exception);
		cxa->catchTemp            = reinterpret_cast<void*>(landing_pad);
		return _URC_HANDLER_FOUND;
	}

	// Phase 2: install the context to transfer control to the landing pad.
	if (found_type == found_terminate)
	{
		return _URC_CONTINUE_UNWIND;
	}
	_Unwind_SetGR(context, 0, reinterpret_cast<uint64_t>(ue_header));
	_Unwind_SetGR(context, 1, static_cast<uint64_t>(handler_switch));
	_Unwind_SetIP(context, landing_pad);
	return _URC_INSTALL_CONTEXT;
}

// std::ios_base::~ios_base() — guest tears down temporary stream objects after
// locale/ctype probes. No host side-effects required for the stub ios_base.
static KYTY_SYSV_ABI void c_ios_base_dtor(void* /*self*/) {}

// std::ios_base::failure::~failure() [complete object]. Guest code owns the
// storage; the HLE exception/locale objects have no host-side payload to tear
// down, so destruction deliberately leaves the guest allocation untouched.
static KYTY_SYSV_ABI void c_ios_base_failure_dtor(void* /*self*/) {}

// Itanium C++ ABI exception entry points (Gen5 libc_v1). Guest code executes
// natively, so these forward to the host libstdc++/libgcc implementations; the
// host runtime owns the __cxa_exception header and the per-thread caught-
// exception stack, keeping every step of the throw/catch lifecycle coherent.
static KYTY_SYSV_ABI void* cxa_allocate_exception(size_t thrown_size)
{
	return __cxa_allocate_exception(thrown_size);
}

static KYTY_SYSV_ABI void cxa_free_exception(void* thrown_exception)
{
	__cxa_free_exception(thrown_exception);
}

static KYTY_SYSV_ABI void* cxa_begin_catch(void* exception_object)
{
	return __cxa_begin_catch(exception_object);
}

static KYTY_SYSV_ABI void cxa_end_catch()
{
	__cxa_end_catch();
}

static KYTY_SYSV_ABI void cxa_rethrow()
{
	__cxa_rethrow();
}

// std::exception_ptr support, absent from the statically-linked libstdc++.
// Shared-exception refcounts are tracked in a side table keyed by the thrown
// object so the host libstdc++ header layout is never probed for a field it
// does not carry.
static std::unordered_map<void*, unsigned>& CxaSharedRefcounts()
{
	static std::unordered_map<void*, unsigned> refcounts;
	return refcounts;
}

// __cxa_current_primary_exception — the primary (non-dependent) unwind header
// for the exception currently being propagated or handled.
static KYTY_SYSV_ABI void* cxa_current_primary_exception()
{
	auto* globals = static_cast<KytyCxaEhGlobals*>(__cxa_get_globals());
	if (globals == nullptr || globals->caughtExceptions == nullptr)
	{
		return nullptr;
	}
	return &globals->caughtExceptions->unwindHeader;
}

static KYTY_SYSV_ABI void cxa_increment_exception_refcount(void* thrown_exception)
{
	if (thrown_exception != nullptr)
	{
		CxaSharedRefcounts()[thrown_exception]++;
	}
}

static KYTY_SYSV_ABI void cxa_decrement_exception_refcount(void* thrown_exception)
{
	if (thrown_exception == nullptr)
	{
		return;
	}
	auto& table = CxaSharedRefcounts();
	auto  it    = table.find(thrown_exception);
	if (it != table.end() && --it->second == 0)
	{
		table.erase(it);
		__cxa_free_exception(thrown_exception);
	}
}

// __cxa_rethrow_primary_exception — raise a dependent exception that refers to
// the still-live primary. Mirrors libc++abi: bump the primary's refcount, then
// raise a dependent object whose unwind header carries the dependent marker.
static KYTY_SYSV_ABI void cxa_rethrow_primary_exception(void* thrown_exception)
{
	if (thrown_exception == nullptr)
	{
		EXIT("__cxa_rethrow_primary_exception(null)\n");
	}
	cxa_increment_exception_refcount(thrown_exception);
	auto* dep = static_cast<KytyCxaDependentException*>(__cxa_allocate_dependent_exception());
	EXIT_IF(dep == nullptr);
	auto* primary            = static_cast<KytyCxaException*>(thrown_exception) - 1;
	dep->primaryException    = thrown_exception;
	dep->exceptionDestructor = primary->exceptionDestructor;
	dep->unexpectedHandler   = primary->unexpectedHandler;
	dep->terminateHandler    = primary->terminateHandler;
	std::memcpy(&dep->unwindHeader.exception_class, "GNUCC++\x01", 8);
	dep->unwindHeader.exception_cleanup = [](_Unwind_Reason_Code, _Unwind_Exception* e) {
		__cxa_free_dependent_exception(reinterpret_cast<KytyCxaDependentException*>(e));
	};
	_Unwind_RaiseException(&dep->unwindHeader);
	EXIT("__cxa_rethrow_primary_exception returned (no landing pad) obj=%p\n", thrown_exception);
}

static KYTY_SYSV_ABI void* cxa_current_exception_type()
{
	return const_cast<std::type_info*>(__cxa_current_exception_type());
}

// Level-2 unwind entry points imported alongside the C++ ABI. Forward to the
// host libgcc unwinder, which unwinds guest frames via the registered
// .eh_frame.
static KYTY_SYSV_ABI int c_unwind_raise_exception(void* exception_object)
{
	return static_cast<int>(_Unwind_RaiseException(static_cast<_Unwind_Exception*>(exception_object)));
}

static KYTY_SYSV_ABI void c_unwind_resume(void* exception_object)
{
	_Unwind_Resume(static_cast<_Unwind_Exception*>(exception_object));
}

static KYTY_SYSV_ABI void c_unwind_resume_or_rethrow(void* exception_object)
{
	_Unwind_Resume_or_Rethrow(static_cast<_Unwind_Exception*>(exception_object));
}

static KYTY_SYSV_ABI void c_unwind_delete_exception(void* exception_object)
{
	_Unwind_DeleteException(static_cast<_Unwind_Exception*>(exception_object));
}

// Read up to `n` bytes from a guest address into `dst` without risking a fault.
// process_vm_readv honours page protections and reports EFAULT for unmapped or
// non-readable pages, so a bad guest pointer can never crash the diagnostic.
// Returns the number of bytes actually readable (0 if the first byte is not).
static size_t CxaSafeRead(void* dst, const void* src, size_t n)
{
	if (dst == nullptr || src == nullptr || n == 0)
	{
		return 0;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	struct iovec local {};
	local.iov_base  = dst;
	local.iov_len   = n;
	struct iovec remote {};
	remote.iov_base = const_cast<void*>(src);
	remote.iov_len  = n;
	const ssize_t r = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
	return r > 0 ? static_cast<size_t>(r) : 0;
#else
	return 0;
#endif
}

[[nodiscard]] static bool CxaGuestPtrLooksMapped(const void* p, size_t bytes)
{
	if (p == nullptr || bytes == 0 || bytes > 256)
	{
		return false;
	}
	unsigned char scratch[256];
	return CxaSafeRead(scratch, p, bytes) == bytes;
}

// Copy a candidate C-string out of guest memory into `buf`; returns `buf` when
// a printable NUL-terminated string fits, else nullptr. Never dereferences `p`
// directly.
static const char* CxaTryReadCString(const void* p, char* buf, size_t bufsz)
{
	if (p == nullptr || buf == nullptr || bufsz < 2)
	{
		return nullptr;
	}
	const size_t n = CxaSafeRead(buf, p, bufsz - 1);
	if (n == 0)
	{
		return nullptr;
	}
	buf[n] = 0;
	for (size_t i = 0; i < n; i++)
	{
		const auto c = static_cast<unsigned char>(buf[i]);
		if (c == 0)
		{
			return i > 0 ? buf : nullptr;
		}
		if (c < 0x09 || (c > 0x0d && c < 0x20))
		{
			return nullptr;
		}
	}
	return nullptr;
}

// Guest type_info / exception layouts (libstdc++ Itanium):
//   type_info:  [0]=vtable, [8]=name (const char*, may be mangled with leading '*')
//   exception with SSO string: after vptr, std::string at +8 (capacity/size/data)
static KYTY_SYSV_ABI void cxa_throw(void* thrown_exception, void* tinfo, void (*dest)(void*))
{
	char        type_buf[256] = {};
	char        what_buf[256] = {};
	const char* type_name     = nullptr;
	const char* what_msg      = nullptr;

	if (CxaGuestPtrLooksMapped(tinfo, 16))
	{
		uint64_t name_ptr = 0;
		if (CxaSafeRead(&name_ptr, static_cast<const uint8_t*>(tinfo) + 8, sizeof(name_ptr)) == sizeof(name_ptr))
		{
			type_name = CxaTryReadCString(reinterpret_cast<const void*>(name_ptr), type_buf, sizeof(type_buf));
			if (type_name != nullptr && type_name[0] == '*')
			{
				type_name++; // libstdc++ marks non-mangled names with a leading '*'
			}
		}
	}

	if (CxaGuestPtrLooksMapped(thrown_exception, 32))
	{
		// Heuristic: many libstdc++ exception objects store a std::string at +8.
		// SSO layout (GCC): local buffer at +16 when capacity field at +24 is small.
		uint64_t words[4] = {};
		CxaSafeRead(words, thrown_exception, sizeof(words));
		const auto cap = words[3]; // often capacity for SSO string
		if (cap <= 15u)
		{
			what_msg = CxaTryReadCString(static_cast<const char*>(thrown_exception) + 16, what_buf, sizeof(what_buf));
		} else
		{
			what_msg = CxaTryReadCString(reinterpret_cast<const void*>(words[1]), what_buf, sizeof(what_buf));
		}
		if (what_msg == nullptr)
		{
			what_msg = CxaTryReadCString(reinterpret_cast<const void*>(words[2]), what_buf, sizeof(what_buf));
		}
	}

	KYTY_LOG_DEBUG("__cxa_throw type=%s what=%s obj=%p tinfo=%p\n", type_name != nullptr ? type_name : "?",
	               what_msg != nullptr ? what_msg : "?", thrown_exception, tinfo);

	// Forward to the real ABI throw: the host unwinder walks the (registered)
	// guest frames and transfers control to the guest's landing pad.
	__cxa_throw(thrown_exception, reinterpret_cast<std::type_info*>(tinfo), dest);
	EXIT("__cxa_throw returned (no landing pad found) type=%s obj=%p\n", type_name != nullptr ? type_name : "?",
	     thrown_exception);
}

static KYTY_SYSV_ABI int atexit(void (*func)())
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("func = %" PRIx64 "\n", reinterpret_cast<uint64_t>(func));

	int ok = ::atexit(func);

	if (ok != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	return 0;
}

static KYTY_SYSV_ABI int libc_printf(VA_ARGS)
{
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	PRINT_NAME();

	return GetPrintfCtxFunc()(&ctx);
}

static KYTY_SYSV_ABI int puts(const char* s)
{
	PRINT_NAME();

	return GetPrintfStdFunc()("%s\n", s);
}

// Gen5 libc_v1 putchar — NID m5wN+SwZOR4. Observed with ch=0x0a (newline)
// after Posix semaphore setup.
static KYTY_SYSV_ABI int c_putchar(int ch)
{
	return GetPrintfStdFunc()("%c", ch);
}

static KYTY_SYSV_ABI void catchReturnFromMain(int status)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("return from main = %d\n", status);
}

KYTY_SYSV_ABI int cxa_atexit(cxa_destructor_func_t func, void* arg, void* d)
{
	PRINT_NAME();

	CxaDestructor c {};
	c.destructor_func   = func;
	c.destructor_object = arg;
	c.module_id         = d;

	{
		std::lock_guard lock(g_cxa_mutex);
		auto* cc = Core::Singleton<CContext>::Instance();
		cc->cxa.Add(c);
	}

	return 0;
}

void KYTY_SYSV_ABI cxa_finalize(void* d)
{
	PRINT_NAME();

	std::vector<CxaDestructor> callbacks;

	{
		std::lock_guard lock(g_cxa_mutex);
		auto* cc = Core::Singleton<CContext>::Instance();
		FOR_LIST_R(i, cc->cxa)
		{
			auto& c = cc->cxa[i];
			if ((d == nullptr || c.module_id == d) && c.destructor_func != nullptr)
			{
				// Claim before invoking so re-entry cannot call this record twice.
				callbacks.push_back(c);
				c.destructor_func = nullptr;
			}
		}
	}

	for (const auto& c: callbacks)
	{
		c.destructor_func(c.destructor_object);
	}
}

// Gen5 libc_v1 wcscpy — NID FM5NPnLqBc8. Wide characters are uint16_t on the
// guest ABI (same as c_vswprintf).
static KYTY_SYSV_ABI uint16_t* c_wcscpy(uint16_t* dst, const uint16_t* src)
{
	if (dst == nullptr || src == nullptr)
	{
		return nullptr;
	}
	uint16_t* d = dst;
	while ((*d++ = *src++) != 0) {}
	return dst;
}

// Gen5 libc_v1 vprintf — NID GMpvxPFW924 (same contract as LibcInternal::vprintf).
static KYTY_SYSV_ABI int c_vprintf(const char* str, VaList* c)
{
	PRINT_NAME();

	return GetVprintfFunc()(str, c);
}

// Gen5 libc_v1 swprintf — NID nJz16JE1txM, over c_vswprintf.
static KYTY_SYSV_ABI int c_swprintf(VA_ARGS)
{
	VA_CONTEXT(ctx);
	uint16_t*       out         = VaArg_ptr<uint16_t>(&ctx.va_list);
	size_t          out_count   = VaArg_size_t(&ctx.va_list);
	const uint16_t* wide_format = VaArg_ptr<const uint16_t>(&ctx.va_list);
	return c_vswprintf(out, out_count, wide_format, &ctx.va_list);
}

static KYTY_SYSV_ABI float c_frexpf(float value, int* exp)
{
	if (exp == nullptr)
	{
		return 0.0f;
	}
	return std::frexpf(value, exp);
}

static KYTY_SYSV_ABI float c_remainderf(float x, float y)
{
	return std::remainderf(x, y);
}

// Gen5 libc_v1 _Thrd_detach — NID L7f7zYwBvZA. POSIX thrd semantics: returns
// the errno-style result of pthread_detach (0 success).
static KYTY_SYSV_ABI int c_thrd_detach(Kernel::Pthread thread)
{
	if (thread == nullptr)
	{
		return Posix::POSIX_EINVAL;
	}
	const int result = Kernel::PthreadDetach(thread);
	return result == OK ? 0 : Posix::POSIX_EINVAL;
}

// Gen5 libc_v1 _Thrd_sleep — NID jfRI3snge3o: int (const xtime* until,
// xtime* remaining). The guest library sleeps until the absolute TIME_UTC time
// `until`; when it wakes before it and `remaining` is given, it stores `until`
// there and returns -1, otherwise it returns 0.
struct GuestXtime
{
	int64_t sec;
	int64_t nsec;
};

static KYTY_SYSV_ABI int c_thrd_sleep(const GuestXtime* until, GuestXtime* remaining)
{
	if (until == nullptr)
	{
		return 0;
	}
	Kernel::KernelTimespec now {};
	if (Kernel::KernelClockGettime(0, &now) == OK)
	{
		int64_t sec  = until->sec - now.tv_sec;
		int64_t nsec = until->nsec - now.tv_nsec;
		if (nsec < 0)
		{
			nsec += 1000000000;
			sec--;
		}
		if (sec > 0 || (sec == 0 && nsec > 0))
		{
			const Kernel::KernelTimespec duration {sec, nsec};
			(void)Kernel::KernelNanosleep(&duration, nullptr);
		}
	}
	if (Kernel::KernelClockGettime(0, &now) != OK || remaining == nullptr)
	{
		return 0;
	}
	if (now.tv_sec < until->sec || (now.tv_sec == until->sec && now.tv_nsec < until->nsec))
	{
		*remaining = *until;
		return -1;
	}
	return 0;
}

// Gen5 libc_v1 _Assert — NID -QgqOT5u2Vk. A real assert failure: report and
// stop structurally.
static KYTY_SYSV_ABI void c_assert(const char* msg, const char* file, int line)
{
	EXIT("_Assert failed: %s (%s:%d)\n", msg != nullptr ? msg : "?", file != nullptr ? file : "?", line);
}

// Gen5 libc_v1 _ZSt16_Throw_Cpp_errori — NID W0j6vCxh9Pc. C++ exceptions
// cannot unwind through the HLE boundary; report structurally like __cxa_throw.
static KYTY_SYSV_ABI void c_throw_cpp_error(int err)
{
	EXIT("std::_Throw_Cpp_error(%d)\n", err);
}

// Gen5 libc_v1 _ZSt9terminatev — NID qYhnoevd9bI.
static KYTY_SYSV_ABI void c_terminate()
{
	EXIT("std::terminate called\n");
}

// Gen5 libc_v1 sceLibcBacktraceGetBufferSize — NID sMko2YZqDNQ. IL2CPP's
// managed StackTrace path asks libc how large a buffer sceLibcBacktraceSelf
// needs for a given frame count; one return-address slot per frame.
static KYTY_SYSV_ABI size_t c_sce_libc_backtrace_get_buffer_size(size_t frame_count)
{
	return frame_count * sizeof(void*);
}

struct BacktraceSelfCtx
{
	void** buffer;
	size_t capacity;
	size_t count;
};

static _Unwind_Reason_Code BacktraceSelfCallback(_Unwind_Context* context, void* arg)
{
	auto* ctx = static_cast<BacktraceSelfCtx*>(arg);
	if (ctx->count >= ctx->capacity)
	{
		return _URC_END_OF_STACK;
	}
	ctx->buffer[ctx->count++] = reinterpret_cast<void*>(_Unwind_GetIP(context));
	return _URC_NO_REASON;
}

// Gen5 libc_v1 sceLibcBacktraceSelf — NID MTnuKt7HiN0. Captures a return-address
// backtrace into the guest buffer. The host _Unwind_Backtrace walks real frames,
// including guest frames published via __register_frame, so IPs land in guest
// code where the managed stack expects them.
static KYTY_SYSV_ABI size_t c_sce_libc_backtrace_self(void* buffer, size_t capacity)
{
	if (buffer == nullptr || capacity == 0)
	{
		return 0;
	}
	BacktraceSelfCtx ctx {static_cast<void**>(buffer), capacity, 0};
	_Unwind_Backtrace(BacktraceSelfCallback, &ctx);
	return ctx.count;
}

} // namespace LibC


LIB_USING(LibC);

LIB_DEFINE(InitLibC_1)
{
	// Re-enabled for the macOS/Rosetta bring-up: HLE-ing the "libc" module lets the
	// eboot run against native implementations instead of executing the game's real
	// libc.prx, whose init hits Rosetta segment/TSD and host/guest-pointer issues.
	LibcInternal::InitLibcInternal_1(s);

	LIB_OBJECT("P330P3dFF68", &LibC::g_need_flag);
	// stdin Object triad: same NIDs as InitLibcInternal_1 (see comment there).
	LIB_OBJECT("1TDo-ImqkJc", Kernel::FileSystem::StandardStream(0));
	LIB_OBJECT("2sWzhYqFH4E", Kernel::FileSystem::StandardStream(1));
	LIB_OBJECT("H8AprKeZtNg", Kernel::FileSystem::StandardStream(2));

	LIB_FUNC("-hn1tcVHq5Q", LibcInternal::LibcMspaceCreate);
	LIB_FUNC("OJjm-QOIHlI", LibcInternal::LibcMspaceMalloc);
	LIB_FUNC("iF1iQHzxBJU", LibcInternal::LibcMspaceMemalign);
	LIB_FUNC("fEoW6BJsPt4", LibcInternal::LibcMspaceMallocUsableSize);
	LIB_FUNC("LYo3GhIlB38", LibcInternal::LibcMspaceCalloc);
	LIB_FUNC("Vla-Z+eXlxo", LibcInternal::LibcMspaceFree);
	LIB_FUNC("k04jLXu3+Ic", LibcInternal::LibcMspaceMallocStatsFast);
	LIB_FUNC("mfHdJTIvhuo", LibcInternal::LibcMspaceMallocStats);
	LIB_FUNC("W6SiVSiCDtI", LibcInternal::LibcMspaceDestroy);
	LIB_FUNC("gigoVHZvVPE", LibcInternal::LibcMspaceRealloc);
	LIB_FUNC("p6lrRW8-MLY", LibcInternal::LibcMspaceReallocalign);
	LIB_FUNC("ljkqMcC4-mk", LibcInternal::LibcMspaceAlignedAlloc);
	LIB_FUNC("qWESlyXMI3E", LibcInternal::LibcMspacePosixMemalign);

	// ISO C entry points (LibCStandard.cpp).
	LIB_FUNC("-kU6bB4M-+k", LibC::c_strspn);
	LIB_FUNC("q0F6yS-rCms", LibC::c_strcspn);
	LIB_FUNC("gjbmYpP-XJQ", LibC::c_strcoll);
	LIB_FUNC("DQbtGaBKlaw", LibC::c_strnlen_s);
	LIB_FUNC("Ezzq78ZgHPs", LibC::c_wcschr);
	LIB_FUNC("g3ShSirD50I", LibC::c_wcsrchr);
	LIB_FUNC("KZm8HUIX2Rw", LibC::c_wcscat);
	LIB_FUNC("6f5f-qx4ucA", LibC::c_wcscpy_s);
	LIB_FUNC("Ye20uNnlglA", LibC::c_abs);
	LIB_FUNC("2gbcltk3swE", LibC::c_div);
	LIB_FUNC("dnaeGXbjP6E", LibC::c_exp2);
	LIB_FUNC("1t1-JoZ0sZQ", LibC::c_sinhf);
	LIB_FUNC("RCQAffkEh9A", LibC::c_coshf);
	LIB_FUNC("SAd0Z3wKwLA", LibC::c_tanhf);
	LIB_FUNC("yPPtp1RMihw", LibC::c_asinhf);
	LIB_FUNC("XJp2C-b0tRU", LibC::c_acoshf);
	LIB_FUNC("cPGyc5FGjy0", LibC::c_atanhf);
	LIB_FUNC("GlelR9EEeck", LibC::c_cbrtf);
	LIB_FUNC("RpTR+VY15ss", LibC::c_fmaf);
	LIB_FUNC("VOKOgR7L-2Y", LibC::c_lrint);
	LIB_FUNC("rcVv5ivMhY0", LibC::c_lrintf);
	LIB_FUNC("zck+6bVj5pA", LibC::c_nan);
	LIB_FUNC("DZU+K1wozGI", LibC::c_nanf);
	LIB_FUNC("0hlfW1O4Aa4", LibC::c_localeconv);
	LIB_FUNC("7SXNu+0KBYQ", LibC::c_wcstof);
	LIB_FUNC("7-a7sBHeUQ8", LibC::c_wcstod);
	LIB_FUNC("d3dMyWORw8A", LibC::c_wcstol);
	LIB_FUNC("5AYcEn7aoro", LibC::c_wcstoul);
	LIB_FUNC("34nH7v2xvNQ", LibC::c_wcstoll);
	LIB_FUNC("DAbZ-Vfu6lQ", LibC::c_wcstoull);
	LIB_FUNC("7Jp3g-qTgZw", LibC::c_scalbln);
	LIB_FUNC("9fs1btfLoUs", LibC::c_scalbnf);
	LIB_FUNC("MU25eqxSDTw", LibC::c_Sinh);
	LIB_FUNC("KeOZ19X8-Ug", LibC::c_Cosh);
	LIB_FUNC("O4L+0oCN9zA", LibC::c_FSinh);
	LIB_FUNC("PdnFCFqKGqA", LibC::c_FCosh);

	// C++ locale / RTTI objects (Qoo175Ig+-k → classic locale).
	// dynlib: _ZSt21_sceLibcClassicLocale, ctype<char>::id, locale::id::_Id_cnt,
	// __class_type_info / __si_class_type_info / __vmi_class_type_info vtables.
	LIB_OBJECT_ALIASES(&LibC::g_sce_classic_locale, "Qoo175Ig+-k", "Mcrl2crhxu0");
	LIB_OBJECT_ALIASES(&LibC::g_ctype_char_id, "Cv+zC4EjGMA", "MmytiDdoGBA");
	LIB_OBJECT_ALIASES(&LibC::g_locale_id_cnt, "H4fcpQOpc08", "EIyErVBW9QI");
	LIB_OBJECT("3ZotGOwzi9k", &LibC::g_lazy_locale_facet_id_0);
	LIB_OBJECT("5+FD4VpX+nw", &LibC::g_lazy_locale_facet_id_1);
	LIB_OBJECT("Jk7KrKMQzDw", &LibC::g_versioned_cerr);
	LIB_OBJECT("tIpEi4OinAI", &LibC::g_versioned_clog);
	LIB_OBJECT("9Qe7XFSQ4Lk", &LibC::g_versioned_cout);
	LIB_OBJECT("TVfbf1sXt0A", &LibC::g_classic_cerr);
	LIB_OBJECT("5PfqUBaQf4g", &LibC::g_classic_cout);
	// Facet ::id Objects — eboot imports (Ps5Nid / ps5_names).
	LIB_OBJECT("VmqsS6auJzo", &LibC::g_ctype_wchar_id);
	LIB_OBJECT("irGo1yaJ-vM", &LibC::g_collate_wchar_id);
	LIB_OBJECT("E14mW8pVpoE", &LibC::g_num_put_char_id);
	LIB_OBJECT("7brRfHVVAlI", &LibC::g_collate_char_id);
	LIB_OBJECT("9iXtwvGVFRI", &LibC::g_numpunct_char_id);
	LIB_OBJECT("-mLzBSk-VGs", &LibC::g_num_get_char_id);
	LIB_OBJECT("a54t8+k7KpY", &LibC::g_time_get_char_id);
	LIB_OBJECT("BamOsNbUcn4", &LibC::g_time_put_char_id);
	LIB_OBJECT("FjZCPmK0SbA", &LibC::g_codecvt_wchar_id);
	LIB_OBJECT("u2MAta5SS84", &LibC::g_codecvt_char32_id);
	// std::codecvt<char, char, mbstate_t>::id — eVFYZnYNDo0.
	LIB_OBJECT("eVFYZnYNDo0", &LibC::g_codecvt_char_id);
	// std::codecvt<char, char, mbstate_t> vtable — aK1Ymf-NhAs.
	LIB_OBJECT("aK1Ymf-NhAs", LibC::g_codecvt_char_vtable);
	// _Inf — HIhqigNaOns.
	LIB_OBJECT("HIhqigNaOns", &LibC::g_positive_infinity);
	LIB_OBJECT("byV+FWlAnB4", LibC::g_class_type_info_vtable);
	LIB_OBJECT("pZ9WXcClPO8", LibC::g_si_class_type_info_vtable);
	LIB_OBJECT("9ByRMdo7ywg", LibC::g_vmi_class_type_info_vtable);
	LIB_OBJECT("aeHxLWwq0gQ", LibC::g_pointer_type_info_vtable);
	LIB_OBJECT("2H51caHZU0Y", LibC::g_pointer_to_member_type_info_vtable);
	LIB_OBJECT("CSEjkTYt5dw", LibC::g_function_type_info_vtable);
	LIB_OBJECT("dCzeFfg9WWI", LibC::g_exception_vtable);
	// Exception / iostream RTTI Objects — eboot libc_v1 imports.
	// Prefer typed type_info/vtable Objects over generic locale-id/dummy placeholders
	// for the same NIDs (unit-tested domain_error layout).
	LIB_OBJECT("5BIbzIuDxTQ", &LibC::g_typeinfo_domain_error);
	LIB_OBJECT("n2kx+OmFUis", &LibC::g_typeinfo_exception);
	LIB_OBJECT("dKjhNUf9FBc", &LibC::g_typeinfo_out_of_range);
	LIB_OBJECT("bLPn1gfqSW8", &LibC::g_typeinfo_runtime_error);
	LIB_OBJECT("XZzWt0ygWdw", &LibC::g_typeinfo_invalid_argument);
	LIB_OBJECT("cxqzgvGm1GI", &LibC::g_typeinfo_length_error);
	LIB_OBJECT("C0IYaaVSC1w", &LibC::g_typeinfo_range_error);
	LIB_OBJECT("lt0mLhNwjs0", &LibC::g_typeinfo_overflow_error);
	LIB_OBJECT("oNRAB0Zs2+0", &LibC::g_typeinfo_underflow_error);
	LIB_OBJECT("DCY9coLQcVI", &LibC::g_typeinfo_future_error);
	LIB_OBJECT("qOD-ksTkE08", &LibC::g_typeinfo_bad_cast);
	LIB_OBJECT("BJCgW9-OxLA", &LibC::g_typeinfo_ios_base);
	LIB_OBJECT("sBCTjFk7Gi4", &LibC::g_typeinfo_ios_failure);
	LIB_OBJECT("RYlvfQvnOzo", &LibC::g_typeinfo_num_put_char);
	LIB_OBJECT("33t+tvosxCI", &LibC::g_typeinfo_time_put_char);
	LIB_OBJECT("oAidKrxuUv0", LibC::g_domain_error_vtable);
	LIB_OBJECT("udTM6Nxx-Ng", LibC::g_logic_error_vtable);
	LIB_OBJECT("n+aUKkC-3sI", LibC::g_out_of_range_vtable);
	LIB_OBJECT("-L+-8F0+gBc", LibC::g_runtime_error_vtable);
	LIB_OBJECT("keXoyW-rV-0", LibC::g_invalid_argument_vtable);
	LIB_OBJECT("cqvea9uWpvQ", LibC::g_length_error_vtable);
	LIB_OBJECT("Bq8m04PN1zw", LibC::g_system_error_vtable);
	LIB_OBJECT("CRoMIoZkYhU", LibC::g_thread_pad_vtable);
	LIB_OBJECT("QQsnQ2bWkdM", &LibC::g_typeinfo_thread_pad);
	LIB_OBJECT("qR6GVq1IplU", LibC::g_ti_name_thread_pad);
	LIB_OBJECT_ALIASES(LibC::g_bad_cast_vtable, "tVHE+C8vGXk", "CvgG53ICQZ8");
	LIB_OBJECT("EMNG6cHitlQ", LibC::g_bad_alloc_vtable);
	LIB_OBJECT("Z+vcX3rnECg", LibC::g_bad_array_new_length_vtable);
	LIB_OBJECT("DwH3gdbYfZo", &LibC::g_typeinfo_bad_alloc);
	LIB_OBJECT("lbLEAN+Y9iI", &LibC::g_typeinfo_bad_array_new_length);
	LIB_OBJECT("22g2xONdXV4", LibC::g_ti_name_bad_alloc);
	LIB_OBJECT("hBvqSQD5yNk", LibC::g_ti_name_bad_array_new_length);
	LIB_FUNC_ALIASES(LibC::c_bad_alloc_dtor, "WiH8rbVv5s4", "khbdMADH4cQ");
	LIB_FUNC("qb6A7pSgAeY", LibC::c_bad_alloc_deleting_dtor);
	LIB_FUNC("xvRvFtnUk3E", LibC::c_bad_alloc_what);
	LIB_FUNC("pS-t9AJblSM", LibC::c_bad_alloc_doraise);
	LIB_FUNC_ALIASES(LibC::c_bad_array_new_length_dtor, "15lB7flw-9w", "XO3N4SBvCy0");
	LIB_FUNC("-UKRka-33sM", LibC::c_bad_array_new_length_deleting_dtor);
	LIB_FUNC_ALIASES(LibC::c_bad_cast_dtor, "rF07weLXJu8", "47RvLSo2HN8");
	LIB_FUNC("2MK5Lr9pgQc", LibC::c_bad_cast_deleting_dtor);
	LIB_FUNC("6CPwoi-cFZM", LibC::c_bad_cast_what);
	LIB_FUNC("NEemVJeMwd0", LibC::c_bad_cast_doraise);
	LIB_OBJECT("6-LMlTS1nno", LibC::g_future_error_vtable);
	LIB_OBJECT("AJsqpbcCiwY", LibC::g_ios_base_vtable);
	LIB_OBJECT("yLE5H3058Ao", LibC::g_ios_failure_vtable);
	LIB_OBJECT("1kZFcktOm+s", LibC::g_num_put_char_vtable);
	// Fundamental RTTI objects.
	// Fundamental type_info objects and C++ runtime vtables, keyed by mangled name.
	for (const auto& rtti: LibC::CxxRttiObjects())
	{
		LIB_OBJECT(Loader::EncodeNameAsNid(rtti.symbol), rtti.object);
	}
	LIB_OBJECT("KfcTPbeaOqg", LibC::g_num_get_char_vtable); // _ZTV std::num_get<char>
	LIB_OBJECT("OwfBD-2nhJQ", LibC::g_time_put_char_vtable);
	LIB_OBJECT("FQ9NFbBHb5Y", &LibC::g_bad_off);
	LIB_OBJECT("wiR+rIcbnlc", LibC::g_fpz);
	LIB_OBJECT("b-xTWRgI1qw", &LibC::g_dtest_table);
	LIB_FUNC("NU-T4QowTNA", LibC::c_Xinvalid_argument); // std::_Xinvalid_argument
	LIB_FUNC("DbEnA+MnVIw", LibC::c_Towctrans);
	// Captured Gen5 UTF-16 string assignment: dst, src, code-unit count.
	LIB_FUNC("fL3O02ypZFE", LibC::c_wmemcpy16);
	// Captured Gen5 UTF-16 compare: lhs, rhs, code-unit count.
	LIB_FUNC("QJ5xVfKkni0", LibC::c_wmemcmp16);
	// Captured Gen5 locale setup: no args, returns a Locimp-like object.
	LIB_FUNC("9rMML086SEE", LibC::c_locale_CreateClassicLocimp);
	// std::locale::classic() returns the process-wide classic locale object.
	LIB_FUNC("Uq5K8tl8I9U", LibC::c_locale_classic);
	LIB_FUNC("QxqK-IdpumU", LibC::c_Getpmbstate);
	LIB_FUNC("zS94yyJRSUs", LibC::c_Getpwcstate);
	LIB_FUNC("U52BlHBvYvE", LibC::c_Getmbcurmax);
	LIB_FUNC("sZLrjx-yEx4", LibC::c_wcstombs_s);
	LIB_FUNC("-9SIhUr4Iuo", LibC::c_Mbtowcx);
	LIB_FUNC("stv1S3BKfgw", LibC::c_Wctombx);
	LIB_OBJECT("2wz4rthdiy8", &LibC::g_dummy_obj_17);
	LIB_FUNC("UWyL6KoR96U", LibC::c_Xregex_error);
	LIB_FUNC("bRujIheWlB0", LibC::c_Throw_C_error);
	LIB_FUNC("3PxvyV7qPPQ", LibC::c_error_exception_what);
	LIB_OBJECT("HUbZmOnT-Dg", &LibC::g_dummy_obj_21);
	LIB_OBJECT("Y6Sl4Xw7gfA", &LibC::g_dummy_obj_23);
	LIB_OBJECT("apPZ6HKZWaQ", &LibC::g_dummy_obj_24);
	LIB_OBJECT("BgZcGDh7o9g", &LibC::g_dummy_obj_25);
	LIB_FUNC_ALIASES(LibC::c_Lock_shared_ptr_spin_lock, "fRWufXAccuI", "XHKkoveq-CI");
	LIB_FUNC_ALIASES(LibC::c_Unlock_shared_ptr_spin_lock, "1HYEoANqZ1w", "ySAwp2f9rqM");
	LIB_FUNC("kALvdgEv5ME", LibC::c_Locksyslock);
	LIB_FUNC("9nf8joUTSaQ", LibC::c_Unlocksyslock);
	LIB_FUNC("hEQ2Yi4PJXA", LibC::c_locale_Getgloballocale);
	LIB_FUNC("hqi8yMOCmG0", LibC::c_locale_InitTemporaryInfo);
	LIB_FUNC("p6LrHjIQMdk", LibC::c_locale_DestroyTemporaryInfo);
	LIB_FUNC("QW2jL1J5rwY", LibC::c_locale_RegisterFacet);
	LIB_FUNC("zr094EQ39Ww", LibC::c_cxa_pure_virtual);
	LIB_FUNC("tyHd3P7oDrU", LibC::c_exception_doraise);
	// std::uncaught_exception — Q1BL70XVV0o after classic-locale probe.
	LIB_FUNC("Q1BL70XVV0o", LibC::c_uncaught_exception);
	// __gxx_personality_v0 — XwLA5cTHjt4 (PS5 export-name catalog).
	LIB_FUNC("XwLA5cTHjt4", LibC::c_gxx_personality_v0);
	// std::ios_base::~ios_base — P8F2oavZXtY after interactive presents start.
	LIB_FUNC("P8F2oavZXtY", LibC::c_ios_base_dtor);
	// std::ios_base::failure::~failure() [complete object] — N2f485TmJms.
	LIB_FUNC("N2f485TmJms", LibC::c_ios_base_failure_dtor);
	LIB_FUNC("YxwfcCH5Q0I", LibC::c_generic_category);
	LIB_FUNC("aotaAaQK6yc", LibC::c_system_category);
	LIB_FUNC("vI85k3GQcz8", LibC::c_future_category);
	LIB_FUNC("g8Jw7V6mn8k", LibC::c_error_category_dtor);
	LIB_FUNC("3qWXO9GTUYU", LibC::c_system_error_dtor);
	LIB_FUNC("8SDojuZyQaY", LibC::c_error_category_default_error_condition);
	LIB_FUNC("GthClwqQAZs", LibC::c_error_category_equivalent_condition);
	LIB_FUNC("9hB8AwIqQfs", LibC::c_error_category_equivalent_code);
	LIB_OBJECT("cbvW20xPgyc", &LibC::g_typeinfo_error_category);
	LIB_FUNC("Cj+Fw5q1tUo", LibC::c_xtime_get_ticks);

	LIB_FUNC("uMei1W9uyNo", LibC::exit);
	LIB_FUNC("bzQExy189ZI", LibC::init_env);
	LIB_FUNC("8G2LB+A3rzg", LibC::atexit);
	LIB_FUNC("hcuQgD53UxM", LibC::libc_printf);
	LIB_FUNC("MUjC4lbHrK4", LibcInternal::fflush);
	LIB_FUNC("YQ0navp+YIc", LibC::puts);
	// Gen5 putchar — NID m5wN+SwZOR4.
	LIB_FUNC("m5wN+SwZOR4", LibC::c_putchar);
	// Captured Gen5 after DirNameSearch/strtol: rdi=formatted log line
	// with trailing CR/LF, rsi=stream-like pointer — fputs ABI.
	LIB_FUNC("QrZZdJ8XsX0", LibC::c_fputs);
	LIB_FUNC("XKRegsFpEpk", LibC::catchReturnFromMain);
	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);
	LIB_FUNC("DiGVep5yB5w", LibC::c_execute_once);
	LIB_FUNC("YaHc3GS7y7g", LibC::c_mtx_init);
	LIB_FUNC("tgioGpKtmbE", LibC::c_mtx_init_with_name);
	LIB_FUNC("JHp7ogc1+HY", LibC::c_mtx_init_with_default_name_override);
	LIB_FUNC("5Lf51jvohTQ", LibC::c_mtx_destroy);
	LIB_FUNC("iS4aWbUonl0", LibC::c_mtx_lock);
	LIB_FUNC("k6pGNMwJB08", LibC::c_mtx_trylock);
	LIB_FUNC("hPzYSd5Nasc", LibC::c_mtx_timedlock);
	LIB_FUNC("gTuXQwP9rrs", LibC::c_mtx_unlock);
	LIB_FUNC("VYQwFs4CC4Y", LibC::c_mtx_current_owns);

	// Standard C allocation uses the guest application heap after it is ready.
	LIB_FUNC("gQX+4GDQjpM", LibC::c_malloc);
	LIB_FUNC("g7zzzLDYGw0", LibC::c_strdup);
	LIB_FUNC("2X5agFjKxMc", LibC::c_calloc);
	LIB_FUNC("smbQukfxYJM", LibC::c_getenv);
	LIB_FUNC("PtsB1Q9wsFA", LibC::c_setlocale);
	LIB_FUNC("802pFCwC9w0", LibC::c_udivti3);
	LIB_FUNC("Y7aJ1uydPMo", LibC::c_realloc);
	LIB_FUNC("tIhsqj0qsFE", LibC::c_free);
	LIB_FUNC("Ujf3KzMvRmI", LibC::c_memalign);
	LIB_FUNC("OGybVuPAhAY", LibC::c_reallocalign);
	LIB_FUNC("2Btkg8k24Zg", LibC::c_aligned_alloc);
	LIB_FUNC("cVSk9y8URbc", LibC::c_posix_memalign);
	LIB_FUNC("KuOuD58hqn4", LibcInternal::LibcMallocStatsFast);
	LIB_FUNC("SreZybSRWpU", LibC::c_cnd_init);
	LIB_FUNC("2B+V3qCqz4s", LibC::c_cnd_init_with_name);
	LIB_FUNC("jBOZAv6CwkM", LibC::c_cnd_init_with_default_name_override);
	LIB_FUNC("VsP3daJgmVA", LibC::c_cnd_broadcast);
	LIB_FUNC("7yMFgcS8EPA", LibC::c_cnd_destroy);
	LIB_FUNC("0uuqgRz9qfo", LibC::c_cnd_signal);
	LIB_FUNC("McaImWKXong", LibC::c_cnd_timedwait);
	LIB_FUNC("vEaqE-7IZYc", LibC::c_cnd_wait);
	LIB_FUNC("7Xl257M4VNI", LibC::c_pthread_equal);
	LIB_FUNC("mqQMh1zPPT8", LibC::c_fstat);
	LIB_FUNC("Q3VBxCXhUHs", LibC::c_memcpy);
	// Gen5 second memcpy NID — C++ std::string SSO short-assign path
	// after __cxa_dynamic_cast (Construct Action setup): (dst, src, n=1..).
	LIB_FUNC("Noj9PsJrsa8", LibC::c_memcpy);
	LIB_FUNC("NFLs+dRJGNg", LibC::c_memcpy_s);
	// Gen5 libc_v1 memmove_s — B59+zQQCcbU after TLS factory / strtoull.
	LIB_FUNC("B59+zQQCcbU", LibC::c_memmove_s);
	LIB_FUNC("8zTFvBIAIN8", LibC::c_memset);
	LIB_FUNC("h8GwqPFbu6I", LibC::c_memset_s);
	LIB_FUNC("DfivPArhucg", LibC::c_memcmp);
	LIB_FUNC("j4ViWNHEgww", LibC::c_strlen);
	LIB_FUNC("5jNubw4vlAA", LibC::c_strnlen);
	LIB_FUNC("WkkeywLJcgU", LibC::c_wcslen);
	LIB_FUNC("0nV21JjYCH8", LibC::c_wcsncpy);
	LIB_FUNC("CyXs2l-1kNA", LibC::c_Iswctype);
	LIB_FUNC("6sJWiWSRuqk", LibC::c_strncpy);
	// Captured Gen5 boot after SaveDataInitialize3: 3-arg call with dest buffer,
	// "SAVEDATA00" src, n=0x20 — same ABI as strncpy (second NID for same export).
	LIB_FUNC("SfQIZcqvvms", LibC::c_strncpy);
	LIB_FUNC("Ovb2dSJOAuE", LibC::c_strcmp);
	LIB_FUNC("aesyjrHVWy4", LibC::c_strncmp);
	// sceLibc strcasecmp — NID AV6ipCNa4Rw
	LIB_FUNC("AV6ipCNa4Rw", LibC::c_strcasecmp);
	LIB_FUNC("pXvbDfchu6k", LibC::c_strncasecmp);
	LIB_FUNC("Ls4tzzhimqQ", LibC::c_strcat);
	LIB_FUNC("kHg45qPC6f0", LibC::c_strncat);
	LIB_FUNC("kDZvoVssCgQ", LibC::c_strpbrk);
	LIB_FUNC("ob5xAW4ln-0", LibC::c_strchr);
	LIB_FUNC("9yDWMxEFdJU", LibC::c_strrchr);
	// Gen5 libc_v1 strstr.
	LIB_FUNC("viiwFMaNamA", LibC::c_strstr);
	LIB_FUNC("Xnrfb2-WhVw", LibC::c_strnstr);
	LIB_FUNC("WDpobjImAb4", LibC::c_wcsstr);
	LIB_FUNC("E8wCoUEbfzk", LibC::c_wcsncmp);
	LIB_FUNC("fJnpuVVBbKk", LibC::cxx_new);         // operator new(size_t)
	LIB_FUNC("ryUxD-60bKM", LibC::cxx_new_nothrow); // operator new(size_t, const std::nothrow_t&)
	LIB_OBJECT("NLwJ3q+64bY", &LibC::g_cxx_nothrow); // std::nothrow
	// Gen5 libc_v1 — public NID cfAXurvfl5o is __cxa_allocate_exception (not
	// operator new). Mis-binding it to cxx_new corrupts the throw path.
	LIB_FUNC("cfAXurvfl5o", LibC::cxa_allocate_exception);
	LIB_FUNC("MQFPAqQPt1s", LibC::cxa_decrement_exception_refcount);
	LIB_FUNC("z+P+xCnWLBk", LibC::cxx_delete);               // operator delete(void*)
	LIB_FUNC("lYDzBVE5mZs", LibC::cxx_delete_sized);         // operator delete(void*, size_t)
	LIB_FUNC("nwujzxOPXzQ", LibC::cxx_delete_sized_aligned); // operator delete(void*, size_t, align_val_t)
	// Gen5 libc_v1 C++ EH — guest throw path after initialization.
	// vkuuLfhnSZI: __cxa_throw (rdi=obj, rsi=typeinfo, rdx=dtor; ud2 after).
	LIB_FUNC("vkuuLfhnSZI", LibC::cxa_throw);
	// Itanium catch/unwind lifecycle — forwarded to the host runtime, which
	// unwinds guest frames via the registered .eh_frame.
	LIB_FUNC("3cUUypQzMiI", LibC::cxa_begin_catch);                    // __cxa_begin_catch
	LIB_FUNC("lX+4FNUklF0", LibC::cxa_end_catch);                      // __cxa_end_catch
	LIB_FUNC("ZL9FV4mJXxo", LibC::cxa_rethrow);                        // __cxa_rethrow
	LIB_FUNC("RY8mQlhg7mI", LibC::cxa_current_primary_exception);      // __cxa_current_primary_exception
	LIB_FUNC("qKQiNX91IGo", LibC::cxa_rethrow_primary_exception);      // __cxa_rethrow_primary_exception
	LIB_FUNC("PsrRUg671K0", LibC::cxa_increment_exception_refcount);   // __cxa_increment_exception_refcount
	LIB_FUNC("nOIEswYD4Ig", LibC::cxa_free_exception);                 // __cxa_free_exception
	LIB_FUNC("f1zwJ3jAI2k", LibC::c_unwind_resume);                    // _Unwind_Resume
	LIB_FUNC("wpNJwmDDtxw", LibC::c_unwind_raise_exception);           // _Unwind_RaiseException
	LIB_FUNC("yK3VESxclT0", LibC::c_unwind_raise_exception);           // __libunwind_Unwind_RaiseException
	LIB_FUNC("Gbr7tQZ4A1A", LibC::c_unwind_resume);                    // __libunwind_Unwind_Resume
	LIB_FUNC("xUsJSLsdv9I", LibC::c_unwind_resume_or_rethrow);         // _Unwind_Resume_or_Rethrow
	LIB_FUNC("rsqqpUwcsQY", LibC::c_unwind_resume_or_rethrow);         // __libunwind_Unwind_Resume_or_Rethrow
	LIB_FUNC("GslDM6l8E7U", LibC::c_unwind_delete_exception);          // _Unwind_DeleteException
	LIB_FUNC("H0NwmJX8SOA", LibC::c_unwind_delete_exception);          // __libunwind_Unwind_DeleteException
	LIB_FUNC("MTnuKt7HiN0", LibC::c_sce_libc_backtrace_self);          // sceLibcBacktraceSelf
	LIB_FUNC("hdm0YfMa7TQ", LibC::cxx_new_array);         // operator new[](size_t)
	LIB_FUNC("Jh5qUcwiSEk", LibC::cxx_new_array_nothrow); // operator new[](size_t, const std::nothrow_t&)
	LIB_FUNC("MLWl90SFWNE", LibC::cxx_delete_array);      // operator delete[](void*)
	LIB_FUNC("FOt55ZNaVJk", LibC::cxx_delete_array_sized); // operator delete[](void*, size_t)
	LIB_FUNC("cjZEuzHkgng", LibC::c_atomic_load_4);
	LIB_FUNC("0AgCOypbQ90", LibC::c_atomic_compare_exchange_weak_4);
	LIB_FUNC("iPBqs+YUUFw", LibC::c_atomic_fetch_add_4);
	LIB_FUNC("2HnmKiLmV6s", LibC::c_atomic_fetch_sub_4);
	LIB_FUNC("np6xXcXEnXE", Kernel::PthreadGetthreadid);
	LIB_FUNC("CHrhwd8QSBs", LibC::c_thread_hardware_concurrency);
	LIB_FUNC("YvmY5Jf0VYU", LibC::c_thread_join);
	LIB_FUNC("exNzzCAQuWM", LibC::c_thread_yield);
	LIB_FUNC("dGYo9mE8K2A", LibC::c_thread_pad_ctor);
	LIB_FUNC("uhnb6dnXOnc", LibC::c_thread_pad_named_ctor);
	LIB_FUNC("gjLRZgfb3i0", LibC::c_thread_pad_dtor);
	LIB_FUNC("XyJPhPqpzMw", LibC::c_thread_pad_dtor);
	LIB_FUNC("xZqiZvmcp9k",
	         static_cast<void(KYTY_SYSV_ABI*)(LibC::CxxThreadPad*, Kernel::Pthread*)>(LibC::c_thread_pad_launch));
	LIB_FUNC("PBbZjsL6nfc", LibC::c_thread_pad_named_launch);
	LIB_FUNC("fLBZMOQh-3Y", LibC::c_thread_pad_attr_launch);
	LIB_FUNC("H7-7Z3ixv-w", LibC::c_thread_pad_named_attr_launch);
	LIB_FUNC("a-z7wxuYO2E", LibC::c_thread_pad_release);

	// string / memory
	LIB_FUNC("+P6FRGH4LfA", LibC::c_memmove);
	LIB_FUNC("8u8lPzUEq+U", LibC::c_memchr);
	LIB_FUNC("fnUEjBCNRVU", LibC::c_memchr);
	// Unique wide-mem HLE from bringup (NID does not collide with HEAD Objects).
	LIB_FUNC("Al8MZJh-4hM", LibC::c_wmemset);
	LIB_FUNC("5TjaJwkLWxE", LibC::c_bcmp);
	LIB_FUNC("kiZSXIWd9vg", LibC::c_strcpy);
	// Gen5 strcpy_s — NID 5Xa2ACNECdo (next hard-abort after thread stack reprotect).
	LIB_FUNC("5Xa2ACNECdo", LibC::c_strcpy_s);
	LIB_FUNC("RIa6GnWp+iU", LibC::c_strerror);
	LIB_FUNC("RBcs3uut1TA", LibC::c_strerror_r);
	LIB_FUNC("YNzNkJzYqEg", LibC::c_strncpy_s);

	// ctype
	LIB_FUNC("sUP1hBaouOw", LibC::c_Getpctype);
	LIB_FUNC("8xXiEPby8h8", LibC::c_Getptimes);
	LIB_FUNC("vU9svJtEnWc", LibC::c_setw);
	LIB_FUNC("1h8hFQghR7w", LibC::c_setprecision);
	LIB_FUNC("j9LU8GsuEGw", LibC::c_time_put_put);
	LIB_FUNC("rcQCUr0EaRU", LibC::c_Getptoupper);
	// Gen5 _Getptolower — guest VFS path lowercasing after ~INDEX.
	LIB_FUNC("1uJgoVq3bQU", LibC::c_Getptolower);

	// stdio
	LIB_FUNC("xeYO4u7uyJ0", LibC::c_fopen);
	// idc/ps4libdoc a71315e7f36e312ae71e9e3a92982e9ffbfc725f:
	// system/common/lib/libc.sprx.json, exported library libc version 1.
	LIB_FUNC("qdlHjTa9hQ4", LibC::c_fdopen);
	LIB_FUNC("gkWgn0p1AfU", LibC::c_freopen);
	LIB_FUNC("uodLYyUip20", LibC::c_fclose);
	LIB_FUNC("lbB+UlZqVG0", LibC::c_fread);
	// Gen5 fgets — NID KdP-nULpuGw.
	LIB_FUNC("KdP-nULpuGw", LibC::c_fgets);
	LIB_FUNC("MpxhMh8QFro", LibC::c_fwrite);
	LIB_FUNC("QMFyLoqNxIg", LibC::c_setvbuf);
	LIB_FUNC("rQFVBXp-Cxg", LibC::c_fseek);
	LIB_FUNC("Qazy8LmXTvw", LibC::c_ftell);
	LIB_FUNC("LxcEU+ICu8U", LibC::c_feof);
	LIB_FUNC("AHxyhN96dy4", LibC::c_ferror);
	LIB_FUNC("Fm-dmyywH9Q", LibC::c_fileno);
	LIB_FUNC("aZK8lNei-Qw", LibC::c_fputc);
	LIB_FUNC("MZO7FXyAPU8", LibC::c_remove);

	// printf / scanf family
	LIB_FUNC("eLdDw6l0-bU", LibC::c_snprintf);
	LIB_FUNC("3BytPOQgVKc", LibC::c_snprintf_s);
	LIB_FUNC("NC4MSB+BRQg", LibC::c_strncat_s);
	// Gen5 vsprintf_s — NID +qitMEbkSWk.
	LIB_FUNC("+qitMEbkSWk", LibC::c_vsprintf_s);
	LIB_FUNC("Q2V+iqvjgC0", LibC::c_vsnprintf); // vsnprintf (Gen5 libc_v1)
	LIB_FUNC("tcVi5SivF7Q", LibC::c_sprintf);
	// Gen5 sprintf_s — NID xEszJVGpybs.
	LIB_FUNC("xEszJVGpybs", LibC::c_sprintf_s);
	LIB_FUNC("fffwELXNVFA", LibC::c_fprintf);
	LIB_FUNC("pDBDcY6uLSA", LibC::c_vfprintf);
	LIB_FUNC("EMutwaQ34Jo", LibC::c_perror);
	LIB_FUNC("3QIPIh-GDjw", LibC::c_rewind);
	LIB_FUNC("AEuF3F2f8TA", LibC::c_fgetc);
	LIB_FUNC("8Q60JLJ6Rv4", LibC::c_getc);
	LIB_FUNC("pNtJdE3x49E", LibC::c_wcscmp);
	LIB_FUNC("1Pk0qZQGeWo", LibC::c_sscanf);
	LIB_FUNC("24m4Z4bUaoY", LibC::c_sscanf_s);
	LIB_FUNC("jbz9I9vkqkk", LibC::c_vsprintf);
	LIB_FUNC("rWSuTWY2JN0", LibC::c_vsnprintf_s);
	LIB_FUNC("u0XOsuOmOzc", LibC::c_vswprintf);

	// stdlib
	LIB_FUNC("2vDqwBlpF-o", LibC::c_strtod);
	// Gen5 libc_v1 strtof — xENtRue8dpI after APR stream wrap (levels.xml path).
	LIB_FUNC("xENtRue8dpI", LibC::c_strtof);
	LIB_FUNC("mXlxhmLNMPg", LibC::c_strtol);
	// Gen5 strtoul: Kyty maps QxmSHBCuKTk / zlfEH8FmyUA; a guest
	// Construct parser also hits VOBg+iNwB-4 (rdi=nptr, rsi=endptr, rdx=10).
	LIB_FUNC_ALIASES(LibC::c_strtoul, "QxmSHBCuKTk", "zlfEH8FmyUA"); // strtoul, _Stoul
	LIB_FUNC("VOBg+iNwB-4", LibC::c_strtoll);
	// Gen5 libc_v1 strtoull — 5OqszGpy7Mg after TLS context factory.
	LIB_FUNC("5OqszGpy7Mg", LibC::c_strtoull);
	LIB_FUNC("SRI6S9B+-a4", LibC::c_atof);
	LIB_FUNC("AEJdIVZTEmo", LibC::c_qsort);
	LIB_FUNC("NesIgTmfF0Q", LibC::c_bsearch);
	LIB_FUNC("L1SBTkC+Cvw", LibC::c_abort);
	LIB_FUNC("VPbJwTCgME0", LibC::c_srand);
	// drand48 family — 48-bit LCG PRNG (IL2CPP runtime init).
	LIB_FUNC("+KSnjvZ0NMc", LibC::c_srand48);
	LIB_FUNC("WIg11rA+MRY", LibC::c_drand48);
	LIB_FUNC("5IpoNfxu84U", LibC::c_lrand48);
	LIB_FUNC("k-l0Jth-Go8", LibC::c_mrand48);
	// IL2CPP libc_v1 imports — math/time/stdio delegations.
	LIB_FUNC("-VVn74ZyhEs", LibC::c_difftime);
	LIB_FUNC("owKuegZU4ew", LibC::c_logb);
	LIB_FUNC("KGKBeVcqJjc", LibC::c_scalbn);
	LIB_FUNC("WuMbPBKN1TU", LibC::c_log10);
	LIB_FUNC("Y5DhuDKGlnQ", LibC::c_log2);
	LIB_FUNC("-LFO7jhD5CE", LibC::c_ungetc);
	LIB_FUNC("SHlt7EhOtqA", LibC::c_fgetpos);
	LIB_FUNC("7PkSz+qnTto", LibC::c_fsetpos);
	LIB_FUNC("qdGFBoLVNKI", LibC::c_quick_exit);
	// IL2CPP libc_v1 imports — CRT/Dinkumware internals.
	LIB_FUNC("vZkmJmvqueY", LibC::c_Lockfilelock);
	LIB_FUNC("0x7rx8TKy2Y", LibC::c_Unlockfilelock);
	LIB_FUNC("vYWK2Pz8vGE", LibC::c_Fiopen);
	LIB_FUNC("MOBxtefPZUg", LibC::c_std_exception_dtor);
	LIB_FUNC("oe9tS0VztYk", LibC::c_std_runtime_error_dtor);
	LIB_FUNC("V23qt24VPVs", LibC::c_iostream_category);
	LIB_FUNC("eT2UsmTewbU", LibC::c_Xbad_alloc);
	LIB_FUNC("MELi-cKqWq0", LibC::c_Xbad_function_call);
	// Gen5 libc_v1 rand — Nmtr628eA3A observed early; cpCOXWMgha0 after Fiber/thread bring-up.
	LIB_FUNC_ALIASES(LibC::c_rand, "Nmtr628eA3A", "cpCOXWMgha0");
	LIB_FUNC("oVkZ8W8-Q8A", LibC::c_strtok);

	// time
	LIB_FUNC("wLlFkwG9UcQ", LibC::Time::c_time);
	LIB_FUNC("QZP6I9ZZxpE", LibC::c_clock);
	LIB_FUNC("n7AepwR0s34", LibC::Time::c_mktime);
	LIB_FUNC("1mecP7RgI2A", LibC::Time::c_gmtime);
	LIB_FUNC("5bBacGLyLOs", LibC::Time::c_gmtime_s);
	LIB_FUNC("efhK-YSUYYQ", LibC::Time::c_localtime);
	LIB_FUNC("fiiNDnNBKVY", LibC::Time::c_localtime_s);
	LIB_FUNC("Av3zjWi64Kw", LibC::Time::c_strftime);
	LIB_FUNC("XbVXpf5WF28", LibC::Time::c_wcsftime);
	LIB_FUNC("jT3xiGpA3B4", LibC::Time::c_asctime);

	// math (double)
	LIB_FUNC("H8ya2H00jbI", LibC::c_sin);
	LIB_FUNC("2WE3BTYVwKM", LibC::c_cos);
	LIB_FUNC("T7uyNqP7vQA", LibC::c_tan);
	LIB_FUNC("JM4EBvWT9rc", LibC::c_tanh);
	LIB_FUNC("7Ly52zaL44Q", LibC::c_asin);
	LIB_FUNC("JBcgYuW8lPU", LibC::c_acos);
	LIB_FUNC("OXmauLdQ8kY", LibC::c_atan);
	LIB_FUNC("HUbZmOnT-Dg", LibC::c_atan2);
	LIB_FUNC("NVadfnzQhHQ", LibC::c_exp);
	LIB_FUNC("rtV7-jWC6Yg", LibC::c_log);
	LIB_FUNC("9LCjpWyQ5Zc", LibC::c_pow);
	LIB_FUNC("H+8UBOwfScI", LibC::c_powidf2);
	LIB_FUNC("EiMkgQsOfU0", LibC::c_powisf2);
	LIB_FUNC("pKwslsMUmSk", LibC::c_fmod);
	// Gen5 libc_v1 double rounding and absolute-value exports. Float variants
	// are registered below.
	LIB_FUNC("gacfOmO8hNs", LibC::c_ceil);
	LIB_FUNC("mpcTgMzhUY8", LibC::c_floor);
	LIB_FUNC("nlaojL9hDtA", LibC::c_round);
	LIB_FUNC("MXRNWnosNlM", LibC::c_sqrt);
	LIB_FUNC("388LcMWHRCA", LibC::c_fabs);
	LIB_FUNC("YFoOw5GkkK0", LibC::c_hypot);
	LIB_FUNC("0WMHDb5Dt94", LibC::c_modf);
	LIB_FUNC("JrwFIMzKNr0", LibC::c_ldexp);
	LIB_FUNC("kA-TdiOCsaY", LibC::c_frexp);
	LIB_FUNC("jMB7EFyu30Y", LibC::c_sincos);
	// math (float)
	LIB_FUNC("1D0H2KNjshE", LibC::c_powf);
	// Gen5 libc_v1 __isnanf — lA94ZgT+vMM after Posix pthread_self.
	LIB_FUNC("lA94ZgT+vMM", LibC::c_isnanf);
	// Gen5 libc_v1 __isfinitef — name-to-NID hash and float predicate ABI.
	LIB_FUNC("Q8pvJimUWis", LibC::c_isfinitef);
	LIB_FUNC("rDMyAf1Jhug", LibC::c_isinff);
	// Gen5 isfinite(double) — used after strtod in a project parse.
	LIB_FUNC("dhK16CKwhQg", LibC::c_isfinite);
	// Gen5 isnan(double) — guest layout coordinate checks after
	// vcvttsd2si; return 0 continues (non-zero rejects).
	LIB_FUNC("GfxAp9Xyiqs", LibC::c_isnan);
	// Gen5 libc_v1 __isinf — double in xmm0, integer predicate in eax.
	LIB_FUNC("V02oFv+-JzA", LibC::c_isinf);
	// Gen5 libc_v1 __signbit — standard double predicate ABI.
	LIB_FUNC("Rw4J-22tu1U", LibC::c_signbit);
	// Gen5 libc_v1 __fpclassifyd — translate host categories to guest values.
	LIB_FUNC("qlWiRfOJx1A", LibC::c_fpclassifyd);
	// Gen5 libc_v1 float math (NIDs from name→NID hash).
	LIB_FUNC("Q4rRL34CEeE", LibC::c_sinf);
	LIB_FUNC("-P6FNMzk2Kc", LibC::c_cosf);
	// Gen5 libc_v1 float math after Posix detach (name→NID; '/' stored as '-').
	LIB_FUNC("ZE6RNL+eLbk", LibC::c_tanf);
	LIB_FUNC("weDug8QD-lE", LibC::c_atanf);
	LIB_FUNC("88Vv-AzHVj8", LibC::c_fmodf);
	LIB_FUNC("GZWjF-YIFFk", LibC::c_asinf);
	LIB_FUNC("QI-x0SL8jhw", LibC::c_acosf);
	LIB_FUNC("EH-x713A99c", LibC::c_atan2f);
	LIB_FUNC("iz2shAGFIxc", LibC::c_hypotf);
	LIB_FUNC("Vo8rvWtZw3g", LibC::c_truncf);
	LIB_FUNC("DDHG1a6+3q0", LibC::c_roundf);
	LIB_FUNC("C6gWCWJKM+U", LibC::c_lroundf);
	// Gen5 libc_v1 float remainder/frexp variants (name→NID).
	LIB_FUNC("eS+MVq+Lltw", LibC::c_remainderf);
	LIB_FUNC("aaDMGGkXFxo", LibC::c_frexpf);
	// Gen5 libc_v1 wide-string and varargs exports.
	LIB_FUNC("FM5NPnLqBc8", LibC::c_wcscpy);
	LIB_FUNC("GMpvxPFW924", LibC::c_vprintf);
	LIB_FUNC("nJz16JE1txM", LibC::c_swprintf);
	// Gen5 libc_v1 thread detach + C++ runtime error paths.
	LIB_FUNC("L7f7zYwBvZA", LibC::c_thrd_detach);
	LIB_FUNC("jfRI3snge3o", LibC::c_thrd_sleep);
	LIB_FUNC("-QgqOT5u2Vk", LibC::c_assert);
	LIB_FUNC("W0j6vCxh9Pc", LibC::c_throw_cpp_error);
	LIB_FUNC("qYhnoevd9bI", LibC::c_terminate);
	// Gen5 libc_v1 sceLibcBacktraceGetBufferSize — sMko2YZqDNQ (IL2CPP stack-trace path).
	LIB_FUNC("sMko2YZqDNQ", LibC::c_sce_libc_backtrace_get_buffer_size);
	LIB_FUNC("lhpd6Wk6ccs", LibC::c_log10f); // next Unpatched after sinf
	LIB_FUNC("RQXLbdT2lc4", LibC::c_logf);
	LIB_FUNC("Q+xU11-h0xQ", LibC::c_sqrtf);
	LIB_FUNC("fmT2cjPoWBs", LibC::c_fabsf);
	LIB_FUNC("mKhVDmYciWA", LibC::c_floorf);
	LIB_FUNC("GAUuLKGhsCw", LibC::c_ceilf);
	LIB_FUNC("hsi9drzHR2k", LibC::c_log2f);
	LIB_FUNC("wuAQt-j+p4o", LibC::c_exp2f);
	LIB_FUNC("8zsu04XNsZ4", LibC::c_expf);
	LIB_FUNC("kn0yiYeExgA", LibC::c_ldexpf);
	// Gen5 libc_v1 modff — float modf; IL2CPP runtimes use it during init.
	LIB_FUNC("3+UPM-9E6xY", LibC::c_modff);
	LIB_FUNC("pztV4AF18iI", LibC::c_sincosf);

	// C++ runtime
	LIB_FUNC("3GPpjQdAMTw", LibC::c_cxa_guard_acquire);
	LIB_FUNC("9rAeANT2tyE", LibC::c_cxa_guard_release);
	LIB_FUNC("2emaaluWzUw", LibC::c_cxa_guard_abort);
	LIB_FUNC("BKSCW2bCACA", LibC::c_cxa_thread_atexit);
	LIB_FUNC("Z2tTVqGDPGQ", LibC::c_cxa_thread_atexit);
	// Gen5 __cxa_dynamic_cast — guest Construct ConditionOrAction→Action.
	LIB_FUNC("hMAe+TWS9mQ", LibC::cxa_dynamic_cast);
	LIB_FUNC("ozMAr28BwSY", LibC::c_Xout_of_range);
	LIB_FUNC("tQIo+GIPklo", LibC::c_Xlength_error);

	// setjmp / longjmp (bound directly to host — no C++ wrapper)
	#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	LIB_FUNC("gNQ1V2vfXDE", kyty_setjmp);
	LIB_FUNC("lKEN2IebgJ0", kyty_longjmp);
	#else
	LIB_FUNC("gNQ1V2vfXDE", _setjmp);
	LIB_FUNC("lKEN2IebgJ0", _longjmp);
	#endif
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
