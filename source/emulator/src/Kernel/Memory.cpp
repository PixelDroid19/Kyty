#include "Emulator/Kernel/Memory.h"

#include "Emulator/Config.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Kernel/Pthread.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <chrono>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::Memory {

namespace VirtualMemory = Core::VirtualMemory;

LIB_NAME("libkernel", "libkernel");

static bool is_representable_range(uint64_t addr, uint64_t size)
{
	return size != 0 && size <= UINT64_MAX - addr;
}

KernelGpuMappingPromotionStatus KernelPromoteGpuMappingRange(uint64_t mapping_addr, uint64_t mapping_size, uint64_t protected_addr,
                                                             uint64_t protected_size, Graphics::GpuMemoryMode requested_mode,
                                                             Graphics::GpuMemoryMode* cleanup_mode)
{
	if (cleanup_mode == nullptr || !is_representable_range(mapping_addr, mapping_size) ||
	    !is_representable_range(protected_addr, protected_size))
	{
		return KernelGpuMappingPromotionStatus::InvalidArgument;
	}
	if (protected_addr < mapping_addr || protected_size > mapping_size ||
	    protected_addr - mapping_addr > mapping_size - protected_size)
	{
		return KernelGpuMappingPromotionStatus::NotContained;
	}
	if (requested_mode != Graphics::GpuMemoryMode::NoAccess && *cleanup_mode == Graphics::GpuMemoryMode::NoAccess)
	{
		*cleanup_mode = requested_mode;
		return KernelGpuMappingPromotionStatus::Promoted;
	}
	return KernelGpuMappingPromotionStatus::Retained;
}

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
		int                     memory_type;
		// Monotonic lifetime marker consumed by ClaimUnmap. It records that
		// cleanup is required, not the mapping's latest protection.
		Graphics::GpuMemoryMode gpu_cleanup_mode;
		bool                    unmap_pending = false;
	};

	PhysicalMemory()
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
		// The backing is sized for the largest supported generation before the
		// loader identifies the guest. Allocation policy still exposes only the
		// guest generation's capacity.
		m_backing = VirtualMemory::CreateSharedBacking(BackingSize());
		EXIT_NOT_IMPLEMENTED(m_backing == nullptr);
	}
	virtual ~PhysicalMemory() { VirtualMemory::DestroySharedBacking(m_backing); }

	KYTY_CLASS_NO_COPY(PhysicalMemory);

	static uint64_t Size()
	{
		constexpr uint64_t kLegacyDirectMemory = static_cast<uint64_t>(5376) * 1024 * 1024;
		constexpr uint64_t kNextGenSystemMemory = static_cast<uint64_t>(16) * 1024 * 1024 * 1024;
		return Config::IsNextGen() ? kNextGenSystemMemory : kLegacyDirectMemory;
	}
	static constexpr uint64_t BackingSize() { return static_cast<uint64_t>(16) * 1024 * 1024 * 1024; }

	bool Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment, uint64_t* phys_addr_out, int memory_type);
	bool Release(uint64_t start, size_t len);
	bool FindMappingsForPhysicalRelease(uint64_t start, size_t len, Vector<MappedBlock>* mappings);
	uint64_t Map(uint64_t vaddr, uint64_t phys_addr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode,
	             uint64_t alignment, bool fixed, bool replace_owned_reservation, bool* physical_range_valid);
	bool ClaimUnmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode);
	bool CompleteUnmap(uint64_t vaddr, uint64_t size);
	KernelGpuMappingPromotionStatus PromoteGpuRange(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode gpu_mode,
	                                               uint64_t* mapping_addr, uint64_t* mapping_size);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
	          Graphics::GpuMemoryMode* gpu_mode, uint64_t* phys_addr = nullptr, int* memory_type = nullptr);
	bool Find(uint64_t phys_addr, bool next, PhysicalMemory::AllocatedBlock* out);
	uint64_t TotalAllocatedBytes();
	bool     FindLargestAvailableSpan(uint64_t search_start, uint64_t search_end, uint64_t alignment, uint64_t* span_start,
	                                uint64_t* span_length);

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
		Graphics::GpuMemoryMode gpu_cleanup_mode;
		bool                    unmap_pending = false;
	};

	FlexibleMemory() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~FlexibleMemory() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(FlexibleMemory);

	static uint64_t Size() { return static_cast<uint64_t>(448) * 1024 * 1024; }
	uint64_t        Available();

	bool Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode);
	bool ClaimUnmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode);
	bool CompleteUnmap(uint64_t vaddr, uint64_t size);
	KernelGpuMappingPromotionStatus PromoteGpuRange(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode gpu_mode,
	                                               uint64_t* mapping_addr, uint64_t* mapping_size);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode);

	[[nodiscard]] Core::Mutex&                  GetMutex() { return m_mutex; }
	[[nodiscard]] const Vector<AllocatedBlock>& GetBlocks() const { return m_allocated; }

private:
	Vector<AllocatedBlock> m_allocated;
	uint64_t               m_allocated_total = 0;
	Core::Mutex            m_mutex;
};

class ReservedMemory
{
public:
	struct Block
	{
		uint64_t addr;
		uint64_t size;
	};

	~ReservedMemory()
	{
		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_blocks)
		{
			VirtualMemory::Free(block.addr);
		}
	}

	KYTY_CLASS_NO_COPY(ReservedMemory);

	ReservedMemory() = default;

	bool Add(uint64_t addr, uint64_t size)
	{
		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_blocks)
		{
			if (addr < block.addr + block.size && block.addr < addr + size)
			{
				return false;
			}
		}
		m_blocks.Add(Block {addr, size});
		return true;
	}

	bool Find(uint64_t addr, uint64_t* base, uint64_t* size)
	{
		if (base == nullptr || size == nullptr)
		{
			return false;
		}

		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_blocks)
		{
			if (addr >= block.addr && addr - block.addr < block.size)
			{
				*base = block.addr;
				*size = block.size;
				return true;
			}
		}
		return false;
	}

	bool Unmap(uint64_t addr, uint64_t size)
	{
		Core::LockGuard lock(m_mutex);
		for (uint32_t index = 0; index < m_blocks.Size(); index++)
		{
			const auto& block = m_blocks[index];
			if (block.addr == addr && block.size == size)
			{
				if (!VirtualMemory::Free(addr))
				{
					return false;
				}
				m_blocks.RemoveAt(index);
				return true;
			}
		}
		return false;
	}

	bool Contains(uint64_t addr, uint64_t size)
	{
		if (size == 0 || addr > std::numeric_limits<uint64_t>::max() - size)
		{
			return false;
		}
		Core::LockGuard lock(m_mutex);
		return std::any_of(m_blocks.begin(), m_blocks.end(),
		                   [addr, size](const Block& block)
		                   { return addr >= block.addr && size <= block.size && addr - block.addr <= block.size - size; });
	}

	template <typename Mapper>
	bool ReplaceAndConsume(uint64_t addr, uint64_t size, Mapper&& mapper)
	{
		if (size == 0 || addr > std::numeric_limits<uint64_t>::max() - size)
		{
			return false;
		}

		Core::LockGuard lock(m_mutex);
		for (uint32_t index = 0; index < m_blocks.Size(); index++)
		{
			const Block block = m_blocks[index];
			if (addr < block.addr || size > block.size || addr - block.addr > block.size - size)
			{
				continue;
			}
			if (!mapper())
			{
				return false;
			}
			SplitConsumedBlock(index, block, addr, size);
			return true;
		}
		return false;
	}

	bool Consume(uint64_t addr, uint64_t size)
	{
		if (size == 0 || addr > std::numeric_limits<uint64_t>::max() - size)
		{
			return false;
		}

		Core::LockGuard lock(m_mutex);
		for (uint32_t index = 0; index < m_blocks.Size(); index++)
		{
			const Block block = m_blocks[index];
			if (addr < block.addr || size > block.size || addr - block.addr > block.size - size)
			{
				continue;
			}

			const uint64_t prefix_size = addr - block.addr;
			const uint64_t suffix_addr = addr + size;
			const uint64_t suffix_size = block.size - prefix_size - size;
			if (!VirtualMemory::Free(block.addr))
			{
				return false;
			}
			m_blocks.RemoveAt(index);

			const bool prefix_ok = prefix_size == 0 || VirtualMemory::AllocFixed(block.addr, prefix_size, VirtualMemory::Mode::NoAccess);
			const bool suffix_ok = suffix_size == 0 || VirtualMemory::AllocFixed(suffix_addr, suffix_size, VirtualMemory::Mode::NoAccess);
			if (!prefix_ok || !suffix_ok)
			{
				if (prefix_ok && prefix_size != 0)
				{
					EXIT_IF(!VirtualMemory::Free(block.addr));
				}
				if (suffix_ok && suffix_size != 0)
				{
					EXIT_IF(!VirtualMemory::Free(suffix_addr));
				}
				EXIT_IF(!VirtualMemory::AllocFixed(block.addr, block.size, VirtualMemory::Mode::NoAccess));
				m_blocks.Add(block);
				return false;
			}

			if (prefix_size != 0)
			{
				m_blocks.Add(Block {block.addr, prefix_size});
			}
			if (suffix_size != 0)
			{
				m_blocks.Add(Block {suffix_addr, suffix_size});
			}
			return true;
		}
		return false;
	}

private:
	void SplitConsumedBlock(uint32_t index, const Block& block, uint64_t addr, uint64_t size)
	{
		const uint64_t prefix_size = addr - block.addr;
		const uint64_t suffix_addr = addr + size;
		const uint64_t suffix_size = block.size - prefix_size - size;
		m_blocks.RemoveAt(index);
		if (prefix_size != 0)
		{
			m_blocks.Add(Block {block.addr, prefix_size});
		}
		if (suffix_size != 0)
		{
			m_blocks.Add(Block {suffix_addr, suffix_size});
		}
	}

	Vector<Block> m_blocks;
	Core::Mutex   m_mutex;
};

static PhysicalMemory* g_physical_memory = nullptr;
static FlexibleMemory* g_flexible_memory = nullptr;
static ReservedMemory* g_reserved_memory = nullptr;
static callback_func_t g_alloc_callback  = nullptr;
static callback_func_t g_free_callback   = nullptr;

KYTY_SUBSYSTEM_INIT(Memory)
{
	VirtualMemory::Init();

	g_physical_memory = new PhysicalMemory;
	g_flexible_memory = new FlexibleMemory;
	g_reserved_memory = new ReservedMemory;
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Memory) {}

KYTY_SUBSYSTEM_DESTROY(Memory) {}

static bool get_aligned_pos(uint64_t pos, size_t align, uint64_t* aligned_pos)
{
	EXIT_IF(aligned_pos == nullptr);

	if (align == 0)
	{
		*aligned_pos = pos;
		return true;
	}

	const uint64_t remainder = pos % align;
	if (remainder == 0)
	{
		*aligned_pos = pos;
		return true;
	}

	const uint64_t increment = align - remainder;
	if (pos > std::numeric_limits<uint64_t>::max() - increment)
	{
		return false;
	}
	*aligned_pos = pos + increment;
	return true;
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

	search_end = std::min(search_end, Size());
	if (search_start >= search_end || len > search_end - search_start)
	{
		return false;
	}

	uint64_t candidate = 0;
	if (!get_aligned_pos(search_start, alignment, &candidate))
	{
		return false;
	}

	// Direct-memory callers repeatedly request large contiguous heaps from the
	// same search window. Keep each window append-only so smaller allocations do
	// not fragment the only remaining range for a later heap. Blocks outside the
	// requested window must not advance this cursor: they are unrelated physical
	// ranges and previously made valid lower-window requests fail.
	for (const auto& block: m_allocated)
	{
		if (block.size > std::numeric_limits<uint64_t>::max() - block.start_addr)
		{
			return false;
		}

		const uint64_t block_end = block.start_addr + block.size;
		if (block_end > search_start && block.start_addr < search_end)
		{
			candidate = std::max(candidate, block_end);
		}
	}

	if (!get_aligned_pos(candidate, alignment, &candidate) || candidate > search_end - len)
	{
		return false;
	}

	AllocatedBlock block {};
	block.size        = len;
	block.start_addr  = candidate;
	block.memory_type = memory_type;

	const auto insert_pos = std::lower_bound(m_allocated.begin(), m_allocated.end(), block.start_addr,
	                                         [](const AllocatedBlock& existing, uint64_t start) { return existing.start_addr < start; });
	m_allocated.InsertAt(static_cast<uint32_t>(insert_pos - m_allocated.begin()), block);

	*phys_addr_out = candidate;
	return true;
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

			// Reclaim host RAM only when no live map still covers this physical
			// range. Gen5 may release the reservation while a mapping remains;
			// Unmap performs the discard in that case.
			bool still_mapped = false;
			for (const auto& mapped: m_mapped)
			{
				if (mapped.phys_addr < start + len && mapped.phys_addr + mapped.map_size > start)
				{
					still_mapped = true;
					break;
				}
			}
			if (!still_mapped)
			{
				(void)VirtualMemory::DiscardSharedBackingRange(m_backing, start, len);
			}
			return true;
		}
		index++;
	}

	return false;
}

bool PhysicalMemory::FindMappingsForPhysicalRelease(uint64_t start, size_t len, Vector<MappedBlock>* mappings)
{
	if (mappings == nullptr || len == 0 || start > std::numeric_limits<uint64_t>::max() - len)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);
	const bool allocation_exists = std::any_of(m_allocated.begin(), m_allocated.end(),
	                                           [start, len](const AllocatedBlock& block)
	                                           { return block.start_addr == start && block.size == len; });
	if (!allocation_exists)
	{
		return false;
	}

	mappings->Clear();
	for (const auto& mapping: m_mapped)
	{
		if (mapping.phys_addr < start + len && start < mapping.phys_addr + mapping.map_size)
		{
			mappings->Add(mapping);
		}
	}
	return true;
}

uint64_t PhysicalMemory::TotalAllocatedBytes()
{
	Core::LockGuard lock(m_mutex);

	uint64_t used = 0;
	for (const auto& block: m_allocated)
	{
		used = std::min(Size(), used + block.size);
	}
	return used;
}

bool PhysicalMemory::FindLargestAvailableSpan(uint64_t search_start, uint64_t search_end, uint64_t alignment, uint64_t* span_start,
                                              uint64_t* span_length)
{
	if (span_start == nullptr || span_length == nullptr || alignment == 0)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);

	*span_start  = 0;
	*span_length = 0;

	search_end = std::min(search_end, Size());
	if (search_start >= search_end)
	{
		return false;
	}

	uint64_t candidate = 0;
	if (!get_aligned_pos(search_start, alignment, &candidate) || candidate >= search_end)
	{
		return false;
	}

	Vector<AllocatedBlock> allocations = m_allocated;
	std::sort(allocations.begin(), allocations.end(),
	          [](const AllocatedBlock& left, const AllocatedBlock& right) { return left.start_addr < right.start_addr; });

	for (const auto& allocation: allocations)
	{
		const uint64_t allocation_end = allocation.start_addr + allocation.size;
		if (allocation_end <= candidate)
		{
			continue;
		}

		const uint64_t gap_end = std::min(allocation.start_addr, search_end);
		if (candidate < gap_end)
		{
			const uint64_t candidate_length = gap_end - candidate;
			if (candidate_length > *span_length)
			{
				*span_start  = candidate;
				*span_length = candidate_length;
			}
		}

		if (allocation.start_addr >= search_end)
		{
			break;
		}

		candidate = std::max(candidate, allocation_end);
		if (!get_aligned_pos(candidate, alignment, &candidate) || candidate >= search_end)
		{
			break;
		}
	}

	if (candidate < search_end)
	{
		const uint64_t candidate_length = search_end - candidate;
		if (candidate_length > *span_length)
		{
			*span_start  = candidate;
			*span_length = candidate_length;
		}
	}

	return *span_length != 0;
}

uint64_t PhysicalMemory::Map(uint64_t vaddr, uint64_t phys_addr, size_t len, int prot, VirtualMemory::Mode mode,
                             Graphics::GpuMemoryMode gpu_mode, uint64_t alignment, bool fixed, bool replace_owned_reservation,
                             bool* physical_range_valid)
{
	EXIT_IF(physical_range_valid == nullptr);
	*physical_range_valid = false;

	Core::LockGuard lock(m_mutex);

	if (len == 0 || alignment == 0)
	{
		return 0;
	}

	const uint64_t map_size = len;
	const auto allocation = std::find_if(m_allocated.begin(), m_allocated.end(),
	                                     [phys_addr, map_size](const auto& block)
	                                     {
		                                     return phys_addr >= block.start_addr && map_size <= block.size &&
		                                            phys_addr - block.start_addr <= block.size - map_size;
	                                     });
	if (allocation == m_allocated.end())
	{
		return 0;
	}
	*physical_range_valid = true;

	uint64_t map_vaddr = 0;
	if (fixed)
	{
		if ((vaddr & (alignment - 1)) == 0)
		{
			if (replace_owned_reservation)
			{
				map_vaddr = VirtualMemory::MapSharedFixedReplacingOwnedReservation(m_backing, vaddr, phys_addr, map_size, mode) ? vaddr : 0;
			} else
			{
				map_vaddr = VirtualMemory::MapSharedFixedOrRelocated(m_backing, vaddr, phys_addr, map_size, mode, alignment);
			}
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
	b.memory_type = allocation->memory_type;
	b.gpu_cleanup_mode = gpu_mode;
	m_mapped.Add(b);

	return map_vaddr;
}

bool PhysicalMemory::ClaimUnmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode)
{
	EXIT_IF(gpu_mode == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& b: m_mapped)
	{
		if (b.map_vaddr == vaddr && b.map_size == size && !b.unmap_pending)
		{
			*gpu_mode = b.gpu_cleanup_mode;
			b.unmap_pending = true;
			return true;
		}
	}

	return false;
}

KernelGpuMappingPromotionStatus PhysicalMemory::PromoteGpuRange(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode gpu_mode,
                                                               uint64_t* mapping_addr, uint64_t* mapping_size)
{
	if (mapping_addr == nullptr || mapping_size == nullptr || !is_representable_range(vaddr, size))
	{
		return KernelGpuMappingPromotionStatus::InvalidArgument;
	}

	Core::LockGuard lock(m_mutex);
	for (auto& block: m_mapped)
	{
		auto cleanup_mode = block.gpu_cleanup_mode;
		const auto result =
		    KernelPromoteGpuMappingRange(block.map_vaddr, block.map_size, vaddr, size, gpu_mode, &cleanup_mode);
		if (result == KernelGpuMappingPromotionStatus::NotContained)
		{
			continue;
		}
		if (result == KernelGpuMappingPromotionStatus::InvalidArgument)
		{
			return result;
		}
		if (block.unmap_pending)
		{
			return KernelGpuMappingPromotionStatus::UnmapPending;
		}
		block.gpu_cleanup_mode = cleanup_mode;
		*mapping_addr  = block.map_vaddr;
		*mapping_size  = block.map_size;
		return result;
	}
	return KernelGpuMappingPromotionStatus::NotContained;
}

bool PhysicalMemory::CompleteUnmap(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);

	uint32_t index = 0;
	for (auto& b: m_mapped)
	{
		if (b.map_vaddr == vaddr && b.map_size == size && b.unmap_pending)
		{
			const uint64_t phys_addr = b.phys_addr;
			const uint64_t map_size  = b.map_size;
			if (!VirtualMemory::Free(vaddr))
			{
				b.unmap_pending = false;
				return false;
			}
			m_mapped.RemoveAt(index);

			// If the physical reservation was already released (Gen5 unmap after
			// Release) and no alias maps remain, drop the host pages.
			bool still_allocated = false;
			for (const auto& allocated: m_allocated)
			{
				if (phys_addr >= allocated.start_addr && map_size <= allocated.size &&
				    phys_addr - allocated.start_addr <= allocated.size - map_size)
				{
					still_allocated = true;
					break;
				}
			}
			bool still_mapped = false;
			for (const auto& mapped: m_mapped)
			{
				if (mapped.phys_addr < phys_addr + map_size && mapped.phys_addr + mapped.map_size > phys_addr)
				{
					still_mapped = true;
					break;
				}
			}
			if (!still_allocated && !still_mapped)
			{
				(void)VirtualMemory::DiscardSharedBackingRange(m_backing, phys_addr, map_size);
			}
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

	const auto first_after = std::lower_bound(m_allocated.begin(), m_allocated.end(), phys_addr,
	                                          [](const AllocatedBlock& block, uint64_t address)
	                                          { return block.start_addr + block.size <= address; });
	if (first_after == m_allocated.end())
	{
		return false;
	}

	const bool contains_address = phys_addr >= first_after->start_addr && phys_addr - first_after->start_addr < first_after->size;
	if (!contains_address && !next)
	{
		return false;
	}

	*out = *first_after;
	for (auto current = first_after + 1; current != m_allocated.end() && current->memory_type == out->memory_type &&
	                                      current->start_addr == out->start_addr + out->size;
	     ++current)
	{
		out->size += current->size;
	}

	return true;
}

bool PhysicalMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
                          Graphics::GpuMemoryMode* gpu_mode, uint64_t* phys_addr, int* memory_type)
{
	Core::LockGuard lock(m_mutex);

	return std::any_of(m_mapped.begin(), m_mapped.end(),
	                   [vaddr, base_addr, len, prot, mode, gpu_mode, phys_addr, memory_type](auto& b)
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
					                   *gpu_mode = b.gpu_cleanup_mode;
				                   }
				                   if (phys_addr != nullptr)
				                   {
					                   *phys_addr = b.phys_addr;
				                   }
				                   if (memory_type != nullptr)
				                   {
					                   *memory_type = b.memory_type;
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
	b.gpu_cleanup_mode = gpu_mode;

	m_allocated.Add(b);
	m_allocated_total += len;

	return true;
}

bool FlexibleMemory::ClaimUnmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode)
{
	EXIT_IF(gpu_mode == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& b: m_allocated)
	{
		if (b.map_vaddr == vaddr && b.map_size == size && !b.unmap_pending)
		{
			*gpu_mode = b.gpu_cleanup_mode;
			b.unmap_pending = true;
			return true;
		}
	}

	return false;
}

KernelGpuMappingPromotionStatus FlexibleMemory::PromoteGpuRange(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode gpu_mode,
                                                               uint64_t* mapping_addr, uint64_t* mapping_size)
{
	if (mapping_addr == nullptr || mapping_size == nullptr || !is_representable_range(vaddr, size))
	{
		return KernelGpuMappingPromotionStatus::InvalidArgument;
	}

	Core::LockGuard lock(m_mutex);
	for (auto& block: m_allocated)
	{
		auto cleanup_mode = block.gpu_cleanup_mode;
		const auto result =
		    KernelPromoteGpuMappingRange(block.map_vaddr, block.map_size, vaddr, size, gpu_mode, &cleanup_mode);
		if (result == KernelGpuMappingPromotionStatus::NotContained)
		{
			continue;
		}
		if (result == KernelGpuMappingPromotionStatus::InvalidArgument)
		{
			return result;
		}
		if (block.unmap_pending)
		{
			return KernelGpuMappingPromotionStatus::UnmapPending;
		}
		block.gpu_cleanup_mode = cleanup_mode;
		*mapping_addr  = block.map_vaddr;
		*mapping_size  = block.map_size;
		return result;
	}
	return KernelGpuMappingPromotionStatus::NotContained;
}

bool FlexibleMemory::CompleteUnmap(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);

	uint32_t index = 0;
	for (auto& b: m_allocated)
	{
		if (b.map_vaddr == vaddr && b.map_size == size && b.unmap_pending)
		{
			if (!VirtualMemory::Free(vaddr))
			{
				b.unmap_pending = false;
				return false;
			}

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
				                   *gpu_mode = b.gpu_cleanup_mode;
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
	const bool trace_flex = std::getenv("KYTY_DEBUG_FLEX_ALLOC") != nullptr;

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
		case 0xf2:
		case 0xf3:
			// 0xf2/0xf3: Gen5 direct-map style GPU+CPU RW (heap / large maps).
			// 0xf2 observed after Fiber/thread bring-up on Astro (decimal 242).
			mode     = VirtualMemory::Mode::ReadWrite;
			gpu_mode = Graphics::GpuMemoryMode::ReadWrite;
			break;
		default: EXIT("unknown prot: %d\n", prot);
	}

	auto in_addr = reinterpret_cast<uint64_t>(*addr_in_out);
	const auto start_us = trace_flex ? std::chrono::duration_cast<std::chrono::microseconds>(
	                                      std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
	if (trace_flex)
	{
		printf("[FlexMap] request in_addr=0x%016" PRIx64 " len=%" PRIu64 " prot=0x%x flags=0x%x name=%s\n", in_addr,
		       static_cast<uint64_t>(len), prot, flags, name);
	}
	auto out_addr = VirtualMemory::Alloc(in_addr, len, mode);
	*addr_in_out  = reinterpret_cast<void*>(out_addr);
	if (trace_flex)
	{
		const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::steady_clock::now().time_since_epoch()).count();
		printf("[FlexMap] after_alloc out_addr=0x%016" PRIx64 " elapsed_us=%" PRIu64 "\n", out_addr, now_us - start_us);
	}

	if (!g_flexible_memory->Map(out_addr, len, prot, mode, gpu_mode))
	{
		printf(FG_RED "\t [Fail]\n" FG_DEFAULT);
		VirtualMemory::Free(out_addr);
		return KERNEL_ERROR_ENOMEM;
	}
	if (trace_flex)
	{
		const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::steady_clock::now().time_since_epoch()).count();
		printf("[FlexMap] after_flex_map elapsed_us=%" PRIu64 "\n", now_us - start_us);
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
		if (trace_flex)
		{
			printf("[FlexMap] calling GpuMemorySetAllocatedRange addr=0x%016" PRIx64 " size=%" PRIu64 "\n", out_addr, static_cast<uint64_t>(len));
		}
		Graphics::GpuMemorySetAllocatedRange(out_addr, len);
	}

	if (g_alloc_callback != nullptr)
	{
		if (trace_flex)
		{
			printf("[FlexMap] callback begin\n");
		}
		g_alloc_callback(out_addr, len);
		if (trace_flex)
		{
			const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
			    std::chrono::steady_clock::now().time_since_epoch()).count();
			printf("[FlexMap] callback_done elapsed_us=%" PRIu64 "\n", now_us - start_us);
		}
	}
	if (trace_flex)
	{
		const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::steady_clock::now().time_since_epoch()).count();
		printf("[FlexMap] done total_us=%" PRIu64 "\n", now_us - start_us);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags)
{
	return KernelMapNamedFlexibleMemory(addr_in_out, len, prot, flags, "");
}

int KYTY_SYSV_ABI KernelReserveVirtualRange(void** addr_in_out, uint64_t len, int flags, uint64_t alignment)
{
	PRINT_NAME();

	constexpr uint64_t kGuestPage      = 0x4000;
	constexpr int      kFixed          = 0x10;
	constexpr int      kNoOverwrite    = 0x80;
	constexpr int      kSupportedFlags = kFixed | kNoOverwrite;

	if (addr_in_out == nullptr || len == 0 || (len & (kGuestPage - 1)) != 0 || (flags & ~kSupportedFlags) != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (alignment == 0)
	{
		alignment = kGuestPage;
	}
	if (alignment < kGuestPage || (alignment & (alignment - 1)) != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_IF(g_reserved_memory == nullptr);

	const uint64_t requested_addr = reinterpret_cast<uint64_t>(*addr_in_out);
	const bool     fixed          = (flags & kFixed) != 0;
	if (fixed &&
	    (requested_addr == 0 || (requested_addr & (alignment - 1)) != 0 || requested_addr > std::numeric_limits<uint64_t>::max() - len))
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t reserved_addr = 0;
	if (fixed)
	{
		reserved_addr = VirtualMemory::AllocFixed(requested_addr, len, VirtualMemory::Mode::NoAccess) ? requested_addr : 0;
	} else
	{
		reserved_addr = VirtualMemory::AllocAligned(requested_addr, len, VirtualMemory::Mode::NoAccess, alignment);
		if (reserved_addr == 0 && requested_addr == 0)
		{
			// ReserveVirtualRange can request an address-space hole larger than
			// Kyty's normal low allocation arena. Keep using the portable host
			// abstraction, but give it a valid high guest-VA hint.
			constexpr uint64_t kLargeReservationHint = UINT64_C(1) << 40;
			reserved_addr = VirtualMemory::AllocAligned(kLargeReservationHint, len, VirtualMemory::Mode::NoAccess, alignment);
		}
	}
	if (reserved_addr == 0)
	{
		return fixed ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_ENOMEM;
	}
	if (!g_reserved_memory->Add(reserved_addr, len))
	{
		VirtualMemory::Free(reserved_addr);
		return KERNEL_ERROR_EBUSY;
	}

	// Reserved VA is PROT_NONE until mapped. Titles (and the application heap)
	// may touch pages before an explicit MapFlexible/MapDirect; demand-map the
	// host page on first fault — same contract as RegisterDemandRange for
	// flexible memory (see VirtualMemory::TryDemandMap).
	VirtualMemory::RegisterDemandRange(reserved_addr, len);

	*addr_in_out = reinterpret_cast<void*>(reserved_addr);
	printf("\t in_addr  = 0x%016" PRIx64 "\n", requested_addr);
	printf("\t out_addr = 0x%016" PRIx64 "\n", reserved_addr);
	printf("\t len      = 0x%016" PRIx64 "\n", len);
	printf("\t flags    = 0x%08x\n", flags);
	printf("\t align    = 0x%016" PRIx64 "\n", alignment);
	return OK;
}

enum class PendingUnmapOwner : uint8_t
{
	Physical,
	Flexible,
};

struct PendingUnmap
{
	PendingUnmapOwner       owner    = PendingUnmapOwner::Physical;
	uint64_t                vaddr    = 0;
	uint64_t                size     = 0;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;
};

static bool complete_mapping_unmap(const PendingUnmap& pending)
{
	switch (pending.owner)
	{
		case PendingUnmapOwner::Physical: return g_physical_memory->CompleteUnmap(pending.vaddr, pending.size);
		case PendingUnmapOwner::Flexible: return g_flexible_memory->CompleteUnmap(pending.vaddr, pending.size);
	}
	EXIT("Unknown pending unmap owner\n");
	return false;
}

static bool complete_gpu_mapping_unmap(void* data)
{
	EXIT_IF(data == nullptr);

	// GraphicsRun owns the GPU admission gate and has already drained every
	// guest queue. VideoOut closes host-buffer admission and drains accepted
	// presentation work before the mapping is detached and released.
	return VideoOut::VideoOutRunBufferUnmapTransaction(
	    static_cast<const PendingUnmap*>(data)->vaddr, static_cast<const PendingUnmap*>(data)->size,
	    [](void* action_data)
	    {
		    EXIT_IF(action_data == nullptr);
		    const auto& pending = *static_cast<const PendingUnmap*>(action_data);
		    Graphics::GpuMemoryFreeMappedRangeQuiesced(Graphics::WindowGetGraphicContext(), pending.vaddr, pending.size);
		    return complete_mapping_unmap(pending);
	    },
	    data);
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

	PendingUnmap pending {};
	pending.vaddr = vaddr;
	pending.size  = len;

	bool mapping_claimed = g_physical_memory->ClaimUnmap(vaddr, len, &pending.gpu_mode);
	if (!mapping_claimed)
	{
		mapping_claimed = g_flexible_memory->ClaimUnmap(vaddr, len, &pending.gpu_mode);
		if (mapping_claimed)
		{
			pending.owner = PendingUnmapOwner::Flexible;
		}
	}

	bool result = false;
	if (mapping_claimed)
	{
		result = pending.gpu_mode == Graphics::GpuMemoryMode::NoAccess
		             ? complete_mapping_unmap(pending)
		             : Graphics::GraphicsRunWithQuiescedSubmissions(complete_gpu_mapping_unmap, &pending);
	} else if (g_reserved_memory != nullptr)
	{
		// Reserved NoAccess ranges never enter the GPU lifetime graph.
		result = g_reserved_memory->Unmap(vaddr, len);
	}

	EXIT_NOT_IMPLEMENTED(!result);

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

	if (!Config::IsNextGen())
	{
		Vector<PhysicalMemory::MappedBlock> mappings;
		if (!g_physical_memory->FindMappingsForPhysicalRelease(static_cast<uint64_t>(start), len, &mappings))
		{
			return KERNEL_ERROR_ENOENT;
		}
		for (const auto& mapping: mappings)
		{
			const int unmap_result = KernelMunmap(mapping.map_vaddr, mapping.map_size);
			if (unmap_result != OK)
			{
				return unmap_result;
			}
		}
	}

	bool result = g_physical_memory->Release(static_cast<uint64_t>(start), len);

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

	if (!KernelDecodeMprotectProt(prot, &mode, &gpu_mode))
	{
		switch (prot)
		{
			case 0x01: mode = VirtualMemory::Mode::Read; break;
			case 0x02:
			case 0x03: mode = VirtualMemory::Mode::ReadWrite; break;
			case 0x04: mode = VirtualMemory::Mode::Execute; break;
			case 0x05: mode = VirtualMemory::Mode::ExecuteRead; break;
			case 0x06:
			case 0x07: mode = VirtualMemory::Mode::ExecuteReadWrite; break;
			case 0x32:
			case 0x33:
			case 0xf2:
			case 0xf3:
				// 0xf2/0xf3: Gen5 direct-map style GPU+CPU RW (heap / large maps).
				// 0xf2 observed after Fiber/thread bring-up on Astro (decimal 242).
				mode     = VirtualMemory::Mode::ReadWrite;
				gpu_mode = Graphics::GpuMemoryMode::ReadWrite;
				break;
			default: EXIT("unknown prot: %d\n", prot);
		}
	}

	auto in_addr = reinterpret_cast<uint64_t>(*addr);

	if (fixed)
	{
		EXIT_NOT_IMPLEMENTED(in_addr == 0);
		EXIT_NOT_IMPLEMENTED((in_addr & (alignment - 1)) != 0);
	}

	bool     physical_range_valid  = false;
	bool     consumed_reservation  = false;
	uint64_t out_addr              = 0;
	const bool replace_reservation = fixed && g_reserved_memory != nullptr &&
	                                 VirtualMemory::SupportsSharedFixedOwnedReservationReplacement() &&
	                                 g_reserved_memory->Contains(in_addr, len);
	if (replace_reservation)
	{
		consumed_reservation = g_reserved_memory->ReplaceAndConsume(
		    in_addr, len,
		    [&]
		    {
			    out_addr = g_physical_memory->Map(in_addr, static_cast<uint64_t>(direct_memory_start), len, prot, mode, gpu_mode,
			                                      alignment, true, true, &physical_range_valid);
			    return out_addr != 0;
		    });
	} else
	{
		consumed_reservation = fixed && g_reserved_memory != nullptr && g_reserved_memory->Consume(in_addr, len);
		out_addr = g_physical_memory->Map(in_addr, static_cast<uint64_t>(direct_memory_start), len, prot, mode, gpu_mode, alignment,
		                                  fixed, false, &physical_range_valid);
	}
	if (out_addr == 0 && consumed_reservation)
	{
		const bool restored = VirtualMemory::AllocFixed(in_addr, len, VirtualMemory::Mode::NoAccess) &&
		                      g_reserved_memory->Add(in_addr, len);
		EXIT_IF(!restored);
	}

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

int KYTY_SYSV_ABI KernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags, int64_t direct_memory_start,
                                         size_t alignment)
{
	PRINT_NAME();
	printf("\t type = %d\n", type);
	// Type is guest memory-class metadata; mapping rights come from prot/flags.
	return KernelMapDirectMemory(addr, len, prot, flags, direct_memory_start, alignment);
}

int KYTY_SYSV_ABI KernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name)
{
	PRINT_NAME();
	printf("\t addr = 0x%016" PRIx64 " len = 0x%016" PRIx64 " name = %s\n", reinterpret_cast<uint64_t>(addr), len,
	       name != nullptr ? name : "(null)");
	// Name tags are diagnostic for guests; host maps are not renamed.
	(void)addr;
	(void)len;
	return OK;
}

int KYTY_SYSV_ABI KernelClearVirtualRangeName(const void* addr, uint64_t len)
{
	PRINT_NAME();
	printf("\t addr = 0x%016" PRIx64 " len = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(addr), len);
	// Pair of SetVirtualRangeName: clearing a name is success when maps exist.
	(void)addr;
	(void)len;
	return OK;
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

int KYTY_SYSV_ABI KernelAvailableDirectMemorySize(int64_t arg0, int64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	const uint64_t direct_size = PhysicalMemory::Size();
	const uint64_t used        = g_physical_memory->TotalAllocatedBytes();
	const uint64_t total_available =
	    used >= direct_size ? 0ULL : direct_size - used;

	if (arg1 != 0 || arg2 != 0 || arg3 != 0 || arg4 != 0)
	{
		const int64_t search_start_raw = arg0;
		const int64_t search_end_raw   = arg1;
		uint64_t      alignment        = arg2 == 0 ? 0x1000ULL : arg2;
		auto*         out_address      = reinterpret_cast<uint64_t*>(arg3);
		auto*         out_size         = reinterpret_cast<uint64_t*>(arg4);
		if (out_address == nullptr || out_size == nullptr)
		{
			return KERNEL_ERROR_EINVAL;
		}

		const uint64_t search_start = search_start_raw < 0 ? 0ULL : static_cast<uint64_t>(search_start_raw);
		uint64_t       search_end   = search_end_raw <= 0 ? direct_size : static_cast<uint64_t>(search_end_raw);
		search_end                  = std::min(search_end, direct_size);
		if (search_start >= search_end)
		{
			return KERNEL_ERROR_EINVAL;
		}

		uint64_t span_start  = 0;
		uint64_t span_length = 0;
		if (!g_physical_memory->FindLargestAvailableSpan(search_start, search_end, alignment, &span_start, &span_length))
		{
			return KERNEL_ERROR_ENOENT;
		}

		*out_address = span_start;
		*out_size    = span_length;
		return OK;
	}

	auto* out_size_address = reinterpret_cast<uint64_t*>(arg0);
	if (out_size_address == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*out_size_address = total_available;
	printf("\t *size = 0x%016" PRIx64 "\n", total_available);
	return OK;
}

namespace {

constexpr int kBatchMapOpMapDirect    = 0;
constexpr int kBatchMapOpUnmap        = 1;
constexpr int kBatchMapOpProtect      = 2;
constexpr int kBatchMapOpMapFlexible  = 3;
constexpr int kBatchMapOpTypeProtect  = 4;
constexpr int kBatchMapEntrySize      = 32;

struct BatchMapEntry
{
	uint64_t start;
	uint64_t offset;
	uint64_t length;
	uint8_t  protection;
	uint8_t  type;
	uint8_t  pad[2];
	uint32_t operation;
};

static_assert(sizeof(BatchMapEntry) == kBatchMapEntrySize);

} // namespace

int KYTY_SYSV_ABI KernelBatchMap2(void* entries, int entry_count, int* processed_out, int flags)
{
	PRINT_NAME();

	printf("\t entries = %p entry_count = %d flags = 0x%x\n", entries, entry_count, flags);

	if (entries == nullptr || entry_count <= 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int processed_count = 0;
	int result          = OK;

	for (int index = 0; index < entry_count; index++)
	{
		const auto* entry = reinterpret_cast<const BatchMapEntry*>(static_cast<uint8_t*>(entries) +
		                                                           static_cast<size_t>(index) * kBatchMapEntrySize);
		if (entry->length == 0 || entry->operation < kBatchMapOpMapDirect || entry->operation > kBatchMapOpTypeProtect)
		{
			result = KERNEL_ERROR_EINVAL;
			break;
		}

		switch (entry->operation)
		{
			case kBatchMapOpMapDirect:
			{
				// entry->start is the guest VA, not a pointer-to-pointer in guest memory.
				void* map_addr = reinterpret_cast<void*>(entry->start);
				result = KernelMapDirectMemory(&map_addr, static_cast<size_t>(entry->length), entry->protection, flags,
				                               static_cast<int64_t>(entry->offset), 0);
				break;
			}
			case kBatchMapOpUnmap:
				result = KernelMunmap(entry->start, static_cast<size_t>(entry->length));
				break;
			case kBatchMapOpProtect:
				result = KernelMprotect(reinterpret_cast<const void*>(entry->start), static_cast<size_t>(entry->length),
				                        entry->protection);
				break;
			case kBatchMapOpMapFlexible:
			{
				void* map_addr = reinterpret_cast<void*>(entry->start);
				result = KernelMapNamedFlexibleMemory(&map_addr, static_cast<size_t>(entry->length), entry->protection, flags, "");
				break;
			}
			case kBatchMapOpTypeProtect:
				result = KERNEL_ERROR_EINVAL;
				break;
			default: result = KERNEL_ERROR_EINVAL; break;
		}

		if (result != OK)
		{
			break;
		}

		processed_count++;
	}

	if (processed_out != nullptr)
	{
		*processed_out = processed_count;
	}

	return result;
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

int KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize(uint64_t* size)
{
	PRINT_NAME();
	if (size == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	size_t available = 0;
	const int rc     = KernelAvailableFlexibleMemorySize(&available);
	if (rc != OK)
	{
		return rc;
	}
	*size = available;
	printf("\t *size = 0x%016" PRIx64 "\n", *size);
	return OK;
}

int KYTY_SYSV_ABI KernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, uint64_t info_size)
{
	PRINT_NAME();

	const uint64_t vaddr = reinterpret_cast<uint64_t>(addr);
	printf("\t addr = 0x%016" PRIx64 " flags = 0x%08x info_size = 0x%016" PRIx64 "\n", vaddr, flags, info_size);

	if (info == nullptr || info_size != sizeof(VirtualQueryInfo) || (flags != 0 && flags != 1))
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_IF(g_physical_memory == nullptr);
	EXIT_IF(g_flexible_memory == nullptr);

	uint64_t base = 0;
	size_t   len  = 0;
	int      prot = 0;
	bool     is_direct   = false;
	bool     is_flexible = false;
	bool     is_reserved = false;
	uint64_t physical_offset = 0;
	int      memory_type     = 0;

	if (g_physical_memory->Find(vaddr, &base, &len, &prot, nullptr, nullptr, &physical_offset, &memory_type))
	{
		is_direct = true;
	}
	else if (g_flexible_memory->Find(vaddr, &base, &len, &prot, nullptr, nullptr))
	{
		is_flexible = true;
	}
	else
	{
		uint64_t reserved_size = 0;
		if (g_reserved_memory != nullptr && g_reserved_memory->Find(vaddr, &base, &reserved_size))
		{
			len         = reserved_size;
			prot        = 0;
			is_reserved = true;
		} else
		{
			return KERNEL_ERROR_EACCES;
		}
	}

	std::memset(info, 0, sizeof(VirtualQueryInfo));
	info->start        = static_cast<uintptr_t>(base);
	info->end          = static_cast<uintptr_t>(base + len);
	info->offset       = physical_offset;
	info->protection   = prot;
	info->memory_type  = memory_type;
	info->is_flexible  = is_flexible ? 1u : 0u;
	info->is_direct    = is_direct ? 1u : 0u;
	info->is_committed = is_reserved ? 0u : 1u;
	info->is_stack     = LibKernel::PthreadQueryStack(addr, nullptr, nullptr) ? 1u : 0u;

	printf("\t start = 0x%016" PRIx64 " end = 0x%016" PRIx64 " prot = 0x%x flex=%d direct=%d\n",
	       static_cast<uint64_t>(info->start), static_cast<uint64_t>(info->end), info->protection,
	       static_cast<int>(info->is_flexible), static_cast<int>(info->is_direct));

	return OK;
}

int KYTY_SYSV_ABI KernelIsStack(const void* addr, void** start, void** end)
{
	PRINT_NAME();

	VirtualQueryInfo info {};
	const int query_result = KernelVirtualQuery(addr, 0, &info, sizeof(info));
	if (query_result != OK)
	{
		return query_result;
	}

	void* stack_start = nullptr;
	void* stack_end   = nullptr;
	(void)LibKernel::PthreadQueryStack(addr, &stack_start, &stack_end);
	if (start != nullptr)
	{
		*start = stack_start;
	}
	if (end != nullptr)
	{
		*end = stack_end;
	}
	return OK;
}

bool KernelDecodeMprotectProt(int prot, Core::VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode)
{
	EXIT_IF(mode == nullptr || gpu_mode == nullptr);

	// CPU-only callers use the ordinary POSIX protection bitmask. GPU-visible
	// mappings add AMPR access bits to the same CPU protection field.
	switch (prot)
	{
		case 0x0:
			*mode     = VirtualMemory::Mode::NoAccess;
			*gpu_mode = Graphics::GpuMemoryMode::NoAccess;
			return true;
		case 0x1:
			*mode     = VirtualMemory::Mode::Read;
			*gpu_mode = Graphics::GpuMemoryMode::NoAccess;
			return true;
		case 0x2:
		case 0x3:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = Graphics::GpuMemoryMode::NoAccess;
			return true;
		case 0x4:
			*mode     = VirtualMemory::Mode::Execute;
			*gpu_mode = Graphics::GpuMemoryMode::NoAccess;
			return true;
		case 0x5:
			*mode     = VirtualMemory::Mode::ExecuteRead;
			*gpu_mode = Graphics::GpuMemoryMode::NoAccess;
			return true;
		case 0x6:
		case 0x7:
			*mode     = VirtualMemory::Mode::ExecuteReadWrite;
			*gpu_mode = Graphics::GpuMemoryMode::NoAccess;
			return true;
		case 0x11:
			*mode     = VirtualMemory::Mode::Read;
			*gpu_mode = Graphics::GpuMemoryMode::Read;
			return true;
		case 0x12:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = Graphics::GpuMemoryMode::Read;
			return true;
		case 0x42:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = Graphics::GpuMemoryMode::Read;
			return true;
		case 0x82:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = Graphics::GpuMemoryMode::Write;
			return true;
		case 0xC2:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = Graphics::GpuMemoryMode::ReadWrite;
			return true;
		default: return false;
	}
}

static const char* gpu_mapping_promotion_status_name(KernelGpuMappingPromotionStatus status)
{
	switch (status)
	{
		case KernelGpuMappingPromotionStatus::Promoted: return "Promoted";
		case KernelGpuMappingPromotionStatus::Retained: return "Retained";
		case KernelGpuMappingPromotionStatus::NotContained: return "NotContained";
		case KernelGpuMappingPromotionStatus::InvalidArgument: return "InvalidArgument";
		case KernelGpuMappingPromotionStatus::UnmapPending: return "UnmapPending";
	}
	return "Unknown";
}

int KYTY_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot)
{
	PRINT_NAME();

	auto vaddr = reinterpret_cast<uint64_t>(addr);

	printf("\t addr = 0x%016" PRIx64 "\n", vaddr);
	printf("\t len  = 0x%016" PRIx64 "\n", static_cast<uint64_t>(len));
	printf("\t prot = 0x%x\n", prot);

	// Always log early mprotect calls even when PRINT_NAME is Silent — needed
	// to correlate NoAccess demotions with later FATAL-ACCESS-VIOLATION sites.
	static std::atomic<uint32_t> mprotect_log_count {0};
	if (mprotect_log_count.fetch_add(1) < 32u)
	{
		std::fprintf(stderr, "KernelMprotect addr=0x%016" PRIx64 " len=0x%016" PRIx64 " prot=0x%x\n", vaddr,
		             static_cast<uint64_t>(len), prot);
	}

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	if (!KernelDecodeMprotectProt(prot, &mode, &gpu_mode))
	{
		EXIT("unknown prot: 0x%x (%d)\n", prot, prot);
	}

	if (len == 0)
	{
		return OK;
	}

	VirtualMemory::Mode old_mode {};
	bool                ok = VirtualMemory::Protect(vaddr, len, mode, &old_mode);

	EXIT_NOT_IMPLEMENTED(!ok);

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		uint64_t mapping_addr = 0;
		uint64_t mapping_size = 0;
		auto     promotion = g_physical_memory->PromoteGpuRange(vaddr, len, gpu_mode, &mapping_addr, &mapping_size);
		if (promotion == KernelGpuMappingPromotionStatus::NotContained)
		{
			promotion = g_flexible_memory->PromoteGpuRange(vaddr, len, gpu_mode, &mapping_addr, &mapping_size);
		}
		const auto registration_action = KernelGpuMappingRegistrationActionFor(promotion);
		if (registration_action == KernelGpuMappingRegistrationAction::Reject)
		{
			EXIT("GPU-visible mprotect range registration failed: address=0x%016" PRIx64 ", size=0x%016" PRIx64
			     ", mode=%s, status=%s(%u)\n",
			     vaddr, static_cast<uint64_t>(len), Core::EnumName(gpu_mode).C_Str(),
			     gpu_mapping_promotion_status_name(promotion), static_cast<uint32_t>(promotion));
		}
		if (registration_action == KernelGpuMappingRegistrationAction::RegisterOwnerMapping)
		{
			Graphics::GpuMemorySetAllocatedRange(mapping_addr, mapping_size);
		} else if (registration_action == KernelGpuMappingRegistrationAction::RegisterProtectedRange)
		{
			// Preserve the pre-lifecycle behavior for valid external regions
			// that are not owned by PhysicalMemory or FlexibleMemory.
			Graphics::GpuMemorySetAllocatedRange(vaddr, len);
		}
	}

	printf("\t prot: %s -> %s\n", Core::EnumName(old_mode).C_Str(), Core::EnumName(mode).C_Str());

	return OK;
}

} // namespace Kyty::Libs::LibKernel::Memory

#endif // KYTY_EMU_ENABLED
