#include "Emulator/Graphics/DepthStencilCopy.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/Utils.h"

#include <cstddef>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr size_t kMaxSourceDescriptors = 256;
constexpr size_t kMaxRenderPipelines   = 64;

// Embedded Vulkan SPIR-V for the fixed-function expansion pipeline.
constexpr uint32_t kVertexShader[] = {
	0x07230203u, 0x00010000u, 0x0008000bu, 0x0000002cu, 0x00000000u, 0x00020011u,
	0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
	0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0007000fu, 0x00000000u,
	0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000cu, 0x00000022u, 0x00030003u,
	0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
	0x00060005u, 0x0000000cu, 0x565f6c67u, 0x65747265u, 0x646e4978u, 0x00007865u,
	0x00060005u, 0x00000020u, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u,
	0x00060006u, 0x00000020u, 0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u,
	0x00070006u, 0x00000020u, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u,
	0x00000000u, 0x00070006u, 0x00000020u, 0x00000002u, 0x435f6c67u, 0x4470696cu,
	0x61747369u, 0x0065636eu, 0x00070006u, 0x00000020u, 0x00000003u, 0x435f6c67u,
	0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u, 0x00000022u, 0x00000000u,
	0x00040047u, 0x0000000cu, 0x0000000bu, 0x0000002au, 0x00030047u, 0x00000020u,
	0x00000002u, 0x00050048u, 0x00000020u, 0x00000000u, 0x0000000bu, 0x00000000u,
	0x00050048u, 0x00000020u, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u,
	0x00000020u, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x00000020u,
	0x00000003u, 0x0000000bu, 0x00000004u, 0x00020013u, 0x00000002u, 0x00030021u,
	0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u,
	0x00000007u, 0x00000006u, 0x00000002u, 0x00040015u, 0x0000000au, 0x00000020u,
	0x00000001u, 0x00040020u, 0x0000000bu, 0x00000001u, 0x0000000au, 0x0004003bu,
	0x0000000bu, 0x0000000cu, 0x00000001u, 0x0004002bu, 0x0000000au, 0x0000000eu,
	0x00000001u, 0x0004002bu, 0x0000000au, 0x00000010u, 0x00000002u, 0x0004002bu,
	0x00000006u, 0x00000017u, 0x40000000u, 0x0004002bu, 0x00000006u, 0x00000019u,
	0x3f800000u, 0x00040017u, 0x0000001cu, 0x00000006u, 0x00000004u, 0x00040015u,
	0x0000001du, 0x00000020u, 0x00000000u, 0x0004002bu, 0x0000001du, 0x0000001eu,
	0x00000001u, 0x0004001cu, 0x0000001fu, 0x00000006u, 0x0000001eu, 0x0006001eu,
	0x00000020u, 0x0000001cu, 0x00000006u, 0x0000001fu, 0x0000001fu, 0x00040020u,
	0x00000021u, 0x00000003u, 0x00000020u, 0x0004003bu, 0x00000021u, 0x00000022u,
	0x00000003u, 0x0004002bu, 0x0000000au, 0x00000023u, 0x00000000u, 0x0004002bu,
	0x00000006u, 0x00000025u, 0x00000000u, 0x00040020u, 0x00000029u, 0x00000003u,
	0x0000001cu, 0x0005002cu, 0x00000007u, 0x0000002bu, 0x00000019u, 0x00000019u,
	0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
	0x00000005u, 0x0004003du, 0x0000000au, 0x0000000du, 0x0000000cu, 0x000500c4u,
	0x0000000au, 0x0000000fu, 0x0000000du, 0x0000000eu, 0x000500c7u, 0x0000000au,
	0x00000011u, 0x0000000fu, 0x00000010u, 0x0004006fu, 0x00000006u, 0x00000012u,
	0x00000011u, 0x000500c7u, 0x0000000au, 0x00000014u, 0x0000000du, 0x00000010u,
	0x0004006fu, 0x00000006u, 0x00000015u, 0x00000014u, 0x00050050u, 0x00000007u,
	0x00000016u, 0x00000012u, 0x00000015u, 0x0005008eu, 0x00000007u, 0x00000018u,
	0x00000016u, 0x00000017u, 0x00050083u, 0x00000007u, 0x0000001bu, 0x00000018u,
	0x0000002bu, 0x00050051u, 0x00000006u, 0x00000026u, 0x0000001bu, 0x00000000u,
	0x00050051u, 0x00000006u, 0x00000027u, 0x0000001bu, 0x00000001u, 0x00070050u,
	0x0000001cu, 0x00000028u, 0x00000026u, 0x00000027u, 0x00000025u, 0x00000019u,
	0x00050041u, 0x00000029u, 0x0000002au, 0x00000022u, 0x00000023u, 0x0003003eu,
	0x0000002au, 0x00000028u, 0x000100fdu, 0x00010038u,
};

constexpr uint32_t kFragmentShader[] = {
	0x07230203u, 0x00010000u, 0x0008000bu, 0x0000003eu, 0x00000000u, 0x00020011u,
	0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
	0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0007000fu, 0x00000004u,
	0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000034u, 0x00030010u,
	0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u,
	0x00000004u, 0x6e69616du, 0x00000000u, 0x00060005u, 0x0000000du, 0x465f6c67u,
	0x43676172u, 0x64726f6fu, 0x00000000u, 0x00060005u, 0x00000011u, 0x79706f43u,
	0x61726150u, 0x6574656du, 0x00007372u, 0x00070006u, 0x00000011u, 0x00000000u,
	0x72756f73u, 0x735f6563u, 0x656c6163u, 0x00000000u, 0x00050005u, 0x00000013u,
	0x61726170u, 0x6574656du, 0x00007372u, 0x00060005u, 0x0000001fu, 0x74706564u,
	0x65745f68u, 0x72757478u, 0x00000065u, 0x00060005u, 0x0000002cu, 0x6e657473u,
	0x5f6c6963u, 0x74786574u, 0x00657275u, 0x00040005u, 0x00000034u, 0x6f6c6f63u,
	0x00000072u, 0x00040047u, 0x0000000du, 0x0000000bu, 0x0000000fu, 0x00030047u,
	0x00000011u, 0x00000002u, 0x00050048u, 0x00000011u, 0x00000000u, 0x00000023u,
	0x00000000u, 0x00040047u, 0x0000001fu, 0x00000021u, 0x00000000u, 0x00040047u,
	0x0000001fu, 0x00000022u, 0x00000000u, 0x00040047u, 0x0000002cu, 0x00000021u,
	0x00000001u, 0x00040047u, 0x0000002cu, 0x00000022u, 0x00000000u, 0x00040047u,
	0x00000034u, 0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u,
	0x00000003u, 0x00000002u, 0x00040015u, 0x00000006u, 0x00000020u, 0x00000001u,
	0x00040017u, 0x00000007u, 0x00000006u, 0x00000002u, 0x00030016u, 0x0000000au,
	0x00000020u,
	0x00040017u, 0x0000000bu, 0x0000000au, 0x00000004u, 0x00040020u, 0x0000000cu,
	0x00000001u, 0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000001u,
	0x00040017u, 0x0000000eu, 0x0000000au, 0x00000002u, 0x0003001eu, 0x00000011u,
	0x0000000eu, 0x00040020u, 0x00000012u, 0x00000009u, 0x00000011u, 0x0004003bu,
	0x00000012u, 0x00000013u, 0x00000009u, 0x0004002bu, 0x00000006u, 0x00000014u,
	0x00000000u, 0x00040020u, 0x00000015u, 0x00000009u, 0x0000000eu, 0x00090019u,
	0x0000001cu, 0x0000000au, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000001u, 0x00000000u, 0x0003001bu, 0x0000001du, 0x0000001cu, 0x00040020u,
	0x0000001eu, 0x00000000u, 0x0000001du, 0x0004003bu, 0x0000001eu, 0x0000001fu,
	0x00000000u, 0x00040015u, 0x00000024u, 0x00000020u, 0x00000000u, 0x00090019u,
	0x00000029u, 0x00000024u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u,
	0x00000001u, 0x00000000u, 0x0003001bu, 0x0000002au, 0x00000029u, 0x00040020u,
	0x0000002bu, 0x00000000u, 0x0000002au, 0x0004003bu, 0x0000002bu, 0x0000002cu,
	0x00000000u, 0x00040017u, 0x00000030u, 0x00000024u, 0x00000004u, 0x00040020u,
	0x00000033u, 0x00000003u, 0x0000000bu, 0x0004003bu, 0x00000033u, 0x00000034u,
	0x00000003u, 0x0004002bu, 0x0000000au, 0x0000003au, 0x00000000u, 0x0004002bu,
	0x0000000au, 0x0000003bu, 0x3f800000u, 0x0004002bu, 0x0000000au, 0x0000003du,
	0x3b808081u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
	0x000200f8u, 0x00000005u, 0x0004003du, 0x0000000bu, 0x0000000fu, 0x0000000du,
	0x0007004fu, 0x0000000eu, 0x00000010u, 0x0000000fu, 0x0000000fu, 0x00000000u,
	0x00000001u, 0x00050041u, 0x00000015u, 0x00000016u, 0x00000013u, 0x00000014u,
	0x0004003du, 0x0000000eu, 0x00000017u, 0x00000016u, 0x00050085u, 0x0000000eu,
	0x00000018u, 0x00000010u, 0x00000017u, 0x0004006eu, 0x00000007u, 0x00000019u,
	0x00000018u, 0x0004003du, 0x0000001du, 0x00000020u, 0x0000001fu, 0x00040064u,
	0x0000001cu, 0x00000022u, 0x00000020u, 0x0007005fu, 0x0000000bu, 0x00000023u,
	0x00000022u, 0x00000019u, 0x00000002u, 0x00000014u, 0x00050051u, 0x0000000au,
	0x00000026u, 0x00000023u, 0x00000000u, 0x0004003du, 0x0000002au, 0x0000002du,
	0x0000002cu, 0x00040064u, 0x00000029u, 0x0000002fu, 0x0000002du, 0x0007005fu,
	0x00000030u, 0x00000031u, 0x0000002fu, 0x00000019u, 0x00000002u, 0x00000014u,
	0x00050051u, 0x00000024u, 0x00000032u, 0x00000031u, 0x00000000u, 0x00040070u,
	0x0000000au, 0x00000037u, 0x00000032u, 0x00050085u, 0x0000000au, 0x00000039u,
	0x00000037u, 0x0000003du, 0x00070050u, 0x0000000bu, 0x0000003cu, 0x00000026u,
	0x00000039u, 0x0000003au, 0x0000003bu, 0x0003003eu, 0x00000034u, 0x0000003cu,
	0x000100fdu, 0x00010038u,
};

VkShaderModule CreateShaderModule(VkDevice device, const uint32_t* words, size_t word_count)
{
	VkShaderModuleCreateInfo create_info {};
	create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.codeSize = word_count * sizeof(uint32_t);
	create_info.pCode    = words;

	VkShaderModule module = nullptr;
	const auto      result = vkCreateShaderModule(device, &create_info, nullptr, &module);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS || module == nullptr);
	return module;
}

} // namespace

void DepthStencilCopyRenderer::Initialize(GraphicContext* context)
{
	EXIT_IF(context == nullptr || context->device == nullptr);

	if (m_device != nullptr)
	{
		EXIT_NOT_IMPLEMENTED(m_device != context->device);
		return;
	}

	m_device = context->device;

	VkSamplerCreateInfo sampler_info {};
	sampler_info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter               = VK_FILTER_NEAREST;
	sampler_info.minFilter               = VK_FILTER_NEAREST;
	sampler_info.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.mipLodBias              = 0.0f;
	sampler_info.anisotropyEnable        = VK_FALSE;
	sampler_info.maxAnisotropy           = 1.0f;
	sampler_info.compareEnable           = VK_FALSE;
	sampler_info.compareOp               = VK_COMPARE_OP_ALWAYS;
	sampler_info.minLod                  = 0.0f;
	sampler_info.maxLod                  = 0.0f;
	sampler_info.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	sampler_info.unnormalizedCoordinates = VK_FALSE;
	EXIT_NOT_IMPLEMENTED(vkCreateSampler(m_device, &sampler_info, nullptr, &m_sampler) != VK_SUCCESS || m_sampler == nullptr);

	VkDescriptorSetLayoutBinding bindings[2] {};
	for (uint32_t binding = 0; binding < 2; binding++)
	{
		bindings[binding].binding            = binding;
		bindings[binding].descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[binding].descriptorCount    = 1;
		bindings[binding].stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
		bindings[binding].pImmutableSamplers = nullptr;
	}

	VkDescriptorSetLayoutCreateInfo descriptor_layout_info {};
	descriptor_layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptor_layout_info.bindingCount = 2;
	descriptor_layout_info.pBindings    = bindings;
	EXIT_NOT_IMPLEMENTED(vkCreateDescriptorSetLayout(m_device, &descriptor_layout_info, nullptr, &m_descriptor_set_layout) != VK_SUCCESS ||
	                     m_descriptor_set_layout == nullptr);

	VkDescriptorPoolSize pool_size {};
	pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = static_cast<uint32_t>(kMaxSourceDescriptors * 2);

	VkDescriptorPoolCreateInfo descriptor_pool_info {};
	descriptor_pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	descriptor_pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	descriptor_pool_info.maxSets       = static_cast<uint32_t>(kMaxSourceDescriptors);
	descriptor_pool_info.poolSizeCount = 1;
	descriptor_pool_info.pPoolSizes    = &pool_size;
	EXIT_NOT_IMPLEMENTED(vkCreateDescriptorPool(m_device, &descriptor_pool_info, nullptr, &m_descriptor_pool) != VK_SUCCESS ||
	                     m_descriptor_pool == nullptr);

	VkPushConstantRange scale_range {};
	scale_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	scale_range.offset      = 0;
	scale_range.size        = sizeof(float) * 2;

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_descriptor_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &scale_range;
	EXIT_NOT_IMPLEMENTED(vkCreatePipelineLayout(m_device, &pipeline_layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS ||
	                     m_pipeline_layout == nullptr);
}

VkDescriptorSet DepthStencilCopyRenderer::GetSourceDescriptor(DepthStencilVulkanImage* source)
{
	EXIT_IF(source == nullptr);
	EXIT_NOT_IMPLEMENTED(source->image_view[VulkanImage::VIEW_DEPTH_TEXTURE] == nullptr ||
	                     source->image_view[VulkanImage::VIEW_STENCIL_TEXTURE] == nullptr);

	for (const auto& entry: m_sources)
	{
		if (entry.source_id == source->memory.unique_id)
		{
			return entry.descriptor;
		}
	}

	EXIT_NOT_IMPLEMENTED(m_sources.size() >= kMaxSourceDescriptors);

	VkDescriptorSetAllocateInfo allocation_info {};
	allocation_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocation_info.descriptorPool     = m_descriptor_pool;
	allocation_info.descriptorSetCount = 1;
	allocation_info.pSetLayouts        = &m_descriptor_set_layout;

	VkDescriptorSet descriptor = nullptr;
	EXIT_NOT_IMPLEMENTED(vkAllocateDescriptorSets(m_device, &allocation_info, &descriptor) != VK_SUCCESS || descriptor == nullptr);

	VkDescriptorImageInfo image_infos[2] {};
	image_infos[0].sampler     = m_sampler;
	image_infos[0].imageView   = source->image_view[VulkanImage::VIEW_DEPTH_TEXTURE];
	image_infos[0].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	image_infos[1].sampler     = m_sampler;
	image_infos[1].imageView   = source->image_view[VulkanImage::VIEW_STENCIL_TEXTURE];
	image_infos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

	VkWriteDescriptorSet writes[2] {};
	for (uint32_t binding = 0; binding < 2; binding++)
	{
		writes[binding].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[binding].dstSet          = descriptor;
		writes[binding].dstBinding      = binding;
		writes[binding].descriptorCount = 1;
		writes[binding].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[binding].pImageInfo      = &image_infos[binding];
	}
	vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

	m_sources.push_back({source->memory.unique_id, descriptor});
	return descriptor;
}

VkPipeline DepthStencilCopyRenderer::GetRenderPipeline(GraphicContext* context, VkRenderPass render_pass, uint64_t render_pass_id)
{
	EXIT_IF(context == nullptr || render_pass == nullptr || render_pass_id == 0);

	for (const auto& entry: m_pipelines)
	{
		if (entry.render_pass_id == render_pass_id)
		{
			return entry.pipeline;
		}
	}

	EXIT_NOT_IMPLEMENTED(m_pipelines.size() >= kMaxRenderPipelines);

	const auto vertex_module   = CreateShaderModule(m_device, kVertexShader, sizeof(kVertexShader) / sizeof(kVertexShader[0]));
	const auto fragment_module = CreateShaderModule(m_device, kFragmentShader, sizeof(kFragmentShader) / sizeof(kFragmentShader[0]));

	VkPipelineShaderStageCreateInfo stages[2] {};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex_module;
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment_module;
	stages[1].pName  = "main";

	VkPipelineVertexInputStateCreateInfo vertex_input_info {};
	vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo input_assembly_info {};
	input_assembly_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly_info.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	input_assembly_info.primitiveRestartEnable = VK_FALSE;

	VkPipelineViewportStateCreateInfo viewport_info {};
	viewport_info.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_info.viewportCount = 1;
	viewport_info.scissorCount  = 1;

	VkPipelineRasterizationStateCreateInfo rasterization_info {};
	rasterization_info.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization_info.depthClampEnable        = VK_FALSE;
	rasterization_info.rasterizerDiscardEnable = VK_FALSE;
	rasterization_info.polygonMode             = VK_POLYGON_MODE_FILL;
	rasterization_info.cullMode                = VK_CULL_MODE_NONE;
	rasterization_info.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization_info.depthBiasEnable         = VK_FALSE;
	rasterization_info.lineWidth               = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample_info {};
	multisample_info.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisample_info.sampleShadingEnable  = VK_FALSE;

	VkPipelineColorBlendAttachmentState color_attachment {};
	color_attachment.blendEnable    = VK_FALSE;
	color_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
	                                  VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo color_blend_info {};
	color_blend_info.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blend_info.logicOpEnable   = VK_FALSE;
	color_blend_info.attachmentCount = 1;
	color_blend_info.pAttachments    = &color_attachment;

	const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state_info {};
	dynamic_state_info.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state_info.dynamicStateCount = 2;
	dynamic_state_info.pDynamicStates    = dynamic_states;

	VkGraphicsPipelineCreateInfo pipeline_info {};
	pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.stageCount          = 2;
	pipeline_info.pStages             = stages;
	pipeline_info.pVertexInputState   = &vertex_input_info;
	pipeline_info.pInputAssemblyState = &input_assembly_info;
	pipeline_info.pViewportState      = &viewport_info;
	pipeline_info.pRasterizationState = &rasterization_info;
	pipeline_info.pMultisampleState   = &multisample_info;
	pipeline_info.pDepthStencilState  = nullptr;
	pipeline_info.pColorBlendState    = &color_blend_info;
	pipeline_info.pDynamicState       = &dynamic_state_info;
	pipeline_info.layout              = m_pipeline_layout;
	pipeline_info.renderPass          = render_pass;
	pipeline_info.subpass             = 0;

	VkPipeline pipeline = nullptr;
	const auto result = vkCreateGraphicsPipelines(m_device, context->pipeline_cache, 1, &pipeline_info, nullptr, &pipeline);
	vkDestroyShaderModule(m_device, fragment_module, nullptr);
	vkDestroyShaderModule(m_device, vertex_module, nullptr);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS || pipeline == nullptr);

	m_pipelines.push_back({render_pass_id, pipeline});
	return pipeline;
}

void DepthStencilCopyRenderer::Render(GraphicContext* context, VkCommandBuffer command_buffer,
	                                  const DepthStencilCopyRequest& request)
{
	EXIT_IF(context == nullptr || command_buffer == nullptr || request.source == nullptr || request.render_pass == nullptr);
	EXIT_NOT_IMPLEMENTED(request.extent.width == 0 || request.extent.height == 0);
	EXIT_NOT_IMPLEMENTED(request.source->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

	Initialize(context);
	const auto descriptor = GetSourceDescriptor(request.source);
	const auto pipeline   = GetRenderPipeline(context, request.render_pass, request.render_pass_id);

	VkViewport viewport {};
	viewport.x        = 0.0f;
	viewport.y        = 0.0f;
	viewport.width    = static_cast<float>(request.extent.width);
	viewport.height   = static_cast<float>(request.extent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor {};
	scissor.offset = {0, 0};
	scissor.extent = request.extent;
	const float source_scale[2] = {static_cast<float>(request.source->extent.width) / static_cast<float>(request.extent.width),
	                               static_cast<float>(request.source->extent.height) / static_cast<float>(request.extent.height)};

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &descriptor, 0, nullptr);
	vkCmdPushConstants(command_buffer, m_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(source_scale), source_scale);
	vkCmdSetViewport(command_buffer, 0, 1, &viewport);
	vkCmdSetScissor(command_buffer, 0, 1, &scissor);
	vkCmdDraw(command_buffer, 3, 1, 0, 0);
}

void DepthStencilCopyRenderer::ReleaseSource(GraphicContext* context, uint64_t source_id)
{
	if (m_device == nullptr || source_id == 0)
	{
		return;
	}
	EXIT_IF(context == nullptr || context->device != m_device);

	for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
	{
		if (it->source_id == source_id)
		{
			EXIT_NOT_IMPLEMENTED(vkFreeDescriptorSets(m_device, m_descriptor_pool, 1, &it->descriptor) != VK_SUCCESS);
			m_sources.erase(it);
			return;
		}
	}
}

void DepthStencilCopyRenderer::ReleaseRenderPass(GraphicContext* context, uint64_t render_pass_id)
{
	if (m_device == nullptr || render_pass_id == 0)
	{
		return;
	}
	EXIT_IF(context == nullptr || context->device != m_device);

	for (auto it = m_pipelines.begin(); it != m_pipelines.end(); ++it)
	{
		if (it->render_pass_id == render_pass_id)
		{
			vkDestroyPipeline(m_device, it->pipeline, nullptr);
			m_pipelines.erase(it);
			return;
		}
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
