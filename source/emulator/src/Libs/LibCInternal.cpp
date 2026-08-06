#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MSpace.h"
#include "Kyty/Core/Singleton.h"
#include "Kyty/Core/String.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/ApplicationHeap.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Printf.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Log.h"
#include "LibCInternal.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace LibcInternal {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

// Same contract as LibC::g_need_flag — request CRT heap/TSD bootstrap.
uint32_t g_need_flag = 1;

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

	if (stream != stdout && stream != stderr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

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

	KYTY_LOG_DEBUG("\t name     = %s\n", mspace_name);
	KYTY_LOG_DEBUG("\t base     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(base));
	KYTY_LOG_DEBUG("\t capacity = %016" PRIx64 "\n", capacity);
	KYTY_LOG_DEBUG("\t flag     = %u\n", flag);

	if (flag != 0 && flag != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (base == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (capacity == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	bool thread_safe = true;

	if (flag == 1)
	{
		thread_safe = false;
	}

	auto* msp = Core::MSpaceCreate(mspace_name, base, capacity, thread_safe, nullptr);

	if (msp == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	return msp;
}

void* KYTY_SYSV_ABI LibcMspaceMalloc(void* msp, size_t size)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t msp  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(msp));
	KYTY_LOG_DEBUG("\t size = %016" PRIx64 "\n", size);

	// Guest libc returns nullptr on failure (OOM / null mspace); do not EXIT —
	// callers are expected to check the return. Strict abort here blocked the
	// runtime before any presentation window after an early null msp malloc(0x28).
	if (msp == nullptr)
	{
		KYTY_LOG_DEBUG("\t buf  = 0000000000000000 (null mspace)\n");
		return nullptr;
	}

	auto* buf = Core::MSpaceMalloc(msp, size);

	KYTY_LOG_DEBUG("\t buf  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(buf));

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

size_t KYTY_SYSV_ABI LibcMspaceMallocUsableSize(const void* ptr)
{
	PRINT_NAME();

	return Core::MSpaceMallocUsableSize(ptr);
}

// sceLibcMspaceCalloc — NID LYo3GhIlB38 (msp, nelem, size). Observed (msp, 1, 0x40).
void* KYTY_SYSV_ABI LibcMspaceCalloc(void* msp, size_t nelem, size_t size)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t msp   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(msp));
	KYTY_LOG_DEBUG("\t nelem = 0x%016" PRIx64 "\n", static_cast<uint64_t>(nelem));
	KYTY_LOG_DEBUG("\t size  = 0x%016" PRIx64 "\n", static_cast<uint64_t>(size));
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
	KYTY_LOG_DEBUG("\t msp   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(msp));
	KYTY_LOG_DEBUG("\t stats = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(stats));
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
	KYTY_LOG_DEBUG("\t system = 0x%016" PRIx64 " inuse = 0x%016" PRIx64 "\n", out->current_system_size, out->current_inuse_size);
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
	LIB_FUNC("tgioGpKtmbE", LibC::c_mtx_init_with_name);
	LIB_FUNC("JHp7ogc1+HY", LibC::c_mtx_init_with_default_name_override);
	LIB_FUNC("5Lf51jvohTQ", LibC::c_mtx_destroy);
	LIB_FUNC("iS4aWbUonl0", LibC::c_mtx_lock);
	LIB_FUNC("k6pGNMwJB08", LibC::c_mtx_trylock);
	LIB_FUNC("hPzYSd5Nasc", LibC::c_mtx_timedlock);
	LIB_FUNC("gTuXQwP9rrs", LibC::c_mtx_unlock);
	LIB_FUNC("VYQwFs4CC4Y", LibC::c_mtx_current_owns);
	LIB_FUNC("SreZybSRWpU", LibC::c_cnd_init);
	LIB_FUNC("2B+V3qCqz4s", LibC::c_cnd_init_with_name);
	LIB_FUNC("jBOZAv6CwkM", LibC::c_cnd_init_with_default_name_override);
	LIB_FUNC("VsP3daJgmVA", LibC::c_cnd_broadcast);
	LIB_FUNC("7yMFgcS8EPA", LibC::c_cnd_destroy);
	LIB_FUNC("0uuqgRz9qfo", LibC::c_cnd_signal);
	LIB_FUNC("McaImWKXong", LibC::c_cnd_timedwait);
	LIB_FUNC("vEaqE-7IZYc", LibC::c_cnd_wait);
	LIB_FUNC("DV2AdZFFEh8", LibC::c_cnd_register_at_thread_exit);
	LIB_FUNC("wpuIiVoCWcM", LibC::c_cnd_unregister_at_thread_exit);
	LIB_FUNC("vyLotuB6AS4", LibC::c_cnd_do_broadcast_at_thread_exit);
	LIB_FUNC("+fAmL52-yfQ", LibC::c_cnd_register_at_thread_exit);
	LIB_FUNC("SwcNvp-Af6c", LibC::c_cnd_unregister_at_thread_exit);
	LIB_FUNC("2s6aLdPIA4I", LibC::c_cnd_do_broadcast_at_thread_exit);

	LIB_FUNC("-hn1tcVHq5Q", LibcInternal::LibcMspaceCreate);
	LIB_FUNC("OJjm-QOIHlI", LibcInternal::LibcMspaceMalloc);
	LIB_FUNC("iF1iQHzxBJU", LibcInternal::LibcMspaceMemalign);
	LIB_FUNC("fEoW6BJsPt4", LibcInternal::LibcMspaceMallocUsableSize);
	LIB_FUNC("LYo3GhIlB38", LibcInternal::LibcMspaceCalloc);
	LIB_FUNC("Vla-Z+eXlxo", LibcInternal::LibcMspaceFree);
	LIB_FUNC("k04jLXu3+Ic", LibcInternal::LibcMspaceMallocStatsFast);
}

} // namespace LibcInternal

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
