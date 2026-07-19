#include "Emulator/Graphics/Objects/IndexBuffer.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/ExactStagingPool.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class IndexBufferManager
{
public:
	IndexBufferManager() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~IndexBufferManager() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(IndexBufferManager);

	void RegisterForDelete(VulkanBuffer* buf)
	{
		Core::LockGuard lock(m_mutex);

		m_buffers.Add(buf);
	}

	ExactStagingLease AcquireStaging(GraphicContext* ctx, uint64_t size)
	{
		Core::LockGuard lock(m_mutex);

		if (m_staging_pool == nullptr)
		{
			m_staging_ctx  = ctx;
			m_staging_pool = new ExactStagingPool(kStagingPoolSlots, {ctx, CreateStaging, DeleteStaging});
		}
		EXIT_IF(m_staging_ctx != ctx);

		return m_staging_pool->Acquire(size);
	}

	bool ReleaseStaging(const ExactStagingLease& lease)
	{
		Core::LockGuard lock(m_mutex);

		EXIT_IF(m_staging_pool == nullptr);
		return m_staging_pool->Release(lease);
	}

	void DeleteAll(GraphicContext* ctx)
	{
		KYTY_PROFILER_BLOCK("IndexBufferManager::DeleteAll");

		Core::LockGuard lock(m_mutex);

		if (m_staging_pool != nullptr)
		{
			EXIT_IF(m_staging_ctx != ctx);
			EXIT_IF(!m_staging_pool->DeleteAll());
			delete m_staging_pool;
			m_staging_pool = nullptr;
			m_staging_ctx  = nullptr;
		}

		for (auto* vk_obj: m_buffers)
		{
			EXIT_IF(vk_obj == nullptr);
			EXIT_IF(vk_obj->buffer == nullptr);
			EXIT_IF(ctx == nullptr);

			VulkanDeleteBuffer(ctx, vk_obj);

			delete vk_obj;
		}

		m_buffers.Clear();
	}

private:
	static constexpr uint32_t kStagingPoolSlots = 8;

	static ExactStagingResource CreateStaging(void* user, uint64_t size)
	{
		auto* ctx = static_cast<GraphicContext*>(user);

		auto* buffer            = new VulkanBuffer;
		buffer->usage           = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		buffer->memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VulkanCreateBuffer(ctx, size, buffer);
		EXIT_NOT_IMPLEMENTED(buffer->buffer == nullptr);

		void* mapped = nullptr;
		VulkanMapMemory(ctx, &buffer->memory, &mapped);
		EXIT_NOT_IMPLEMENTED(mapped == nullptr);
		return {buffer, mapped};
	}

	static void DeleteStaging(void* user, const ExactStagingResource& resource)
	{
		auto* ctx    = static_cast<GraphicContext*>(user);
		auto* buffer = static_cast<VulkanBuffer*>(resource.object);

		EXIT_IF(ctx == nullptr);
		EXIT_IF(buffer == nullptr);
		EXIT_IF(resource.mapped == nullptr);

		VulkanUnmapMemory(ctx, &buffer->memory);
		VulkanDeleteBuffer(ctx, buffer);
		delete buffer;
	}

	Core::Mutex m_mutex;

	Vector<VulkanBuffer*> m_buffers;
	ExactStagingPool*     m_staging_pool = nullptr;
	GraphicContext*       m_staging_ctx  = nullptr;
};

static IndexBufferManager* g_index_buffer_manager = nullptr;

void IndexBufferInit()
{
	EXIT_IF(g_index_buffer_manager != nullptr);

	g_index_buffer_manager = new IndexBufferManager;
}

void IndexBufferDeleteAll(GraphicContext* ctx)
{
	EXIT_IF(g_index_buffer_manager == nullptr);

	g_index_buffer_manager->DeleteAll(ctx);
}

// Device-local index buffers mirror VertexBuffer: create allocates GPU memory,
// update re-uploads guest CPU bytes through a host-visible staging buffer.
// GpuMemory calls update when a registered index range is dirtied after create.
static void update_func(GraphicContext* ctx, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr, const uint64_t* size,
                        int vaddr_num)
{
	KYTY_PROFILER_BLOCK("IndexBufferGpuObject::update_func");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0);
	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);

	auto* vk_obj = static_cast<VulkanBuffer*>(obj);

	EXIT_IF(g_index_buffer_manager == nullptr);

	const auto staging = g_index_buffer_manager->AcquireStaging(ctx, *size);
	EXIT_NOT_IMPLEMENTED(!staging.IsValid());
	memcpy(staging.resource.mapped, reinterpret_cast<void*>(*vaddr), *size);

	UtilCopyBuffer(static_cast<VulkanBuffer*>(staging.resource.object), vk_obj, *size);
	EXIT_IF(!g_index_buffer_manager->ReleaseStaging(staging));
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("IndexBufferGpuObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr || *vaddr == 0);

	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto* vk_obj = new VulkanBuffer;

	vk_obj->usage           = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	vk_obj->memory.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	vk_obj->buffer          = nullptr;

	VulkanCreateBuffer(ctx, *size, vk_obj);
	EXIT_NOT_IMPLEMENTED(vk_obj->buffer == nullptr);

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	return vk_obj;
}

static void delete_func(GraphicContext* /*ctx*/, void* obj, VulkanMemory* /*mem*/)
{
	KYTY_PROFILER_BLOCK("IndexBufferGpuObject::delete_func");

	EXIT_IF(g_index_buffer_manager == nullptr);

	auto* vk_obj = reinterpret_cast<VulkanBuffer*>(obj);

	g_index_buffer_manager->RegisterForDelete(vk_obj);
}

bool IndexBufferGpuObject::Equal(const uint64_t* /*other*/) const
{
	return true;
}

GpuObject::create_func_t IndexBufferGpuObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::delete_func_t IndexBufferGpuObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t IndexBufferGpuObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
