#include "Kyty/Core/Common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
//#error "KYTY_PLATFORM != KYTY_PLATFORM_LINUX"
#else

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Sys/SysVirtual.h"

#include "cpuinfo.h"

#include <cerrno>
#include <map>
#include <pthread.h>
#include <sys/mman.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
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
static std::map<uintptr_t, int>*    g_protects = nullptr;

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
	g_protects = new std::map<uintptr_t, int>;

	cpuinfo_initialize();
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
	if (addr + size - 1ull > kGuestVaMax)
	{
		return false;
	}
	return true;
}

static void track_alloc(uintptr_t ret_addr, uint64_t size, int protect)
{
	pthread_mutex_lock(&g_virtual_mutex);
	(*g_allocs)[ret_addr] = size;
	uintptr_t page_start  = ret_addr >> 12u;
	uintptr_t page_end    = (ret_addr + size - 1) >> 12u;
	for (uintptr_t page = page_start; page <= page_end; page++)
	{
		(*g_protects)[page] = protect;
	}
	pthread_mutex_unlock(&g_virtual_mutex);
}

static void* mmap_in_guest_window(uintptr_t prefer, uint64_t size, int protect, uint64_t alignment)
{
	if (alignment == 0)
	{
		alignment = 0x1000;
	}

	// Always honor the requested alignment (e.g. 2 MiB GPU heaps).
	uintptr_t start = (prefer != 0) ? prefer : kGuestHeapLo;
	if (start < kGuestHeapLo || start > kGuestHeapHi - size)
	{
		start = kGuestHeapLo;
	}
	start = align_up(start, alignment);

	if (start > kGuestHeapHi - size)
	{
		return MAP_FAILED;
	}

	// Large flexible/direct heaps are demand-backed; avoid immediate commit checks.
	const int flags_base = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;

	for (uintptr_t a = start; a + size <= kGuestHeapHi && a + size - 1 <= kGuestVaMax; a += alignment)
	{
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
			return ptr;
		}
	}
	return MAP_FAILED;
}

uint64_t sys_virtual_alloc(uint64_t address, uint64_t size, VirtualMemory::Mode mode)
{
	EXIT_IF(g_allocs == nullptr);

	auto addr = static_cast<uintptr_t>(address);

	int protect = get_protection_flag(mode);

	void* ptr = nullptr;
	if (addr == 0)
	{
		ptr = mmap_in_guest_window(0, size, protect, 0x1000);
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
		track_alloc(ret_addr, size, protect);
		return ret_addr;
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

[[maybe_unused]] static bool is_mmaped(void* ptr, size_t length)
{
	// Returns true if any existing mapping overlaps [ptr, ptr + length)
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
		return false;
	}

	return address < reinterpret_cast<mach_vm_address_t>(ptr) + length;
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
		pthread_mutex_lock(&g_virtual_mutex);
		(*g_allocs)[ret_addr] = size;
		uintptr_t page_start  = ret_addr >> 12u;
		uintptr_t page_end    = (ret_addr + size - 1) >> 12u;
		for (uintptr_t page = page_start; page <= page_end; page++)
		{
			(*g_protects)[page] = protect;
		}
		pthread_mutex_unlock(&g_virtual_mutex);

		return true;
	}

	return false;
}

bool sys_virtual_free(uint64_t address)
{
	EXIT_IF(g_allocs == nullptr);
	size_t size = 0;

	auto addr = static_cast<uintptr_t>(address & ~static_cast<uint64_t>(0xfffu));

	pthread_mutex_lock(&g_virtual_mutex);
	if (auto s = g_allocs->find(addr); s != g_allocs->end())
	{
		size = s->second;
		g_allocs->erase(s);
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	if (size == 0)
	{
		return false;
	}

	if (munmap(reinterpret_cast<void*>(addr), size) == 0)
	{
		uintptr_t page_start = addr >> 12u;
		uintptr_t page_end   = (addr + size - 1) >> 12u;
		pthread_mutex_lock(&g_virtual_mutex);
		for (uintptr_t page = page_start; page <= page_end; page++)
		{
			if (auto s = g_protects->find(page); s != g_protects->end())
			{
				g_protects->erase(s);
			}
		}
		pthread_mutex_unlock(&g_virtual_mutex);
		return true;
	}

	return false;
}

bool sys_virtual_protect(uint64_t address, uint64_t size, VirtualMemory::Mode mode, VirtualMemory::Mode* old_mode)
{
	auto addr = static_cast<uintptr_t>(address);

	pthread_mutex_lock(&g_virtual_mutex);
	if (old_mode != nullptr)
	{
		if (auto s = g_protects->find(addr >> 12u); s != g_protects->end())
		{
			*old_mode = get_protection_flag(s->second);
		} else
		{
			*old_mode = VirtualMemory::Mode::NoAccess;
		}
	}
	pthread_mutex_unlock(&g_virtual_mutex);

	uintptr_t page_start = addr >> 12u;
	uintptr_t page_end   = (addr + size - 1) >> 12u;
	if (mprotect(reinterpret_cast<void*>(page_start << 12u), (page_end - page_start + 1) << 12u, get_protection_flag(mode)) == 0)
	{
		pthread_mutex_lock(&g_virtual_mutex);
		for (uintptr_t page = page_start; page <= page_end; page++)
		{
			(*g_protects)[page] = get_protection_flag(mode);
		}
		pthread_mutex_unlock(&g_virtual_mutex);
		return true;
	}

	return false;
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
