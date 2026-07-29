#include "Emulator/Graphics/DepthStencilCopy.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VulkanVertexInputLayout.h"

#include <cstddef>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr size_t kMaxSourceDescriptors = 256;
constexpr size_t kMaxRenderPipelines   = 512;

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
	0x0000001fu, 0x00000022u, 0x00000001u, 0x00040047u, 0x0000002cu, 0x00000021u,
	0x00000001u, 0x00040047u, 0x0000002cu, 0x00000022u, 0x00000001u, 0x00040047u,
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

VkColorComponentFlags ResolveDepthStencilCopyColorWriteMask(uint8_t mask)
{
	EXIT_NOT_IMPLEMENTED((mask & ~0x0fu) != 0 || mask == 0);

	VkColorComponentFlags flags = 0;
	if ((mask & 0x1u) != 0)
	{
		flags |= VK_COLOR_COMPONENT_R_BIT;
	}
	if ((mask & 0x2u) != 0)
	{
		flags |= VK_COLOR_COMPONENT_G_BIT;
	}
	if ((mask & 0x4u) != 0)
	{
		flags |= VK_COLOR_COMPONENT_B_BIT;
	}
	if ((mask & 0x8u) != 0)
	{
		flags |= VK_COLOR_COMPONENT_A_BIT;
	}

	return flags;
}

bool IsSameDepthStencilCopyStencilTest(const DepthStencilCopyStencilTest& lhs, const DepthStencilCopyStencilTest& rhs)
{
	if (lhs.enabled != rhs.enabled)
	{
		return false;
	}
	if (!lhs.enabled)
	{
		return true;
	}

	return lhs.front.fail_op == rhs.front.fail_op && lhs.front.pass_op == rhs.front.pass_op &&
	       lhs.front.depth_fail_op == rhs.front.depth_fail_op && lhs.front.compare_op == rhs.front.compare_op &&
	       lhs.front.compare_mask == rhs.front.compare_mask && lhs.front.write_mask == rhs.front.write_mask &&
	       lhs.front.reference == rhs.front.reference && lhs.back.fail_op == rhs.back.fail_op &&
	       lhs.back.pass_op == rhs.back.pass_op && lhs.back.depth_fail_op == rhs.back.depth_fail_op &&
	       lhs.back.compare_op == rhs.back.compare_op && lhs.back.compare_mask == rhs.back.compare_mask &&
	       lhs.back.write_mask == rhs.back.write_mask && lhs.back.reference == rhs.back.reference;
}

VkStencilOpState ResolveDepthStencilCopyStencilFace(const DepthStencilCopyStencilFace& face)
{
	VkStencilOpState state {};
	state.failOp      = face.fail_op;
	state.passOp      = face.pass_op;
	state.depthFailOp = face.depth_fail_op;
	state.compareOp   = face.compare_op;
	state.compareMask = face.compare_mask;
	state.writeMask   = face.write_mask;
	state.reference   = face.reference;
	return state;
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

	VkDescriptorSetLayoutCreateInfo empty_descriptor_layout_info {};
	empty_descriptor_layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	empty_descriptor_layout_info.bindingCount = 0;
	empty_descriptor_layout_info.pBindings    = nullptr;
	EXIT_NOT_IMPLEMENTED(vkCreateDescriptorSetLayout(m_device, &empty_descriptor_layout_info, nullptr,
	                                                 &m_empty_descriptor_set_layout) != VK_SUCCESS ||
	                     m_empty_descriptor_set_layout == nullptr);

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
	EXIT_NOT_IMPLEMENTED(vkCreateDescriptorSetLayout(m_device, &descriptor_layout_info, nullptr, &m_source_descriptor_set_layout) !=
	                         VK_SUCCESS ||
	                     m_source_descriptor_set_layout == nullptr);

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
	allocation_info.pSetLayouts        = &m_source_descriptor_set_layout;

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

DepthStencilCopyRenderer::RenderPipeline* DepthStencilCopyRenderer::GetRenderPipeline(GraphicContext* context,
	                                                                                      const DepthStencilCopyRequest& request)
{
	EXIT_IF(context == nullptr || request.render_pass == nullptr || request.render_pass_id == 0);
	const bool expand_to_color = (request.mode == DepthStencilCopyMode::ExpandToColor);
	EXIT_NOT_IMPLEMENTED(request.mode != DepthStencilCopyMode::ExpandToColor && request.mode != DepthStencilCopyMode::DepthStencilOnly);
	EXIT_NOT_IMPLEMENTED(expand_to_color && ((request.color_write_mask & ~0x0fu) != 0 || request.color_write_mask == 0));
	EXIT_NOT_IMPLEMENTED(!expand_to_color && (request.color_write_mask != 0 || request.source != nullptr));

	const auto* vertex_stage = request.vertex_stage;
	const bool  guest_vertex_stage = (vertex_stage != nullptr);
	const ShaderBindResources* vertex_bind = (guest_vertex_stage ? vertex_stage->bind : nullptr);
	const bool vertex_requires_descriptor = (vertex_bind != nullptr && ShaderBindRequiresDescriptorSet(*vertex_bind));

	if (guest_vertex_stage)
	{
		EXIT_NOT_IMPLEMENTED(vertex_stage->input == nullptr || vertex_bind == nullptr || vertex_stage->shader_id == nullptr ||
		                     vertex_stage->shader_words == nullptr || vertex_stage->shader_word_count == 0);
		EXIT_NOT_IMPLEMENTED(vertex_requires_descriptor != (vertex_stage->descriptor_set_layout != nullptr));
		EXIT_NOT_IMPLEMENTED(vertex_requires_descriptor && vertex_bind->descriptor_set_slot != 0);
	}

	for (auto& entry: m_pipelines)
	{
		if (entry.render_pass_id != request.render_pass_id || entry.guest_vertex_stage != guest_vertex_stage || entry.mode != request.mode ||
		    entry.color_write_mask != request.color_write_mask || entry.depth_test_enable != request.depth_test.enabled ||
		    entry.depth_write_enable != request.depth_test.write_enable || entry.depth_compare_op != request.depth_test.compare_op ||
		    !IsSameDepthStencilCopyStencilTest(entry.stencil_test, request.stencil_test))
		{
			continue;
		}
		if (!guest_vertex_stage)
		{
			return &entry;
		}
		if (entry.vertex_shader_id == *vertex_stage->shader_id &&
		    entry.vertex_descriptor_set_layout == vertex_stage->descriptor_set_layout && entry.topology == vertex_stage->topology &&
		    entry.cull_front == vertex_stage->cull_front && entry.cull_back == vertex_stage->cull_back && entry.face == vertex_stage->face &&
		    entry.dx_clip_space == vertex_stage->dx_clip_space &&
		    entry.vertex_push_constant_offset == vertex_bind->push_constant_offset &&
		    entry.vertex_push_constant_size == vertex_bind->push_constant_size &&
		    entry.vertex_uses_uniform_buffer == vertex_bind->vsharp_uniform_buffer)
		{
			return &entry;
		}
	}

	const auto* vertex_words = (guest_vertex_stage ? vertex_stage->shader_words : kVertexShader);
	const size_t vertex_word_count =
	    (guest_vertex_stage ? vertex_stage->shader_word_count : sizeof(kVertexShader) / sizeof(kVertexShader[0]));
	const auto vertex_module = CreateShaderModule(m_device, vertex_words, vertex_word_count);
	const auto fragment_module =
	    (expand_to_color ? CreateShaderModule(m_device, kFragmentShader, sizeof(kFragmentShader) / sizeof(kFragmentShader[0])) : nullptr);

	VkPipelineShaderStageCreateInfo stages[2] {};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex_module;
	stages[0].pName  = "main";
	if (expand_to_color)
	{
		stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragment_module;
		stages[1].pName  = "main";
	}

	VulkanVertexInputLayout vertex_input_layout {};
	if (guest_vertex_stage)
	{
		EXIT_NOT_IMPLEMENTED(!VulkanBuildVertexInputLayout(*vertex_stage->input, &vertex_input_layout));
	}

	VkPipelineVertexInputStateCreateInfo vertex_input_info {};
	vertex_input_info.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input_info.vertexBindingDescriptionCount   = vertex_input_layout.binding_count;
	vertex_input_info.pVertexBindingDescriptions      =
	    (vertex_input_layout.binding_count > 0 ? vertex_input_layout.bindings : nullptr);
	vertex_input_info.vertexAttributeDescriptionCount = vertex_input_layout.attribute_count;
	vertex_input_info.pVertexAttributeDescriptions    =
	    (vertex_input_layout.attribute_count > 0 ? vertex_input_layout.attributes : nullptr);

	VkPipelineInputAssemblyStateCreateInfo input_assembly_info {};
	input_assembly_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly_info.topology               = (guest_vertex_stage ? vertex_stage->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	input_assembly_info.primitiveRestartEnable = VK_FALSE;

	const bool dx_clip_space = (!guest_vertex_stage || vertex_stage->dx_clip_space);
	EXIT_NOT_IMPLEMENTED(!dx_clip_space && !context->depth_clip_control_supported);

	VkPipelineViewportDepthClipControlCreateInfoEXT depth_clip_control {};
	depth_clip_control.sType            = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT;
	depth_clip_control.negativeOneToOne = (dx_clip_space ? VK_FALSE : VK_TRUE);

	VkPipelineViewportStateCreateInfo viewport_info {};
	viewport_info.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_info.pNext         = (context->depth_clip_control_supported ? &depth_clip_control : nullptr);
	viewport_info.viewportCount = 1;
	viewport_info.scissorCount  = 1;

	VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
	if (guest_vertex_stage)
	{
		cull_mode |= (vertex_stage->cull_back ? VK_CULL_MODE_BACK_BIT : 0u);
		cull_mode |= (vertex_stage->cull_front ? VK_CULL_MODE_FRONT_BIT : 0u);
	}

	VkPipelineRasterizationDepthClipStateCreateInfoEXT depth_clip_state {};
	depth_clip_state.sType           = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
	depth_clip_state.depthClipEnable = VK_FALSE;

	VkPipelineRasterizationStateCreateInfo rasterization_info {};
	rasterization_info.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization_info.pNext                   = (context->depth_clip_enable_supported ? &depth_clip_state : nullptr);
	rasterization_info.depthClampEnable        = (context->depth_clip_enable_supported ? VK_FALSE : VK_TRUE);
	rasterization_info.rasterizerDiscardEnable = VK_FALSE;
	rasterization_info.polygonMode             = VK_POLYGON_MODE_FILL;
	rasterization_info.cullMode                = cull_mode;
	rasterization_info.frontFace               =
	    (guest_vertex_stage && vertex_stage->face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
	rasterization_info.depthBiasEnable         = VK_FALSE;
	rasterization_info.lineWidth               = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample_info {};
	multisample_info.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisample_info.sampleShadingEnable  = VK_FALSE;

	VkPipelineColorBlendAttachmentState color_attachment {};
	color_attachment.blendEnable    = VK_FALSE;
	color_attachment.colorWriteMask = (expand_to_color ? ResolveDepthStencilCopyColorWriteMask(request.color_write_mask) : 0);

	VkPipelineColorBlendStateCreateInfo color_blend_info {};
	color_blend_info.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blend_info.logicOpEnable   = VK_FALSE;
	color_blend_info.attachmentCount = 1;
	color_blend_info.pAttachments    = &color_attachment;

	VkPipelineDepthStencilStateCreateInfo depth_stencil_info {};
	depth_stencil_info.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil_info.depthTestEnable       = (request.depth_test.enabled ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthWriteEnable      = (request.depth_test.write_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthCompareOp        = request.depth_test.compare_op;
	depth_stencil_info.depthBoundsTestEnable = VK_FALSE;
	depth_stencil_info.stencilTestEnable     = (request.stencil_test.enabled ? VK_TRUE : VK_FALSE);
	depth_stencil_info.front                 = ResolveDepthStencilCopyStencilFace(request.stencil_test.front);
	depth_stencil_info.back                  = ResolveDepthStencilCopyStencilFace(request.stencil_test.back);

	const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state_info {};
	dynamic_state_info.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state_info.dynamicStateCount = 2;
	dynamic_state_info.pDynamicStates    = dynamic_states;

	VkDescriptorSetLayout set_layouts[2] = {};
	uint32_t              set_layout_count = 0;
	if (vertex_requires_descriptor)
	{
		set_layouts[set_layout_count++] = vertex_stage->descriptor_set_layout;
	} else if (expand_to_color)
	{
		set_layouts[set_layout_count++] = m_empty_descriptor_set_layout;
	}
	if (expand_to_color)
	{
		set_layouts[set_layout_count++] = m_source_descriptor_set_layout;
	}
	VkPushConstantRange push_constant_ranges[2] {};
	uint32_t            push_constant_range_count = 0;
	if (guest_vertex_stage && !vertex_bind->vsharp_uniform_buffer && vertex_bind->push_constant_size > 0)
	{
		push_constant_ranges[push_constant_range_count].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		push_constant_ranges[push_constant_range_count].offset     = vertex_bind->push_constant_offset;
		push_constant_ranges[push_constant_range_count].size       = vertex_bind->push_constant_size;
		push_constant_range_count++;
	}
	if (expand_to_color)
	{
		push_constant_ranges[push_constant_range_count].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		push_constant_ranges[push_constant_range_count].offset     = 0;
		push_constant_ranges[push_constant_range_count].size       = sizeof(float) * 2;
		push_constant_range_count++;
	}

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.setLayoutCount         = set_layout_count;
	pipeline_layout_info.pSetLayouts            = (set_layout_count > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_range_count;
	pipeline_layout_info.pPushConstantRanges    = (push_constant_range_count > 0 ? push_constant_ranges : nullptr);

	VkPipelineLayout pipeline_layout = nullptr;
	EXIT_NOT_IMPLEMENTED(vkCreatePipelineLayout(m_device, &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS ||
	                     pipeline_layout == nullptr);

	VkGraphicsPipelineCreateInfo pipeline_info {};
	pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.stageCount          = (expand_to_color ? 2u : 1u);
	pipeline_info.pStages             = stages;
	pipeline_info.pVertexInputState   = &vertex_input_info;
	pipeline_info.pInputAssemblyState = &input_assembly_info;
	pipeline_info.pViewportState      = &viewport_info;
	pipeline_info.pRasterizationState = &rasterization_info;
	pipeline_info.pMultisampleState   = &multisample_info;
	pipeline_info.pDepthStencilState  = (request.depth_test.enabled || request.stencil_test.enabled ? &depth_stencil_info : nullptr);
	pipeline_info.pColorBlendState    = &color_blend_info;
	pipeline_info.pDynamicState       = &dynamic_state_info;
	pipeline_info.layout              = pipeline_layout;
	pipeline_info.renderPass          = request.render_pass;
	pipeline_info.subpass             = 0;

	VkPipeline pipeline = nullptr;
	const auto result = vkCreateGraphicsPipelines(m_device, context->pipeline_cache, 1, &pipeline_info, nullptr, &pipeline);
	if (fragment_module != nullptr)
	{
		vkDestroyShaderModule(m_device, fragment_module, nullptr);
	}
	vkDestroyShaderModule(m_device, vertex_module, nullptr);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS || pipeline == nullptr);

	RenderPipeline entry {};
	entry.render_pass_id              = request.render_pass_id;
	entry.guest_vertex_stage          = guest_vertex_stage;
	entry.mode                        = request.mode;
	entry.pipeline                    = pipeline;
	entry.pipeline_layout             = pipeline_layout;
	entry.vertex_descriptor_set_layout = (vertex_requires_descriptor ? vertex_stage->descriptor_set_layout : nullptr);
	entry.color_write_mask            = request.color_write_mask;
	entry.depth_test_enable           = request.depth_test.enabled;
	entry.depth_write_enable          = request.depth_test.write_enable;
	entry.depth_compare_op            = request.depth_test.compare_op;
	entry.stencil_test                = request.stencil_test;
	if (guest_vertex_stage)
	{
		entry.vertex_shader_id             = *vertex_stage->shader_id;
		entry.topology                     = vertex_stage->topology;
		entry.cull_front                   = vertex_stage->cull_front;
		entry.cull_back                    = vertex_stage->cull_back;
		entry.face                         = vertex_stage->face;
		entry.dx_clip_space                = vertex_stage->dx_clip_space;
		entry.vertex_push_constant_offset  = vertex_bind->push_constant_offset;
		entry.vertex_push_constant_size    = vertex_bind->push_constant_size;
		entry.vertex_uses_uniform_buffer   = vertex_bind->vsharp_uniform_buffer;
	}
	if (m_pipelines.size() < kMaxRenderPipelines)
	{
		m_pipelines.push_back(entry);
		return &m_pipelines.back();
	}

	// Keep specialized pipelines bounded during long sessions. Pipeline handles
	// are recreated on demand after round-robin eviction.
	const auto evict = PipelineCacheNextEvictIndex(static_cast<uint32_t>(m_pipelines.size()), &m_evict_cursor);
	DebugStatsRecordPipelineEviction();
	DestroyRenderPipeline(&m_pipelines[evict]);
	m_pipelines[evict] = entry;
	return &m_pipelines[evict];
}

DepthStencilCopyPreparedDraw DepthStencilCopyRenderer::PrepareDraw(GraphicContext* context, const DepthStencilCopyRequest& request)
{
	EXIT_IF(context == nullptr || request.render_pass == nullptr);
	EXIT_NOT_IMPLEMENTED(request.extent.width == 0 || request.extent.height == 0);
	EXIT_NOT_IMPLEMENTED(request.viewport.width <= 0.0f || request.viewport.height == 0.0f);
	const bool expand_to_color = (request.mode == DepthStencilCopyMode::ExpandToColor);
	EXIT_NOT_IMPLEMENTED(request.mode != DepthStencilCopyMode::ExpandToColor && request.mode != DepthStencilCopyMode::DepthStencilOnly);
	EXIT_NOT_IMPLEMENTED(expand_to_color && request.source == nullptr);
	EXIT_NOT_IMPLEMENTED(!expand_to_color && request.source != nullptr);
	EXIT_NOT_IMPLEMENTED(expand_to_color && request.source->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

	Initialize(context);
	auto*      pipeline   = GetRenderPipeline(context, request);
	const auto descriptor = (expand_to_color ? GetSourceDescriptor(request.source) : nullptr);

	DepthStencilCopyPreparedDraw draw {};
	draw.pipeline          = pipeline->pipeline;
	draw.pipeline_layout   = pipeline->pipeline_layout;
	draw.source_descriptor = descriptor;
	draw.viewport          = request.viewport;
	draw.scissor           = request.scissor;
	if (expand_to_color)
	{
		draw.source_scale[0] = static_cast<float>(request.source->extent.width) / static_cast<float>(request.extent.width);
		draw.source_scale[1] = static_cast<float>(request.source->extent.height) / static_cast<float>(request.extent.height);
	}
	return draw;
}

void DepthStencilCopyRenderer::BindPreparedDraw(VkCommandBuffer command_buffer, const DepthStencilCopyPreparedDraw& draw) const
{
	EXIT_IF(command_buffer == nullptr || draw.pipeline == nullptr || draw.pipeline_layout == nullptr);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
	if (draw.source_descriptor != nullptr)
	{
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline_layout, 1, 1, &draw.source_descriptor, 0,
		                        nullptr);
		vkCmdPushConstants(command_buffer, draw.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(draw.source_scale),
		                   draw.source_scale);
	}
	vkCmdSetViewport(command_buffer, 0, 1, &draw.viewport);
	vkCmdSetScissor(command_buffer, 0, 1, &draw.scissor);
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

void DepthStencilCopyRenderer::DestroyRenderPipeline(RenderPipeline* pipeline)
{
	EXIT_IF(pipeline == nullptr || pipeline->pipeline == nullptr || pipeline->pipeline_layout == nullptr);

	vkDestroyPipeline(m_device, pipeline->pipeline, nullptr);
	vkDestroyPipelineLayout(m_device, pipeline->pipeline_layout, nullptr);
	pipeline->pipeline        = nullptr;
	pipeline->pipeline_layout = nullptr;
}

void DepthStencilCopyRenderer::ReleaseRenderPass(GraphicContext* context, uint64_t render_pass_id)
{
	if (m_device == nullptr || render_pass_id == 0)
	{
		return;
	}
	EXIT_IF(context == nullptr || context->device != m_device);

	for (auto it = m_pipelines.begin(); it != m_pipelines.end();)
	{
		if (it->render_pass_id == render_pass_id)
		{
			DestroyRenderPipeline(&*it);
			it = m_pipelines.erase(it);
			continue;
		}
		++it;
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
