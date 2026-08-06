#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/CommandProcessorSubmissionSlots.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/PipelineCacheStore.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/TimePort.h"
#include "Emulator/Log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// GraphicsRenderInit/CreateContext, RenderContext EOP, GdsBuffer, CommandPool

void GraphicsRenderInit()
{
	EXIT_IF(g_render_ctx != nullptr);

	g_render_ctx = new RenderContext;
}

void GraphicsRenderCreateContext()
{
	EXIT_IF(g_render_ctx == nullptr);

	auto* ctx = WindowGetGraphicContext();
	g_render_ctx->SetGraphicCtx(ctx);

	if (ctx != nullptr && ctx->device != nullptr && ctx->pipeline_cache == nullptr)
	{
		VkPhysicalDeviceProperties properties {};
		vkGetPhysicalDeviceProperties(ctx->physical_device, &properties);
		auto initial_data = PipelineCacheStoreLoad(properties);

		VkPipelineCacheCreateInfo cache_info {};
		cache_info.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
		cache_info.pNext           = nullptr;
		cache_info.flags           = 0;
		cache_info.initialDataSize = initial_data.size();
		cache_info.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();

		auto result = vkCreatePipelineCache(ctx->device, &cache_info, nullptr, &ctx->pipeline_cache);
		if (result != VK_SUCCESS && !initial_data.empty())
		{
			cache_info.initialDataSize = 0;
			cache_info.pInitialData    = nullptr;
			result                     = vkCreatePipelineCache(ctx->device, &cache_info, nullptr, &ctx->pipeline_cache);
		}

		if (result != VK_SUCCESS || ctx->pipeline_cache == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: result != VK_SUCCESS || ctx->pipeline_cache == nullptr condition ignored (continuing)\n"); }

		if (!initial_data.empty() && Config::GetPrintfDirection() != Log::Direction::Silent)
		{
			KYTY_LOG_DEBUG("Loaded Vulkan pipeline cache: %zu bytes\n", initial_data.size());
		}
	}
}

static bool EopTraceEnabled()
{
	static const bool enabled = (std::getenv("KYTY_EOP_TRACE") != nullptr);
	return enabled;
}

void* RenderContext::BeginEopEqRegistration(Kernel::EventQueue::KernelEqueueIdentity identity, int id)
{
	Core::LockGuard lock(m_eop_mutex);

	auto* registration     = new EopEqRegistration;
	registration->identity = identity;
	registration->id       = id;
	m_eop_eqs.Add(registration);

	if (EopTraceEnabled())
	{
		KYTY_LOG_DEBUG( "EOP_BEGIN eq=%p id=0x%x count=%u\n", static_cast<void*>(identity.eq), id,
		             static_cast<unsigned>(m_eop_eqs.Size()));
	}
	return registration;
}

void RenderContext::PublishEopEqRegistration(void* registration_ptr)
{
	auto*              registration = static_cast<EopEqRegistration*>(registration_ptr);
	EopEqRegistration* release      = nullptr;
	Core::LockGuard    lock(m_eop_mutex);
	const auto         index = m_eop_eqs.Find(registration);
	EXIT_IF(!m_eop_eqs.IndexValid(index) || registration->published);
	if (registration->deleted)
	{
		m_eop_eqs.RemoveAt(index);
		release = registration;
	} else
	{
		registration->published = true;
	}

	if (EopTraceEnabled())
	{
		KYTY_LOG_DEBUG( "EOP_PUBLISH eq=%p id=0x%x deleted=%d\n", static_cast<void*>(registration->identity.eq), registration->id,
		             registration->deleted ? 1 : 0);
	}
	delete release;
}

void RenderContext::CancelEopEqRegistration(void* registration_ptr)
{
	auto*           registration = static_cast<EopEqRegistration*>(registration_ptr);
	Core::LockGuard lock(m_eop_mutex);
	const auto      index = m_eop_eqs.Find(registration);
	EXIT_IF(!m_eop_eqs.IndexValid(index) || registration->published);
	m_eop_eqs.RemoveAt(index);
	delete registration;
}

void RenderContext::DeleteEopEqRegistration(void* registration_ptr, Kernel::EventQueue::KernelEqueue eq, int id)
{
	auto*              registration = static_cast<EopEqRegistration*>(registration_ptr);
	EopEqRegistration* release      = nullptr;
	{
		Core::LockGuard lock(m_eop_mutex);
		const auto      index = m_eop_eqs.Find(registration);
		EXIT_IF(!m_eop_eqs.IndexValid(index));
		EXIT_IF(registration->identity.eq != eq || registration->id != id || registration->deleted);
		registration->deleted = true;
		if (registration->published)
		{
			m_eop_eqs.RemoveAt(index);
			release = registration;
		}
	}

	if (EopTraceEnabled())
	{
		KYTY_LOG_DEBUG( "EOP_DEL eq=%p id=0x%x\n", static_cast<void*>(eq), id);
	}
	delete release;
}

void RenderContext::TriggerEopEvent()
{
	TriggerRegisteredEvents(CompletionSignal::EndOfPipe);
}

void RenderContext::TriggerQueuedGraphicsInterrupt()
{
	TriggerRegisteredEvents(CompletionSignal::QueuedGraphicsInterrupt);
}

void RenderContext::TriggerRegisteredEvents(CompletionSignal signal)
{
	struct PendingTrigger
	{
		Kernel::EventQueue::KernelEqueuePin pin;
		int                                    id = GRAPHICS_EVENT_EOP;
	};
	std::vector<PendingTrigger> triggers;
	{
		Core::LockGuard lock(m_eop_mutex);
		triggers.reserve(m_eop_eqs.Size());
		for (auto* entry: m_eop_eqs)
		{
			const bool selected = (signal == CompletionSignal::EndOfPipe ? IsGraphicsEopEventId(entry->id)
			                                                                    : entry->id == GRAPHICS_EVENT_QUEUED_GRAPHICS_INTERRUPT);
			if (selected && entry->published && !entry->deleted)
			{
				auto pin = Kernel::EventQueue::KernelAcquireEqueue(entry->identity);
				if (pin)
				{
					triggers.push_back(PendingTrigger {std::move(pin), entry->id});
				}
			}
		}
	}

	for (auto& trigger: triggers)
	{
		void* trigger_data = nullptr;
		if (signal == CompletionSignal::EndOfPipe)
		{
		trigger_data = reinterpret_cast<void*>(Kernel::TimePort::GetCounter());
		}
		const auto result = Kernel::EventQueue::KernelTriggerEvent(trigger.pin, static_cast<uintptr_t>(trigger.id),
		                                                              Kernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		                                                              trigger_data);
		if (result != Kernel::OK && result != Kernel::KERNEL_ERROR_ENOENT) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: result != Kernel::OK && result != Kernel::KERNEL_ERROR_ENOENT condition ignored (continuing)\n"); }
	}

	if (EopTraceEnabled())
	{
		static std::atomic<uint32_t> trigger_logs {0};
		const uint32_t               n = trigger_logs.fetch_add(1);
		if (n < 64u)
		{
			const char* signal_name = (signal == CompletionSignal::EndOfPipe ? "EOP_TRIGGER" : "QUEUED_GRAPHICS_INTERRUPT");
			KYTY_LOG_DEBUG( "%s live=%u\n", signal_name, static_cast<unsigned>(triggers.size()));
		}
	}
}

void GdsBuffer::Init(GraphicContext* ctx)
{
	if (m_buffer == nullptr)
	{
		m_buffer = new VulkanBuffer;

		m_buffer->usage           = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		m_buffer->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		m_buffer->buffer          = nullptr;

		VulkanCreateBuffer(ctx, DW_SIZE * 4, m_buffer);
		if (m_buffer->buffer == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_buffer->buffer == nullptr condition ignored (continuing)\n"); }
	}
}

void GdsBuffer::Clear(GraphicContext* ctx, uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value)
{
	EXIT_IF(ctx == nullptr);

	Core::LockGuard lock(m_mutex);

	Init(ctx);

	if (dw_offset >= DW_SIZE) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dw_offset >= DW_SIZE condition ignored (continuing)\n"); }
	if (dw_offset + dw_num > DW_SIZE) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dw_offset + dw_num > DW_SIZE condition ignored (continuing)\n"); }

	EXIT_IF(m_buffer == nullptr);

	void* data = nullptr;
	VulkanMapMemory(ctx, &m_buffer->memory, &data);

	EXIT_IF(data == nullptr);

	for (uint32_t i = 0; i < dw_num; i++)
	{
		static_cast<uint32_t*>(data)[dw_offset + i] = clear_value;
	}

	VulkanUnmapMemory(ctx, &m_buffer->memory);
}

void GdsBuffer::Read(GraphicContext* ctx, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size)
{
	EXIT_IF(dst == nullptr);

	Core::LockGuard lock(m_mutex);

	Init(ctx);

	if (dw_offset >= DW_SIZE) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dw_offset >= DW_SIZE condition ignored (continuing)\n"); }
	if (dw_offset + dw_size > DW_SIZE) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dw_offset + dw_size > DW_SIZE condition ignored (continuing)\n"); }

	EXIT_IF(m_buffer == nullptr);

	void* data = nullptr;
	VulkanMapMemory(ctx, &m_buffer->memory, &data);

	EXIT_IF(data == nullptr);

	for (uint32_t i = 0; i < dw_size; i++)
	{
		dst[i] = static_cast<uint32_t*>(data)[dw_offset + i];
	}

	VulkanUnmapMemory(ctx, &m_buffer->memory);
}

VulkanBuffer* GdsBuffer::GetBuffer(GraphicContext* ctx)
{
	Core::LockGuard lock(m_mutex);

	Init(ctx);

	return m_buffer;
}

void CommandPool::Create(int id)
{
	auto* ctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(id < 0 || id >= GraphicContext::QUEUES_NUM);
	EXIT_IF(m_pool[id] != nullptr);

	m_pool[id] = new VulkanCommandPool;

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->device == nullptr);
	EXIT_IF(ctx->queues[id].family == static_cast<uint32_t>(-1));
	EXIT_IF(m_pool[id]->pool != nullptr);
	EXIT_IF(m_pool[id]->buffers != nullptr);
	EXIT_IF(m_pool[id]->fences != nullptr);
	EXIT_IF(m_pool[id]->semaphores != nullptr);
	EXIT_IF(m_pool[id]->buffers_count != 0);

	VkCommandPoolCreateInfo pool_info {};
	pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.pNext            = nullptr;
	pool_info.queueFamilyIndex = ctx->queues[id].family;
	pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	vkCreateCommandPool(ctx->device, &pool_info, nullptr, &m_pool[id]->pool);

	if (m_pool[id]->pool == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_pool[id]->pool == nullptr condition ignored (continuing)\n"); }

	m_pool[id]->buffers_count = CommandProcessorSubmissionSlots::SlotCount;
	m_pool[id]->buffers       = new VkCommandBuffer[m_pool[id]->buffers_count];
	m_pool[id]->fences        = new VkFence[m_pool[id]->buffers_count];
	m_pool[id]->semaphores    = new VkSemaphore[m_pool[id]->buffers_count];
	m_pool[id]->busy          = new bool[m_pool[id]->buffers_count];

	VkCommandBufferAllocateInfo alloc_info {};
	alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.commandPool        = m_pool[id]->pool;
	alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = m_pool[id]->buffers_count;

	if (vkAllocateCommandBuffers(ctx->device, &alloc_info, m_pool[id]->buffers) != VK_SUCCESS)
	{
		EXIT("Can't allocate command buffers");
	}

	for (uint32_t i = 0; i < m_pool[id]->buffers_count; i++)
	{
		m_pool[id]->busy[i] = false;

		VkFenceCreateInfo fence_info {};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence_info.pNext = nullptr;
		fence_info.flags = 0;

		if (vkCreateFence(ctx->device, &fence_info, nullptr, &m_pool[id]->fences[i]) != VK_SUCCESS)
		{
			EXIT("Can't create fence");
		}

		VkSemaphoreCreateInfo semaphore_info {};
		semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore_info.pNext = nullptr;
		semaphore_info.flags = 0;

		if (vkCreateSemaphore(ctx->device, &semaphore_info, nullptr, &m_pool[id]->semaphores[i]) != VK_SUCCESS)
		{
			EXIT("Can't create semaphore");
		}

		EXIT_IF(m_pool[id]->buffers[i] == nullptr);
		EXIT_IF(m_pool[id]->fences[i] == nullptr);
		EXIT_IF(m_pool[id]->semaphores[i] == nullptr);
	}
}

void CommandPool::DeleteAll()
{
	auto* ctx = g_render_ctx->GetGraphicCtx();

	for (auto& pool: m_pool)
	{
		if (pool != nullptr)
		{
			EXIT_IF(ctx == nullptr);
			EXIT_IF(ctx->device == nullptr);

			for (uint32_t i = 0; i < pool->buffers_count; i++)
			{
				vkDestroySemaphore(ctx->device, pool->semaphores[i], nullptr);
				vkDestroyFence(ctx->device, pool->fences[i], nullptr);
			}

			vkFreeCommandBuffers(ctx->device, pool->pool, pool->buffers_count, pool->buffers);

			vkDestroyCommandPool(ctx->device, pool->pool, nullptr);

			delete[] pool->semaphores;
			delete[] pool->fences;
			delete[] pool->buffers;
			delete[] pool->busy;

			delete pool;
			pool = nullptr;
		}
	}
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED