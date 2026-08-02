#include "Kyty/Core/Common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
//#error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Sys/SysVirtual.h"

#include "cpuinfo.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <windows.h> // IWYU pragma: keep

// IWYU pragma: no_include <basetsd.h>
// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <memoryapi.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>
// IWYU pragma: no_include <winerror.h>
// IWYU pragma: no_include <wtypes.h>

namespace Kyty::Core {

static DWORD    get_protection_flag(VirtualMemory::Mode mode);
static uint64_t align_up(uint64_t addr, uint64_t alignment);
static bool     try_align_up(uint64_t addr, uint64_t alignment, uint64_t* result);

namespace {

struct SharedBacking
{
	HANDLE   mapping = nullptr;
	uint64_t size    = 0;
};

struct ReservationRoot
{
	uint64_t address = 0;
	uint64_t size    = 0;
};

struct SharedView
{
	uint64_t mapped_address = 0;
	uint64_t mapped_size    = 0;
};

std::mutex                   g_shared_views_mutex;
std::mutex                   g_protection_transaction_mutex;
std::mutex                   g_reservations_mutex;
std::unordered_map<uint64_t, SharedView> g_shared_views;
std::unordered_map<uint64_t, uint64_t> g_private_shared_views;
std::vector<ReservationRoot>           g_reservation_roots;

constexpr uint64_t SYSTEM_MANAGED_MIN = 0x0000040000u;
constexpr uint64_t SYSTEM_MANAGED_MAX = 0x07FFFFBFFFu;
constexpr uint64_t USER_MIN           = 0x1000000000u;
constexpr uint64_t USER_MAX           = 0xFBFFFFFFFFu;

bool is_power_of_two(uint64_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

bool range_contains(uint64_t outer_address, uint64_t outer_size, uint64_t address, uint64_t size)
{
	return size != 0 && outer_size != 0 && address >= outer_address && address - outer_address <= outer_size &&
	       size <= outer_size - (address - outer_address);
}

bool find_reservation_root(uint64_t address, uint64_t size, ReservationRoot* root)
{
	std::scoped_lock lock(g_reservations_mutex);
	const auto       reservation = std::find_if(g_reservation_roots.begin(), g_reservation_roots.end(),
	                                            [address, size](const ReservationRoot& candidate)
	                                            { return range_contains(candidate.address, candidate.size, address, size); });
	if (reservation == g_reservation_roots.end())
	{
		return false;
	}
	if (root != nullptr)
	{
		*root = *reservation;
	}
	return true;
}

void register_reservation_root(uint64_t address, uint64_t size)
{
	std::scoped_lock lock(g_reservations_mutex);
	g_reservation_roots.push_back({address, size});
}

bool range_is_uncommitted(uint64_t address, uint64_t size)
{
	if (size == 0 || address > UINT64_MAX - size)
	{
		return false;
	}

	const uint64_t end = address + size;
	for (uint64_t cursor = address; cursor < end;)
	{
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(cursor)), &info, sizeof(info)) == 0 ||
		    info.State != MEM_RESERVE)
		{
			return false;
		}
		const uint64_t region_address = reinterpret_cast<uint64_t>(info.BaseAddress);
		if (info.RegionSize == 0 || region_address > UINT64_MAX - info.RegionSize)
		{
			return false;
		}
		const uint64_t next = std::min(end, region_address + info.RegionSize);
		if (next <= cursor)
		{
			return false;
		}
		cursor = next;
	}
	return true;
}

uint64_t get_allocation_granularity()
{
	SYSTEM_INFO info {};
	GetSystemInfo(&info);
	return info.dwAllocationGranularity;
}

bool shared_range_is_valid(const SharedBacking* backing, uint64_t backing_offset, uint64_t size)
{
	return backing != nullptr && backing->mapping != nullptr && size != 0 && backing_offset <= backing->size &&
	       size <= backing->size - backing_offset && size <= SIZE_MAX;
}

bool validate_shared_range(const SharedBacking* backing, uint64_t backing_offset, uint64_t size)
{
	const auto granularity = get_allocation_granularity();
	return shared_range_is_valid(backing, backing_offset, size) && backing_offset % granularity == 0;
}

uint64_t map_shared_at(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
	                    VirtualMemory::Mode mode)
{
	const auto granularity = get_allocation_granularity();
	if (!shared_range_is_valid(backing, backing_offset, size) || granularity == 0)
	{
		return 0;
	}

	// MapViewOfFile requires the section offset to be aligned to the host
	// allocation granularity (64 KiB on Windows), while guest direct memory is
	// page-aligned at 16 KiB. Map the containing section range and expose the
	// requested guest page inside that view. The visible address is tracked
	// separately so unmapping releases the actual section base.
	const auto offset_delta = backing_offset % granularity;
	const auto section_offset = backing_offset - offset_delta;
	if (offset_delta > UINT64_MAX - size || !shared_range_is_valid(backing, section_offset, size + offset_delta))
	{
		return 0;
	}
	const auto view_size = size + offset_delta;

	const auto offset_high = static_cast<DWORD>(section_offset >> 32u);
	const auto offset_low  = static_cast<DWORD>(section_offset & 0xffffffffu);
	auto*      view        = MapViewOfFileEx(backing->mapping, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, offset_high, offset_low,
	                                          static_cast<SIZE_T>(view_size), reinterpret_cast<LPVOID>(address));
	if (view == nullptr)
	{
		return 0;
	}

	const auto mapped_address = reinterpret_cast<uint64_t>(view);
	if (mapped_address != address)
	{
		UnmapViewOfFile(view);
		return 0;
	}

	auto* committed = VirtualAlloc(view, static_cast<SIZE_T>(view_size), MEM_COMMIT, PAGE_READWRITE);
	if (committed != view)
	{
		printf("VirtualAlloc(shared view) failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		UnmapViewOfFile(view);
		return 0;
	}

	DWORD old_protect = 0;
	if (VirtualProtect(view, static_cast<SIZE_T>(view_size), get_protection_flag(mode), &old_protect) == 0)
	{
		printf("VirtualProtect(shared view) failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		UnmapViewOfFile(view);
		return 0;
	}

	if (mapped_address > UINT64_MAX - offset_delta)
	{
		UnmapViewOfFile(view);
		return 0;
	}
	const auto visible_address = mapped_address + offset_delta;
	if (visible_address < address)
	{
		UnmapViewOfFile(view);
		return 0;
	}

	{
		std::scoped_lock lock(g_shared_views_mutex);
		g_shared_views.emplace(visible_address, SharedView {mapped_address, view_size});
	}
	return visible_address;
}

uint64_t probe_shared_range(SharedBacking* backing, uint64_t begin, uint64_t end, uint64_t backing_offset, uint64_t size,
	                         VirtualMemory::Mode mode, uint64_t alignment)
{
	if (begin > end || size - 1 > end - begin)
	{
		return 0;
	}

	uint64_t candidate = 0;
	if (!try_align_up(begin, alignment, &candidate))
	{
		return 0;
	}
	while (candidate <= end && size - 1 <= end - candidate)
	{
		if (const auto mapped = map_shared_at(backing, candidate, backing_offset, size, mode); mapped != 0)
		{
			return mapped;
		}
		if (alignment > end - candidate)
		{
			break;
		}
		candidate += alignment;
	}
	return 0;
}

} // namespace

void sys_get_system_info(SystemInfo* info)
{
	EXIT_IF(info == nullptr);

	const auto* p = cpuinfo_get_package(0);

	EXIT_IF(p == nullptr);

	info->ProcessorName = String::FromUtf8(p->name);
}

static DWORD get_protection_flag(VirtualMemory::Mode mode)
{
	DWORD protect = PAGE_NOACCESS;
	switch (mode)
	{
		case VirtualMemory::Mode::Read: protect = PAGE_READONLY; break;

		case VirtualMemory::Mode::Write:
		case VirtualMemory::Mode::ReadWrite: protect = PAGE_READWRITE; break;

		case VirtualMemory::Mode::Execute: protect = PAGE_EXECUTE; break;

		case VirtualMemory::Mode::ExecuteRead: protect = PAGE_EXECUTE_READ; break;

		case VirtualMemory::Mode::ExecuteWrite:
		case VirtualMemory::Mode::ExecuteReadWrite: protect = PAGE_EXECUTE_READWRITE; break;

		case VirtualMemory::Mode::NoAccess:
		default: protect = PAGE_NOACCESS; break;
	}
	return protect;
}

static VirtualMemory::Mode get_protection_flag(DWORD mode)
{
	switch (mode)
	{
		case PAGE_NOACCESS: return VirtualMemory::Mode::NoAccess;
		case PAGE_READONLY: return VirtualMemory::Mode::Read;
		case PAGE_READWRITE: return VirtualMemory::Mode::ReadWrite;
		case PAGE_WRITECOPY: return VirtualMemory::Mode::ReadWrite;
		case PAGE_EXECUTE: return VirtualMemory::Mode::Execute;
		case PAGE_EXECUTE_READ: return VirtualMemory::Mode::ExecuteRead;
		case PAGE_EXECUTE_READWRITE: return VirtualMemory::Mode::ExecuteReadWrite;
		case PAGE_EXECUTE_WRITECOPY: return VirtualMemory::Mode::ExecuteReadWrite;
		default: return VirtualMemory::Mode::NoAccess;
	}
}

void sys_virtual_init()
{
	cpuinfo_initialize();
}

uint64_t sys_virtual_get_page_size()
{
	SYSTEM_INFO info {};
	GetSystemInfo(&info);
	return info.dwPageSize;
}

uint64_t sys_virtual_alloc(uint64_t address, uint64_t size, VirtualMemory::Mode mode)
{
	auto ptr = (address == 0 ? sys_virtual_alloc_aligned(address, size, mode, 1)
	                         : reinterpret_cast<uintptr_t>(VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                                                                    static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
	                                                                    get_protection_flag(mode))));
	if (ptr == 0)
	{
		auto err = static_cast<uint32_t>(GetLastError());

		if (err != ERROR_INVALID_ADDRESS)
		{
			printf("VirtualAlloc() failed: 0x%08" PRIx32 "\n", err);
		} else
		{
			return sys_virtual_alloc_aligned(address, size, mode, 1);
		}
	}
	return ptr;
}

using VirtualAlloc2_func_t = /*WINBASEAPI*/ PVOID WINAPI (*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

static VirtualAlloc2_func_t ResolveVirtualAlloc2()
{
	HMODULE h = GetModuleHandle("KernelBase"); // @suppress("Invalid arguments")
	if (h != nullptr)
	{
		return reinterpret_cast<VirtualAlloc2_func_t>(GetProcAddress(h, "VirtualAlloc2"));
	}
	return nullptr;
}

static uint64_t align_up(uint64_t addr, uint64_t alignment)
{
	return (addr + alignment - 1) & ~(alignment - 1);
}

static bool try_align_up(uint64_t addr, uint64_t alignment, uint64_t* result)
{
	if (result == nullptr || !is_power_of_two(alignment) || addr > UINT64_MAX - (alignment - 1))
	{
		return false;
	}
	*result = align_up(addr, alignment);
	return true;
}

uint64_t sys_virtual_alloc_aligned(uint64_t address, uint64_t size, VirtualMemory::Mode mode, uint64_t alignment)
{
	if (alignment == 0)
	{
		printf("VirtualAlloc2 failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return 0;
	}

	MEM_ADDRESS_REQUIREMENTS req {};
	MEM_EXTENDED_PARAMETER   param {};
	req.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MIN) : reinterpret_cast<PVOID>(align_up(address, alignment)));
	req.HighestEndingAddress = (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MAX) : reinterpret_cast<PVOID>(USER_MAX));
	req.Alignment            = alignment;
	param.Type               = MemExtendedParameterAddressRequirements;
	param.Pointer            = &req;

	MEM_ADDRESS_REQUIREMENTS req2 {};
	MEM_EXTENDED_PARAMETER   param2 {};
	req2.LowestStartingAddress = (address == 0 ? reinterpret_cast<PVOID>(USER_MIN) : reinterpret_cast<PVOID>(align_up(address, alignment)));
	req2.HighestEndingAddress  = reinterpret_cast<PVOID>(USER_MAX);
	req2.Alignment             = alignment;
	param2.Type                = MemExtendedParameterAddressRequirements;
	param2.Pointer             = &req2;

	static auto virtual_alloc2 = ResolveVirtualAlloc2();

	if (virtual_alloc2 == nullptr)
	{
		const auto granularity = get_allocation_granularity();
		if (alignment > granularity)
		{
			return 0;
		}
		return reinterpret_cast<uint64_t>(
		    VirtualAlloc(address == 0 ? nullptr : reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
		                 MEM_COMMIT | MEM_RESERVE, get_protection_flag(mode)));
	}

	auto ptr = reinterpret_cast<uintptr_t>(virtual_alloc2(GetCurrentProcess(), nullptr, size,
	                                                      static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
	                                                      get_protection_flag(mode), &param, 1));

	if (ptr == 0)
	{
		ptr = reinterpret_cast<uintptr_t>(virtual_alloc2(GetCurrentProcess(), nullptr, size,
		                                                 static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
		                                                 get_protection_flag(mode), &param2, 1));
	}

	if (ptr == 0)
	{
		auto err = static_cast<uint32_t>(GetLastError());
		if (err != ERROR_INVALID_PARAMETER)
		{
			printf("VirtualAlloc2(alignment = 0x%016" PRIx64 ") failed: 0x%08" PRIx32 "\n", alignment, err);
		} else
		{
			return sys_virtual_alloc_aligned(address, size, mode, alignment << 1u);
		}
	}
	return ptr;
}

uint64_t sys_virtual_reserve(uint64_t address, uint64_t size)
{
	return sys_virtual_reserve_aligned(address, size, 1);
}

uint64_t sys_virtual_reserve_aligned(uint64_t address, uint64_t size, uint64_t alignment)
{
	if (alignment == 0)
	{
		return 0;
	}

	MEM_ADDRESS_REQUIREMENTS req {};
	MEM_EXTENDED_PARAMETER   param {};
	req.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MIN) : reinterpret_cast<PVOID>(align_up(address, alignment)));
	req.HighestEndingAddress = (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MAX) : reinterpret_cast<PVOID>(USER_MAX));
	req.Alignment            = alignment;
	param.Type               = MemExtendedParameterAddressRequirements;
	param.Pointer            = &req;

	MEM_ADDRESS_REQUIREMENTS req2 {};
	MEM_EXTENDED_PARAMETER   param2 {};
	req2.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(USER_MIN) : reinterpret_cast<PVOID>(align_up(address, alignment)));
	req2.HighestEndingAddress = reinterpret_cast<PVOID>(USER_MAX);
	req2.Alignment            = alignment;
	param2.Type               = MemExtendedParameterAddressRequirements;
	param2.Pointer            = &req2;

	static auto virtual_alloc2 = ResolveVirtualAlloc2();
	if (virtual_alloc2 == nullptr)
	{
		const auto granularity = get_allocation_granularity();
		if (alignment > granularity)
		{
			return 0;
		}
		const auto ptr = reinterpret_cast<uint64_t>(
		    VirtualAlloc(address == 0 ? nullptr : reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size, MEM_RESERVE,
		                 PAGE_NOACCESS));
		if (ptr != 0)
		{
			register_reservation_root(ptr, size);
		}
		return ptr;
	}

	auto ptr = reinterpret_cast<uintptr_t>(
	    virtual_alloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE, PAGE_NOACCESS, &param, 1));
	if (ptr == 0)
	{
		ptr = reinterpret_cast<uintptr_t>(
		    virtual_alloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE, PAGE_NOACCESS, &param2, 1));
	}
	if (ptr == 0)
	{
		const auto err = static_cast<uint32_t>(GetLastError());
		if (err == ERROR_INVALID_PARAMETER && alignment <= (UINT64_MAX >> 1u))
		{
			return sys_virtual_reserve_aligned(address, size, alignment << 1u);
		}
		printf("VirtualAlloc2 reserve (alignment = 0x%016" PRIx64 ") failed: 0x%08" PRIx32 "\n", alignment, err);
	} else
	{
		register_reservation_root(ptr, size);
	}
	return ptr;
}

bool sys_virtual_reserve_fixed(uint64_t address, uint64_t size)
{
	if (find_reservation_root(address, size, nullptr) && range_is_uncommitted(address, size))
	{
		// A guest reservation can be split at 16 KiB boundaries, while Windows
		// owns the containing VirtualAlloc reservation as one larger region.
		// The address space is already reserved; only Kyty's logical ownership
		// needs to be restored for this sub-range.
		return true;
	}

	auto* ptr = VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size, MEM_RESERVE, PAGE_NOACCESS);
	if (ptr == nullptr)
	{
		printf("VirtualAlloc reserve failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	if (reinterpret_cast<uint64_t>(ptr) != address)
	{
		VirtualFree(ptr, 0, MEM_RELEASE);
		return false;
	}
	register_reservation_root(address, size);
	return true;
}

bool sys_virtual_alloc_fixed(uint64_t address, uint64_t size, VirtualMemory::Mode mode)
{
	auto ptr = reinterpret_cast<uintptr_t>(VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                                                    static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
	                                                    get_protection_flag(mode)));
	if (ptr == 0)
	{
		auto err = static_cast<uint32_t>(GetLastError());

		printf("VirtualAlloc() failed: 0x%08" PRIx32 "\n", err);

		return false;
	}

	if (ptr != address)
	{
		printf("VirtualAlloc() failed: wrong address\n");
		VirtualFree(reinterpret_cast<LPVOID>(ptr), 0, MEM_RELEASE);
		return false;
	}

	return true;
}

bool sys_virtual_alloc_fixed_replacing_owned_reservation(uint64_t address, uint64_t size, VirtualMemory::Mode mode)
{
	if (address == 0 || size == 0 || !find_reservation_root(address, size, nullptr) || !range_is_uncommitted(address, size))
	{
		return false;
	}

	auto* committed = VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), static_cast<SIZE_T>(size),
	                               MEM_COMMIT, get_protection_flag(mode));
	if (committed != reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)))
	{
		return false;
	}

	std::scoped_lock lock(g_reservations_mutex);
	if (!g_private_shared_views.emplace(address, size).second)
	{
		VirtualFree(committed, static_cast<SIZE_T>(size), MEM_DECOMMIT);
		return false;
	}
	return true;
}

void* sys_virtual_create_shared_backing(uint64_t size)
{
	if (size == 0)
	{
		return nullptr;
	}

	auto* backing = new SharedBacking {};
	backing->mapping = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE,
	                                     static_cast<DWORD>(size >> 32u), static_cast<DWORD>(size & 0xffffffffu), nullptr);
	if (backing->mapping == nullptr)
	{
		printf("CreateFileMapping() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		delete backing;
		return nullptr;
	}
	backing->size = size;
	return backing;
}

void sys_virtual_destroy_shared_backing(void* backing)
{
	auto* shared = static_cast<SharedBacking*>(backing);
	if (shared == nullptr)
	{
		return;
	}
	if (shared->mapping != nullptr && CloseHandle(shared->mapping) == 0)
	{
		printf("CloseHandle(shared backing) failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
	}
	delete shared;
}

bool sys_virtual_discard_shared_backing_range(void* backing, uint64_t backing_offset, uint64_t size)
{
	// SEC_RESERVE file mappings do not expose a portable punch-hole path that
	// reclaims committed pages while other views may still exist. Release/unmap
	// already drop host commit via UnmapViewOfFile; treat discard as success so
	// callers share one control flow with Linux.
	auto* shared = static_cast<SharedBacking*>(backing);
	if (shared == nullptr || shared->mapping == nullptr || size == 0 || backing_offset > shared->size ||
	    size > shared->size - backing_offset)
	{
		return false;
	}
	return true;
}

uint64_t sys_virtual_map_shared_aligned(void* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
	                                    VirtualMemory::Mode mode, uint64_t alignment)
{
	auto* shared = static_cast<SharedBacking*>(backing);
	if (!shared_range_is_valid(shared, backing_offset, size) || !is_power_of_two(alignment))
	{
		return 0;
	}

	const auto granularity = get_allocation_granularity();
	if (alignment < granularity)
	{
		alignment = granularity;
	}

	if (address != 0)
	{
		return probe_shared_range(shared, address, USER_MAX, backing_offset, size, mode, alignment);
	}

	if (const auto mapped = probe_shared_range(shared, SYSTEM_MANAGED_MIN, SYSTEM_MANAGED_MAX, backing_offset, size, mode,
	                                           alignment);
	    mapped != 0)
	{
		return mapped;
	}
	return probe_shared_range(shared, USER_MIN, USER_MAX, backing_offset, size, mode, alignment);
}

bool sys_virtual_map_shared_fixed(void* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
	                              VirtualMemory::Mode mode)
{
	auto*      shared      = static_cast<SharedBacking*>(backing);
	const auto granularity = get_allocation_granularity();
	if (!validate_shared_range(shared, backing_offset, size) || address == 0 || address % granularity != 0)
	{
		return false;
	}
	return map_shared_at(shared, address, backing_offset, size, mode) == address;
}

bool sys_virtual_map_shared_fixed_replacing_owned_reservation(void* backing, uint64_t address, uint64_t backing_offset,
                                                              uint64_t size, VirtualMemory::Mode mode)
{
	auto* shared = static_cast<SharedBacking*>(backing);
	if (!shared_range_is_valid(shared, backing_offset, size) || address == 0 ||
	    !find_reservation_root(address, size, nullptr) || !range_is_uncommitted(address, size))
	{
		return false;
	}

	// Windows section views require both their address and file offset to have
	// the same 64 KiB allocation-granularity congruence. PS5 direct memory uses
	// 16 KiB pages, so mappings such as VA % 64 KiB == 0 with physical offset
	// % 64 KiB == 16 KiB cannot be represented by MapViewOfFile. Commit private
	// pages inside Kyty's owned reservation for that case. Coherent section
	// views remain the normal path for mappings that do not replace a reserved
	// guest range.
	auto* committed = VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), static_cast<SIZE_T>(size),
	                               MEM_COMMIT, get_protection_flag(mode));
	if (committed != reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)))
	{
		printf("VirtualAlloc(shared reservation fallback) failed: 0x%08" PRIx32 "\n",
		       static_cast<uint32_t>(GetLastError()));
		return false;
	}

	{
		std::scoped_lock lock(g_reservations_mutex);
		if (!g_private_shared_views.emplace(address, size).second)
		{
			VirtualFree(committed, static_cast<SIZE_T>(size), MEM_DECOMMIT);
			return false;
		}
	}
	return true;
}

bool sys_virtual_supports_shared_fixed_owned_reservation_replacement()
{
	return true;
}

uint64_t sys_virtual_map_shared_fixed_or_relocated(void* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
                                                   VirtualMemory::Mode mode, uint64_t /*alignment*/)
{
	return sys_virtual_map_shared_fixed(backing, address, backing_offset, size, mode) ? address : 0;
}

bool sys_virtual_free(uint64_t address)
{
	{
		std::scoped_lock lock(g_shared_views_mutex);
		const auto view = g_shared_views.find(address);
		if (view != g_shared_views.end())
		{
			if (UnmapViewOfFile(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(view->second.mapped_address))) == 0)
			{
				printf("UnmapViewOfFile() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
				return false;
			}
			g_shared_views.erase(address);
			return true;
		}
	}
	{
		std::scoped_lock lock(g_reservations_mutex);
		const auto       private_view = g_private_shared_views.find(address);
		if (private_view != g_private_shared_views.end())
		{
			if (VirtualFree(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)),
			                static_cast<SIZE_T>(private_view->second), MEM_DECOMMIT) == 0)
			{
				printf("VirtualFree(private shared view) failed: 0x%08" PRIx32 "\n",
				       static_cast<uint32_t>(GetLastError()));
				return false;
			}
			g_private_shared_views.erase(private_view);
			return true;
		}

		const auto reservation = std::find_if(g_reservation_roots.begin(), g_reservation_roots.end(),
		                                      [address](const ReservationRoot& root)
		                                      { return range_contains(root.address, root.size, address, 1); });
		if (reservation != g_reservation_roots.end())
		{
			if (reservation->address != address)
			{
				// Logical sub-ranges of a Windows reservation cannot be released
				// independently. Keep the host reservation and release ownership
				// in Kyty's ReservedMemory bookkeeping.
				return true;
			}
			if (VirtualFree(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), 0, MEM_RELEASE) == 0)
			{
				printf("VirtualFree(reservation) failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
				return false;
			}
			g_reservation_roots.erase(reservation);
			return true;
		}
	}
	if (VirtualFree(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), 0, MEM_RELEASE) == 0)
	{
		printf("VirtualFree() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool sys_virtual_protect(uint64_t address, uint64_t size, VirtualMemory::Mode mode, VirtualMemory::Mode* old_mode)
{
	std::scoped_lock transaction(g_protection_transaction_mutex);
	DWORD old_protect = 0;
	if (VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size, get_protection_flag(mode), &old_protect) == 0)
	{
		printf("VirtualProtect() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	if (old_mode != nullptr)
	{
		*old_mode = get_protection_flag(old_protect);
	}
	return true;
}

bool sys_virtual_is_range_readable(uint64_t address, uint64_t size)
{
	if (size == 0 || address > std::numeric_limits<uint64_t>::max() - size)
	{
		return false;
	}

	const uint64_t end = address + size;
	for (uint64_t cursor = address; cursor < end;)
	{
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(cursor)), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
		    (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		{
			return false;
		}
		const DWORD access = info.Protect & 0xffu;
		if (!(access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ ||
		      access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY))
		{
			return false;
		}
		const uint64_t region_start = reinterpret_cast<uint64_t>(info.BaseAddress);
		if (info.RegionSize == 0 || region_start > std::numeric_limits<uint64_t>::max() - info.RegionSize)
		{
			return false;
		}
		const uint64_t next = std::min(end, region_start + info.RegionSize);
		if (next <= cursor)
		{
			return false;
		}
		cursor = next;
	}
	return true;
}

static bool remove_write_protection(DWORD original, DWORD* target) noexcept
{
	const DWORD modifiers = original & ~0xffu;
	if ((modifiers & PAGE_GUARD) != 0)
	{
		return false;
	}
	switch (original & 0xffu)
	{
		case PAGE_READWRITE:
		case PAGE_WRITECOPY: *target = PAGE_READONLY | modifiers; return true;
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY: *target = PAGE_EXECUTE_READ | modifiers; return true;
		default: return false;
	}
}

VirtualMemory::ProtectionChangeResult sys_virtual_remove_write_and_capture(
	uint64_t address, uint64_t size, VirtualMemory::CapturedProtectionVisitor visitor, void* context) noexcept
{
	using Status = VirtualMemory::ProtectionChangeStatus;
	VirtualMemory::ProtectionChangeResult result {};
	if (visitor == nullptr || size == 0 || address > std::numeric_limits<uint64_t>::max() - size)
	{
		result.status = Status::InvalidRange;
		return result;
	}
	struct NativeRun
	{
		uint64_t address = 0;
		uint64_t size = 0;
		DWORD original = PAGE_NOACCESS;
		DWORD target = PAGE_NOACCESS;
	};
	std::vector<NativeRun> runs;
	const uint64_t end = address + size;
	std::scoped_lock transaction(g_protection_transaction_mutex);
	for (uint64_t cursor = address; cursor < end;)
	{
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(cursor)), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT)
		{
			result.status = Status::UnmappedRange;
			return result;
		}
		const uint64_t region_start = reinterpret_cast<uint64_t>(info.BaseAddress);
		if (region_start > std::numeric_limits<uint64_t>::max() - info.RegionSize)
		{
			result.status = Status::InvalidRange;
			return result;
		}
		const uint64_t run_end = std::min(end, region_start + info.RegionSize);
		DWORD target = PAGE_NOACCESS;
		if (!remove_write_protection(info.Protect, &target) || run_end <= cursor)
		{
			result.status = Status::UnsupportedProtection;
			return result;
		}
		runs.push_back({cursor, run_end - cursor, info.Protect, target});
		cursor = run_end;
	}

	auto rollback = [&]() noexcept
	{
		bool restored = true;
		for (const auto& run: runs)
		{
			DWORD ignored = 0;
			restored = VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(run.address)), run.size, run.original,
			                          &ignored) != 0 && restored;
		}
		return restored;
	};
	for (const auto& run: runs)
	{
		VirtualMemory::CapturedProtectionRun captured {run.address, run.size, get_protection_flag(run.original & 0xffu), run.original};
		if (!visitor(context, captured))
		{
			result.status = Status::ApplyFailedRolledBack;
			return result;
		}
	}
	for (const auto& run: runs)
	{
		DWORD ignored = 0;
		if (VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(run.address)), run.size, run.target, &ignored) == 0)
		{
			result.status = rollback() ? Status::ApplyFailedRolledBack : Status::RollbackFailed;
			return result;
		}
		result.applied_runs++;
		result.applied_bytes += run.size;
	}
	result.status = Status::Success;
	return result;
}

bool sys_virtual_remove_write_from_protection(uint64_t address, uint64_t size, uint32_t restore_token) noexcept
{
	DWORD target = PAGE_NOACCESS;
	if (size == 0 || !remove_write_protection(restore_token, &target))
	{
		return false;
	}
	std::scoped_lock transaction(g_protection_transaction_mutex);
	DWORD old_protect = 0;
	return VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size, target, &old_protect) != 0;
}

bool sys_virtual_restore_protection(uint64_t address, uint64_t size, uint32_t restore_token) noexcept
{
	std::scoped_lock transaction(g_protection_transaction_mutex);
	DWORD old_protect = 0;
	return VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size, restore_token, &old_protect) != 0;
}

bool sys_virtual_restore_protection_signal_safe(uint64_t address, uint64_t size, uint32_t restore_token) noexcept
{
	DWORD old_protect = 0;
	return size != 0 &&
	       VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size, restore_token, &old_protect) != 0;
}

bool sys_virtual_protect_write_signal_safe(uint64_t address, uint64_t size)
{
	DWORD old_protect = 0;
	return VirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size, PAGE_READWRITE, &old_protect) != 0;
}

bool sys_virtual_flush_instruction_cache(uint64_t address, uint64_t size)
{
	if (::FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size) == 0)
	{
		printf("FlushInstructionCache() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
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
