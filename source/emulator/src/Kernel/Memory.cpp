#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Errors.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Trace.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::Memory {

namespace VirtualMemory = Core::VirtualMemory;

// sceKernelMap* flags (BSD mmap layout): MAP_FIXED and MAP_NO_OVERWRITE.
constexpr int kMapFixed       = 0x10;
constexpr int kMapNoOverwrite = 0x80;
KERNEL_LIB_NAME();

static bool is_representable_range(uint64_t addr, uint64_t size)
{
	return size != 0 && size <= UINT64_MAX - addr;
}

struct MemoryProtectionBlock
{
	uint64_t            address;
	uint64_t            size;
	int                 prot;
	VirtualMemory::Mode mode;
};

static bool apply_protection_blocks(std::vector<MemoryProtectionBlock>* blocks, uint64_t address, uint64_t size, int prot,
                                    VirtualMemory::Mode mode)
{
	if (blocks == nullptr || !is_representable_range(address, size))
	{
		return false;
	}

	const uint64_t                     end = address + size;
	std::vector<MemoryProtectionBlock> covered;
	for (const auto& block: *blocks)
	{
		const uint64_t block_end = block.address + block.size;
		if (block.address < end && address < block_end)
		{
			covered.push_back({std::max(address, block.address), std::min(end, block_end) - std::max(address, block.address), prot, mode});
		}
	}
	std::sort(covered.begin(), covered.end(),
	          [](const MemoryProtectionBlock& left, const MemoryProtectionBlock& right) { return left.address < right.address; });

	uint64_t cursor = address;
	for (const auto& block: covered)
	{
		if (block.address > cursor)
		{
			return false;
		}
		cursor = std::max(cursor, block.address + block.size);
		if (cursor >= end)
		{
			break;
		}
	}
	if (cursor < end)
	{
		return false;
	}

	std::vector<MemoryProtectionBlock> updated;
	updated.reserve(blocks->size() + covered.size() * 2);
	for (const auto& block: *blocks)
	{
		const uint64_t block_end = block.address + block.size;
		if (block.address >= end || block_end <= address)
		{
			updated.push_back(block);
			continue;
		}

		const uint64_t protected_start = std::max(address, block.address);
		const uint64_t protected_end   = std::min(end, block_end);
		if (block.address < protected_start)
		{
			updated.push_back({block.address, protected_start - block.address, block.prot, block.mode});
		}
		updated.push_back({protected_start, protected_end - protected_start, prot, mode});
		if (protected_end < block_end)
		{
			updated.push_back({protected_end, block_end - protected_end, block.prot, block.mode});
		}
	}
	*blocks = std::move(updated);
	return true;
}

static void remove_protection_blocks(std::vector<MemoryProtectionBlock>* blocks, uint64_t address, uint64_t size)
{
	EXIT_IF(blocks == nullptr || !is_representable_range(address, size));

	const uint64_t end = address + size;
	std::vector<MemoryProtectionBlock> updated;
	updated.reserve(blocks->size() + 1);
	for (const auto& block: *blocks)
	{
		const uint64_t block_end = block.address + block.size;
		if (block.address >= end || block_end <= address)
		{
			updated.push_back(block);
			continue;
		}
		if (block.address < address)
		{
			updated.push_back({block.address, address - block.address, block.prot, block.mode});
		}
		if (end < block_end)
		{
			updated.push_back({end, block_end - end, block.prot, block.mode});
		}
	}
	*blocks = std::move(updated);
}

KernelGpuMappingPromotionStatus KernelPromoteGpuMappingRange(uint64_t mapping_addr, uint64_t mapping_size, uint64_t protected_addr,
                                                             uint64_t protected_size, KernelGpuMappingAccessMode requested_mode,
                                                             KernelGpuMappingAccessMode* cleanup_mode)
{
	if (cleanup_mode == nullptr || !is_representable_range(mapping_addr, mapping_size) ||
	    !is_representable_range(protected_addr, protected_size))
	{
		return KernelGpuMappingPromotionStatus::InvalidArgument;
	}
	if (protected_addr < mapping_addr || protected_size > mapping_size || protected_addr - mapping_addr > mapping_size - protected_size)
	{
		return KernelGpuMappingPromotionStatus::NotContained;
	}
	if (requested_mode != KernelGpuMappingAccessMode::NoAccess && *cleanup_mode == KernelGpuMappingAccessMode::NoAccess)
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
		uint64_t            phys_addr;
		uint64_t            map_vaddr;
		uint64_t            map_size;
		int                 prot;
		VirtualMemory::Mode mode;
		int                 memory_type;
		// Monotonic lifetime marker consumed by ClaimUnmap. It records that
		// cleanup is required, not the mapping's latest protection.
		KernelGpuMappingAccessMode gpu_cleanup_mode;
		bool                    unmap_pending     = false;
		bool                    physical_released = false;
	};

	PhysicalMemory()
	{
		EXIT_IF(!Core::Thread::IsMainThread());
		// The backing is sized for the largest supported generation before the
		// loader identifies the guest. Allocation policy still exposes only the
		// guest generation's capacity.
		m_backing = VirtualMemory::CreateSharedBacking(BackingSize());
		EXIT_IF(m_backing == nullptr);
	}
	virtual ~PhysicalMemory()
	{
		for (const auto& mapping: m_mapped)
		{
			(void)VirtualMemory::FreeRange(mapping.map_vaddr, mapping.map_size);
		}
		VirtualMemory::DestroySharedBacking(m_backing);
	}

	KYTY_CLASS_NO_COPY(PhysicalMemory);

	static uint64_t Size()
	{
		constexpr uint64_t kLegacyDirectMemory  = static_cast<uint64_t>(5376) * 1024 * 1024;
		constexpr uint64_t kNextGenSystemMemory = static_cast<uint64_t>(16) * 1024 * 1024 * 1024;
		return Config::IsNextGen() ? kNextGenSystemMemory : kLegacyDirectMemory;
	}
	static constexpr uint64_t BackingSize() { return static_cast<uint64_t>(16) * 1024 * 1024 * 1024; }

	bool     Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment, uint64_t* phys_addr_out, int memory_type);
	bool     Release(uint64_t start, size_t len);
	bool     FindMappingsForPhysicalRelease(uint64_t start, size_t len, Vector<MappedBlock>* mappings);
	struct MappedView
	{
		uint64_t vaddr    = 0;
		uint64_t size     = 0;
		bool     released = false;
	};
	// Live (not unmapping) views overlapping [vaddr, vaddr + size), in address order.
	std::vector<MappedView> OverlappingViews(uint64_t vaddr, uint64_t size);
	uint64_t Map(uint64_t vaddr, uint64_t phys_addr, size_t len, int prot, VirtualMemory::Mode mode, KernelGpuMappingAccessMode gpu_mode,
	             uint64_t alignment, bool fixed, bool replace_owned_reservation, bool* physical_range_valid);
	bool     ClaimUnmap(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode* gpu_mode);
	bool     CompleteUnmap(uint64_t vaddr, uint64_t size);
	// Unmapping part of one mapping: the claim marks the mapping, the completion
	// unmaps [cut_vaddr, cut_vaddr + cut_size) and keeps the rest as mappings.
	bool     ClaimCut(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode* gpu_mode);
	bool     CompleteCut(uint64_t vaddr, uint64_t size, uint64_t cut_vaddr, uint64_t cut_size);
	void     AbortCut(uint64_t vaddr, uint64_t size);
	bool     DecommitRange(uint64_t vaddr, uint64_t size);
	bool     ApplyProtection(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode);
	KernelGpuMappingPromotionStatus PromoteGpuRange(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode gpu_mode, uint64_t* mapping_addr,
	                                                uint64_t* mapping_size);
	bool     Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode, KernelGpuMappingAccessMode* gpu_mode,
	              uint64_t* phys_addr = nullptr, int* memory_type = nullptr);
	bool     Find(uint64_t phys_addr, bool next, PhysicalMemory::AllocatedBlock* out);
	uint64_t MapAlias(uint64_t vaddr, uint64_t size);
	void     FindUnpopulatedSpans(KernelPhysicalSpan* spans, size_t count);
	uint64_t TotalAllocatedBytes();
	void     FillSnapshot(KernelMemorySnapshot* snapshot);
	bool     FindLargestAvailableSpan(uint64_t search_start, uint64_t search_end, uint64_t alignment, uint64_t* span_start,
	                                  uint64_t* span_length);

	[[nodiscard]] Core::Mutex&               GetMutex() { return m_mutex; }
	[[nodiscard]] const Vector<MappedBlock>& GetMappedBlocks() const { return m_mapped; }
	// The backing outlives every mapping, so the query needs no lock.
	[[nodiscard]] bool QueryPopulation(VirtualMemory::SharedBackingPopulation* out) const
	{
		return VirtualMemory::QuerySharedBackingPopulation(m_backing, out);
	}

private:
	static constexpr uint64_t kNextGenAutoMapBegin = 0x2000000000ull;
	static constexpr uint64_t kNextGenAutoMapEnd   = 0x40000000000ull;
	// Every byte of [start, start + len) belongs to an allocation; adjacent
	// allocations may share the range. With memory_type, they must also share
	// one memory type, which is returned.
	[[nodiscard]] bool IsAllocatedRangeCoveredUnlocked(uint64_t start, size_t len, int* memory_type = nullptr) const;
	// Live views never overlap, so the one holding an address is the nearest
	// live view at or below it. Titles keep thousands of views; per-address
	// queries search an index ordered by guest address instead of the list.
	[[nodiscard]] const MappedBlock* FindLiveViewUnlocked(uint64_t vaddr);
	void                             AddMappedUnlocked(const MappedBlock& block)
	{
		m_mapped.Add(block);
		m_mapped_order_stale = true;
	}
	void RemoveMappedAtUnlocked(uint32_t index)
	{
		m_mapped.RemoveAt(index);
		m_mapped_order_stale = true;
	}

	// Gen5 releases the physical reservation independently from its virtual mapping.
	// KernelMunmap owns the mapping and host/GPU cleanup lifecycle.
	// SharedBacking maps keep re-used physical ranges byte-coherent across aliases.
	Vector<AllocatedBlock>             m_allocated;
	Vector<MappedBlock>                m_mapped;
	std::vector<uint32_t>              m_mapped_order; // m_mapped indices by guest address
	bool                               m_mapped_order_stale = false;
	std::vector<MemoryProtectionBlock> m_protections;
	Core::Mutex                        m_mutex;
	VirtualMemory::SharedBacking*      m_backing = nullptr;
	uint64_t                           m_next_gen_auto_map_cursor = kNextGenAutoMapBegin;
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
		KernelGpuMappingAccessMode gpu_cleanup_mode;
		bool                    unmap_pending = false;
	};

	FlexibleMemory() { EXIT_IF(!Core::Thread::IsMainThread()); }
	virtual ~FlexibleMemory()
	{
		for (const auto& mapping: m_allocated)
		{
			(void)VirtualMemory::Free(mapping.map_vaddr);
		}
	}

	KYTY_CLASS_NO_COPY(FlexibleMemory);

	static uint64_t Size() { return static_cast<uint64_t>(448) * 1024 * 1024; }
	uint64_t        Available();
	void            FillSnapshot(KernelMemorySnapshot* snapshot);

	bool                            Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, KernelGpuMappingAccessMode gpu_mode);
	bool                            ClaimUnmap(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode* gpu_mode);
	bool                            CompleteUnmap(uint64_t vaddr, uint64_t size);
	// Live (not unmapping) mappings overlapping [vaddr, vaddr + size) as {vaddr, size}.
	std::vector<std::pair<uint64_t, uint64_t>> OverlappingMappings(uint64_t vaddr, uint64_t size);
	bool                            ApplyProtection(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode);
	KernelGpuMappingPromotionStatus PromoteGpuRange(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode gpu_mode, uint64_t* mapping_addr,
	                                                uint64_t* mapping_size);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode, KernelGpuMappingAccessMode* gpu_mode);

	[[nodiscard]] Core::Mutex&                  GetMutex() { return m_mutex; }
	[[nodiscard]] const Vector<AllocatedBlock>& GetBlocks() const { return m_allocated; }

private:
	Vector<AllocatedBlock>             m_allocated;
	std::vector<MemoryProtectionBlock> m_protections;
	uint64_t                           m_allocated_total = 0;
	Core::Mutex                        m_mutex;
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
			(void)VirtualMemory::UnregisterDemandRange(block.addr, block.size);
			VirtualMemory::Free(block.addr);
		}
	}

	KYTY_CLASS_NO_COPY(ReservedMemory);

	ReservedMemory() = default;

	bool Add(uint64_t addr, uint64_t size)
	{
		if (size == 0 || addr > std::numeric_limits<uint64_t>::max() - size)
		{
			return false;
		}
		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_blocks)
		{
			if (addr < block.addr + block.size && block.addr < addr + size)
			{
				return false;
			}
		}
		if (!VirtualMemory::RegisterDemandRange(addr, size))
		{
			return false;
		}
		m_blocks.Add(Block {addr, size});
		return true;
	}

	template <typename Decommit>
	bool AddFromMapping(uint64_t addr, uint64_t size, Decommit&& decommit)
	{
		if (size == 0 || addr > std::numeric_limits<uint64_t>::max() - size)
		{
			return false;
		}

		Core::LockGuard lock(m_mutex);
		for (const auto& block: m_blocks)
		{
			if (addr < block.addr + block.size && block.addr < addr + size)
			{
				return false;
			}
		}
		if (!VirtualMemory::RegisterDemandRange(addr, size))
		{
			return false;
		}
		if (!decommit())
		{
			EXIT_IF(!VirtualMemory::UnregisterDemandRange(addr, size));
			return false;
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
				if (!VirtualMemory::UnregisterDemandRange(addr, size))
				{
					return false;
				}
				if (!VirtualMemory::Free(addr))
				{
					EXIT_IF(!VirtualMemory::RegisterDemandRange(addr, size));
					return false;
				}
				m_blocks.RemoveAt(index);
				return true;
			}
		}
		return false;
	}

	// Parts of reserved blocks inside [addr, addr + size), as {addr, size}.
	std::vector<std::pair<uint64_t, uint64_t>> OverlappingParts(uint64_t addr, uint64_t size)
	{
		std::vector<std::pair<uint64_t, uint64_t>> parts;
		Core::LockGuard                             lock(m_mutex);
		for (const auto& block: m_blocks)
		{
			const uint64_t start = std::max(addr, block.addr);
			const uint64_t end   = std::min(addr + size, block.addr + block.size);
			if (start < end)
			{
				parts.emplace_back(start, end - start);
			}
		}
		return parts;
	}

	bool Contains(uint64_t addr, uint64_t size)
	{
		if (size == 0 || addr > std::numeric_limits<uint64_t>::max() - size)
		{
			return false;
		}
		Core::LockGuard lock(m_mutex);
		return std::any_of(m_blocks.begin(), m_blocks.end(), [addr, size](const Block& block)
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
			if (!VirtualMemory::UnregisterDemandRange(addr, size))
			{
				return false;
			}
			if (!mapper())
			{
				EXIT_IF(!VirtualMemory::RegisterDemandRange(addr, size));
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
			if (!VirtualMemory::UnregisterDemandRange(addr, size))
			{
				return false;
			}
			// Release only the consumed part. Releasing the whole block and then
			// re-reserving its remainder leaves a window in which another thread's
			// host mapping can take the address, and the remainder is lost.
			if (VirtualMemory::FreeRange(addr, size))
			{
				SplitConsumedBlock(index, block, addr, size);
				return true;
			}
			// Hosts without partial release (Windows) re-reserve the remainder.
			if (!VirtualMemory::Free(block.addr))
			{
				EXIT_IF(!VirtualMemory::RegisterDemandRange(addr, size));
				return false;
			}
			m_blocks.RemoveAt(index);

			const bool prefix_ok = prefix_size == 0 || VirtualMemory::ReserveFixed(block.addr, prefix_size);
			const bool suffix_ok = suffix_size == 0 || VirtualMemory::ReserveFixed(suffix_addr, suffix_size);
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
				EXIT_IF(!VirtualMemory::ReserveFixed(block.addr, block.size));
				EXIT_IF(!VirtualMemory::RegisterDemandRange(addr, size));
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

void MemorySubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	// CoreSubsystem owns process-wide virtual-memory initialization. Memory has
	// Core as an explicit dependency and must not install platform handlers twice.
	g_physical_memory = new PhysicalMemory;
	g_flexible_memory = new FlexibleMemory;
	g_reserved_memory = new ReservedMemory;
}

void MemorySubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void MemorySubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
{
	delete g_flexible_memory;
	g_flexible_memory = nullptr;
	delete g_physical_memory;
	g_physical_memory = nullptr;
	delete g_reserved_memory;
	g_reserved_memory = nullptr;
	g_alloc_callback  = nullptr;
	g_free_callback   = nullptr;
}

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

KernelMemorySnapshot KernelGetMemorySnapshot()
{
	KernelMemorySnapshot snapshot {};
	if (g_physical_memory != nullptr)
	{
		g_physical_memory->FillSnapshot(&snapshot);
	}
	if (g_flexible_memory != nullptr)
	{
		g_flexible_memory->FillSnapshot(&snapshot);
	}
	return snapshot;
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

	// Walk the sorted allocations and choose the first aligned free span inside
	// the caller's search window. Released ranges must become reusable; otherwise
	// a long-running guest exhausts the physical pool despite having large holes.
	for (const auto& block: m_allocated)
	{
		if (block.size > std::numeric_limits<uint64_t>::max() - block.start_addr)
		{
			return false;
		}

		const uint64_t block_end = block.start_addr + block.size;
		if (block_end <= candidate)
		{
			continue;
		}
		if (block.start_addr >= search_end)
		{
			break;
		}
		if (candidate < block.start_addr && len <= block.start_addr - candidate)
		{
			break;
		}

		candidate = std::max(candidate, block_end);
		if (!get_aligned_pos(candidate, alignment, &candidate) || candidate > search_end - len)
		{
			return false;
		}
	}

	if (candidate > search_end - len)
	{
		return false;
	}
	// A physical range can be released while virtual aliases still reference the
	// shared backing. Reusing that range starts a new allocation lifetime, which
	// must not expose bytes retained by the previous owner. Discard before
	// publishing the allocation; all aliases then observe the same fresh pages.
	if (!VirtualMemory::DiscardSharedBackingRange(m_backing, candidate, len))
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

bool PhysicalMemory::IsAllocatedRangeCoveredUnlocked(uint64_t start, size_t len, int* memory_type) const
{
	if (len == 0 || start > std::numeric_limits<uint64_t>::max() - len)
	{
		return false;
	}

	const uint64_t        end    = start + len;
	uint64_t              cursor = start;
	const AllocatedBlock* first  = nullptr;
	for (const auto& block: m_allocated)
	{
		const uint64_t block_end = block.start_addr + block.size;
		if (block_end <= cursor)
		{
			continue;
		}
		if (block.start_addr > cursor || (memory_type != nullptr && first != nullptr && block.memory_type != first->memory_type))
		{
			return false;
		}
		if (first == nullptr)
		{
			first = &block;
			if (memory_type != nullptr)
			{
				*memory_type = block.memory_type;
			}
		}
		cursor = std::min(end, block_end);
		if (cursor == end)
		{
			return true;
		}
	}
	return false;
}

bool PhysicalMemory::Release(uint64_t start, size_t len)
{
	if (len == 0 || start > std::numeric_limits<uint64_t>::max() - len)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);
	if (!IsAllocatedRangeCoveredUnlocked(start, len))
	{
		return false;
	}
	const uint64_t end = start + len;

	Vector<AllocatedBlock> remaining;
	for (const auto& block: m_allocated)
	{
		const uint64_t block_end = block.start_addr + block.size;
		if (block_end <= start || block.start_addr >= end)
		{
			remaining.Add(block);
			continue;
		}
		if (block.start_addr < start)
		{
			auto left = block;
			left.size = start - block.start_addr;
			remaining.Add(left);
		}
		if (block_end > end)
		{
			auto right       = block;
			right.start_addr = end;
			right.size       = block_end - end;
			remaining.Add(right);
		}
	}
	m_allocated = std::move(remaining);

	// Only the pages whose memory was released lose their backing. A view the
	// release covers in part is split, so its other pages stay a live mapping
	// of memory that is still allocated: a guest that shrinks a buffer by
	// releasing its tail and maps new memory over that tail keeps its head.
	std::vector<uint32_t> partial;
	for (uint32_t index = 0; index < m_mapped.Size(); index++)
	{
		auto& mapped = m_mapped[index];
		if (mapped.phys_addr >= end || mapped.phys_addr + mapped.map_size <= start)
		{
			continue;
		}
		const bool covered = mapped.phys_addr >= start && mapped.phys_addr + mapped.map_size <= end;
		if (covered || mapped.unmap_pending || mapped.physical_released)
		{
			mapped.physical_released = true;
			continue;
		}
		partial.push_back(index);
	}
	std::vector<MappedBlock> pieces;
	for (auto index = partial.rbegin(); index != partial.rend(); ++index)
	{
		const MappedBlock original = m_mapped[*index];
		RemoveMappedAtUnlocked(*index);
		const uint64_t original_end = original.phys_addr + original.map_size;
		const uint64_t cut_begin    = std::max(original.phys_addr, start);
		const uint64_t cut_end      = std::min(original_end, end);
		const auto     piece        = [&](uint64_t phys_begin, uint64_t phys_end, bool released)
		{
			MappedBlock part       = original;
			part.phys_addr         = phys_begin;
			part.map_vaddr         = original.map_vaddr + (phys_begin - original.phys_addr);
			part.map_size          = phys_end - phys_begin;
			part.physical_released = released;
			pieces.push_back(part);
		};
		if (original.phys_addr < cut_begin)
		{
			piece(original.phys_addr, cut_begin, false);
		}
		piece(cut_begin, cut_end, true);
		if (cut_end < original_end)
		{
			piece(cut_end, original_end, false);
		}
	}
	for (const auto& part: pieces)
	{
		AddMappedUnlocked(part);
	}

	const bool still_mapped = std::any_of(m_mapped.begin(), m_mapped.end(), [start, end](const MappedBlock& mapped)
	                                      { return mapped.phys_addr < end && mapped.phys_addr + mapped.map_size > start; });
	if (!still_mapped)
	{
		(void)VirtualMemory::DiscardSharedBackingRange(m_backing, start, len);
	}
	return true;
}

bool PhysicalMemory::FindMappingsForPhysicalRelease(uint64_t start, size_t len, Vector<MappedBlock>* mappings)
{
	if (mappings == nullptr || len == 0 || start > std::numeric_limits<uint64_t>::max() - len)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);
	if (!IsAllocatedRangeCoveredUnlocked(start, len))
	{
		return false;
	}
	const uint64_t end = start + len;

	mappings->Clear();
	for (const auto& mapping: m_mapped)
	{
		if (mapping.phys_addr < end && start < mapping.phys_addr + mapping.map_size)
		{
			mappings->Add(mapping);
		}
	}
	return true;
}

std::vector<PhysicalMemory::MappedView> PhysicalMemory::OverlappingViews(uint64_t vaddr, uint64_t size)
{
	std::vector<MappedView> views;
	if (!is_representable_range(vaddr, size))
	{
		return views;
	}
	Core::LockGuard lock(m_mutex);
	for (const auto& block: m_mapped)
	{
		if (!block.unmap_pending && block.map_vaddr < vaddr + size && vaddr < block.map_vaddr + block.map_size)
		{
			views.push_back({block.map_vaddr, block.map_size, block.physical_released});
		}
	}
	std::sort(views.begin(), views.end(), [](const MappedView& a, const MappedView& b) { return a.vaddr < b.vaddr; });
	return views;
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

void PhysicalMemory::FillSnapshot(KernelMemorySnapshot* snapshot)
{
	EXIT_IF(snapshot == nullptr);

	Core::LockGuard lock(m_mutex);
	struct PhysicalRange
	{
		uint64_t start = 0;
		uint64_t end   = 0;
	};
	std::vector<PhysicalRange> mapped_ranges;
	mapped_ranges.reserve(m_mapped.Size());
	for (const auto& block: m_allocated)
	{
		snapshot->direct_allocated_bytes += block.size;
		snapshot->direct_allocation_count++;
	}
	for (const auto& mapping: m_mapped)
	{
		snapshot->direct_mapped_bytes += mapping.map_size;
		snapshot->direct_mapping_count++;
		mapped_ranges.push_back({mapping.phys_addr, mapping.phys_addr + mapping.map_size});
		if (mapping.physical_released)
		{
			snapshot->direct_released_mapped_bytes += mapping.map_size;
			snapshot->direct_released_mapping_count++;
		}
	}
	std::sort(mapped_ranges.begin(), mapped_ranges.end(),
	          [](const PhysicalRange& left, const PhysicalRange& right) { return left.start < right.start; });
	if (!mapped_ranges.empty())
	{
		uint64_t range_start = mapped_ranges.front().start;
		uint64_t range_end   = mapped_ranges.front().end;
		for (const auto& range: mapped_ranges)
		{
			if (range.start > range_end)
			{
				snapshot->direct_unique_mapped_bytes += range_end - range_start;
				range_start = range.start;
				range_end   = range.end;
			} else
			{
				range_end = std::max(range_end, range.end);
			}
		}
		snapshot->direct_unique_mapped_bytes += range_end - range_start;
	}
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
                             KernelGpuMappingAccessMode gpu_mode, uint64_t alignment, bool fixed, bool replace_owned_reservation,
                             bool* physical_range_valid)
{
	EXIT_IF(physical_range_valid == nullptr);
	*physical_range_valid = false;

	Core::LockGuard lock(m_mutex);

	if (len == 0 || alignment == 0)
	{
		return 0;
	}

	// A mapping may span physically adjacent allocations (titles allocate in
	// chunks and map larger windows), as long as every byte is allocated.
	const uint64_t map_size    = len;
	int            memory_type = 0;
	if (!IsAllocatedRangeCoveredUnlocked(phys_addr, map_size, &memory_type))
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
		const bool automatic_next_gen_map = Config::IsNextGen() && vaddr == 0;
		uint64_t   map_hint               = vaddr;
		if (automatic_next_gen_map)
		{
			if (!get_aligned_pos(m_next_gen_auto_map_cursor, alignment, &map_hint) || map_hint >= kNextGenAutoMapEnd ||
			    map_size > kNextGenAutoMapEnd - map_hint)
			{
				map_hint = kNextGenAutoMapBegin;
			}
		}

		map_vaddr = VirtualMemory::MapSharedAligned(m_backing, map_hint, phys_addr, map_size, mode, alignment);
		if (automatic_next_gen_map && map_vaddr == 0 && map_hint != kNextGenAutoMapBegin)
		{
			map_vaddr = VirtualMemory::MapSharedAligned(m_backing, kNextGenAutoMapBegin, phys_addr, map_size, mode, alignment);
		}
		if (automatic_next_gen_map && map_vaddr != 0)
		{
			const uint64_t map_end = map_vaddr + map_size;
			if (map_vaddr < kNextGenAutoMapBegin || map_vaddr >= kNextGenAutoMapEnd || map_end < map_vaddr ||
			    map_end > kNextGenAutoMapEnd)
			{
				EXIT_IF(!VirtualMemory::Free(map_vaddr));
				map_vaddr = 0;
			} else if (map_end > m_next_gen_auto_map_cursor)
			{
				m_next_gen_auto_map_cursor = map_end;
			}
		}
	}

	if (map_vaddr == 0)
	{
		return 0;
	}

	MappedBlock b {};
	b.phys_addr        = phys_addr;
	b.map_vaddr        = map_vaddr;
	b.map_size         = map_size;
	b.prot             = prot;
	b.mode             = mode;
	b.memory_type      = memory_type;
	b.gpu_cleanup_mode = gpu_mode;
	AddMappedUnlocked(b);
	m_protections.push_back({map_vaddr, map_size, prot, mode});

	return map_vaddr;
}

bool PhysicalMemory::ClaimUnmap(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode* gpu_mode)
{
	EXIT_IF(gpu_mode == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& b: m_mapped)
	{
		if (b.map_vaddr == vaddr && b.map_size == size && !b.unmap_pending)
		{
			*gpu_mode       = b.gpu_cleanup_mode;
			b.unmap_pending = true;
			return true;
		}
	}

	return false;
}

KernelGpuMappingPromotionStatus PhysicalMemory::PromoteGpuRange(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode gpu_mode,
                                                                uint64_t* mapping_addr, uint64_t* mapping_size)
{
	if (mapping_addr == nullptr || mapping_size == nullptr || !is_representable_range(vaddr, size))
	{
		return KernelGpuMappingPromotionStatus::InvalidArgument;
	}

	Core::LockGuard lock(m_mutex);
	for (auto& block: m_mapped)
	{
		auto       cleanup_mode = block.gpu_cleanup_mode;
		const auto result       = KernelPromoteGpuMappingRange(block.map_vaddr, block.map_size, vaddr, size, gpu_mode, &cleanup_mode);
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
		*mapping_addr          = block.map_vaddr;
		*mapping_size          = block.map_size;
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
			// A view may be one piece of a host mapping that a partial physical
			// release split; free exactly its pages.
			if (!VirtualMemory::FreeRange(vaddr, size))
			{
				b.unmap_pending = false;
				return false;
			}
			RemoveMappedAtUnlocked(index);
			// Trim, not only drop, the protection blocks: one recorded for a whole
			// mapping still covers its other pieces when a piece goes alone.
			remove_protection_blocks(&m_protections, vaddr, size);

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

// Removes [start, end) from protection blocks, splitting a block that spans it.
static void erase_protection_span(std::vector<MemoryProtectionBlock>* blocks, uint64_t start, uint64_t end)
{
	std::vector<MemoryProtectionBlock> kept;
	for (const auto& block: *blocks)
	{
		const uint64_t block_end = block.address + block.size;
		if (block_end <= start || block.address >= end)
		{
			kept.push_back(block);
			continue;
		}
		if (block.address < start)
		{
			kept.push_back({block.address, start - block.address, block.prot, block.mode});
		}
		if (block_end > end)
		{
			kept.push_back({end, block_end - end, block.prot, block.mode});
		}
	}
	*blocks = std::move(kept);
}

bool PhysicalMemory::ClaimCut(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode* gpu_mode)
{
	EXIT_IF(gpu_mode == nullptr);
	Core::LockGuard lock(m_mutex);
	for (auto& b: m_mapped)
	{
		if (b.map_vaddr == vaddr && b.map_size == size && !b.unmap_pending)
		{
			*gpu_mode       = b.gpu_cleanup_mode;
			b.unmap_pending = true;
			return true;
		}
	}
	return false;
}

bool PhysicalMemory::CompleteCut(uint64_t vaddr, uint64_t size, uint64_t cut_vaddr, uint64_t cut_size)
{
	Core::LockGuard lock(m_mutex);
	for (uint32_t index = 0; index < m_mapped.Size(); index++)
	{
		auto& b = m_mapped[index];
		if (b.map_vaddr != vaddr || b.map_size != size || !b.unmap_pending)
		{
			continue;
		}
		if (!VirtualMemory::FreeRange(cut_vaddr, cut_size))
		{
			b.unmap_pending = false;
			return false;
		}
		const MappedBlock original = b;
		RemoveMappedAtUnlocked(index);
		const uint64_t cut_end = cut_vaddr + cut_size;
		const uint64_t end     = vaddr + size;
		if (cut_vaddr > vaddr)
		{
			MappedBlock left   = original;
			left.map_size      = cut_vaddr - vaddr;
			left.unmap_pending = false;
			AddMappedUnlocked(left);
		}
		if (cut_end < end)
		{
			MappedBlock right   = original;
			right.map_vaddr     = cut_end;
			right.map_size      = end - cut_end;
			right.phys_addr     = original.phys_addr + (cut_end - vaddr);
			right.unmap_pending = false;
			AddMappedUnlocked(right);
		}
		erase_protection_span(&m_protections, cut_vaddr, cut_end);
		const uint64_t cut_phys    = original.phys_addr + (cut_vaddr - vaddr);
		const bool     allocated   = std::any_of(m_allocated.begin(), m_allocated.end(), [&](const AllocatedBlock& a)
                                               { return cut_phys < a.start_addr + a.size && a.start_addr < cut_phys + cut_size; });
		const bool     still_shown = std::any_of(m_mapped.begin(), m_mapped.end(), [&](const MappedBlock& m)
                                                 { return cut_phys < m.phys_addr + m.map_size && m.phys_addr < cut_phys + cut_size; });
		if (!allocated && !still_shown)
		{
			(void)VirtualMemory::DiscardSharedBackingRange(m_backing, cut_phys, cut_size);
		}
		return true;
	}
	return false;
}

void PhysicalMemory::AbortCut(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);
	for (auto& b: m_mapped)
	{
		if (b.map_vaddr == vaddr && b.map_size == size)
		{
			b.unmap_pending = false;
		}
	}
}

bool PhysicalMemory::DecommitRange(uint64_t vaddr, uint64_t size)
{
	if (!is_representable_range(vaddr, size))
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);
	for (uint32_t index = 0; index < m_mapped.Size(); index++)
	{
		const auto mapping = m_mapped[index];
		if (vaddr < mapping.map_vaddr || size > mapping.map_size || vaddr - mapping.map_vaddr > mapping.map_size - size)
		{
			continue;
		}
		if (mapping.unmap_pending || mapping.gpu_cleanup_mode != KernelGpuMappingAccessMode::NoAccess)
		{
			return false;
		}
		if (!VirtualMemory::DecommitGuestRange(vaddr, size))
		{
			return false;
		}

		RemoveMappedAtUnlocked(index);
		const uint64_t prefix_size = vaddr - mapping.map_vaddr;
		const uint64_t end         = vaddr + size;
		const uint64_t mapping_end = mapping.map_vaddr + mapping.map_size;
		if (prefix_size != 0)
		{
			auto prefix     = mapping;
			prefix.map_size = prefix_size;
			AddMappedUnlocked(prefix);
		}
		if (end < mapping_end)
		{
			auto suffix      = mapping;
			suffix.phys_addr += end - mapping.map_vaddr;
			suffix.map_vaddr = end;
			suffix.map_size  = mapping_end - end;
			AddMappedUnlocked(suffix);
		}
		remove_protection_blocks(&m_protections, vaddr, size);
		return true;
	}
	return false;
}

bool PhysicalMemory::ApplyProtection(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode)
{
	Core::LockGuard lock(m_mutex);
	return apply_protection_blocks(&m_protections, vaddr, size, prot, mode);
}

// Maps a read-write view of the backing pages behind [vaddr, vaddr + size) at
// a host-chosen address. The view shares bytes with the guest mapping and
// ignores its protection, which the dirty-page tracker may have lowered.
uint64_t PhysicalMemory::MapAlias(uint64_t vaddr, uint64_t size)
{
	if (size == 0)
	{
		return 0;
	}
	Core::LockGuard    lock(m_mutex);
	const MappedBlock* live = FindLiveViewUnlocked(vaddr);
	if (live != nullptr && size <= live->map_size && vaddr - live->map_vaddr <= live->map_size - size)
	{
		return VirtualMemory::MapSharedAligned(m_backing, 0, live->phys_addr + (vaddr - live->map_vaddr), size,
		                                       VirtualMemory::Mode::ReadWrite, VirtualMemory::GetPageSize());
	}
	for (const auto& mapping: m_mapped)
	{
		if (vaddr < mapping.map_vaddr || size > mapping.map_size || vaddr - mapping.map_vaddr > mapping.map_size - size)
		{
			continue;
		}
		// CPU protection segments can split one physical mapping. The alias
		// follows its backing bytes and must remain independent of those rights.
		const uint64_t offset = mapping.phys_addr + (vaddr - mapping.map_vaddr);
		return VirtualMemory::MapSharedAligned(m_backing, 0, offset, size, VirtualMemory::Mode::ReadWrite,
		                                       VirtualMemory::GetPageSize());
	}
	return 0;
}

void PhysicalMemory::FindUnpopulatedSpans(KernelPhysicalSpan* spans, size_t count)
{
	std::vector<VirtualMemory::SharedBackingSpan> backing_spans;
	std::vector<size_t>                           owners;
	Core::LockGuard                               lock(m_mutex);
	for (size_t i = 0; i < count; i++)
	{
		auto& span       = spans[i];
		span.unpopulated = false;
		if (span.vaddr == 0 || span.size == 0 || span.vaddr > UINT64_MAX - (span.size - 1u))
		{
			continue;
		}
		const MappedBlock* mapping = FindLiveViewUnlocked(span.vaddr);
		if (mapping == nullptr || span.size > mapping->map_size || span.vaddr - mapping->map_vaddr > mapping->map_size - span.size ||
		    span.vaddr - mapping->map_vaddr > UINT64_MAX - mapping->phys_addr)
		{
			continue;
		}
		backing_spans.push_back({mapping->phys_addr + (span.vaddr - mapping->map_vaddr), span.size});
		owners.push_back(i);
	}
	VirtualMemory::FindUnpopulatedSharedBackingSpans(m_backing, backing_spans.data(), backing_spans.size());
	for (size_t i = 0; i < owners.size(); i++)
	{
		spans[owners[i]].unpopulated = backing_spans[i].unpopulated;
	}
}

const PhysicalMemory::MappedBlock* PhysicalMemory::FindLiveViewUnlocked(uint64_t vaddr)
{
	if (m_mapped_order_stale)
	{
		m_mapped_order.resize(m_mapped.Size());
		for (uint32_t index = 0; index < m_mapped.Size(); index++)
		{
			m_mapped_order[index] = index;
		}
		std::stable_sort(m_mapped_order.begin(), m_mapped_order.end(),
		                 [this](uint32_t a, uint32_t b) { return m_mapped[a].map_vaddr < m_mapped[b].map_vaddr; });
		m_mapped_order_stale = false;
	}
	auto next = std::upper_bound(m_mapped_order.begin(), m_mapped_order.end(), vaddr,
	                             [this](uint64_t address, uint32_t index) { return address < m_mapped[index].map_vaddr; });
	while (next != m_mapped_order.begin())
	{
		const MappedBlock& mapping = m_mapped[*--next];
		if (!mapping.unmap_pending)
		{
			return vaddr - mapping.map_vaddr < mapping.map_size ? &mapping : nullptr;
		}
	}
	return nullptr;
}

bool PhysicalMemory::Find(uint64_t phys_addr, bool next, AllocatedBlock* out)
{
	EXIT_IF(out == nullptr);

	Core::LockGuard lock(m_mutex);

	const auto first_after =
	    std::lower_bound(m_allocated.begin(), m_allocated.end(), phys_addr,
	                     [](const AllocatedBlock& block, uint64_t address) { return block.start_addr + block.size <= address; });
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
	for (auto current = first_after + 1;
	     current != m_allocated.end() && current->memory_type == out->memory_type && current->start_addr == out->start_addr + out->size;
	     ++current)
	{
		out->size += current->size;
	}

	return true;
}

bool PhysicalMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
                          KernelGpuMappingAccessMode* gpu_mode, uint64_t* phys_addr, int* memory_type)
{
	Core::LockGuard lock(m_mutex);

	return std::any_of(m_mapped.begin(), m_mapped.end(),
	                   [this, vaddr, base_addr, len, prot, mode, gpu_mode, phys_addr, memory_type](auto& b)
	                   {
		                   if (vaddr >= b.map_vaddr && vaddr - b.map_vaddr < b.map_size)
		                   {
			                   const auto protection =
			                       std::find_if(m_protections.begin(), m_protections.end(), [vaddr](const MemoryProtectionBlock& block)
			                                    { return vaddr >= block.address && vaddr - block.address < block.size; });
			                   const bool has_protection = protection != m_protections.end();
			                   // A region never extends past its mapping, whatever protection block covers it.
			                   const uint64_t region_begin = has_protection ? std::max(protection->address, b.map_vaddr) : b.map_vaddr;
			                   const uint64_t region_end   = has_protection
			                                                     ? std::min(protection->address + protection->size, b.map_vaddr + b.map_size)
			                                                     : b.map_vaddr + b.map_size;
			                   if (base_addr != nullptr)
			                   {
				                   *base_addr = region_begin;
			                   }
			                   if (len != nullptr)
			                   {
				                   *len = region_end - region_begin;
			                   }
			                   if (prot != nullptr)
			                   {
				                   *prot = has_protection ? protection->prot : b.prot;
			                   }
			                   if (mode != nullptr)
			                   {
				                   *mode = has_protection ? protection->mode : b.mode;
			                   }
			                   if (gpu_mode != nullptr)
			                   {
				                   *gpu_mode = b.gpu_cleanup_mode;
			                   }
			                   if (phys_addr != nullptr)
			                   {
				                   *phys_addr = b.phys_addr + (region_begin - b.map_vaddr);
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

bool FlexibleMemory::Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, KernelGpuMappingAccessMode gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	AllocatedBlock b {};
	b.map_vaddr        = vaddr;
	b.map_size         = len;
	b.prot             = prot;
	b.mode             = mode;
	b.gpu_cleanup_mode = gpu_mode;

	m_allocated.Add(b);
	m_protections.push_back({vaddr, len, prot, mode});
	m_allocated_total += len;

	return true;
}

std::vector<std::pair<uint64_t, uint64_t>> FlexibleMemory::OverlappingMappings(uint64_t vaddr, uint64_t size)
{
	std::vector<std::pair<uint64_t, uint64_t>> mappings;
	Core::LockGuard                             lock(m_mutex);
	for (const auto& b: m_allocated)
	{
		if (!b.unmap_pending && b.map_vaddr < vaddr + size && vaddr < b.map_vaddr + b.map_size)
		{
			mappings.emplace_back(b.map_vaddr, b.map_size);
		}
	}
	return mappings;
}

bool FlexibleMemory::ClaimUnmap(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode* gpu_mode)
{
	EXIT_IF(gpu_mode == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& b: m_allocated)
	{
		if (b.map_vaddr == vaddr && b.map_size == size && !b.unmap_pending)
		{
			*gpu_mode       = b.gpu_cleanup_mode;
			b.unmap_pending = true;
			return true;
		}
	}

	return false;
}

KernelGpuMappingPromotionStatus FlexibleMemory::PromoteGpuRange(uint64_t vaddr, uint64_t size, KernelGpuMappingAccessMode gpu_mode,
                                                                uint64_t* mapping_addr, uint64_t* mapping_size)
{
	if (mapping_addr == nullptr || mapping_size == nullptr || !is_representable_range(vaddr, size))
	{
		return KernelGpuMappingPromotionStatus::InvalidArgument;
	}

	Core::LockGuard lock(m_mutex);
	for (auto& block: m_allocated)
	{
		auto       cleanup_mode = block.gpu_cleanup_mode;
		const auto result       = KernelPromoteGpuMappingRange(block.map_vaddr, block.map_size, vaddr, size, gpu_mode, &cleanup_mode);
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
		*mapping_addr          = block.map_vaddr;
		*mapping_size          = block.map_size;
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
			m_protections.erase(std::remove_if(m_protections.begin(), m_protections.end(), [vaddr, size](const MemoryProtectionBlock& block)
			                                   { return block.address >= vaddr && block.address - vaddr < size; }),
			                    m_protections.end());
			m_allocated_total -= size;
			return true;
		}
		index++;
	}

	return false;
}

bool FlexibleMemory::ApplyProtection(uint64_t vaddr, uint64_t size, int prot, VirtualMemory::Mode mode)
{
	Core::LockGuard lock(m_mutex);
	return apply_protection_blocks(&m_protections, vaddr, size, prot, mode);
}

bool FlexibleMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
                          KernelGpuMappingAccessMode* gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	return std::any_of(m_allocated.begin(), m_allocated.end(),
	                   [this, vaddr, base_addr, len, prot, mode, gpu_mode](auto& b)
	                   {
		                   if (vaddr >= b.map_vaddr && vaddr < b.map_vaddr + b.map_size)
		                   {
			                   const auto protection =
			                       std::find_if(m_protections.begin(), m_protections.end(), [vaddr](const MemoryProtectionBlock& block)
			                                    { return vaddr >= block.address && vaddr - block.address < block.size; });
			                   const bool has_protection = protection != m_protections.end();
			                   // A region never extends past its mapping, whatever protection block covers it.
			                   const uint64_t region_begin = has_protection ? std::max(protection->address, b.map_vaddr) : b.map_vaddr;
			                   const uint64_t region_end   = has_protection
			                                                     ? std::min(protection->address + protection->size, b.map_vaddr + b.map_size)
			                                                     : b.map_vaddr + b.map_size;
			                   if (base_addr != nullptr)
			                   {
				                   *base_addr = region_begin;
			                   }
			                   if (len != nullptr)
			                   {
				                   *len = region_end - region_begin;
			                   }
			                   if (prot != nullptr)
			                   {
				                   *prot = has_protection ? protection->prot : b.prot;
			                   }
			                   if (mode != nullptr)
			                   {
				                   *mode = has_protection ? protection->mode : b.mode;
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

void FlexibleMemory::FillSnapshot(KernelMemorySnapshot* snapshot)
{
	EXIT_IF(snapshot == nullptr);

	Core::LockGuard lock(m_mutex);
	snapshot->flexible_mapped_bytes  = m_allocated_total;
	snapshot->flexible_mapping_count = m_allocated.Size();
}

int32_t KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags, const char* name)
{
	PRINT_NAME();

	EXIT_IF(g_flexible_memory == nullptr);
	const bool trace_flex = std::getenv("KYTY_DEBUG_FLEX_ALLOC") != nullptr;

	constexpr int kFixed          = 0x10;
	constexpr int kNoOverwrite    = 0x80;
	constexpr int kInternal       = 0x8000;
	constexpr int kSupportedFlags = kFixed | kNoOverwrite | kInternal;
	if (addr_in_out == nullptr || len == 0 || (flags & ~kSupportedFlags) != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (len > g_flexible_memory->Available())
	{
		return KERNEL_ERROR_ENOMEM;
	}

	VirtualMemory::Mode          mode     = VirtualMemory::Mode::NoAccess;
	KernelGpuMappingAccessMode gpu_mode = KernelGpuMappingAccessMode::NoAccess;

	if (!KernelDecodeMprotectProt(prot, &mode, &gpu_mode))
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto       in_addr   = reinterpret_cast<uint64_t>(*addr_in_out);
	const bool fixed     = (flags & kFixed) != 0;
	const auto page_size = VirtualMemory::GetPageSize();
	if ((in_addr != 0 && !is_representable_range(in_addr, len)) ||
	    (fixed && (in_addr == 0 || page_size == 0 || (in_addr & (page_size - 1)) != 0)))
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (gpu_mode != KernelGpuMappingAccessMode::NoAccess && !GetGpuMappingLifecyclePort().IsInstalled())
	{
		return KERNEL_ERROR_EBUSY;
	}
	const auto start_us =
	    trace_flex ? std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
	if (trace_flex)
	{
		KYTY_LOG_DEBUG("[FlexMap] request in_addr=0x%016" PRIx64 " len=%" PRIu64 " prot=0x%x flags=0x%x name=%s\n", in_addr,
		       static_cast<uint64_t>(len), prot, flags, name != nullptr ? name : "");
	}
	uint64_t out_addr             = 0;
	bool     consumed_reservation = false;
	if (fixed && g_reserved_memory != nullptr && g_reserved_memory->Contains(in_addr, len))
	{
		consumed_reservation =
		    g_reserved_memory->ReplaceAndConsume(in_addr, len,
		                                         [&]
		                                         {
			                                         const bool mapped =
			                                             VirtualMemory::AllocFixedReplacingOwnedReservation(in_addr, len, mode);
			                                         out_addr = mapped ? in_addr : 0;
			                                         return mapped;
		                                         });
	} else if (fixed)
	{
		out_addr = VirtualMemory::AllocFixed(in_addr, len, mode) ? in_addr : 0;
	} else
	{
		out_addr = VirtualMemory::Alloc(in_addr, len, mode);
	}
	*addr_in_out = reinterpret_cast<void*>(out_addr);
	if (trace_flex)
	{
		const auto now_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		KYTY_LOG_DEBUG("[FlexMap] after_alloc out_addr=0x%016" PRIx64 " elapsed_us=%" PRIu64 "\n", out_addr, now_us - start_us);
	}

	if (out_addr == 0)
	{
		return fixed ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_ENOMEM;
	}

	if (!g_flexible_memory->Map(out_addr, len, prot, mode, gpu_mode))
	{
		KYTY_LOG_DEBUG(FG_RED "\t [Fail]\n" FG_DEFAULT);
		VirtualMemory::Free(out_addr);
		if (consumed_reservation)
		{
			const bool restored = VirtualMemory::ReserveFixed(in_addr, len) && g_reserved_memory->Add(in_addr, len);
			EXIT_IF(!restored);
		}
		return KERNEL_ERROR_ENOMEM;
	}
	if (trace_flex)
	{
		const auto now_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		KYTY_LOG_DEBUG("[FlexMap] after_flex_map elapsed_us=%" PRIu64 "\n", now_us - start_us);
	}

	KYTY_LOG_DEBUG("\t in_addr  = 0x%016" PRIx64 "\n", in_addr);
	KYTY_LOG_DEBUG("\t out_addr = 0x%016" PRIx64 "\n", out_addr);
	KYTY_LOG_DEBUG("\t size     = %" PRIu64 "\n", len);
	KYTY_LOG_DEBUG("\t mode     = %s\n", Core::EnumName(mode).C_Str());
	KYTY_LOG_DEBUG("\t name     = %s\n", name != nullptr ? name : "");
	KYTY_LOG_DEBUG("\t gpu_mode = %s\n", Core::EnumName(gpu_mode).C_Str());

	if (gpu_mode != KernelGpuMappingAccessMode::NoAccess)
	{
		if (trace_flex)
		{
			KYTY_LOG_DEBUG("[FlexMap] registering GPU mapping range addr=0x%016" PRIx64 " size=%" PRIu64 "\n", out_addr,
			       static_cast<uint64_t>(len));
		}
		EXIT_IF(!GetGpuMappingLifecyclePort().RegisterRange(out_addr, len, KernelGpuMappingBacking::Flexible));
	}

	if (g_alloc_callback != nullptr)
	{
		if (trace_flex)
		{
			KYTY_LOG_DEBUG("[FlexMap] callback begin\n");
		}
		g_alloc_callback(out_addr, len);
		if (trace_flex)
		{
			const auto now_us =
			    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			KYTY_LOG_DEBUG("[FlexMap] callback_done elapsed_us=%" PRIu64 "\n", now_us - start_us);
		}
	}
	if (trace_flex)
	{
		const auto now_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		KYTY_LOG_DEBUG("[FlexMap] done total_us=%" PRIu64 "\n", now_us - start_us);
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
	constexpr int      kNoCoalesce     = 0x400000;
	constexpr int      kSupportedFlags = kFixed | kNoOverwrite | kNoCoalesce;

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
	EXIT_IF(g_physical_memory == nullptr);

	const uint64_t requested_addr = reinterpret_cast<uint64_t>(*addr_in_out);
	const bool     fixed          = (flags & kFixed) != 0;
	if (fixed &&
	    (requested_addr == 0 || (requested_addr & (alignment - 1)) != 0 || requested_addr > std::numeric_limits<uint64_t>::max() - len))
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t reserved_addr = 0;
	const bool decommitted = fixed && g_reserved_memory->AddFromMapping(
	                                      requested_addr, len, [&] { return g_physical_memory->DecommitRange(requested_addr, len); });
	if (decommitted)
	{
		reserved_addr = requested_addr;
	} else if (fixed)
	{
		reserved_addr = VirtualMemory::ReserveFixed(requested_addr, len) ? requested_addr : 0;
	} else
	{
		reserved_addr = VirtualMemory::ReserveAligned(requested_addr, len, alignment);
		if (reserved_addr == 0 && requested_addr == 0)
		{
			// ReserveVirtualRange can request an address-space hole larger than
			// Kyty's normal low allocation arena. Keep using the portable host
			// abstraction, but give it a valid high guest-VA hint.
			constexpr uint64_t kLargeReservationHint = UINT64_C(1) << 40;
			reserved_addr                            = VirtualMemory::ReserveAligned(kLargeReservationHint, len, alignment);
		}
	}
	if (reserved_addr == 0)
	{
		return fixed ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_ENOMEM;
	}
	if (!decommitted && !g_reserved_memory->Add(reserved_addr, len))
	{
		VirtualMemory::Free(reserved_addr);
		return KERNEL_ERROR_EBUSY;
	}
	*addr_in_out = reinterpret_cast<void*>(reserved_addr);
	KYTY_LOG_DEBUG("\t in_addr  = 0x%016" PRIx64 "\n", requested_addr);
	KYTY_LOG_DEBUG("\t out_addr = 0x%016" PRIx64 "\n", reserved_addr);
	KYTY_LOG_DEBUG("\t len      = 0x%016" PRIx64 "\n", len);
	KYTY_LOG_DEBUG("\t flags    = 0x%08x\n", flags);
	KYTY_LOG_DEBUG("\t align    = 0x%016" PRIx64 "\n", alignment);
	return OK;
}

enum class PendingUnmapOwner : uint8_t
{
	Physical,
	Flexible,
};

struct PendingUnmap
{
	PendingUnmapOwner          owner    = PendingUnmapOwner::Physical;
	uint64_t                   vaddr    = 0;
	uint64_t                   size     = 0;
	KernelGpuMappingAccessMode gpu_mode = KernelGpuMappingAccessMode::NoAccess;
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

static bool complete_pending_mapping_unmap(void* data)
{
	EXIT_IF(data == nullptr);

	return complete_mapping_unmap(*static_cast<const PendingUnmap*>(data));
}

struct PendingCut
{
	uint64_t vaddr     = 0;
	uint64_t size      = 0;
	uint64_t cut_vaddr = 0;
	uint64_t cut_size  = 0;
};

static bool complete_pending_cut(void* data)
{
	EXIT_IF(data == nullptr);
	const auto* cut = static_cast<const PendingCut*>(data);
	return g_physical_memory->CompleteCut(cut->vaddr, cut->size, cut->cut_vaddr, cut->cut_size);
}

// Unmaps the part of the direct mapping {vaddr, size} inside [start, end),
// keeping the rest mapped, as BSD munmap and fixed maps do.
static int cut_physical_mapping(uint64_t vaddr, uint64_t size, uint64_t start, uint64_t end)
{
	const uint64_t cut_start = std::max(vaddr, start);
	const uint64_t cut_end   = std::min(vaddr + size, end);
	PendingCut     cut {vaddr, size, cut_start, cut_end - cut_start};
	auto           gpu_mode = KernelGpuMappingAccessMode::NoAccess;
	if (cut_start >= cut_end || !g_physical_memory->ClaimCut(vaddr, size, &gpu_mode))
	{
		return KERNEL_ERROR_ENOENT;
	}
	const bool done = gpu_mode == KernelGpuMappingAccessMode::NoAccess
	                      ? complete_pending_cut(&cut)
	                      : GetGpuMappingLifecyclePort().ReleaseRange(cut.cut_vaddr, cut.cut_size, complete_pending_cut, &cut);
	if (!done)
	{
		g_physical_memory->AbortCut(vaddr, size);
		return KERNEL_ERROR_EBUSY;
	}
	return OK;
}

// A munmap range that covers several mappings and reservation blocks (an
// allocator decommits pieces of a mapping by reserving over them, then frees
// the whole span) unmaps every piece, as BSD munmap does. A direct mapping
// that crosses the range edge is cut; a flexible one must lie inside it.
static int unmap_spanning_range(uint64_t vaddr, uint64_t len)
{
	const auto physical = g_physical_memory->OverlappingViews(vaddr, len);
	const auto flexible = g_flexible_memory->OverlappingMappings(vaddr, len);
	const auto reserved = g_reserved_memory != nullptr ? g_reserved_memory->OverlappingParts(vaddr, len)
	                                                   : std::vector<std::pair<uint64_t, uint64_t>> {};
	if (physical.empty() && flexible.empty() && reserved.empty())
	{
		return KERNEL_ERROR_ENOENT;
	}
	const auto inside = [vaddr, len](uint64_t start, uint64_t size) { return start >= vaddr && start + size <= vaddr + len; };
	for (const auto& [start, size]: flexible)
	{
		if (!inside(start, size))
		{
			return KERNEL_ERROR_EINVAL;
		}
	}
	// Cuts first: they alone can be unsupported by the host, before any change.
	for (const auto& view: physical)
	{
		if (!inside(view.vaddr, view.size))
		{
			const int cut = cut_physical_mapping(view.vaddr, view.size, vaddr, vaddr + len);
			if (cut != OK)
			{
				return cut;
			}
		}
	}
	for (const auto& view: physical)
	{
		if (inside(view.vaddr, view.size))
		{
			const int unmapped = KernelMunmap(view.vaddr, view.size);
			if (unmapped != OK)
			{
				return unmapped;
			}
		}
	}
	for (const auto& [start, size]: flexible)
	{
		const int unmapped = KernelMunmap(start, size);
		if (unmapped != OK)
		{
			return unmapped;
		}
	}
	for (const auto& [start, size]: reserved)
	{
		if (!g_reserved_memory->Consume(start, size))
		{
			return KERNEL_ERROR_EINVAL;
		}
	}
	if (g_free_callback != nullptr)
	{
		g_free_callback(vaddr, len);
	}
	return OK;
}

int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t start = 0x%016" PRIx64 "\n", vaddr);
	KYTY_LOG_DEBUG("\t len   = 0x%016" PRIx64 "\n", len);

	EXIT_IF(g_physical_memory == nullptr);
	EXIT_IF(g_flexible_memory == nullptr);

	if (vaddr == 0 || len == 0 || vaddr > UINT64_MAX - len)
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
		result = pending.gpu_mode == KernelGpuMappingAccessMode::NoAccess
		             ? complete_mapping_unmap(pending)
		             : GetGpuMappingLifecyclePort().ReleaseRange(pending.vaddr, pending.size, complete_pending_mapping_unmap, &pending);
	} else if (g_reserved_memory != nullptr)
	{
		// Reserved NoAccess ranges never enter the GPU lifetime graph.
		result = g_reserved_memory->Consume(vaddr, len);
	}
	if (!result)
	{
		return mapping_claimed ? KERNEL_ERROR_ENOENT : unmap_spanning_range(vaddr, len);
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

	KYTY_LOG_DEBUG("\t offset    = 0x%016" PRIx64 "\n", offset);
	KYTY_LOG_DEBUG("\t flags     = 0x%08" PRIx32 "\n", flags);
	KYTY_LOG_DEBUG("\t info_size = 0x%016" PRIx64 "\n", info_size);

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
		KYTY_LOG_DEBUG(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EACCES;
	}

	auto* query_info = static_cast<QueryInfo*>(info);

	query_info->start       = static_cast<int64_t>(block.start_addr);
	query_info->end         = static_cast<int64_t>(block.start_addr + block.size);
	query_info->memory_type = block.memory_type;

	KYTY_LOG_DEBUG("\t start       = %016" PRIx64 "\n", query_info->start);
	KYTY_LOG_DEBUG("\t end         = %016" PRIx64 "\n", query_info->end);
	KYTY_LOG_DEBUG("\t memory_type = %d\n", query_info->memory_type);
	KYTY_LOG_DEBUG(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

int KYTY_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type,
                                             int64_t* phys_addr_out)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	KYTY_LOG_DEBUG("\t search_start = 0x%016" PRIx64 "\n", search_start);
	KYTY_LOG_DEBUG("\t search_end   = 0x%016" PRIx64 "\n", search_end);
	KYTY_LOG_DEBUG("\t len          = 0x%016" PRIx64 "\n", len);
	KYTY_LOG_DEBUG("\t alignment    = 0x%016" PRIx64 "\n", alignment);
	KYTY_LOG_DEBUG("\t memory_type  = %d\n", memory_type);

	if (search_start < 0 || search_end <= search_start || len == 0 || phys_addr_out == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t addr = 0;
	if (!g_physical_memory->Alloc(search_start, search_end, len, alignment, &addr, memory_type))
	{
		KYTY_LOG_DEBUG(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	KYTY_LOG_DEBUG("\tphys_addr    = %016" PRIx64 "\n", addr);
	KYTY_LOG_DEBUG(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

int KYTY_SYSV_ABI KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type, int64_t* phys_addr_out)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	KYTY_LOG_DEBUG("\t len          = 0x%016" PRIx64 "\n", len);
	KYTY_LOG_DEBUG("\t alignment    = 0x%016" PRIx64 "\n", alignment);
	KYTY_LOG_DEBUG("\t memory_type  = %d\n", memory_type);

	if (len == 0 || phys_addr_out == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t addr = 0;
	if (!g_physical_memory->Alloc(0, UINT64_MAX, len, alignment, &addr, memory_type))
	{
		KYTY_LOG_DEBUG(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	KYTY_LOG_DEBUG("\tphys_addr    = %016" PRIx64 "\n", addr);
	KYTY_LOG_DEBUG(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

static int release_direct_memory(int64_t start, size_t len)
{
	EXIT_IF(g_physical_memory == nullptr);

	if (start < 0 || len == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	Vector<PhysicalMemory::MappedBlock> mappings;
	if (!g_physical_memory->FindMappingsForPhysicalRelease(static_cast<uint64_t>(start), len, &mappings))
	{
		return KERNEL_ERROR_ENOENT;
	}

	if (!Config::IsNextGen())
	{
		for (const auto& mapping: mappings)
		{
			const int unmap_result = KernelMunmap(mapping.map_vaddr, mapping.map_size);
			if (unmap_result != OK)
			{
				return unmap_result;
			}
		}
	} else
	{
		// Ending a physical allocation lifetime does not remove its guest VA, but
		// resources backed by the old contents must be detached before the range can
		// be reused. Keep this transaction outside PhysicalMemory's lock because the
		// graphics adapter drains queues and may query Kernel mappings.
		bool needs_gpu_invalidation = false;
		for (const auto& mapping: mappings)
		{
			if (mapping.unmap_pending)
			{
				return KERNEL_ERROR_EBUSY;
			}
			needs_gpu_invalidation = needs_gpu_invalidation || mapping.gpu_cleanup_mode != KernelGpuMappingAccessMode::NoAccess;
		}
		if (needs_gpu_invalidation && !GetGpuMappingLifecyclePort().IsInstalled())
		{
			return KERNEL_ERROR_EBUSY;
		}
		// Only the view of the released pages loses its contents; the rest of
		// each mapping stays live (a partial release keeps it mapped).
		const uint64_t release_end = static_cast<uint64_t>(start) + len;
		for (const auto& mapping: mappings)
		{
			const uint64_t lo = std::max(static_cast<uint64_t>(start), mapping.phys_addr);
			const uint64_t hi = std::min(release_end, mapping.phys_addr + mapping.map_size);
			if (mapping.gpu_cleanup_mode != KernelGpuMappingAccessMode::NoAccess &&
			    !GetGpuMappingLifecyclePort().InvalidateRange(mapping.map_vaddr + (lo - mapping.phys_addr), hi - lo))
			{
				return KERNEL_ERROR_EBUSY;
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

	KYTY_LOG_DEBUG("\t start = 0x%016" PRIx64 "\n", start);
	KYTY_LOG_DEBUG("\t len   = 0x%016" PRIx64 "\n", len);

	return release_direct_memory(start, len);
}

int KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(int64_t start, size_t len)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t start = 0x%016" PRIx64 "\n", start);
	KYTY_LOG_DEBUG("\t len   = 0x%016" PRIx64 "\n", len);

	return release_direct_memory(start, len);
}

// Returns [start, end) of the reservation to the guest reservation set.
static bool restore_reservation(uint64_t start, uint64_t end)
{
	return start >= end ||
	       (g_reserved_memory != nullptr && VirtualMemory::ReserveFixed(start, end - start) && g_reserved_memory->Add(start, end - start));
}

// A fixed map without NO_OVERWRITE replaces what its range holds, as the BSD
// kernel does. A released view (its pages may already back another
// allocation, so no one may use it) is dropped whole and its parts outside the
// range return to the reservation; the part of a live view inside the range is
// cut away; reservation parts inside the range are consumed. The range then
// becomes one reservation the fixed-map transaction consumes. A range already
// inside one reservation needs none of this.
static int clear_fixed_map_target(uint64_t vaddr, uint64_t len)
{
	const auto views = g_physical_memory->OverlappingViews(vaddr, len);
	if (views.empty() && (g_reserved_memory == nullptr || g_reserved_memory->Contains(vaddr, len)))
	{
		return OK;
	}
	// Cuts first: they alone can be unsupported by the host, before any change.
	for (const auto& view: views)
	{
		const bool contained = view.vaddr >= vaddr && view.vaddr + view.size <= vaddr + len;
		if (!view.released && !contained)
		{
			const int cut = cut_physical_mapping(view.vaddr, view.size, vaddr, vaddr + len);
			if (cut != OK)
			{
				return cut;
			}
		}
	}
	for (const auto& view: views)
	{
		const bool contained = view.vaddr >= vaddr && view.vaddr + view.size <= vaddr + len;
		if (!view.released && !contained)
		{
			continue;
		}
		const int unmapped = KernelMunmap(view.vaddr, view.size);
		if (unmapped != OK)
		{
			return unmapped;
		}
		if (!restore_reservation(view.vaddr, std::min(view.vaddr + view.size, vaddr)) ||
		    !restore_reservation(std::max(view.vaddr, vaddr + len), view.vaddr + view.size))
		{
			return KERNEL_ERROR_EBUSY;
		}
	}
	if (g_reserved_memory != nullptr)
	{
		for (const auto& [start, size]: g_reserved_memory->OverlappingParts(vaddr, len))
		{
			if (!g_reserved_memory->Consume(start, size))
			{
				return KERNEL_ERROR_EBUSY;
			}
		}
	}
	return restore_reservation(vaddr, vaddr + len) ? OK : KERNEL_ERROR_EBUSY;
}

int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	if (addr == nullptr || len == 0 || direct_memory_start < 0 || static_cast<uint64_t>(direct_memory_start) > UINT64_MAX - len)
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

	// MAP_FIXED is bit 0x10 and MAP_NO_OVERWRITE bit 0x80; other bits are accepted.
	bool fixed = (flags & kMapFixed) != 0;
	KYTY_LOG_DEBUG("\t flags        = 0x%x (fixed=%d)\n", flags, fixed ? 1 : 0);

	VirtualMemory::Mode          mode     = VirtualMemory::Mode::NoAccess;
	KernelGpuMappingAccessMode gpu_mode = KernelGpuMappingAccessMode::NoAccess;

	if (!KernelDecodeMprotectProt(prot, &mode, &gpu_mode))
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto in_addr = reinterpret_cast<uint64_t>(*addr);

	if (fixed)
	{
		if (in_addr == 0 || (in_addr & (alignment - 1)) != 0 || in_addr > UINT64_MAX - len)
		{
			return KERNEL_ERROR_EINVAL;
		}
	}
	if (gpu_mode != KernelGpuMappingAccessMode::NoAccess && !GetGpuMappingLifecyclePort().IsInstalled())
	{
		return KERNEL_ERROR_EBUSY;
	}

	if (fixed && (flags & kMapNoOverwrite) == 0)
	{
		const int cleared = clear_fixed_map_target(in_addr, len);
		if (cleared != OK)
		{
			return cleared;
		}
	}

	bool       physical_range_valid = false;
	bool       consumed_reservation = false;
	uint64_t   out_addr             = 0;
	const bool replace_reservation  = fixed && g_reserved_memory != nullptr &&
	                                  VirtualMemory::SupportsSharedFixedOwnedReservationReplacement() &&
	                                  g_reserved_memory->Contains(in_addr, len);
	if (replace_reservation)
	{
		consumed_reservation = g_reserved_memory->ReplaceAndConsume(in_addr, len,
		                                                            [&]
		                                                            {
			                                                            out_addr = g_physical_memory->Map(
			                                                                in_addr, static_cast<uint64_t>(direct_memory_start), len, prot,
			                                                                mode, gpu_mode, alignment, true, true, &physical_range_valid);
			                                                            return out_addr != 0;
		                                                            });
	} else
	{
		consumed_reservation = fixed && g_reserved_memory != nullptr && g_reserved_memory->Consume(in_addr, len);
		out_addr = g_physical_memory->Map(in_addr, static_cast<uint64_t>(direct_memory_start), len, prot, mode, gpu_mode, alignment, fixed,
		                                  false, &physical_range_valid);
	}
	if (out_addr == 0 && consumed_reservation)
	{
		const bool restored = VirtualMemory::ReserveFixed(in_addr, len) && g_reserved_memory->Add(in_addr, len);
		EXIT_IF(!restored);
	}

	*addr = reinterpret_cast<void*>(out_addr);

	KYTY_LOG_DEBUG("\t in_addr  = 0x%016" PRIx64 "\n", in_addr);
	KYTY_LOG_DEBUG("\t out_addr = 0x%016" PRIx64 "\n", out_addr);
	KYTY_LOG_DEBUG("\t size     = 0x%016" PRIx64 "\n", len);
	KYTY_LOG_DEBUG("\t mode     = %s\n", Core::EnumName(mode).C_Str());
	KYTY_LOG_DEBUG("\t align    = 0x%016" PRIx64 "\n", alignment);
	KYTY_LOG_DEBUG("\t gpu_mode = %s\n", Core::EnumName(gpu_mode).C_Str());

	if (!physical_range_valid)
	{
		return KERNEL_ERROR_EACCES;
	}

	if (out_addr == 0)
	{
		KYTY_LOG_DEBUG(FG_RED "\t [Fail]\n" FG_DEFAULT);
		return fixed ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_ENOMEM;
	}

	if (gpu_mode != KernelGpuMappingAccessMode::NoAccess)
	{
		EXIT_IF(!GetGpuMappingLifecyclePort().RegisterRange(out_addr, len, KernelGpuMappingBacking::Physical));
	}

	if (g_alloc_callback != nullptr)
	{
		g_alloc_callback(out_addr, len);
	}

	KYTY_LOG_DEBUG(FG_GREEN "\t [Ok]\n" FG_DEFAULT);

	return OK;
}

int KYTY_SYSV_ABI KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags, off_t direct_memory_start, size_t alignment,
                                             const char* name)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t name = %s\n", name);

	return KernelMapDirectMemory(addr, len, prot, flags, direct_memory_start, alignment);
}

int KYTY_SYSV_ABI KernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags, int64_t direct_memory_start,
                                         size_t alignment)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t type = %d\n", type);
	// Type is guest memory-class metadata; mapping rights come from prot/flags.
	return KernelMapDirectMemory(addr, len, prot, flags, direct_memory_start, alignment);
}

int KYTY_SYSV_ABI KernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t addr = 0x%016" PRIx64 " len = 0x%016" PRIx64 " name = %s\n", reinterpret_cast<uint64_t>(addr), len,
	       name != nullptr ? name : "(null)");
	// Name tags are diagnostic for guests; host maps are not renamed.
	(void)addr;
	(void)len;
	return OK;
}

int KYTY_SYSV_ABI KernelClearVirtualRangeName(const void* addr, uint64_t len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t addr = 0x%016" PRIx64 " len = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(addr), len);
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

	if (addr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

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

uint64_t KernelMapPhysicalAlias(uint64_t vaddr, uint64_t size)
{
	if (g_physical_memory == nullptr)
	{
		return 0;
	}
	return g_physical_memory->MapAlias(vaddr, size);
}

bool KernelUnmapPhysicalAlias(uint64_t alias)
{
	return alias != 0 && VirtualMemory::Free(alias);
}

void KernelFindUnpopulatedPhysicalSpans(KernelPhysicalSpan* spans, size_t count)
{
	if (g_physical_memory == nullptr)
	{
		for (size_t i = 0; i < count; i++)
		{
			spans[i].unpopulated = false;
		}
		return;
	}
	g_physical_memory->FindUnpopulatedSpans(spans, count);
}

bool KernelQueryPhysicalPopulation(Core::VirtualMemory::SharedBackingPopulation* out)
{
	return g_physical_memory != nullptr && out != nullptr && g_physical_memory->QueryPopulation(out);
}

bool KernelQueryMappedRange(uint64_t vaddr, uint64_t size, KernelMappedRange* out)
{
	if (out == nullptr || vaddr == 0 || size == 0 || vaddr > UINT64_MAX - (size - 1u) || g_physical_memory == nullptr ||
	    g_flexible_memory == nullptr)
	{
		return false;
	}

	*out = {};
	uint64_t                   base        = 0;
	size_t                     mapped_size = 0;
	int                        protection  = 0;
	KernelGpuMappingAccessMode gpu_mode     = KernelGpuMappingAccessMode::NoAccess;
	if (g_physical_memory->Find(vaddr, &base, &mapped_size, &protection, nullptr, &gpu_mode))
	{
		if (vaddr - base > mapped_size || size > mapped_size - (vaddr - base))
		{
			return false;
		}
		out->kind       = KernelMappedRangeKind::Physical;
		out->base       = base;
		out->size       = mapped_size;
		out->protection = protection;
		out->gpu_mode   = gpu_mode;
		return true;
	}
	if (g_flexible_memory->Find(vaddr, &base, &mapped_size, &protection, nullptr, &gpu_mode))
	{
		if (vaddr - base > mapped_size || size > mapped_size - (vaddr - base))
		{
			return false;
		}
		out->kind       = KernelMappedRangeKind::Flexible;
		out->base       = base;
		out->size       = mapped_size;
		out->protection = protection;
		out->gpu_mode   = gpu_mode;
		return true;
	}
	return false;
}

int KYTY_SYSV_ABI KernelAvailableDirectMemorySize(int64_t arg0, int64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	const uint64_t direct_size     = PhysicalMemory::Size();
	const uint64_t used            = g_physical_memory->TotalAllocatedBytes();
	const uint64_t total_available = used >= direct_size ? 0ULL : direct_size - used;

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

		// [start, end) is searched as given: an empty range, like a range with no
		// free span, has no available memory (ENOMEM). Titles split the address space
		// recursively around each span and stop on ENOMEM.
		const uint64_t search_start = search_start_raw < 0 ? 0ULL : static_cast<uint64_t>(search_start_raw);
		const uint64_t search_end   = std::min<uint64_t>(search_end_raw < 0 ? 0 : static_cast<uint64_t>(search_end_raw), direct_size);
		uint64_t       span_start   = 0;
		uint64_t       span_length  = 0;
		if (search_start >= search_end ||
		    !g_physical_memory->FindLargestAvailableSpan(search_start, search_end, alignment, &span_start, &span_length))
		{
			return KERNEL_ERROR_ENOMEM;
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
	KYTY_LOG_DEBUG("\t *size = 0x%016" PRIx64 "\n", total_available);
	return OK;
}

namespace {

constexpr int kBatchMapOpMapDirect   = 0;
constexpr int kBatchMapOpUnmap       = 1;
constexpr int kBatchMapOpProtect     = 2;
constexpr int kBatchMapOpMapFlexible = 3;
constexpr int kBatchMapOpTypeProtect = 4;
constexpr int kBatchMapEntrySize     = 32;

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

	KYTY_LOG_DEBUG("\t entries = %p entry_count = %d flags = 0x%x\n", entries, entry_count, flags);

	if (entries == nullptr || entry_count <= 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int processed_count = 0;
	int result          = OK;

	for (int index = 0; index < entry_count; index++)
	{
		const auto* entry =
		    reinterpret_cast<const BatchMapEntry*>(static_cast<uint8_t*>(entries) + static_cast<size_t>(index) * kBatchMapEntrySize);
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
				result         = KernelMapDirectMemory(&map_addr, static_cast<size_t>(entry->length), entry->protection, flags,
				                                       static_cast<int64_t>(entry->offset), 0);
				break;
			}
			case kBatchMapOpUnmap: result = KernelMunmap(entry->start, static_cast<size_t>(entry->length)); break;
			case kBatchMapOpProtect:
				result = KernelMprotect(reinterpret_cast<const void*>(entry->start), static_cast<size_t>(entry->length), entry->protection);
				break;
			case kBatchMapOpMapFlexible:
			{
				void* map_addr = reinterpret_cast<void*>(entry->start);
				result         = KernelMapNamedFlexibleMemory(&map_addr, static_cast<size_t>(entry->length), entry->protection, flags, "");
				break;
			}
			case kBatchMapOpTypeProtect: result = KERNEL_ERROR_EINVAL; break;
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

// sceKernelBatchMap places every mapping at its requested address (MAP_FIXED).
int KYTY_SYSV_ABI KernelBatchMap(void* entries, int entry_count, int* processed_out)
{
	return KernelBatchMap2(entries, entry_count, processed_out, kMapFixed);
}

int KYTY_SYSV_ABI KernelAvailableFlexibleMemorySize(size_t* size)
{
	PRINT_NAME();

	EXIT_IF(g_flexible_memory == nullptr);

	if (size == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*size = g_flexible_memory->Available();

	KYTY_LOG_DEBUG("\t *size = 0x%016" PRIx64 "\n", *size);

	return OK;
}

int KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize(uint64_t* size)
{
	PRINT_NAME();
	if (size == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	size_t    available = 0;
	const int rc        = KernelAvailableFlexibleMemorySize(&available);
	if (rc != OK)
	{
		return rc;
	}
	*size = available;
	KYTY_LOG_DEBUG("\t *size = 0x%016" PRIx64 "\n", *size);
	return OK;
}

int KYTY_SYSV_ABI KernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, uint64_t info_size)
{
	PRINT_NAME();

	const uint64_t vaddr = reinterpret_cast<uint64_t>(addr);
	KYTY_LOG_DEBUG("\t addr = 0x%016" PRIx64 " flags = 0x%08x info_size = 0x%016" PRIx64 "\n", vaddr, flags, info_size);

	if (info == nullptr || info_size != sizeof(VirtualQueryInfo) || (flags != 0 && flags != 1))
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_IF(g_physical_memory == nullptr);
	EXIT_IF(g_flexible_memory == nullptr);

	uint64_t base            = 0;
	size_t   len             = 0;
	int      prot            = 0;
	bool     is_direct       = false;
	bool     is_flexible     = false;
	bool     is_reserved     = false;
	uint64_t physical_offset = 0;
	int      memory_type     = 0;

	if (g_physical_memory->Find(vaddr, &base, &len, &prot, nullptr, nullptr, &physical_offset, &memory_type))
	{
		is_direct = true;
	} else if (g_flexible_memory->Find(vaddr, &base, &len, &prot, nullptr, nullptr))
	{
		is_flexible = true;
	} else
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
	info->is_stack     = PthreadQueryStack(addr, nullptr, nullptr) ? 1u : 0u;

	KYTY_LOG_DEBUG("\t start = 0x%016" PRIx64 " end = 0x%016" PRIx64 " prot = 0x%x flex=%d direct=%d\n", static_cast<uint64_t>(info->start),
	       static_cast<uint64_t>(info->end), info->protection, static_cast<int>(info->is_flexible), static_cast<int>(info->is_direct));

	return OK;
}

int KYTY_SYSV_ABI KernelIsStack(const void* addr, void** start, void** end)
{
	PRINT_NAME();

	VirtualQueryInfo info {};
	const int        query_result = KernelVirtualQuery(addr, 0, &info, sizeof(info));
	if (query_result != OK)
	{
		return query_result;
	}

	void* stack_start = nullptr;
	void* stack_end   = nullptr;
	(void)PthreadQueryStack(addr, &stack_start, &stack_end);
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

bool KernelDecodeMprotectProt(int prot, Core::VirtualMemory::Mode* mode, KernelGpuMappingAccessMode* gpu_mode)
{
	EXIT_IF(mode == nullptr || gpu_mode == nullptr);

	// CPU-only callers use the ordinary POSIX protection bitmask. GPU-visible
	// mappings add AMPR access bits to the same CPU protection field.
	switch (prot)
	{
		case 0x0:
			*mode     = VirtualMemory::Mode::NoAccess;
			*gpu_mode = KernelGpuMappingAccessMode::NoAccess;
			return true;
		case 0x1:
			*mode     = VirtualMemory::Mode::Read;
			*gpu_mode = KernelGpuMappingAccessMode::NoAccess;
			return true;
		case 0x2:
		case 0x3:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = KernelGpuMappingAccessMode::NoAccess;
			return true;
		case 0x4:
			*mode     = VirtualMemory::Mode::Execute;
			*gpu_mode = KernelGpuMappingAccessMode::NoAccess;
			return true;
		case 0x5:
			*mode     = VirtualMemory::Mode::ExecuteRead;
			*gpu_mode = KernelGpuMappingAccessMode::NoAccess;
			return true;
		case 0x6:
		case 0x7:
			*mode     = VirtualMemory::Mode::ExecuteReadWrite;
			*gpu_mode = KernelGpuMappingAccessMode::NoAccess;
			return true;
		case 0x11:
			*mode     = VirtualMemory::Mode::Read;
			*gpu_mode = KernelGpuMappingAccessMode::Read;
			return true;
		case 0x12:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = KernelGpuMappingAccessMode::Read;
			return true;
		case 0x42:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = KernelGpuMappingAccessMode::Read;
			return true;
		case 0x82:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = KernelGpuMappingAccessMode::Write;
			return true;
		case 0xC2:
		case 0x32:
		case 0x33:
		case 0xF2:
		case 0xF3:
		case 0x3F2:
		case 0x3F3:
			*mode     = VirtualMemory::Mode::ReadWrite;
			*gpu_mode = KernelGpuMappingAccessMode::ReadWrite;
			return true;
		default: return false;
	}
}

int KYTY_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot)
{
	PRINT_NAME();

	const auto vaddr = reinterpret_cast<uint64_t>(addr);

	KYTY_LOG_DEBUG("\t addr = 0x%016" PRIx64 "\n", vaddr);
	KYTY_LOG_DEBUG("\t len  = 0x%016" PRIx64 "\n", static_cast<uint64_t>(len));
	KYTY_LOG_DEBUG("\t prot = 0x%x\n", prot);

	VirtualMemory::Mode          mode     = VirtualMemory::Mode::NoAccess;
	KernelGpuMappingAccessMode gpu_mode = KernelGpuMappingAccessMode::NoAccess;

	if (!KernelDecodeMprotectProt(prot, &mode, &gpu_mode))
	{
		return KERNEL_ERROR_EINVAL;
	}

	constexpr uint64_t kGuestPage = 0x4000;
	if (vaddr == 0 || len == 0 || vaddr > UINT64_MAX - len)
	{
		return KERNEL_ERROR_EINVAL;
	}
	const uint64_t range_end     = vaddr + len;
	const uint64_t aligned_vaddr = vaddr & ~(kGuestPage - 1);
	if (range_end > UINT64_MAX - (kGuestPage - 1))
	{
		return KERNEL_ERROR_EINVAL;
	}
	const uint64_t aligned_end = (range_end + kGuestPage - 1) & ~(kGuestPage - 1);
	const uint64_t aligned_len = aligned_end - aligned_vaddr;
	if (gpu_mode != KernelGpuMappingAccessMode::NoAccess && !GetGpuMappingLifecyclePort().IsInstalled())
	{
		return KERNEL_ERROR_EBUSY;
	}

	VirtualMemory::Mode old_mode {};
	// The guest-only transition validates ownership and applies host protection
	// under one VM transaction, so a Free()/reuse cannot interleave here.
	const bool          ok = VirtualMemory::ProtectGuest(aligned_vaddr, aligned_len, mode, &old_mode);
	if (!ok)
	{
		return KERNEL_ERROR_ENOENT;
	}

	if (!g_physical_memory->ApplyProtection(aligned_vaddr, aligned_len, prot, mode))
	{
		(void)g_flexible_memory->ApplyProtection(aligned_vaddr, aligned_len, prot, mode);
	}

	if (gpu_mode != KernelGpuMappingAccessMode::NoAccess)
	{
		uint64_t mapping_addr = 0;
		uint64_t mapping_size = 0;
		auto     promotion    = g_physical_memory->PromoteGpuRange(aligned_vaddr, aligned_len, gpu_mode, &mapping_addr, &mapping_size);
		const auto backing    = promotion == KernelGpuMappingPromotionStatus::NotContained ? KernelGpuMappingBacking::Flexible
		                                                                                     : KernelGpuMappingBacking::Physical;
		if (promotion == KernelGpuMappingPromotionStatus::NotContained)
		{
			promotion = g_flexible_memory->PromoteGpuRange(aligned_vaddr, aligned_len, gpu_mode, &mapping_addr, &mapping_size);
		}
		const auto registration_action = KernelGpuMappingRegistrationActionFor(promotion);
		if (registration_action == KernelGpuMappingRegistrationAction::Reject)
		{
			return promotion == KernelGpuMappingPromotionStatus::UnmapPending ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_EINVAL;
		}
		if (registration_action == KernelGpuMappingRegistrationAction::RegisterOwnerMapping)
		{
			EXIT_IF(!GetGpuMappingLifecyclePort().RegisterRange(mapping_addr, mapping_size, backing));
		} else if (registration_action == KernelGpuMappingRegistrationAction::RegisterProtectedRange)
		{
			// Guest-owned mappings outside the physical/flexible records still
			// need lifecycle registration after their guest protection changes.
			EXIT_IF(!GetGpuMappingLifecyclePort().RegisterRange(aligned_vaddr, aligned_len, KernelGpuMappingBacking::Flexible));
		}
	}

	KYTY_LOG_DEBUG("\t prot: %s -> %s\n", Core::EnumName(old_mode).C_Str(), Core::EnumName(mode).C_Str());

	return OK;
}

} // namespace Kyty::Kernel::Memory

#endif // KYTY_EMU_ENABLED
