#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/LinkList.h"
#include "Kyty/Core/MSpace.h"
#include "Kyty/Core/Singleton.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/ApplicationHeap.h"
#include "Emulator/Libs/CxaDynamicCast.h"
#include "Emulator/Libs/CxxLocale.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Memalign.h"
#include "Emulator/Libs/ProcessEnvironment.h"
#include "Emulator/Libs/Printf.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <cctype>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <mutex>
#include <setjmp.h>
#include <strings.h>
#include <unordered_map>

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

using Kyty::Libs::Graphics::GpuMemoryNotifyHostWrite;

LIB_VERSION("libc", 1, "libc", 1, 1);

// Gen5 libc/libSceLibcInternal "need" flag: non-zero asks the guest CRT to run
// heap/TSD bootstrap. Zero claims "already initialized" and skips that path.
// A title that uses libc's internal mspace before ApplicationHeap create can
// leave the mspace at BSS zero when this stays 0, so operator new
// returns null → bad_alloc → terminate (DebugRaiseException 0xa0020008).
// Keep both LibC and LibcInternal objects in sync.
static uint32_t g_need_flag = 1;

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
static KYTY_SYSV_ABI int c_cnd_init(LibKernel::PthreadCond* cond)
{
	return LibKernel::PthreadCondInit(cond, nullptr, nullptr);
}

enum class CThreadResult : int
{
	Success = 0,
	Error   = 4,
};

static int c_thread_result(int result)
{
	return result == OK ? static_cast<int>(CThreadResult::Success) : static_cast<int>(CThreadResult::Error);
}

static KYTY_SYSV_ABI int c_cnd_broadcast(LibKernel::PthreadCond* cond)
{
	return c_thread_result(LibKernel::PthreadCondBroadcast(cond));
}

enum class AllocationOwner
{
	Host,
	ApplicationHeap,
};

struct AllocationRecord
{
	AllocationOwner owner;
	size_t          size;
};

static std::mutex                                  g_allocations_mutex;
static std::unordered_map<void*, AllocationRecord> g_allocations;

static bool register_allocation(void* ptr, AllocationRecord record)
{
	if (ptr == nullptr)
	{
		return false;
	}

	std::lock_guard lock(g_allocations_mutex);
	return g_allocations.emplace(ptr, record).second;
}

static bool claim_allocation(void* ptr, AllocationRecord* record)
{
	if (ptr == nullptr || record == nullptr)
	{
		return false;
	}

	std::lock_guard lock(g_allocations_mutex);
	const auto      it = g_allocations.find(ptr);
	if (it == g_allocations.end())
	{
		return false;
	}

	*record = it->second;
	g_allocations.erase(it);
	return true;
}

static void* allocate_with_owner(size_t size)
{
	if (void* ptr = LibKernel::ApplicationHeap::Malloc(size); ptr != nullptr)
	{
		const bool registered = register_allocation(ptr, {AllocationOwner::ApplicationHeap, size});
		EXIT_IF(!registered);
		return ptr;
	}

	if (LibKernel::ApplicationHeap::IsInitialized())
	{
		return nullptr;
	}

	void* ptr = ::malloc(size);
	if (ptr != nullptr)
	{
		const bool registered = register_allocation(ptr, {AllocationOwner::Host, size});
		EXIT_IF(!registered);
	}
	return ptr;
}

static bool free_by_owner(void* ptr)
{
	if (ptr == nullptr)
	{
		return true;
	}

	AllocationRecord record {};
	if (claim_allocation(ptr, &record))
	{
		if (record.owner == AllocationOwner::ApplicationHeap)
		{
			return LibKernel::ApplicationHeap::Free(ptr);
		}
		::free(ptr);
		return true;
	}

	if (LibKernel::ApplicationHeap::HasAllocator())
	{
		return LibKernel::ApplicationHeap::Free(ptr);
	}

	::free(ptr);
	return true;
}

// Capture the host allocator's live accounting for the libc bootstrap path.
// The startup path calls malloc_stats_fast before the application heap table
// is published, so forwarding to a guest slot at that point would recurse into
// an uninitialized allocator. mallinfo2 is used where glibc exposes it; the
// ledger remains the portable lower-bound fallback on other hosts.
static void collect_host_malloc_stats(Core::MSpaceSize* out)
{
	if (out == nullptr)
	{
		return;
	}

	size_t current_inuse = 0;
	{
		std::lock_guard lock(g_allocations_mutex);
		for (const auto& [ptr, record]: g_allocations)
		{
			(void)ptr;
			if (record.size <= SIZE_MAX - current_inuse)
			{
				current_inuse += record.size;
			}
		}
	}

	size_t current_system = current_inuse;
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__GLIBC__)
	const auto host_info = ::mallinfo2();
	if (host_info.arena <= static_cast<size_t>(SIZE_MAX - host_info.hblkhd))
	{
		current_system = std::max(current_system, static_cast<size_t>(host_info.arena + host_info.hblkhd));
	}
	current_inuse = std::max(current_inuse, static_cast<size_t>(host_info.uordblks));
#endif

	current_system              = std::max(current_system, current_inuse);
	out->max_system_size     = current_system;
	out->current_system_size = current_system;
	out->max_inuse_size      = current_inuse;
	out->current_inuse_size  = current_inuse;
}

// Standard C allocation routes through the guest application heap after the
// title registers and creates it. Before that point, host allocation remains
// the bootstrap fallback for HLE-owned libc objects.
static KYTY_SYSV_ABI void* c_malloc(size_t size)
{
	return allocate_with_owner(size);
}

static KYTY_SYSV_ABI void* c_calloc(size_t n, size_t size)
{
	if (size != 0 && n > SIZE_MAX / size)
	{
		return nullptr;
	}

	const size_t total = n * size;
	void*        ptr   = allocate_with_owner(total);
	if (ptr != nullptr)
	{
		::memset(ptr, 0, total);
	}
	return ptr;
}

struct AlignedAllocation
{
	void*  base;
	size_t size;
	size_t alignment;
};

static std::mutex                                   g_aligned_allocations_mutex;
static std::unordered_map<void*, AlignedAllocation> g_aligned_allocations;

static bool register_aligned_allocation(void* ptr, const AlignedAllocation& allocation)
{
	if (ptr == nullptr)
	{
		return false;
	}

	std::lock_guard lock(g_aligned_allocations_mutex);
	return g_aligned_allocations.emplace(ptr, allocation).second;
}

// Transfers ownership out of the registry in one operation. Callers must not
// concurrently realloc or free the same pointer; distinct allocations remain
// independently safe. A failed realloc restores the claimed record.
static bool claim_aligned_allocation(void* ptr, AlignedAllocation* allocation)
{
	if (ptr == nullptr || allocation == nullptr)
	{
		return false;
	}

	std::lock_guard lock(g_aligned_allocations_mutex);
	const auto      it = g_aligned_allocations.find(ptr);
	if (it == g_aligned_allocations.end())
	{
		return false;
	}

	*allocation = it->second;
	g_aligned_allocations.erase(it);
	return true;
}

static KYTY_SYSV_ABI void* c_memalign(size_t alignment, size_t size)
{
	// Prospero/FreeBSD memalign: any power-of-two alignment (including 4).
	// Do not require alignof(void*); titles allocate uint32 index tables via
	// memalign(4, N) and immediately fill the returned pointer.
	if (!MemalignAlignmentOk(alignment) || size > SIZE_MAX - (alignment - 1))
	{
		return nullptr;
	}

	const size_t total = size + alignment - 1;
	void*        base  = allocate_with_owner(total);
	if (base == nullptr)
	{
		return nullptr;
	}

	const auto raw     = reinterpret_cast<uintptr_t>(base);
	const auto aligned = (raw + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
	if (!register_aligned_allocation(reinterpret_cast<void*>(aligned), AlignedAllocation {base, size, alignment}))
	{
		(void)free_by_owner(base);
		return nullptr;
	}
	return reinterpret_cast<void*>(aligned);
}

static KYTY_SYSV_ABI void c_free(void* p);

static KYTY_SYSV_ABI void* c_realloc(void* p, size_t size)
{
	if (p == nullptr)
	{
		return c_malloc(size);
	}
	if (size == 0)
	{
		c_free(p);
		return nullptr;
	}

	AlignedAllocation allocation {};
	if (claim_aligned_allocation(p, &allocation))
	{
		void* replacement = c_memalign(allocation.alignment, size);
		if (replacement == nullptr)
		{
			const bool restored = register_aligned_allocation(p, allocation);
			EXIT_IF(!restored);
			return nullptr;
		}

		::memcpy(replacement, p, (allocation.size < size ? allocation.size : size));
		if (!free_by_owner(allocation.base))
		{
			EXIT("ApplicationHeap free failed during aligned realloc\n");
		}
		return replacement;
	}

	AllocationRecord record {};
	if (!claim_allocation(p, &record))
	{
		EXIT_NOT_IMPLEMENTED(LibKernel::ApplicationHeap::HasAllocator());
		return ::realloc(p, size);
	}

	if (record.owner == AllocationOwner::ApplicationHeap)
	{
		void* replacement = allocate_with_owner(size);
		if (replacement == nullptr)
		{
			const bool restored = register_allocation(p, record);
			EXIT_IF(!restored);
			return nullptr;
		}

		::memcpy(replacement, p, (record.size < size ? record.size : size));
		if (!LibKernel::ApplicationHeap::Free(p))
		{
			EXIT("ApplicationHeap free failed during realloc\n");
		}
		return replacement;
	}

	void* replacement = ::realloc(p, size);
	if (replacement == nullptr)
	{
		const bool restored = register_allocation(p, record);
		EXIT_IF(!restored);
		return nullptr;
	}

	const bool registered = register_allocation(replacement, {AllocationOwner::Host, size});
	EXIT_IF(!registered);
	return replacement;
}

static KYTY_SYSV_ABI void c_free(void* p)
{
	AlignedAllocation allocation {};
	if (claim_aligned_allocation(p, &allocation))
	{
		if (!free_by_owner(allocation.base))
		{
			EXIT("ApplicationHeap aligned free failed\n");
		}
		return;
	}
	if (!free_by_owner(p))
	{
		EXIT("ApplicationHeap free failed\n");
	}
}
static KYTY_SYSV_ABI void* c_memcpy(void* d, const void* s, size_t n)
{
	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(d), n);
	return ::memcpy(d, s, n);
}
static KYTY_SYSV_ABI int c_memcpy_s(void* d, size_t dn, const void* s, size_t n)
{
	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(d), n < dn ? n : dn);
	return (::memcpy(d, s, n < dn ? n : dn), 0);
}
// Gen5 libc_v1 memmove_s — NID B59+zQQCcbU (Astro after strtoull).
static KYTY_SYSV_ABI int c_memmove_s(void* d, size_t dn, const void* s, size_t n)
{
	if (d == nullptr || s == nullptr)
	{
		return -1;
	}
	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(d), n < dn ? n : dn);
	::memmove(d, s, n < dn ? n : dn);
	return 0;
}
static KYTY_SYSV_ABI void* c_memmove(void* d, const void* s, size_t n)
{
	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(d), n);
	return ::memmove(d, s, n);
}
static KYTY_SYSV_ABI void* c_memset(void* d, int c, size_t n)
{
	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(d), n);
	return ::memset(d, c, n);
}
// Gen5 libc_v1 memset_s — NID h8GwqPFbu6I (Astro after DrawIndexIndirect).
// SysV: rdi=s, rsi=smax, rdx=c, rcx=n. Returns 0 on success.
static KYTY_SYSV_ABI int c_memset_s(void* s, size_t smax, int c, size_t n)
{
	if (s == nullptr)
	{
		return -1;
	}
	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(s), n < smax ? n : smax);
	::memset(s, c, n < smax ? n : smax);
	return 0;
}
static KYTY_SYSV_ABI int c_memcmp(const void* a, const void* b, size_t n)
{
	return ::memcmp(a, b, n);
}
static KYTY_SYSV_ABI void* c_memchr(const void* s, int c, size_t n)
{
	return const_cast<void*>(::memchr(s, c, n));
}
static KYTY_SYSV_ABI size_t c_strlen(const char* s)
{
	return ::strlen(s);
}
static KYTY_SYSV_ABI size_t c_wcslen(const uint16_t* s)
{
	const uint16_t* end = s;
	while (*end != 0)
	{
		end++;
	}
	return static_cast<size_t>(end - s);
}
static KYTY_SYSV_ABI uint16_t* c_wcsncpy(uint16_t* destination, const uint16_t* source, size_t count)
{
	size_t index = 0;
	while (index < count && source[index] != 0)
	{
		destination[index] = source[index];
		index++;
	}
	while (index < count)
	{
		destination[index] = 0;
		index++;
	}
	return destination;
}
static bool c_is_ascii_alphanumeric(uint32_t character)
{
	return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
	       (character >= 'a' && character <= 'z');
}

static bool c_is_ascii_punctuation(uint32_t character)
{
	return (character >= '!' && character <= '/') || (character >= ':' && character <= '@') ||
	       (character >= '[' && character <= '`') || (character >= '{' && character <= '~');
}

static KYTY_SYSV_ABI int c_Iswctype(uint32_t character, int character_class)
{
	if (character > 0x7f)
	{
		return 0;
	}

	// The guest CRT passes compact descriptor values rather than host wctype_t
	// masks. Class 2 appears in its wide printf parser and must accept conversion
	// letters as well as decimal width and precision digits. Keep this ASCII-only
	// so host locale state cannot alter guest control flow.
	switch (character_class)
	{
		case 2: return c_is_ascii_alphanumeric(character) ? 1 : 0;
		case 9: return c_is_ascii_punctuation(character) ? 1 : 0;
		default: return 0;
	}
}
static KYTY_SYSV_ABI int c_Wctombx(char* dst, uint32_t character, std::mbstate_t* /*state*/, const void* /*cvtvec*/)
{
	if (dst == nullptr)
	{
		return 0;
	}
	EXIT_NOT_IMPLEMENTED(character > 0x7f);
	dst[0] = static_cast<char>(character);
	return 1;
}
static KYTY_SYSV_ABI int c_Mbtowcx(uint16_t* dst, const char* src, size_t count, std::mbstate_t* /*state*/, const void* /*cvtvec*/)
{
	if (src == nullptr)
	{
		return 0;
	}
	if (count == 0)
	{
		return -2;
	}
	const auto ch = static_cast<uint8_t>(src[0]);
	EXIT_NOT_IMPLEMENTED(ch > 0x7f);
	if (dst != nullptr)
	{
		*dst = ch;
	}
	return ch == 0 ? 0 : 1;
}
static KYTY_SYSV_ABI char* c_strcpy(char* d, const char* s)
{
	return ::strcpy(d, s);
}
// Gen5 libc_v1 wide mem* — NIDs from the public Gen5 export hash (SHA1+suffix, byte-reversed).
static KYTY_SYSV_ABI wchar_t* c_wmemchr(const wchar_t* s, wchar_t c, size_t n)
{
	return const_cast<wchar_t*>(std::wmemchr(s, c, n));
}
static KYTY_SYSV_ABI int      c_wmemcmp(const wchar_t* a, const wchar_t* b, size_t n)
{
	return std::wmemcmp(a, b, n);
}
static KYTY_SYSV_ABI int c_wmemcmp16(const char16_t* a, const char16_t* b, size_t n)
{
	if (n == 0)
	{
		return 0;
	}
	EXIT_IF(a == nullptr || b == nullptr);
	for (size_t i = 0; i < n; i++)
	{
		if (a[i] != b[i])
		{
			return (a[i] < b[i] ? -1 : 1);
		}
	}
	return 0;
}
static KYTY_SYSV_ABI char16_t* c_wmemcpy16(char16_t* d, const char16_t* s, size_t n)
{
	if (n == 0)
	{
		return d;
	}
	EXIT_IF(d == nullptr || s == nullptr);
	for (size_t i = 0; i < n; i++)
	{
		d[i] = s[i];
	}
	return d;
}
static KYTY_SYSV_ABI wchar_t* c_wmemcpy(wchar_t* d, const wchar_t* s, size_t n)
{
	return std::wmemcpy(d, s, n);
}
static KYTY_SYSV_ABI wchar_t* c_wmemmove(wchar_t* d, const wchar_t* s, size_t n)
{
	return std::wmemmove(d, s, n);
}
static KYTY_SYSV_ABI wchar_t* c_wmemset(wchar_t* s, wchar_t c, size_t n)
{
	return std::wmemset(s, c, n);
}
// Gen5 strcpy_s — NID 5Xa2ACNECdo: dest, destsz, src. Returns 0 on success.
static KYTY_SYSV_ABI int c_strcpy_s(char* d, size_t destsz, const char* s)
{
	if (d == nullptr || s == nullptr || destsz == 0)
	{
		return -1;
	}
	const size_t n = ::strlen(s);
	if (n + 1 > destsz)
	{
		d[0] = '\0';
		return -1;
	}
	::memcpy(d, s, n + 1);
	return 0;
}
static KYTY_SYSV_ABI char* c_strncpy(char* d, const char* s, size_t n)
{
	return ::strncpy(d, s, n);
}
static KYTY_SYSV_ABI int c_strcmp(const char* a, const char* b)
{
	return ::strcmp(a, b);
}
static KYTY_SYSV_ABI int c_strncmp(const char* a, const char* b, size_t n)
{
	return ::strncmp(a, b, n);
}
// NID AV6ipCNa4Rw. Null args are UB on host strcasecmp; guest may pass null
// strcasecmp; guest may pass null in boot string compares — return non-zero when
// either side is null (not equal), matching a safe strcmp-like contract.
static KYTY_SYSV_ABI int c_strcasecmp(const char* a, const char* b)
{
	if (a == nullptr || b == nullptr)
	{
		return (a == b ? 0 : 1);
	}
	return ::strcasecmp(a, b);
}

static KYTY_SYSV_ABI char* c_strcat(char* d, const char* s)
{
	return ::strcat(d, s);
}
static KYTY_SYSV_ABI char* c_strncat(char* d, const char* s, size_t n)
{
	return ::strncat(d, s, n);
}
static KYTY_SYSV_ABI char* c_strchr(const char* s, int c)
{
	return const_cast<char*>(::strchr(s, c));
}
static KYTY_SYSV_ABI char* c_strstr(const char* haystack, const char* needle)
{
	return const_cast<char*>(::strstr(haystack, needle));
}
static KYTY_SYSV_ABI char* c_getenv(const char* name)
{
	const char* value = ProcessEnvironment::GetEnvironmentVariable(name);
	return const_cast<char*>(value != nullptr ? value : (name != nullptr ? ::getenv(name) : nullptr));
}
static KYTY_SYSV_ABI char* c_setlocale(int category, const char* locale)
{
	return ::setlocale(category, locale);
}
static KYTY_SYSV_ABI unsigned __int128 c_udivti3(unsigned __int128 numerator, unsigned __int128 denominator)
{
	return numerator / denominator;
}
static KYTY_SYSV_ABI uint16_t* c_wcsstr(const uint16_t* haystack, const uint16_t* needle)
{
	if (haystack == nullptr || needle == nullptr)
	{
		return nullptr;
	}
	if (*needle == 0)
	{
		return const_cast<uint16_t*>(haystack);
	}

	for (auto* candidate = haystack; *candidate != 0; ++candidate)
	{
		const uint16_t* left  = candidate;
		const uint16_t* right = needle;
		while (*left != 0 && *right != 0 && *left == *right)
		{
			++left;
			++right;
		}
		if (*right == 0)
		{
			return const_cast<uint16_t*>(candidate);
		}
	}
	return nullptr;
}
static KYTY_SYSV_ABI int c_wcsncmp(const uint16_t* left, const uint16_t* right, size_t count)
{
	if (count == 0 || left == right)
	{
		return 0;
	}
	if (left == nullptr || right == nullptr)
	{
		return left == nullptr ? -1 : 1;
	}

	for (size_t index = 0; index < count; ++index)
	{
		if (left[index] != right[index])
		{
			return left[index] < right[index] ? -1 : 1;
		}
		if (left[index] == 0)
		{
			return 0;
		}
	}
	return 0;
}
// Helpers kept for pending NID registration; not yet bound via LIB_FUNC.
[[maybe_unused]] static KYTY_SYSV_ABI char* c_strrchr(const char* s, int c)
{
	return const_cast<char*>(::strrchr(s, c));
}
static KYTY_SYSV_ABI size_t c_strnlen(const char* s, size_t n)
{
	return ::strnlen(s, n);
}
static KYTY_SYSV_ABI void* c_aligned_alloc(size_t alignment, size_t size)
{
	return c_memalign(alignment, size);
}
static KYTY_SYSV_ABI int c_posix_memalign(void** memptr, size_t alignment, size_t size)
{
	if (memptr == nullptr)
	{
		return 22;
	}
	void* ptr = c_memalign(alignment, size);
	if (ptr == nullptr)
	{
		return 12;
	}
	*memptr = ptr;
	return 0;
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

// C++ operator new/delete (mangled _Znwm/_ZdlPv/_ZdaPv), same ownership as libc malloc.
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

// Gen5 libc NIDs iPBqs+YUUFw / 2HnmKiLmV6s — same SysV ABI from call sites:
//   lea 8(obj),%rdi; mov $expected,%esi; mov $desired,%edx; call; cmp $1,%eax
// Observed pairs: (ptr,1,0) then (ptr,1,4) on a 32-bit state word. Success
// returns 1 when *p matched expected (FreeBSD-style atomic_cmpset_int).
static KYTY_SYSV_ABI int c_atomic_cmpset_32(volatile uint32_t* p, uint32_t expected, uint32_t desired)
{
	if (p == nullptr)
	{
		return 0;
	}
	uint32_t cur = *p;
	if (cur == expected)
	{
		*p = desired;
		return 1;
	}
	return 0;
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

// --- ctype (musl/newlib style tables) ----------------------------------------
// The PS4 libc exposes _Getpctype/_Getptoupper returning pointers to internal
// classification tables indexed by (c+1). We return host-derived tables.
static KYTY_SYSV_ABI const unsigned short* c_Getpctype()
{
	static unsigned short table[384];
	static bool           init = false;
	if (!init)
	{
		for (int c = -1; c < 256; c++)
		{
			unsigned short m = 0;
			if (c >= 0)
			{
				if (::isupper(c)) m |= 0x01;
				if (::islower(c)) m |= 0x02;
				if (::isdigit(c)) m |= 0x04;
				if (::isspace(c)) m |= 0x08;
				if (::ispunct(c)) m |= 0x10;
				if (::iscntrl(c)) m |= 0x20;
				if (::isxdigit(c)) m |= 0x40;
				if (::isblank(c)) m |= 0x80;
				if (::isalpha(c)) m |= 0x100;
			}
			table[c + 1] = m;
		}
		init = true;
	}
	return table + 1;
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

// --- stdio (host FILE* passed back opaquely to the guest) --------------------
static KYTY_SYSV_ABI FILE* c_fopen(const char* path, const char* mode)
{
	if (path == nullptr)
	{
		return nullptr;
	}
	// Translate mounted guest paths (e.g. /app0/...) to real host paths.
	String host = LibKernel::FileSystem::GetRealFilename(String::FromUtf8(path));
	String use  = host.IsEmpty() ? String::FromUtf8(path) : host;
	FILE*  f    = ::fopen(use.C_Str(), mode);
	printf("\t fopen('%s' -> '%s', '%s') = %p\n", path, use.C_Str(), mode, static_cast<void*>(f));
	return f;
}
static KYTY_SYSV_ABI int c_fclose(FILE* f)
{
	return (f != nullptr) ? ::fclose(f) : 0;
}
static KYTY_SYSV_ABI size_t c_fread(void* p, size_t sz, size_t n, FILE* f)
{
	const size_t result = ::fread(p, sz, n, f);
	GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(p), result * sz);
	return result;
}
// Gen5 libc_v1 fgets — NID KdP-nULpuGw.
static KYTY_SYSV_ABI char* c_fgets(char* s, int n, FILE* f)
{
	if (s == nullptr || n <= 0 || f == nullptr)
	{
		return nullptr;
	}
	char* result = ::fgets(s, n, f);
	if (result != nullptr)
	{
		GpuMemoryNotifyHostWrite(reinterpret_cast<uint64_t>(s), std::strlen(s) + 1);
	}
	return result;
}
static KYTY_SYSV_ABI size_t c_fwrite(const void* p, size_t sz, size_t n, FILE* f)
{
	return ::fwrite(p, sz, n, f);
}
static KYTY_SYSV_ABI int c_fseek(FILE* f, long off, int w)
{
	return ::fseek(f, off, w);
}
static KYTY_SYSV_ABI long c_ftell(FILE* f)
{
	return ::ftell(f);
}
static KYTY_SYSV_ABI int c_feof(FILE* f)
{
	return ::feof(f);
}
static KYTY_SYSV_ABI int c_ferror(FILE* f)
{
	return ::ferror(f);
}
static KYTY_SYSV_ABI int c_fileno(FILE* f)
{
	if (f == nullptr)
	{
		return -1;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return ::_fileno(f);
#else
	return ::fileno(f);
#endif
}
static KYTY_SYSV_ABI int c_fputc(int ch, FILE* f)
{
	return ::fputc(ch, f);
}
static KYTY_SYSV_ABI int c_remove(const char* p)
{
	String host = LibKernel::FileSystem::GetRealFilename(String::FromUtf8(p));
	return ::remove((host.IsEmpty() ? String::FromUtf8(p) : host).C_Str());
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
static KYTY_SYSV_ABI int c_vsnprintf(char* s, size_t n, const char* fmt, VaList* ap)
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
	for (const char* cursor = format; *cursor != '\0'; cursor++)
	{
		if (*cursor != '%')
		{
			continue;
		}
		cursor++;
		if (*cursor == '%')
		{
			continue;
		}

		while (*cursor == '-' || *cursor == '+' || *cursor == ' ' || *cursor == '#' || *cursor == '0')
		{
			cursor++;
		}
		if (*cursor == '*')
		{
			cursor++;
		} else
		{
			while (std::isdigit(static_cast<unsigned char>(*cursor)) != 0)
			{
				cursor++;
			}
		}
		if (*cursor == '.')
		{
			cursor++;
			if (*cursor == '*')
			{
				cursor++;
			} else
			{
				while (std::isdigit(static_cast<unsigned char>(*cursor)) != 0)
				{
					cursor++;
				}
			}
		}
		if (*cursor == 'h' || *cursor == 'l')
		{
			const char length = *cursor++;
			if (*cursor == length)
			{
				cursor++;
			}
		} else if (*cursor == 't' || *cursor == 'j' || *cursor == 'z')
		{
			cursor++;
		}

		if (std::strchr("diuxXobfFeEgGcsp", *cursor) == nullptr)
		{
			return false;
		}
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

// --- time --------------------------------------------------------------------
static KYTY_SYSV_ABI int64_t c_time(int64_t* t)
{
	time_t r = ::time(nullptr);
	if (t) *t = r;
	return r;
}
static KYTY_SYSV_ABI int64_t c_mktime(struct tm* tmv)
{
	return ::mktime(tmv);
}
static KYTY_SYSV_ABI struct tm* c_gmtime(const int64_t* t)
{
	time_t v = *t;
	return ::gmtime(&v);
}
static KYTY_SYSV_ABI struct tm* c_localtime(const int64_t* t)
{
	time_t v = *t;
	return ::localtime(&v);
}
static KYTY_SYSV_ABI size_t c_strftime(char* s, size_t n, const char* f, const struct tm* tmv)
{
	return ::strftime(s, n, f, tmv);
}
// Gen5 libc_v1 asctime — NID jT3xiGpA3B4. Returns static host buffer (same ABI as C).
static KYTY_SYSV_ABI char* c_asctime(const struct tm* tmv)
{
	if (tmv == nullptr)
	{
		return nullptr;
	}
	return ::asctime(tmv);
}

// --- math (double) -----------------------------------------------------------
static KYTY_SYSV_ABI double c_sin(double x)
{
	return ::sin(x);
}
static KYTY_SYSV_ABI double c_cos(double x)
{
	return ::cos(x);
}
static KYTY_SYSV_ABI double c_tan(double x)
{
	return ::tan(x);
}
static KYTY_SYSV_ABI double c_asin(double x)
{
	return ::asin(x);
}
static KYTY_SYSV_ABI double c_acos(double x)
{
	return ::acos(x);
}
static KYTY_SYSV_ABI double c_atan(double x)
{
	return ::atan(x);
}
static KYTY_SYSV_ABI double c_atan2(double y, double x)
{
	return ::atan2(y, x);
}
static KYTY_SYSV_ABI double c_exp(double x)
{
	return ::exp(x);
}
static KYTY_SYSV_ABI double c_log(double x)
{
	return ::log(x);
}
static KYTY_SYSV_ABI double c_pow(double x, double y)
{
	return ::pow(x, y);
}
static KYTY_SYSV_ABI double c_fmod(double x, double y)
{
	return ::fmod(x, y);
}
static KYTY_SYSV_ABI double c_ceil(double x)
{
	return ::ceil(x);
}
static KYTY_SYSV_ABI double c_floor(double x)
{
	return ::floor(x);
}
static KYTY_SYSV_ABI double c_round(double x)
{
	return ::round(x);
}
static KYTY_SYSV_ABI double c_sqrt(double x)
{
	return ::sqrt(x);
}
static KYTY_SYSV_ABI double c_fabs(double x)
{
	return ::fabs(x);
}
static KYTY_SYSV_ABI double c_modf(double x, double* ip)
{
	return ::modf(x, ip);
}
static KYTY_SYSV_ABI double c_ldexp(double x, int e)
{
	return ::ldexp(x, e);
}
static KYTY_SYSV_ABI double c_frexp(double x, int* e)
{
	return ::frexp(x, e);
}
static KYTY_SYSV_ABI void c_sincos(double x, double* s, double* c)
{
	*s = ::sin(x);
	*c = ::cos(x);
}
// --- math (float) ------------------------------------------------------------
static KYTY_SYSV_ABI float c_powf(float x, float y)
{
	return ::powf(x, y);
}
// Gen5 libc_v1 __isnanf — NID lA94ZgT+vMM. Float in xmm0; non-zero if NaN.
// Observed Astro after pthread_self: call site loads float via vmovss then tests eax.
static KYTY_SYSV_ABI int c_isnanf(float x)
{
	return std::isnan(x) ? 1 : 0;
}
// Gen5 libc_v1 isfinite(double) — NID dhK16CKwhQg. Dreaming Sarah Construct
// number parser after strtod: store double, call, test %eax; non-zero keeps value.
// xmm0 = value; return non-zero when finite.
static KYTY_SYSV_ABI int c_isfinite(double x)
{
	return std::isfinite(x) ? 1 : 0;
}
static KYTY_SYSV_ABI int c_isnan(double x)
{
	return std::isnan(x) ? 1 : 0;
}
static KYTY_SYSV_ABI int c_isinf(double x)
{
	return std::isinf(x) ? 1 : 0;
}
// Gen5 libc_v1 sinf — NID Q4rRL34CEeE (Astro after usleep).
static KYTY_SYSV_ABI float c_sinf(float x)
{
	return ::sinf(x);
}
static KYTY_SYSV_ABI float c_cosf(float x)
{
	return ::cosf(x);
}
// Gen5 libc_v1 tanf — NID ZE6RNL+eLbk (Astro after Posix pthread_detach; float in xmm0).
static KYTY_SYSV_ABI float c_tanf(float x)
{
	return ::tanf(x);
}
// Gen5 libc_v1 inverse/extra float math (name→NID; import tables use '-' for '/').
static KYTY_SYSV_ABI float c_atanf(float x)
{
	return ::atanf(x);
}
static KYTY_SYSV_ABI float c_asinf(float x)
{
	return ::asinf(x);
}
static KYTY_SYSV_ABI float c_acosf(float x)
{
	return ::acosf(x);
}
static KYTY_SYSV_ABI float c_atan2f(float y, float x)
{
	return ::atan2f(y, x);
}
static KYTY_SYSV_ABI float c_fmodf(float x, float y)
{
	return ::fmodf(x, y);
}
static KYTY_SYSV_ABI float c_hypotf(float x, float y)
{
	return ::hypotf(x, y);
}
static KYTY_SYSV_ABI float c_truncf(float x)
{
	return ::truncf(x);
}
static KYTY_SYSV_ABI float c_roundf(float x)
{
	return ::roundf(x);
}
static KYTY_SYSV_ABI float c_log10f(float x)
{
	return ::log10f(x);
}
static KYTY_SYSV_ABI float c_logf(float x)
{
	return ::logf(x);
}
static KYTY_SYSV_ABI float c_sqrtf(float x)
{
	return ::sqrtf(x);
}
static KYTY_SYSV_ABI float c_fabsf(float x)
{
	return ::fabsf(x);
}
static KYTY_SYSV_ABI float c_floorf(float x)
{
	return ::floorf(x);
}
static KYTY_SYSV_ABI float c_ceilf(float x)
{
	return ::ceilf(x);
}
static KYTY_SYSV_ABI float c_log2f(float x)
{
	return ::log2f(x);
}
static KYTY_SYSV_ABI float c_exp2f(float x)
{
	return ::exp2f(x);
}
static KYTY_SYSV_ABI float c_expf(float x)
{
	return ::expf(x);
}
static KYTY_SYSV_ABI float c_ldexpf(float x, int e)
{
	return ::ldexpf(x, e);
}
static KYTY_SYSV_ABI void c_sincosf(float x, float* s, float* c)
{
	*s = ::sinf(x);
	*c = ::cosf(x);
}

// --- C++ runtime -------------------------------------------------------------
// One-time static-init guard. jmp_buf-free implementation over the guard byte.
static KYTY_SYSV_ABI int c_cxa_guard_acquire(uint64_t* g)
{
	auto* done = reinterpret_cast<volatile uint8_t*>(g);
	return (*done == 0) ? 1 : 0;
}
static KYTY_SYSV_ABI void c_cxa_guard_release(uint64_t* g)
{
	*reinterpret_cast<volatile uint8_t*>(g) = 1;
}
static KYTY_SYSV_ABI void c_cxa_guard_abort(uint64_t* g)
{
	*reinterpret_cast<volatile uint8_t*>(g) = 0;
}

using execute_once_callback_t = KYTY_SYSV_ABI int (*)(void*, void*, void**);

static std::mutex              g_execute_once_mutex;
static std::condition_variable g_execute_once_cv;

static KYTY_SYSV_ABI int c_execute_once(int* flag, execute_once_callback_t callback, void* context)
{
	PRINT_NAME();

	if (flag == nullptr || callback == nullptr)
	{
		return 0;
	}

	constexpr int once_uninitialized = 0;
	constexpr int once_complete      = 1;
	constexpr int once_running       = 2;

	{
		std::unique_lock lock(g_execute_once_mutex);
		g_execute_once_cv.wait(lock, [flag] { return *flag != once_running; });
		if (*flag == once_complete)
		{
			return 1;
		}
		*flag = once_running;
	}

	void* callback_result = nullptr;
	const int result      = callback(nullptr, context, &callback_result);

	{
		std::lock_guard lock(g_execute_once_mutex);
		*flag = result != 0 ? once_complete : once_uninitialized;
	}
	g_execute_once_cv.notify_all();

	return result;
}

static KYTY_SYSV_ABI int c_mtx_init(LibKernel::PthreadMutex* mutex, int type)
{
	PRINT_NAME();

	if (mutex == nullptr)
	{
		return static_cast<int>(CThreadResult::Error);
	}

	constexpr int mtx_recursive = 0x100;
	if ((type & mtx_recursive) == 0)
	{
		return c_thread_result(LibKernel::PthreadMutexInit(mutex, nullptr, nullptr));
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

	return c_thread_result(result);
}

static KYTY_SYSV_ABI int c_mtx_lock(LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return c_thread_result(LibKernel::PthreadMutexLock(mutex));
}

static KYTY_SYSV_ABI int c_mtx_unlock(LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return c_thread_result(LibKernel::PthreadMutexUnlock(mutex));
}

static KYTY_SYSV_ABI void c_Xout_of_range(const char* msg)
{
	EXIT("std::out_of_range: %s\n", msg != nullptr ? msg : "");
}
static KYTY_SYSV_ABI void c_Xlength_error(const char* msg)
{
	EXIT("std::length_error: %s\n", msg != nullptr ? msg : "");
}
static KYTY_SYSV_ABI void c_Xregex_error(int error_type)
{
	EXIT("std::regex_error: error_type=%d\n", error_type);
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

// Minimal facet object: vptr only (methods do not touch further fields).
struct CxxFacetStub
{
	void** vtable;
};

static CxxFacetStub g_ctype_facet {g_ctype_vtable};

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
static std::uint64_t g_locale_id_2   = 2;
static std::uint64_t g_locale_id_3   = 3;
static std::uint64_t g_locale_id_4   = 4;
static std::uint64_t g_locale_id_5   = 5;
static std::uint64_t g_locale_id_6   = 6;
static std::uint64_t g_locale_id_7   = 7;
static std::int32_t  g_locale_id_cnt = 8;
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
static void* g_system_error_vtable[8]       = KYTY_CXX_NOOP_VTBL;
static void* g_bad_cast_vtable[8]           = KYTY_CXX_NOOP_VTBL;
static void* g_ios_failure_vtable[8]        = KYTY_CXX_NOOP_VTBL;
static void* g_num_put_char_vtable[8]       = KYTY_CXX_NOOP_VTBL;
#undef KYTY_CXX_NOOP_VTBL

static const char g_ti_name_exception[]         = "St9exception";
static const char g_ti_name_domain_error[]      = "St12domain_error";
static const char g_ti_name_out_of_range[]      = "St12out_of_range";
static const char g_ti_name_runtime_error[]     = "St13runtime_error";
static const char g_ti_name_invalid_argument[]  = "St16invalid_argument";
static const char g_ti_name_bad_cast[]          = "St8bad_cast";
static const char g_ti_name_ios_base[]          = "St8ios_base";
static const char g_ti_name_ios_failure[]       = "NSt8ios_base7failureE";

static CxxSiTypeInfoLayout g_typeinfo_exception {g_si_class_type_info_vtable, g_ti_name_exception, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_domain_error {g_si_class_type_info_vtable, g_ti_name_domain_error, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_out_of_range {g_si_class_type_info_vtable, g_ti_name_out_of_range, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_runtime_error {g_si_class_type_info_vtable, g_ti_name_runtime_error, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_invalid_argument {g_si_class_type_info_vtable, g_ti_name_invalid_argument, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_bad_cast {g_si_class_type_info_vtable, g_ti_name_bad_cast, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_ios_base {g_si_class_type_info_vtable, g_ti_name_ios_base, nullptr};
static CxxSiTypeInfoLayout g_typeinfo_ios_failure {g_si_class_type_info_vtable, g_ti_name_ios_failure, nullptr};

// streamoff sentinel and fpz (common libc++/MSVC objects; zero-safe).
static std::int64_t g_bad_off = -1;
static std::uint8_t g_fpz[16] {};

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

// std::ios_base::~ios_base() — guest tears down temporary stream objects after
// locale/ctype probes. No host side-effects required for the stub ios_base.
static KYTY_SYSV_ABI void c_ios_base_dtor(void* /*self*/) {}

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

	::printf("func = %" PRIx64 "\n", reinterpret_cast<uint64_t>(func));

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

	::printf("return from main = %d\n", status);
}

static KYTY_SYSV_ABI int cxa_atexit(void (*func)(void*), void* arg, void* d)
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

namespace LibcInternalExt {

LIB_VERSION("LibcInternalExt", 1, "LibcInternal", 1, 1);

static uint64_t g_mspace_atomic_id_mask = 0;
static uint64_t g_mstate_table[64]      = {0};

struct Info
{
	uint64_t  size;
	uint32_t  unknown1;
	uint32_t  unknown2;
	uint64_t* mspace_atomic_id_mask;
	uint64_t* mstate_table;
};

void KYTY_SYSV_ABI LibcHeapGetTraceInfo(Info* info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(info->size != 32);

	info->mspace_atomic_id_mask = &g_mspace_atomic_id_mask;
	info->mstate_table          = g_mstate_table;
}

LIB_DEFINE(InitLibcInternalExt_1)
{
	LIB_FUNC("NWtTN10cJzE", LibcInternalExt::LibcHeapGetTraceInfo);
}

} // namespace LibcInternalExt

namespace LibcInternal {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

// Same contract as LibC::g_need_flag — request CRT heap/TSD bootstrap.
static uint32_t g_need_flag = 1;

int KYTY_SYSV_ABI vprintf(const char* str, VaList* c)
{
	PRINT_NAME();

	return GetVprintfFunc()(str, c);
}

static KYTY_SYSV_ABI int snprintf(VA_ARGS)
{
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	PRINT_NAME();

	return GetSnrintfCtxFunc()(&ctx);
}

int KYTY_SYSV_ABI fflush(FILE* stream)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(stream != stdout && stream != stderr);

	return ::fflush(stream);
}

void* KYTY_SYSV_ABI memset(void* s, int c, size_t n)
{
	PRINT_NAME();

	return ::memset(s, c, n);
}

void* KYTY_SYSV_ABI LibcMspaceCreate(const char* name, void* base, size_t capacity, uint32_t flag)
{
	PRINT_NAME();

	// Gen5 heap paths may omit a name; treat null as empty diagnostic tag.
	const char* mspace_name = (name != nullptr ? name : "");

	printf("\t name     = %s\n", mspace_name);
	printf("\t base     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(base));
	printf("\t capacity = %016" PRIx64 "\n", capacity);
	printf("\t flag     = %u\n", flag);

	EXIT_NOT_IMPLEMENTED(flag != 0 && flag != 1);
	EXIT_NOT_IMPLEMENTED(base == nullptr);
	EXIT_NOT_IMPLEMENTED(capacity == 0);

	bool thread_safe = true;

	if (flag == 1)
	{
		thread_safe = false;
	}

	auto* msp = Core::MSpaceCreate(mspace_name, base, capacity, thread_safe, nullptr);

	EXIT_NOT_IMPLEMENTED(msp == nullptr);

	return msp;
}

void* KYTY_SYSV_ABI LibcMspaceMalloc(void* msp, size_t size)
{
	PRINT_NAME();

	printf("\t msp  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(msp));
	printf("\t size = %016" PRIx64 "\n", size);

	// Guest libc returns nullptr on failure (OOM / null mspace); do not EXIT —
	// callers are expected to check the return. Strict abort here blocked the
	// runtime before any presentation window after an early null msp malloc(0x28).
	if (msp == nullptr)
	{
		printf("\t buf  = 0000000000000000 (null mspace)\n");
		return nullptr;
	}

	auto* buf = Core::MSpaceMalloc(msp, size);

	printf("\t buf  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(buf));

	return buf;
}

void* KYTY_SYSV_ABI LibcMspaceMemalign(void* msp, size_t align, size_t size)
{
	PRINT_NAME();

	if (msp == nullptr)
	{
		return nullptr;
	}

	return Core::MSpaceMemalign(msp, align, size);
}

// sceLibcMspaceCalloc — NID LYo3GhIlB38 (msp, nelem, size). Observed (msp, 1, 0x40).
void* KYTY_SYSV_ABI LibcMspaceCalloc(void* msp, size_t nelem, size_t size)
{
	PRINT_NAME();
	printf("\t msp   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(msp));
	printf("\t nelem = 0x%016" PRIx64 "\n", static_cast<uint64_t>(nelem));
	printf("\t size  = 0x%016" PRIx64 "\n", static_cast<uint64_t>(size));
	if (msp == nullptr)
	{
		return nullptr;
	}
	return Core::MSpaceCalloc(msp, nelem, size);
}

// Gen5 sceLibcMspaceMallocStatsFast — NID k04jLXu3+Ic.
// Guest structure is SceLibcMallocManagedSize (size/version 0x00010028):
//   +0x00 u16 size=0x28, u16 version=1  (stored as u32 0x00010028)
//   +0x04 u32 reserved
//   +0x08 size_t maxSystemSize
//   +0x10 size_t currentSystemSize   // Astro Onion pre-check: need <= this
//   +0x18 size_t maxInuseSize
//   +0x20 size_t currentInuseSize
// Stack frame places the block at rbp-0x48 with the canary at rbp-0x20, so only
// 0x28 bytes are writable before the canary.
struct LibcMallocManagedSize
{
	uint32_t size_version;
	uint32_t reserved;
	uint64_t max_system_size;
	uint64_t current_system_size;
	uint64_t max_inuse_size;
	uint64_t current_inuse_size;
};
static_assert(sizeof(LibcMallocManagedSize) == 0x28, "SceLibcMallocManagedSize");

int KYTY_SYSV_ABI LibcMspaceMallocStatsFast(void* msp, void* stats)
{
	PRINT_NAME();
	printf("\t msp   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(msp));
	printf("\t stats = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(stats));
	if (msp == nullptr || stats == nullptr)
	{
		return -1;
	}

	Core::MSpaceSize sizes {};
	if (!Core::MSpaceMallocStatsFast(msp, &sizes))
	{
		return -1;
	}

	auto* out                = static_cast<LibcMallocManagedSize*>(stats);
	out->size_version        = 0x00010028u;
	out->reserved            = 0;
	out->max_system_size     = sizes.max_system_size;
	out->current_system_size = sizes.current_system_size;
	out->max_inuse_size      = sizes.max_inuse_size;
	out->current_inuse_size  = sizes.current_inuse_size;
	printf("\t system = 0x%016" PRIx64 " inuse = 0x%016" PRIx64 "\n", out->current_system_size, out->current_inuse_size);
	return 0;
}

// libc malloc_stats_fast — NID KuOuD58hqn4.  The public replacement-table
// ABI is int(void*), unlike the internal mspace form above.  During bootstrap
// there is no application-heap handle; report the host allocator snapshot.
int KYTY_SYSV_ABI LibcMallocStatsFast(void* stats)
{
	PRINT_NAME();
	if (stats == nullptr)
	{
		return -1;
	}

	if (LibKernel::ApplicationHeap::HasMallocStatsFast())
	{
		return LibKernel::ApplicationHeap::MallocStatsFast(stats);
	}
	if (LibKernel::ApplicationHeap::IsInitialized())
	{
		return -1;
	}

	Core::MSpaceSize sizes {};
	LibC::collect_host_malloc_stats(&sizes);

	auto* out                = static_cast<LibcMallocManagedSize*>(stats);
	out->size_version        = 0x00010028u;
	out->reserved            = 0;
	out->max_system_size     = sizes.max_system_size;
	out->current_system_size = sizes.current_system_size;
	out->max_inuse_size      = sizes.max_inuse_size;
	out->current_inuse_size  = sizes.current_inuse_size;
	return 0;
}

void KYTY_SYSV_ABI LibcMspaceFree(void* msp, void* ptr)
{
	PRINT_NAME();

	if (msp == nullptr || ptr == nullptr)
	{
		return;
	}

	Core::MSpaceFree(msp, ptr);
}

LIB_DEFINE(InitLibcInternal_1)
{
	LibcInternalExt::InitLibcInternalExt_1(s);

	LIB_OBJECT("ZT4ODD2Ts9o", &LibcInternal::g_need_flag);
	// stdin Object triad: guest import tables list 1TDo-ImqkJc immediately before
	// the registered stdout NID 2sWzhYqFH4E and stderr H8AprKeZtNg (libc_v1).
	LIB_OBJECT("1TDo-ImqkJc", stdin);
	LIB_OBJECT("2sWzhYqFH4E", stdout);

	LIB_FUNC("GMpvxPFW924", LibcInternal::vprintf);
	LIB_FUNC("MUjC4lbHrK4", LibcInternal::fflush);
	LIB_FUNC("8zTFvBIAIN8", LibcInternal::memset);
	LIB_FUNC("eLdDw6l0-bU", LibcInternal::snprintf);
	LIB_FUNC("Q2V+iqvjgC0", LibC::c_vsnprintf);

	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);
	LIB_FUNC("DiGVep5yB5w", LibC::c_execute_once);
	LIB_FUNC("YaHc3GS7y7g", LibC::c_mtx_init);
	LIB_FUNC("iS4aWbUonl0", LibC::c_mtx_lock);
	LIB_FUNC("gTuXQwP9rrs", LibC::c_mtx_unlock);
	LIB_FUNC("VsP3daJgmVA", LibC::c_cnd_broadcast);

	LIB_FUNC("-hn1tcVHq5Q", LibcInternal::LibcMspaceCreate);
	LIB_FUNC("OJjm-QOIHlI", LibcInternal::LibcMspaceMalloc);
	LIB_FUNC("iF1iQHzxBJU", LibcInternal::LibcMspaceMemalign);
	LIB_FUNC("LYo3GhIlB38", LibcInternal::LibcMspaceCalloc);
	LIB_FUNC("Vla-Z+eXlxo", LibcInternal::LibcMspaceFree);
	LIB_FUNC("k04jLXu3+Ic", LibcInternal::LibcMspaceMallocStatsFast);
}

} // namespace LibcInternal

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

	// C++ locale / RTTI objects — Dreaming Sarah (Qoo175Ig+-k → classic locale).
	// dynlib: _ZSt21_sceLibcClassicLocale, ctype<char>::id, locale::id::_Id_cnt,
	// __class_type_info / __si_class_type_info / __vmi_class_type_info vtables.
	LIB_OBJECT("Qoo175Ig+-k", &LibC::g_sce_classic_locale);
	LIB_OBJECT("Cv+zC4EjGMA", &LibC::g_ctype_char_id);
	LIB_OBJECT("H4fcpQOpc08", &LibC::g_locale_id_cnt);
	// Facet ::id Objects — eboot imports (Ps5Nid / ps5_names).
	LIB_OBJECT("VmqsS6auJzo", &LibC::g_ctype_wchar_id);
	LIB_OBJECT("irGo1yaJ-vM", &LibC::g_collate_wchar_id);
	LIB_OBJECT("E14mW8pVpoE", &LibC::g_num_put_char_id);
	LIB_OBJECT("byV+FWlAnB4", LibC::g_class_type_info_vtable);
	LIB_OBJECT("pZ9WXcClPO8", LibC::g_si_class_type_info_vtable);
	LIB_OBJECT("9ByRMdo7ywg", LibC::g_vmi_class_type_info_vtable);
	LIB_OBJECT("dCzeFfg9WWI", LibC::g_exception_vtable);
	// Exception / iostream RTTI Objects — eboot libc_v1 imports.
	// Prefer typed type_info/vtable Objects over generic locale-id/dummy placeholders
	// for the same NIDs (unit-tested domain_error layout).
	LIB_OBJECT("5BIbzIuDxTQ", &LibC::g_typeinfo_domain_error);
	LIB_OBJECT("n2kx+OmFUis", &LibC::g_typeinfo_exception);
	LIB_OBJECT("dKjhNUf9FBc", &LibC::g_typeinfo_out_of_range);
	LIB_OBJECT("bLPn1gfqSW8", &LibC::g_typeinfo_runtime_error);
	LIB_OBJECT("XZzWt0ygWdw", &LibC::g_typeinfo_invalid_argument);
	LIB_OBJECT("qOD-ksTkE08", &LibC::g_typeinfo_bad_cast);
	LIB_OBJECT("BJCgW9-OxLA", &LibC::g_typeinfo_ios_base);
	LIB_OBJECT("sBCTjFk7Gi4", &LibC::g_typeinfo_ios_failure);
	LIB_OBJECT("oAidKrxuUv0", LibC::g_domain_error_vtable);
	LIB_OBJECT("udTM6Nxx-Ng", LibC::g_logic_error_vtable);
	LIB_OBJECT("n+aUKkC-3sI", LibC::g_out_of_range_vtable);
	LIB_OBJECT("-L+-8F0+gBc", LibC::g_runtime_error_vtable);
	LIB_OBJECT("keXoyW-rV-0", LibC::g_invalid_argument_vtable);
	LIB_OBJECT("Bq8m04PN1zw", LibC::g_system_error_vtable);
	LIB_OBJECT("tVHE+C8vGXk", LibC::g_bad_cast_vtable);
	LIB_OBJECT("yLE5H3058Ao", LibC::g_ios_failure_vtable);
	LIB_OBJECT("1kZFcktOm+s", LibC::g_num_put_char_vtable);
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
	LIB_FUNC("QxqK-IdpumU", LibC::c_Getpmbstate);
	LIB_FUNC("zS94yyJRSUs", LibC::c_Getpwcstate);
	LIB_FUNC("-9SIhUr4Iuo", LibC::c_Mbtowcx);
	LIB_FUNC("stv1S3BKfgw", LibC::c_Wctombx);
	LIB_OBJECT("2wz4rthdiy8", &LibC::g_dummy_obj_17);
	LIB_FUNC("UWyL6KoR96U", LibC::c_Xregex_error);
	LIB_OBJECT("HUbZmOnT-Dg", &LibC::g_dummy_obj_21);
	LIB_OBJECT("Y6Sl4Xw7gfA", &LibC::g_dummy_obj_23);
	LIB_OBJECT("apPZ6HKZWaQ", &LibC::g_dummy_obj_24);
	LIB_OBJECT("BgZcGDh7o9g", &LibC::g_dummy_obj_25);
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
	// std::ios_base::~ios_base — P8F2oavZXtY after interactive presents start.
	LIB_FUNC("P8F2oavZXtY", LibC::c_ios_base_dtor);

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
	LIB_FUNC("iS4aWbUonl0", LibC::c_mtx_lock);
	LIB_FUNC("gTuXQwP9rrs", LibC::c_mtx_unlock);

	// Standard C allocation uses the guest application heap after it is ready.
	LIB_FUNC("gQX+4GDQjpM", LibC::c_malloc);
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
	LIB_FUNC("VsP3daJgmVA", LibC::c_cnd_broadcast);
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
	LIB_FUNC("Ls4tzzhimqQ", LibC::c_strcat);
	LIB_FUNC("kHg45qPC6f0", LibC::c_strncat);
	LIB_FUNC("ob5xAW4ln-0", LibC::c_strchr);
	LIB_FUNC("9yDWMxEFdJU", LibC::c_strrchr);
	// Gen5 libc_v1 strstr.
	LIB_FUNC("viiwFMaNamA", LibC::c_strstr);
	LIB_FUNC("WDpobjImAb4", LibC::c_wcsstr);
	LIB_FUNC("E8wCoUEbfzk", LibC::c_wcsncmp);
	LIB_FUNC("fJnpuVVBbKk", LibC::cxx_new);         // operator new(size_t)
	LIB_FUNC("ryUxD-60bKM", LibC::cxx_new_nothrow); // operator new(size_t, const std::nothrow_t&)
	// Gen5 libc_v1 — public NID cfAXurvfl5o is __cxa_allocate_exception (not
	// operator new). Mis-binding it to cxx_new corrupts the throw path.
	LIB_FUNC("cfAXurvfl5o", LibC::cxa_allocate_exception);
	LIB_FUNC("MQFPAqQPt1s", LibC::cxa_decrement_exception_refcount);
	LIB_FUNC("z+P+xCnWLBk", LibC::cxx_delete); // operator delete(void*)
	// Gen5 libc_v1 C++ EH — Dreaming Sarah throw path after flip/init.
	// vkuuLfhnSZI: __cxa_throw (rdi=obj, rsi=typeinfo, rdx=dtor; ud2 after).
	LIB_FUNC("vkuuLfhnSZI", LibC::cxa_throw);
	LIB_FUNC("hdm0YfMa7TQ", LibC::cxx_new_array);         // operator new[](size_t)
	LIB_FUNC("Jh5qUcwiSEk", LibC::cxx_new_array_nothrow); // operator new[](size_t, const std::nothrow_t&)
	LIB_FUNC("MLWl90SFWNE", LibC::cxx_delete_array);      // operator delete[](void*)
	LIB_FUNC("iPBqs+YUUFw", LibC::c_atomic_cmpset_32);
	LIB_FUNC("2HnmKiLmV6s", LibC::c_atomic_cmpset_32);

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
	LIB_FUNC("rQFVBXp-Cxg", LibC::c_fseek);
	LIB_FUNC("Qazy8LmXTvw", LibC::c_ftell);
	LIB_FUNC("LxcEU+ICu8U", LibC::c_feof);
	LIB_FUNC("AHxyhN96dy4", LibC::c_ferror);
	LIB_FUNC("Fm-dmyywH9Q", LibC::c_fileno);
	LIB_FUNC("aZK8lNei-Qw", LibC::c_fputc);
	LIB_FUNC("MZO7FXyAPU8", LibC::c_remove);

	// printf / scanf family
	LIB_FUNC("eLdDw6l0-bU", LibC::c_snprintf);
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
	LIB_FUNC("wLlFkwG9UcQ", LibC::c_time);
	LIB_FUNC("QZP6I9ZZxpE", LibC::c_clock);
	LIB_FUNC("n7AepwR0s34", LibC::c_mktime);
	LIB_FUNC("1mecP7RgI2A", LibC::c_gmtime);
	LIB_FUNC("efhK-YSUYYQ", LibC::c_localtime);
	LIB_FUNC("Av3zjWi64Kw", LibC::c_strftime);
	// Gen5 libc asctime (NID jT3xiGpA3B4) — hard-aborted Astro without STUB_MISSING.
	LIB_FUNC("jT3xiGpA3B4", LibC::c_asctime);

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
