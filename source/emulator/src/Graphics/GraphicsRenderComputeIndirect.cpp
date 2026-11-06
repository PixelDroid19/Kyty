#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Utils.h"
#include "GraphicsRenderInternal.h"

#include <atomic>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

#include "host_shaders/dispatch_indirect_args_comp.inc"
#include "host_shaders/draw_indexed_indirect_args_comp.inc"

struct DispatchIndirectArgsPush
{
	uint32_t source_word = 0;
	uint32_t max_x       = 0;
	uint32_t max_y       = 0;
	uint32_t max_z       = 0;
};

struct DrawIndexedIndirectArgsPush
{
	uint32_t source_word        = 0;
	uint32_t index_window       = 0;
	int32_t  vertex_offset      = 0;
	uint32_t latch_word         = 0;
	uint32_t previous_instances = 0;
};

// A one-invocation kernel that converts a guest record into host indirect arguments, with
// host-visible words it reports through. Created once per process; it runs under the render mutex.
struct ArgumentConverter
{
	bool                   attempted       = false;
	bool                   ready           = false;
	VkShaderModule         module          = VK_NULL_HANDLE;
	VkPipelineLayout       pipeline_layout = VK_NULL_HANDLE;
	VkPipeline             pipeline        = VK_NULL_HANDLE;
	VulkanBuffer           host_words;
	std::atomic<uint32_t*> host_word {nullptr};
};

// Host words: the sticky limit flag.
ArgumentConverter g_dispatch_converter;
// Host words: the NUM_INSTANCES latch of each queue.
ArgumentConverter g_draw_converter;

ShaderBindResources ConverterBindings()
{
	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num = 3;
	ShaderCalcBindingIndices(&bind);
	// The host-authored converter declares no write mask.
	bind.storage_buffers.write_mask_binding_index = -1;
	return bind;
}

bool CreateConverter(GraphicContext* ctx, const uint32_t* code, size_t code_bytes, uint32_t push_bytes, uint32_t host_words,
                     ArgumentConverter* converter)
{
	VkShaderModuleCreateInfo shader_info {};
	shader_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader_info.codeSize = code_bytes;
	shader_info.pCode    = code;
	if (vkCreateShaderModule(ctx->device, &shader_info, nullptr, &converter->module) != VK_SUCCESS)
	{
		return false;
	}

	const VkDescriptorSetLayout set_layout =
	    g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(DescriptorCache::Stage::Compute, ConverterBindings());
	VkPushConstantRange push_range {};
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.size       = push_bytes;
	VkPipelineLayoutCreateInfo layout_info {};
	layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	if (set_layout == VK_NULL_HANDLE ||
	    vkCreatePipelineLayout(ctx->device, &layout_info, nullptr, &converter->pipeline_layout) != VK_SUCCESS)
	{
		return false;
	}

	VkComputePipelineCreateInfo pipeline_info {};
	pipeline_info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = converter->module;
	pipeline_info.stage.pName  = "main";
	pipeline_info.layout       = converter->pipeline_layout;
	if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &converter->pipeline) != VK_SUCCESS)
	{
		return false;
	}

	converter->host_words.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	converter->host_words.memory.property =
	    static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VulkanCreateBuffer(ctx, host_words * sizeof(uint32_t), &converter->host_words);
	void* words = nullptr;
	VulkanMapMemory(ctx, &converter->host_words.memory, &words);
	if (converter->host_words.buffer == VK_NULL_HANDLE || words == nullptr)
	{
		return false;
	}
	std::memset(words, 0, host_words * sizeof(uint32_t));
	converter->host_word.store(static_cast<uint32_t*>(words), std::memory_order_release);
	return true;
}

bool ConverterReady(const uint32_t* code, size_t code_bytes, uint32_t push_bytes, uint32_t host_words, ArgumentConverter* converter)
{
	if (!converter->attempted)
	{
		converter->attempted = true;
		converter->ready     = CreateConverter(g_render_ctx->GetGraphicCtx(), code, code_bytes, push_bytes, host_words, converter);
	}
	return converter->ready;
}

// Records the conversion outside the render pass. The guest record is read after every earlier
// write of this queue, and the one argument scratch of a recording is rewritten only after its
// last indirect read; the host words are read once the recording's fence completes.
bool RecordConverter(CommandBuffer* buffer, const ArgumentConverter& converter, const VulkanBuffer* source, VulkanBuffer* args,
                     const void* push, uint32_t push_bytes)
{
	const auto    bind                  = ConverterBindings();
	VulkanBuffer* descriptor_buffers[3] = {const_cast<VulkanBuffer*>(source), args, const_cast<VulkanBuffer*>(&converter.host_words)};
	VulkanImage*  no_images[DescriptorCache::TEXTURES_SAMPLED_MAX]         = {};
	int           no_views[DescriptorCache::TEXTURES_SAMPLED_MAX]          = {};
	VulkanImage*  no_storage_images[DescriptorCache::TEXTURES_STORAGE_MAX] = {};
	int           no_storage_views[DescriptorCache::TEXTURES_STORAGE_MAX]  = {};
	uint64_t      no_samplers[DescriptorCache::SAMPLERS_MAX]               = {};
	VulkanBuffer* no_gds[DescriptorCache::GDS_BUFFER_MAX]                  = {};
	auto* descriptor = g_render_ctx->GetDescriptorCache()->GetDescriptor(
	    buffer, DescriptorCache::Stage::Compute, descriptor_buffers, no_images, no_views, no_images, no_views, no_images, no_views,
	    no_images, no_views, no_images, no_views, no_images, no_views, no_images, no_views, no_storage_images, no_storage_views,
	    no_samplers, no_gds, nullptr, bind);
	if (descriptor == nullptr || descriptor->set == VK_NULL_HANDLE)
	{
		return false;
	}

	auto*           vk_buffer = buffer->RecordOutsideRenderPass();
	VkMemoryBarrier before {};
	before.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0,
	                     nullptr);

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, converter.pipeline);
	vkCmdBindDescriptorSets(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, converter.pipeline_layout, 0, 1, &descriptor->set, 0, nullptr);
	vkCmdPushConstants(vk_buffer, converter.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
	vkCmdDispatch(vk_buffer, 1, 1, 1);

	VkMemoryBarrier after {};
	after.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	after.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_HOST_BIT,
	                     0, 1, &after, 0, nullptr, 0, nullptr);
	return true;
}

bool RecordRangeValid(const VulkanBuffer* source, uint64_t offset, uint32_t record_words)
{
	const uint64_t record_bytes = static_cast<uint64_t>(record_words) * sizeof(uint32_t);
	return (offset & 3u) == 0u && offset / 4u <= UINT32_MAX - (record_words - 1u) && source->descriptor_range >= record_bytes &&
	       offset <= source->descriptor_range - record_bytes;
}

} // namespace

VulkanBuffer* GraphicsRenderRecordDispatchIndirectArgs(CommandBuffer* buffer, const VulkanBuffer* source, uint64_t offset,
                                                       const uint32_t* max_group_count)
{
	EXIT_IF(buffer == nullptr || source == nullptr || max_group_count == nullptr);
	if (!RecordRangeValid(source, offset, 3u) || !ConverterReady(kDispatchIndirectArgsSpirv, sizeof(kDispatchIndirectArgsSpirv),
	                                                             sizeof(DispatchIndirectArgsPush), 1u, &g_dispatch_converter))
	{
		return nullptr;
	}
	auto* args = buffer->AllocateTransientScratchBuffer(3u * sizeof(uint32_t),
	                                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
	if (args == nullptr || args->buffer == VK_NULL_HANDLE)
	{
		return nullptr;
	}

	DispatchIndirectArgsPush push;
	push.source_word = static_cast<uint32_t>(offset / 4u);
	push.max_x       = max_group_count[0];
	push.max_y       = max_group_count[1];
	push.max_z       = max_group_count[2];
	return RecordConverter(buffer, g_dispatch_converter, source, args, &push, sizeof(push)) ? args : nullptr;
}

void GraphicsRenderCheckDispatchIndirectLimits()
{
	const uint32_t* flag = g_dispatch_converter.host_word.load(std::memory_order_acquire);
	if (flag != nullptr && __atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0u)
	{
		EXIT("device-side DISPATCH_INDIRECT group count exceeds the host limit\n");
	}
}

VulkanBuffer* GraphicsRenderRecordDrawIndexedIndirectArgs(CommandBuffer* buffer, const VulkanBuffer* source, uint64_t offset,
                                                          uint32_t index_window, int32_t vertex_offset, uint32_t latch_queue,
                                                          uint32_t previous_instances)
{
	EXIT_IF(buffer == nullptr || source == nullptr || latch_queue >= static_cast<uint32_t>(GraphicContext::QUEUES_NUM));
	if (!RecordRangeValid(source, offset, 5u) ||
	    !ConverterReady(kDrawIndexedIndirectArgsSpirv, sizeof(kDrawIndexedIndirectArgsSpirv), sizeof(DrawIndexedIndirectArgsPush),
	                    static_cast<uint32_t>(GraphicContext::QUEUES_NUM), &g_draw_converter))
	{
		return nullptr;
	}
	auto* args = buffer->AllocateTransientScratchBuffer(sizeof(VkDrawIndexedIndirectCommand),
	                                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
	if (args == nullptr || args->buffer == VK_NULL_HANDLE)
	{
		return nullptr;
	}

	DrawIndexedIndirectArgsPush push;
	push.source_word        = static_cast<uint32_t>(offset / 4u);
	push.index_window       = index_window;
	push.vertex_offset      = vertex_offset;
	push.latch_word         = latch_queue;
	push.previous_instances = previous_instances;
	return RecordConverter(buffer, g_draw_converter, source, args, &push, sizeof(push)) ? args : nullptr;
}

uint32_t GraphicsRenderReadDrawIndexedIndirectInstances(uint32_t latch_queue)
{
	const uint32_t* words = g_draw_converter.host_word.load(std::memory_order_acquire);
	EXIT_IF(words == nullptr || latch_queue >= static_cast<uint32_t>(GraphicContext::QUEUES_NUM));
	return __atomic_load_n(words + latch_queue, __ATOMIC_ACQUIRE);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
