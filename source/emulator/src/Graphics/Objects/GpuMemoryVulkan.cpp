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
#include <vulkan/vk_enum_string_helper.h>

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

bool VulkanAllocate(GraphicContext* ctx, VulkanMemory* mem)
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

	mem->type   = index;
	mem->offset = 0;

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
	EXIT("size = %" PRIu64 ", index = %u, error: %s:%s\n", mem->requirements.size, index, string_VkResult(result),
	     stat.Concat(U'\n').C_Str());

	return false;
}

void VulkanFree(GraphicContext* ctx, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);

	vkFreeMemory(ctx->device, mem->memory, nullptr);

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

	vkMapMemory(ctx->device, mem->memory, mem->offset, mem->requirements.size, 0, data);
}

void VulkanUnmapMemory(GraphicContext* ctx, VulkanMemory* mem)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(mem == nullptr);

	vkUnmapMemory(ctx->device, mem->memory);
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
