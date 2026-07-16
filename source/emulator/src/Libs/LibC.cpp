#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/LinkList.h"
#include "Kyty/Core/MSpace.h"
#include "Kyty/Core/Singleton.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Printf.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>

// setjmp/longjmp: must NOT be wrapped in a C++ function (the wrapper frame would
// be gone by the time longjmp fires). Kyty runs guest code natively, so we bind
// the guest NIDs straight to the host implementations.
extern "C" int  _setjmp(void*);
extern "C" void _longjmp(void*, int);

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace LibC {

LIB_VERSION("libc", 1, "libc", 1, 1);

// Gen5 libc/libSceLibcInternal "need" flag: 0 means already initialized so the
// guest skips redundant CRT bootstrap. 1 re-enters init and was observed to
// leave application globals half-built after ApplicationHeap create.
static uint32_t g_need_flag = 0;

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

static KYTY_SYSV_ABI void init_env()
{
	PRINT_NAME();
}

// Standard C library forwarded to the host runtime. The guest's SysV x86-64 ABI
// matches the host's, so these thin wrappers are correct. Used when the game runs
// against Kyty's HLE "libc" instead of its own libc.prx.
static KYTY_SYSV_ABI void*  c_malloc(size_t size) { return ::malloc(size); }
static KYTY_SYSV_ABI void*  c_calloc(size_t n, size_t size) { return ::calloc(n, size); }

struct AlignedAllocationHeader
{
	uint64_t magic;
	void*    base;
	size_t   size;
	size_t   alignment;
};

static constexpr uint64_t ALIGNED_ALLOCATION_MAGIC = 0x4b595459414c4e47ull;

static AlignedAllocationHeader* aligned_header(void* ptr)
{
	if (ptr == nullptr)
	{
		return nullptr;
	}

	auto* header = reinterpret_cast<AlignedAllocationHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(AlignedAllocationHeader));
	return (header->magic == ALIGNED_ALLOCATION_MAGIC ? header : nullptr);
}

static KYTY_SYSV_ABI void* c_memalign(size_t alignment, size_t size)
{
	if (alignment < alignof(void*) || (alignment & (alignment - 1)) != 0 ||
	    size > SIZE_MAX - alignment - sizeof(AlignedAllocationHeader))
	{
		return nullptr;
	}

	const size_t total = size + alignment - 1 + sizeof(AlignedAllocationHeader);
	void*       base  = ::malloc(total);
	if (base == nullptr)
	{
		return nullptr;
	}

	const auto raw     = reinterpret_cast<uintptr_t>(base) + sizeof(AlignedAllocationHeader);
	const auto aligned = (raw + alignment - 1) & ~(alignment - 1);
	auto*      header  = reinterpret_cast<AlignedAllocationHeader*>(aligned - sizeof(AlignedAllocationHeader));
	*header            = {ALIGNED_ALLOCATION_MAGIC, base, size, alignment};
	return reinterpret_cast<void*>(aligned);
}

static KYTY_SYSV_ABI void* c_realloc(void* p, size_t size)
{
	if (auto* header = aligned_header(p); header != nullptr)
	{
		if (size == 0)
		{
			::free(header->base);
			return nullptr;
		}

		void* replacement = c_memalign(header->alignment, size);
		if (replacement != nullptr)
		{
			::memcpy(replacement, p, (header->size < size ? header->size : size));
			::free(header->base);
		}
		return replacement;
	}
	return ::realloc(p, size);
}

static KYTY_SYSV_ABI void c_free(void* p)
{
	if (auto* header = aligned_header(p); header != nullptr)
	{
		::free(header->base);
		return;
	}
	::free(p);
}
static KYTY_SYSV_ABI void*  c_memcpy(void* d, const void* s, size_t n) { return ::memcpy(d, s, n); }
static KYTY_SYSV_ABI int    c_memcpy_s(void* d, size_t dn, const void* s, size_t n) { return (::memcpy(d, s, n < dn ? n : dn), 0); }
static KYTY_SYSV_ABI void*  c_memmove(void* d, const void* s, size_t n) { return ::memmove(d, s, n); }
static KYTY_SYSV_ABI void*  c_memset(void* d, int c, size_t n) { return ::memset(d, c, n); }
static KYTY_SYSV_ABI int    c_memcmp(const void* a, const void* b, size_t n) { return ::memcmp(a, b, n); }
static KYTY_SYSV_ABI void*  c_memchr(const void* s, int c, size_t n) { return const_cast<void*>(::memchr(s, c, n)); }
static KYTY_SYSV_ABI size_t c_strlen(const char* s) { return ::strlen(s); }
static KYTY_SYSV_ABI char*  c_strcpy(char* d, const char* s) { return ::strcpy(d, s); }
static KYTY_SYSV_ABI char*  c_strncpy(char* d, const char* s, size_t n) { return ::strncpy(d, s, n); }
static KYTY_SYSV_ABI int    c_strcmp(const char* a, const char* b) { return ::strcmp(a, b); }
static KYTY_SYSV_ABI int    c_strncmp(const char* a, const char* b, size_t n) { return ::strncmp(a, b, n); }
// NID AV6ipCNa4Rw (public Gen5 tables / SharpEmu map). Null args are UB on host
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

// NID 1uJgoVq3bQU — name not yet in public tables. Captured SysV after fopen(~INDEX):
// rdi=object with embedded name at +8 ("data.js"), rsi=guest side-buffer, rcx=0x64.
// Guest treats the return as a non-null object pointer. Writing into rsi as a raw
// n-byte buffer regressed to an earlier null crash (rsi is not a plain peek buf).
// Return rdi only — same continue path as the non-null missing-func stub return.
static KYTY_SYSV_ABI void* c_1uJgoVq3bQU(void* obj, void* /*buf*/, void* /*a2*/, uint64_t /*n*/, void* /*a4*/, uint64_t /*a5*/)
{
	return obj;
}
static KYTY_SYSV_ABI char*  c_strcat(char* d, const char* s) { return ::strcat(d, s); }
static KYTY_SYSV_ABI char*  c_strchr(const char* s, int c) { return const_cast<char*>(::strchr(s, c)); }
// Helpers kept for pending NID registration; not yet bound via LIB_FUNC.
[[maybe_unused]] static KYTY_SYSV_ABI char*  c_strrchr(const char* s, int c)
{
	return const_cast<char*>(::strrchr(s, c));
}
[[maybe_unused]] static KYTY_SYSV_ABI size_t c_strnlen(const char* s, size_t n) { return ::strnlen(s, n); }
static KYTY_SYSV_ABI void c_srand(unsigned int seed) { ::srand(seed); }
// Gen5 libc_v1 rand (Nmtr628eA3A): first Unpatched after Global Heap create.
static KYTY_SYSV_ABI int c_rand() { return ::rand(); }
// Gen5 libc_v1 strtok (oVkZ8W8-Q8A): host uses strtok_r with a per-thread save pointer.
static KYTY_SYSV_ABI char* c_strtok(char* str, const char* delim)
{
	static thread_local char* save = nullptr;
	return ::strtok_r(str, delim, &save);
}

// C++ operator new/delete (mangled _Znwm/_ZdlPv), forwarded to the host allocator.
static KYTY_SYSV_ABI void* cxx_new(size_t size) { return ::malloc(size != 0 ? size : 1); }
static KYTY_SYSV_ABI void  cxx_delete(void* p) { ::free(p); }
static KYTY_SYSV_ABI void* cxx_new_array(size_t size) { return ::malloc(size != 0 ? size : 1); }

// --- Additional string / memory ---------------------------------------------
static KYTY_SYSV_ABI int   c_bcmp(const void* a, const void* b, size_t n) { return ::memcmp(a, b, n); }
static KYTY_SYSV_ABI char* c_strerror(int e) { return ::strerror(e); }
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
	static bool init = false;
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
	static bool init = false;
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
static KYTY_SYSV_ABI int      c_fclose(FILE* f) { return (f != nullptr) ? ::fclose(f) : 0; }
static KYTY_SYSV_ABI size_t   c_fread(void* p, size_t sz, size_t n, FILE* f) { return ::fread(p, sz, n, f); }
static KYTY_SYSV_ABI size_t   c_fwrite(const void* p, size_t sz, size_t n, FILE* f) { return ::fwrite(p, sz, n, f); }
static KYTY_SYSV_ABI int      c_fseek(FILE* f, long off, int w) { return ::fseek(f, off, w); }
static KYTY_SYSV_ABI long     c_ftell(FILE* f) { return ::ftell(f); }
static KYTY_SYSV_ABI int      c_feof(FILE* f) { return ::feof(f); }
static KYTY_SYSV_ABI int      c_ferror(FILE* f) { return ::ferror(f); }
static KYTY_SYSV_ABI int      c_fputc(int ch, FILE* f) { return ::fputc(ch, f); }
static KYTY_SYSV_ABI int      c_remove(const char* p)
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
static KYTY_SYSV_ABI int c_sprintf(VA_ARGS)
{
	VA_CONTEXT(ctx);
	char*       s   = VaArg_ptr<char>(&ctx.va_list);
	const char* fmt = VaArg_ptr<const char>(&ctx.va_list);
	return Format(s, C_UNBOUNDED_FORMAT, fmt, &ctx.va_list);
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
static KYTY_SYSV_ABI int c_vsprintf(char* s, const char* fmt, VaList* ap)
{
	return Format(s, C_UNBOUNDED_FORMAT, fmt, ap);
}
static KYTY_SYSV_ABI int c_vsnprintf(char* s, size_t n, const char* fmt, VaList* ap)
{
	return Format(s, n, fmt, ap);
}
static KYTY_SYSV_ABI int c_vsnprintf_s(char* s, size_t dn, size_t count, const char* fmt, VaList* ap)
{
	size_t n = (count + 1 < dn) ? count + 1 : dn;
	return Format(s, n, fmt, ap);
}

// --- stdlib ------------------------------------------------------------------
static KYTY_SYSV_ABI double c_strtod(const char* s, char** e) { return ::strtod(s, e); }
static KYTY_SYSV_ABI long   c_strtol(const char* s, char** e, int b) { return ::strtol(s, e, b); }
static KYTY_SYSV_ABI double c_atof(const char* s) { return ::atof(s); }
static KYTY_SYSV_ABI void   c_qsort(void* base, size_t n, size_t sz, int (KYTY_SYSV_ABI* cmp)(const void*, const void*))
{
	::qsort(base, n, sz, reinterpret_cast<int (*)(const void*, const void*)>(cmp));
}
static KYTY_SYSV_ABI void   c_abort()
{
	printf("libc::abort() called by guest\n");
	::abort();
}

// --- time --------------------------------------------------------------------
static KYTY_SYSV_ABI int64_t    c_time(int64_t* t) { time_t r = ::time(nullptr); if (t) *t = r; return r; }
static KYTY_SYSV_ABI int64_t    c_mktime(struct tm* tmv) { return ::mktime(tmv); }
static KYTY_SYSV_ABI struct tm* c_gmtime(const int64_t* t) { time_t v = *t; return ::gmtime(&v); }
static KYTY_SYSV_ABI struct tm* c_localtime(const int64_t* t) { time_t v = *t; return ::localtime(&v); }
static KYTY_SYSV_ABI size_t     c_strftime(char* s, size_t n, const char* f, const struct tm* tmv) { return ::strftime(s, n, f, tmv); }

// --- math (double) -----------------------------------------------------------
static KYTY_SYSV_ABI double c_sin(double x) { return ::sin(x); }
static KYTY_SYSV_ABI double c_cos(double x) { return ::cos(x); }
static KYTY_SYSV_ABI double c_tan(double x) { return ::tan(x); }
static KYTY_SYSV_ABI double c_asin(double x) { return ::asin(x); }
static KYTY_SYSV_ABI double c_acos(double x) { return ::acos(x); }
static KYTY_SYSV_ABI double c_atan(double x) { return ::atan(x); }
static KYTY_SYSV_ABI double c_atan2(double y, double x) { return ::atan2(y, x); }
static KYTY_SYSV_ABI double c_exp(double x) { return ::exp(x); }
static KYTY_SYSV_ABI double c_log(double x) { return ::log(x); }
static KYTY_SYSV_ABI double c_pow(double x, double y) { return ::pow(x, y); }
static KYTY_SYSV_ABI double c_fmod(double x, double y) { return ::fmod(x, y); }
static KYTY_SYSV_ABI double c_modf(double x, double* ip) { return ::modf(x, ip); }
static KYTY_SYSV_ABI double c_ldexp(double x, int e) { return ::ldexp(x, e); }
static KYTY_SYSV_ABI double c_frexp(double x, int* e) { return ::frexp(x, e); }
static KYTY_SYSV_ABI void   c_sincos(double x, double* s, double* c) { *s = ::sin(x); *c = ::cos(x); }
// --- math (float) ------------------------------------------------------------
static KYTY_SYSV_ABI float  c_powf(float x, float y) { return ::powf(x, y); }
static KYTY_SYSV_ABI float  c_log2f(float x) { return ::log2f(x); }
static KYTY_SYSV_ABI float  c_exp2f(float x) { return ::exp2f(x); }
static KYTY_SYSV_ABI float  c_expf(float x) { return ::expf(x); }
static KYTY_SYSV_ABI float  c_ldexpf(float x, int e) { return ::ldexpf(x, e); }
static KYTY_SYSV_ABI void   c_sincosf(float x, float* s, float* c) { *s = ::sinf(x); *c = ::cosf(x); }

// --- C++ runtime -------------------------------------------------------------
// One-time static-init guard. jmp_buf-free implementation over the guard byte.
static KYTY_SYSV_ABI int  c_cxa_guard_acquire(uint64_t* g)
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
static KYTY_SYSV_ABI void c_Xout_of_range(const char* msg)
{
	EXIT("std::out_of_range: %s\n", msg != nullptr ? msg : "");
}
static KYTY_SYSV_ABI void c_Xlength_error(const char* msg)
{
	EXIT("std::length_error: %s\n", msg != nullptr ? msg : "");
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

// Same contract as LibC::g_need_flag — already-initialized, skip CRT re-entry.
static uint32_t g_need_flag = 0;

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

	EXIT_NOT_IMPLEMENTED(stream != stdout);

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

	printf("\t name     = %s\n", name);
	printf("\t base     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(base));
	printf("\t capacity = %016" PRIx64 "\n", capacity);
	printf("\t flag     = %u\n", flag);

	EXIT_NOT_IMPLEMENTED(flag != 0 && flag != 1);
	EXIT_NOT_IMPLEMENTED(name == nullptr);
	EXIT_NOT_IMPLEMENTED(base == nullptr);
	EXIT_NOT_IMPLEMENTED(capacity == 0);

	bool thread_safe = true;

	if (flag == 1)
	{
		thread_safe = false;
	}

	auto* msp = Core::MSpaceCreate(name, base, capacity, thread_safe, nullptr);

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
	LIB_OBJECT("2sWzhYqFH4E", stdout);

	LIB_FUNC("GMpvxPFW924", LibcInternal::vprintf);
	LIB_FUNC("MUjC4lbHrK4", LibcInternal::fflush);
	LIB_FUNC("8zTFvBIAIN8", LibcInternal::memset);
	LIB_FUNC("eLdDw6l0-bU", LibcInternal::snprintf);
	LIB_FUNC("Q2V+iqvjgC0", LibC::c_vsnprintf);

	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);

	LIB_FUNC("-hn1tcVHq5Q", LibcInternal::LibcMspaceCreate);
	LIB_FUNC("OJjm-QOIHlI", LibcInternal::LibcMspaceMalloc);
	LIB_FUNC("iF1iQHzxBJU", LibcInternal::LibcMspaceMemalign);
	LIB_FUNC("Vla-Z+eXlxo", LibcInternal::LibcMspaceFree);
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
	LIB_OBJECT("2sWzhYqFH4E", stdout);

	LIB_FUNC("uMei1W9uyNo", LibC::exit);
	LIB_FUNC("bzQExy189ZI", LibC::init_env);
	LIB_FUNC("8G2LB+A3rzg", LibC::atexit);
	LIB_FUNC("hcuQgD53UxM", LibC::libc_printf);
	LIB_FUNC("MUjC4lbHrK4", LibcInternal::fflush);
	LIB_FUNC("YQ0navp+YIc", LibC::puts);
	// Captured Gen5 after DirNameSearch/strtol: rdi=formatted log line
	// with trailing CR/LF, rsi=stream-like pointer — fputs ABI.
	LIB_FUNC("QrZZdJ8XsX0", LibC::c_fputs);
	LIB_FUNC("XKRegsFpEpk", LibC::catchReturnFromMain);
	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);

	// Standard C library, forwarded to the host runtime.
	LIB_FUNC("gQX+4GDQjpM", LibC::c_malloc);
	LIB_FUNC("2X5agFjKxMc", LibC::c_calloc);
	LIB_FUNC("Y7aJ1uydPMo", LibC::c_realloc);
	LIB_FUNC("tIhsqj0qsFE", LibC::c_free);
	LIB_FUNC("Ujf3KzMvRmI", LibC::c_memalign);
	LIB_FUNC("Q3VBxCXhUHs", LibC::c_memcpy);
	LIB_FUNC("NFLs+dRJGNg", LibC::c_memcpy_s);
	LIB_FUNC("8zTFvBIAIN8", LibC::c_memset);
	LIB_FUNC("DfivPArhucg", LibC::c_memcmp);
	LIB_FUNC("j4ViWNHEgww", LibC::c_strlen);
	LIB_FUNC("6sJWiWSRuqk", LibC::c_strncpy);
	// Captured Gen5 boot after SaveDataInitialize3: 3-arg call with dest buffer,
	// "SAVEDATA00" src, n=0x20 — same ABI as strncpy (second NID for same export).
	LIB_FUNC("SfQIZcqvvms", LibC::c_strncpy);
	LIB_FUNC("Ovb2dSJOAuE", LibC::c_strcmp);
	LIB_FUNC("aesyjrHVWy4", LibC::c_strncmp);
	// sceLibc strcasecmp — NID AV6ipCNa4Rw
	LIB_FUNC("AV6ipCNa4Rw", LibC::c_strcasecmp);
	// Captured Gen5 post-~INDEX open: object+"data.js"; non-null return advances.
	LIB_FUNC("1uJgoVq3bQU", LibC::c_1uJgoVq3bQU);
	LIB_FUNC("Ls4tzzhimqQ", LibC::c_strcat);
	LIB_FUNC("ob5xAW4ln-0", LibC::c_strchr);
	LIB_FUNC("fJnpuVVBbKk", LibC::cxx_new);    // operator new(size_t)
	LIB_FUNC("z+P+xCnWLBk", LibC::cxx_delete); // operator delete(void*)
	LIB_FUNC("hdm0YfMa7TQ", LibC::cxx_new_array);    // operator new[](size_t)

	// string / memory
	LIB_FUNC("+P6FRGH4LfA", LibC::c_memmove);
	LIB_FUNC("8u8lPzUEq+U", LibC::c_memchr);
	LIB_FUNC("5TjaJwkLWxE", LibC::c_bcmp);
	LIB_FUNC("kiZSXIWd9vg", LibC::c_strcpy);
	LIB_FUNC("RIa6GnWp+iU", LibC::c_strerror);
	LIB_FUNC("YNzNkJzYqEg", LibC::c_strncpy_s);

	// ctype
	LIB_FUNC("sUP1hBaouOw", LibC::c_Getpctype);
	LIB_FUNC("rcQCUr0EaRU", LibC::c_Getptoupper);

	// stdio
	LIB_FUNC("xeYO4u7uyJ0", LibC::c_fopen);
	LIB_FUNC("uodLYyUip20", LibC::c_fclose);
	LIB_FUNC("lbB+UlZqVG0", LibC::c_fread);
	LIB_FUNC("MpxhMh8QFro", LibC::c_fwrite);
	LIB_FUNC("rQFVBXp-Cxg", LibC::c_fseek);
	LIB_FUNC("Qazy8LmXTvw", LibC::c_ftell);
	LIB_FUNC("LxcEU+ICu8U", LibC::c_feof);
	LIB_FUNC("AHxyhN96dy4", LibC::c_ferror);
	LIB_FUNC("aZK8lNei-Qw", LibC::c_fputc);
	LIB_FUNC("MZO7FXyAPU8", LibC::c_remove);

	// printf / scanf family
	LIB_FUNC("eLdDw6l0-bU", LibC::c_snprintf);
	LIB_FUNC("Q2V+iqvjgC0", LibC::c_vsnprintf); // vsnprintf (Gen5 libc_v1)
	LIB_FUNC("tcVi5SivF7Q", LibC::c_sprintf);
	LIB_FUNC("fffwELXNVFA", LibC::c_fprintf);
	LIB_FUNC("1Pk0qZQGeWo", LibC::c_sscanf);
	LIB_FUNC("jbz9I9vkqkk", LibC::c_vsprintf);
	LIB_FUNC("rWSuTWY2JN0", LibC::c_vsnprintf_s);

	// stdlib
	LIB_FUNC("2vDqwBlpF-o", LibC::c_strtod);
	LIB_FUNC("mXlxhmLNMPg", LibC::c_strtol);
	// Captured Gen5 after SaveDataDirNameSearch: SysV (char* "00", endptr=null, base=10)
	// — same ABI as strtol (second NID for same export).
	LIB_FUNC("zlfEH8FmyUA", LibC::c_strtol);
	LIB_FUNC("SRI6S9B+-a4", LibC::c_atof);
	LIB_FUNC("AEJdIVZTEmo", LibC::c_qsort);
	LIB_FUNC("L1SBTkC+Cvw", LibC::c_abort);
	LIB_FUNC("VPbJwTCgME0", LibC::c_srand);
	// Gen5 libc_v1 rand / strtok — evidenced first imports after global heap initialization.
	LIB_FUNC("Nmtr628eA3A", LibC::c_rand);
	LIB_FUNC("oVkZ8W8-Q8A", LibC::c_strtok);

	// time
	LIB_FUNC("wLlFkwG9UcQ", LibC::c_time);
	LIB_FUNC("n7AepwR0s34", LibC::c_mktime);
	LIB_FUNC("1mecP7RgI2A", LibC::c_gmtime);
	LIB_FUNC("efhK-YSUYYQ", LibC::c_localtime);
	LIB_FUNC("Av3zjWi64Kw", LibC::c_strftime);

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
	LIB_FUNC("0WMHDb5Dt94", LibC::c_modf);
	LIB_FUNC("JrwFIMzKNr0", LibC::c_ldexp);
	LIB_FUNC("kA-TdiOCsaY", LibC::c_frexp);
	LIB_FUNC("jMB7EFyu30Y", LibC::c_sincos);
	// math (float)
	LIB_FUNC("1D0H2KNjshE", LibC::c_powf);
	LIB_FUNC("hsi9drzHR2k", LibC::c_log2f);
	LIB_FUNC("wuAQt-j+p4o", LibC::c_exp2f);
	LIB_FUNC("8zsu04XNsZ4", LibC::c_expf);
	LIB_FUNC("kn0yiYeExgA", LibC::c_ldexpf);
	LIB_FUNC("pztV4AF18iI", LibC::c_sincosf);

	// C++ runtime
	LIB_FUNC("3GPpjQdAMTw", LibC::c_cxa_guard_acquire);
	LIB_FUNC("9rAeANT2tyE", LibC::c_cxa_guard_release);
	LIB_FUNC("2emaaluWzUw", LibC::c_cxa_guard_abort);
	LIB_FUNC("ozMAr28BwSY", LibC::c_Xout_of_range);
	LIB_FUNC("tQIo+GIPklo", LibC::c_Xlength_error);

	// setjmp / longjmp (bound directly to host — no C++ wrapper)
	LIB_FUNC("gNQ1V2vfXDE", _setjmp);
	LIB_FUNC("lKEN2IebgJ0", _longjmp);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
