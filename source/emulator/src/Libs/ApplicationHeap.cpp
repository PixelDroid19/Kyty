#include "Emulator/Libs/ApplicationHeap.h"

#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Core/DbgAssert.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Loader/GuestCall.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <thread>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::ApplicationHeap {

namespace {

using MallocFunc = void*(KYTY_SYSV_ABI*)(size_t);
using FreeFunc   = void(KYTY_SYSV_ABI*)(void*);
using StatsFunc  = int(KYTY_SYSV_ABI*)(void*);

struct RuntimeApi
{
	MallocFunc malloc = nullptr;
	FreeFunc free = nullptr;
	StatsFunc stats_fast = nullptr;
};
std::mutex g_api_mutex;
RuntimeApi g_api;

RuntimeApi GetApi()
{
	std::lock_guard lock(g_api_mutex);
	return g_api;
}

static thread_local bool g_in_guest_allocator = false;

enum class StartupState { Empty, Initializing, Ready };
std::mutex   g_startup_mutex;
StartupState g_startup_state = StartupState::Empty;
uint64_t     g_startup_parameters = 0;
std::thread::id g_startup_thread;
std::condition_variable g_startup_changed;

// Only the common, size-gated prefixes are read. The replacement record is
// distinct from the direct kernel API: its header precedes init/fini and the
// ten callbacks; version 2 appends aligned_alloc.
struct ProcessParametersPrefix
{
	uint64_t size;
	uint32_t magic;
	uint32_t version;
	uint64_t reserved[5];
	uint64_t libc_parameters;
};
struct LibcParametersPrefix
{
	uint64_t size;
	uint64_t reserved[5];
	uint64_t malloc_replace;
};
struct MallocReplacement
{
	uint64_t size;
	uint64_t version;
	uint64_t initialize;
	uint64_t finalize;
	uint64_t callbacks[kApiSlotCount];
};
static_assert(sizeof(ProcessParametersPrefix) == 0x40);
static_assert(sizeof(LibcParametersPrefix) == 0x38);
static_assert(sizeof(MallocReplacement) == 0x70);

template <typename T> bool ReadRecord(uint64_t address, T* record)
{
	return Core::VirtualMemory::CopyFromGuest(record, address, sizeof(T)) && record->size >= sizeof(T) &&
	       Core::VirtualMemory::IsRangeReadable(address, record->size);
}

bool ValidCallback(uint64_t address)
{
	return address == 0 || (Core::VirtualMemory::IsRangeGuestOwned(address, 1) &&
	                        Emulator::GuestRuntimePort::IsExecutableAddress(address));
}

thread_local char g_failure_reason[192] = "";

bool Fail(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	std::vsnprintf(g_failure_reason, sizeof(g_failure_reason), format, args);
	va_end(args);
	return false;
}

class AllocatorCallbackScope
{
public:
	AllocatorCallbackScope(): m_previous(g_in_guest_allocator) { g_in_guest_allocator = true; }
	~AllocatorCallbackScope() { g_in_guest_allocator = m_previous; }

	AllocatorCallbackScope(const AllocatorCallbackScope&)            = delete;
	AllocatorCallbackScope& operator=(const AllocatorCallbackScope&) = delete;

private:
	bool m_previous;
};

} // namespace

bool IsValidApi(const Api* api)
{
	return api != nullptr && api->slots[kMallocSlot] != nullptr && api->slots[kFreeSlot] != nullptr;
}

void RegisterApi(void* const api[kApiSlotCount])
{
	const auto* table = reinterpret_cast<const Api*>(api);
	std::lock_guard lock(g_api_mutex);
	if (!IsValidApi(table))
	{
		g_api = {};
		return;
	}

	g_api.malloc     = reinterpret_cast<MallocFunc>(table->slots[kMallocSlot]);
	g_api.free       = reinterpret_cast<FreeFunc>(table->slots[kFreeSlot]);
	g_api.stats_fast = reinterpret_cast<StatsFunc>(table->slots[kMallocStatsFastSlot]);
}

bool InitializeProcessHeap(uint64_t process_parameters)
{
	g_failure_reason[0] = '\0';
	if (process_parameters == 0) { return true; }
	ProcessParametersPrefix process {};
	if (!ReadRecord(process_parameters, &process) || process.magic != 0x4942524f || process.version == 0)
	{
		return Fail("process parameters at 0x%llx are unreadable or malformed (size 0x%llx magic 0x%x version %u)",
		            static_cast<unsigned long long>(process_parameters), static_cast<unsigned long long>(process.size),
		            static_cast<unsigned>(process.magic), static_cast<unsigned>(process.version));
	}
	if (process.libc_parameters == 0) { return true; }
	LibcParametersPrefix libc {};
	if (!ReadRecord(process.libc_parameters, &libc))
	{
		return Fail("libc parameters at 0x%llx are unreadable (size 0x%llx)", static_cast<unsigned long long>(process.libc_parameters),
		            static_cast<unsigned long long>(libc.size));
	}
	if (libc.malloc_replace == 0) { return true; }
	MallocReplacement replacement {};
	if (!ReadRecord(libc.malloc_replace, &replacement) ||
	    !((replacement.version == 1 && replacement.size == 0x70) ||
	      (replacement.version == 2 && replacement.size == 0x78)))
	{
		return Fail("malloc replacement at 0x%llx has an unsupported layout (size 0x%llx version %llu)",
		            static_cast<unsigned long long>(libc.malloc_replace), static_cast<unsigned long long>(replacement.size),
		            static_cast<unsigned long long>(replacement.version));
	}
	if (!ValidCallback(replacement.initialize) || !ValidCallback(replacement.finalize))
	{
		return Fail("malloc replacement initialize 0x%llx or finalize 0x%llx is not guest code",
		            static_cast<unsigned long long>(replacement.initialize), static_cast<unsigned long long>(replacement.finalize));
	}
	Api api {};
	for (size_t slot = 0; slot < kApiSlotCount; ++slot)
	{
		if (!ValidCallback(replacement.callbacks[slot]))
		{
			return Fail("malloc replacement slot %zu = 0x%llx is not guest code", slot,
			            static_cast<unsigned long long>(replacement.callbacks[slot]));
		}
		api.slots[slot] = reinterpret_cast<void*>(replacement.callbacks[slot]);
	}
	bool empty = replacement.initialize == 0 && replacement.finalize == 0;
	for (const auto callback: replacement.callbacks) { empty = empty && callback == 0; }
	if (replacement.version == 2)
	{
		uint64_t aligned_alloc = 0;
		if (!Core::VirtualMemory::CopyFromGuest(&aligned_alloc, libc.malloc_replace + sizeof(replacement), sizeof(aligned_alloc)) ||
		    !ValidCallback(aligned_alloc))
		{
			return Fail("malloc replacement aligned_alloc 0x%llx is unreadable or not guest code",
			            static_cast<unsigned long long>(aligned_alloc));
		}
		empty = empty && aligned_alloc == 0;
	}
	// Public CRTs emit a default record whose callbacks are all null.
	if (empty) { return true; }
	if (!IsValidApi(&api)) { return Fail("malloc replacement lacks malloc or free"); }
	{
		std::unique_lock lock(g_startup_mutex);
		if (g_startup_state == StartupState::Initializing && g_startup_thread == std::this_thread::get_id())
		{
			return g_startup_parameters == process_parameters ||
			       Fail("re-entered with different process parameters 0x%llx", static_cast<unsigned long long>(process_parameters));
		}
		g_startup_changed.wait(lock, [] { return g_startup_state != StartupState::Initializing; });
		if (g_startup_state != StartupState::Empty)
		{
			return (g_startup_parameters == process_parameters && g_startup_state == StartupState::Ready) ||
			       Fail("an earlier startup with parameters 0x%llx is not ready", static_cast<unsigned long long>(g_startup_parameters));
		}
		g_startup_parameters = process_parameters;
		g_startup_state = StartupState::Initializing;
		g_startup_thread = std::this_thread::get_id();
	}
	// Do not hold a host lock across guest code, or publish partially initialized
	// callbacks. Main-image constructors remain exclusively owned by its CRT.
	// The initializer result is not a failure signal: a shipped title declares
	// initialize as a bare `ret`, leaving the callback address in rax.
	if (replacement.initialize != 0)
	{
		(void)Emulator::GuestRuntimePort::Invoke(replacement.initialize, 0, 0, 0);
	}
	std::lock_guard lock(g_startup_mutex);
	g_startup_thread = {};
	RegisterApi(api.slots);
	g_startup_state = StartupState::Ready;
	g_startup_changed.notify_all();
	return true;
}

const char* ProcessHeapFailureReason()
{
	return g_failure_reason;
}

bool IsInitialized()
{
	const auto api = GetApi();
	return api.malloc != nullptr && api.free != nullptr;
}

bool HasAllocator()
{
	return IsInitialized() && !g_in_guest_allocator;
}

bool IsAllocatorCallbackActive()
{
	return g_in_guest_allocator;
}

bool HasMallocStatsFast()
{
	const auto api = GetApi();
	return !g_in_guest_allocator && api.malloc != nullptr && api.free != nullptr && api.stats_fast != nullptr;
}

void* Malloc(size_t size)
{
	const auto api = GetApi();
	if (g_in_guest_allocator || api.malloc == nullptr || api.free == nullptr)
	{
		return nullptr;
	}

	AllocatorCallbackScope scope;
	const uint64_t         ptr = Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(api.malloc), size, 0, 0);
	return reinterpret_cast<void*>(ptr);
}

int MallocStatsFast(void* stats)
{
	const auto api = GetApi();
	if (g_in_guest_allocator || api.malloc == nullptr || api.free == nullptr || api.stats_fast == nullptr || stats == nullptr)
	{
		return -1;
	}

	AllocatorCallbackScope scope;
	const int result = static_cast<int>(Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(api.stats_fast),
	                                                              reinterpret_cast<uint64_t>(stats), 0, 0));
	return result;
}

bool Free(void* ptr)
{
	if (ptr == nullptr)
	{
		return true;
	}

	const auto api = GetApi();
	if (g_in_guest_allocator || api.malloc == nullptr || api.free == nullptr)
	{
		return false;
	}

	AllocatorCallbackScope scope;
	Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(api.free), reinterpret_cast<uint64_t>(ptr), 0, 0);
	return true;
}

void Reset()
{
	std::unique_lock lock(g_startup_mutex);
	EXIT_IF(g_startup_state == StartupState::Initializing && g_startup_thread == std::this_thread::get_id());
	g_startup_changed.wait(lock, [] { return g_startup_state != StartupState::Initializing; });
	g_startup_state       = StartupState::Empty;
	g_startup_parameters  = 0;
	g_startup_thread     = {};
	RegisterApi(nullptr);
	g_in_guest_allocator = false;
}

} // namespace Kyty::Libs::LibKernel::ApplicationHeap

#endif // KYTY_EMU_ENABLED
