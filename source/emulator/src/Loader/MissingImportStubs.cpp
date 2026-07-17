#include "Emulator/Loader/MissingImportStubs.h"

#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader::MissingImport {
namespace {

// Static, compiled stubs — required under Rosetta (no runtime-generated pages).
using stub_fn_t = KYTY_SYSV_ABI int64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

struct Slot
{
	char        name[96]     = {};
	char        library[64]  = {};
	char        module[64]   = {};
	int         library_version      = 0;
	int         module_version_major = 0;
	int         module_version_minor = 0;
	SymbolType  type                 = SymbolType::Func;
	bool        used                 = false;
	std::atomic<uint64_t> call_count {0};
	std::atomic<uint64_t> arg0 {0};
	std::atomic<uint64_t> arg1 {0};
	std::atomic<uint64_t> arg2 {0};
	std::atomic<uint64_t> arg3 {0};
	std::atomic<uint64_t> arg4 {0};
	std::atomic<uint64_t> arg5 {0};
	bool        logged_first_call = false;
};

Slot                    g_slots[kMaxSlots];
std::atomic<int>        g_next {0};
Core::Mutex             g_mutex;
std::atomic<uint64_t>   g_total_calls {0};

// Scratch page + callable for permissive return (preserve prior ABI: return a
// real function pointer address usable as data or code).
static KYTY_SYSV_ABI int64_t kyty_missing_callable()
{
	return 0;
}

static uint64_t stub_scratch_addr()
{
	static uint64_t addr = Core::VirtualMemory::Alloc(0, 0x10000, Core::VirtualMemory::Mode::ReadWrite);
	return addr;
}

static KYTY_SYSV_ABI int64_t stub_return_value()
{
	const uint64_t scratch = stub_scratch_addr();
	if (scratch != 0)
	{
		reinterpret_cast<uint64_t*>(scratch)[0] = reinterpret_cast<uint64_t>(kyty_missing_callable);
	}
	// Preserve prior permissive return: a callable address (not zero).
	return static_cast<int64_t>(reinterpret_cast<uint64_t>(kyty_missing_callable));
}

template <int I>
static KYTY_SYSV_ABI int64_t stub_impl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	Slot& s = g_slots[I];
	s.arg0.store(a0, std::memory_order_relaxed);
	s.arg1.store(a1, std::memory_order_relaxed);
	s.arg2.store(a2, std::memory_order_relaxed);
	s.arg3.store(a3, std::memory_order_relaxed);
	s.arg4.store(a4, std::memory_order_relaxed);
	s.arg5.store(a5, std::memory_order_relaxed);
	s.call_count.fetch_add(1, std::memory_order_relaxed);
	g_total_calls.fetch_add(1, std::memory_order_relaxed);
	Core::BringUp::NoteMissingImportCalled();

	if (!s.logged_first_call)
	{
		// Best-effort once; races may double-print, never corrupt.
		s.logged_first_call = true;
		std::printf("CALLED missing stub: %s [%s] module=%s\n", s.name, s.library, s.module);
		std::fflush(stdout);
	}
	return stub_return_value();
}

template <int... Is>
static constexpr void fill_table(stub_fn_t* t, std::integer_sequence<int, Is...> /*seq*/)
{
	((t[Is] = &stub_impl<Is>), ...);
}

static stub_fn_t* table()
{
	static stub_fn_t t[kMaxSlots];
	static bool      init = false;
	if (!init)
	{
		fill_table(t, std::make_integer_sequence<int, kMaxSlots> {});
		init = true;
	}
	return t;
}

static bool IdentityEqual(const Slot& s, const SymbolResolve& sr)
{
	return s.used && s.library_version == sr.library_version && s.module_version_major == sr.module_version_major &&
	       s.module_version_minor == sr.module_version_minor && s.type == sr.type &&
	       std::strncmp(s.name, sr.name.C_Str(), sizeof(s.name)) == 0 &&
	       std::strncmp(s.library, sr.library.C_Str(), sizeof(s.library)) == 0 &&
	       std::strncmp(s.module, sr.module.C_Str(), sizeof(s.module)) == 0;
}

static void CopyField(char* dst, size_t cap, const String& src)
{
	const char* p = src.C_Str();
	if (p == nullptr)
	{
		dst[0] = '\0';
		return;
	}
	std::snprintf(dst, cap, "%s", p);
}

} // namespace

uint64_t Assign(const SymbolResolve& sr)
{
	EXIT_IF(sr.type != SymbolType::Func);

	Core::LockGuard lock(g_mutex);

	// Deduplicate: same identity → same address.
	const int used = g_next.load(std::memory_order_relaxed);
	for (int i = 0; i < used; ++i)
	{
		if (IdentityEqual(g_slots[i], sr))
		{
			return reinterpret_cast<uint64_t>(table()[i]);
		}
	}

	if (used >= kMaxSlots)
	{
		std::fprintf(stderr,
		             "KYTY_BRINGUP: missing-import stub capacity exhausted (%d); aborting (no anonymous fallback)\n",
		             kMaxSlots);
		std::fflush(stderr);
		EXIT("missing-import stub capacity exhausted\n");
	}

	Slot& s = g_slots[used];
	CopyField(s.name, sizeof(s.name), sr.name);
	CopyField(s.library, sizeof(s.library), sr.library);
	CopyField(s.module, sizeof(s.module), sr.module);
	s.library_version      = sr.library_version;
	s.module_version_major = sr.module_version_major;
	s.module_version_minor = sr.module_version_minor;
	s.type                 = sr.type;
	s.used                 = true;
	s.call_count.store(0, std::memory_order_relaxed);
	s.logged_first_call = false;

	g_next.store(used + 1, std::memory_order_relaxed);
	Core::BringUp::NoteMissingImportAssigned();
	Core::BringUp::NoteMissingImportSlots(static_cast<uint32_t>(used + 1));

	std::printf("STUB (missing): %s [%s] module=%s\n", s.name, s.library, s.module);
	std::fflush(stdout);

	return reinterpret_cast<uint64_t>(table()[used]);
}

int UsedSlots()
{
	return g_next.load(std::memory_order_relaxed);
}

bool FindByIdentity(const SymbolResolve& sr, SlotInfo* out)
{
	if (out == nullptr)
	{
		return false;
	}
	Core::LockGuard lock(g_mutex);
	const int       used = g_next.load(std::memory_order_relaxed);
	for (int i = 0; i < used; ++i)
	{
		if (IdentityEqual(g_slots[i], sr))
		{
			const Slot& s         = g_slots[i];
			out->name             = s.name;
			out->library          = s.library;
			out->library_version  = s.library_version;
			out->module           = s.module;
			out->module_version_major = s.module_version_major;
			out->module_version_minor = s.module_version_minor;
			out->type             = s.type;
			out->call_count       = s.call_count.load(std::memory_order_relaxed);
			out->last_args[0]     = s.arg0.load(std::memory_order_relaxed);
			out->last_args[1]     = s.arg1.load(std::memory_order_relaxed);
			out->last_args[2]     = s.arg2.load(std::memory_order_relaxed);
			out->last_args[3]     = s.arg3.load(std::memory_order_relaxed);
			out->last_args[4]     = s.arg4.load(std::memory_order_relaxed);
			out->last_args[5]     = s.arg5.load(std::memory_order_relaxed);
			out->vaddr            = reinterpret_cast<uint64_t>(table()[i]);
			return true;
		}
	}
	return false;
}

uint64_t TotalCalls()
{
	return g_total_calls.load(std::memory_order_relaxed);
}

void ResetForTests()
{
	Core::LockGuard lock(g_mutex);
	const int       used = g_next.load(std::memory_order_relaxed);
	for (int i = 0; i < used; ++i)
	{
		Slot& s = g_slots[i];
		s.name[0]     = '\0';
		s.library[0]  = '\0';
		s.module[0]   = '\0';
		s.library_version      = 0;
		s.module_version_major = 0;
		s.module_version_minor = 0;
		s.type                 = SymbolType::Func;
		s.used                 = false;
		s.call_count.store(0, std::memory_order_relaxed);
		s.arg0.store(0, std::memory_order_relaxed);
		s.arg1.store(0, std::memory_order_relaxed);
		s.arg2.store(0, std::memory_order_relaxed);
		s.arg3.store(0, std::memory_order_relaxed);
		s.arg4.store(0, std::memory_order_relaxed);
		s.arg5.store(0, std::memory_order_relaxed);
		s.logged_first_call = false;
	}
	g_next.store(0, std::memory_order_relaxed);
	g_total_calls.store(0, std::memory_order_relaxed);
}

} // namespace Kyty::Loader::MissingImport

#endif // KYTY_EMU_ENABLED
