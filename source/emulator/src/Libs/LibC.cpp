#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/LinkList.h"
#include "Kyty/Core/MSpace.h"
#include "Kyty/Core/Singleton.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/ApplicationHeap.h"
#include "Emulator/Libs/CxaDynamicCast.h"
#include "Emulator/Libs/CxxLocale.h"
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
#include <setjmp.h>
#include <string>
#include <strings.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

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

using cxa_destructor_func_t = void (*)(void*);

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

static KYTY_SYSV_ABI void exit(int code)
{
	PRINT_NAME();

	::exit(code);
}

static KYTY_SYSV_ABI void init_env(const ProcessEnvironment::InitParameters* parameters)
{
	PRINT_NAME();

	(void)ProcessEnvironment::Initialize(parameters);
}

// The C++ runtime uses _Cnd_t as a pointer to the kernel condition object.
// _Cnd_init receives the address of that handle and owns its allocation; use
// the same condition implementation as the public pthread ABI so both paths
// share lifetime and error handling.
int c_thread_sync_result(int result);
static LibKernel::KernelUseconds c_abstime_remaining_usec(const LibKernel::KernelTimespec* abstime);

KYTY_SYSV_ABI int c_cnd_init(LibKernel::PthreadCond* cond)
{
	return c_thread_sync_result(LibKernel::PthreadCondInit(cond, nullptr, nullptr));
}

KYTY_SYSV_ABI int c_cnd_init_with_name(LibKernel::PthreadCond* cond, const char* name)
{
	return c_thread_sync_result(LibKernel::PthreadCondInit(cond, nullptr, name));
}

KYTY_SYSV_ABI int c_cnd_init_with_default_name_override(LibKernel::PthreadCond* cond, const char* name)
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

KYTY_SYSV_ABI int c_cnd_broadcast(LibKernel::PthreadCond* cond)
{
	return c_thread_sync_result(LibKernel::PthreadCondBroadcast(cond));
}

KYTY_SYSV_ABI int c_cnd_signal(LibKernel::PthreadCond* cond)
{
	return c_thread_sync_result(LibKernel::PthreadCondSignal(cond));
}

KYTY_SYSV_ABI int c_cnd_wait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex)
{
	return c_thread_sync_result(LibKernel::PthreadCondWait(cond, mutex));
}

KYTY_SYSV_ABI int c_cnd_timedwait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex,
                                         const LibKernel::KernelTimespec* abstime)
{
	return c_thread_sync_result(LibKernel::PthreadCondTimedwaitAbsolute(cond, mutex, abstime));
}

KYTY_SYSV_ABI void c_cnd_destroy(LibKernel::PthreadCond* cond)
{
	if (cond == nullptr)
	{
		return;
	}

	auto* private_cond = *cond;
	if (private_cond != nullptr && reinterpret_cast<uintptr_t>(private_cond) >= 0x100000)
	{
		(void)LibKernel::PthreadCondDestroy(cond);
	}
}

static KYTY_SYSV_ABI int c_pthread_equal(LibKernel::Pthread thread1, LibKernel::Pthread thread2)
{
	return LibKernel::PthreadEqual(thread1, thread2);
}
static KYTY_SYSV_ABI int c_fstat(int fd, LibKernel::FileSystem::FileStat* sb)
{
	return LibKernel::FileSystem::KernelFstat(fd, sb);
}
static KYTY_SYSV_ABI int c_wcscmp(const wchar_t* s1, const wchar_t* s2)
{
	return ::wcscmp(s1, s2);
}
static KYTY_SYSV_ABI void c_perror(const char* s)
{
	::perror(s);
}
static KYTY_SYSV_ABI void c_rewind(FILE* f)
{
	if (f != nullptr)
	{
		::rewind(f);
	}
}
static KYTY_SYSV_ABI int c_fgetc(FILE* f)
{
	return (f != nullptr ? ::fgetc(f) : EOF);
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

static KYTY_SYSV_ABI int c_thread_join(LibKernel::Pthread thread, int* result)
{
	void* joined_value = nullptr;
	if (LibKernel::PthreadJoin(thread, &joined_value) != OK)
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
	LibKernel::PthreadYield();
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
static KYTY_SYSV_ABI const short* c_Getptoupper()
{
	static short table[384];
	static bool  init = false;
	if (!init)
	{
		for (int c = -1; c < 256; c++)
		{
			table[c + 1] = (c >= 0) ? static_cast<short>(::toupper(c)) : 0;
		}
		init = true;
	}
	return table + 1;
}

// Gen5 libc_v1 _Getptolower — NID 1uJgoVq3bQU. Same table contract as
// _Getptoupper: short[384] centered so index 0 is EOF (-1). Dreaming Sarah's
// Construct VFS lowercases asset names with:
//   table = _Getptolower();  dest[i] = (uint8_t)table[(unsigned char)src[i]];
// Returning a non-table pointer corrupted "data.js" and the project parse hit EOF.
static KYTY_SYSV_ABI const short* c_Getptolower()
{
	static short table[384];
	static bool  init = false;
	if (!init)
	{
		for (int c = -1; c < 256; c++)
		{
			table[c + 1] = (c >= 0) ? static_cast<short>(::tolower(c)) : 0;
		}
		init = true;
	}
	return table + 1;
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

// Gen5 libc_v1 NID NC4MSB+BRQg — same SysV shape as snprintf(buf, n, fmt, ...),
// but Astro ObjectDefinition path-building checks `r == 0` after the call (errno_t
// style: 0 success, non-zero failure). Standard snprintf returns the written
// length, which falsely trips that assert for any non-empty format result.
//
// Note: guest mesh/anim companions may open as bare `/app0/.jxm` after OD load;
// that is handled by PreferHostOdCompanionAsset (last OD basename → gfx/anim).
static KYTY_SYSV_ABI int c_snprintf_errno(VA_ARGS)
{
	VA_CONTEXT(ctx);
	char*       s   = VaArg_ptr<char>(&ctx.va_list);
	size_t      n   = VaArg_size_t(&ctx.va_list);
	const char* fmt = VaArg_ptr<const char>(&ctx.va_list);
	if (s == nullptr || n == 0 || fmt == nullptr)
	{
		return -1;
	}
	const int written = Format(s, n, fmt, &ctx.va_list);
	if (written < 0)
	{
		return written;
	}
	if (static_cast<size_t>(written) >= n)
	{
		return -1;
	}
	return 0;
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
	::fwrite(buffer, 1, static_cast<size_t>(written), f);
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
	::fwrite(buffer, 1, static_cast<size_t>(written), f);
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

static bool c_wide_format_supported(const char* format)
{
	// The narrow formatter consumes the guest VaList. Keep the accepted grammar
	// limited to formats whose argument consumption is established: literal text,
	// %% , %i and %08x. Accepting the host printf grammar here would silently
	// misinterpret unsupported guest argument layouts.
	for (const char* cursor = format; *cursor != '\0'; cursor++)
	{
		if (*cursor != '%')
		{
			continue;
		}
		cursor++;
		if (*cursor == '%' || *cursor == 'i')
		{
			continue;
		}
		if (std::strncmp(cursor, "08x", 3) != 0)
		{
			return false;
		}
		cursor += 2;
	}
	return true;
}

static KYTY_SYSV_ABI int c_vswprintf(uint16_t* out, size_t out_count, const uint16_t* wide_format, VaList* ap)
{
	if (out == nullptr || out_count == 0 || wide_format == nullptr || ap == nullptr)
	{
		return -1;
	}

	char   format[1024] = {};
	size_t format_len   = 0;
	while (wide_format[format_len] != 0)
	{
		if (format_len + 1 >= sizeof(format) || wide_format[format_len] > 0x7f)
		{
			out[0] = 0;
			return -1;
		}
		format[format_len] = static_cast<char>(wide_format[format_len]);
		format_len++;
	}
	if (!c_wide_format_supported(format))
	{
		out[0] = 0;
		return -1;
	}

	char      narrow[C_UNBOUNDED_FORMAT] = {};
	const int written                    = Format(narrow, sizeof(narrow), format, ap);
	if (written < 0 || static_cast<size_t>(written) >= out_count)
	{
		out[0] = 0;
		return -1;
	}
	for (int index = 0; index < written; index++)
	{
		out[index] = static_cast<uint8_t>(narrow[index]);
	}
	out[written] = 0;
	return written;
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
// Gen5 libc_v1 strtoull — NID 5OqszGpy7Mg (Astro after TLS context factory).
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
static KYTY_SYSV_ABI void c_abort()
{
	printf("libc::abort() called by guest\n");
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
			printf(FG_BRIGHT_YELLOW "libc: recursive static initialization guard %p skipped by owner thread" DEFAULT "\n",
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
	void (*destructor)(void*);
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

KYTY_SYSV_ABI int c_cxa_thread_atexit(void (*dtor)(void*), void* obj, void* dso_handle)
{
	if (dtor == nullptr)
	{
		return -1;
	}

	std::call_once(g_thread_atexit_hook_once, [] { LibKernel::PthreadSetHostThreadDtors(run_thread_atexit_destructors); });
	g_thread_atexit_entries.push_back({dtor, obj, dso_handle});
	return 0;
}

KYTY_SYSV_ABI int c_mtx_init(LibKernel::PthreadMutex* mutex, int type)
{
	PRINT_NAME();

	if (mutex == nullptr)
	{
		return static_cast<int>(CThreadResult::Error);
	}

	constexpr int mtx_recursive = 0x100;
	if ((type & mtx_recursive) == 0)
	{
		return c_thread_sync_result(LibKernel::PthreadMutexInit(mutex, nullptr, nullptr));
	}

	LibKernel::PthreadMutexattr attr = nullptr;
	int result                       = LibKernel::PthreadMutexattrInit(&attr);
	if (result == OK)
	{
		result = LibKernel::PthreadMutexattrSettype(&attr, 2);
	}
	if (result == OK)
	{
		result = LibKernel::PthreadMutexInit(mutex, &attr, nullptr);
	}
	if (attr != nullptr)
	{
		(void)LibKernel::PthreadMutexattrDestroy(&attr);
	}

	return c_thread_sync_result(result);
}

KYTY_SYSV_ABI int c_mtx_init_with_name(LibKernel::PthreadMutex* mutex, int type, const char* name)
{
	PRINT_NAME();

	if (mutex == nullptr)
	{
		return static_cast<int>(CThreadResult::Error);
	}

	constexpr int mtx_recursive = 0x100;
	if ((type & mtx_recursive) == 0)
	{
		return c_thread_sync_result(LibKernel::PthreadMutexInit(mutex, nullptr, name));
	}

	LibKernel::PthreadMutexattr attr = nullptr;
	int result                       = LibKernel::PthreadMutexattrInit(&attr);
	if (result == OK)
	{
		result = LibKernel::PthreadMutexattrSettype(&attr, 2);
	}
	if (result == OK)
	{
		result = LibKernel::PthreadMutexInit(mutex, &attr, name);
	}
	if (attr != nullptr)
	{
		(void)LibKernel::PthreadMutexattrDestroy(&attr);
	}

	return c_thread_sync_result(result);
}

KYTY_SYSV_ABI int c_mtx_init_with_default_name_override(LibKernel::PthreadMutex* mutex, int type, const char* name)
{
	PRINT_NAME();

	return c_mtx_init_with_name(mutex, type, name);
}

KYTY_SYSV_ABI void c_mtx_destroy(LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	if (mutex == nullptr)
	{
		return;
	}

	auto* private_mutex = *mutex;
	if (private_mutex != nullptr && reinterpret_cast<uintptr_t>(private_mutex) >= 0x100000)
	{
		(void)LibKernel::PthreadMutexDestroy(mutex);
	}
}

KYTY_SYSV_ABI int c_mtx_lock(LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return c_thread_sync_result(LibKernel::PthreadMutexLock(mutex));
}

KYTY_SYSV_ABI int c_mtx_trylock(LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return c_thread_sync_result(LibKernel::PthreadMutexTrylock(mutex));
}

static LibKernel::KernelUseconds c_abstime_remaining_usec(const LibKernel::KernelTimespec* abstime)
{
	LibKernel::KernelTimespec now {};
	if (abstime == nullptr || LibKernel::KernelClockGettime(0, &now) != OK)
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
	return static_cast<LibKernel::KernelUseconds>(delta);
}

KYTY_SYSV_ABI int c_mtx_timedlock(LibKernel::PthreadMutex* mutex, const LibKernel::KernelTimespec* abstime)
{
	PRINT_NAME();

	return c_thread_sync_result(LibKernel::PthreadMutexTimedlock(mutex, c_abstime_remaining_usec(abstime)));
}

KYTY_SYSV_ABI int c_mtx_unlock(LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return c_thread_sync_result(LibKernel::PthreadMutexUnlock(mutex));
}

KYTY_SYSV_ABI int c_mtx_current_owns(LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return LibKernel::PthreadMutexCurrentOwns(mutex) ? 1 : 0;
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
	LibKernel::PthreadCond*  condition;
	LibKernel::PthreadMutex* mutex;
	int*                     completed;
};

thread_local std::vector<CndThreadExitEntry> g_cnd_thread_exit_entries;

KYTY_SYSV_ABI void c_cnd_register_at_thread_exit(LibKernel::PthreadCond* condition,
                                                        LibKernel::PthreadMutex* mutex, int* completed)
{
	EXIT_IF(condition == nullptr || mutex == nullptr);
	g_cnd_thread_exit_entries.push_back({condition, mutex, completed});
}

KYTY_SYSV_ABI void c_cnd_unregister_at_thread_exit(LibKernel::PthreadMutex* mutex)
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
	LibKernel::KernelTimespec now {};
	if (LibKernel::KernelClockGettime(0, &now) != OK)
	{
		return 0;
	}

	// The C++ runtime converts these absolute ticks to nanoseconds by
	// multiplying by 1000 before splitting them into a timespec. Its tick
	// contract is therefore microseconds, not the 100-nanosecond host unit.
	constexpr int64_t ticks_per_second = 1000000;
	return now.tv_sec * ticks_per_second + now.tv_nsec / 1000;
}

static KYTY_SYSV_ABI void c_Xout_of_range(const char* msg)
{
	printf("std::out_of_range warning: %s\n", msg != nullptr ? msg : "");
}
static KYTY_SYSV_ABI void c_Xlength_error(const char* msg)
{
	printf("std::length_error warning: %s\n", msg != nullptr ? msg : "");
}
static KYTY_SYSV_ABI void c_Xregex_error(int error_type)
{
	printf("std::regex_error warning: error_type=%d\n", error_type);
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

// Itanium __cxa_dynamic_cast (NID hMAe+TWS9mQ). Captured Dreaming Sarah after
// Construct JSON load: rdi=src, rsi/rdx=type_info ("17ConditionOrAction" /
// "6Action"), rcx=src2dst (0 = unique base at offset 0). type_info vtables often
// point at the unresolved-object sentinel, so only src2dst arithmetic runs.
static KYTY_SYSV_ABI void* cxa_dynamic_cast(void* src, const void* /*src_type*/, const void* /*dst_type*/, int64_t src2dst)
{
	return CxaDynamicCastApply(src, src2dst);
}

// --- C++ locale / RTTI objects (Dreaming Sarah Construct string path) --------
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
static std::uint64_t g_dummy_obj_5   = 0;
static std::uint64_t g_dummy_obj_6   = 0;
static std::uint64_t g_dummy_obj_7   = 0;
static std::uint64_t g_dummy_obj_8   = 0;
static std::uint64_t g_dummy_obj_9   = 0;
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

// Exception / iostream RTTI Objects imported by case_dreaming_sarah eboot
// (libc_v1). NIDs from eboot import table; names from public symbol catalogs.
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
static CxxSiTypeInfoLayout g_typeinfo_ios_base {g_si_class_type_info_vtable, g_ti_name_ios_base, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_ios_failure {g_si_class_type_info_vtable, g_ti_name_ios_failure, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_num_put_char {g_si_class_type_info_vtable, g_ti_name_num_put_char, nullptr};

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
constexpr std::int32_t  kCxxNumPutMaxWidth = 1 << 20;
constexpr std::int32_t  kCxxNumPutMaxPrecision = 512;

// std::setw(int) returns the ABI's two-register smanip {apply, arg} (rax:rdx).
// Observed guest use after PLT resolution:
//   call setw(N) → mov %edx,%esi → call *%rax with rdi already adjusted to the
//   ios_base subobject. Returning only N in eax made call *%rax jump to a
//   near-null address (RIP=N) and FatalFault with rc=139.
struct CxxIosSmanipInt
{
	void (*apply)(CxxIosBaseLayout* ios, int arg);
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
	*format++ = '.';
	*format++ = '*';
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
	const int output_size = ::snprintf(output, sizeof(output), format_buffer, precision, value);
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
// then the eight standard narrow-character numeric formatting overloads.
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
    reinterpret_cast<void*>(&c_num_put_do_put_double),
    reinterpret_cast<void*>(&c_num_put_do_put_long_double),
    reinterpret_cast<void*>(&c_num_put_do_put_pointer),
    reinterpret_cast<void*>(&c_num_put_do_put_long_long),
    reinterpret_cast<void*>(&c_num_put_do_put_ulong_long),
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
	LibKernel::PthreadCond  condition;
	LibKernel::PthreadMutex mutex;
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

static void c_thread_pad_launch(CxxThreadPad* self, LibKernel::Pthread* thread, const LibKernel::PthreadAttr* attr,
                                const char* name)
{
	EXIT_IF(self == nullptr || thread == nullptr);

	const int result = LibKernel::PthreadCreate(thread, attr, c_thread_pad_call_func, self, name != nullptr ? name : "");
	if (result != OK)
	{
		EXIT("std::_Pad thread creation failed with kernel result 0x%x\n", static_cast<unsigned int>(result));
	}

	while (!self->launched)
	{
		c_thread_require(c_cnd_wait(&self->condition, &self->mutex), "std::_Pad launch condition wait");
	}
}

static KYTY_SYSV_ABI void c_thread_pad_launch(CxxThreadPad* self, LibKernel::Pthread* thread)
{
	c_thread_pad_launch(self, thread, nullptr, "");
}

static KYTY_SYSV_ABI void c_thread_pad_named_launch(CxxThreadPad* self, const char* name, LibKernel::Pthread* thread)
{
	c_thread_pad_launch(self, thread, nullptr, name);
}

static KYTY_SYSV_ABI void c_thread_pad_attr_launch(CxxThreadPad* self, const LibKernel::PthreadAttr* attr,
                                                   LibKernel::Pthread* thread)
{
	c_thread_pad_launch(self, thread, attr, "");
}

static KYTY_SYSV_ABI void c_thread_pad_named_attr_launch(CxxThreadPad* self, const char* name,
                                                         const LibKernel::PthreadAttr* attr, LibKernel::Pthread* thread)
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

static KYTY_SYSV_ABI CxxStringLayout* c_error_category_message(CxxStringLayout* result, const CxxErrorCategoryLayout* self,
                                                               int32_t value)
{
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

static KYTY_SYSV_ABI const CxxErrorCategoryLayout* c_generic_category()
{
	return &g_generic_error_category;
}

static KYTY_SYSV_ABI const CxxErrorCategoryLayout* c_system_category()
{
	return &g_system_error_category;
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

// std::uncaught_exception() — returns non-zero while an exception is active.
// Full EH is not implemented; report "no active exception" so destructors that
// probe this during Construct string/locale work continue (Dreaming Sarah).
static KYTY_SYSV_ABI int c_uncaught_exception()
{
	return 0;
}

// Itanium ABI __gxx_personality_v0. The current HLE exception path stops at
// __cxa_throw, so this is only required to relocate guest unwind metadata.
// Returning _URC_CONTINUE_UNWIND (8) is the conservative choice if a guest
// unwinder reaches it: do not claim to recognize or handle a foreign frame.
static KYTY_SYSV_ABI int c_gxx_personality_v0(int /*version*/, int /*actions*/, uint64_t /*exception_class*/,
                                              void* /*exception_object*/, void* /*context*/)
{
	return 8;
}

// std::ios_base::~ios_base() — guest tears down temporary stream objects after
// locale/ctype probes. No host side-effects required for the stub ios_base.
static KYTY_SYSV_ABI void c_ios_base_dtor(void* /*self*/) {}

// std::ios_base::failure::~failure() [complete object]. Guest code owns the
// storage; the HLE exception/locale objects have no host-side payload to tear
// down, so destruction deliberately leaves the guest allocation untouched.
static KYTY_SYSV_ABI void c_ios_base_failure_dtor(void* /*self*/) {}

// Itanium C++ ABI exception entry points (Gen5 libc_v1). Full unwind is not
// implemented; throws are host-fatal with a decoded type/message so the
// producer of the exception remains diagnosable (same spirit as Xlength_error).
static KYTY_SYSV_ABI void* cxa_allocate_exception(size_t thrown_size)
{
	// Header is opaque to the guest object body; keep a small leading region
	// for freestanding dtor bookkeeping if free_exception is later wired.
	constexpr size_t kHeader = 128;
	void*            block   = ::malloc(kHeader + (thrown_size != 0 ? thrown_size : 1));
	EXIT_IF(block == nullptr);
	return static_cast<uint8_t*>(block) + kHeader;
}

static KYTY_SYSV_ABI void cxa_free_exception(void* thrown_exception)
{
	if (thrown_exception == nullptr)
	{
		return;
	}
	constexpr size_t kHeader = 128;
	::free(static_cast<uint8_t*>(thrown_exception) - kHeader);
}

static KYTY_SYSV_ABI void cxa_decrement_exception_refcount(void* thrown_exception)
{
	if (thrown_exception == nullptr)
	{
		return;
	}

	EXIT("__cxa_decrement_exception_refcount requires an exception header: obj=%p\n", thrown_exception);
}

// Only touch pointers that are known host-mapped. Reject the NoAccess
// unresolved-object sentinel at ~0x840000000 and other sparse holes.
[[nodiscard]] static bool CxaGuestPtrLooksMapped(const void* p, size_t bytes)
{
	const auto a = reinterpret_cast<uintptr_t>(p);
	if (a < 0x1000u || bytes == 0 || a > UINTPTR_MAX - bytes)
	{
		return false;
	}
	const auto end = a + bytes;
	// Unresolved weak Object sentinel (VirtualMemory NoAccess page).
	if (a >= 0x840000000ull && a < 0x850000000ull)
	{
		return false;
	}
	// Main image and TLS/data around 0x900000000.
	if (a >= 0x900000000ull && end <= 0x920000000ull)
	{
		return true;
	}
	// Flexible/direct guest heaps used by titles (low 32-bit and mid ranges).
	if (a >= 0x10000ull && end <= 0x080000000ull)
	{
		return true;
	}
	if (a >= 0x100000000ull && end <= 0x200000000ull)
	{
		return true;
	}
	// Host/HLE-owned exception objects may still land here. High userspace
	// mappings on Linux.
	if (a >= 0x7f0000000000ull && end < 0x800000000000ull)
	{
		return true;
	}
	return false;
}

static const char* CxaTryReadCString(const void* p)
{
	if (!CxaGuestPtrLooksMapped(p, 1))
	{
		return nullptr;
	}
	const auto* s = static_cast<const char*>(p);
	for (int i = 0; i < 256; i++)
	{
		if (!CxaGuestPtrLooksMapped(s + i, 1))
		{
			return nullptr;
		}
		const unsigned char c = static_cast<unsigned char>(s[i]);
		if (c == 0)
		{
			return i > 0 ? s : nullptr;
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
static KYTY_SYSV_ABI void cxa_throw(void* thrown_exception, void* tinfo, void (* /*dest*/)(void*))
{
	const char* type_name = nullptr;
	const char* what_msg  = nullptr;

	if (CxaGuestPtrLooksMapped(tinfo, 16))
	{
		const auto* words = static_cast<const uint64_t*>(tinfo);
		type_name         = CxaTryReadCString(reinterpret_cast<const void*>(words[1]));
		if (type_name != nullptr && type_name[0] == '*')
		{
			type_name++; // libstdc++ marks non-mangled names with a leading '*'
		}
	}

	if (CxaGuestPtrLooksMapped(thrown_exception, 32))
	{
		// Heuristic: many libstdc++ exception objects store a std::string at +8.
		// SSO layout (GCC): local buffer at +16 when capacity field at +24 is small.
		const auto* words = static_cast<const uint64_t*>(thrown_exception);
		const auto  cap   = words[3]; // often capacity for SSO string
		if (cap <= 15u)
		{
			what_msg = CxaTryReadCString(static_cast<const char*>(thrown_exception) + 16);
		} else
		{
			what_msg = CxaTryReadCString(reinterpret_cast<const void*>(words[1]));
		}
		if (what_msg == nullptr)
		{
			what_msg = CxaTryReadCString(reinterpret_cast<const void*>(words[2]));
		}
	}

	EXIT("__cxa_throw type=%s what=%s obj=%p tinfo=%p\n", type_name != nullptr ? type_name : "?", what_msg != nullptr ? what_msg : "?",
	     thrown_exception, tinfo);
}

static KYTY_SYSV_ABI int atexit(void (*func)())
{
	PRINT_NAME();

	Kyty::printf("func = %" PRIx64 "\n", reinterpret_cast<uint64_t>(func));

	int ok = ::atexit(func);

	EXIT_NOT_IMPLEMENTED(ok != 0);

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
// on the Astro boot path after Posix semaphores.
static KYTY_SYSV_ABI int c_putchar(int ch)
{
	return GetPrintfStdFunc()("%c", ch);
}

// Guest FILE* is not always a host FILE*. Log path uses the host printf
// sink; the stream argument is accepted for ABI compatibility only.
static KYTY_SYSV_ABI int c_fputs(const char* s, FILE* /*stream*/)
{
	if (s == nullptr)
	{
		return EOF;
	}
	return GetPrintfStdFunc()("%s", s);
}

static KYTY_SYSV_ABI void catchReturnFromMain(int status)
{
	PRINT_NAME();

	Kyty::printf("return from main = %d\n", status);
}

KYTY_SYSV_ABI int cxa_atexit(void (*func)(void*), void* arg, void* d)
{
	PRINT_NAME();

	auto* cc = Core::Singleton<CContext>::Instance();

	CxaDestructor c {};
	c.destructor_func   = func;
	c.destructor_object = arg;
	c.module_id         = d;

	cc->cxa.Add(c);

	return 0;
}

void KYTY_SYSV_ABI cxa_finalize(void* d)
{
	PRINT_NAME();

	auto* cc = Core::Singleton<CContext>::Instance();

	FOR_LIST_R(i, cc->cxa)
	{
		auto& c = cc->cxa[i];
		if (c.module_id == d && c.destructor_func != nullptr)
		{
			c.destructor_func(c.destructor_object);
			c.destructor_func = nullptr;
		}
	}
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
	LIB_OBJECT("1TDo-ImqkJc", stdin);
	LIB_OBJECT("2sWzhYqFH4E", stdout);
	LIB_OBJECT("H8AprKeZtNg", stderr);

	LIB_FUNC("-hn1tcVHq5Q", LibcInternal::LibcMspaceCreate);
	LIB_FUNC("OJjm-QOIHlI", LibcInternal::LibcMspaceMalloc);
	LIB_FUNC("iF1iQHzxBJU", LibcInternal::LibcMspaceMemalign);
	LIB_FUNC("fEoW6BJsPt4", LibcInternal::LibcMspaceMallocUsableSize);
	LIB_FUNC("LYo3GhIlB38", LibcInternal::LibcMspaceCalloc);
	LIB_FUNC("Vla-Z+eXlxo", LibcInternal::LibcMspaceFree);
	LIB_FUNC("k04jLXu3+Ic", LibcInternal::LibcMspaceMallocStatsFast);

	// C++ locale / RTTI objects — Dreaming Sarah (Qoo175Ig+-k → classic locale).
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
	LIB_OBJECT("OwfBD-2nhJQ", LibC::g_time_put_char_vtable);
	LIB_OBJECT("FQ9NFbBHb5Y", &LibC::g_bad_off);
	LIB_OBJECT("wiR+rIcbnlc", LibC::g_fpz);
	// HEAD-only unresolved Object placeholders (NIDs that do not collide with RTTI above).
	LIB_OBJECT("MpxhMh8QFro", &LibC::g_dummy_obj_5);
	LIB_OBJECT("NU-T4QowTNA", &LibC::g_dummy_obj_6);
	LIB_OBJECT("DbEnA+MnVIw", &LibC::g_dummy_obj_9);
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
	// Gen5 putchar — NID m5wN+SwZOR4 (hard-abort after Posix sem on Astro).
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
	// Gen5 second memcpy NID — Dreaming Sarah std::string SSO short-assign path
	// after __cxa_dynamic_cast (Construct Action setup): (dst, src, n=1..).
	LIB_FUNC("Noj9PsJrsa8", LibC::c_memcpy);
	LIB_FUNC("NFLs+dRJGNg", LibC::c_memcpy_s);
	// Gen5 libc_v1 memmove_s — B59+zQQCcbU after TLS factory / strtoull on Astro.
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
	// Gen5 libc_v1 C++ EH — Dreaming Sarah throw path after flip/init.
	// vkuuLfhnSZI: __cxa_throw (rdi=obj, rsi=typeinfo, rdx=dtor; ud2 after).
	LIB_FUNC("vkuuLfhnSZI", LibC::cxa_throw);
	LIB_FUNC("hdm0YfMa7TQ", LibC::cxx_new_array);         // operator new[](size_t)
	LIB_FUNC("Jh5qUcwiSEk", LibC::cxx_new_array_nothrow); // operator new[](size_t, const std::nothrow_t&)
	LIB_FUNC("MLWl90SFWNE", LibC::cxx_delete_array);      // operator delete[](void*)
	LIB_FUNC("FOt55ZNaVJk", LibC::cxx_delete_array_sized); // operator delete[](void*, size_t)
	LIB_FUNC("cjZEuzHkgng", LibC::c_atomic_load_4);
	LIB_FUNC("0AgCOypbQ90", LibC::c_atomic_compare_exchange_weak_4);
	LIB_FUNC("iPBqs+YUUFw", LibC::c_atomic_fetch_add_4);
	LIB_FUNC("2HnmKiLmV6s", LibC::c_atomic_fetch_sub_4);
	LIB_FUNC("np6xXcXEnXE", LibKernel::PthreadGetthreadid);
	LIB_FUNC("CHrhwd8QSBs", LibC::c_thread_hardware_concurrency);
	LIB_FUNC("YvmY5Jf0VYU", LibC::c_thread_join);
	LIB_FUNC("exNzzCAQuWM", LibC::c_thread_yield);
	LIB_FUNC("dGYo9mE8K2A", LibC::c_thread_pad_ctor);
	LIB_FUNC("uhnb6dnXOnc", LibC::c_thread_pad_named_ctor);
	LIB_FUNC("gjLRZgfb3i0", LibC::c_thread_pad_dtor);
	LIB_FUNC("XyJPhPqpzMw", LibC::c_thread_pad_dtor);
	LIB_FUNC("xZqiZvmcp9k", static_cast<void (*)(LibC::CxxThreadPad*, LibKernel::Pthread*)>(LibC::c_thread_pad_launch));
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
	LIB_FUNC("j9LU8GsuEGw", LibC::c_time_put_put);
	LIB_FUNC("rcQCUr0EaRU", LibC::c_Getptoupper);
	// Gen5 _Getptolower — Dreaming Sarah VFS path lowercasing after ~INDEX.
	LIB_FUNC("1uJgoVq3bQU", LibC::c_Getptolower);

	// stdio
	LIB_FUNC("xeYO4u7uyJ0", LibC::c_fopen);
	LIB_FUNC("uodLYyUip20", LibC::c_fclose);
	LIB_FUNC("lbB+UlZqVG0", LibC::c_fread);
	// Gen5 fgets — NID KdP-nULpuGw (next hard-abort after asctime on Astro).
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
	// Gen5 libc_v1 safe format — NID NC4MSB+BRQg. SysV matches snprintf, but
	// return is 0 on success (ObjectDefinition path builder asserts r == 0).
	LIB_FUNC("NC4MSB+BRQg", LibC::c_snprintf_errno);
	// Gen5 vsprintf_s — NID +qitMEbkSWk (hard-abort after fgets on Astro).
	LIB_FUNC("+qitMEbkSWk", LibC::c_vsprintf_s);
	LIB_FUNC("Q2V+iqvjgC0", LibC::c_vsnprintf); // vsnprintf (Gen5 libc_v1)
	LIB_FUNC("tcVi5SivF7Q", LibC::c_sprintf);
	// Gen5 sprintf_s — NID xEszJVGpybs (hard-abort after Fiber init on Astro).
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
	// Gen5 strtoul: Kyty maps QxmSHBCuKTk / zlfEH8FmyUA; Dreaming Sarah
	// Construct parser also hits VOBg+iNwB-4 (rdi=nptr, rsi=endptr, rdx=10).
	LIB_FUNC_ALIASES(LibC::c_strtoul, "QxmSHBCuKTk", "zlfEH8FmyUA", "VOBg+iNwB-4");
	// Gen5 libc_v1 strtoull — 5OqszGpy7Mg after TLS context factory on Astro.
	LIB_FUNC("5OqszGpy7Mg", LibC::c_strtoull);
	LIB_FUNC("SRI6S9B+-a4", LibC::c_atof);
	LIB_FUNC("AEJdIVZTEmo", LibC::c_qsort);
	LIB_FUNC("L1SBTkC+Cvw", LibC::c_abort);
	LIB_FUNC("VPbJwTCgME0", LibC::c_srand);
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
	LIB_FUNC("jT3xiGpA3B4", LibC::Time::c_asctime);

	// math (double)
	LIB_FUNC("H8ya2H00jbI", LibC::c_sin);
	LIB_FUNC("2WE3BTYVwKM", LibC::c_cos);
	LIB_FUNC("T7uyNqP7vQA", LibC::c_tan);
	LIB_FUNC("7Ly52zaL44Q", LibC::c_asin);
	LIB_FUNC("JBcgYuW8lPU", LibC::c_acos);
	LIB_FUNC("OXmauLdQ8kY", LibC::c_atan);
	LIB_FUNC("HUbZmOnT-Dg", LibC::c_atan2);
	LIB_FUNC("NVadfnzQhHQ", LibC::c_exp);
	LIB_FUNC("rtV7-jWC6Yg", LibC::c_log);
	LIB_FUNC("9LCjpWyQ5Zc", LibC::c_pow);
	LIB_FUNC("H+8UBOwfScI", LibC::c_powidf2);
	LIB_FUNC("pKwslsMUmSk", LibC::c_fmod);
	// Gen5 libc_v1 double rounding and absolute-value exports. Float variants
	// are registered below.
	LIB_FUNC("gacfOmO8hNs", LibC::c_ceil);
	LIB_FUNC("mpcTgMzhUY8", LibC::c_floor);
	LIB_FUNC("nlaojL9hDtA", LibC::c_round);
	LIB_FUNC("MXRNWnosNlM", LibC::c_sqrt);
	LIB_FUNC("388LcMWHRCA", LibC::c_fabs);
	LIB_FUNC("0WMHDb5Dt94", LibC::c_modf);
	LIB_FUNC("JrwFIMzKNr0", LibC::c_ldexp);
	LIB_FUNC("kA-TdiOCsaY", LibC::c_frexp);
	LIB_FUNC("jMB7EFyu30Y", LibC::c_sincos);
	// math (float)
	LIB_FUNC("1D0H2KNjshE", LibC::c_powf);
	// Gen5 libc_v1 __isnanf — lA94ZgT+vMM after Posix pthread_self on Astro.
	LIB_FUNC("lA94ZgT+vMM", LibC::c_isnanf);
	// Gen5 isfinite(double) — Dreaming Sarah after strtod in project parse.
	LIB_FUNC("dhK16CKwhQg", LibC::c_isfinite);
	// Gen5 isnan(double) — Dreaming Sarah layout coord checks after
	// vcvttsd2si; return 0 continues (non-zero rejects).
	LIB_FUNC("GfxAp9Xyiqs", LibC::c_isnan);
	// Gen5 libc_v1 float math (Astro after usleep; NIDs from name→NID hash).
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
	LIB_FUNC("pztV4AF18iI", LibC::c_sincosf);

	// C++ runtime
	LIB_FUNC("3GPpjQdAMTw", LibC::c_cxa_guard_acquire);
	LIB_FUNC("9rAeANT2tyE", LibC::c_cxa_guard_release);
	LIB_FUNC("2emaaluWzUw", LibC::c_cxa_guard_abort);
	LIB_FUNC("BKSCW2bCACA", LibC::c_cxa_thread_atexit);
	LIB_FUNC("Z2tTVqGDPGQ", LibC::c_cxa_thread_atexit);
	// Gen5 __cxa_dynamic_cast — Dreaming Sarah Construct ConditionOrAction→Action.
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
