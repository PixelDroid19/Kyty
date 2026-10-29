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

struct DispatchIndirectArgsPush
{
	uint32_t source_word = 0;
	uint32_t max_x       = 0;
	uint32_t max_y       = 0;
	uint32_t max_z       = 0;
};

// One pipeline and one sticky limit flag for the process; the converter runs under the render mutex.
struct DispatchIndirectConverter
{
	bool                  attempted       = false;
	bool                  ready           = false;
	VkShaderModule        module          = VK_NULL_HANDLE;
	VkPipelineLayout      pipeline_layout = VK_NULL_HANDLE;
	VkPipeline            pipeline        = VK_NULL_HANDLE;
	VulkanBuffer          limit_flag;
	std::atomic<uint32_t*> limit_flag_word {nullptr};
};

DispatchIndirectConverter g_converter;

ShaderBindResources ConverterBindings()
{
	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num = 3;
	ShaderCalcBindingIndices(&bind);
	// The host-authored converter declares no write mask.
	bind.storage_buffers.write_mask_binding_index = -1;
	return bind;
}

bool CreateConverter(GraphicContext* ctx)
{
	VkShaderModuleCreateInfo shader_info {};
	shader_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader_info.codeSize = sizeof(kDispatchIndirectArgsSpirv);
	shader_info.pCode    = kDispatchIndirectArgsSpirv;
	if (vkCreateShaderModule(ctx->device, &shader_info, nullptr, &g_converter.module) != VK_SUCCESS)
	{
		return false;
	}

	const VkDescriptorSetLayout set_layout =
	    g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(DescriptorCache::Stage::Compute, ConverterBindings());
	VkPushConstantRange push_range {};
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.size       = sizeof(DispatchIndirectArgsPush);
	VkPipelineLayoutCreateInfo layout_info {};
	layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	if (set_layout == VK_NULL_HANDLE || vkCreatePipelineLayout(ctx->device, &layout_info, nullptr, &g_converter.pipeline_layout) != VK_SUCCESS)
	{
		return false;
	}

	VkComputePipelineCreateInfo pipeline_info {};
	pipeline_info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = g_converter.module;
	pipeline_info.stage.pName  = "main";
	pipeline_info.layout       = g_converter.pipeline_layout;
	if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &g_converter.pipeline) != VK_SUCCESS)
	{
		return false;
	}

	g_converter.limit_flag.usage           = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	g_converter.limit_flag.memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VulkanCreateBuffer(ctx, sizeof(uint32_t), &g_converter.limit_flag);
	void* flag = nullptr;
	VulkanMapMemory(ctx, &g_converter.limit_flag.memory, &flag);
	if (g_converter.limit_flag.buffer == VK_NULL_HANDLE || flag == nullptr)
	{
		return false;
	}
	std::memset(flag, 0, sizeof(uint32_t));
	g_converter.limit_flag_word.store(static_cast<uint32_t*>(flag), std::memory_order_release);
	return true;
}

} // namespace

VulkanBuffer* GraphicsRenderRecordDispatchIndirectArgs(CommandBuffer* buffer, const VulkanBuffer* source, uint64_t offset,
                                                       const uint32_t* max_group_count)
{
	EXIT_IF(buffer == nullptr || source == nullptr || max_group_count == nullptr);
	if ((offset & 3u) != 0u || offset / 4u > UINT32_MAX - 2u || source->descriptor_range < 12u || offset > source->descriptor_range - 12u)
	{
		return nullptr;
	}
	auto* ctx = g_render_ctx->GetGraphicCtx();
	if (!g_converter.attempted)
	{
		g_converter.attempted = true;
		g_converter.ready     = CreateConverter(ctx);
	}
	if (!g_converter.ready)
	{
		return nullptr;
	}
	auto* args = buffer->AllocateTransientScratchBuffer(3u * sizeof(uint32_t),
	                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
	if (args == nullptr || args->buffer == VK_NULL_HANDLE)
	{
		return nullptr;
	}

	const auto    bind                  = ConverterBindings();
	VulkanBuffer* descriptor_buffers[3] = {const_cast<VulkanBuffer*>(source), args, &g_converter.limit_flag};
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
		return nullptr;
	}

	auto* vk_buffer = buffer->RecordOutsideRenderPass();
	// The guest record is read after every earlier write of this queue, and the one
	// argument scratch of this recording is rewritten only after its last indirect read.
	VkMemoryBarrier before {};
	before.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0,
	                     nullptr);

	DispatchIndirectArgsPush push;
	push.source_word = static_cast<uint32_t>(offset / 4u);
	push.max_x       = max_group_count[0];
	push.max_y       = max_group_count[1];
	push.max_z       = max_group_count[2];
	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_converter.pipeline);
	vkCmdBindDescriptorSets(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_converter.pipeline_layout, 0, 1, &descriptor->set, 0, nullptr);
	vkCmdPushConstants(vk_buffer, g_converter.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch(vk_buffer, 1, 1, 1);

	VkMemoryBarrier after {};
	after.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	after.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &after, 0, nullptr, 0,
	                     nullptr);
	return args;
}

void GraphicsRenderCheckDispatchIndirectLimits()
{
	const uint32_t* flag = g_converter.limit_flag_word.load(std::memory_order_acquire);
	if (flag != nullptr && __atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0u)
	{
		EXIT("device-side DISPATCH_INDIRECT group count exceeds the host limit\n");
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
