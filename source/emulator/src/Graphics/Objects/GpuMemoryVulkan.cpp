#include "GpuMemoryInternal.h"

#include "Kyty/Core/Database.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GpuDeferredDeletionQueue.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GpuMemoryMaterializationCache.h"
#include "Emulator/Graphics/GpuMemoryRangeQueryCache.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct VulkanMemoryStat
{
	std::atomic_uint64_t allocated[VK_MAX_MEMORY_TYPES];
	std::atomic_uint64_t count[VK_MAX_MEMORY_TYPES];
};

static VulkanMemoryStat* g_mem_stat = nullptr;

void GpuMemoryVulkanStatsInit()
{
	g_mem_stat = new VulkanMemoryStat;

	for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; i++)
	{
		g_mem_stat->allocated[i] = 0;
		g_mem_stat->count[i]     = 0;
	}
}

// Small allocations share large device-memory blocks. Every vkAllocateMemory
// costs the driver a GPU VA allocation and a VM bind; per-upload staging
// buffers and small objects made that the dominant CPU cost.
namespace {

constexpr VkDeviceSize kPoolBlockBytes     = 64ull << 20u;
constexpr VkDeviceSize kPoolMaxAllocation  = 4ull << 20u;

struct PoolBlock
{
	VkDevice                               device = nullptr;
	VkDeviceMemory                         memory = nullptr;
	uint8_t*                               mapped = nullptr;
	uint32_t                               type   = 0;
	VulkanMemoryResource                   resource {};
	VkDeviceSize                           used   = 0;
	std::map<VkDeviceSize, VkDeviceSize>   free_ranges; // offset -> size, coalesced
};

struct MemoryPool
{
	std::mutex                              mutex;
	std::vector<std::unique_ptr<PoolBlock>> blocks;
};

MemoryPool& GetMemoryPool()
{
	static MemoryPool pool;
	return pool;
}

bool PoolTakeRange(PoolBlock* block, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize* offset)
{
	for (auto it = block->free_ranges.begin(); it != block->free_ranges.end(); ++it)
	{
		const VkDeviceSize start   = it->first;
		const VkDeviceSize length  = it->second;
		const VkDeviceSize aligned = (start + alignment - 1) / alignment * alignment;
		if (aligned + size > start + length)
		{
			continue;
		}
		block->free_ranges.erase(it);
		if (aligned > start)
		{
			block->free_ranges.emplace(start, aligned - start);
		}
		if (aligned + size < start + length)
		{
			block->free_ranges.emplace(aligned + size, start + length - aligned - size);
		}
		block->used += size;
		*offset = aligned;
		return true;
	}
	return false;
}

void PoolReturnRange(PoolBlock* block, VkDeviceSize offset, VkDeviceSize size)
{
	block->used -= size;
	auto next = block->free_ranges.lower_bound(offset);
	if (next != block->free_ranges.begin())
	{
		auto previous = std::prev(next);
		if (previous->first + previous->second == offset)
		{
			size += previous->second;
			offset = previous->first;
			block->free_ranges.erase(previous);
		}
	}
	if (next != block->free_ranges.end() && offset + size == next->first)
	{
		size += next->second;
		block->free_ranges.erase(next);
	}
	block->free_ranges.emplace(offset, size);
}

// Returns false when the request should get its own allocation.
bool PoolAllocate(GraphicContext* ctx, VulkanMemory* mem, uint32_t type, VkMemoryPropertyFlags type_flags, VulkanMemoryResource resource)
{
	const bool host_visible = (type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
	if (mem->requirements.size > kPoolMaxAllocation ||
	    (host_visible && (type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0))
	{
		return false;
	}
	const VkDeviceSize alignment = std::max<VkDeviceSize>(mem->requirements.alignment, 1);
	auto&              pool      = GetMemoryPool();
	std::lock_guard    lock(pool.mutex);
	for (auto& block: pool.blocks)
	{
		VkDeviceSize offset = 0;
		if (block->device == ctx->device && block->type == type && block->resource == resource &&
		    kPoolBlockBytes - block->used >= mem->requirements.size &&
		    PoolTakeRange(block.get(), mem->requirements.size, alignment, &offset))
		{
			mem->memory     = block->memory;
			mem->offset     = offset;
			mem->pool_block = block.get();
			return true;
		}
	}
	auto block      = std::make_unique<PoolBlock>();
	block->device   = ctx->device;
	block->type     = type;
	block->resource = resource;
	VkMemoryAllocateInfo alloc_info {};
	alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.allocationSize  = kPoolBlockBytes;
	alloc_info.memoryTypeIndex = type;
	if (vkAllocateMemory(ctx->device, &alloc_info, nullptr, &block->memory) != VK_SUCCESS)
	{
		return false;
	}
	if (host_visible)
	{
		void* mapped = nullptr;
		if (vkMapMemory(ctx->device, block->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
		{
			vkFreeMemory(ctx->device, block->memory, nullptr);
			return false;
		}
		block->mapped = static_cast<uint8_t*>(mapped);
	}
	block->free_ranges.emplace(0, kPoolBlockBytes);
	VkDeviceSize offset = 0;
	EXIT_IF(!PoolTakeRange(block.get(), mem->requirements.size, alignment, &offset));
	mem->memory     = block->memory;
	mem->offset     = offset;
	mem->pool_block = block.get();
	pool.blocks.push_back(std::move(block));
	return true;
}

void PoolFree(GraphicContext* ctx, VulkanMemory* mem)
{
	auto&           pool = GetMemoryPool();
	std::lock_guard lock(pool.mutex);
	auto*           block = static_cast<PoolBlock*>(mem->pool_block);
	PoolReturnRange(block, mem->offset, mem->requirements.size);
	if (block->used != 0)
	{
		return;
	}
	// Keep one empty block per kind so steady churn does not reallocate it.
	const bool other_empty = std::any_of(pool.blocks.begin(), pool.blocks.end(),
	                                     [block](const auto& other)
	                                     {
		                                     return other.get() != block && other->used == 0 && other->device == block->device &&
		                                            other->type == block->type && other->resource == block->resource;
	                                     });
	if (!other_empty)
	{
		return;
	}
	if (block->mapped != nullptr)
	{
		vkUnmapMemory(ctx->device, block->memory);
	}
	vkFreeMemory(ctx->device, block->memory, nullptr);
	pool.blocks.erase(std::find_if(pool.blocks.begin(), pool.blocks.end(), [block](const auto& other) { return other.get() == block; }));
}

} // namespace

void VulkanMemoryPoolRelease(GraphicContext* ctx)
{
	EXIT_IF(ctx == nullptr);
	auto&           pool = GetMemoryPool();
	std::lock_guard lock(pool.mutex);
	for (auto it = pool.blocks.begin(); it != pool.blocks.end();)
	{
		if ((*it)->device != ctx->device)
		{
			++it;
			continue;
		}
		if ((*it)->mapped != nullptr)
		{
			vkUnmapMemory(ctx->device, (*it)->memory);
		}
		vkFreeMemory(ctx->device, (*it)->memory, nullptr);
		it = pool.blocks.erase(it);
	}
}

bool VulkanAllocate(GraphicContext* ctx, VulkanMemory* mem, VulkanMemoryResource resource)
{
	KYTY_PROFILER_FUNCTION();

	static std::atomic_uint64_t seq = 0;

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(mem->memory != nullptr);
	if (mem->requirements.size == 0)
	{
		mem->requirements.size = 4096;
	}

	VkPhysicalDeviceMemoryProperties memory_properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &memory_properties);

	uint32_t index = 0;
	for (; index < memory_properties.memoryTypeCount; index++)
	{
		if ((mem->requirements.memoryTypeBits & (static_cast<uint32_t>(1) << index)) != 0 &&
		    (memory_properties.memoryTypes[index].propertyFlags & mem->property) == mem->property)
		{
			break;
		}
	}

	mem->type       = index;
	mem->offset     = 0;
	mem->pool_block = nullptr;

	if (index < memory_properties.memoryTypeCount &&
	    PoolAllocate(ctx, mem, index, memory_properties.memoryTypes[index].propertyFlags, resource))
	{
		mem->unique_id = ++seq;
		g_mem_stat->allocated[index] += mem->requirements.size;
		g_mem_stat->count[index]++;
		return true;
	}

	VkMemoryAllocateInfo alloc_info {};
	alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext           = nullptr;
	alloc_info.allocationSize  = mem->requirements.size;
	alloc_info.memoryTypeIndex = index;

	mem->unique_id = ++seq;

	const auto allocate_start = std::chrono::steady_clock::now();
	auto       result         = vkAllocateMemory(ctx->device, &alloc_info, nullptr, &mem->memory);
	const auto allocate_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - allocate_start).count();
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::VulkanAllocate, static_cast<uint64_t>(allocate_ns),
	                                                mem->requirements.size);

	if (result == VK_SUCCESS)
	{
		g_mem_stat->allocated[index] += mem->requirements.size;
		g_mem_stat->count[index]++;
		return true;
	}

	Core::StringList stat;
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
	{
		uint64_t allocated = g_mem_stat->allocated[i];
		uint64_t count     = g_mem_stat->count[i];
		stat.Add(String::FromPrintf("%u, %" PRIu64 ", %" PRIu64 "", i, count, allocated));
	}
	g_gpu_memory->DbgDbDump();
	g_gpu_memory->DbgDbSave(U"_gpu_memory.db");
	EXIT("size = %" PRIu64 ", index = %u, VkResult=%d:%s\n", mem->requirements.size, index, static_cast<int>(result),
	     stat.Concat(U'\n').C_Str());

	return false;
}

void VulkanFree(GraphicContext* ctx, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);

	if (mem->pool_block != nullptr)
	{
		PoolFree(ctx, mem);
		mem->pool_block = nullptr;
	} else
	{
		vkFreeMemory(ctx->device, mem->memory, nullptr);
	}

	g_mem_stat->allocated[mem->type] -= mem->requirements.size;
	g_mem_stat->count[mem->type]--;

	mem->memory = nullptr;
}

void VulkanMapMemory(GraphicContext* ctx, VulkanMemory* mem, void** data)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(data == nullptr);

	if (mem->pool_block != nullptr)
	{
		auto* block = static_cast<const PoolBlock*>(mem->pool_block);
		EXIT_IF(block->mapped == nullptr);
		*data = block->mapped + mem->offset;
		return;
	}
	vkMapMemory(ctx->device, mem->memory, mem->offset, mem->requirements.size, 0, data);
}

void VulkanUnmapMemory(GraphicContext* ctx, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);

	// Pooled host-visible blocks stay persistently mapped.
	if (mem->pool_block == nullptr)
	{
		vkUnmapMemory(ctx->device, mem->memory);
	}
}

void VulkanBindImageMemory(GraphicContext* ctx, VulkanImage* image, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(image == nullptr);

	const auto bind_start = std::chrono::steady_clock::now();
	vkBindImageMemory(ctx->device, image->image, mem->memory, mem->offset);
	const auto bind_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - bind_start).count();
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::VulkanBind, static_cast<uint64_t>(bind_ns));
}

void VulkanBindBufferMemory(GraphicContext* ctx, VulkanBuffer* buffer, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(buffer == nullptr);

	const auto bind_start = std::chrono::steady_clock::now();
	vkBindBufferMemory(ctx->device, buffer->buffer, mem->memory, mem->offset);
	const auto bind_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - bind_start).count();
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::VulkanBind, static_cast<uint64_t>(bind_ns));
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
