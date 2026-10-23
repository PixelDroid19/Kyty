#include "Emulator/Graphics/Objects/VertexBuffer.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Vertex buffer objects live in the same host-visible coherent memory as the
// transient vertex snapshots, so guest bytes are written through the mapping.
// Coherent host writes are visible to every later submission, which is the
// ordering the former one-shot staging copy and fence wait provided.
static void update_func(GraphicContext* ctx, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr, const uint64_t* size,
                        int vaddr_num)
{
	KYTY_PROFILER_BLOCK("VertexBufferGpuObject::update_func");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0);
	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);

	auto* vk_obj = static_cast<VulkanBuffer*>(obj);

	const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, *size);
	void*                      data = nullptr;
	VulkanMapMemory(ctx, &vk_obj->memory, &data);
	EXIT_IF(data == nullptr);
	memcpy(data, reinterpret_cast<void*>(*vaddr), *size);
	VulkanUnmapMemory(ctx, &vk_obj->memory);
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("VertexBufferGpuObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto* vk_obj = new VulkanBuffer;

	vk_obj->usage           = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	vk_obj->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	vk_obj->buffer          = nullptr;

	VulkanCreateBuffer(ctx, *size, vk_obj);
	if (vk_obj->buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vk_obj->buffer == nullptr condition ignored (continuing)\n"); }

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* /*mem*/)
{
	KYTY_PROFILER_BLOCK("VertexBufferGpuObject::delete_func");

	auto* vk_obj = reinterpret_cast<VulkanBuffer*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(vk_obj->buffer == nullptr);
	EXIT_IF(ctx == nullptr);

	VulkanDeleteBuffer(ctx, vk_obj);

	delete vk_obj;
}

bool VertexBufferGpuObject::Equal(const uint64_t* /*other*/) const
{
	return true;
}

GpuObject::create_func_t VertexBufferGpuObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::delete_func_t VertexBufferGpuObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t VertexBufferGpuObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED