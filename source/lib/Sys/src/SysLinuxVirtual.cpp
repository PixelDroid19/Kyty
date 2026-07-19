#include "Kyty/Core/Common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
//#error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Sys/SysVirtual.h"

#include "cpuinfo.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <map>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdlib>
#include <chrono>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <linux/falloc.h>
#include <sys/syscall.h>
#endif

// IWYU pragma: no_include <asm/mman-common.h>
// IWYU pragma: no_include <asm/mman.h>
// IWYU pragma: no_include <bits/pthread_types.h>
// IWYU pragma: no_include <linux/mman.h>

#if defined(MAP_FIXED_NOREPLACE) && KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#define KYTY_FIXED_NOREPLACE
#endif

namespace Kyty::Core {

static pthread_mutex_t              g_virtual_mutex {};
static std::map<uintptr_t, size_t>* g_allocs   = nullptr;
static uintptr_t                    g_guest_map_cursor = 0;
static uintptr_t                    g_shared_map_cursor = 0;

static bool IsDebugVirtualAlloc()
{
	static const bool enabled = (std::getenv("KYTY_DEBUG_FLEX_ALLOC") != nullptr);
	return enabled;
}

struct ProtectionRange
{
	uintptr_t end_page = 0;
	int       protect  = PROT_NONE;
};

// Protection metadata is interval-based and uses host-page indices. A
// direct-memory reservation can span gigabytes; storing one map node per host
// page turns a normal map or mprotect into an unbounded host-side operation.
static std::map<uintptr_t, ProtectionRange>* g_protects = nullptr;

struct SharedBacking
{
	int      fd   = -1;
	uint64_t size = 0;
};

void sys_get_system_info(SystemInfo* info)
{
	EXIT_IF(info == nullptr);

	const auto* p = cpuinfo_get_package(0);

	EXIT_IF(p == nullptr);

	info->ProcessorName = String::FromUtf8(p->name);
}

void sys_virtual_init()
{
	pthread_mutexattr_t attr {};

	pthread_mutexattr_init(&attr);
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_FAST_NP);
#else
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
#endif
	pthread_mutex_init(&g_virtual_mutex, &attr);
	pthread_mutexattr_destroy(&attr);

	g_allocs   = new std::map<uintptr_t, size_t>;
	g_protects = new std::map<uintptr_t, ProtectionRange>;
	g_guest_map_cursor  = 0;
	g_shared_map_cursor = 0;

	cpuinfo_initialize();
}

uint64_t sys_virtual_get_page_size()
{
	const long page_size = sysconf(_SC_PAGESIZE);
	return page_size > 0 ? static_cast<uint64_t>(page_size) : 0;
}

static int get_protection_flag(VirtualMemory::Mode mode)
{
	int protect = PROT_NONE;
	switch (mode)
	{
		case VirtualMemory::Mode::Read: protect = PROT_READ; break;
		case VirtualMemory::Mode::Write:
		case VirtualMemory::Mode::ReadWrite: protect = PROT_READ | PROT_WRITE; break; // NOLINT
		case VirtualMemory::Mode::Execute: protect = PROT_EXEC; break;
		case VirtualMemory::Mode::ExecuteRead: protect = PROT_EXEC | PROT_READ; break; // NOLINT
		case VirtualMemory::Mode::ExecuteWrite:
		case VirtualMemory::Mode::ExecuteReadWrite: protect = PROT_EXEC | PROT_WRITE | PROT_READ; break; // NOLINT
		case VirtualMemory::Mode::NoAccess:
		default: protect = PROT_NONE; break;
	}
	return protect;
}

static VirtualMemory::Mode get_protection_flag(int mode)
{
	switch (mode)
	{
		case PROT_NONE: return VirtualMemory::Mode::NoAccess;
		case PROT_READ: return VirtualMemory::Mode::Read;
		case PROT_WRITE: return VirtualMemory::Mode::Write;
		case PROT_READ | PROT_WRITE: return VirtualMemory::Mode::ReadWrite; // NOLINT
		case PROT_EXEC: return VirtualMemory::Mode::Execute;
		case PROT_EXEC | PROT_WRITE: return VirtualMemory::Mode::ExecuteWrite;                 // NOLINT
		case PROT_EXEC | PROT_READ: return VirtualMemory::Mode::ExecuteRead;                   // NOLINT
		case PROT_EXEC | PROT_WRITE | PROT_READ: return VirtualMemory::Mode::ExecuteReadWrite; // NOLINT
		default: return VirtualMemory::Mode::NoAccess;
	}
}

static uintptr_t align_up(uintptr_t addr, uint64_t alignment)
{
	return (addr + alignment - 1) & ~(alignment - 1);
}

static bool try_align_up(uintptr_t addr, uint64_t alignment, uintptr_t* result)
{
	if (result == nullptr || alignment == 0 || (alignment & (alignment - 1)) != 0 || addr > UINTPTR_MAX - (alignment - 1))
	{
		return false;
	}
	*result = align_up(addr, alignment);
	return true;
}

// GpuMemory page ids pack vaddr>>14 into a uint32, so guest addresses must fit
// below 2^46. Linux free mmap commonly returns ~0x7f... which is outside that
// window and aborts on the first GPU-backed VideoOut registration. Keep free
// guest placements in a low window modeled after Windows SYSTEM_MANAGED, below
// the typical eboot base at 0x9_0000_0000.
static constexpr uint64_t kGuestVaMax  = (1ull << 46) - 1ull;
static constexpr uintptr_t kGuestHeapLo = 0x0000040000ull;
static constexpr uintptr_t kGuestHeapHi = 0x0800000000ull;

static bool guest_va_compatible(uint64_t addr, uint64_t size)
{
	if (addr == 0 || size == 0)
	{
		return false;
	}
	if (addr > kGuestVaMax)
	{
		return false;
	}
	if (size - 1ull > kGuestVaMax - addr)
	{
		return false;
	}
	return true;
}

static bool is_power_of_two(uint64_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

static bool shared_mapping_valid(const SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
	                             uint64_t alignment, bool fixed)
{
	if (backing == nullptr || backing->fd < 0 || size == 0 || !is_power_of_two(alignment))
	{
		return false;
	}

	const uint64_t page_size = sys_virtual_get_page_size();
	if (page_size == 0 || alignment == 0 || !is_power_of_two(alignment) || backing_offset % page_size != 0 ||
	    size % page_size != 0)
	{
		return false;
	}

	if (backing_offset > backing->size || size > backing->size - backing_offset ||
	    backing_offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
	{
		return false;
	}

	if (fixed && (address == 0 || address % page_size != 0 || !guest_va_compatible(address, size)))
	{
		return false;
	}

	return true;
}

static void split_protection_range(uintptr_t page)
{
	auto it = g_protects->upper_bound(page);
	if (it == g_protects->begin())
	{
		return;
	}
	--it;
	if (it->first >= page || it->second.end_page <= page)
	{
		return;
	}

	const ProtectionRange right {it->second.end_page, it->second.protect};
	it->second.end_page = page;
	g_protects->emplace(page, right);
}

static const ProtectionRange* find_protection_range(uintptr_t page)
{
	auto it = g_protects->upper_bound(page);
	if (it == g_protects->begin())
	{
		return nullptr;
	}
	--it;
	return page < it->second.end_page ? &it->second : nullptr;
}

static void assign_protection_range(uintptr_t page_start, uintptr_t page_end, int protect)
{
	if (page_start >= page_end)
	{
		return;
	}

	split_protection_range(page_start);
	split_protection_range(page_end);

	auto it = g_protects->lower_bound(page_start);
	while (it != g_protects->end() && it->first < page_end)
	{
		it = g_protects->erase(it);
	}

	auto [inserted, unused] = g_protects->emplace(page_start, ProtectionRange {page_end, protect});
	(void)unused;

	if (inserted != g_protects->begin())
	{
		auto previous = inserted;
		--previous;
		if (previous->second.end_page == page_start && previous->second.protect == protect)
		{
			previous->second.end_page = page_end;
			g_protects->erase(inserted);
			inserted = previous;
		}
	}

	auto next = inserted;
	++next;
	if (next != g_protects->end() && inserted->second.end_page == next->first && next->second.protect == protect)
	{
		inserted->second.end_page = next->second.end_page;
		g_protects->erase(next);
	}
}

static void erase_protection_range(uintptr_t page_start, uintptr_t page_end)
{
	if (page_start >= page_end)
	{
		return;
	}

	split_protection_range(page_start);
	split_protection_range(page_end);

	auto it = g_protects->lower_bound(page_start);
	while (it != g_protects->end() && it->first < page_end)
	{
		it = g_protects->erase(it);
	}
}

static bool get_host_page_range(uintptr_t address, uint64_t size, uintptr_t* page_start, uintptr_t* page_end)
{
	if (page_start == nullptr || page_end == nullptr || size == 0)
	{
		return false;
	}

	const uint64_t page_size = sys_virtual_get_page_size();
	if (page_size == 0 || address > UINTPTR_MAX - (size - 1))
	{
		return false;
	}

	const uintptr_t last_address = address + size - 1;
	*page_start                 = address / page_size;
	*page_end                   = last_address / page_size + 1;
	return *page_start < *page_end;
}

static void track_alloc(uintptr_t ret_addr, uint64_t size, int protect)
{
	uintptr_t page_start = 0;
	uintptr_t page_end   = 0;
	EXIT_IF(!get_host_page_range(ret_addr, size, &page_start, &page_end));

	pthread_mutex_lock(&g_virtual_mutex);
	(*g_allocs)[ret_addr] = size;
	assign_protection_range(page_start, page_end, protect);
	pthread_mutex_unlock(&g_virtual_mutex);
}

static void* mmap_in_guest_window(uintptr_t prefer, uint64_t size, int protect, uint64_t alignment)
{
	if (alignment == 0)
	{
		alignment = 0x1000;
	}
	const uint64_t page_size = sys_virtual_get_page_size();
	alignment                  = align_up(alignment, page_size);

	// Always honor the requested alignment (e.g. 2 MiB GPU heaps).
	uintptr_t start = (prefer != 0) ? prefer : kGuestHeapLo;
	if (prefer == 0)
	{
		// Shared/direct mappings publish the end of their occupied span. Reusing
		// it avoids probing that span once per guest page during heap growth.
		pthread_mutex_lock(&g_virtual_mutex);
		if (g_guest_map_cursor >= kGuestHeapLo)
		{
			start = g_guest_map_cursor;
		}
		pthread_mutex_unlock(&g_virtual_mutex);
	}
	if (start < kGuestHeapLo || start > kGuestHeapHi - size)
	{
		start = kGuestHeapLo;
	}
	if (!try_align_up(start, alignment, &start))
	{
		return MAP_FAILED;
	}

	if (start > kGuestHeapHi - size)
	{
		return MAP_FAILED;
	}

	// Large flexible/direct heaps are demand-backed; avoid immediate commit checks
	// where supported. On macOS this is not an available flag, so map only with
	// portable protection flags.
#ifdef __APPLE__
	const int flags_base = MAP_PRIVATE | MAP_ANON;
#else
	const int flags_base = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#endif
	if (IsDebugVirtualAlloc())
	{
		printf("[mmap_window] prefer=0x%016" PRIxPTR " size=%" PRIu64 " align=0x%" PRIx64 "\n", start, size, alignment);
	}

	uint64_t attempts = 0;
	for (uintptr_t a = start; a + size <= kGuestHeapHi && a + size - 1 <= kGuestVaMax; a += alignment)
	{
		attempts++;
#ifdef KYTY_FIXED_NOREPLACE
		// NOLINTNEXTLINE
		void* ptr = mmap(reinterpret_cast<void*>(a), size, protect, MAP_FIXED_NOREPLACE | flags_base, -1, 0);
#else
		// NOLINTNEXTLINE
		void* ptr = mmap(reinterpret_cast<void*>(a), size, protect, MAP_FIXED | flags_base, -1, 0);
		if (ptr != MAP_FAILED && reinterpret_cast<uintptr_t>(ptr) != a)
		{
			munmap(ptr, size);
			ptr = MAP_FAILED;
		}
#endif
		if (ptr != MAP_FAILED)
		{
			if (prefer == 0)
			{
				pthread_mutex_lock(&g_virtual_mutex);
				g_guest_map_cursor = a + size;
				pthread_mutex_unlock(&g_virtual_mutex);
			}
			if (IsDebugVirtualAlloc())
			{
					printf("[mmap_window] success attempt=%" PRIu64 " addr=0x%016" PRIxPTR "\n", attempts, reinterpret_cast<uintptr_t>(ptr));
			}
			return ptr;
		}

		if (IsDebugVirtualAlloc() && (attempts % 1024u) == 0)
		{
			printf("[mmap_window] fail attempt=%" PRIu64 " at=0x%016" PRIxPTR "\n", attempts, a);
		}
	}
	if (IsDebugVirtualAlloc())
	{
		printf("[mmap_window] exhausted attempts=%" PRIu64 "\n", attempts);
	}
	return MAP_FAILED;
}

uint64_t sys_virtual_alloc(uint64_t address, uint64_t size, VirtualMemory::Mode mode)
{
	EXIT_IF(g_allocs == nullptr);

	auto addr = static_cast<uintptr_t>(address);
	const auto start_us = IsDebugVirtualAlloc() ? std::chrono::duration_cast<std::chrono::microseconds>(
	                                   std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
	if (IsDebugVirtualAlloc())
	{
		printf("[sys_alloc] req address=0x%016" PRIx64 " size=%" PRIu64 "\n", address, size);
	}

	int protect = get_protection_flag(mode);

	void* ptr = nullptr;
	if (addr == 0)
	{
	#ifdef __APPLE__
		// Prefer an unrestricted anonymous mapping first on Apple: it often lands
		// directly in the guest-compatible range and avoids expensive linear fixed
		// probing when large windows are heavily fragmented.
		ptr = mmap(nullptr, size, protect, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (ptr != MAP_FAILED)
		{
			const uintptr_t mapped_addr = reinterpret_cast<uintptr_t>(ptr);
			if (!guest_va_compatible(mapped_addr, size) || mapped_addr < kGuestHeapLo)
			{
				munmap(ptr, size);
				ptr = MAP_FAILED;
			}
		}
		if (ptr == MAP_FAILED)
		{
			ptr = mmap_in_guest_window(0, size, protect, 0x1000);
		}
	#else
		ptr = mmap_in_guest_window(0, size, protect, 0x1000);
	#endif
	} else
	{
		// NOLINTNEXTLINE
		ptr = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_PRIVATE | MAP_ANON, -1, 0);
		auto ret_addr = reinterpret_cast<uintptr_t>(ptr);
		if (ptr != MAP_FAILED && !guest_va_compatible(ret_addr, size))
		{
			munmap(ptr, size);
			ptr = mmap_in_guest_window(addr, size, protect, 0x1000);
		}
	}

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED)
	{
		if (IsDebugVirtualAlloc())
		{
			const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
			    std::chrono::steady_clock::now().time_since_epoch()).count();
			printf("[sys_alloc] ok out=0x%016" PRIx64 " elapsed_us=%" PRIu64 "\n", static_cast<uint64_t>(ret_addr), now_us - start_us);
		}
		track_alloc(ret_addr, size, protect);
		return ret_addr;
	}
	if (IsDebugVirtualAlloc())
	{
		printf("[sys_alloc] fail\n");
	}

	return 0;
}

uint64_t sys_virtual_alloc_aligned(uint64_t address, uint64_t size, VirtualMemory::Mode mode, uint64_t alignment)
{
	if (alignment == 0)
	{
		return 0;
	}

	EXIT_IF(g_allocs == nullptr);

	auto addr    = static_cast<uintptr_t>(address);
	int  protect = get_protection_flag(mode);

	void*     ptr      = MAP_FAILED;
	uintptr_t ret_addr = 0;

	if (addr == 0)
	{
		ptr = mmap_in_guest_window(0, size, protect, alignment);
		ret_addr = reinterpret_cast<uintptr_t>(ptr);
	} else
	{
		// NOLINTNEXTLINE
		ptr      = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_PRIVATE | MAP_ANON, -1, 0);
		ret_addr = reinterpret_cast<uintptr_t>(ptr);

		if (ptr != MAP_FAILED && (((ret_addr & (alignment - 1)) != 0) || !guest_va_compatible(ret_addr, size)))
		{
			munmap(ptr, size);
			ptr      = MAP_FAILED;
			ret_addr = 0;
		}

		if (ptr == MAP_FAILED)
		{
			// Prefer the caller's hint when it is already in the guest window.
			const uintptr_t prefer = guest_va_compatible(addr, size) ? addr : 0;
			ptr                    = mmap_in_guest_window(prefer, size, protect, alignment);
			ret_addr               = reinterpret_cast<uintptr_t>(ptr);
		}
	}

	if (ptr == MAP_FAILED || ((ret_addr & (alignment - 1)) != 0) || !guest_va_compatible(ret_addr, size))
	{
		if (ptr != MAP_FAILED)
		{
			munmap(ptr, size);
		}
		// Widening alignment can recover from awkward free-space fragmentation.
		if (alignment < (1ull << 30))
		{
			return sys_virtual_alloc_aligned(address, size, mode, alignment << 1u);
		}
		return 0;
	}

	track_alloc(ret_addr, size, protect);
	return ret_addr;
}

#ifdef __APPLE__

enum class MappedRangeState
{
	Available,
	Occupied,
	QueryFailed
};

static MappedRangeState get_mapped_range_state(void* ptr, size_t length, uintptr_t* occupied_end)
{
	if (ptr == nullptr || length == 0 || occupied_end == nullptr)
	{
		return MappedRangeState::QueryFailed;
	}

	// mach_vm_region returns the region at or immediately after `address`. Keep
	// the returned end so callers can skip a complete occupied interval instead
	// of probing every guest alignment inside it.
	auto                           address     = reinterpret_cast<mach_vm_address_t>(ptr);
	mach_vm_size_t                 region_size = 0;
	vm_region_basic_info_data_64_t info {};
	mach_msg_type_number_t         count       = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t                    object_name = MACH_PORT_NULL;

	kern_return_t kr =
	    mach_vm_region(mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
	                   reinterpret_cast<vm_region_info_t>(&info), &count, &object_name);

	if (kr != KERN_SUCCESS)
	{
		return (kr == KERN_INVALID_ADDRESS ? MappedRangeState::Available : MappedRangeState::QueryFailed);
	}

	const auto requested_start = reinterpret_cast<mach_vm_address_t>(ptr);
	if (length > std::numeric_limits<mach_vm_address_t>::max() - requested_start ||
	    region_size > std::numeric_limits<mach_vm_address_t>::max() - address)
	{
		return MappedRangeState::QueryFailed;
	}
	const auto requested_end   = requested_start + length;
	const auto region_end      = address + region_size;
	if (address < requested_end && region_end > requested_start)
	{
		*occupied_end = static_cast<uintptr_t>(region_end);
		return MappedRangeState::Occupied;
	}

	return MappedRangeState::Available;
}

static bool reserve_shared_mapping(uintptr_t address, uint64_t size)
{
	mach_vm_address_t reservation = address;
	return mach_vm_allocate(mach_task_self(), &reservation, size, VM_FLAGS_FIXED) == KERN_SUCCESS && reservation == address;
}

static void release_shared_mapping_reservation(uintptr_t address, uint64_t size)
{
	(void)mach_vm_deallocate(mach_task_self(), address, size);
}

[[maybe_unused]] static bool is_mmaped(void* ptr, size_t length)
{
	uintptr_t occupied_end = 0;
	return get_mapped_range_state(ptr, length, &occupied_end) != MappedRangeState::Available;
}

#else

[[maybe_unused]] static bool is_mmaped(void* ptr, size_t length)
{
	FILE* file = fopen("/proc/self/maps", "r");
	char  line[1024];
	bool  ret  = false;
	auto  addr = reinterpret_cast<uintptr_t>(ptr);

	[[maybe_unused]] int result = 0;

	while (feof(file) == 0)
	{
		if (fgets(line, 1024, file) == nullptr)
		{
			break;
		}
		uint64_t start = 0;
		uint64_t end   = 0;
		// NOLINTNEXTLINE(cert-err34-c)
		if (sscanf(line, "%" SCNx64 "-%" SCNx64, &start, &end) != 2)
		{
			continue;
		}
		if (addr >= start && addr + length <= end)
		{
			ret = true;
			break;
		}
	}
	result = fclose(file);
	return ret;
}

#endif

void* sys_virtual_create_shared_backing(uint64_t size)
{
	if (size == 0 || size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
	{
		return nullptr;
	}

	int fd = -1;
#ifdef __APPLE__
	// Darwin does not expose Linux's memfd_create and the SDK does not define
	// SHM_ANON. Create a private temporary file, unlink it before returning, and
	// keep the descriptor as the anonymous shared backing for mmap.
	char temp_path[] = "/tmp/kyty-direct-memory-XXXXXX";
	fd             = ::mkstemp(temp_path);
	if (fd >= 0)
	{
		::unlink(temp_path);
	}
#else
#ifdef SYS_memfd_create
	static constexpr unsigned int kMemfdCloseOnExec = 0x0001u;
	static constexpr unsigned int kMemfdExecutable  = 0x0010u;
	fd = static_cast<int>(syscall(SYS_memfd_create, "kyty-direct-memory", kMemfdCloseOnExec | kMemfdExecutable));
	if (fd < 0 && errno == EINVAL)
	{
		fd = static_cast<int>(syscall(SYS_memfd_create, "kyty-direct-memory", kMemfdCloseOnExec));
	}
#endif
#endif
	if (fd < 0)
	{
		return nullptr;
	}
	const int descriptor_flags = ::fcntl(fd, F_GETFD);
	if (descriptor_flags < 0 || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0)
	{
		::close(fd);
		return nullptr;
	}

	if (ftruncate(fd, static_cast<off_t>(size)) != 0)
	{
		close(fd);
		return nullptr;
	}

	auto* backing = new SharedBacking;
	backing->fd    = fd;
	backing->size  = size;
	return backing;
}

void sys_virtual_destroy_shared_backing(void* backing)
{
	auto* shared = static_cast<SharedBacking*>(backing);
	if (shared == nullptr)
	{
		return;
	}

	if (shared->fd >= 0)
	{
		close(shared->fd);
	}
	delete shared;
}

bool sys_virtual_discard_shared_backing_range(void* backing, uint64_t backing_offset, uint64_t size)
{
	auto* shared = static_cast<SharedBacking*>(backing);
	if (shared == nullptr || shared->fd < 0 || size == 0 || backing_offset > shared->size || size > shared->size - backing_offset)
	{
		return false;
	}

	// Align outward to host pages so partial ranges still reclaim whole faulted pages.
	const uint64_t page_size = sys_virtual_get_page_size();
	if (page_size == 0)
	{
		return false;
	}
	const uint64_t start = backing_offset & ~(page_size - 1u);
	uint64_t       end   = (backing_offset + size + page_size - 1u) & ~(page_size - 1u);
	if (end > shared->size)
	{
		end = shared->size;
	}
	if (end <= start)
	{
		return false;
	}

#if defined(__APPLE__)
#ifdef F_PUNCHHOLE
	fpunchhole_t hole {};
	hole.fp_offset = static_cast<off_t>(start);
	hole.fp_length = static_cast<off_t>(end - start);
	return ::fcntl(shared->fd, F_PUNCHHOLE, &hole) == 0;
#else
	(void)start;
	return false;
#endif
#else
	return ::fallocate(shared->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(start),
	                   static_cast<off_t>(end - start)) == 0;
#endif
}

static void* mmap_shared_in_guest_window(const SharedBacking* backing, uintptr_t prefer, uint64_t backing_offset, uint64_t size,
	                                     int protect, uint64_t alignment)
{
	if (size > kGuestHeapHi - kGuestHeapLo)
	{
		return MAP_FAILED;
	}

	uintptr_t start = prefer;
	if (start == 0)
	{
		pthread_mutex_lock(&g_virtual_mutex);
		start = g_shared_map_cursor;
		pthread_mutex_unlock(&g_virtual_mutex);
	}
	if (start < kGuestHeapLo || start > kGuestHeapHi - size)
	{
		start = kGuestHeapLo;
	}
	if (!try_align_up(start, alignment, &start))
	{
		return MAP_FAILED;
	}

	if (start > kGuestHeapHi - size)
	{
		return MAP_FAILED;
	}

	const uintptr_t last_address = std::min<uintptr_t>(kGuestHeapHi - size, kGuestVaMax - (size - 1));
	uintptr_t       address      = start;
	while (address <= last_address)
	{
		void* ptr = MAP_FAILED;
#ifdef KYTY_FIXED_NOREPLACE
		// NOLINTNEXTLINE
		ptr = mmap(reinterpret_cast<void*>(address), size, protect, MAP_FIXED_NOREPLACE | MAP_SHARED, backing->fd,
		           static_cast<off_t>(backing_offset));
#else
#ifdef __APPLE__
		if (!reserve_shared_mapping(address, size))
		{
		uintptr_t occupied_end = 0;
		const auto range_state = get_mapped_range_state(reinterpret_cast<void*>(address), size, &occupied_end);
		if (range_state == MappedRangeState::QueryFailed)
		{
			return MAP_FAILED;
		}
		if (range_state == MappedRangeState::Occupied)
		{
			uintptr_t next_address = 0;
			if (!try_align_up(occupied_end, alignment, &next_address) || next_address <= address || next_address > last_address)
			{
				break;
			}
			address = next_address;
			continue;
		}
			if (alignment > last_address - address)
			{
				break;
			}
			address += alignment;
			continue;
		}
		// NOLINTNEXTLINE
		ptr = mmap(reinterpret_cast<void*>(address), size, protect, MAP_FIXED | MAP_SHARED, backing->fd,
		           static_cast<off_t>(backing_offset));
		if (ptr == MAP_FAILED || reinterpret_cast<uintptr_t>(ptr) != address)
		{
			if (ptr != MAP_FAILED)
			{
				munmap(ptr, size);
			}
			release_shared_mapping_reservation(address, size);
			ptr = MAP_FAILED;
		}
#else
		if (!is_mmaped(reinterpret_cast<void*>(address), size))
		{
			// NOLINTNEXTLINE
			ptr = mmap(reinterpret_cast<void*>(address), size, protect, MAP_FIXED | MAP_SHARED, backing->fd,
			           static_cast<off_t>(backing_offset));
		}
#endif
#endif
		if (ptr != MAP_FAILED)
		{
			if (reinterpret_cast<uintptr_t>(ptr) == address)
			{
				if (prefer == 0)
				{
					pthread_mutex_lock(&g_virtual_mutex);
					g_shared_map_cursor = address + size;
					g_guest_map_cursor  = address + size;
					pthread_mutex_unlock(&g_virtual_mutex);
				}
				return ptr;
			}
			munmap(ptr, size);
		}

		if (alignment > last_address - address)
		{
			break;
		}
		address += alignment;
	}

	return MAP_FAILED;
}

uint64_t sys_virtual_map_shared_aligned(void* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
	                                    VirtualMemory::Mode mode, uint64_t alignment)
{
	EXIT_IF(g_allocs == nullptr);

	auto* shared = static_cast<SharedBacking*>(backing);
	if (!shared_mapping_valid(shared, address, backing_offset, size, alignment, false))
	{
		return 0;
	}

	const uintptr_t prefer = guest_va_compatible(address, size) ? static_cast<uintptr_t>(address) : 0;
	const int       protect = get_protection_flag(mode);
	const uint64_t  page_size = sys_virtual_get_page_size();
	const uint64_t  effective_alignment = std::max(alignment, page_size);
	void*           ptr     = mmap_shared_in_guest_window(shared, prefer, backing_offset, size, protect, effective_alignment);
	if (ptr == MAP_FAILED)
	{
		return 0;
	}

	const auto ret_addr = reinterpret_cast<uintptr_t>(ptr);
	if ((ret_addr & (effective_alignment - 1)) != 0 || !guest_va_compatible(ret_addr, size))
	{
		munmap(ptr, size);
		return 0;
	}

	track_alloc(ret_addr, size, protect);
	return ret_addr;
}

bool sys_virtual_map_shared_fixed(void* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
	                              VirtualMemory::Mode mode)
{
	EXIT_IF(g_allocs == nullptr);

	auto* shared          = static_cast<SharedBacking*>(backing);
	const uint64_t page_size = sys_virtual_get_page_size();
	if (!shared_mapping_valid(shared, address, backing_offset, size, page_size, true))
	{
		return false;
	}

	const auto addr    = static_cast<uintptr_t>(address);
	const int  protect = get_protection_flag(mode);
	void*      ptr     = MAP_FAILED;

#ifdef KYTY_FIXED_NOREPLACE
	// NOLINTNEXTLINE
	ptr = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_FIXED_NOREPLACE | MAP_SHARED, shared->fd,
	           static_cast<off_t>(backing_offset));
#else
#ifdef __APPLE__
	// macOS has no MAP_FIXED_NOREPLACE. Reserve the exact interval first so the
	// MAP_FIXED call replaces only a reservation created by this process, never
	// an unrelated host mapping (including a Rosetta reservation).
	if (reserve_shared_mapping(addr, size))
	{
		// NOLINTNEXTLINE
		ptr = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_FIXED | MAP_SHARED, shared->fd,
		           static_cast<off_t>(backing_offset));
		if (ptr == MAP_FAILED || reinterpret_cast<uintptr_t>(ptr) != addr)
		{
			if (ptr != MAP_FAILED)
			{
				munmap(ptr, size);
			}
			release_shared_mapping_reservation(addr, size);
			ptr = MAP_FAILED;
		}
	}
#else
	if (!is_mmaped(reinterpret_cast<void*>(addr), size))
	{
		// NOLINTNEXTLINE
		ptr = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_FIXED | MAP_SHARED, shared->fd,
		           static_cast<off_t>(backing_offset));
	}
#endif
#endif

	if (ptr == MAP_FAILED)
	{
		return false;
	}

	if (reinterpret_cast<uintptr_t>(ptr) != addr)
	{
		munmap(ptr, size);
		return false;
	}

	track_alloc(addr, size, protect);
	return true;
}

uint64_t sys_virtual_map_shared_fixed_or_relocated(void* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
                                                   VirtualMemory::Mode mode, uint64_t alignment)
{
	if (sys_virtual_map_shared_fixed(backing, address, backing_offset, size, mode))
	{
		return address;
	}

#ifdef __APPLE__
	// Rosetta reserves parts of the low address space used by PS5 applications.
	// Relocate only an external-host collision; a Kyty-owned overlap remains a
	// real fixed-map conflict and must fail.
	bool overlaps_tracked = false;
	pthread_mutex_lock(&g_virtual_mutex);
	for (const auto& [base, tracked_size]: *g_allocs)
	{
		const uint64_t requested_end = address <= UINT64_MAX - size ? address + size : UINT64_MAX;
		const uint64_t tracked_end   = base <= UINT64_MAX - tracked_size ? base + tracked_size : UINT64_MAX;
		if (base < requested_end && address < tracked_end)
		{
			overlaps_tracked = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	if (!overlaps_tracked)
	{
		return sys_virtual_map_shared_aligned(backing, 0, backing_offset, size, mode, alignment);
	}
#else
	(void)alignment;
#endif
	return 0;
}

bool sys_virtual_alloc_fixed(uint64_t address, uint64_t size, VirtualMemory::Mode mode)
{
	EXIT_IF(g_allocs == nullptr);

	auto addr    = static_cast<uintptr_t>(address);
	int  protect = get_protection_flag(mode);

#ifdef KYTY_FIXED_NOREPLACE
	// NOLINTNEXTLINE
	void* ptr = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON, -1, 0);
#elif defined(__APPLE__)
	// macOS ignores a plain address hint (only MAP_FIXED honours a specific
	// address) and has no MAP_FIXED_NOREPLACE. The PS5 guest address space is
	// managed by Kyty, so the requested address is free on the host; MAP_FIXED
	// there is safe. The return value is validated against `addr` below.
	void* ptr = mmap(reinterpret_cast<void*>(addr), size, protect, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
#else
	// NOLINTNEXTLINE
	void* ptr = (is_mmaped(reinterpret_cast<void*>(addr), size)
	                 ? MAP_FAILED
	                 : mmap(reinterpret_cast<void*>(addr), size, protect, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0));
#endif

	auto ret_addr = reinterpret_cast<uintptr_t>(ptr);

	if (ptr != MAP_FAILED && ret_addr != addr)
	{
		munmap(ptr, size);
		ret_addr = 0;
		ptr      = MAP_FAILED;
	}

	if (ptr != MAP_FAILED)
	{
		track_alloc(ret_addr, size, protect);

		return true;
	}

	return false;
}

bool sys_virtual_free(uint64_t address)
{
	EXIT_IF(g_allocs == nullptr);
	size_t size = 0;

	const uint64_t page_size = sys_virtual_get_page_size();
	if (page_size == 0)
	{
		return false;
	}
	auto addr = static_cast<uintptr_t>(address / page_size * page_size);

	pthread_mutex_lock(&g_virtual_mutex);
	if (auto s = g_allocs->find(addr); s != g_allocs->end())
	{
		size = s->second;
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	if (size == 0)
	{
		return false;
	}

	if (munmap(reinterpret_cast<void*>(addr), size) == 0)
	{
		uintptr_t page_start = 0;
		uintptr_t page_end   = 0;
		EXIT_IF(!get_host_page_range(addr, size, &page_start, &page_end));
		pthread_mutex_lock(&g_virtual_mutex);
		g_allocs->erase(addr);
		erase_protection_range(page_start, page_end);
		if (g_guest_map_cursor > addr)
		{
			g_guest_map_cursor = addr;
		}
		pthread_mutex_unlock(&g_virtual_mutex);
		return true;
	}

	return false;
}

bool sys_virtual_protect(uint64_t address, uint64_t size, VirtualMemory::Mode mode, VirtualMemory::Mode* old_mode)
{
	auto addr = static_cast<uintptr_t>(address);
	uintptr_t page_start = 0;
	uintptr_t page_end   = 0;
	if (!get_host_page_range(addr, size, &page_start, &page_end))
	{
		return false;
	}

	pthread_mutex_lock(&g_virtual_mutex);
	if (old_mode != nullptr)
	{
		if (const auto* protection = find_protection_range(page_start); protection != nullptr)
		{
			*old_mode = get_protection_flag(protection->protect);
		} else
		{
			*old_mode = VirtualMemory::Mode::NoAccess;
		}
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	const uint64_t page_size = sys_virtual_get_page_size();
	if (mprotect(reinterpret_cast<void*>(page_start * page_size), (page_end - page_start) * page_size, get_protection_flag(mode)) == 0)
	{
		pthread_mutex_lock(&g_virtual_mutex);
		assign_protection_range(page_start, page_end, get_protection_flag(mode));
		pthread_mutex_unlock(&g_virtual_mutex);
		return true;
	}

	return false;
}

bool sys_virtual_protect_write_signal_safe(uint64_t address, uint64_t size)
{
	if (size == 0)
	{
		return false;
	}
	return mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), size, PROT_READ | PROT_WRITE) == 0;
}

bool sys_virtual_flush_instruction_cache(uint64_t /*address*/, uint64_t /*size*/)
{
	return true;
}

bool sys_virtual_patch_replace(uint64_t vaddr, uint64_t value)
{
	VirtualMemory::Mode old_mode {};
	sys_virtual_protect(vaddr, 8, VirtualMemory::Mode::ReadWrite, &old_mode);

	auto* ptr = reinterpret_cast<uint64_t*>(vaddr);

	bool ret = (*ptr != value);

	*ptr = value;

	sys_virtual_protect(vaddr, 8, old_mode);

	if (VirtualMemory::IsExecute(old_mode))
	{
		sys_virtual_flush_instruction_cache(vaddr, 8);
	}

	return ret;
}

} // namespace Kyty::Core

#endif
