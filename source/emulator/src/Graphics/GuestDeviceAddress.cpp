#include "Emulator/Graphics/GuestDeviceAddress.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Kernel/Memory.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

// Imports cover resident pages only (never-touched pages read as zero through
// the table's zero prefix), in chunks bounded to keep each pin modest.
constexpr uint64_t kChunkBytes = 64ull << 20u;
constexpr uint64_t kPageBytes  = kGuestDeviceAddressPageBytes;

struct Chunk
{
	uint64_t       guest  = 0;
	uint64_t       size   = 0; // bytes the table maps to this chunk
	uint64_t       span   = 0; // imported bytes
	VkDeviceMemory memory = nullptr;
	VkBuffer       buffer = nullptr;
	uint64_t       device = 0;
	uint64_t       alias  = 0;       // writable host view that was imported
	void*          copy   = nullptr; // tracked snapshot that was imported
	uint64_t       generation = 0;   // dirty-tracker generation of the snapshot
	bool           tracked    = false; // snapshot is refreshed from tracker generations
	bool           registered = false; // this chunk holds a dirty-tracker registration
};

struct Range
{
	uint64_t             size = 0;
	std::vector<Chunk>   chunks;
	std::vector<uint8_t> imported; // one byte per page
};

struct Table
{
	VkDeviceMemory memory  = nullptr;
	VkBuffer       buffer  = nullptr;
	uint64_t       device  = 0;
	uint32_t       entries = 0;
};

struct Registry
{
	std::mutex                mutex;
	std::map<uint64_t, Range> ranges;
	bool                      dirty = true;
	Table                     table;
	std::vector<Table>        retired;
};

Registry& GetRegistry()
{
	static Registry registry;
	return registry;
}

void DestroyChunk(VkDevice device, const Chunk& chunk)
{
	vkDestroyBuffer(device, chunk.buffer, nullptr);
	vkFreeMemory(device, chunk.memory, nullptr);
	Kernel::Memory::KernelUnmapPhysicalAlias(chunk.alias);
	std::free(chunk.copy);
	if (chunk.registered)
	{
		(void)GpuDirtyPageTracker::Instance().UnregisterRange(chunk.guest, chunk.span);
	}
}

// Copies guest memory into a snapshot using the dirty tracker's read protocol
// (arm write protection, copy, validate the generation). Returns false when
// the range is not tracked, since a snapshot could then go stale unnoticed.
bool SnapshotGuest(void* copy, uint64_t guest, uint64_t size, uint64_t* generation)
{
	auto& tracker = GpuDirtyPageTracker::Instance();
	for (int attempt = 0; attempt < 8; attempt++)
	{
		const auto observation = tracker.BeginRead(guest, size);
		if (!observation.tracked || !Core::VirtualMemory::CopyFromGuest(copy, guest, size))
		{
			return false;
		}
		if (tracker.ReadObservationIsStable(guest, size, observation))
		{
			*generation = observation.generation;
			return true;
		}
	}
	return false;
}

void DestroyTable(VkDevice device, const Table& table)
{
	if (table.buffer != nullptr)
	{
		vkDestroyBuffer(device, table.buffer, nullptr);
		vkFreeMemory(device, table.memory, nullptr);
	}
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

VkBuffer CreateAddressBuffer(VkDevice device, uint64_t size, bool external)
{
	VkExternalMemoryBufferCreateInfo external_info {};
	external_info.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
	external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

	VkBufferCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.pNext       = external ? &external_info : nullptr;
	info.size        = size;
	info.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer buffer  = nullptr;
	return vkCreateBuffer(device, &info, nullptr, &buffer) == VK_SUCCESS ? buffer : nullptr;
}

uint64_t BufferDeviceAddress(VkDevice device, VkBuffer buffer)
{
	VkBufferDeviceAddressInfo info {};
	info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = buffer;
	return vkGetBufferDeviceAddress(device, &info);
}

// Imports [guest, guest + size) of host memory as a device-addressable buffer.
// Imports `size` bytes of host memory at `pointer` as a device-addressable
// buffer. Host-pointer import requires writable, page-aligned memory.
bool ImportPointer(GraphicContext* ctx, void* pointer, uint64_t size, Chunk* out)
{
	static auto get_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
	    vkGetDeviceProcAddr(ctx->device, "vkGetMemoryHostPointerPropertiesEXT"));
	VkMemoryHostPointerPropertiesEXT host_properties {};
	host_properties.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
	uint32_t type_index   = 0;
	if (get_properties == nullptr ||
	    get_properties(ctx->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer, &host_properties) != VK_SUCCESS ||
	    !FindMemoryType(ctx->physical_device, host_properties.memoryTypeBits, 0, &type_index))
	{
		return false;
	}
	VkBuffer buffer = CreateAddressBuffer(ctx->device, size, true);
	if (buffer == nullptr)
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
	allocate.allocationSize  = size;
	allocate.memoryTypeIndex = type_index;
	VkDeviceMemory memory    = nullptr;
	if (vkAllocateMemory(ctx->device, &allocate, nullptr, &memory) != VK_SUCCESS ||
	    vkBindBufferMemory(ctx->device, buffer, memory, 0) != VK_SUCCESS)
	{
		if (memory != nullptr)
		{
			vkFreeMemory(ctx->device, memory, nullptr);
		}
		vkDestroyBuffer(ctx->device, buffer, nullptr);
		return false;
	}
	out->memory = memory;
	out->buffer = buffer;
	out->device = BufferDeviceAddress(ctx->device, buffer);
	return true;
}

// Imports [guest, guest + span) for a chunk mapping [guest, guest + size)
// from a writable alias of physical direct memory, or else from a snapshot.
// The guest view itself is never imported: dirty tracking and guest mprotect
// change its protection, and a driver that pins host pages (a userptr) cannot
// revalidate a page that lost write access, which loses the device.
bool ImportChunk(GraphicContext* ctx, uint64_t guest, uint64_t size, uint64_t span, Chunk* out)
{
	*out       = {};
	out->guest = guest;
	out->size  = size;
	out->span  = span;
	if (const uint64_t alias = Kernel::Memory::KernelMapPhysicalAlias(guest, span); alias != 0)
	{
		out->alias = alias;
		if (ImportPointer(ctx, reinterpret_cast<void*>(alias), span, out))
		{
			return true;
		}
		Kernel::Memory::KernelUnmapPhysicalAlias(alias);
		return false;
	}
	void* copy = std::aligned_alloc(kPageBytes, span);
	// Writable memory: a snapshot refreshed when the dirty tracker sees CPU
	// writes; the chunk registers the range so the tracker covers it.
	// Memory the host keeps non-writable (guest mprotect to read-only) cannot
	// change until another mprotect, which invalidates the import.
	auto&      tracker   = GpuDirtyPageTracker::Instance();
	const bool writable  = Core::VirtualMemory::IsRangeWritable(guest, span);
	out->registered      = copy != nullptr && writable && tracker.RegisterRange(guest, span);
	const bool tracked   = out->registered && SnapshotGuest(copy, guest, span, &out->generation);
	const bool immutable = copy != nullptr && !writable && Core::VirtualMemory::CopyFromGuest(copy, guest, span);
	if ((tracked || immutable) && ImportPointer(ctx, copy, span, out))
	{
		out->copy    = copy;
		out->tracked = tracked;
		return true;
	}
	if (out->registered)
	{
		(void)tracker.UnregisterRange(guest, span);
		out->registered = false;
	}
	std::free(copy);
	return false;
}

bool ImportResidentSpan(GraphicContext* ctx, uint64_t base, Range* range, uint64_t first, uint64_t last,
                        std::vector<uint8_t>* resident, bool* changed)
{
	const uint64_t pages        = last - first;
	const uint64_t span_address = base + first * kPageBytes;
	const uint64_t span_size    = pages * kPageBytes;
	// A wholly sparse physical interval cannot contain resident pages. Requery
	// each preparation so a later guest write is discovered normally.
	if (Kernel::Memory::KernelIsPhysicalRangeUnpopulated(span_address, span_size))
	{
		return true;
	}
	resident->resize(static_cast<size_t>(pages));
	if (!Core::VirtualMemory::QueryResidentPages(span_address, span_size, resident->data()))
	{
		return false;
	}
	for (uint64_t page = 0; page < pages;)
	{
		if ((*resident)[page] == 0)
		{
			page++;
			continue;
		}
		uint64_t end = page;
		while (end < pages && (*resident)[end] != 0 && (end - page) * kPageBytes < kChunkBytes)
		{
			end++;
		}
		const uint64_t guest = base + (first + page) * kPageBytes;
		const uint64_t size  = (end - page) * kPageBytes;
		Chunk          chunk;
		// Do not pin an extra guest page: the import itself would make that
		// page resident and every subsequent prepare would import another one.
		// Translated loads split at page boundaries instead.
		if (!ImportChunk(ctx, guest, size, size, &chunk))
		{
			std::fprintf(stderr, "guest import failed: base=0x%012" PRIx64 " size=0x%" PRIx64 " host_writable=%d\n", guest, size,
			             Core::VirtualMemory::IsRangeWritable(guest, size) ? 1 : 0);
			return false;
		}
		range->chunks.push_back(chunk);
		std::fill(range->imported.begin() + static_cast<std::ptrdiff_t>(first + page),
		          range->imported.begin() + static_cast<std::ptrdiff_t>(first + end), 1);
		*changed = true;
		page     = end;
	}
	return true;
}

// Imported pages already have a pinned alias or tracked snapshot. Recheck every
// remaining interval on each preparation so newly faulted pages are discovered.
// Quiesced invalidation clears the bitmap when those imports cease to be valid.
bool ImportResident(GraphicContext* ctx, uint64_t base, Range* range, bool* changed)
{
	range->imported.resize(static_cast<size_t>(range->size / kPageBytes), 0);
	std::vector<uint8_t> resident;
	const auto limit = range->imported.end();
	for (auto cursor = range->imported.begin(); cursor != limit;)
	{
		const auto first = std::find(cursor, limit, uint8_t {0});
		if (first == limit) { break; }
		const auto last = std::find(first, limit, uint8_t {1});
		if (!ImportResidentSpan(ctx, base, range, static_cast<uint64_t>(first - range->imported.begin()),
		                        static_cast<uint64_t>(last - range->imported.begin()), &resident, changed))
		{
			return false;
		}
		cursor = last;
	}
	return true;
}

// Writes a fresh host-visible table; the previous one stays alive until the
// next quiesced release because in-flight work may still read it.
bool RebuildTable(GraphicContext* ctx, Registry* registry)
{
	// The device lookup binary-searches entries by guest base.
	std::vector<const Chunk*> sorted;
	for (const auto& [base, range]: registry->ranges)
	{
		for (const auto& chunk: range.chunks)
		{
			sorted.push_back(&chunk);
		}
	}
	std::sort(sorted.begin(), sorted.end(), [](const Chunk* a, const Chunk* b) { return a->guest < b->guest; });
	std::vector<uint32_t> words(kGuestDeviceAddressNullBytes / 4u, 0u);
	for (const auto* chunk: sorted)
	{
		const uint32_t entry[kGuestDeviceAddressEntryDwords] = {static_cast<uint32_t>(chunk->guest), static_cast<uint32_t>(chunk->guest >> 32u),
		                                                        static_cast<uint32_t>(chunk->size),  static_cast<uint32_t>(chunk->size >> 32u),
		                                                        static_cast<uint32_t>(chunk->device), static_cast<uint32_t>(chunk->device >> 32u),
		                                                        static_cast<uint32_t>(chunk->span),  static_cast<uint32_t>(chunk->span >> 32u)};
		words.insert(words.end(), entry, entry + kGuestDeviceAddressEntryDwords);
	}
	const uint64_t bytes  = words.size() * 4u;
	Table          table;
	table.buffer          = CreateAddressBuffer(ctx->device, bytes, false);
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
	std::memcpy(mapped, words.data(), bytes);
	vkUnmapMemory(ctx->device, table.memory);
	table.device  = BufferDeviceAddress(ctx->device, table.buffer);
	table.entries = static_cast<uint32_t>((words.size() - kGuestDeviceAddressNullBytes / 4u) / kGuestDeviceAddressEntryDwords);
	if (registry->table.buffer != nullptr)
	{
		registry->retired.push_back(registry->table);
	}
	registry->table = table;
	registry->dirty = false;
	return true;
}

} // namespace

void GuestDeviceAddressRegisterRange(uint64_t vaddr, uint64_t size)
{
	if (vaddr == 0 || size == 0 || (vaddr % kPageBytes) != 0 || (size % kPageBytes) != 0)
	{
		return;
	}
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	registry.ranges[vaddr] = {size, {}, {}};
	registry.dirty         = true;
}

void GuestDeviceAddressInvalidateRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	for (auto& [base, range]: registry.ranges)
	{
		if (!(base < vaddr + size && vaddr < base + range.size))
		{
			continue;
		}
		for (const auto& chunk: range.chunks)
		{
			DestroyChunk(ctx->device, chunk);
		}
		range.chunks.clear();
		range.imported.assign(range.imported.size(), 0);
		registry.dirty = true;
	}
}

void GuestDeviceAddressReleaseRangeQuiesced(GraphicContext* ctx, uint64_t vaddr, uint64_t size)
{
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	for (auto it = registry.ranges.begin(); it != registry.ranges.end();)
	{
		const bool overlaps = it->first < vaddr + size && vaddr < it->first + it->second.size;
		if (!overlaps)
		{
			++it;
			continue;
		}
		for (const auto& chunk: it->second.chunks)
		{
			DestroyChunk(ctx->device, chunk);
		}
		it             = registry.ranges.erase(it);
		registry.dirty = true;
	}
	for (const auto& table: registry.retired)
	{
		DestroyTable(ctx->device, table);
	}
	registry.retired.clear();
}

static std::vector<std::pair<uint64_t, uint64_t>> RegisteredRangesSnapshot()
{
	std::vector<std::pair<uint64_t, uint64_t>> ranges;
	{
		auto&                       registry = GetRegistry();
		std::lock_guard<std::mutex> lock(registry.mutex);
		for (const auto& [base, range]: registry.ranges)
		{
			ranges.emplace_back(base, range.size);
		}
	}
	return ranges;
}

bool GuestDeviceAddressPendingWriteBack(SubmissionId* dependency)
{
	EXIT_IF(dependency == nullptr);
	for (const auto& [base, size]: RegisteredRangesSnapshot())
	{
		if (GpuMemoryPendingStorageWriteBack(base, size, dependency)) { return true; }
	}
	return false;
}

void GuestDeviceAddressWriteBack(GraphicContext* ctx)
{
	const auto ranges = RegisteredRangesSnapshot();
	// GPU-memory mutation never nests inside the address registry lock.
	for (const auto& [base, size]: ranges)
	{
		GpuMemoryWriteBackStorageRange(ctx, base, size);
	}
}

bool GuestDeviceAddressPrepare(GraphicContext* ctx, uint64_t* table_address, uint32_t* entry_count)
{
	if (ctx == nullptr || table_address == nullptr || entry_count == nullptr || !ctx->guest_device_address_supported)
	{
		return false;
	}
	auto&                       registry = GetRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	for (auto& [base, range]: registry.ranges)
	{
		bool changed = false;
		if (!ImportResident(ctx, base, &range, &changed))
		{
			return false;
		}
		registry.dirty = registry.dirty || changed;
		// Refresh snapshots the CPU wrote since they were taken; the imported
		// host memory is coherent, so the device sees the refresh directly.
		for (auto& chunk: range.chunks)
		{
			if (chunk.tracked && GpuDirtyPageTracker::Instance().ChangedSince(chunk.guest, chunk.span, chunk.generation) &&
			    !SnapshotGuest(chunk.copy, chunk.guest, chunk.span, &chunk.generation))
			{
				return false;
			}
		}
	}
	if (registry.dirty && !RebuildTable(ctx, &registry))
	{
		return false;
	}
	*table_address = registry.table.device;
	*entry_count   = registry.table.entries;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
