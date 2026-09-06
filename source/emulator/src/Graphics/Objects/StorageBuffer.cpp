#include "Emulator/Graphics/Objects/StorageBuffer.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static void update_func(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size,
                        int vaddr_num)
{
	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::update_func");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(obj == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = reinterpret_cast<StorageVulkanBuffer*>(obj);

	const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, *size);
	EXIT_IF(vk_obj->mapped == nullptr);
	memcpy(vk_obj->mapped, reinterpret_cast<void*>(*vaddr), *size);
	// Only proven non-writing uses may omit the baseline. Unknown access is
	// writable; after promotion retain the snapshot for this backing's lifetime.
	if (params[StorageBufferGpuObject::PARAM_INITIAL_READ_ONLY] == 0u || vk_obj->writeback_cache.IsInitialized())
	{
		vk_obj->writeback_cache.Reset(vk_obj->mapped, *size);
	}
	// HTILE clears often arrive through GpuMemory Update before the world draw.
	(void)DepthMetaObserveStorageWrite(vk_obj->depth_meta_addr, vk_obj->mapped, *size);
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

	vk_obj->usage           = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	vk_obj->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	vk_obj->buffer = nullptr;

	VulkanCreateBuffer(ctx, *size, vk_obj);
	if (vk_obj->buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vk_obj->buffer == nullptr condition ignored (continuing)\n"); }
	VulkanMapMemory(ctx, &vk_obj->memory, &vk_obj->mapped);
	EXIT_IF(vk_obj->mapped == nullptr);

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	return vk_obj;
}

void StorageBufferPrepareWriteback(void* object, GpuObject::create_func_t factory)
{
	if (factory != create_func)
	{
		return;
	}
	auto* storage = static_cast<StorageVulkanBuffer*>(object);
	EXIT_IF(storage == nullptr || storage->mapped == nullptr || storage->guest_size == 0u);
	if (!storage->writeback_cache.IsInitialized())
	{
		storage->writeback_cache.Reset(storage->mapped, storage->guest_size);
	}
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* /*mem*/)
{
	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::delete_func");

	auto* vk_obj = reinterpret_cast<StorageVulkanBuffer*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(vk_obj->buffer == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(vk_obj->mapped == nullptr);

	VulkanUnmapMemory(ctx, &vk_obj->memory);
	vk_obj->mapped = nullptr;
	VulkanDeleteBuffer(ctx, vk_obj);

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

	void* data = vk_obj->mapped;
	EXIT_IF(data == nullptr);

	KYTY_PROFILER_BLOCK("StorageBufferGpuObject::write_back::memcpy");
	const auto result =
	    LabelWriteBackCopy(reinterpret_cast<void*>(*vaddr), data, *size, &vk_obj->writeback_cache);
	if (vk_obj->depth_meta_addr != 0 && DepthMetaIsClearPattern(data, *size))
	{
		DepthMetaPatternSnapshot pattern {};
		if (DepthMetaInspectPattern(data, *size, &pattern))
		{
			DepthMetaMarkClear(vk_obj->depth_meta_addr, DepthMetaClearSource::StorageWriteback, &pattern, *size);
		}
	}
	KYTY_PROFILER_END_BLOCK;

	return result;
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
