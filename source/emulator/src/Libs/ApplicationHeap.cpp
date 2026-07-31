#include "Emulator/Libs/ApplicationHeap.h"

#include "Emulator/Loader/GuestCall.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::ApplicationHeap {

namespace {

using MallocFunc = void*(KYTY_SYSV_ABI*)(size_t);
using FreeFunc   = void(KYTY_SYSV_ABI*)(void*);
using StatsFunc  = int(KYTY_SYSV_ABI*)(void*);

static MallocFunc g_malloc     = nullptr;
static FreeFunc   g_free       = nullptr;
static StatsFunc  g_stats_fast = nullptr;

static thread_local bool g_in_guest_allocator = false;

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
	if (!IsValidApi(table))
	{
		g_malloc     = nullptr;
		g_free       = nullptr;
		g_stats_fast = nullptr;
		return;
	}

	g_malloc     = reinterpret_cast<MallocFunc>(table->slots[kMallocSlot]);
	g_free       = reinterpret_cast<FreeFunc>(table->slots[kFreeSlot]);
	g_stats_fast = reinterpret_cast<StatsFunc>(table->slots[kMallocStatsFastSlot]);
}

bool IsInitialized()
{
	return g_malloc != nullptr && g_free != nullptr;
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
	return HasAllocator() && g_stats_fast != nullptr;
}

void* Malloc(size_t size)
{
	if (!HasAllocator())
	{
		return nullptr;
	}

	AllocatorCallbackScope scope;
	const uint64_t         ptr = Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(g_malloc), size, 0, 0);
	return reinterpret_cast<void*>(ptr);
}

int MallocStatsFast(void* stats)
{
	if (!HasMallocStatsFast() || stats == nullptr)
	{
		return -1;
	}

	AllocatorCallbackScope scope;
	const int result = static_cast<int>(Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(g_stats_fast),
	                                                              reinterpret_cast<uint64_t>(stats), 0, 0));
	return result;
}

bool Free(void* ptr)
{
	if (ptr == nullptr)
	{
		return true;
	}

	if (!HasAllocator())
	{
		return false;
	}

	AllocatorCallbackScope scope;
	Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(g_free), reinterpret_cast<uint64_t>(ptr), 0, 0);
	return true;
}

void Reset()
{
	g_malloc             = nullptr;
	g_free               = nullptr;
	g_stats_fast         = nullptr;
	g_in_guest_allocator = false;
}

} // namespace Kyty::Libs::LibKernel::ApplicationHeap

#endif // KYTY_EMU_ENABLED
