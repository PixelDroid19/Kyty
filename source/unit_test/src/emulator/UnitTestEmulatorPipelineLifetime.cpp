#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Utils.h"

#include "../../../emulator/src/Graphics/GraphicsRenderInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace Kyty::Libs::Graphics {

class PipelineLifetimeTestAccess
{
public:
	static VulkanPipeline* AddRawPipeline(PipelineCache& cache, VkPipeline pipeline, VkPipelineLayout layout)
	{
		Core::LockGuard lock(cache.m_mutex);
		if (cache.m_pipelines.Size() >= PipelineCache::MaxPipelineCount())
		{
			return nullptr;
		}
		PipelineCache::Pipeline entry {};
		entry.static_params             = new PipelineStaticParameters {};
		entry.dynamic_params            = new PipelineDynamicParameters {};
		entry.pipeline                  = new VulkanPipeline {};
		entry.pipeline->pipeline        = pipeline;
		entry.pipeline->pipeline_layout = layout;
		entry.pipeline->static_params   = entry.static_params;
		entry.pipeline->dynamic_params  = entry.dynamic_params;
		cache.m_pipelines.Add(entry);
		return entry.pipeline;
	}

	static bool Attach(PipelineCache& cache, CommandBuffer& buffer, VulkanPipeline* pipeline)
	{
		Core::LockGuard lock(cache.m_mutex);
		for (auto& entry: cache.m_pipelines)
		{
			if (entry.pipeline == pipeline)
			{
				cache.AttachPipelineLease(&buffer, entry);
				return true;
			}
		}
		return false;
	}

	static bool Contains(PipelineCache& cache, const VulkanPipeline* pipeline)
	{
		Core::LockGuard lock(cache.m_mutex);
		for (const auto& entry: cache.m_pipelines)
		{
			if (entry.pipeline == pipeline)
			{
				return true;
			}
		}
		return false;
	}
};

} // namespace Kyty::Libs::Graphics

UT_BEGIN(EmulatorPipelineLifetime);

using namespace Libs::Graphics;

namespace {

constexpr std::array<uint32_t, 35> kNoopComputeSpirv = {
    0x07230203, 0x00010000, 0,          5,          0, 0x00020011, 1, 0x0003000e, 0, 1,          0x0005000f, 5,
    1,          0x6e69616d, 0,          0x00060010, 1, 17,         1, 1,          1, 0x00020013, 2,          0x00030021,
    3,          2,          0x00050036, 2,          1, 0,          3, 0x000200f8, 4, 0x000100fd, 0x00010038,
};

VKAPI_ATTR VkBool32 VKAPI_CALL PipelineLifetimeValidationCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                                  const VkDebugUtilsMessengerCallbackDataEXT*, void* user_data)
{
	if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
	{
		static_cast<std::atomic<uint32_t>*>(user_data)->fetch_add(1u);
	}
	return VK_FALSE;
}

class VulkanPipelineLifetimeTestContext
{
public:
	~VulkanPipelineLifetimeTestContext()
	{
		if (context.device != VK_NULL_HANDLE)
		{
			(void)vkDeviceWaitIdle(context.device);
			if (renderer_context_bound)
			{
				EXIT_IF(!GraphicsRenderUnbindContextForTesting(&context));
			}
			VulkanMemoryPoolRelease(&context);
			vkDestroyDevice(context.device, nullptr);
		}
		if (debug_messenger != VK_NULL_HANDLE && destroy_debug_messenger != nullptr)
		{
			destroy_debug_messenger(instance, debug_messenger, nullptr);
		}
		if (instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(instance, nullptr);
		}
	}

	bool Initialize(std::string* reason)
	{
		if (reason == nullptr)
		{
			return false;
		}
		if (g_render_ctx != nullptr && g_render_ctx->GetGraphicCtx() != nullptr)
		{
			*reason = "a renderer test context is already bound";
			return false;
		}
		if (!HasValidationLayer() || !HasDebugUtilsExtension())
		{
			*reason = "VK_LAYER_KHRONOS_validation with VK_EXT_debug_utils is unavailable";
			return false;
		}

		VkApplicationInfo app_info {};
		app_info.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app_info.apiVersion = VK_API_VERSION_1_4;

		const char*                        layer     = "VK_LAYER_KHRONOS_validation";
		const char*                        extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		VkDebugUtilsMessengerCreateInfoEXT debug_info {};
		debug_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debug_info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debug_info.pfnUserCallback = PipelineLifetimeValidationCallback;
		debug_info.pUserData       = &validation_errors;

		VkInstanceCreateInfo instance_info {};
		instance_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance_info.pNext                   = &debug_info;
		instance_info.pApplicationInfo        = &app_info;
		instance_info.enabledLayerCount       = 1;
		instance_info.ppEnabledLayerNames     = &layer;
		instance_info.enabledExtensionCount   = 1;
		instance_info.ppEnabledExtensionNames = &extension;
		if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
		{
			*reason = "failed to create a validation-enabled Vulkan instance";
			return false;
		}

		const auto create_messenger =
		    reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
		destroy_debug_messenger =
		    reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
		if (create_messenger == nullptr || destroy_debug_messenger == nullptr ||
		    create_messenger(instance, &debug_info, nullptr, &debug_messenger) != VK_SUCCESS)
		{
			*reason = "failed to create the Vulkan validation messenger";
			return false;
		}

		uint32_t physical_device_count = 0;
		if (vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr) != VK_SUCCESS || physical_device_count == 0)
		{
			*reason = "no Vulkan physical device is available";
			return false;
		}
		std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
		if (vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.data()) != VK_SUCCESS)
		{
			*reason = "failed to enumerate Vulkan physical devices";
			return false;
		}

		for (const auto physical_device: physical_devices)
		{
			VkPhysicalDeviceProperties properties {};
			vkGetPhysicalDeviceProperties(physical_device, &properties);
			if (properties.apiVersion < VK_API_VERSION_1_4)
			{
				continue;
			}

			VkPhysicalDeviceTimelineSemaphoreFeatures timeline_supported {};
			timeline_supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
			VkPhysicalDeviceFeatures2 features {};
			features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			features.pNext = &timeline_supported;
			vkGetPhysicalDeviceFeatures2(physical_device, &features);
			if (timeline_supported.timelineSemaphore != VK_TRUE)
			{
				continue;
			}

			uint32_t family_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
			std::vector<VkQueueFamilyProperties> families(family_count);
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
			for (uint32_t family = 0; family < family_count; family++)
			{
				const auto flags = families[family].queueFlags;
				if (families[family].queueCount == 0 ||
				    (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
				{
					continue;
				}

				constexpr float         priority = 1.0f;
				VkDeviceQueueCreateInfo queue_info {};
				queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				queue_info.queueFamilyIndex = family;
				queue_info.queueCount       = 1;
				queue_info.pQueuePriorities = &priority;
				VkPhysicalDeviceTimelineSemaphoreFeatures timeline_enabled {};
				timeline_enabled.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
				timeline_enabled.timelineSemaphore = VK_TRUE;
				VkDeviceCreateInfo device_info {};
				device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
				device_info.pNext                = &timeline_enabled;
				device_info.queueCreateInfoCount = 1;
				device_info.pQueueCreateInfos    = &queue_info;
				if (vkCreateDevice(physical_device, &device_info, nullptr, &context.device) != VK_SUCCESS)
				{
					continue;
				}

				context.instance                                 = instance;
				context.physical_device                          = physical_device;
				context.queue_mutex_count                        = 1;
				context.queues[GraphicContext::QUEUE_GFX].family = family;
				context.queues[GraphicContext::QUEUE_GFX].index  = 0;
				context.queues[GraphicContext::QUEUE_GFX].mutex  = &context.queue_mutexes[0];
				context.queues[GraphicContext::QUEUE_UTIL]       = context.queues[GraphicContext::QUEUE_GFX];
				vkGetDeviceQueue(context.device, family, 0, &context.queues[GraphicContext::QUEUE_GFX].vk_queue);
				context.queues[GraphicContext::QUEUE_UTIL].vk_queue = context.queues[GraphicContext::QUEUE_GFX].vk_queue;
				if (context.queues[GraphicContext::QUEUE_GFX].vk_queue == VK_NULL_HANDLE)
				{
					vkDestroyDevice(context.device, nullptr);
					context.device = VK_NULL_HANDLE;
					continue;
				}

				if (!GraphicsRenderBindContextForTesting(&context))
				{
					*reason = "failed to bind the test Vulkan device to the renderer";
					return false;
				}
				renderer_context_bound = true;
				return true;
			}
		}

		*reason = "no Vulkan 1.4 graphics+compute device with timeline semaphores is available";
		return false;
	}

	[[nodiscard]] uint32_t ValidationErrors() const { return validation_errors.load(); }

	GraphicContext context {};

private:
	static bool HasValidationLayer()
	{
		uint32_t count = 0;
		if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0)
		{
			return false;
		}
		std::vector<VkLayerProperties> layers(count);
		if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
		{
			return false;
		}
		return std::any_of(layers.begin(), layers.end(),
		                   [](const auto& layer) { return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
	}

	static bool HasDebugUtilsExtension()
	{
		uint32_t count = 0;
		if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
		{
			return false;
		}
		std::vector<VkExtensionProperties> extensions(count);
		if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS)
		{
			return false;
		}
		return std::any_of(extensions.begin(), extensions.end(), [](const auto& extension)
		                   { return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0; });
	}

	VkInstance                          instance                = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT            debug_messenger         = VK_NULL_HANDLE;
	PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger = nullptr;
	std::atomic<uint32_t>               validation_errors {0};
	bool                                renderer_context_bound = false;
};

class ScopedComputePipeline
{
public:
	explicit ScopedComputePipeline(VkDevice device): m_device(device) {}
	~ScopedComputePipeline()
	{
		if (m_pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(m_device, m_pipeline, nullptr);
		}
		if (m_layout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(m_device, m_layout, nullptr);
		}
	}

	bool Create()
	{
		VkShaderModuleCreateInfo module_info {};
		module_info.sType     = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		module_info.codeSize  = kNoopComputeSpirv.size() * sizeof(uint32_t);
		module_info.pCode     = kNoopComputeSpirv.data();
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(m_device, &module_info, nullptr, &module) != VK_SUCCESS)
		{
			return false;
		}

		VkPipelineLayoutCreateInfo layout_info {};
		layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		if (vkCreatePipelineLayout(m_device, &layout_info, nullptr, &m_layout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, module, nullptr);
			return false;
		}

		VkPipelineShaderStageCreateInfo stage_info {};
		stage_info.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage_info.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
		stage_info.module = module;
		stage_info.pName  = "main";
		VkComputePipelineCreateInfo pipeline_info {};
		pipeline_info.sType             = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipeline_info.stage             = stage_info;
		pipeline_info.layout            = m_layout;
		pipeline_info.basePipelineIndex = -1;
		const auto result               = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipeline);
		vkDestroyShaderModule(m_device, module, nullptr);
		return result == VK_SUCCESS && m_pipeline != VK_NULL_HANDLE;
	}

	[[nodiscard]] VkPipeline       Pipeline() const { return m_pipeline; }
	[[nodiscard]] VkPipelineLayout Layout() const { return m_layout; }
	void                           Release()
	{
		m_pipeline = VK_NULL_HANDLE;
		m_layout   = VK_NULL_HANDLE;
	}

private:
	VkDevice         m_device   = VK_NULL_HANDLE;
	VkPipeline       m_pipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_layout   = VK_NULL_HANDLE;
};

class ScopedTimelineQueueBlock
{
public:
	ScopedTimelineQueueBlock(VkDevice device, VkQueue queue, VkCommandPool command_pool)
	    : m_device(device), m_queue(queue), m_command_pool(command_pool)
	{
	}
	~ScopedTimelineQueueBlock()
	{
		if (m_semaphore != VK_NULL_HANDLE && !m_signaled)
		{
			Signal();
		}
		if (m_submitted)
		{
			(void)vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, 5000000000ull);
		}
		if (m_command_buffer != VK_NULL_HANDLE)
		{
			vkFreeCommandBuffers(m_device, m_command_pool, 1, &m_command_buffer);
		}
		if (m_fence != VK_NULL_HANDLE)
		{
			vkDestroyFence(m_device, m_fence, nullptr);
		}
		if (m_semaphore != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(m_device, m_semaphore, nullptr);
		}
	}

	bool Initialize()
	{
		VkSemaphoreTypeCreateInfo type_info {};
		type_info.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo semaphore_info {};
		semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore_info.pNext = &type_info;
		if (vkCreateSemaphore(m_device, &semaphore_info, nullptr, &m_semaphore) != VK_SUCCESS)
		{
			return false;
		}
		VkFenceCreateInfo fence_info {};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (vkCreateFence(m_device, &fence_info, nullptr, &m_fence) != VK_SUCCESS)
		{
			return false;
		}
		VkCommandBufferAllocateInfo allocate_info {};
		allocate_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate_info.commandPool        = m_command_pool;
		allocate_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate_info.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(m_device, &allocate_info, &m_command_buffer) != VK_SUCCESS)
		{
			return false;
		}
		VkCommandBufferBeginInfo begin_info {};
		begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		if (vkBeginCommandBuffer(m_command_buffer, &begin_info) != VK_SUCCESS)
		{
			return false;
		}
		return vkEndCommandBuffer(m_command_buffer) == VK_SUCCESS;
	}

	bool SubmitBlocker()
	{
		const uint64_t                wait_value = 1;
		const VkPipelineStageFlags    wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkTimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.sType                   = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		timeline_info.waitSemaphoreValueCount = 1;
		timeline_info.pWaitSemaphoreValues    = &wait_value;
		VkSubmitInfo submit_info {};
		submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit_info.pNext              = &timeline_info;
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores    = &m_semaphore;
		submit_info.pWaitDstStageMask  = &wait_stage;
		submit_info.commandBufferCount = 1;
		submit_info.pCommandBuffers    = &m_command_buffer;
		m_submitted                    = vkQueueSubmit(m_queue, 1, &submit_info, m_fence) == VK_SUCCESS;
		return m_submitted;
	}

	bool Signal()
	{
		if (m_signaled)
		{
			return true;
		}
		VkSemaphoreSignalInfo signal_info {};
		signal_info.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
		signal_info.semaphore = m_semaphore;
		signal_info.value     = 1;
		m_signaled            = vkSignalSemaphore(m_device, &signal_info) == VK_SUCCESS;
		return m_signaled;
	}

	[[nodiscard]] VkFence Fence() const { return m_fence; }

private:
	VkDevice        m_device         = VK_NULL_HANDLE;
	VkQueue         m_queue          = VK_NULL_HANDLE;
	VkCommandPool   m_command_pool   = VK_NULL_HANDLE;
	VkCommandBuffer m_command_buffer = VK_NULL_HANDLE;
	VkSemaphore     m_semaphore      = VK_NULL_HANDLE;
	VkFence         m_fence          = VK_NULL_HANDLE;
	bool            m_submitted      = false;
	bool            m_signaled       = false;
};

class ScopedCachedPipelineRetirement
{
public:
	explicit ScopedCachedPipelineRetirement(PipelineCache* cache): m_cache(cache) {}
	~ScopedCachedPipelineRetirement()
	{
		if (m_cache != nullptr && m_pipeline != nullptr && PipelineLifetimeTestAccess::Contains(*m_cache, m_pipeline))
		{
			m_cache->DeletePipeline(m_pipeline);
		}
	}
	void SetPipeline(VulkanPipeline* pipeline) { m_pipeline = pipeline; }

private:
	PipelineCache*  m_cache    = nullptr;
	VulkanPipeline* m_pipeline = nullptr;
};

class ScopedCommandBufferResetWithoutLabelCallbacks
{
public:
	explicit ScopedCommandBufferResetWithoutLabelCallbacks(CommandBuffer* command_buffer): m_command_buffer(command_buffer) {}
	~ScopedCommandBufferResetWithoutLabelCallbacks()
	{
		if (m_command_buffer != nullptr && !m_command_buffer->IsInvalid())
		{
			m_command_buffer->WaitForFenceAndResetWithoutLabelCallbacks();
		}
	}

private:
	CommandBuffer* m_command_buffer = nullptr;
};

} // namespace

TEST(EmulatorPipelineLifetime, RetirementWaitsForEveryRecordingLease)
{
	PipelineLifetimeState lifetime;

	EXPECT_TRUE(lifetime.Acquire());
	EXPECT_TRUE(lifetime.Acquire());
	EXPECT_FALSE(lifetime.RequestRetirement());
	EXPECT_FALSE(lifetime.Acquire());
	EXPECT_FALSE(lifetime.Release());
	EXPECT_TRUE(lifetime.Release());
	EXPECT_TRUE(lifetime.RetirementRequested());
	EXPECT_EQ(lifetime.ReferenceCount(), 0u);
}

TEST(EmulatorPipelineLifetime, EvictionSkipsPinnedEntriesAndReportsExhaustion)
{
	PipelineLifetimeState lifetimes[3];
	ASSERT_TRUE(lifetimes[0].Acquire());
	ASSERT_TRUE(lifetimes[1].Acquire());
	uint32_t cursor = 0;

	EXPECT_EQ(PipelineCacheNextEvictableIndex(3, &cursor, [&lifetimes](uint32_t index) { return lifetimes[index].CanEvict(); }), 2u);
	EXPECT_EQ(cursor, 0u);
	ASSERT_TRUE(lifetimes[2].Acquire());
	EXPECT_EQ(PipelineCacheNextEvictableIndex(3, &cursor, [&lifetimes](uint32_t index) { return lifetimes[index].CanEvict(); }),
	          UINT32_MAX);
	EXPECT_EQ(cursor, 0u);

	EXPECT_FALSE(lifetimes[1].Release());
	EXPECT_EQ(PipelineCacheNextEvictableIndex(3, &cursor, [&lifetimes](uint32_t index) { return lifetimes[index].CanEvict(); }), 1u);
}

TEST(EmulatorPipelineLifetime, CacheInvalidationKeepsBoundPipelineAliveUntilPendingBufferReset)
{
	VulkanPipelineLifetimeTestContext test_context;
	std::string                       reason;
	if (!test_context.Initialize(&reason))
	{
		GTEST_SKIP() << reason;
	}

	auto* cache = g_render_ctx->GetPipelineCache();
	ASSERT_NE(cache, nullptr);
	ScopedCachedPipelineRetirement                cleanup(cache);
	CommandBuffer                                 command_buffer(GraphicContext::QUEUE_GFX);
	ScopedCommandBufferResetWithoutLabelCallbacks command_buffer_reset(&command_buffer);
	ASSERT_FALSE(command_buffer.IsInvalid());
	ScopedTimelineQueueBlock queue_block(test_context.context.device, test_context.context.queues[GraphicContext::QUEUE_GFX].vk_queue,
	                                     command_buffer.GetPool()->pool);
	ASSERT_TRUE(queue_block.Initialize());
	ScopedComputePipeline created_pipeline(test_context.context.device);
	ASSERT_TRUE(created_pipeline.Create());

	const VkPipeline       raw_pipeline = created_pipeline.Pipeline();
	const VkPipelineLayout raw_layout   = created_pipeline.Layout();
	auto*                  pipeline     = PipelineLifetimeTestAccess::AddRawPipeline(*cache, raw_pipeline, raw_layout);
	ASSERT_NE(pipeline, nullptr);
	created_pipeline.Release();
	cleanup.SetPipeline(pipeline);

	command_buffer.Begin();
	ASSERT_TRUE(PipelineLifetimeTestAccess::Attach(*cache, command_buffer, pipeline));
	const auto command = command_buffer.RecordOutsideRenderPass();
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, raw_pipeline);
	vkCmdDispatch(command, 1, 1, 1);
	command_buffer.End();

	// Invalidate while the command buffer is executable, then keep the queue
	// pending so invalidation is also exercised against an in-flight submission.
	cache->DeletePipeline(pipeline);
	ASSERT_TRUE(PipelineLifetimeTestAccess::Contains(*cache, pipeline));
	ASSERT_TRUE(queue_block.SubmitBlocker());
	command_buffer.Execute();
	cache->DeleteAllPipelines();
	ASSERT_TRUE(PipelineLifetimeTestAccess::Contains(*cache, pipeline));
	const auto command_fence = command_buffer.GetPool()->fences[command_buffer.GetIndex()];
	if (vkGetFenceStatus(test_context.context.device, command_fence) != VK_NOT_READY)
	{
		GTEST_SKIP() << "the Vulkan queue did not remain pending behind its timeline semaphore";
	}

	ASSERT_TRUE(queue_block.Signal());
	ASSERT_EQ(vkWaitForFences(test_context.context.device, 1, &command_fence, VK_TRUE, 5000000000ull), VK_SUCCESS);
	const VkFence blocker_fence = queue_block.Fence();
	ASSERT_EQ(vkWaitForFences(test_context.context.device, 1, &blocker_fence, VK_TRUE, 5000000000ull), VK_SUCCESS);
	command_buffer.WaitForFenceAndResetWithoutLabelCallbacks();
	EXPECT_FALSE(PipelineLifetimeTestAccess::Contains(*cache, pipeline));
	EXPECT_EQ(test_context.ValidationErrors(), 0u);
}

UT_END();
