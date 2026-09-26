#include "Emulator/Graphics/GuestDeviceAddress.h"

#include "Emulator/Graphics/GraphicContext.h"

#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

// Device buffers are limited to 4 GiB; 1 GiB chunks keep allocations modest.
constexpr uint64_t kChunkBytes = 1ull << 30u;
constexpr uint64_t kPageBytes  = 4096;

struct Chunk
{
	uint64_t       guest  = 0;
	uint64_t       size   = 0;
	VkDeviceMemory memory = nullptr;
	VkBuffer       buffer = nullptr;
	uint64_t       device = 0;
};

struct Range
{
	uint64_t           size = 0;
	std::vector<Chunk> chunks; // empty until imported
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
bool ImportChunk(GraphicContext* ctx, uint64_t guest, uint64_t size, Chunk* out)
{
	static auto get_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
	    vkGetDeviceProcAddr(ctx->device, "vkGetMemoryHostPointerPropertiesEXT"));
	auto* pointer = reinterpret_cast<void*>(guest);
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
	*out = {guest, size, memory, buffer, BufferDeviceAddress(ctx->device, buffer)};
	return true;
}

bool ImportRange(GraphicContext* ctx, uint64_t base, Range* range)
{
	for (uint64_t offset = 0; offset < range->size; offset += kChunkBytes)
	{
		const uint64_t size = range->size - offset < kChunkBytes ? range->size - offset : kChunkBytes;
		Chunk          chunk;
		if (!ImportChunk(ctx, base + offset, size, &chunk))
		{
			for (const auto& imported: range->chunks)
			{
				DestroyChunk(ctx->device, imported);
			}
			range->chunks.clear();
			return false;
		}
		range->chunks.push_back(chunk);
	}
	return true;
}

// Writes a fresh host-visible table; the previous one stays alive until the
// next quiesced release because in-flight work may still read it.
bool RebuildTable(GraphicContext* ctx, Registry* registry)
{
	std::vector<uint32_t> words(kGuestDeviceAddressNullBytes / 4u, 0u);
	for (const auto& [base, range]: registry->ranges)
	{
		for (const auto& chunk: range.chunks)
		{
			const uint32_t entry[kGuestDeviceAddressEntryDwords] = {static_cast<uint32_t>(chunk.guest), static_cast<uint32_t>(chunk.guest >> 32u),
			                                                        static_cast<uint32_t>(chunk.size),  static_cast<uint32_t>(chunk.size >> 32u),
			                                                        static_cast<uint32_t>(chunk.device), static_cast<uint32_t>(chunk.device >> 32u),
			                                                        0,
			                                                        0};
			words.insert(words.end(), entry, entry + kGuestDeviceAddressEntryDwords);
		}
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
	registry.ranges[vaddr] = {size, {}};
	registry.dirty         = true;
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
		if (!range.chunks.empty())
		{
			continue;
		}
		if (!ImportRange(ctx, base, &range))
		{
			return false;
		}
		registry.dirty = true;
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
