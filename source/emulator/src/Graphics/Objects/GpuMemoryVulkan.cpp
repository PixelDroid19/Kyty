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
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
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
	VkDevice                                  device = nullptr;
	VkDeviceMemory                            memory = nullptr;
	uint8_t*                                  mapped = nullptr;
	uint32_t                                  type   = 0;
	VulkanMemoryResource                      resource {};
	VkDeviceSize                              used   = 0;
	std::map<VkDeviceSize, VkDeviceSize>      free_ranges; // offset -> size, coalesced
	std::set<std::pair<VkDeviceSize, VkDeviceSize>> free_sizes; // (size, offset), same ranges
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

void PoolAddFree(PoolBlock* block, VkDeviceSize offset, VkDeviceSize size)
{
	block->free_ranges.emplace(offset, size);
	block->free_sizes.emplace(size, offset);
}

void PoolRemoveFree(PoolBlock* block, std::map<VkDeviceSize, VkDeviceSize>::iterator range)
{
	block->free_sizes.erase({range->second, range->first});
	block->free_ranges.erase(range);
}

VkDeviceSize PoolLargestFree(const PoolBlock& block)
{
	return block.free_sizes.empty() ? 0 : block.free_sizes.rbegin()->first;
}

// Best fit through the size index: a range of at least size + alignment - 1
// bytes always fits; a few smaller ones may fit when their start is aligned.
bool PoolTakeRange(PoolBlock* block, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize* offset)
{
	auto pick = block->free_sizes.end();
	int  tries = 0;
	for (auto it = block->free_sizes.lower_bound({size, 0}); it != block->free_sizes.end() && tries < 8; ++it, ++tries)
	{
		const VkDeviceSize aligned = (it->second + alignment - 1) / alignment * alignment;
		if (aligned + size <= it->second + it->first)
		{
			pick = it;
			break;
		}
	}
	if (pick == block->free_sizes.end())
	{
		pick = block->free_sizes.lower_bound({size + alignment - 1, 0});
		if (pick == block->free_sizes.end())
		{
			return false;
		}
	}
	const VkDeviceSize start   = pick->second;
	const VkDeviceSize length  = pick->first;
	const VkDeviceSize aligned = (start + alignment - 1) / alignment * alignment;
	PoolRemoveFree(block, block->free_ranges.find(start));
	if (aligned > start)
	{
		PoolAddFree(block, start, aligned - start);
	}
	if (aligned + size < start + length)
	{
		PoolAddFree(block, aligned + size, start + length - aligned - size);
	}
	block->used += size;
	*offset = aligned;
	return true;
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
			PoolRemoveFree(block, previous);
		}
	}
	if (next != block->free_ranges.end() && offset + size == next->first)
	{
		size += next->second;
		PoolRemoveFree(block, next);
	}
	PoolAddFree(block, offset, size);
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
		    PoolLargestFree(*block) >= mem->requirements.size &&
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
	PoolAddFree(block.get(), 0, kPoolBlockBytes);
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

// GDS sources: texture-type objects keep device content in an image layout, so their
// bytes have no established raw form for a GDS copy.
static bool GdsSourceIsTextureType(GpuMemoryObjectType type)
{
	return type == GpuMemoryObjectType::Texture || type == GpuMemoryObjectType::RenderTexture ||
	       type == GpuMemoryObjectType::StorageTexture || type == GpuMemoryObjectType::DepthStencilBuffer ||
	       type == GpuMemoryObjectType::VideoOutBuffer;
}

GpuMemoryGdsSource GpuMemory::AcquireGdsSource(GraphicContext* ctx, CommandBuffer* buffer, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(ctx == nullptr || buffer == nullptr);
	GpuMemoryGdsSource source;
	SubmissionId       recording;
	if (size == 0 || vaddr > UINT64_MAX - size || !buffer->GetSubmissionId(&recording))
	{
		return source;
	}

	Core::LockGuard    backing_lock(m_backing_mutation_mutex);
	Core::LockGuard    lock(m_mutex);
	Vector<Destructor> destructors;
	source.status     = GpuMemoryGdsSourceStatus::GuestBytesCurrent;
	const int heap_id = GetHeapId(vaddr, size);
	int       device_object  = -1;
	int       pending_owners = 0;
	bool      decided        = false;
	if (heap_id >= 0)
	{
		for (const auto& found: FindBlocks(heap_id, &vaddr, &size, 1))
		{
			auto& object = m_heaps[heap_id].objects[found.object_id];
			// Objects without unwritten GPU writes hold guest bytes or a copy of them.
			if (object.free || m_pending_write_back.count({heap_id, found.object_id}) == 0)
			{
				continue;
			}
			const auto type = object.info.object.type;
			if (GdsSourceIsTextureType(type))
			{
				source.status = GpuMemoryGdsSourceStatus::Unsupported;
				source.writer = type;
				decided       = true;
				break;
			}
			bool recording_queue_owner = false;
			for (const auto& use: object.info.submission_uses.Dependencies())
			{
				if (m_deferred_deletions.AreDependenciesComplete({use}))
				{
					continue;
				}
				if (!(use.queue == recording.queue))
				{
					source.status     = GpuMemoryGdsSourceStatus::SubmissionCompletionRequired;
					source.dependency = use;
					source.writer     = type;
					decided           = true;
					break;
				}
				recording_queue_owner = true;
			}
			if (decided)
			{
				break;
			}
			if (!recording_queue_owner)
			{
				// Every writer has completed: publish its content before the guest bytes are read.
				WriteBackObjectLocked(ctx, heap_id, found.object_id, &destructors);
				continue;
			}
			pending_owners++;
			source.writer = type;
			if (type == GpuMemoryObjectType::StorageBuffer && object.block.vaddr_num == 1 && !object.info.read_only &&
			    object.info.object.obj != nullptr && object.block.vaddr[0] <= vaddr &&
			    vaddr + size <= object.block.vaddr[0] + object.block.size[0])
			{
				device_object = found.object_id;
			}
		}
	}
	if (!decided && pending_owners == 1 && device_object >= 0)
	{
		auto&       object  = m_heaps[heap_id].objects[device_object];
		const auto* storage = static_cast<const StorageVulkanBuffer*>(object.info.object.obj);
		if (storage->buffer != nullptr && storage->guest_addr == object.block.vaddr[0])
		{
			// The recorded use keeps this backing alive until the copy's submission completes.
			RecordUse(&object.info, recording);
			object.info.use_last_frame = GpuMemoryAliasLookupUseFrame(m_current_frame, object.info.use_last_frame);
			source.status              = GpuMemoryGdsSourceStatus::DeviceBuffer;
			source.buffer              = storage;
			source.offset              = vaddr - storage->guest_addr;
			decided                    = true;
		}
	}
	if (!decided && pending_owners > 0)
	{
		source.status = GpuMemoryGdsSourceStatus::ProcessorWriteBackRequired;
	}
	ScheduleDestructorsOutsideMutationLocks(ctx, &destructors);
	return source;
}

void GpuMemory::DeferUntilSubmissionComplete(SubmissionId submission, std::function<void()> task)
{
	if (m_deferred_deletions.Enqueue({submission}, std::move(task)) != GpuDeferredDeletionResult::Success)
	{
		EXIT("deferred GDS release rejected: queue=%u sequence=%" PRIu64 "\n", submission.queue.Value(), submission.sequence);
	}
}

GpuMemoryGdsSource GpuMemoryAcquireGdsSource(GraphicContext* ctx, CommandBuffer* buffer, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_gpu_memory == nullptr);
	return g_gpu_memory->AcquireGdsSource(ctx, buffer, vaddr, size);
}

void GpuMemoryDeferUntilSubmissionComplete(SubmissionId submission, std::function<void()> task)
{
	EXIT_IF(g_gpu_memory == nullptr);
	g_gpu_memory->DeferUntilSubmissionComplete(submission, std::move(task));
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
