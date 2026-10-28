#include "Emulator/Graphics/Objects/StorageBuffer.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static void update_func(GraphicContext* ctx, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr, const uint64_t* size,
                        int vaddr_num)
{
	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::update_func");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(obj == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = reinterpret_cast<StorageVulkanBuffer*>(obj);

	const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, *size);
	void*                      data = nullptr;
	// vkMapMemory(ctx->device, vk_obj->memory.memory, vk_obj->memory.offset, *size, 0, &data);
	VulkanMapMemory(ctx, &vk_obj->memory, &data);
	LabelStorageUpload(data, reinterpret_cast<void*>(*vaddr), *size, {{0, *size}}, &vk_obj->writeback_cache, &vk_obj->label_publication);
	// HTILE clears often arrive through GpuMemory Update before the world draw.
	(void)DepthMetaObserveStorageWrite(vk_obj->depth_meta_addr, data, *size);
	// vkUnmapMemory(ctx->device, vk_obj->memory.memory);
	VulkanUnmapMemory(ctx, &vk_obj->memory);
}

bool StorageBufferUploadRuns(GraphicContext* ctx, void* obj, uint64_t vaddr, uint64_t size, const std::vector<GpuByteRun>& runs)
{
	EXIT_IF(ctx == nullptr || obj == nullptr || runs.empty());
	auto* vk_obj = reinterpret_cast<StorageVulkanBuffer*>(obj);
	if (vk_obj->depth_meta_addr != 0)
	{
		return false;
	}
	uint64_t bytes = 0;
	for (const auto& run: runs)
	{
		bytes += run.bytes;
	}
	const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, bytes);
	void*                      data = nullptr;
	VulkanMapMemory(ctx, &vk_obj->memory, &data);
	LabelStorageUpload(data, reinterpret_cast<void*>(vaddr), size, runs, &vk_obj->writeback_cache, &vk_obj->label_publication);
	VulkanUnmapMemory(ctx, &vk_obj->memory);
	return true;
}

// Shader writes flag their pages here (ShaderSpirvStorageWriteMask.h); the
// mask stays mapped for the write-back that consumes it.
static void create_write_mask(GraphicContext* ctx, StorageVulkanBuffer* vk_obj)
{
	const uint64_t pages  = (vk_obj->guest_size + GpuWritebackPageCache::kDefaultPageSize - 1u) / GpuWritebackPageCache::kDefaultPageSize;
	auto*          mask   = new VulkanBuffer;
	mask->usage           = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	mask->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	VulkanCreateBuffer(ctx, pages * sizeof(uint32_t), mask);
	void* flags = nullptr;
	VulkanMapMemory(ctx, &mask->memory, &flags);
	std::memset(flags, 0, pages * sizeof(uint32_t));
	vk_obj->write_mask = mask;
	vk_obj->writeback_cache.TrackWrittenPages(static_cast<uint32_t*>(flags));
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0);

	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto* vk_obj = new StorageVulkanBuffer;
	vk_obj->guest_addr = *vaddr;
	vk_obj->guest_size = *size;

	// Transfers: a proven uniform fill is written by a fill, and copied into the
	// guest device-address view. Vertex streams the GPU wrote are fetched from it.
	vk_obj->usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
	                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	vk_obj->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	vk_obj->buffer = nullptr;

	VulkanCreateBuffer(ctx, *size, vk_obj);
	if (vk_obj->buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vk_obj->buffer == nullptr condition ignored (continuing)\n"); }
	create_write_mask(ctx, vk_obj);

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* /*mem*/)
{
	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::delete_func");

	auto* vk_obj = reinterpret_cast<StorageVulkanBuffer*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(vk_obj->buffer == nullptr || vk_obj->write_mask == nullptr);
	EXIT_IF(ctx == nullptr);

	VulkanDeleteBuffer(ctx, vk_obj);
	VulkanUnmapMemory(ctx, &vk_obj->write_mask->memory);
	VulkanDeleteBuffer(ctx, vk_obj->write_mask);

	delete vk_obj->write_mask;
	delete vk_obj;
}

static GpuWritebackResult write_back(GraphicContext* ctx, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr,
                                     const uint64_t* size, int vaddr_num)
{
	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::write_back");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(obj == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = reinterpret_cast<StorageVulkanBuffer*>(obj);

	void* data = nullptr;

	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::write_back::vkMapMemory");
	// vkMapMemory(ctx->device, vk_obj->memory.memory, vk_obj->memory.offset, *size, 0, &data);
	VulkanMapMemory(ctx, &vk_obj->memory, &data);
	KYTY_PROFILER_END_BLOCK;

	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::write_back::memcpy");
	const auto result =
	    LabelWriteBackCopy(reinterpret_cast<void*>(*vaddr), data, *size, &vk_obj->writeback_cache, &vk_obj->label_publication);
	if (vk_obj->depth_meta_addr != 0 && DepthMetaIsClearPattern(data, *size))
	{
		DepthMetaPatternSnapshot pattern {};
		if (DepthMetaInspectPattern(data, *size, &pattern))
		{
			DepthMetaMarkClear(vk_obj->depth_meta_addr, DepthMetaClearSource::StorageWriteback, &pattern, *size);
		}
	}
	KYTY_PROFILER_END_BLOCK;

	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::write_back::vkUnmapMemory");
	// vkUnmapMemory(ctx->device, vk_obj->memory.memory);
	VulkanUnmapMemory(ctx, &vk_obj->memory);
	KYTY_PROFILER_END_BLOCK;
	return result;
}

bool StorageBufferWriteBackPublishedUniform(void* obj, uint64_t vaddr, uint64_t size, const GpuWritebackPageCache::UniformWords& words,
                                            GpuWritebackResult* result)
{
	auto* vk_obj = static_cast<StorageVulkanBuffer*>(obj);
	EXIT_IF(vk_obj == nullptr || result == nullptr);
	// Depth metadata clears are recognized from the written bytes.
	if (vk_obj->depth_meta_addr != 0 || vk_obj->guest_addr != vaddr || vk_obj->guest_size != size)
	{
		return false;
	}
	return LabelWriteBackAdoptUniform(vaddr, size, words, &vk_obj->writeback_cache, &vk_obj->label_publication, result);
}

bool StorageBufferWriteBackPublishedDevice(GraphicContext* ctx, void* obj, uint64_t vaddr, uint64_t size, GpuWritebackResult* result)
{
	auto* vk_obj = static_cast<StorageVulkanBuffer*>(obj);
	EXIT_IF(ctx == nullptr || vk_obj == nullptr || result == nullptr);
	// Depth metadata clears are recognized from the written bytes.
	if (vk_obj->depth_meta_addr != 0 || vk_obj->guest_addr != vaddr || vk_obj->guest_size != size)
	{
		return false;
	}
	void* data = nullptr;
	VulkanMapMemory(ctx, &vk_obj->memory, &data);
	const bool adopted = LabelWriteBackAdoptDevice(vaddr, data, size, &vk_obj->writeback_cache, &vk_obj->label_publication, result);
	VulkanUnmapMemory(ctx, &vk_obj->memory);
	return adopted;
}

bool StorageBufferGpuObject::Equal(const uint64_t* other) const
{
	// GpuMemory calls Equal only after type and guest byte ranges match.
	// Stride and record count describe the shader view; they do not affect the
	// VkBuffer backing created above.
	return other != nullptr;
}

GpuObject::create_func_t StorageBufferGpuObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::write_back_func_t StorageBufferGpuObject::GetWriteBackFunc() const
{
	return write_back;
}

GpuObject::delete_func_t StorageBufferGpuObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t StorageBufferGpuObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
