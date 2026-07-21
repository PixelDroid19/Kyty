#include "Emulator/Libs/ApplicationHeap.h"

#include "Emulator/Loader/GuestCall.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::ApplicationHeap {

namespace {

using MallocFunc = void*(KYTY_SYSV_ABI*)(size_t);
using FreeFunc   = void(KYTY_SYSV_ABI*)(void*);

static MallocFunc g_malloc = nullptr;
static FreeFunc   g_free   = nullptr;

static thread_local bool g_in_guest_allocator = false;

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
		g_malloc = nullptr;
		g_free   = nullptr;
		return;
	}

	g_malloc = reinterpret_cast<MallocFunc>(table->slots[kMallocSlot]);
	g_free   = reinterpret_cast<FreeFunc>(table->slots[kFreeSlot]);
}

bool IsInitialized()
{
	return g_malloc != nullptr && g_free != nullptr;
}

bool HasAllocator()
{
	return IsInitialized() && !g_in_guest_allocator;
}

void* Malloc(size_t size)
{
	if (!HasAllocator())
	{
		return nullptr;
	}

	g_in_guest_allocator = true;
	const uint64_t ptr   = Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(g_malloc), size, 0, 0);
	g_in_guest_allocator = false;
	return reinterpret_cast<void*>(ptr);
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

	g_in_guest_allocator = true;
	Loader::GuestCall::Invoke(reinterpret_cast<uint64_t>(g_free), reinterpret_cast<uint64_t>(ptr), 0, 0);
	g_in_guest_allocator = false;
	return true;
}

void Reset()
{
	g_malloc              = nullptr;
	g_free                = nullptr;
	g_in_guest_allocator  = false;
}

} // namespace Kyty::Libs::LibKernel::ApplicationHeap

#endif // KYTY_EMU_ENABLED
