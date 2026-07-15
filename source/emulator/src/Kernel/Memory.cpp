#include "Emulator/Kernel/Memory.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <algorithm>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::Memory {

namespace VirtualMemory = Core::VirtualMemory;

LIB_NAME("libkernel", "libkernel");

class PhysicalMemory
{
public:
	struct AllocatedBlock
	{
		uint64_t start_addr;
		uint64_t size;
		int      memory_type;
	};

	struct MappedBlock
	{
		uint64_t                phys_addr;
		uint64_t                map_vaddr;
		uint64_t                map_size;
		int                     prot;
		VirtualMemory::Mode     mode;
		Graphics::GpuMemoryMode gpu_mode;
	};

	PhysicalMemory()
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
		m_backing = VirtualMemory::CreateSharedBacking(Size());
		EXIT_NOT_IMPLEMENTED(m_backing == nullptr);
	}
	virtual ~PhysicalMemory() { VirtualMemory::DestroySharedBacking(m_backing); }

	KYTY_CLASS_NO_COPY(PhysicalMemory);

	static uint64_t Size() { return static_cast<uint64_t>(5376) * 1024 * 1024; }

	bool Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment, uint64_t* phys_addr_out, int memory_type);
	bool Release(uint64_t start, size_t len);
	uint64_t Map(uint64_t vaddr, uint64_t phys_addr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode,
	             uint64_t alignment, bool fixed, bool* physical_range_valid);
	bool Unmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode);
	bool Find(uint64_t phys_addr, bool next, PhysicalMemory::AllocatedBlock* out);

	[[nodiscard]] Core::Mutex&               GetMutex() { return m_mutex; }
	[[nodiscard]] const Vector<MappedBlock>& GetMappedBlocks() const { return m_mapped; }

private:
	// Gen5 releases the physical reservation independently from its virtual mapping.
	// KernelMunmap owns the mapping and host/GPU cleanup lifecycle.
	// SharedBacking maps keep re-used physical ranges byte-coherent across aliases.
	Vector<AllocatedBlock>        m_allocated;
	Vector<MappedBlock>           m_mapped;
	Core::Mutex                   m_mutex;
	VirtualMemory::SharedBacking* m_backing = nullptr;
};

class FlexibleMemory
{
public:
	struct AllocatedBlock
	{
		uint64_t                map_vaddr;
		uint64_t                map_size;
		int                     prot;
		VirtualMemory::Mode     mode;
		Graphics::GpuMemoryMode gpu_mode;
	};

	FlexibleMemory() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~FlexibleMemory() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(FlexibleMemory);

	static uint64_t Size() { return static_cast<uint64_t>(448) * 1024 * 1024; }
	uint64_t        Available();

	bool Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode);
	bool Unmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode);

	[[nodiscard]] Core::Mutex&                  GetMutex() { return m_mutex; }
	[[nodiscard]] const Vector<AllocatedBlock>& GetBlocks() const { return m_allocated; }

private:
	Vector<AllocatedBlock> m_allocated;
	uint64_t               m_allocated_total = 0;
	Core::Mutex            m_mutex;
};

static PhysicalMemory* g_physical_memory = nullptr;
static FlexibleMemory* g_flexible_memory = nullptr;
static callback_func_t g_alloc_callback  = nullptr;
static callback_func_t g_free_callback   = nullptr;

KYTY_SUBSYSTEM_INIT(Memory)
{
	VirtualMemory::Init();

	g_physical_memory = new PhysicalMemory;
	g_flexible_memory = new FlexibleMemory;
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Memory) {}

KYTY_SUBSYSTEM_DESTROY(Memory) {}

static uint64_t get_aligned_pos(uint64_t pos, size_t align)
{
	return (align != 0 ? (pos + (align - 1)) & ~(align - 1) : pos);
}

void RegisterCallbacks(callback_func_t alloc_func, callback_func_t free_func)
{
	EXIT_IF(g_alloc_callback != nullptr || g_free_callback != nullptr);
	EXIT_IF(alloc_func == nullptr || free_func == nullptr);

	g_alloc_callback = alloc_func;
	g_free_callback  = free_func;

	g_physical_memory->GetMutex().Lock();
	for (const auto& b: g_physical_memory->GetMappedBlocks())
	{
		g_alloc_callback(b.map_vaddr, b.map_size);
	}
	g_physical_memory->GetMutex().Unlock();

	g_flexible_memory->GetMutex().Lock();
	for (const auto& b: g_flexible_memory->GetBlocks())
	{
		g_alloc_callback(b.map_vaddr, b.map_size);
	}
	g_flexible_memory->GetMutex().Unlock();
}

bool PhysicalMemory::Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment, uint64_t* phys_addr_out,
                           int memory_type)
{
	if (phys_addr_out == nullptr)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);

	uint64_t free_pos = 0;

	for (const auto& b: m_allocated)
	{
		uint64_t n = b.start_addr + b.size;
		if (n > free_pos)
		{
			free_pos = n;
		}
	}

	// Start the search at search_start, not below it: with no prior allocations
	// free_pos is 0, which would fail the `free_pos >= search_start` check and make
	// the first direct-memory allocation spuriously fail (seen as HashLink OOM).
	if (free_pos < search_start)
	{
		free_pos = search_start;
	}

	free_pos = get_aligned_pos(free_pos, alignment);

	if (free_pos >= search_start && free_pos + len <= search_end)
	{
		AllocatedBlock b {};
		b.size        = len;
		b.start_addr  = free_pos;
		b.memory_type = memory_type;

		m_allocated.Add(b);

		*phys_addr_out = free_pos;
		return true;
	}

	return false;
}

bool PhysicalMemory::Release(uint64_t start, size_t len)
{
	Core::LockGuard lock(m_mutex);

	uint32_t index = 0;
	for (auto& b: m_allocated)
	{
		if (start == b.start_addr && len == b.size)
		{
			m_allocated.RemoveAt(index);
			return true;
		}
		index++;
	}

	return false;
}

uint64_t PhysicalMemory::Map(uint64_t vaddr, uint64_t phys_addr, size_t len, int prot, VirtualMemory::Mode mode,
                             Graphics::GpuMemoryMode gpu_mode, uint64_t alignment, bool fixed, bool* physical_range_valid)
{
	EXIT_IF(physical_range_valid == nullptr);
	*physical_range_valid = false;

	Core::LockGuard lock(m_mutex);

	if (len == 0 || alignment == 0)
	{
		return 0;
	}

	const uint64_t map_size  = len;
	const bool     allocated = std::any_of(m_allocated.begin(), m_allocated.end(),
	                                       [phys_addr, map_size](const auto& b)
	                                       {
		                                       return phys_addr >= b.start_addr && map_size <= b.size &&
		                                              phys_addr - b.start_addr <= b.size - map_size;
	                                       });
	if (!allocated)
	{
		return 0;
	}
	*physical_range_valid = true;

	uint64_t map_vaddr = 0;
	if (fixed)
	{
		if ((vaddr & (alignment - 1)) == 0 && VirtualMemory::MapSharedFixed(m_backing, vaddr, phys_addr, map_size, mode))
		{
			map_vaddr = vaddr;
		}
	} else
	{
		map_vaddr = VirtualMemory::MapSharedAligned(m_backing, vaddr, phys_addr, map_size, mode, alignment);
	}

	if (map_vaddr == 0)
	{
		return 0;
	}

	MappedBlock b {};
	b.phys_addr = phys_addr;
	b.map_vaddr = map_vaddr;
	b.map_size  = map_size;
	b.prot      = prot;
	b.mode      = mode;
	b.gpu_mode  = gpu_mode;
	m_mapped.Add(b);

	return map_vaddr;
}

bool PhysicalMemory::Unmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode)
{
	EXIT_IF(gpu_mode == nullptr);

	Core::LockGuard lock(m_mutex);

	uint32_t index = 0;
	for (auto& b: m_mapped)
	{
		if (b.map_vaddr == vaddr && b.map_size == size)
		{
			if (!VirtualMemory::Free(vaddr))
			{
				return false;
			}
			*gpu_mode = b.gpu_mode;
			m_mapped.RemoveAt(index);
			return true;
		}
		index++;
	}

	return false;
}

bool PhysicalMemory::Find(uint64_t phys_addr, bool next, AllocatedBlock* out)
{
	EXIT_IF(out == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& b: m_allocated)
	{
		if (phys_addr >= b.start_addr && phys_addr < b.start_addr + b.size)
		{
			*out = b;
			return true;
		}
	}

	if (next)
	{
		uint64_t        min_start_addr = UINT64_MAX;
		AllocatedBlock* next           = nullptr;
		for (auto& b: m_allocated)
		{
			if (b.start_addr > phys_addr && b.start_addr < min_start_addr)
			{
				min_start_addr = b.start_addr;
				next           = &b;
			}
		}
		if (next != nullptr)
		{
			*out = *next;
			return true;
		}
	}

	return false;
}

bool PhysicalMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
                          Graphics::GpuMemoryMode* gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	return std::any_of(m_mapped.begin(), m_mapped.end(),
	                   [vaddr, base_addr, len, prot, mode, gpu_mode](auto& b)
	                   {
		                   if (vaddr >= b.map_vaddr && vaddr - b.map_vaddr < b.map_size)
		                   {
			                   if (base_addr != nullptr)
			                   {
				                   *base_addr = b.map_vaddr;
			                   }
			                   if (len != nullptr)
			                   {
				                   *len = b.map_size;
			                   }
			                   if (prot != nullptr)
			                   {
				                   *prot = b.prot;
			                   }
			                   if (mode != nullptr)
			                   {
				                   *mode = b.mode;
			                   }
			                   if (gpu_mode != nullptr)
			                   {
				                   *gpu_mode = b.gpu_mode;
			                   }

			                   return true;
		                   }
		                   return false;
	                   });
}

bool FlexibleMemory::Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	AllocatedBlock b {};
	b.map_vaddr = vaddr;
	b.map_size  = len;
	b.prot      = prot;
	b.mode      = mode;
	b.gpu_mode  = gpu_mode;

	m_allocated.Add(b);
	m_allocated_total += len;

	return true;
}

bool FlexibleMemory::Unmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode)
{
	EXIT_IF(gpu_mode == nullptr);

	Core::LockGuard lock(m_mutex);

	uint32_t index = 0;
	for (auto& b: m_allocated)
	{
		if (b.map_vaddr == vaddr && b.map_size == size)
		{
			if (!VirtualMemory::Free(vaddr))
			{
				return false;
			}
			*gpu_mode = b.gpu_mode;

			m_allocated.RemoveAt(index);
			m_allocated_total -= size;
			return true;
		}
		index++;
	}

	return false;
}

bool FlexibleMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
                          Graphics::GpuMemoryMode* gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	return std::any_of(m_allocated.begin(), m_allocated.end(),
	                   [vaddr, base_addr, len, prot, mode, gpu_mode](auto& b)
	                   {
		                   if (vaddr >= b.map_vaddr && vaddr < b.map_vaddr + b.map_size)
		                   {
			                   if (base_addr != nullptr)
			                   {
				                   *base_addr = b.map_vaddr;
			                   }
			                   if (len != nullptr)
			                   {
				                   *len = b.map_size;
			                   }
			                   if (prot != nullptr)
			                   {
				                   *prot = b.prot;
			                   }
			                   if (mode != nullptr)
			                   {
				                   *mode = b.mode;
			                   }
			                   if (gpu_mode != nullptr)
			                   {
				                   *gpu_mode = b.gpu_mode;
			                   }

			                   return true;
		                   }
		                   return false;
	                   });
}

uint64_t FlexibleMemory::Available()
{
	Core::LockGuard lock(m_mutex);

	return Size() - m_allocated_total;
}

int32_t KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags, const char* name)
{
	PRINT_NAME();

	EXIT_IF(g_flexible_memory == nullptr);

	EXIT_NOT_IMPLEMENTED(addr_in_out == nullptr);
	// PS5 titles pass flags such as 0x8000 (fixed-address request); accept the
	// known bits instead of bailing. Unknown bits still trip the assert.
	EXIT_NOT_IMPLEMENTED((flags & ~static_cast<int>(0x8000)) != 0);

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	switch (prot)
	{
		case 0: mode = VirtualMemory::Mode::NoAccess; break;
		case 1: mode = VirtualMemory::Mode::Read; break;
		case 2:
		case 3: mode = VirtualMemory::Mode::ReadWrite; break;
		case 4: mode = VirtualMemory::Mode::Execute; break;
		case 5: mode = VirtualMemory::Mode::ExecuteRead; break;
		case 6:
		case 7: mode = VirtualMemory::Mode::ExecuteReadWrite; break;
		case 0x32:
		case 0x33:
			mode     = VirtualMemory::Mode::ReadWrite;
			gpu_mode = Graphics::GpuMemoryMode::ReadWrite;
			break;
		default: EXIT("unknown prot: %d\n", prot);
	}

	auto in_addr  = reinterpret_cast<uint64_t>(*addr_in_out);
	auto out_addr = VirtualMemory::Alloc(in_addr, len, mode);
	*addr_in_out  = reinterpret_cast<void*>(out_addr);

	if (!g_flexible_memory->Map(out_addr, len, prot, mode, gpu_mode))
	{
		printf(FG_RED "\t [Fail]\n" FG_DEFAULT);
		VirtualMemory::Free(out_addr);
		return KERNEL_ERROR_ENOMEM;
	}

	printf("\t in_addr  = 0x%016" PRIx64 "\n", in_addr);
	printf("\t out_addr = 0x%016" PRIx64 "\n", out_addr);
	printf("\t size     = %" PRIu64 "\n", len);
	printf("\t mode     = %s\n", Core::EnumName(mode).C_Str());
	printf("\t name     = %s\n", name);
	printf("\t gpu_mode = %s\n", Core::EnumName(gpu_mode).C_Str());

	if (out_addr == 0)
	{
		return KERNEL_ERROR_ENOMEM;
	}

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		Graphics::GpuMemorySetAllocatedRange(out_addr, len);
	}

	if (g_alloc_callback != nullptr)
	{
		g_alloc_callback(out_addr, len);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags)
{
	return KernelMapNamedFlexibleMemory(addr_in_out, len, prot, flags, "");
}

int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len)
{
	PRINT_NAME();

	printf("\t start = 0x%016" PRIx64 "\n", vaddr);
	printf("\t len   = 0x%016" PRIx64 "\n", len);

	EXIT_IF(g_physical_memory == nullptr);
	EXIT_IF(g_flexible_memory == nullptr);

	if (vaddr < 0 || len == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	bool result = g_physical_memory->Unmap(vaddr, len, &gpu_mode);

	if (!result)
	{
		result = g_flexible_memory->Unmap(vaddr, len, &gpu_mode);
	}

	EXIT_NOT_IMPLEMENTED(!result);

	// Physical and flexible Unmap own VirtualMemory::Free for their views.

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		Graphics::GraphicsRunWait();
		Graphics::GpuMemoryFree(Graphics::WindowGetGraphicContext(), vaddr, len, true);
	}

	if (g_free_callback != nullptr)
	{
		g_free_callback(vaddr, len);
	}

	return OK;
}

size_t KYTY_SYSV_ABI KernelGetDirectMemorySize()
{
	PRINT_NAME();

	return PhysicalMemory::Size();
}

int KYTY_SYSV_ABI KernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	printf("\t offset    = 0x%016" PRIx64 "\n", offset);
	printf("\t flags     = 0x%08" PRIx32 "\n", flags);
	printf("\t info_size = 0x%016" PRIx64 "\n", info_size);

	struct QueryInfo
	{
		int64_t start;
		int64_t end;
		int     memory_type;
	};

	if (offset < 0 || info_size != sizeof(QueryInfo) || info == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	PhysicalMemory::AllocatedBlock block {};
	if (!g_physical_memory->Find(offset, flags != 0, &block))
	{
		printf(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EACCES;
	}

	auto* query_info = static_cast<QueryInfo*>(info);

	query_info->start       = static_cast<int64_t>(block.start_addr);
	query_info->end         = static_cast<int64_t>(block.start_addr + block.size);
	query_info->memory_type = block.memory_type;

	printf("\t start       = %016" PRIx64 "\n", query_info->start);
	printf("\t end         = %016" PRIx64 "\n", query_info->end);
	printf("\t memory_type = %d\n", query_info->memory_type);
	printf(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

int KYTY_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type,
                                             int64_t* phys_addr_out)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	printf("\t search_start = 0x%016" PRIx64 "\n", search_start);
	printf("\t search_end   = 0x%016" PRIx64 "\n", search_end);
	printf("\t len          = 0x%016" PRIx64 "\n", len);
	printf("\t alignment    = 0x%016" PRIx64 "\n", alignment);
	printf("\t memory_type  = %d\n", memory_type);

	if (search_start < 0 || search_end <= search_start || len == 0 || phys_addr_out == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t addr = 0;
	if (!g_physical_memory->Alloc(search_start, search_end, len, alignment, &addr, memory_type))
	{
		printf(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	printf("\tphys_addr    = %016" PRIx64 "\n", addr);
	printf(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

int KYTY_SYSV_ABI KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type, int64_t* phys_addr_out)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	printf("\t len          = 0x%016" PRIx64 "\n", len);
	printf("\t alignment    = 0x%016" PRIx64 "\n", alignment);
	printf("\t memory_type  = %d\n", memory_type);

	if (len == 0 || phys_addr_out == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t addr = 0;
	if (!g_physical_memory->Alloc(0, UINT64_MAX, len, alignment, &addr, memory_type))
	{
		printf(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	printf("\tphys_addr    = %016" PRIx64 "\n", addr);
	printf(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

static int release_direct_memory(int64_t start, size_t len)
{
	EXIT_IF(g_physical_memory == nullptr);

	if (start < 0 || len == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	bool result = g_physical_memory->Release(start, len);

	if (!result)
	{
		return KERNEL_ERROR_ENOENT;
	}

	return OK;
}

int KYTY_SYSV_ABI KernelReleaseDirectMemory(int64_t start, size_t len)
{
	PRINT_NAME();

	printf("\t start = 0x%016" PRIx64 "\n", start);
	printf("\t len   = 0x%016" PRIx64 "\n", len);

	return release_direct_memory(start, len);
}

int KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(int64_t start, size_t len)
{
	PRINT_NAME();

	printf("\t start = 0x%016" PRIx64 "\n", start);
	printf("\t len   = 0x%016" PRIx64 "\n", len);

	return release_direct_memory(start, len);
}

int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	EXIT_NOT_IMPLEMENTED(addr == nullptr);
	if (len == 0 || direct_memory_start < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (alignment == 0)
	{
		alignment = VirtualMemory::GetPageSize();
	}
	if ((alignment & (alignment - 1)) != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// The fixed-address request is bit 0x10; accept any other flag bits rather than
	// bailing (PS5 titles pass e.g. 0x11 = fixed + no-overwrite).
	bool fixed = (flags & 0x10) != 0;
	printf("\t flags        = 0x%x (fixed=%d)\n", flags, fixed ? 1 : 0);

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	switch (prot)
	{
		case 0x00: mode = VirtualMemory::Mode::NoAccess; break;
		case 0x01: mode = VirtualMemory::Mode::Read; break;
		case 0x02:
		case 0x03: mode = VirtualMemory::Mode::ReadWrite; break;
		case 0x04: mode = VirtualMemory::Mode::Execute; break;
		case 0x05: mode = VirtualMemory::Mode::ExecuteRead; break;
		case 0x06:
		case 0x07: mode = VirtualMemory::Mode::ExecuteReadWrite; break;
		case 0x32:
		case 0x33:
			mode     = VirtualMemory::Mode::ReadWrite;
			gpu_mode = Graphics::GpuMemoryMode::ReadWrite;
			break;
		default: EXIT("unknown prot: %d\n", prot);
	}

	auto in_addr = reinterpret_cast<uint64_t>(*addr);

	if (fixed)
	{
		EXIT_NOT_IMPLEMENTED(in_addr == 0);
		EXIT_NOT_IMPLEMENTED((in_addr & (alignment - 1)) != 0);
	}

	bool     physical_range_valid = false;
	uint64_t out_addr =
	    g_physical_memory->Map(in_addr, static_cast<uint64_t>(direct_memory_start), len, prot, mode, gpu_mode, alignment, fixed,
	                           &physical_range_valid);

	*addr = reinterpret_cast<void*>(out_addr);

	printf("\t in_addr  = 0x%016" PRIx64 "\n", in_addr);
	printf("\t out_addr = 0x%016" PRIx64 "\n", out_addr);
	printf("\t size     = 0x%016" PRIx64 "\n", len);
	printf("\t mode     = %s\n", Core::EnumName(mode).C_Str());
	printf("\t align    = 0x%016" PRIx64 "\n", alignment);
	printf("\t gpu_mode = %s\n", Core::EnumName(gpu_mode).C_Str());

	if (!physical_range_valid)
	{
		EXIT("direct-memory range is not allocated: phys=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     static_cast<uint64_t>(direct_memory_start), len);
	}

	if (out_addr == 0)
	{
		printf(FG_RED "\t [Fail]\n" FG_DEFAULT);
		return fixed ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_ENOMEM;
	}

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		Graphics::GpuMemorySetAllocatedRange(out_addr, len);
	}

	if (g_alloc_callback != nullptr)
	{
		g_alloc_callback(out_addr, len);
	}

	printf(FG_GREEN "\t [Ok]\n" FG_DEFAULT);

	return OK;
}

int KYTY_SYSV_ABI KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags, off_t direct_memory_start, size_t alignment,
                                             const char* name)
{
	PRINT_NAME();

	printf("\t name = %s\n", name);

	return KernelMapDirectMemory(addr, len, prot, flags, direct_memory_start, alignment);
}

int KYTY_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);
	EXIT_IF(g_flexible_memory == nullptr);

	EXIT_NOT_IMPLEMENTED(addr == nullptr);

	size_t   len  = 0;
	int      p    = 0;
	uint64_t base = 0;

	if (!g_physical_memory->Find(reinterpret_cast<uint64_t>(addr), &base, &len, &p, nullptr, nullptr))
	{
		if (!g_flexible_memory->Find(reinterpret_cast<uint64_t>(addr), &base, &len, &p, nullptr, nullptr))
		{
			return KERNEL_ERROR_EACCES;
		}
	}

	if (start != nullptr)
	{
		*start = reinterpret_cast<void*>(base);
	}
	if (end != nullptr)
	{
		*end = reinterpret_cast<void*>(base + len - 1);
	}
	if (prot != nullptr)
	{
		*prot = p;
	}

	return OK;
}

int KYTY_SYSV_ABI KernelAvailableFlexibleMemorySize(size_t* size)
{
	PRINT_NAME();

	EXIT_IF(g_flexible_memory == nullptr);

	EXIT_NOT_IMPLEMENTED(size == nullptr);

	*size = g_flexible_memory->Available();

	printf("\t *size = 0x%016" PRIx64 "\n", *size);

	return OK;
}

bool KernelDecodeMprotectProt(int prot, Core::VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode)
{
	EXIT_IF(mode == nullptr || gpu_mode == nullptr);

	// Orbis/PS5 protection mixes CPU bits (low nibble) with GPU/AMPR-style
	// high bits. Historical forms: 0x11 = CPU_READ|GPU_READ, 0x12 =
	// CPU_WRITE|GPU_READ. Observed Gen5 boot: 0xC2 = CPU_WRITE | high AMPR
	// flags (0xC0); map CPU write to host ReadWrite and GPU to ReadWrite.
	switch (prot)
	{
		case 0x11:
			*mode     = VirtualMemory::Mode::Read;
			*gpu_mode = Graphics::GpuMemoryMode::Read;
			return true;
		case 0x12:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = Graphics::GpuMemoryMode::Read;
			return true;
		case 0xC2:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = Graphics::GpuMemoryMode::ReadWrite;
			return true;
		default: return false;
	}
}

int KYTY_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot)
{
	PRINT_NAME();

	auto vaddr = reinterpret_cast<uint64_t>(addr);

	printf("\t addr = 0x%016" PRIx64 "\n", vaddr);
	printf("\t len  = 0x%016" PRIx64 "\n", static_cast<uint64_t>(len));

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	if (!KernelDecodeMprotectProt(prot, &mode, &gpu_mode))
	{
		EXIT("unknown prot: 0x%x (%d)\n", prot, prot);
	}

	VirtualMemory::Mode old_mode {};
	bool                ok = VirtualMemory::Protect(vaddr, len, mode, &old_mode);

	EXIT_NOT_IMPLEMENTED(!ok);

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		Graphics::GpuMemorySetAllocatedRange(vaddr, len);
	}

	printf("\t prot: %s -> %s\n", Core::EnumName(old_mode).C_Str(), Core::EnumName(mode).C_Str());

	return OK;
}

} // namespace Kyty::Libs::LibKernel::Memory

#endif // KYTY_EMU_ENABLED
