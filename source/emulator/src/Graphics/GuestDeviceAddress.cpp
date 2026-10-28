#include "Emulator/Graphics/GuestDeviceAddress.h"
#include "Emulator/Graphics/DebugStats.h"
#include "GuestHostImportPlan.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Kernel/Memory.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

// Imports cover resident pages only, widened to whole direct-memory blocks for
// physical ranges (pages outside every import read as zero through the table's
// zero prefix), in chunks bounded to keep each pin modest.
constexpr uint64_t kChunkBytes = 64ull << 20u;
constexpr uint64_t kPageBytes  = kGuestDeviceAddressPageBytes;
// Physical ranges read direct memory through shared windows: aligned
// power-of-two intervals of whole blocks, at most one chunk long.
constexpr uint64_t kPhysicalImportBlockBytes = 2ull << 20u;
// Bound host padding independently from the guest-visible chunk size.
constexpr uint64_t kMaxHostImportPaddingBytes = 2ull << 20u;
// Keep a bounded number of old address tables alive while submissions may reference them.
constexpr std::size_t kMaxRetiredTables = 32;

struct Chunk
{
	uint64_t       guest      = 0;
	uint64_t       size       = 0; // bytes the table maps to this chunk
	uint64_t       span       = 0; // imported bytes
	VkDeviceMemory memory     = nullptr;
	VkBuffer       buffer     = nullptr;
	uint64_t       device = 0;
	uint64_t       buffer_offset = 0; // where `guest` lies in `buffer`
	uint64_t       alias  = 0;       // writable host view that was imported
	bool           physical = false; // imports the direct-memory pages themselves
	bool           windowed = false; // reads through the physical window at `window`
	uint64_t       window   = 0;
	void*          copy   = nullptr; // tracked snapshot that was imported
	std::size_t    copy_alignment = 0;
	bool           mapped     = false; // `copy` is the mapping of `memory`, not a host allocation
	uint64_t       generation = 0;   // dirty-tracker generation of the snapshot
	bool           tracked    = false; // snapshot is refreshed from tracker generations
	bool           registered = false; // this chunk holds a dirty-tracker registration
};

// A device import of direct memory [offset, offset + size) that every
// physical chunk inside it reads through. Its pages stay pinned in Kernel.
struct PhysicalWindow
{
	uint64_t size   = 0;
	Chunk    import;
	uint32_t users  = 0;
};

struct Range
{
	uint64_t             size = 0;
	std::vector<Chunk>   chunks;
	std::vector<uint8_t> imported; // one byte per page
	bool                 physical_backing = false;
	uint32_t             tracked_chunks   = 0;
	uint64_t             imported_pages   = 0; // pages set in `imported`
};

struct Table
{
	VkDeviceMemory memory  = nullptr;
	VkBuffer       buffer  = nullptr;
	uint64_t       device  = 0;
	uint32_t       entries = 0;
};

struct TableEntry
{
	uint64_t guest  = 0;
	uint64_t size   = 0;
	uint64_t device = 0;
	uint64_t span   = 0;
};

struct Reader
{
	SubmissionId submission {};
	bool         active = false;
};

struct Registry
{
	std::mutex                mutex;
	std::map<uint64_t, Range> ranges;
	// Disjoint, by direct-memory offset. RADV validates every imported memory
	// object on each submit, and titles map thousands of small direct-memory
	// ranges, so ranges share the windows over their pages.
	std::map<uint64_t, PhysicalWindow> windows;
	// Titles map thousands of ranges. Preparation revisits flexible ranges and
	// refreshes snapshots every time, and a physical scan visits the physical
	// ranges with pages not imported yet, so it walks these subsets, not every range.
	std::set<uint64_t>        flexible;
	std::set<uint64_t>        tracked;
	std::set<uint64_t>        partial;
	bool                      dirty = true;
	// Physical-backing population at the last complete scan of physical ranges.
	Core::VirtualMemory::SharedBackingPopulation scanned_population;
	bool                                         population_scanned = false;
	Table                     table;
	std::vector<Table>        retired;
	std::array<Reader, GraphicContext::QUEUES_NUM> readers {}; // Latest registered reader for each queue.
	// The device table's entries, one per chunk, sorted by guest base. Kept in
	// step with the chunks so a rebuild after a mapping change copies it
	// instead of walking thousands of ranges.
	std::vector<TableEntry> entries;
};

Registry& GetRegistry()
{
	static Registry registry;
	return registry;
}

void ClearReaders(Registry* registry)
{
	for (auto& reader: registry->readers) { reader = {}; }
}

bool ValidReader(SubmissionId reader)
{
	return reader.sequence != 0 && reader.queue.Value() < GraphicContext::QUEUES_NUM;
}

SubmissionId PendingReaderForSnapshotRefresh(const Registry& registry)
{
	bool changed = false;
	for (const uint64_t base: registry.tracked)
	{
		for (const auto& chunk: registry.ranges.at(base).chunks)
		{
			changed = changed || (chunk.tracked && GpuDirtyPageTracker::Instance().ChangedSince(chunk.guest, chunk.span,
			                                                                                   chunk.generation));
			if (changed) { break; }
		}
		if (changed) { break; }
	}
	if (!changed) { return {}; }
	for (const auto& reader: registry.readers)
	{
		if (reader.active) { return reader.submission; }
	}
	return {};
}

SubmissionId FirstActiveReader(const Registry& registry)
{
	for (const auto& reader: registry.readers)
	{
		if (reader.active) { return reader.submission; }
	}
	return {};
}

void* AllocateHostSnapshot(uint64_t size, uint64_t alignment) noexcept
{
	const auto max_size = static_cast<uint64_t>(std::numeric_limits<std::size_t>::max());
	if (size == 0 || size > max_size || alignment > max_size || !GuestHostImportPlan::IsPowerOfTwo(alignment))
	{
		return nullptr;
	}
	return ::operator new(static_cast<std::size_t>(size), static_cast<std::align_val_t>(static_cast<std::size_t>(alignment)),
	                      std::nothrow);
}

void FreeHostSnapshot(void* snapshot, std::size_t alignment) noexcept
{
	if (snapshot != nullptr)
	{
		::operator delete(snapshot, static_cast<std::align_val_t>(alignment));
	}
}

void DestroyImport(VkDevice device, const Chunk& chunk)
{
	vkDestroyBuffer(device, chunk.buffer, nullptr);
	vkFreeMemory(device, chunk.memory, nullptr);
	Kernel::Memory::KernelUnmapPhysicalAlias(chunk.alias);
	if (!chunk.mapped)
	{
		FreeHostSnapshot(chunk.copy, chunk.copy_alignment);
	}
	if (chunk.registered)
	{
		(void)GpuDirtyPageTracker::Instance().UnregisterRange(chunk.guest, chunk.span);
	}
}

std::vector<TableEntry>::iterator EntryAtOrAfter(Registry* registry, uint64_t guest)
{
	return std::lower_bound(registry->entries.begin(), registry->entries.end(), guest,
	                        [](const TableEntry& entry, uint64_t value) { return entry.guest < value; });
}

// Chunks cover disjoint guest bytes, so the guest base identifies an entry.
void AddTableEntry(Registry* registry, const Chunk& chunk)
{
	const auto at = EntryAtOrAfter(registry, chunk.guest);
	EXIT_IF(at != registry->entries.end() && at->guest == chunk.guest);
	registry->entries.insert(at, TableEntry {chunk.guest, chunk.size, chunk.device, chunk.span});
}

void RemoveTableEntry(Registry* registry, const Chunk& chunk)
{
	const auto at = EntryAtOrAfter(registry, chunk.guest);
	EXIT_IF(at == registry->entries.end() || at->guest != chunk.guest);
	registry->entries.erase(at);
}

void DestroyChunk(VkDevice device, Registry* registry, const Chunk& chunk)
{
	RemoveTableEntry(registry, chunk);
	if (!chunk.windowed)
	{
		DestroyImport(device, chunk);
		return;
	}
	const auto window = registry->windows.find(chunk.window);
	EXIT_IF(window == registry->windows.end() || window->second.users == 0);
	if (--window->second.users != 0)
	{
		return;
	}
	DestroyImport(device, window->second.import);
	Kernel::Memory::KernelUnpinPhysicalWindow(window->first, window->second.size);
	registry->windows.erase(window);
}

// Registered ranges mirror live guest mappings and are disjoint, so the only
// range starting before `vaddr` that can overlap it is its predecessor.
std::map<uint64_t, Range>::iterator FirstOverlapCandidate(std::map<uint64_t, Range>* ranges, uint64_t vaddr)
{
	auto it = ranges->upper_bound(vaddr);
	if (it != ranges->begin())
	{
		auto previous = std::prev(it);
		if (vaddr - previous->first < previous->second.size)
		{
			return previous;
		}
	}
	return it;
}

// Drops the imports of `range` that cover [vaddr, end); imports of the rest of
// the range stay valid. Cleared pages are rediscovered by the next preparation.
void DropChunks(VkDevice device, Registry* registry, uint64_t base, Range* range, uint64_t vaddr, uint64_t end)
{
	auto kept = range->chunks.begin();
	for (auto chunk = range->chunks.begin(); chunk != range->chunks.end(); ++chunk)
	{
		if (!(chunk->guest < end && vaddr < chunk->guest + chunk->span))
		{
			if (kept != chunk)
			{
				*kept = *chunk;
			}
			++kept;
			continue;
		}
		DestroyChunk(device, registry, *chunk);
		range->tracked_chunks -= chunk->tracked ? 1u : 0u;
		const auto first = static_cast<size_t>((chunk->guest - base) / kPageBytes);
		const auto last  = std::min(range->imported.size(), static_cast<size_t>((chunk->guest + chunk->span - base) / kPageBytes));
		if (first < last)
		{
			const auto begin = range->imported.begin() + static_cast<std::ptrdiff_t>(first);
			const auto end   = range->imported.begin() + static_cast<std::ptrdiff_t>(last);
			range->imported_pages -= static_cast<uint64_t>(std::count(begin, end, uint8_t {1}));
			std::fill(begin, end, uint8_t {0});
		}
		registry->dirty              = true;
		registry->population_scanned = false;
		if (range->physical_backing)
		{
			registry->partial.insert(base);
		}
	}
	range->chunks.erase(kept, range->chunks.end());
	if (range->tracked_chunks == 0)
	{
		registry->tracked.erase(base);
	}
}

// Copies guest memory into a snapshot taken under armed write protection.
// The snapshot keeps the generation observed before arming: a page the CPU
// writes during or after the copy advances past it, so ChangedSince reports
// the chunk and the next prepare refreshes it. Memory the guest writes
// continuously (thread stacks) never holds one generation across the copy.
// Returns false when the range is not tracked, since a snapshot could then go
// stale unnoticed.
bool SnapshotGuest(void* copy, uint64_t guest, uint64_t size, uint64_t* generation)
{
	auto&          tracker = GpuDirtyPageTracker::Instance();
	const uint64_t before  = tracker.SnapshotGeneration(guest, size);
	// A host write in flight (a guest thread stack, file I/O) refuses arming;
	// the snapshot is still valid because ChangedSince keeps reporting the range
	// changed. The second arm covers a write that ended before the check.
	const bool armed = tracker.PrepareForRead(guest, size) || tracker.HasHostWrite(guest, size) || tracker.PrepareForRead(guest, size);
	if (!armed || !Core::VirtualMemory::CopyFromGuest(copy, guest, size))
	{
		return false;
	}
	*generation = before;
	return true;
}

void DestroyTable(VkDevice device, const Table& table)
{
	if (table.buffer != nullptr)
	{
		vkDestroyBuffer(device, table.buffer, nullptr);
		vkFreeMemory(device, table.memory, nullptr);
	}
}

void DestroyRetiredTables(VkDevice device, Registry* registry)
{
	for (const auto& table: registry->retired)
	{
		DestroyTable(device, table);
	}
	registry->retired.clear();
}

// Picks a memory type allowed by type_bits that has all `required` flags.
bool FindMemoryType(VkPhysicalDevice physical, uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t* index)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physical, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
	{
		if ((type_bits & (1u << i)) != 0 && (properties.memoryTypes[i].propertyFlags & required) == required)
		{
			*index = i;
			return true;
		}
	}
	return false;
}

bool FindHostImportMemoryType(VkPhysicalDevice physical, uint32_t type_bits, uint32_t* index)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physical, &properties);
	if (properties.memoryTypeCount > VK_MAX_MEMORY_TYPES)
	{
		return false;
	}
	uint32_t flags[VK_MAX_MEMORY_TYPES] {};
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
	{
		flags[i] = properties.memoryTypes[i].propertyFlags;
	}
	return GuestHostImportPlan::SelectMemoryType(
	    type_bits, flags, properties.memoryTypeCount,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, index);
}

// A zero handle type creates a device-owned buffer.
VkBuffer CreateAddressBuffer(const GraphicContext* ctx, uint64_t size, VkExternalMemoryHandleTypeFlags handle_type)
{
	const bool external = handle_type != 0;
	VkExternalMemoryBufferCreateInfo external_info {};
	external_info.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
	external_info.handleTypes = handle_type;

	VkBufferCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.pNext       = external ? &external_info : nullptr;
	info.size        = size;
	// Imports also receive device-side publications of GPU results.
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VulkanApplyResourceSharing(*ctx, &info.sharingMode, &info.queueFamilyIndexCount, &info.pQueueFamilyIndices);
	VkBuffer buffer = nullptr;
	return vkCreateBuffer(ctx->device, &info, nullptr, &buffer) == VK_SUCCESS ? buffer : nullptr;
}

uint64_t BufferDeviceAddress(VkDevice device, VkBuffer buffer)
{
	VkBufferDeviceAddressInfo info {};
	info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = buffer;
	return vkGetBufferDeviceAddress(device, &info);
}

struct PreparedHostImport
{
	VkDevice            device                  = VK_NULL_HANDLE;
	VkBuffer            buffer                  = VK_NULL_HANDLE;
	VkDeviceMemory      memory                  = VK_NULL_HANDLE;
	VkMemoryRequirements requirements          {};
	uint64_t            min_pointer_alignment = 0;
	VkBool32            requires_dedicated    = VK_FALSE;
	VkBool32            prefers_dedicated     = VK_FALSE;

	PreparedHostImport() = default;
	PreparedHostImport(const PreparedHostImport&) = delete;
	PreparedHostImport& operator=(const PreparedHostImport&) = delete;

	~PreparedHostImport()
	{
		Reset();
	}

	void Reset()
	{
		if (buffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, buffer, nullptr);
			buffer = VK_NULL_HANDLE;
		}
		if (memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, memory, nullptr);
			memory = VK_NULL_HANDLE;
		}
	}

	void Release(Chunk* chunk)
	{
		chunk->buffer = buffer;
		chunk->memory = memory;
		chunk->device = BufferDeviceAddress(device, buffer);
		buffer        = VK_NULL_HANDLE;
		memory        = VK_NULL_HANDLE;
	}

	bool Prepare(GraphicContext* ctx, uint64_t size,
	             VkExternalMemoryHandleTypeFlags handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT)
	{
		device = ctx->device;
		if (handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT)
		{
			if (!ctx->guest_host_pointer_import_supported)
			{
				return false;
			}
			VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_properties {};
			host_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
			VkPhysicalDeviceProperties2 physical_properties {};
			physical_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			physical_properties.pNext = &host_properties;
			vkGetPhysicalDeviceProperties2(ctx->physical_device, &physical_properties);
			min_pointer_alignment = host_properties.minImportedHostPointerAlignment;
			if (!GuestHostImportPlan::IsPowerOfTwo(min_pointer_alignment))
			{
				return false;
			}
		}

		buffer = CreateAddressBuffer(ctx, size, handle_type);
		if (buffer == VK_NULL_HANDLE)
		{
			return false;
		}

		VkMemoryDedicatedRequirements dedicated {};
		dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
		VkMemoryRequirements2 requirements2 {};
		requirements2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
		requirements2.pNext = &dedicated;
		VkBufferMemoryRequirementsInfo2 buffer_requirements {};
		buffer_requirements.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
		buffer_requirements.buffer = buffer;
		vkGetBufferMemoryRequirements2(device, &buffer_requirements, &requirements2);
		requirements       = requirements2.memoryRequirements;
		requires_dedicated = dedicated.requiresDedicatedAllocation;
		prefers_dedicated  = dedicated.prefersDedicatedAllocation;
		if (requirements.size == 0 || !GuestHostImportPlan::IsPowerOfTwo(requirements.alignment))
		{
			return false;
		}
		if (requires_dedicated == VK_TRUE)
		{
			return false;
		}
		return true;
	}
};

// Imports host memory into the prepared device-addressable buffer. Requirements
// and host-pointer alignment are queried once, before the import properties.
bool ImportPointer(GraphicContext* ctx, void* pointer, uint64_t span, uint64_t host_capacity, bool physical_alias,
	               PreparedHostImport* prepared, Chunk* out)
{
	if (pointer == nullptr || prepared == nullptr || out == nullptr || prepared->buffer == VK_NULL_HANDLE ||
	    !GuestHostImportPlan::IsPointerAligned(reinterpret_cast<uintptr_t>(pointer), prepared->min_pointer_alignment))
	{
		return false;
	}
	uint64_t allocation_alignment = std::max(prepared->min_pointer_alignment, static_cast<uint64_t>(alignof(std::max_align_t)));
	if (!GuestHostImportPlan::IsPowerOfTwo(allocation_alignment))
	{
		return false;
	}
	if (physical_alias &&
	    !GuestHostImportPlan::IsPhysicalImportShapeValid(reinterpret_cast<uintptr_t>(pointer), span, prepared->requirements.size,
	                                                    prepared->min_pointer_alignment, prepared->requires_dedicated == VK_TRUE))
	{
		return false;
	}
	uint64_t snapshot_capacity = 0;
	if (!physical_alias &&
	    (!GuestHostImportPlan::PlanOwnedSnapshotCapacity(span, prepared->requirements.size, allocation_alignment,
	                                                     kMaxHostImportPaddingBytes, &snapshot_capacity) ||
	     snapshot_capacity != host_capacity))
	{
		return false;
	}
	auto get_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
	    vkGetDeviceProcAddr(ctx->device, "vkGetMemoryHostPointerPropertiesEXT"));
	VkMemoryHostPointerPropertiesEXT host_properties {};
	host_properties.sType     = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
	uint32_t type_index = 0;
	const VkResult property_result =
	    get_properties == nullptr
	        ? VK_ERROR_EXTENSION_NOT_PRESENT
	        : get_properties(ctx->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer, &host_properties);
	if (property_result != VK_SUCCESS)
	{
		return false;
	}
	GuestHostImportPlan::ImportPlan plan;
	const bool planned = physical_alias
	                         ? GuestHostImportPlan::PlanPhysicalImport(
	                               reinterpret_cast<uintptr_t>(pointer), span, prepared->requirements.size,
	                               prepared->min_pointer_alignment, host_properties.memoryTypeBits,
	                               prepared->requirements.memoryTypeBits, prepared->requires_dedicated == VK_TRUE,
	                               prepared->prefers_dedicated == VK_TRUE, &plan)
	                         : GuestHostImportPlan::PlanOwnedSnapshotImport(
	                               reinterpret_cast<uintptr_t>(pointer), span, prepared->requirements.size,
	                               prepared->min_pointer_alignment, allocation_alignment, kMaxHostImportPaddingBytes,
	                               host_properties.memoryTypeBits, prepared->requirements.memoryTypeBits,
	                               prepared->requires_dedicated == VK_TRUE, prepared->prefers_dedicated == VK_TRUE, &plan);
	if (!planned || plan.host_capacity != host_capacity)
	{
		return false;
	}
	if (!FindHostImportMemoryType(ctx->physical_device, plan.memory_type_bits, &type_index))
	{
		return false;
	}
	VkImportMemoryHostPointerInfoEXT import_info {};
	import_info.sType        = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
	import_info.handleType   = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	import_info.pHostPointer = pointer;
	VkMemoryAllocateFlagsInfo flags {};
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.pNext = &import_info;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo allocate {};
	allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext           = &flags;
	allocate.allocationSize  = plan.allocation_size;
	allocate.memoryTypeIndex = type_index;
	const VkResult allocation_result = vkAllocateMemory(ctx->device, &allocate, nullptr, &prepared->memory);
	const VkResult bind_result = allocation_result == VK_SUCCESS ? vkBindBufferMemory(ctx->device, prepared->buffer, prepared->memory, 0)
	                                                             : VK_NOT_READY;
	if (allocation_result != VK_SUCCESS || bind_result != VK_SUCCESS)
	{
		return false;
	}
	prepared->Release(out);
	return true;
}

// Takes ownership of `dma_buf` and imports its pages into a device-addressable buffer.
bool ImportDmaBuf(GraphicContext* ctx, int dma_buf, uint64_t span, Chunk* out)
{
	PreparedHostImport prepared;
	auto get_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdPropertiesKHR"));
	VkMemoryFdPropertiesKHR properties {};
	properties.sType    = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
	uint32_t type_index = 0;
	if (dma_buf < 0 || get_properties == nullptr || !prepared.Prepare(ctx, span, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ||
	    prepared.requirements.size > span ||
	    get_properties(ctx->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dma_buf, &properties) != VK_SUCCESS ||
	    !FindHostImportMemoryType(ctx->physical_device, properties.memoryTypeBits & prepared.requirements.memoryTypeBits, &type_index))
	{
		Core::VirtualMemory::CloseDmaBuf(dma_buf);
		return false;
	}
	VkImportMemoryFdInfoKHR import_info {};
	import_info.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	import_info.fd         = dma_buf;
	VkMemoryAllocateFlagsInfo flags {};
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.pNext = &import_info;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo allocate {};
	allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext           = &flags;
	allocate.allocationSize  = span;
	allocate.memoryTypeIndex = type_index;
	// A successful import owns the descriptor.
	if (vkAllocateMemory(ctx->device, &allocate, nullptr, &prepared.memory) != VK_SUCCESS)
	{
		Core::VirtualMemory::CloseDmaBuf(dma_buf);
		return false;
	}
	if (vkBindBufferMemory(ctx->device, prepared.buffer, prepared.memory, 0) != VK_SUCCESS)
	{
		return false;
	}
	prepared.Release(out);
	out->physical = true;
	return true;
}

enum class PhysicalImport
{
	HostPointer,
	DmaBuf,
	Unavailable,
};

// Physical direct memory is a shared host mapping, and one shared page decides
// how physical ranges are imported for the device's lifetime. A dma-buf of the
// backing pins its pages. An imported host pointer leaves them movable: each
// host migration or reclaim of a page invalidates the import, and the next
// submission walks and remaps the whole window again. Some drivers also import
// only private anonymous host pointers. The host pointer serves hosts that
// cannot export the backing.
PhysicalImport ProbePhysicalImport(GraphicContext* ctx)
{
	PreparedHostImport prepared;
	auto*              backing      = Core::VirtualMemory::CreateSharedBacking(kPageBytes);
	const bool         host_pointer = backing != nullptr && prepared.Prepare(ctx, kPageBytes);
	const uint64_t     view         = !host_pointer ? 0
	                                                : Core::VirtualMemory::MapSharedAligned(
	                                                      backing, 0, 0, kPageBytes, Core::VirtualMemory::Mode::ReadWrite,
	                                                      std::max(prepared.min_pointer_alignment, kPageBytes));
	Chunk          probe;
	PhysicalImport result = PhysicalImport::Unavailable;
	if (backing != nullptr && ctx->guest_dma_buf_import_supported &&
	    ImportDmaBuf(ctx, Core::VirtualMemory::ExportSharedBackingDmaBuf(backing, 0, kPageBytes), kPageBytes, &probe))
	{
		result = PhysicalImport::DmaBuf;
		DestroyImport(ctx->device, probe);
	} else if (view != 0 && ImportPointer(ctx, reinterpret_cast<void*>(view), kPageBytes, kPageBytes, true, &prepared, &probe))
	{
		result = PhysicalImport::HostPointer;
		DestroyImport(ctx->device, probe);
	}
	if (view != 0)
	{
		(void)Core::VirtualMemory::Free(view);
	}
	Core::VirtualMemory::DestroySharedBacking(backing);
	if (result == PhysicalImport::Unavailable)
	{
		std::fprintf(stderr, "guest physical memory cannot be device-addressed: the device rejects shared host pointers and "
		                     "the host cannot export it as a dma-buf (needs read-write access to /dev/udmabuf)\n");
	}
	return result;
}

// Imports direct memory [offset, offset + size) as a window pinned in Kernel,
// through a dma-buf of the backing or, where the host cannot export one, a
// writable alias of it.
bool ImportWindow(GraphicContext* ctx, uint64_t offset, uint64_t size, Chunk* out)
{
	*out       = {};
	out->size  = size;
	out->span  = size;
	static const PhysicalImport physical_import = ProbePhysicalImport(ctx);
	if (physical_import == PhysicalImport::DmaBuf)
	{
		int dma_buf = -1;
		if (!Kernel::Memory::KernelExportPinnedPhysicalWindowDmaBuf(offset, size, &dma_buf) || dma_buf < 0)
		{
			return false;
		}
		if (ImportDmaBuf(ctx, dma_buf, size, out))
		{
			return true;
		}
		Kernel::Memory::KernelUnpinPhysicalWindow(offset, size);
		return false;
	}
	PreparedHostImport prepared;
	if (physical_import != PhysicalImport::HostPointer || !prepared.Prepare(ctx, size))
	{
		return false;
	}
	const uint64_t alias = Kernel::Memory::KernelMapPinnedPhysicalWindow(offset, size, prepared.min_pointer_alignment);
	if (alias == 0)
	{
		return false;
	}
	if (ImportPointer(ctx, reinterpret_cast<void*>(alias), size, size, true, &prepared, out))
	{
		out->alias    = alias;
		out->physical = true;
		return true;
	}
	prepared.Reset();
	Kernel::Memory::KernelUnmapPhysicalAlias(alias);
	Kernel::Memory::KernelUnpinPhysicalWindow(offset, size);
	return false;
}

// The window holding direct-memory offset `offset`, imported if none does. A
// new window is the largest aligned power-of-two run of blocks, at most one
// chunk, that starts at the block of `offset` and ends by `limit` and before
// the next window.
PhysicalWindow* AcquireWindow(GraphicContext* ctx, Registry* registry, uint64_t offset, uint64_t limit, uint64_t* base)
{
	auto next = registry->windows.upper_bound(offset);
	if (next != registry->windows.begin())
	{
		auto holder = std::prev(next);
		if (offset - holder->first < holder->second.size)
		{
			*base = holder->first;
			return &holder->second;
		}
	}
	const uint64_t start = offset / kPhysicalImportBlockBytes * kPhysicalImportBlockBytes;
	if (next != registry->windows.end())
	{
		limit = std::min(limit, next->first);
	}
	uint64_t size = kPhysicalImportBlockBytes;
	while (size < kChunkBytes && start % (size * 2u) == 0 && limit - start >= size * 2u)
	{
		size *= 2u;
	}
	PhysicalWindow window;
	window.size = size;
	if (!ImportWindow(ctx, start, size, &window.import))
	{
		std::fprintf(stderr, "guest physical window import failed: offset=0x%012" PRIx64 " size=0x%" PRIx64 "\n", start, size);
		return nullptr;
	}
	*base = start;
	return &registry->windows.emplace(start, window).first->second;
}

// Maps guest [guest, guest + size), backed by direct memory from `offset`, to
// the windows over those pages, one chunk per window.
bool ImportPhysicalRun(GraphicContext* ctx, Registry* registry, Range* range, uint64_t guest, uint64_t offset, uint64_t size)
{
	const uint64_t end   = offset + size;
	const uint64_t limit = (end + kPhysicalImportBlockBytes - 1u) / kPhysicalImportBlockBytes * kPhysicalImportBlockBytes;
	for (uint64_t cursor = offset; cursor < end;)
	{
		uint64_t        base   = 0;
		PhysicalWindow* window = AcquireWindow(ctx, registry, cursor, limit, &base);
		if (window == nullptr)
		{
			return false;
		}
		const uint64_t piece_end = std::min(end, base + window->size);
		Chunk          chunk;
		chunk.guest         = guest + (cursor - offset);
		chunk.size          = piece_end - cursor;
		chunk.span          = chunk.size;
		chunk.buffer        = window->import.buffer;
		chunk.buffer_offset = cursor - base;
		chunk.device        = window->import.device + chunk.buffer_offset;
		chunk.physical      = true;
		chunk.windowed      = true;
		chunk.window        = base;
		window->users++;
		AddTableEntry(registry, chunk);
		range->chunks.push_back(chunk);
		cursor = piece_end;
	}
	return true;
}

// Backs a snapshot with mapped device-addressable memory, for devices that do
// not import host pointers. CPU-cached memory keeps write-back reads fast.
bool AllocateMappedSnapshot(GraphicContext* ctx, uint64_t span, Chunk* out)
{
	VkBuffer buffer = CreateAddressBuffer(ctx, span, 0);
	if (buffer == VK_NULL_HANDLE)
	{
		return false;
	}
	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(ctx->device, buffer, &requirements);
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &properties);
	uint32_t flags[VK_MAX_MEMORY_TYPES] {};
	for (uint32_t i = 0; i < properties.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
	{
		flags[i] = properties.memoryTypes[i].propertyFlags;
	}
	constexpr uint32_t coherent  = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t           type_index = 0;
	const bool         typed =
	    GuestHostImportPlan::SelectMemoryType(requirements.memoryTypeBits, flags, properties.memoryTypeCount,
	                                          coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &type_index) ||
	    GuestHostImportPlan::SelectMemoryType(requirements.memoryTypeBits, flags, properties.memoryTypeCount, coherent, &type_index);
	VkMemoryAllocateFlagsInfo allocate_flags {};
	allocate_flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	allocate_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo allocate {};
	allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext           = &allocate_flags;
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = type_index;
	VkDeviceMemory memory  = VK_NULL_HANDLE;
	void*          mapping = nullptr;
	if (!typed || vkAllocateMemory(ctx->device, &allocate, nullptr, &memory) != VK_SUCCESS ||
	    vkBindBufferMemory(ctx->device, buffer, memory, 0) != VK_SUCCESS ||
	    vkMapMemory(ctx->device, memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS)
	{
		vkDestroyBuffer(ctx->device, buffer, nullptr);
		vkFreeMemory(ctx->device, memory, nullptr);
		return false;
	}
	out->buffer = buffer;
	out->memory = memory;
	out->device = BufferDeviceAddress(ctx->device, buffer);
	out->copy   = mapping;
	out->mapped = true;
	return true;
}

// ImportChunk for devices that do not import host pointers: the snapshot is
// the mapping itself, tracked or immutable under the same rules.
bool ImportMappedChunk(GraphicContext* ctx, uint64_t guest, uint64_t span, Chunk* out)
{
	if (!AllocateMappedSnapshot(ctx, span, out))
	{
		return false;
	}
	auto&      tracker   = GpuDirtyPageTracker::Instance();
	const bool writable  = Core::VirtualMemory::IsRangeWritable(guest, span);
	out->registered      = writable && tracker.RegisterRange(guest, span);
	out->tracked         = out->registered && SnapshotGuest(out->copy, guest, span, &out->generation);
	const bool immutable = !writable && Core::VirtualMemory::CopyFromGuest(out->copy, guest, span);
	if (out->tracked || immutable)
	{
		return true;
	}
	DestroyImport(ctx->device, *out);
	*out = {};
	return false;
}

// Imports [guest, guest + span) for a chunk mapping [guest, guest + size)
// from a snapshot of flexible memory.
// The guest view itself is never imported: dirty tracking and guest mprotect
// change its protection, and a driver that pins host pages (a userptr) cannot
// revalidate a page that lost write access, which loses the device.
bool ImportChunk(GraphicContext* ctx, uint64_t guest, uint64_t size, uint64_t span, Chunk* out)
{
	if (ctx == nullptr || out == nullptr || guest == 0 || (guest % kPageBytes) != 0 || size == 0 || span == 0 || span > size ||
	    span > kChunkBytes || (span % kPageBytes) != 0 || guest > UINT64_MAX - span)
	{
		return false;
	}
	*out       = {};
	out->guest = guest;
	out->size  = size;
	out->span  = span;
	if (!ctx->guest_host_pointer_import_supported)
	{
		return ImportMappedChunk(ctx, guest, span, out);
	}
	PreparedHostImport prepared;
	if (!prepared.Prepare(ctx, span))
	{
		return false;
	}
	const uint64_t allocation_alignment = std::max(prepared.min_pointer_alignment, static_cast<uint64_t>(alignof(std::max_align_t)));
	uint64_t       copy_capacity       = 0;
	if (!GuestHostImportPlan::PlanOwnedSnapshotCapacity(span, prepared.requirements.size, allocation_alignment,
	                                                  kMaxHostImportPaddingBytes, &copy_capacity) ||
	    allocation_alignment > std::numeric_limits<size_t>::max() || copy_capacity > std::numeric_limits<size_t>::max())
	{
		return false;
	}
	void* copy = AllocateHostSnapshot(copy_capacity, allocation_alignment);
	// Writable memory: a snapshot refreshed when the dirty tracker sees CPU
	// writes; the chunk registers the range so the tracker covers it.
	// Memory the guest keeps non-writable (guest mprotect to read-only) cannot
	// change until a guest mprotect makes it writable, which for a GPU-visible
	// flexible owner invalidates the import under quiesced submissions.
	auto&      tracker   = GpuDirtyPageTracker::Instance();
	const bool writable  = Core::VirtualMemory::IsRangeWritable(guest, span);
	out->registered      = copy != nullptr && writable && tracker.RegisterRange(guest, span);
	const bool tracked   = out->registered && SnapshotGuest(copy, guest, span, &out->generation);
	const bool immutable = copy != nullptr && !writable && Core::VirtualMemory::CopyFromGuest(copy, guest, span);
	const bool imported = (tracked || immutable) && ImportPointer(ctx, copy, span, copy_capacity, false, &prepared, out);
	if (imported)
	{
		out->copy           = copy;
		out->copy_alignment = static_cast<std::size_t>(allocation_alignment);
		out->tracked        = tracked;
		return true;
	}
	prepared.Reset();
	if (out->registered)
	{
		(void)tracker.UnregisterRange(guest, span);
		out->registered = false;
	}
	FreeHostSnapshot(copy, static_cast<std::size_t>(allocation_alignment));
	return false;
}

// Marks every page of an import block that holds a resident page. `resident`
// covers consecutive direct-memory pages from page index `first`; blocks are
// counted in direct memory and clipped to that span.
void ExpandToImportBlocks(uint64_t first, std::vector<uint8_t>* resident)
{
	constexpr uint64_t block_pages = kPhysicalImportBlockBytes / kPageBytes;
	const uint64_t     pages       = resident->size();
	for (uint64_t page = 0; page < pages;)
	{
		const uint64_t block_end = std::min(pages, ((first + page) / block_pages + 1u) * block_pages - first);
		const auto     begin     = resident->begin() + static_cast<std::ptrdiff_t>(page);
		const auto     end       = resident->begin() + static_cast<std::ptrdiff_t>(block_end);
		if (std::any_of(begin, end, [](uint8_t value) { return value != 0; }))
		{
			std::fill(begin, end, uint8_t {1});
		}
		page = block_end;
	}
}

// Marks range pages [first, last) imported.
void MarkImported(Range* range, uint64_t first, uint64_t last)
{
	range->imported_pages += last - first;
	std::fill(range->imported.begin() + static_cast<std::ptrdiff_t>(first), range->imported.begin() + static_cast<std::ptrdiff_t>(last),
	          1);
}

bool ImportResidentSpan(GraphicContext* ctx, Registry* registry, uint64_t base, Range* range, uint64_t first, uint64_t last,
                        std::vector<uint8_t>* resident, bool* changed)
{
	const uint64_t pages        = last - first;
	const uint64_t span_address = base + first * kPageBytes;
	const uint64_t span_size    = pages * kPageBytes;
	resident->resize(static_cast<size_t>(pages));
	if (!Core::VirtualMemory::QueryResidentPages(span_address, span_size, resident->data()))
	{
		return false;
	}
	// A snapshot chunk is either tracked or immutable as a whole, so a run
	// ends where CPU write access changes.
	const auto writable = [&](uint64_t page)
	{ return Core::VirtualMemory::IsRangeWritable(base + (first + page) * kPageBytes, kPageBytes); };
	for (uint64_t page = 0; page < pages;)
	{
		if ((*resident)[page] == 0)
		{
			page++;
			continue;
		}
		const bool run_writable = writable(page);
		uint64_t   end          = page + 1;
		while (end < pages && (*resident)[end] != 0 && (end - page) * kPageBytes < kChunkBytes && writable(end) == run_writable)
		{
			end++;
		}
		const uint64_t guest = base + (first + page) * kPageBytes;
		const uint64_t size  = (end - page) * kPageBytes;
		Chunk          chunk;
		// Do not pin a page outside the run: the import itself would make that
		// page resident and every subsequent prepare would import another one.
		// Translated loads split at page boundaries instead.
		if (!ImportChunk(ctx, guest, size, size, &chunk))
		{
			std::fprintf(stderr, "guest import failed: base=0x%012" PRIx64 " size=0x%" PRIx64 " host_writable=%d\n", guest, size,
			             Core::VirtualMemory::IsRangeWritable(guest, size) ? 1 : 0);
			return false;
		}
		AddTableEntry(registry, chunk);
		range->chunks.push_back(chunk);
		range->tracked_chunks += chunk.tracked ? 1u : 0u;
		MarkImported(range, first + page, first + end);
		*changed = true;
		page     = end;
	}
	return true;
}

// Physical runs read through windows whatever the guest view's protection.
bool ImportPhysicalSpan(GraphicContext* ctx, Registry* registry, uint64_t base, Range* range, uint64_t first, uint64_t last,
                        std::vector<uint8_t>* resident, bool* changed)
{
	const uint64_t pages        = last - first;
	const uint64_t span_address = base + first * kPageBytes;
	const uint64_t span_size    = pages * kPageBytes;
	uint64_t       offset       = 0;
	const auto     resolution   = Kernel::Memory::KernelResolvePhysical(span_address, span_size, &offset);
	if (resolution == Kernel::Memory::KernelPhysicalResolution::Withdrawing)
	{
		// The guest unmaps or releases this range, so no later GPU work reads
		// it; its quiesced release drops the range without importing it.
		return true;
	}
	if (resolution != Kernel::Memory::KernelPhysicalResolution::Resolved)
	{
		std::fprintf(stderr, "guest import failed: base=0x%012" PRIx64 " size=0x%" PRIx64 " has no direct memory\n", span_address,
		             span_size);
		return false;
	}
	// The backing's extents, not the view's residency: a loading title keeps
	// large spans mostly unwritten, and a per-page walk of them dominated
	// every preparation while pages were being populated.
	resident->resize(static_cast<size_t>(pages));
	if (!Kernel::Memory::KernelQueryPopulatedPhysicalPages(offset, span_size, resident->data()))
	{
		return false;
	}
	if (std::none_of(resident->begin(), resident->end(), [](uint8_t value) { return value != 0; }))
	{
		return true;
	}
	// Guest writes reach physical pages one at a time, and importing each newly
	// resident run gave a chunk per page or two. Whole blocks bound the chunks
	// per range; the pages a block import populates are imported with it, and a
	// block without a resident page is not pinned.
	ExpandToImportBlocks(offset / kPageBytes, resident);
	for (uint64_t page = 0; page < pages;)
	{
		if ((*resident)[page] == 0)
		{
			page++;
			continue;
		}
		const uint64_t end = static_cast<uint64_t>(std::find(resident->begin() + static_cast<std::ptrdiff_t>(page), resident->end(),
		                                                     uint8_t {0}) -
		                                           resident->begin());
		if (!ImportPhysicalRun(ctx, registry, range, span_address + page * kPageBytes, offset + page * kPageBytes,
		                       (end - page) * kPageBytes))
		{
			return false;
		}
		MarkImported(range, first + page, first + end);
		*changed = true;
		page     = end;
	}
	return true;
}

// Calls visit(first, last) for each maximal run of unimported pages; a visit
// may import pages inside its own run only.
template <typename Visit>
bool ForEachUnimportedSpan(Range* range, const Visit& visit)
{
	range->imported.resize(static_cast<size_t>(range->size / kPageBytes), 0);
	const auto begin = range->imported.begin();
	const auto limit = range->imported.end();
	for (auto cursor = begin; cursor != limit;)
	{
		const auto first = std::find(cursor, limit, uint8_t {0});
		if (first == limit) { break; }
		const auto last = std::find(first, limit, uint8_t {1});
		if (!visit(static_cast<uint64_t>(first - begin), static_cast<uint64_t>(last - begin)))
		{
			return false;
		}
		cursor = last;
	}
	return true;
}

// Imported pages already have a pinned alias or tracked snapshot. Recheck every
// remaining interval of a range whose backing may have gained pages, so newly
// faulted pages are discovered.
// Quiesced invalidation clears the bitmap when those imports cease to be valid.
bool ImportResident(GraphicContext* ctx, Registry* registry, uint64_t base, Range* range, bool* changed)
{
	std::vector<uint8_t> resident;
	return ForEachUnimportedSpan(range, [&](uint64_t first, uint64_t last)
	                             { return ImportResidentSpan(ctx, registry, base, range, first, last, &resident, changed); });
}

// Physical ranges number in the thousands; only those with pages not imported
// yet are visited. Their unimported spans are asked about in one sweep of the
// backing, so a span before the next populated page costs no host query and
// only spans holding populated pages have their backing extents read.
bool ImportPhysicalRanges(GraphicContext* ctx, Registry* registry)
{
	struct Pending
	{
		uint64_t base  = 0;
		Range*   range = nullptr;
		uint64_t first = 0;
		uint64_t last  = 0;
	};
	std::vector<Pending>                            pending;
	std::vector<Kernel::Memory::KernelPhysicalSpan> spans;
	for (const uint64_t base: registry->partial)
	{
		auto&          range = registry->ranges.at(base);
		const uint64_t pages = range.size / kPageBytes;
		EXIT_IF(!range.physical_backing);
		if (range.imported_pages == pages)
		{
			continue;
		}
		range.imported.resize(static_cast<size_t>(pages), 0);
		if (range.imported_pages == 0)
		{
			pending.push_back({base, &range, 0, pages});
			spans.push_back({base, range.size});
			continue;
		}
		const uint64_t range_base  = base;
		Range*         range_entry = &range;
		(void)ForEachUnimportedSpan(range_entry,
		                            [&](uint64_t first, uint64_t last)
		                            {
			                            pending.push_back({range_base, range_entry, first, last});
			                            spans.push_back({range_base + first * kPageBytes, (last - first) * kPageBytes});
			                            return true;
		                            });
	}
	Kernel::Memory::KernelFindUnpopulatedPhysicalSpans(spans.data(), spans.size());
	std::vector<uint8_t> resident;
	for (size_t i = 0; i < pending.size(); i++)
	{
		const auto& span    = pending[i];
		bool        changed = false;
		if (spans[i].unpopulated)
		{
			continue;
		}
		if (!ImportPhysicalSpan(ctx, registry, span.base, span.range, span.first, span.last, &resident, &changed))
		{
			return false;
		}
		registry->dirty = registry->dirty || changed;
		if (span.range->tracked_chunks != 0)
		{
			registry->tracked.insert(span.base);
		}
	}
	for (auto base = registry->partial.begin(); base != registry->partial.end();)
	{
		const auto& range = registry->ranges.at(*base);
		base              = range.imported_pages == range.size / kPageBytes ? registry->partial.erase(base) : std::next(base);
	}
	return true;
}

// Writes a fresh host-visible table; keep the previous one alive while any
// registered reader may still reference it.
bool RebuildTable(GraphicContext* ctx, Registry* registry)
{
	// The device lookup binary-searches entries by guest base within one directory region.
	static_assert(sizeof(TableEntry) == kGuestDeviceAddressEntryDwords * 4u);
	std::vector<uint64_t> bases(registry->entries.size());
	for (std::size_t i = 0; i < bases.size(); ++i)
	{
		bases[i] = registry->entries[i].guest;
	}
	const auto     directory       = GuestDeviceAddressBuildDirectory(bases.data(), static_cast<uint32_t>(bases.size()));
	const uint64_t entry_bytes     = registry->entries.size() * sizeof(TableEntry);
	const uint64_t directory_bytes = directory.words.size() * sizeof(uint32_t);
	const uint64_t bytes           = kGuestDeviceAddressNullBytes + entry_bytes + directory_bytes;
	Table          table;
	table.buffer = CreateAddressBuffer(ctx, bytes, 0);
	VkMemoryRequirements requirements {};
	uint32_t             type_index = 0;
	if (table.buffer == nullptr)
	{
		return false;
	}
	vkGetBufferMemoryRequirements(ctx->device, table.buffer, &requirements);
	if (!FindMemoryType(ctx->physical_device, requirements.memoryTypeBits,
	                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type_index))
	{
		vkDestroyBuffer(ctx->device, table.buffer, nullptr);
		return false;
	}
	VkMemoryAllocateFlagsInfo flags {};
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo allocate {};
	allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext           = &flags;
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = type_index;
	void* mapped             = nullptr;
	if (vkAllocateMemory(ctx->device, &allocate, nullptr, &table.memory) != VK_SUCCESS ||
	    vkBindBufferMemory(ctx->device, table.buffer, table.memory, 0) != VK_SUCCESS ||
	    vkMapMemory(ctx->device, table.memory, 0, bytes, 0, &mapped) != VK_SUCCESS)
	{
		DestroyTable(ctx->device, table);
		return false;
	}
	// Each entry is the guest base, size, device address and span as
	// little-endian 64-bit words, the layout TableEntry already has.
	std::memset(mapped, 0, kGuestDeviceAddressNullBytes);
	const uint32_t header[4] = {static_cast<uint32_t>(directory.base), static_cast<uint32_t>(directory.base >> 32u), directory.shift,
	                            static_cast<uint32_t>(directory.words.size() - 2u)};
	std::memcpy(static_cast<uint8_t*>(mapped) + kGuestDeviceAddressDirectoryHeaderOffset, header, sizeof(header));
	if (entry_bytes != 0)
	{
		std::memcpy(static_cast<uint8_t*>(mapped) + kGuestDeviceAddressNullBytes, registry->entries.data(), entry_bytes);
	}
	std::memcpy(static_cast<uint8_t*>(mapped) + kGuestDeviceAddressNullBytes + entry_bytes, directory.words.data(), directory_bytes);
	vkUnmapMemory(ctx->device, table.memory);
	table.device  = BufferDeviceAddress(ctx->device, table.buffer);
	table.entries = static_cast<uint32_t>(registry->entries.size());
	if (registry->table.buffer != nullptr)
	{
		registry->retired.push_back(registry->table);
	}
	registry->table = table;
	registry->dirty = false;
	return true;
}

} // namespace

GuestDeviceAddressDirectory GuestDeviceAddressBuildDirectory(const uint64_t* sorted_bases, uint32_t count)
{
	// Few enough regions to rebuild with every table, fine enough that a region holds few entries.
	constexpr uint32_t kMaxRegions = 16384;
	constexpr uint32_t kMinShift   = 12;
	GuestDeviceAddressDirectory directory;
	directory.shift = kMinShift;
	if (count != 0)
	{
		const uint64_t extent = sorted_bases[count - 1u] - sorted_bases[0];
		while ((extent >> directory.shift) >= kMaxRegions)
		{
			++directory.shift;
		}
		directory.base = sorted_bases[0] & ~((uint64_t {1} << directory.shift) - 1u);
	}
	const uint64_t regions = count == 0 ? 1u : ((sorted_bases[count - 1u] - directory.base) >> directory.shift) + 1u;
	directory.words.resize(regions + 1u);
	uint32_t entry = 0;
	for (uint64_t region = 0; region < regions; ++region)
	{
		const uint64_t start = directory.base + (region << directory.shift);
		while (entry < count && sorted_bases[entry] <= start)
		{
			++entry;
		}
		directory.words[region] = entry;
	}
	directory.words[regions] = count;
	return directory;
}

void GuestDeviceAddressRegisterRange(uint64_t vaddr, uint64_t size, bool physical_backing)
{
	if (vaddr == 0 || size == 0 || (vaddr % kPageBytes) != 0 || (size % kPageBytes) != 0)
	{
		return;
	}
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	// The table mirrors the chunks of the registered ranges; a replaced range's
	// chunks leave it with the range.
	if (const auto replaced = registry.ranges.find(vaddr); replaced != registry.ranges.end())
	{
		for (const auto& chunk: replaced->second.chunks)
		{
			RemoveTableEntry(&registry, chunk);
		}
	}
	registry.ranges[vaddr]      = {size, {}, {}, physical_backing};
	registry.tracked.erase(vaddr);
	if (physical_backing)
	{
		registry.flexible.erase(vaddr);
		registry.partial.insert(vaddr);
	} else
	{
		registry.flexible.insert(vaddr);
		registry.partial.erase(vaddr);
	}
	registry.dirty              = true;
	registry.population_scanned = false;
}

void GuestDeviceAddressInvalidateRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	ClearReaders(&registry);
	const uint64_t              end = vaddr + size;
	for (auto it = FirstOverlapCandidate(&registry.ranges, vaddr); it != registry.ranges.end() && it->first < end; ++it)
	{
		DropChunks(ctx->device, &registry, it->first, &it->second, vaddr, end);
	}
	DestroyRetiredTables(ctx->device, &registry);
}

void GuestDeviceAddressReleaseRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	ClearReaders(&registry);
	const uint64_t              end = vaddr + size;
	// Pieces of a partially unmapped range stay mapped and stay registered.
	std::vector<std::pair<uint64_t, Range>> survivors;
	for (auto it = FirstOverlapCandidate(&registry.ranges, vaddr); it != registry.ranges.end() && it->first < end;)
	{
		const uint64_t base       = it->first;
		const uint64_t range_end  = base + it->second.size;
		const bool     physical   = it->second.physical_backing;
		for (const auto& chunk: it->second.chunks)
		{
			DestroyChunk(ctx->device, &registry, chunk);
		}
		registry.flexible.erase(base);
		registry.tracked.erase(base);
		registry.partial.erase(base);
		it              = registry.ranges.erase(it);
		registry.dirty  = true;
		if (base < vaddr)
		{
			survivors.push_back({base, Range {vaddr - base, {}, {}, physical, 0}});
		}
		if (end < range_end)
		{
			survivors.push_back({end, Range {range_end - end, {}, {}, physical, 0}});
		}
	}
	for (auto& [base, range]: survivors)
	{
		if (!range.physical_backing)
		{
			registry.flexible.insert(base);
		} else
		{
			registry.partial.insert(base);
		}
		registry.ranges.emplace(base, std::move(range));
		registry.population_scanned = false;
	}
	DestroyRetiredTables(ctx->device, &registry);
}

// GPU memory asks this under its own lock; the registry never reaches GPU
// memory while it holds the registry lock, so the nesting has one order.
static bool OverlapsRegisteredRange(uint64_t vaddr, uint64_t size)
{
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	const auto                  it = FirstOverlapCandidate(&registry.ranges, vaddr);
	return it != registry.ranges.end() && it->first < vaddr + size;
}

bool GuestDeviceAddressPendingWriteBack(GpuQueueId consumer, const std::vector<SubmissionId>& device_awaited,
                                        std::vector<GpuMemoryStoragePublication>* publishable, SubmissionId* dependency)
{
	EXIT_IF(dependency == nullptr);
	return GpuMemoryPendingStorageWriteBack(OverlapsRegisteredRange, consumer, device_awaited, publishable, dependency);
}

void GuestDeviceAddressWriteBack(GraphicContext* ctx, GpuQueueId consumer)
{
	GpuMemoryWriteBackStorageRanges(ctx, OverlapsRegisteredRange, consumer);
}

struct PublicationPiece
{
	VkBuffer     target        = nullptr;
	VkDeviceSize source_offset = 0;
	VkDeviceSize target_offset = 0;
	VkDeviceSize size          = 0;
};

// Splits the guest range over the physical chunks that alias it. Fails when
// any byte lies outside such a chunk.
static bool ResolvePublicationPiecesLocked(const Registry& registry, uint64_t vaddr, uint64_t size, std::vector<PublicationPiece>* pieces)
{
	const uint64_t end = vaddr + size;
	for (uint64_t cursor = vaddr; cursor < end;)
	{
		auto range = registry.ranges.upper_bound(cursor);
		if (range == registry.ranges.begin())
		{
			return false;
		}
		--range;
		const Chunk* found = nullptr;
		for (const auto& chunk: range->second.chunks)
		{
			if (chunk.physical && chunk.guest <= cursor && cursor - chunk.guest < chunk.size)
			{
				found = &chunk;
				break;
			}
		}
		if (found == nullptr)
		{
			return false;
		}
		const uint64_t piece_end = std::min(end, found->guest + found->size);
		pieces->push_back({found->buffer, cursor - vaddr, found->buffer_offset + (cursor - found->guest), piece_end - cursor});
		cursor = piece_end;
	}
	return true;
}

// Earlier device-address reads of the range and the producer's writes
// complete before the transfer.
static void RecordPublicationBarrierBefore(VkCommandBuffer_T* cmd)
{
	VkMemoryBarrier before {};
	before.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0,
	                     nullptr);
}

// Every later command observes the transfer.
static void RecordPublicationBarrierAfter(VkCommandBuffer_T* cmd)
{
	VkMemoryBarrier after {};
	after.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
	                     &after, 0, nullptr, 0, nullptr);
}

bool GuestDeviceAddressRecordPublication(GraphicContext* ctx, VkCommandBuffer_T* cmd, const VulkanBuffer& source, uint64_t vaddr,
                                         uint64_t size)
{
	if (ctx == nullptr || cmd == nullptr || source.buffer == nullptr || size == 0 || vaddr > UINT64_MAX - size ||
	    !ctx->guest_device_address_supported)
	{
		return false;
	}
	std::vector<PublicationPiece> pieces;
	auto&                         registry = GetRegistry();
	std::lock_guard<std::mutex>   lock(registry.mutex);
	if (!ResolvePublicationPiecesLocked(registry, vaddr, size, &pieces))
	{
		return false;
	}
	RecordPublicationBarrierBefore(cmd);
	for (const auto& piece: pieces)
	{
		const VkBufferCopy copy {piece.source_offset, piece.target_offset, piece.size};
		vkCmdCopyBuffer(cmd, source.buffer, piece.target, 1, &copy);
	}
	RecordPublicationBarrierAfter(cmd);
	return true;
}

bool GuestDeviceAddressRecordFill(GraphicContext* ctx, VkCommandBuffer_T* cmd, uint32_t word, uint64_t vaddr, uint64_t size)
{
	if (ctx == nullptr || cmd == nullptr || size == 0 || vaddr > UINT64_MAX - size || !ctx->guest_device_address_supported)
	{
		return false;
	}
	std::vector<PublicationPiece> pieces;
	auto&                         registry = GetRegistry();
	std::lock_guard<std::mutex>   lock(registry.mutex);
	if (!ResolvePublicationPiecesLocked(registry, vaddr, size, &pieces))
	{
		return false;
	}
	// vkCmdFillBuffer writes whole 4-byte words at 4-byte offsets; each piece
	// must also keep the word's phase from the start of the range.
	for (const auto& piece: pieces)
	{
		if ((piece.source_offset % 4u) != 0u || (piece.target_offset % 4u) != 0u || (piece.size % 4u) != 0u)
		{
			return false;
		}
	}
	RecordPublicationBarrierBefore(cmd);
	for (const auto& piece: pieces)
	{
		vkCmdFillBuffer(cmd, piece.target, piece.target_offset, piece.size, word);
	}
	RecordPublicationBarrierAfter(cmd);
	return true;
}

static GuestDeviceAddressPrepareResult PrepareLocked(GraphicContext* ctx, Registry* registry, GuestDeviceAddressTable* table,
	                                                    SubmissionId* dependency)
{
	EXIT_IF(registry == nullptr || table == nullptr || dependency == nullptr);
	if (ctx == nullptr || !ctx->guest_device_address_supported)
	{
		return GuestDeviceAddressPrepareResult::Failed;
	}
	const DebugStatsScopedTimer prepare_timer(DebugStatsRecordGuestAddressPrepare);
	// Query before scanning: a page populated during the scan changes the next query.
	Core::VirtualMemory::SharedBackingPopulation population;
	const bool known    = Kernel::Memory::KernelQueryPhysicalPopulation(&population);
	const bool physical = !known || !registry->population_scanned || population != registry->scanned_population;
	// Ranges number in the thousands for some titles: time each phase once
	// per preparation, not once per range.
	{
		const DebugStatsScopedTimer residency_timer(DebugStatsRecordGuestAddressResidency);
		const auto                  revisit = [&](uint64_t base, Range* range)
		{
			bool changed = false;
			if (!ImportResident(ctx, registry, base, range, &changed))
			{
				return false;
			}
			registry->dirty = registry->dirty || changed;
			if (range->tracked_chunks != 0)
			{
				registry->tracked.insert(base);
			}
			return true;
		};
		if (physical && !ImportPhysicalRanges(ctx, registry))
		{
			return GuestDeviceAddressPrepareResult::Failed;
		}
		for (const uint64_t base: registry->flexible)
		{
			if (!revisit(base, &registry->ranges.at(base)))
			{
				return GuestDeviceAddressPrepareResult::Failed;
			}
		}
	}
	// Refresh snapshots the CPU wrote since they were taken; the imported
	// host memory is coherent, so the device sees the refresh directly.
	const DebugStatsScopedTimer refresh_timer(DebugStatsRecordGuestAddressRefresh);
	for (const uint64_t base: registry->tracked)
	{
		for (auto& chunk: registry->ranges.at(base).chunks)
		{
			if (!chunk.tracked || !GpuDirtyPageTracker::Instance().ChangedSince(chunk.guest, chunk.span, chunk.generation))
			{
				continue;
			}
			// A write fault can dirty this snapshot after the whole-registry preflight
			// above. Check again at the refresh point before replacing bytes that an
			// in-flight GPU reader may still observe.
			if (const SubmissionId pending = FirstActiveReader(*registry); pending.sequence != 0)
			{
				*dependency = pending;
				return GuestDeviceAddressPrepareResult::PendingReader;
			}
			if (!SnapshotGuest(chunk.copy, chunk.guest, chunk.span, &chunk.generation))
			{
				return GuestDeviceAddressPrepareResult::Failed;
			}
		}
	}
	if (registry->dirty)
	{
		const SubmissionId active_reader       = FirstActiveReader(*registry);
		const bool         retires_current     = registry->table.buffer != nullptr;
		const bool         would_exceed_budget = retires_current && registry->retired.size() >= kMaxRetiredTables;
		if (would_exceed_budget && active_reader.sequence != 0)
		{
			*dependency = active_reader;
			return GuestDeviceAddressPrepareResult::PendingReader;
		}
		if (active_reader.sequence == 0)
		{
			DestroyRetiredTables(ctx->device, registry);
		}
		if (!RebuildTable(ctx, registry))
		{
			return GuestDeviceAddressPrepareResult::Failed;
		}
	}
	registry->scanned_population = population;
	registry->population_scanned = known;
	table->address               = registry->table.device;
	table->entry_count           = registry->table.entries;
	return GuestDeviceAddressPrepareResult::Ready;
}

bool GuestDeviceAddressPrepare(GraphicContext* ctx, uint64_t* table_address, uint32_t* entry_count)
{
	if (ctx == nullptr || table_address == nullptr || entry_count == nullptr || !ctx->guest_device_address_supported)
	{
		return false;
	}
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	if (const SubmissionId dependency = PendingReaderForSnapshotRefresh(registry); dependency.sequence != 0)
	{
		return false;
	}
	GuestDeviceAddressTable table {};
	SubmissionId dependency {};
	if (PrepareLocked(ctx, &registry, &table, &dependency) != GuestDeviceAddressPrepareResult::Ready) { return false; }
	*table_address = table.address;
	*entry_count   = table.entry_count;
	return true;
}

GuestDeviceAddressPrepareResult GuestDeviceAddressPrepareForRead(GraphicContext* ctx, SubmissionId reader,
	                                                              GuestDeviceAddressTable* table, SubmissionId* dependency)
{
	if (dependency != nullptr) { *dependency = {}; }
	if (ctx == nullptr || table == nullptr || dependency == nullptr || !ctx->guest_device_address_supported || !ValidReader(reader))
	{
		return GuestDeviceAddressPrepareResult::Failed;
	}
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	if (const SubmissionId pending = PendingReaderForSnapshotRefresh(registry); pending.sequence != 0)
	{
		*dependency = pending;
		return GuestDeviceAddressPrepareResult::PendingReader;
	}
	GuestDeviceAddressTable prepared {};
	const auto result = PrepareLocked(ctx, &registry, &prepared, dependency);
	if (result != GuestDeviceAddressPrepareResult::Ready) { return result; }
	auto& latest_reader = registry.readers[reader.queue.Value()];
	// This slot is scoped to reader.queue, so sequence ordering is never compared across queues.
	if (!latest_reader.active || reader.sequence >= latest_reader.submission.sequence)
	{
		latest_reader = {reader, true};
	}
	*table                              = prepared;
	return GuestDeviceAddressPrepareResult::Ready;
}

bool GuestDeviceAddressAcknowledgeReader(SubmissionId completed)
{
	if (!ValidReader(completed)) { return false; }
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	auto&                       reader = registry.readers[completed.queue.Value()];
	if (!reader.active || reader.submission != completed) { return false; }
	reader = {};
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
