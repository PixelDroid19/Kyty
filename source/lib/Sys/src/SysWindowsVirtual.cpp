#include "Kyty/Core/Common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
//#error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/Sys/SysVirtual.h"

#include "cpuinfo.h"

#include <mutex>
#include <unordered_set>
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

std::mutex                   g_shared_views_mutex;
std::unordered_set<uint64_t> g_shared_views;

constexpr uint64_t SYSTEM_MANAGED_MIN = 0x0000040000u;
constexpr uint64_t SYSTEM_MANAGED_MAX = 0x07FFFFBFFFu;
constexpr uint64_t USER_MIN           = 0x1000000000u;
constexpr uint64_t USER_MAX           = 0xFBFFFFFFFFu;

bool is_power_of_two(uint64_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

uint64_t get_allocation_granularity()
{
	SYSTEM_INFO info {};
	GetSystemInfo(&info);
	return info.dwAllocationGranularity;
}

bool validate_shared_range(const SharedBacking* backing, uint64_t backing_offset, uint64_t size)
{
	const auto granularity = get_allocation_granularity();
	return backing != nullptr && backing->mapping != nullptr && size != 0 && backing_offset % granularity == 0 &&
	       backing_offset <= backing->size && size <= backing->size - backing_offset && size <= SIZE_MAX;
}

uint64_t map_shared_at(SharedBacking* backing, uint64_t address, uint64_t backing_offset, uint64_t size,
	                    VirtualMemory::Mode mode)
{
	const auto offset_high = static_cast<DWORD>(backing_offset >> 32u);
	const auto offset_low  = static_cast<DWORD>(backing_offset & 0xffffffffu);
	auto*      view        = MapViewOfFileEx(backing->mapping, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, offset_high, offset_low,
	                                          static_cast<SIZE_T>(size), reinterpret_cast<LPVOID>(address));
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

	auto* committed = VirtualAlloc(view, static_cast<SIZE_T>(size), MEM_COMMIT, PAGE_READWRITE);
	if (committed != view)
	{
		printf("VirtualAlloc(shared view) failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		UnmapViewOfFile(view);
		return 0;
	}

	DWORD old_protect = 0;
	if (VirtualProtect(view, static_cast<SIZE_T>(size), get_protection_flag(mode), &old_protect) == 0)
	{
		printf("VirtualProtect(shared view) failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		UnmapViewOfFile(view);
		return 0;
	}

	{
		std::scoped_lock lock(g_shared_views_mutex);
		g_shared_views.insert(mapped_address);
	}
	return mapped_address;
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
		case PAGE_EXECUTE: return VirtualMemory::Mode::Execute;
		case PAGE_EXECUTE_READ: return VirtualMemory::Mode::ExecuteRead;
		case PAGE_EXECUTE_READWRITE: return VirtualMemory::Mode::ExecuteReadWrite;
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

	EXIT_NOT_IMPLEMENTED(virtual_alloc2 == nullptr);

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
	if (!validate_shared_range(shared, backing_offset, size) || !is_power_of_two(alignment))
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

bool sys_virtual_free(uint64_t address)
{
	{
		std::scoped_lock lock(g_shared_views_mutex);
		if (g_shared_views.find(address) != g_shared_views.end())
		{
			if (UnmapViewOfFile(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address))) == 0)
			{
				printf("UnmapViewOfFile() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
				return false;
			}
			g_shared_views.erase(address);
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
