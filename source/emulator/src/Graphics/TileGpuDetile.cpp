#include "Emulator/Graphics/Tile.h"

#include "Kyty/Core/Threads.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Utils.h"
#include "GraphicsRenderInternal.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// This state belongs to one GraphicContext, not a process-global VkDevice
// cache. A timed-out submission keeps every referenced object here until the
// fence completes or the owning context is explicitly released.
struct GpuDetileContext
{
	VkDevice              device          = VK_NULL_HANDLE;
	VkPhysicalDevice      physical_device = VK_NULL_HANDLE;
	VkQueue               queue           = VK_NULL_HANDLE;
	Core::Mutex*          queue_mutex     = nullptr;
	uint32_t              queue_family    = UINT32_MAX;
	VkShaderModule        module          = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout      = VK_NULL_HANDLE;
	VkPipelineLayout      pipeline_layout = VK_NULL_HANDLE;
	VkPipeline            pipeline        = VK_NULL_HANDLE;
	VkDescriptorPool      descriptor_pool = VK_NULL_HANDLE;
	VkDescriptorSet       descriptor_set  = VK_NULL_HANDLE;
	VkCommandPool         command_pool    = VK_NULL_HANDLE;
	VkCommandBuffer       command_buffer  = VK_NULL_HANDLE;
	VkFence               fence           = VK_NULL_HANDLE;
	VulkanBuffer          tiled;
	VulkanBuffer          linear;
	uint64_t              tiled_capacity  = 0;
	uint64_t              linear_capacity = 0;
	bool                  resources_ready = false;
	bool                  in_flight       = false;
	bool                  unavailable     = false;
};

namespace {

#include "host_shaders/gen5_detile_comp.inc"

struct DetilePushConstants
{
	uint32_t width             = 0;
	uint32_t height            = 0;
	uint32_t pitch_elems       = 0;
	uint32_t dst_pitch_elems   = 0;
	uint32_t bytes_per_element = 0;
	uint32_t layout_kind       = 0;
	uint32_t src_u32_count     = 0;
	uint32_t dst_u32_count     = 0;
	uint32_t src_byte_offset   = 0;
};

constexpr uint64_t k_max_shader_byte_range = std::numeric_limits<uint32_t>::max();
// Diagnostic-only host-visible buffers are deliberately capped well below
// Vulkan's guaranteed maxStorageBufferRange minimum. This keeps an accidental
// diagnostic request from reserving a large pair of persistent context buffers.
constexpr uint64_t k_max_diagnostic_session_bytes = 16u * 1024u * 1024u;
constexpr uint64_t k_fence_timeout_ns              = 10000000000ull;
constexpr uint64_t k_release_fence_timeout_ns      = 1000000000ull;

std::atomic<TileGpuDetileTestFault> g_test_fault {TileGpuDetileTestFault::None};

bool HasTestFault(TileGpuDetileTestFault fault)
{
	return g_test_fault.load(std::memory_order_relaxed) == fault;
}

bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t* result)
{
	if (result == nullptr || (left != 0u && right > std::numeric_limits<uint64_t>::max() / left))
	{
		return false;
	}
	*result = left * right;
	return true;
}

bool CheckedAdd(uint64_t left, uint64_t right, uint64_t* result)
{
	if (result == nullptr || right > std::numeric_limits<uint64_t>::max() - left)
	{
		return false;
	}
	*result = left + right;
	return true;
}

bool DivideRoundUp(uint32_t value, uint32_t divisor, uint32_t* result)
{
	if (result == nullptr || divisor == 0u)
	{
		return false;
	}
	*result = value / divisor + (value % divisor == 0u ? 0u : 1u);
	return true;
}

uint32_t ResolvePitch(uint32_t pitch_elems, uint32_t width)
{
	return pitch_elems != 0u ? pitch_elems : width;
}

uint32_t LayoutKind(TileDetileLayout layout)
{
	switch (layout)
	{
		case TileDetileLayout::Sw64kRx: return 0u;
		case TileDetileLayout::Standard64KB: return 1u;
		case TileDetileLayout::Standard4KB: return 2u;
		case TileDetileLayout::Depth64KB: return 3u;
		default: return UINT32_MAX;
	}
}

bool GpuLayoutSupported(const TileDetileRequest& request)
{
	if (!TileDetileIsSupported(request) || request.src_bytes == 0u || request.src_bytes > k_max_shader_byte_range ||
	    (request.src_bytes % 4u) != 0u || (request.bytes_per_element % 4u) != 0u)
	{
		return false;
	}

	switch (request.layout)
	{
		case TileDetileLayout::Sw64kRx: return request.bytes_per_element == 4u || request.bytes_per_element == 8u;
		case TileDetileLayout::Standard64KB:
			return request.bytes_per_element == 4u || request.bytes_per_element == 8u || request.bytes_per_element == 16u;
		case TileDetileLayout::Standard4KB: return request.bytes_per_element == 4u;
		case TileDetileLayout::Depth64KB: return request.bytes_per_element == 4u;
		default: return false;
	}
}

bool CalculateGpuLinearBytes(const TileDetileRequest& request, uint64_t* bytes)
{
	uint64_t elements = 0;
	return bytes != nullptr && CheckedMultiply(ResolvePitch(request.dst_pitch_elems, request.width), request.height, &elements) &&
	       CheckedMultiply(elements, request.bytes_per_element, bytes) && *bytes != 0u && *bytes <= k_max_shader_byte_range &&
	       (*bytes % 4u) == 0u;
}

bool QueueSupportsGraphicsCompute(VkPhysicalDevice physical_device, uint32_t family)
{
	if (physical_device == VK_NULL_HANDLE)
	{
		return false;
	}
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
	if (family >= count || count == 0u)
	{
		return false;
	}
	std::vector<VkQueueFamilyProperties> properties(count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, properties.data());
	const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
	return (properties[family].queueFlags & required) == required;
}

bool ValidateDeviceLimits(GraphicContext* ctx, const TileDetileRequest& request, uint64_t linear_bytes, uint32_t* groups_x,
                          uint32_t* groups_y)
{
	if (ctx == nullptr || ctx->physical_device == VK_NULL_HANDLE || groups_x == nullptr || groups_y == nullptr)
	{
		return false;
	}
	if (!DivideRoundUp(request.width, 8u, groups_x) || !DivideRoundUp(request.height, 8u, groups_y))
	{
		return false;
	}

	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(ctx->physical_device, &properties);
	if (request.src_bytes > properties.limits.maxStorageBufferRange || linear_bytes > properties.limits.maxStorageBufferRange ||
	    *groups_x > properties.limits.maxComputeWorkGroupCount[0] || *groups_y > properties.limits.maxComputeWorkGroupCount[1] ||
	    properties.limits.maxComputeWorkGroupSize[0] < 8u || properties.limits.maxComputeWorkGroupSize[1] < 8u ||
	    properties.limits.maxComputeWorkGroupSize[2] == 0u || properties.limits.maxComputeWorkGroupInvocations < 64u)
	{
		return false;
	}

	const auto& queue = ctx->queues[GraphicContext::QUEUE_GFX];
	return queue.vk_queue != VK_NULL_HANDLE && queue.mutex != nullptr && queue.family != UINT32_MAX &&
	       QueueSupportsGraphicsCompute(ctx->physical_device, queue.family);
}

bool FindHostCoherentMemoryType(GraphicContext* ctx, uint32_t type_bits, uint32_t* type)
{
	if (ctx == nullptr || ctx->physical_device == VK_NULL_HANDLE || type == nullptr)
	{
		return false;
	}
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &properties);
	const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((type_bits & (static_cast<uint32_t>(1u) << index)) != 0u &&
		    (properties.memoryTypes[index].propertyFlags & required) == required)
		{
			*type = index;
			return true;
		}
	}
	return false;
}

bool DestroyHostStorageBuffer(GraphicContext* ctx, VulkanBuffer* buffer)
{
	if (buffer == nullptr)
	{
		return false;
	}
	if (buffer->buffer == VK_NULL_HANDLE && buffer->memory.memory == VK_NULL_HANDLE)
	{
		*buffer = {};
		return true;
	}
	if (ctx == nullptr || ctx->device == VK_NULL_HANDLE)
	{
		return false;
	}
	if (buffer->buffer != VK_NULL_HANDLE)
	{
		vkDestroyBuffer(ctx->device, buffer->buffer, nullptr);
		buffer->buffer = VK_NULL_HANDLE;
	}
	if (buffer->memory.memory != VK_NULL_HANDLE)
	{
		vkFreeMemory(ctx->device, buffer->memory.memory, nullptr);
		buffer->memory.memory = VK_NULL_HANDLE;
	}
	*buffer = {};
	return true;
}

bool CreateHostStorageBuffer(GraphicContext* ctx, uint64_t size, VulkanBuffer* buffer)
{
	if (ctx == nullptr || buffer == nullptr || ctx->device == VK_NULL_HANDLE || ctx->physical_device == VK_NULL_HANDLE || size == 0u ||
	    size > k_max_shader_byte_range)
	{
		return false;
	}

	VulkanBuffer candidate {};
	candidate.usage           = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	candidate.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VkBufferCreateInfo buffer_info {};
	buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size        = size;
	buffer_info.usage       = candidate.usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(ctx->device, &buffer_info, nullptr, &candidate.buffer) != VK_SUCCESS || candidate.buffer == VK_NULL_HANDLE)
	{
		return false;
	}

	vkGetBufferMemoryRequirements(ctx->device, candidate.buffer, &candidate.memory.requirements);
	uint32_t memory_type = 0;
	if (!FindHostCoherentMemoryType(ctx, candidate.memory.requirements.memoryTypeBits, &memory_type))
	{
		DestroyHostStorageBuffer(ctx, &candidate);
		return false;
	}

	VkMemoryAllocateInfo allocate_info {};
	allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize  = candidate.memory.requirements.size;
	allocate_info.memoryTypeIndex = memory_type;
	if (HasTestFault(TileGpuDetileTestFault::AllocateMemory) ||
	    vkAllocateMemory(ctx->device, &allocate_info, nullptr, &candidate.memory.memory) != VK_SUCCESS ||
	    candidate.memory.memory == VK_NULL_HANDLE)
	{
		DestroyHostStorageBuffer(ctx, &candidate);
		return false;
	}
	candidate.memory.offset = 0;
	candidate.memory.type   = memory_type;
	if (vkBindBufferMemory(ctx->device, candidate.buffer, candidate.memory.memory, candidate.memory.offset) != VK_SUCCESS)
	{
		DestroyHostStorageBuffer(ctx, &candidate);
		return false;
	}

	*buffer = candidate;
	return true;
}

bool EnsureHostStorageBuffer(GraphicContext* ctx, VulkanBuffer* buffer, uint64_t* capacity, uint64_t required)
{
	if (buffer == nullptr || capacity == nullptr || required == 0u || required > k_max_shader_byte_range)
	{
		return false;
	}
	if (buffer->buffer != VK_NULL_HANDLE && *capacity >= required)
	{
		return true;
	}

	VulkanBuffer replacement {};
	if (!CreateHostStorageBuffer(ctx, required, &replacement))
	{
		return false;
	}
	if (!DestroyHostStorageBuffer(ctx, buffer))
	{
		(void)DestroyHostStorageBuffer(ctx, &replacement);
		return false;
	}
	*buffer   = replacement;
	*capacity = required;
	return true;
}

bool FitsDiagnosticRetainedCapacity(const GpuDetileContext& state, uint64_t tiled_required, uint64_t linear_required)
{
	const uint64_t tiled_capacity  = state.tiled_capacity > tiled_required ? state.tiled_capacity : tiled_required;
	const uint64_t linear_capacity = state.linear_capacity > linear_required ? state.linear_capacity : linear_required;
	uint64_t       retained_bytes  = 0;
	return CheckedAdd(tiled_capacity, linear_capacity, &retained_bytes) && retained_bytes <= k_max_diagnostic_session_bytes;
}

bool CopyHostToBuffer(GraphicContext* ctx, const void* source, VulkanBuffer* buffer, uint64_t bytes)
{
	if (ctx == nullptr || source == nullptr || buffer == nullptr || buffer->buffer == VK_NULL_HANDLE ||
	    buffer->memory.memory == VK_NULL_HANDLE || bytes == 0u || bytes > buffer->memory.requirements.size)
	{
		return false;
	}
	void* mapped = nullptr;
	if (vkMapMemory(ctx->device, buffer->memory.memory, buffer->memory.offset, bytes, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
	{
		return false;
	}
	std::memcpy(mapped, source, static_cast<size_t>(bytes));
	vkUnmapMemory(ctx->device, buffer->memory.memory);
	return true;
}

bool CopyBufferToHost(GraphicContext* ctx, VulkanBuffer* buffer, void* destination, uint64_t bytes)
{
	if (ctx == nullptr || destination == nullptr || buffer == nullptr || buffer->buffer == VK_NULL_HANDLE ||
	    buffer->memory.memory == VK_NULL_HANDLE || bytes == 0u || bytes > buffer->memory.requirements.size)
	{
		return false;
	}
	void* mapped = nullptr;
	if (vkMapMemory(ctx->device, buffer->memory.memory, buffer->memory.offset, bytes, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
	{
		return false;
	}
	std::memcpy(destination, mapped, static_cast<size_t>(bytes));
	vkUnmapMemory(ctx->device, buffer->memory.memory);
	return true;
}

bool DestroyGpuDetileResources(GraphicContext* ctx, GpuDetileContext* state)
{
	if (ctx == nullptr || state == nullptr || ctx->device == VK_NULL_HANDLE || state->device != ctx->device || state->in_flight)
	{
		return false;
	}
	if (!DestroyHostStorageBuffer(ctx, &state->linear) || !DestroyHostStorageBuffer(ctx, &state->tiled))
	{
		return false;
	}
	if (state->descriptor_pool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(ctx->device, state->descriptor_pool, nullptr);
		state->descriptor_pool = VK_NULL_HANDLE;
	}
	state->descriptor_set = VK_NULL_HANDLE;
	if (state->pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(ctx->device, state->pipeline, nullptr);
		state->pipeline = VK_NULL_HANDLE;
	}
	if (state->pipeline_layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(ctx->device, state->pipeline_layout, nullptr);
		state->pipeline_layout = VK_NULL_HANDLE;
	}
	if (state->set_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(ctx->device, state->set_layout, nullptr);
		state->set_layout = VK_NULL_HANDLE;
	}
	if (state->module != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(ctx->device, state->module, nullptr);
		state->module = VK_NULL_HANDLE;
	}
	if (state->fence != VK_NULL_HANDLE)
	{
		vkDestroyFence(ctx->device, state->fence, nullptr);
		state->fence = VK_NULL_HANDLE;
	}
	// Command buffers are implicitly freed with their pool. Clear the child
	// handle before destroying the parent so a repeated teardown cannot reuse it.
	state->command_buffer = VK_NULL_HANDLE;
	if (state->command_pool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(ctx->device, state->command_pool, nullptr);
		state->command_pool = VK_NULL_HANDLE;
	}
	state->tiled_capacity  = 0;
	state->linear_capacity = 0;
	state->resources_ready = false;
	state->in_flight       = false;
	return true;
}

bool CreateGpuDetileResources(GraphicContext* ctx, GpuDetileContext* state)
{
	if (ctx == nullptr || state == nullptr || ctx->device == VK_NULL_HANDLE || ctx->physical_device == VK_NULL_HANDLE)
	{
		return false;
	}
	const auto& queue = ctx->queues[GraphicContext::QUEUE_GFX];
	if (queue.vk_queue == VK_NULL_HANDLE || queue.mutex == nullptr || queue.family == UINT32_MAX ||
	    !QueueSupportsGraphicsCompute(ctx->physical_device, queue.family))
	{
		return false;
	}

	state->device          = ctx->device;
	state->physical_device = ctx->physical_device;
	state->queue           = queue.vk_queue;
	state->queue_mutex     = queue.mutex;
	state->queue_family    = queue.family;

	VkShaderModuleCreateInfo shader_info {};
	shader_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader_info.codeSize = sizeof(kGen5DetileCompSpirv);
	shader_info.pCode    = kGen5DetileCompSpirv;
	if (vkCreateShaderModule(ctx->device, &shader_info, nullptr, &state->module) != VK_SUCCESS || state->module == VK_NULL_HANDLE)
	{
		return false;
	}

	VkDescriptorSetLayoutBinding bindings[1] {};
	bindings[0].binding         = 0;
	bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].descriptorCount = 2;
	bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo set_layout_info {};
	set_layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout_info.bindingCount = 1;
	set_layout_info.pBindings    = bindings;
	if (vkCreateDescriptorSetLayout(ctx->device, &set_layout_info, nullptr, &state->set_layout) != VK_SUCCESS ||
	    state->set_layout == VK_NULL_HANDLE)
	{
		return false;
	}

	VkPushConstantRange push_range {};
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.size       = sizeof(DetilePushConstants);
	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &state->set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	if (vkCreatePipelineLayout(ctx->device, &pipeline_layout_info, nullptr, &state->pipeline_layout) != VK_SUCCESS ||
	    state->pipeline_layout == VK_NULL_HANDLE)
	{
		return false;
	}

	VkPipelineShaderStageCreateInfo shader_stage {};
	shader_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shader_stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	shader_stage.module = state->module;
	shader_stage.pName  = "main";
	VkComputePipelineCreateInfo pipeline_info {};
	pipeline_info.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage  = shader_stage;
	pipeline_info.layout = state->pipeline_layout;
	// This diagnostic session keeps its pipeline for the context lifetime. Do
	// not share the graphics pipeline cache, whose use is synchronized by the
	// renderer independently of gpu_detile_mutex.
	if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &state->pipeline) != VK_SUCCESS ||
	    state->pipeline == VK_NULL_HANDLE)
	{
		return false;
	}

	VkDescriptorPoolSize pool_size {};
	pool_size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_size.descriptorCount = 2;
	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes    = &pool_size;
	if (vkCreateDescriptorPool(ctx->device, &pool_info, nullptr, &state->descriptor_pool) != VK_SUCCESS ||
	    state->descriptor_pool == VK_NULL_HANDLE)
	{
		return false;
	}

	VkDescriptorSetAllocateInfo set_info {};
	set_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool     = state->descriptor_pool;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts        = &state->set_layout;
	if (vkAllocateDescriptorSets(ctx->device, &set_info, &state->descriptor_set) != VK_SUCCESS || state->descriptor_set == VK_NULL_HANDLE)
	{
		return false;
	}

	VkCommandPoolCreateInfo command_pool_info {};
	command_pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	command_pool_info.queueFamilyIndex = state->queue_family;
	if (vkCreateCommandPool(ctx->device, &command_pool_info, nullptr, &state->command_pool) != VK_SUCCESS ||
	    state->command_pool == VK_NULL_HANDLE)
	{
		return false;
	}
	VkCommandBufferAllocateInfo command_buffer_info {};
	command_buffer_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_buffer_info.commandPool        = state->command_pool;
	command_buffer_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_buffer_info.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(ctx->device, &command_buffer_info, &state->command_buffer) != VK_SUCCESS ||
	    state->command_buffer == VK_NULL_HANDLE)
	{
		return false;
	}
	VkFenceCreateInfo fence_info {};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (HasTestFault(TileGpuDetileTestFault::CreateFence) ||
	    vkCreateFence(ctx->device, &fence_info, nullptr, &state->fence) != VK_SUCCESS || state->fence == VK_NULL_HANDLE)
	{
		return false;
	}

	state->resources_ready = true;
	return true;
}

TileGpuDetileStatus GetGpuDetileContext(GraphicContext* ctx, GpuDetileContext** state)
{
	if (ctx == nullptr || state == nullptr || ctx->device == VK_NULL_HANDLE || ctx->physical_device == VK_NULL_HANDLE)
	{
		return TileGpuDetileStatus::ContextUnavailable;
	}
	if (ctx->gpu_detile_context == nullptr)
	{
		ctx->gpu_detile_context = new (std::nothrow) GpuDetileContext;
		if (ctx->gpu_detile_context == nullptr)
		{
			return TileGpuDetileStatus::ResourceUnavailable;
		}
	}

	auto* result = ctx->gpu_detile_context;
	if (result->device != VK_NULL_HANDLE && (result->device != ctx->device || result->physical_device != ctx->physical_device ||
	                                         result->queue != ctx->queues[GraphicContext::QUEUE_GFX].vk_queue ||
	                                         result->queue_family != ctx->queues[GraphicContext::QUEUE_GFX].family))
	{
		return TileGpuDetileStatus::ContextMismatch;
	}
	if (result->unavailable)
	{
		return TileGpuDetileStatus::ResourceUnavailable;
	}
	if (!result->resources_ready && !CreateGpuDetileResources(ctx, result))
	{
		(void)DestroyGpuDetileResources(ctx, result);
		result->unavailable = true;
		return TileGpuDetileStatus::ResourceUnavailable;
	}

	*state = result;
	return TileGpuDetileStatus::Success;
}

TileGpuDetileStatus RecycleCompletedSubmission(GraphicContext* ctx, GpuDetileContext* state)
{
	if (ctx == nullptr || state == nullptr || state->device != ctx->device || state->fence == VK_NULL_HANDLE)
	{
		return TileGpuDetileStatus::ContextMismatch;
	}
	if (!state->in_flight)
	{
		return TileGpuDetileStatus::Success;
	}

	const auto status = vkGetFenceStatus(ctx->device, state->fence);
	if (status == VK_NOT_READY)
	{
		return TileGpuDetileStatus::ContextBusy;
	}
	if (status != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("tile_detile_fence_status", status);
		return TileGpuDetileStatus::FenceFailed;
	}
	// The fence is signaled, so no GPU work can still reference the session.
	// Keep it signaled until the next submit resets it immediately before use.
	state->in_flight = false;
	return TileGpuDetileStatus::Success;
}

void WriteBufferDescriptors(const GpuDetileContext& state, uint64_t tiled_bytes, uint64_t linear_bytes)
{
	VkDescriptorBufferInfo infos[2] {};
	infos[0].buffer = state.tiled.buffer;
	infos[0].range  = tiled_bytes;
	infos[1].buffer = state.linear.buffer;
	infos[1].range  = linear_bytes;
	VkWriteDescriptorSet write {};
	write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet          = state.descriptor_set;
	write.dstBinding      = 0;
	write.descriptorCount = 2;
	write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo     = infos;
	vkUpdateDescriptorSets(state.device, 1, &write, 0, nullptr);
}

void RecordHostReadbackBarrier(VkCommandBuffer command_buffer)
{
	// The diagnostic path maps the coherent linear allocation after the fence.
	// Make compute shader writes available and visible to that host read before
	// the command buffer completes.
	VkMemoryBarrier barrier {};
	barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0,
	                     nullptr);
}

TileGpuDetileStatus SubmitAndWait(GraphicContext* ctx, GpuDetileContext* state, const TileDetileRequest& request, uint64_t linear_bytes,
                                  uint32_t groups_x, uint32_t groups_y)
{
	if (ctx == nullptr || state == nullptr || state->device != ctx->device || state->queue == VK_NULL_HANDLE ||
	    state->queue_mutex == nullptr || state->command_buffer == VK_NULL_HANDLE || state->fence == VK_NULL_HANDLE)
	{
		return TileGpuDetileStatus::ContextMismatch;
	}
	if (state->in_flight)
	{
		return TileGpuDetileStatus::ContextBusy;
	}
	// Reset command state before recording. The fence remains signaled until
	// immediately before vkQueueSubmit so a failed record never loses its state.
	const auto command_reset_result = vkResetCommandBuffer(state->command_buffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	if (command_reset_result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("tile_detile_command_reset", command_reset_result);
		return TileGpuDetileStatus::SubmitFailed;
	}

	VkCommandBufferBeginInfo begin_info {};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	const auto begin_result = vkBeginCommandBuffer(state->command_buffer, &begin_info);
	if (begin_result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("tile_detile_command_begin", begin_result);
		return TileGpuDetileStatus::SubmitFailed;
	}

	DetilePushConstants push {};
	push.width             = request.width;
	push.height            = request.height;
	push.pitch_elems       = ResolvePitch(request.pitch_elems, request.width);
	push.dst_pitch_elems   = ResolvePitch(request.dst_pitch_elems, request.width);
	push.bytes_per_element = request.bytes_per_element;
	push.layout_kind       = LayoutKind(request.layout);
	push.src_u32_count     = static_cast<uint32_t>(request.src_bytes / 4u);
	push.dst_u32_count     = static_cast<uint32_t>(linear_bytes / 4u);
	vkCmdBindPipeline(state->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->pipeline);
	vkCmdBindDescriptorSets(state->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->pipeline_layout, 0, 1, &state->descriptor_set, 0,
	                        nullptr);
	vkCmdPushConstants(state->command_buffer, state->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch(state->command_buffer, groups_x, groups_y, 1);
	RecordHostReadbackBarrier(state->command_buffer);
	const auto end_result = vkEndCommandBuffer(state->command_buffer);
	if (end_result != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("tile_detile_command_end", end_result);
		return TileGpuDetileStatus::SubmitFailed;
	}

	VkSubmitInfo submit_info {};
	submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers    = &state->command_buffer;
	auto* trail = VulkanSubmitFaultTraceTrail();
	VulkanSubmitAttempt attempt {};
	VulkanSubmitAttempt observed {};
	if (trail != nullptr)
	{
		attempt.kind                = VulkanSubmitKind::TileDetile;
		attempt.queue               = static_cast<uint32_t>(GraphicContext::QUEUE_GFX);
		attempt.command_buffer_slot = UINT32_MAX;
		attempt.frame               = GraphicsRunGetFrameNum();
	}
	{
		Core::LockGuard queue_lock(*state->queue_mutex);
		const auto reset_result =
		    HasTestFault(TileGpuDetileTestFault::ResetFence) ? VK_ERROR_OUT_OF_HOST_MEMORY : vkResetFences(ctx->device, 1, &state->fence);
		if (reset_result != VK_SUCCESS)
		{
			VulkanSubmitFaultReport("tile_detile_fence_reset", reset_result);
			return TileGpuDetileStatus::FenceFailed;
		}
		const auto submit_result = VulkanTraceSubmitAttempt(
		    trail, attempt, [&] { return vkQueueSubmit(state->queue, 1, &submit_info, state->fence); }, &observed);
		if (submit_result != VK_SUCCESS)
		{
			VulkanSubmitFaultReport("tile_detile_submit", submit_result, trail != nullptr ? &observed : nullptr);
			return TileGpuDetileStatus::SubmitFailed;
		}
	}
	state->in_flight = true;

	if (HasTestFault(TileGpuDetileTestFault::WaitTimeout))
	{
		return TileGpuDetileStatus::FenceTimeout;
	}
	const auto wait = vkWaitForFences(ctx->device, 1, &state->fence, VK_TRUE, k_fence_timeout_ns);
	if (wait == VK_TIMEOUT)
	{
		return TileGpuDetileStatus::FenceTimeout;
	}
	if (wait != VK_SUCCESS)
	{
		VulkanSubmitFaultReport("tile_detile_fence_wait", wait);
		return TileGpuDetileStatus::FenceFailed;
	}
	state->in_flight = false;
	return TileGpuDetileStatus::Success;
}

} // namespace

bool TileGpuDetileImageCopyIsSupported(const TileDetileRequest& request, const TileGpuDetileImageCopy& copy, uint32_t image_width_texels,
                                       uint32_t image_height_texels)
{
	if (!GpuLayoutSupported(request) || copy.copy_width_texels == 0u || copy.copy_height_texels == 0u || copy.texels_per_element_x == 0u ||
	    copy.texels_per_element_y == 0u || image_width_texels == 0u || image_height_texels == 0u)
	{
		return false;
	}

	uint64_t element_width_texels  = 0;
	uint64_t element_height_texels = 0;
	uint64_t row_length_texels     = 0;
	if (!CheckedMultiply(request.width, copy.texels_per_element_x, &element_width_texels) ||
	    !CheckedMultiply(request.height, copy.texels_per_element_y, &element_height_texels) ||
	    !CheckedMultiply(ResolvePitch(request.dst_pitch_elems, request.width), copy.texels_per_element_x, &row_length_texels) ||
	    element_width_texels > UINT32_MAX || element_height_texels > UINT32_MAX || row_length_texels > UINT32_MAX ||
	    copy.copy_width_texels > element_width_texels || copy.copy_height_texels > element_height_texels ||
	    copy.copy_width_texels > image_width_texels || copy.copy_height_texels > image_height_texels)
	{
		return false;
	}

	uint32_t used_width_elements  = 0;
	uint32_t used_height_elements = 0;
	if (!DivideRoundUp(copy.copy_width_texels, copy.texels_per_element_x, &used_width_elements) ||
	    !DivideRoundUp(copy.copy_height_texels, copy.texels_per_element_y, &used_height_elements) || used_width_elements != request.width ||
	    used_height_elements != request.height)
	{
		return false;
	}
	if (copy.texels_per_element_x == 4u && copy.texels_per_element_y == 4u)
	{
		uint64_t dst_pitch_texels = 0;
		if (!CheckedMultiply(ResolvePitch(request.dst_pitch_elems, request.width), 4u, &dst_pitch_texels) ||
		    dst_pitch_texels > UINT32_MAX)
		{
			return false;
		}
		TileBc1BufferCopyLayout bc1_layout {};
		if (!TileGetBc1BufferCopyLayout(copy.copy_width_texels, copy.copy_height_texels, static_cast<uint32_t>(dst_pitch_texels),
		                                &bc1_layout) ||
		    bc1_layout.copy_width_blocks != used_width_elements || bc1_layout.copy_height_blocks != used_height_elements)
		{
			return false;
		}
		if (copy.buffer_row_length_texels == 0u)
		{
			return bc1_layout.row_pitch_blocks == bc1_layout.copy_width_blocks;
		}
		return copy.buffer_row_length_texels == bc1_layout.buffer_row_length_texels;
	}

	// A zero row length means Vulkan derives the tight row from the image
	// extent. For compressed copies that is ceil(copy_width / block_width), so
	// compare the element pitch rather than raw texel widths.
	if (copy.buffer_row_length_texels == 0u)
	{
		return ResolvePitch(request.dst_pitch_elems, request.width) == used_width_elements;
	}
	return copy.buffer_row_length_texels == row_length_texels && (copy.buffer_row_length_texels % copy.texels_per_element_x) == 0u;
}

bool TileGpuDetileDepthD16InlineIsSupported(uint64_t source_offset, uint64_t source_range, uint32_t width, uint32_t height,
	                                        uint32_t pitch_elems, uint64_t* required_source_bytes, uint64_t* linear_bytes)
{
	uint64_t blocks_y = 0;
	uint64_t required = 0;
	uint64_t linear   = 0;
	if (source_range == 0u || (source_range % 4u) != 0u || width == 0u || height == 0u || pitch_elems < width ||
	    (pitch_elems % 256u) != 0u || !CheckedAdd(height, 127u, &blocks_y))
	{
		return false;
	}
	blocks_y /= 128u;
	if (!CheckedMultiply(pitch_elems / 256u, blocks_y, &required) || !CheckedMultiply(required, 65536u, &required) ||
	    !CheckedMultiply(width, height, &linear) || !CheckedMultiply(linear, 2u, &linear) || required == 0u || linear == 0u ||
	    required > k_max_diagnostic_session_bytes || linear > k_max_diagnostic_session_bytes || source_offset > source_range ||
	    required > source_range - source_offset || (source_offset % 4u) != 0u || source_offset > UINT32_MAX ||
	    source_range > k_max_shader_byte_range || linear > k_max_shader_byte_range)
	{
		return false;
	}
	if (required_source_bytes != nullptr)
	{
		*required_source_bytes = required;
	}
	if (linear_bytes != nullptr)
	{
		*linear_bytes = linear;
	}
	return true;
}

void TileGpuDetileSetTestFaultForTesting(TileGpuDetileTestFault fault)
{
	g_test_fault.store(fault, std::memory_order_relaxed);
}

TileGpuDetileStatus TileGpuDetile(GraphicContext* ctx, const TileDetileRequest& request)
{
	if (request.src == nullptr || request.dst == nullptr || !GpuLayoutSupported(request))
	{
		return TileGpuDetileStatus::InvalidRequest;
	}
	if (ctx == nullptr || ctx->device == VK_NULL_HANDLE || ctx->physical_device == VK_NULL_HANDLE)
	{
		return TileGpuDetileStatus::ContextUnavailable;
	}

	uint64_t linear_bytes = 0;
	uint32_t groups_x     = 0;
	uint32_t groups_y     = 0;
	if (!CalculateGpuLinearBytes(request, &linear_bytes))
	{
		return TileGpuDetileStatus::InvalidRequest;
	}
	uint64_t session_bytes = 0;
	if (!CheckedAdd(request.src_bytes, linear_bytes, &session_bytes) || session_bytes > k_max_diagnostic_session_bytes)
	{
		return TileGpuDetileStatus::DiagnosticCapacityExceeded;
	}
	if (!ValidateDeviceLimits(ctx, request, linear_bytes, &groups_x, &groups_y))
	{
		return TileGpuDetileStatus::DeviceUnsupported;
	}

	Core::LockGuard lock(ctx->gpu_detile_mutex);
	GpuDetileContext* state  = nullptr;
	auto              status = GetGpuDetileContext(ctx, &state);
	if (status != TileGpuDetileStatus::Success)
	{
		return status;
	}
	status = RecycleCompletedSubmission(ctx, state);
	if (status != TileGpuDetileStatus::Success)
	{
		return status;
	}
	// Buffers are retained for the owning context, so the cap applies to the
	// prospective pair of capacities rather than this individual request alone.
	if (!FitsDiagnosticRetainedCapacity(*state, request.src_bytes, linear_bytes))
	{
		return TileGpuDetileStatus::DiagnosticCapacityExceeded;
	}
	if (!EnsureHostStorageBuffer(ctx, &state->tiled, &state->tiled_capacity, request.src_bytes) ||
	    !EnsureHostStorageBuffer(ctx, &state->linear, &state->linear_capacity, linear_bytes))
	{
		return TileGpuDetileStatus::ResourceUnavailable;
	}
	if (!CopyHostToBuffer(ctx, request.src, &state->tiled, request.src_bytes) ||
	    !CopyHostToBuffer(ctx, request.dst, &state->linear, linear_bytes))
	{
		return TileGpuDetileStatus::UploadFailed;
	}
	WriteBufferDescriptors(*state, request.src_bytes, linear_bytes);

	status = SubmitAndWait(ctx, state, request, linear_bytes, groups_x, groups_y);
	if (status != TileGpuDetileStatus::Success)
	{
		return status;
	}
	if (!CopyBufferToHost(ctx, &state->linear, request.dst, linear_bytes))
	{
		(void)RecycleCompletedSubmission(ctx, state);
		return TileGpuDetileStatus::ReadbackFailed;
	}
	const auto recycle_status = RecycleCompletedSubmission(ctx, state);
	return recycle_status == TileGpuDetileStatus::Success ? TileGpuDetileStatus::Success : recycle_status;
}

TileGpuDetileStatus TileGpuDetileToImage(GraphicContext* ctx, const TileDetileRequest& request, VulkanImage* dst_image,
                                         const TileGpuDetileImageCopy& copy, uint64_t dst_layout)
{
	static_cast<void>(dst_layout);
	if (request.src == nullptr || dst_image == nullptr || dst_image->image == VK_NULL_HANDLE || !GpuLayoutSupported(request) ||
	    !TileGpuDetileImageCopyIsSupported(request, copy, dst_image->extent.width, dst_image->extent.height))
	{
		return TileGpuDetileStatus::InvalidRequest;
	}
	if (ctx == nullptr || ctx->device == VK_NULL_HANDLE || ctx->physical_device == VK_NULL_HANDLE)
	{
		return TileGpuDetileStatus::ContextUnavailable;
	}
	return TileGpuDetileStatus::ImagePathUnsupported;
}

TileGpuDetileStatus TileGpuDetileDepthD16Inline(GraphicContext* ctx, CommandBuffer* command_buffer,
	                                            const VulkanBuffer* source_buffer, uint64_t source_offset,
	                                            uint64_t source_range, VulkanImage* dst_image, uint32_t width,
	                                            uint32_t height, uint32_t pitch_elems)
{
	uint64_t required_source = 0;
	uint64_t linear_bytes    = 0;
	if (!TileGpuDetileDepthD16InlineIsSupported(source_offset, source_range, width, height, pitch_elems, &required_source,
	                                           &linear_bytes) ||
	    command_buffer == nullptr || command_buffer->IsInvalid() || source_buffer == nullptr || source_buffer->buffer == VK_NULL_HANDLE ||
	    (source_buffer->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0u || dst_image == nullptr ||
	    dst_image->image == VK_NULL_HANDLE || dst_image->format != VK_FORMAT_D16_UNORM || dst_image->extent.width != width ||
	    dst_image->extent.height != height || (dst_image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0u)
	{
		return TileGpuDetileStatus::InvalidRequest;
	}
	if (ctx == nullptr || ctx->device == VK_NULL_HANDLE || ctx->physical_device == VK_NULL_HANDLE)
	{
		return TileGpuDetileStatus::ContextUnavailable;
	}
	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(ctx->physical_device, &properties);
	if (source_range > properties.limits.maxStorageBufferRange || linear_bytes > properties.limits.maxStorageBufferRange ||
	    properties.limits.maxComputeWorkGroupSize[0] < 8u || properties.limits.maxComputeWorkGroupSize[1] < 8u ||
	    properties.limits.maxComputeWorkGroupInvocations < 64u)
	{
		return TileGpuDetileStatus::DeviceUnsupported;
	}
	const uint32_t groups_x = width / 8u + (width % 8u == 0u ? 0u : 1u);
	const uint32_t groups_y = height / 8u + (height % 8u == 0u ? 0u : 1u);
	if (groups_x > properties.limits.maxComputeWorkGroupCount[0] || groups_y > properties.limits.maxComputeWorkGroupCount[1])
	{
		return TileGpuDetileStatus::DeviceUnsupported;
	}
	const uint64_t scratch_bytes = (linear_bytes + 3u) & ~3ull;
	auto* scratch = command_buffer->AllocateTransientScratchBuffer(
	    scratch_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	if (scratch == nullptr || scratch->buffer == VK_NULL_HANDLE)
	{
		return TileGpuDetileStatus::ResourceUnavailable;
	}

	Core::LockGuard lock(ctx->gpu_detile_mutex);
	GpuDetileContext* state = nullptr;
	auto status = GetGpuDetileContext(ctx, &state);
	if (status != TileGpuDetileStatus::Success || state == nullptr || !state->resources_ready)
	{
		return status;
	}

	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num = 2;
	ShaderCalcBindingIndices(&bind);
	VulkanBuffer* descriptor_buffers[2]                                    = {const_cast<VulkanBuffer*>(source_buffer), scratch};
	VulkanImage*  no_images[DescriptorCache::TEXTURES_SAMPLED_MAX]          = {};
	int           no_views[DescriptorCache::TEXTURES_SAMPLED_MAX]           = {};
	VulkanImage*  no_storage_images[DescriptorCache::TEXTURES_STORAGE_MAX]  = {};
	int           no_storage_views[DescriptorCache::TEXTURES_STORAGE_MAX]   = {};
	uint64_t      no_samplers[DescriptorCache::SAMPLERS_MAX]                = {};
	VulkanBuffer* no_gds[DescriptorCache::GDS_BUFFER_MAX]                   = {};
	auto* descriptor = g_render_ctx->GetDescriptorCache()->GetDescriptor(
	    DescriptorCache::Stage::Compute, descriptor_buffers, no_images, no_views, no_images, no_views, no_images, no_views,
	    no_images, no_views, no_images, no_views, no_images, no_views, no_images, no_views, no_storage_images, no_storage_views,
	    no_samplers, no_gds, nullptr, bind);
	if (descriptor == nullptr || descriptor->set == VK_NULL_HANDLE)
	{
		return TileGpuDetileStatus::ResourceUnavailable;
	}

	auto vk_buffer = command_buffer->GetPool()->buffers[command_buffer->GetIndex()];
	// The command buffer deliberately reuses one bounded scratch allocation.
	// Complete any earlier shader/copy reads before the next transfer clear.
	VkMemoryBarrier before_clear {};
	before_clear.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	before_clear.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	before_clear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 1u, &before_clear, 0u,
	                     nullptr, 0u, nullptr);
	vkCmdFillBuffer(vk_buffer, scratch->buffer, 0u, scratch_bytes, 0u);
	VkMemoryBarrier before_compute {};
	before_compute.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	before_compute.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	before_compute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u,
	                     &before_compute, 0u, nullptr, 0u, nullptr);

	DetilePushConstants push {};
	push.width             = width;
	push.height            = height;
	push.pitch_elems       = pitch_elems;
	push.dst_pitch_elems   = width;
	push.bytes_per_element = 2u;
	push.layout_kind       = LayoutKind(TileDetileLayout::Depth64KB);
	push.src_u32_count     = static_cast<uint32_t>(source_range / 4u);
	push.dst_u32_count     = static_cast<uint32_t>(scratch_bytes / 4u);
	push.src_byte_offset   = static_cast<uint32_t>(source_offset);
	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->pipeline);
	vkCmdBindDescriptorSets(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->pipeline_layout, 0u, 1u, &descriptor->set, 0u, nullptr);
	vkCmdPushConstants(vk_buffer, state->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(push), &push);
	vkCmdDispatch(vk_buffer, groups_x, groups_y, 1u);
	VkMemoryBarrier before_copy {};
	before_copy.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	before_copy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	before_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 1u, &before_copy, 0u,
	                     nullptr, 0u, nullptr);
	UtilBufferToDepthImage(command_buffer, scratch, width, dst_image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
	return TileGpuDetileStatus::Success;
}

bool TileGpuDetileReleaseContext(GraphicContext* ctx)
{
	if (ctx == nullptr)
	{
		return false;
	}
	Core::LockGuard lock(ctx->gpu_detile_mutex);
	if (ctx->gpu_detile_context == nullptr)
	{
		return true;
	}
	auto* state = ctx->gpu_detile_context;
	if (ctx->device == VK_NULL_HANDLE || state->device != ctx->device)
	{
		return false;
	}
	if (state->in_flight)
	{
		if (state->fence == VK_NULL_HANDLE)
		{
			return false;
		}
		if (HasTestFault(TileGpuDetileTestFault::ReleaseTimeout))
		{
			return false;
		}
		const auto wait = vkWaitForFences(ctx->device, 1, &state->fence, VK_TRUE, k_release_fence_timeout_ns);
		if (wait != VK_SUCCESS)
		{
			VulkanSubmitFaultReport("tile_detile_release_wait", wait);
			return false;
		}
		state->in_flight = false;
	}
	if (!DestroyGpuDetileResources(ctx, state))
	{
		return false;
	}
	delete state;
	ctx->gpu_detile_context = nullptr;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif
