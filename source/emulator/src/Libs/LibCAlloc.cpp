#include "LibCInternal.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Libs/ApplicationHeap.h"
#include "Emulator/Libs/Memalign.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__GLIBC__)
#include <malloc.h>
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibC {

struct HostAllocationRecord
{
	size_t size;
};

static std::mutex                                      g_allocations_mutex;
static std::unordered_map<void*, HostAllocationRecord> g_allocations;

static bool register_allocation(void* ptr, HostAllocationRecord record)
{
	if (ptr == nullptr)
	{
		return false;
	}

	std::lock_guard lock(g_allocations_mutex);
	return g_allocations.emplace(ptr, record).second;
}

static bool claim_allocation(void* ptr, HostAllocationRecord* record)
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

static void* allocate_host_owned(size_t size)
{
	void* ptr = ::malloc(size);
	if (ptr != nullptr)
	{
		const bool registered = register_allocation(ptr, {size});
		EXIT_IF(!registered);
	}
	return ptr;
}

void* allocate_with_owner(size_t size)
{
	if (LibKernel::ApplicationHeap::IsAllocatorCallbackActive())
	{
		return allocate_host_owned(size);
	}

	if (void* ptr = LibKernel::ApplicationHeap::Malloc(size); ptr != nullptr)
	{
		return ptr;
	}

	if (LibKernel::ApplicationHeap::IsInitialized())
	{
		return nullptr;
	}

	return allocate_host_owned(size);
}

bool free_by_owner(void* ptr)
{
	if (ptr == nullptr)
	{
		return true;
	}

	HostAllocationRecord record {};
	if (claim_allocation(ptr, &record))
	{
		::free(ptr);
		return true;
	}

	if (LibKernel::ApplicationHeap::IsInitialized())
	{
		if (LibKernel::ApplicationHeap::HasAllocator())
		{
			return LibKernel::ApplicationHeap::Free(ptr);
		}
		return false;
	}

	::free(ptr);
	return true;
}

// Capture the host allocator's live accounting for the libc bootstrap path.
// The startup path calls malloc_stats_fast before the application heap table
// is published, so forwarding to a guest slot at that point would recurse into
// an uninitialized allocator. mallinfo2 is used where glibc exposes it; the
// ledger remains the portable lower-bound fallback on other hosts.
void collect_host_malloc_stats(Core::MSpaceSize* out)
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
KYTY_SYSV_ABI void* c_malloc(size_t size)
{
	return allocate_with_owner(size);
}

KYTY_SYSV_ABI char* c_strdup(const char* source)
{
	if (source == nullptr)
	{
		return nullptr;
	}

	const size_t size        = ::strlen(source) + 1;
	auto*        destination = static_cast<char*>(allocate_with_owner(size));
	if (destination != nullptr)
	{
		::memcpy(destination, source, size);
	}
	return destination;
}

KYTY_SYSV_ABI void* c_calloc(size_t n, size_t size)
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

KYTY_SYSV_ABI void* c_memalign(size_t alignment, size_t size)
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

KYTY_SYSV_ABI void c_free(void* p);

KYTY_SYSV_ABI void* c_realloc(void* p, size_t size)
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

	HostAllocationRecord record {};
	if (!claim_allocation(p, &record))
	{
		if (LibKernel::ApplicationHeap::IsInitialized())
		{
			EXIT("libc HLE cannot realloc an unowned application-heap pointer\n");
		}
		return ::realloc(p, size);
	}

	void* replacement = ::realloc(p, size);
	if (replacement == nullptr)
	{
		const bool restored = register_allocation(p, record);
		EXIT_IF(!restored);
		return nullptr;
	}

	const bool registered = register_allocation(replacement, {size});
	EXIT_IF(!registered);
	return replacement;
}

KYTY_SYSV_ABI void c_free(void* p)
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
KYTY_SYSV_ABI void* c_aligned_alloc(size_t alignment, size_t size)
{
	return c_memalign(alignment, size);
}
KYTY_SYSV_ABI int c_posix_memalign(void** memptr, size_t alignment, size_t size)
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
} // namespace Kyty::Libs::LibC

#endif // KYTY_EMU_ENABLED
