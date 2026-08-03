#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/PipelineCacheStore.h"
#include "Emulator/Graphics/RenderResolutionPolicy.h"
#include "Emulator/Graphics/RenderResolutionTransform.h"
#include "Emulator/Graphics/SampleLocations.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderTranslationCache.h"
#include "Emulator/Graphics/SpirvBinaryCacheStore.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/VulkanVertexInputLayout.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

std::atomic_uint32_t g_vertex_input_layout_log_count {0};

bool VertexInputLayoutLogEnabled()
{
	const char* value = std::getenv("KYTY_VIL_LOG");
	return value != nullptr && std::strcmp(value, "0") != 0 && std::strcmp(value, "off") != 0 && std::strcmp(value, "false") != 0;
}

} // namespace

// SamplerCache::GetSamplerId, PipelineCache, CreatePipelineInternal

uint64_t SamplerCache::GetSamplerId(const ShaderSamplerResource& r)
{
	Core::LockGuard lock(m_mutex);
	uint32_t        m_samplers_size = m_samplers.Size();
	for (uint32_t i = 0; i < m_samplers_size; i++)
	{
		const auto& s = m_samplers.At(i);
		if (s.r.fields[0] == r.fields[0] && s.r.fields[1] == r.fields[1] && s.r.fields[2] == r.fields[2] && s.r.fields[3] == r.fields[3])
		{
			return i;
		}
	}
	Sampler s;
	s.r  = r;
	s.vk = nullptr;

	bool  aniso       = false;
	float aniso_ratio = 1.0f;
	auto  mag_filter  = r.XyMagFilter();
	auto  min_filter  = r.XyMinFilter();

	if (mag_filter == 2 || mag_filter == 3 || min_filter == 2 || min_filter == 3)
	{
		aniso = true;
		switch (r.MaxAnisoRatio())
		{
			case 0: aniso_ratio = 1.0f; break;
			case 1: aniso_ratio = 2.0f; break;
			case 2: aniso_ratio = 4.0f; break;
			case 3: aniso_ratio = 8.0f; break;
			case 4:
				aniso_ratio = 16.0f;
				break;
				printf("WARNING: unknown aniso ratio (continuing)\n");
		}
	}

	auto  mip_filter = r.MipFilter();
	float min_lod    = 0.0f;
	float max_lod    = 0.0f;
	if (mip_filter != 0)
	{
		min_lod = static_cast<float>(r.MinLod()) / 256.0f;
		max_lod = static_cast<float>(r.MaxLod()) / 256.0f;
	}

	VkSamplerCreateInfo sampler_info {};
	const auto          sampler_comparison  = State::ResolveSamplerComparison(r.DepthCompareFunc(), State::ImageSampleOperation::Regular);
	const auto          unnormalized_policy = State::ResolveUnnormalizedSamplerPolicy(r.ForceUnormCoords());

	auto get_warp = [](uint8_t clamp)
	{
		switch (State::ResolveSamplerAddressMode(clamp))
		{
			case State::SamplerAddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			case State::SamplerAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			case State::SamplerAddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case State::SamplerAddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		}
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	};

	VkBorderColor border = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
	switch (r.BorderColorType())
	{
		case 0: border = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK; break;
		case 1: border = VK_BORDER_COLOR_INT_OPAQUE_BLACK; break;
		case 2:
			border = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
			break;
			printf("WARNING: unknown border color (continuing)\n");
	}

	sampler_info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.pNext                   = nullptr;
	sampler_info.flags                   = 0;
	sampler_info.magFilter               = (mag_filter == 0 || mag_filter == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	sampler_info.minFilter               = (min_filter == 0 || min_filter == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	sampler_info.mipmapMode              = (mip_filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST);
	sampler_info.addressModeU            = get_warp(r.ClampX());
	sampler_info.addressModeV            = get_warp(r.ClampY());
	sampler_info.addressModeW            = get_warp(r.ClampZ());
	sampler_info.mipLodBias              = static_cast<float>(static_cast<int16_t>((r.LodBias() ^ 0x2000u) - 0x2000u)) / 256.0f;
	sampler_info.anisotropyEnable        = (aniso ? VK_TRUE : VK_FALSE);
	sampler_info.maxAnisotropy           = aniso_ratio;
	sampler_info.compareEnable           = sampler_comparison.enabled ? VK_TRUE : VK_FALSE;
	sampler_info.compareOp               = static_cast<VkCompareOp>(sampler_comparison.function);
	sampler_info.minLod                  = min_lod;
	sampler_info.maxLod                  = max_lod;
	sampler_info.borderColor             = border;
	sampler_info.unnormalizedCoordinates = (r.ForceUnormCoords() ? VK_TRUE : VK_FALSE);
	if (unnormalized_policy.enabled)
	{
		sampler_info.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.addressModeV     = sampler_info.addressModeU;
		sampler_info.addressModeW     = sampler_info.addressModeU;
		sampler_info.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sampler_info.minLod           = 0.0f;
		sampler_info.maxLod           = 0.0f;
		sampler_info.anisotropyEnable = VK_FALSE;
		sampler_info.maxAnisotropy    = 1.0f;
		sampler_info.compareEnable    = VK_FALSE;
		sampler_info.mipLodBias       = 0.0f;
	}

	vkCreateSampler(g_render_ctx->GetGraphicCtx()->device, &sampler_info, nullptr, &s.vk);
	EXIT_NOT_IMPLEMENTED(s.vk == nullptr);

	m_samplers.Add(s);
	return m_samplers_size;
}

static VkBlendFactor get_blend_factor(uint32_t factor)
{
	switch (factor)
	{
		case /* Zero */ 0x00000000: return VK_BLEND_FACTOR_ZERO;
		case /* One */ 0x00000001: return VK_BLEND_FACTOR_ONE;
		case /* SrcColor */ 0x00000002: return VK_BLEND_FACTOR_SRC_COLOR;
		case /* OneMinusSrcColor */ 0x00000003: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case /* SrcAlpha */ 0x00000004: return VK_BLEND_FACTOR_SRC_ALPHA;
		case /* OneMinusSrcAlpha */ 0x00000005: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case /* DestAlpha */ 0x00000006: return VK_BLEND_FACTOR_DST_ALPHA;
		case /* OneMinusDestAlpha */ 0x00000007: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case /* DestColor */ 0x00000008: return VK_BLEND_FACTOR_DST_COLOR;
		case /* OneMinusDestColor */ 0x00000009: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case /* SrcAlphaSaturate */ 0x0000000a: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		case /* ConstantColor */ 0x0000000d: return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case /* OneMinusConstantColor */ 0x0000000e: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		case /* Src1Color */ 0x0000000f: return VK_BLEND_FACTOR_SRC1_COLOR;
		case /* InverseSrc1Color */ 0x00000010: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
		case /* Src1Alpha */ 0x00000011: return VK_BLEND_FACTOR_SRC1_ALPHA;
		case /* InverseSrc1Alpha */ 0x00000012: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
		case /* ConstantAlpha */ 0x00000013: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case /* OneMinusConstantAlpha */ 0x00000014: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		default: printf("WARNING: unknown blend factor %u (continuing)\n", factor); break;
	}
	return VK_BLEND_FACTOR_ZERO;
}

struct BlendFactors
{
	VkBlendFactor src;
	VkBlendFactor dst;
};

static BlendFactors get_blend_factors(uint32_t src_factor, uint32_t dst_factor)
{
	switch (src_factor)
	{
		case /* BothSrcAlpha */ 0x0000000b: return {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
		case /* BothInverseSrcAlpha */ 0x0000000c: return {VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA};
		default: break;
	}
	return {get_blend_factor(src_factor), get_blend_factor(dst_factor)};
}

static VkBlendOp get_blend_op(uint32_t op)
{
	switch (op)
	{
		case /* Add */ 0x00000000: return VK_BLEND_OP_ADD;
		case /* Subtract */ 0x00000001: return VK_BLEND_OP_SUBTRACT;
		case /* Min */ 0x00000002: return VK_BLEND_OP_MIN;
		case /* Max */ 0x00000003: return VK_BLEND_OP_MAX;
		case /* ReverseSubtract */ 0x00000004: return VK_BLEND_OP_REVERSE_SUBTRACT;
		default: printf("WARNING: unknown blend operation %u (continuing)\n", op); break;
	}
	return VK_BLEND_OP_ADD;
}

static void CreateLayout(VkDescriptorSetLayout* set_layouts, uint32_t* set_layouts_num, VkPushConstantRange* push_constant_info,
                         uint32_t* push_constant_info_num, const ShaderBindResources& bind, VkShaderStageFlags vk_stage,
                         DescriptorCache::Stage stage)
{
	EXIT_IF(set_layouts == nullptr);
	EXIT_IF(set_layouts_num == nullptr);
	EXIT_IF(push_constant_info == nullptr);
	EXIT_IF(push_constant_info_num == nullptr);

	const bool need_descriptor = ShaderBindRequiresDescriptorSet(bind);

	EXIT_IF(need_descriptor && bind.push_constant_size == 0);

	if (bind.push_constant_size != 0 && !bind.vsharp_uniform_buffer)
	{
		auto index = *push_constant_info_num;

		push_constant_info[index].stageFlags = vk_stage;
		push_constant_info[index].offset     = bind.push_constant_offset;
		push_constant_info[index].size       = bind.push_constant_size;
		(*push_constant_info_num)++;
	}

	if (need_descriptor)
	{
		EXIT_IF(bind.descriptor_set_slot != *set_layouts_num);

		set_layouts[*set_layouts_num] = g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(stage, bind /*, bind_params*/);
		(*set_layouts_num)++;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static VulkanPipeline* CreatePipelineInternal(VkRenderPass render_pass, const ShaderVertexInputInfo* vs_input_info,
                                              const Vector<uint32_t>& vs_shader, const ShaderPixelInputInfo* ps_input_info,
                                              const Vector<uint32_t>& ps_shader, const PipelineStaticParameters* static_params,
                                              PipelineDynamicParameters* dynamic_params)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(render_pass == nullptr);
	EXIT_IF(static_params == nullptr);
	EXIT_IF(dynamic_params == nullptr);

	auto* pipeline           = new VulkanPipeline;
	pipeline->static_params  = static_params;
	pipeline->dynamic_params = dynamic_params;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);
	EXIT_NOT_IMPLEMENTED(static_params->sample_shading_enable && !gctx->sample_rate_shading_supported);

	VkShaderModule vert_shader_module = nullptr;
	VkShaderModule frag_shader_module = nullptr;

	VkShaderModuleCreateInfo create_info {};

	create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext = nullptr;
	create_info.flags = 0;

	create_info.codeSize = static_cast<size_t>(vs_shader.Size()) * 4;
	create_info.pCode    = vs_shader.GetDataConst();
	vkCreateShaderModule(gctx->device, &create_info, nullptr, &vert_shader_module);

	const bool has_fragment_stage = !ps_shader.IsEmpty();
	if (has_fragment_stage)
	{
		create_info.codeSize = static_cast<size_t>(ps_shader.Size()) * 4;
		create_info.pCode    = ps_shader.GetDataConst();
		vkCreateShaderModule(gctx->device, &create_info, nullptr, &frag_shader_module);
	}

	EXIT_NOT_IMPLEMENTED(vert_shader_module == nullptr);
	EXIT_NOT_IMPLEMENTED(has_fragment_stage && frag_shader_module == nullptr);

	VkPipelineShaderStageCreateInfo vert_shader_stage_info {};
	vert_shader_stage_info.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vert_shader_stage_info.pNext               = nullptr;
	vert_shader_stage_info.flags               = 0;
	vert_shader_stage_info.stage               = VK_SHADER_STAGE_VERTEX_BIT;
	vert_shader_stage_info.module              = vert_shader_module;
	vert_shader_stage_info.pName               = "main";
	vert_shader_stage_info.pSpecializationInfo = nullptr;

	VkPipelineShaderStageCreateInfo frag_shader_stage_info {};
	frag_shader_stage_info.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	frag_shader_stage_info.flags               = 0;
	frag_shader_stage_info.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
	frag_shader_stage_info.module              = frag_shader_module;
	frag_shader_stage_info.pName               = "main";
	frag_shader_stage_info.pSpecializationInfo = nullptr;

	// Request the guest fragment wave width when Vulkan can honor it. The
	// pipeline remains valid on devices without exact subgroup-size control;
	// those devices execute the translated subgroup operations at their native
	// width instead of dropping the draw during pipeline creation.
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required_subgroup {};
	const auto gctx_dev = g_render_ctx->GetGraphicCtx();
	const uint32_t required_ps_size = ps_input_info != nullptr ? ps_input_info->required_subgroup_size : 0u;
	if (required_ps_size != 0u && gctx_dev->subgroup_size_control_supported && required_ps_size >= gctx_dev->subgroup_min_size &&
	    required_ps_size <= gctx_dev->subgroup_max_size)
	{
		required_subgroup.sType                = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
		required_subgroup.pNext                = nullptr;
		required_subgroup.requiredSubgroupSize = required_ps_size;
		frag_shader_stage_info.pNext           = &required_subgroup;
	}

	VkPipelineShaderStageCreateInfo shader_stages[] = {vert_shader_stage_info, frag_shader_stage_info};


	VulkanVertexInputLayout input_layout {};
	if (!VulkanBuildVertexInputLayout(*vs_input_info, &input_layout))
	{
		const bool log_vil_failure = VertexInputLayoutLogEnabled() &&
		                             g_vertex_input_layout_log_count.fetch_add(1, std::memory_order_relaxed) < 32u;
		if (log_vil_failure)
		{
			if (FILE* f = fopen("/tmp/kyty_vil.log", "a"))
			{
				fprintf(f, "VIL FAIL: resources=%d buffers=%d\n", vs_input_info->resources_num, vs_input_info->buffers_num);
				for (int bi = 0; bi < vs_input_info->buffers_num; bi++)
				{
					const auto& b = vs_input_info->buffers[bi];
					fprintf(f, "  buf[%d] addr=0x%llx stride=%u records=%u attr_num=%d\n", bi, (unsigned long long)b.addr, b.stride,
					        b.num_records, b.attr_num);
					for (int ai = 0; ai < b.attr_num && ai < 16; ai++)
					{
						fprintf(f, "    attr[%d] resource=%d offset=%u\n", ai, b.attr_indices[ai], b.attr_offsets[ai]);
					}
				}
				for (int ri = 0; ri < vs_input_info->resources_num; ri++)
				{
					const auto& r = vs_input_info->resources[ri];
					fprintf(f,
					        "  res[%d] fields=%08x,%08x,%08x,%08x format=%u dfmt=%u nfmt=%u records=%llu stride=%u addtid=%d swizzle=%d "
					        "dst=%d/%d dsel=%u,%u,%u,%u\n",
					        ri, r.fields[0], r.fields[1], r.fields[2], r.fields[3], (unsigned)r.Format(), (unsigned)r.Dfmt(), (unsigned)r.Nfmt(),
					        (unsigned long long)r.NumRecords(), (unsigned)r.Stride(), r.AddTid() ? 1 : 0, r.SwizzleEnabled() ? 1 : 0,
					        vs_input_info->resources_dst[ri].register_start, vs_input_info->resources_dst[ri].registers_num, r.DstSelX(), r.DstSelY(),
					        r.DstSelZ(), r.DstSelW());
				}
				fclose(f);
			}
		}
		EXIT_NOT_IMPLEMENTED(!VulkanBuildVertexInputLayout(*vs_input_info, &input_layout));
	}

	VkPipelineVertexInputStateCreateInfo vertex_input_info {};
	vertex_input_info.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input_info.pNext                           = nullptr;
	vertex_input_info.flags                           = 0;
	vertex_input_info.vertexBindingDescriptionCount   = input_layout.binding_count;
	vertex_input_info.pVertexBindingDescriptions      = input_layout.bindings;
	vertex_input_info.vertexAttributeDescriptionCount = input_layout.attribute_count;
	vertex_input_info.pVertexAttributeDescriptions    = input_layout.attributes;

	VkPipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.pNext                  = nullptr;
	input_assembly.flags                  = 0;
	input_assembly.topology               = static_params->topology;
	input_assembly.primitiveRestartEnable = VK_FALSE;

	const bool unrestricted = g_render_ctx->GetGraphicCtx()->depth_range_unrestricted_supported;

	VkViewport viewport {};
	VkRect2D   scissor {};

	if (!dynamic_params->vk_dynamic_state_viewport)
	{
		const auto depth_range = State::ResolveViewportDepth(dynamic_params->viewport_scale[2], dynamic_params->viewport_offset[2],
		                                                     static_params->dx_clip_space, unrestricted,
		                                                     dynamic_params->viewport_depth_clamp[0],
		                                                     dynamic_params->viewport_depth_clamp[1]);
		const auto xy          = State::ResolveViewportXy(dynamic_params->viewport_scale[0], dynamic_params->viewport_offset[0],
		                                                  dynamic_params->viewport_scale[1], dynamic_params->viewport_offset[1]);
		viewport.x             = xy.x;
		viewport.y             = xy.y;
		viewport.width         = xy.width;
		viewport.height        = xy.height;
		viewport.minDepth      = depth_range.min_depth;
		viewport.maxDepth      = depth_range.max_depth;
	}

	if (!dynamic_params->vk_dynamic_state_scissor)
	{
		scissor.offset = {dynamic_params->scissor_ltrb[0], dynamic_params->scissor_ltrb[1]};
		scissor.extent = {static_cast<uint32_t>(dynamic_params->scissor_ltrb[2] - dynamic_params->scissor_ltrb[0]),
		                  static_cast<uint32_t>(dynamic_params->scissor_ltrb[3] - dynamic_params->scissor_ltrb[1])};
	}

	const bool depth_clip_control_supported = g_render_ctx->GetGraphicCtx()->depth_clip_control_supported;
	EXIT_NOT_IMPLEMENTED(!static_params->dx_clip_space && !depth_clip_control_supported);

	VkPipelineViewportDepthClipControlCreateInfoEXT depth_clip_control {};
	depth_clip_control.sType            = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT;
	depth_clip_control.pNext            = nullptr;
	depth_clip_control.negativeOneToOne = (static_params->dx_clip_space ? VK_FALSE : VK_TRUE);

	VkPipelineViewportStateCreateInfo viewport_state {};
	viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.pNext         = (depth_clip_control_supported ? &depth_clip_control : nullptr);
	viewport_state.flags         = 0;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports    = (dynamic_params->vk_dynamic_state_viewport ? nullptr : &viewport);
	viewport_state.scissorCount  = 1;
	viewport_state.pScissors     = (dynamic_params->vk_dynamic_state_scissor ? nullptr : &scissor);

	VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
	cull_mode |= (static_params->cull_back ? VK_CULL_MODE_BACK_BIT : 0u);
	cull_mode |= (static_params->cull_front ? VK_CULL_MODE_FRONT_BIT : 0u);

	VkFrontFace front_face = (static_params->face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);

	bool depth_clip_supported = g_render_ctx->GetGraphicCtx()->depth_clip_enable_supported;

	VkPipelineRasterizationDepthClipStateCreateInfoEXT clip_ext {};
	clip_ext.sType           = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
	clip_ext.pNext           = nullptr;
	clip_ext.flags           = 0;
	clip_ext.depthClipEnable = VK_FALSE;

	VkPipelineRasterizationStateCreateInfo rasterizer {};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	// Without VK_EXT_depth_clip_enable, emulate depthClipEnable=FALSE via the core
	// depthClampEnable (their behaviours are complementary).
	rasterizer.pNext                   = (depth_clip_supported ? &clip_ext : nullptr);
	rasterizer.flags                   = 0;
	rasterizer.depthClampEnable        = (depth_clip_supported ? VK_FALSE : VK_TRUE);
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode                = cull_mode;
	rasterizer.frontFace               = front_face;
	rasterizer.depthBiasEnable         = static_params->depth_bias_enable ? VK_TRUE : VK_FALSE;
	rasterizer.depthBiasConstantFactor = dynamic_params->depth_bias_constant_factor;
	rasterizer.depthBiasClamp          = dynamic_params->depth_bias_clamp;
	rasterizer.depthBiasSlopeFactor    = dynamic_params->depth_bias_slope_factor;
	rasterizer.lineWidth               = dynamic_params->line_width;

	VkPipelineMultisampleStateCreateInfo multisampling {};
	multisampling.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.pNext                 = nullptr;
	multisampling.flags                 = 0;
	multisampling.sampleShadingEnable   = static_params->sample_shading_enable ? VK_TRUE : VK_FALSE;
	multisampling.rasterizationSamples  = static_params->rasterization_samples;
	multisampling.minSampleShading      = 1.0f;
	multisampling.pSampleMask           = nullptr;
	multisampling.alphaToCoverageEnable = VK_FALSE;
	multisampling.alphaToOneEnable      = VK_FALSE;

	VkSampleLocationEXT sample_location_values[kVulkanSampleLocationMaxCount] = {};
	VkSampleLocationsInfoEXT sample_location_info {};
	VkPipelineSampleLocationsStateCreateInfoEXT sample_location_pipeline_state {};
	if (VulkanSampleLocationsEnabled(static_params->sample_locations))
	{
		EXIT_NOT_IMPLEMENTED(!VulkanSampleLocationsPopulateInfo(static_params->sample_locations, sample_location_values,
		                                                       &sample_location_info));
		sample_location_pipeline_state.sType                 = VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT;
		sample_location_pipeline_state.sampleLocationsEnable = VK_TRUE;
		sample_location_pipeline_state.sampleLocationsInfo   = sample_location_info;
		multisampling.pNext                                  = &sample_location_pipeline_state;
	}

	// CB_TARGET_MASK: 4 bits per MRT (RGBA). One blend attachment per active target.
	EXIT_NOT_IMPLEMENTED(static_params->color_targets_num == 0 || static_params->color_targets_num > 8);
	VkPipelineColorBlendAttachmentState color_blend_attachments[8] {};
	VkBool32                            color_write_enables[8] {};
	for (uint32_t rt = 0; rt < static_params->color_targets_num; rt++)
	{
		const uint32_t        nibble           = static_params->color_mask[rt] & 0xFu;
		VkColorComponentFlags color_write_mask = 0;
		if ((nibble & 0x1u) != 0)
		{
			color_write_mask |= static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_R_BIT);
		}
		if ((nibble & 0x2u) != 0)
		{
			color_write_mask |= static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_G_BIT);
		}
		if ((nibble & 0x4u) != 0)
		{
			color_write_mask |= static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_B_BIT);
		}
		if ((nibble & 0x8u) != 0)
		{
			color_write_mask |= static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_A_BIT);
		}

		auto& att               = color_blend_attachments[rt];
		att.colorWriteMask      = color_write_mask;
		att.blendEnable         = (static_params->blend_enable[rt] && !static_params->blend_bypass[rt]) ? VK_TRUE : VK_FALSE;
		const auto color_blend  = get_blend_factors(static_params->color_srcblend[rt], static_params->color_destblend[rt]);
		const auto alpha_blend  = (static_params->separate_alpha_blend[rt]
		                               ? get_blend_factors(static_params->alpha_srcblend[rt], static_params->alpha_destblend[rt])
		                               : color_blend);
		att.srcColorBlendFactor = color_blend.src;
		att.dstColorBlendFactor = color_blend.dst;
		att.colorBlendOp        = get_blend_op(static_params->color_comb_fcn[rt]);
		att.srcAlphaBlendFactor = alpha_blend.src;
		att.dstAlphaBlendFactor = alpha_blend.dst;
		att.alphaBlendOp        = (static_params->separate_alpha_blend[rt] ? get_blend_op(static_params->alpha_comb_fcn[rt]) : att.colorBlendOp);
		color_write_enables[rt] = (pipeline->dynamic_params->color_write_enable ? VK_TRUE : VK_FALSE);
	}

	bool cwe_supported = g_render_ctx->GetGraphicCtx()->color_write_enable_supported;

	VkPipelineColorWriteCreateInfoEXT color_write {};
	color_write.sType              = VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT;
	color_write.pNext              = nullptr;
	color_write.attachmentCount    = static_params->color_targets_num;
	color_write.pColorWriteEnables = color_write_enables;

	// Without VK_EXT_color_write_enable (MoltenVK) fold the enable bit into the
	// attachment's write mask instead of chaining the extension struct.
	if (!cwe_supported && !pipeline->dynamic_params->color_write_enable)
	{
		for (uint32_t rt = 0; rt < static_params->color_targets_num; rt++)
		{
			color_blend_attachments[rt].colorWriteMask = 0;
		}
	}

	VkPipelineColorBlendStateCreateInfo color_blending {};
	color_blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blending.pNext           = (cwe_supported ? &color_write : nullptr);
	color_blending.flags           = 0;
	color_blending.logicOpEnable   = VK_FALSE;
	color_blending.logicOp         = VK_LOGIC_OP_COPY;
	color_blending.attachmentCount = static_params->color_targets_num;
	color_blending.pAttachments    = color_blend_attachments;
	if (!dynamic_params->vk_dynamic_state_blend_constants)
	{
		color_blending.blendConstants[0] = dynamic_params->blend_color_red;
		color_blending.blendConstants[1] = dynamic_params->blend_color_green;
		color_blending.blendConstants[2] = dynamic_params->blend_color_blue;
		color_blending.blendConstants[3] = dynamic_params->blend_color_alpha;
	}

	VkDescriptorSetLayout set_layouts[2]  = {};
	uint32_t              set_layouts_num = 0;

	VkPushConstantRange push_constant_info[2];
	uint32_t            push_constant_info_num = 0;

	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num, vs_input_info->bind,
	             /*additional_params->vs_bind,*/ VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);
	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num, ps_input_info->bind,
	             /*additional_params->ps_bind,*/ VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = 0;
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges    = push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr, &pipeline->pipeline_layout);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline_layout == nullptr);

	VkPipelineDepthStencilStateCreateInfo depth_stencil_info {};
	depth_stencil_info.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil_info.pNext                 = nullptr;
	depth_stencil_info.flags                 = 0;
	depth_stencil_info.depthTestEnable       = (static_params->depth_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthWriteEnable      = (static_params->depth_write_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthCompareOp        = static_params->depth_compare_op;
	depth_stencil_info.depthBoundsTestEnable = (static_params->depth_bounds_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.stencilTestEnable     = (static_params->stencil_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.front.failOp          = static_params->stencil_front.failOp;
	depth_stencil_info.front.passOp          = static_params->stencil_front.passOp;
	depth_stencil_info.front.depthFailOp     = static_params->stencil_front.depthFailOp;
	depth_stencil_info.front.compareOp       = static_params->stencil_front.compareOp;
	depth_stencil_info.front.compareMask     = dynamic_params->stencil_front.compareMask;
	depth_stencil_info.front.writeMask       = dynamic_params->stencil_front.writeMask;
	depth_stencil_info.front.reference       = dynamic_params->stencil_front.reference;
	depth_stencil_info.back.failOp           = static_params->stencil_back.failOp;
	depth_stencil_info.back.passOp           = static_params->stencil_back.passOp;
	depth_stencil_info.back.depthFailOp      = static_params->stencil_back.depthFailOp;
	depth_stencil_info.back.compareOp        = static_params->stencil_back.compareOp;
	depth_stencil_info.back.compareMask      = dynamic_params->stencil_back.compareMask;
	depth_stencil_info.back.writeMask        = dynamic_params->stencil_back.writeMask;
	depth_stencil_info.back.reference        = dynamic_params->stencil_back.reference;
	depth_stencil_info.minDepthBounds        = static_params->depth_min_bounds;
	depth_stencil_info.maxDepthBounds        = static_params->depth_max_bounds;

	VkDynamicState dynamic_states[11]   = {};
	uint32_t       dynamic_states_count = 0;
	if (dynamic_params->vk_dynamic_state_line_width)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_LINE_WIDTH;
	}
	if (dynamic_params->vk_dynamic_state_stencil_compare_mask)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
	}
	if (dynamic_params->vk_dynamic_state_stencil_reference)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
	}
	if (dynamic_params->vk_dynamic_state_stencil_write_mask)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
	}
	if (dynamic_params->vk_dynamic_state_color_write_enable_ext && cwe_supported)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT;
	}
	if (dynamic_params->vk_dynamic_state_viewport)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_VIEWPORT;
	}
	if (dynamic_params->vk_dynamic_state_scissor)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_SCISSOR;
	}
	if (dynamic_params->vk_dynamic_state_blend_constants)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
	}
	if (dynamic_params->vk_dynamic_state_depth_bias)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
	}

	VkPipelineDynamicStateCreateInfo dynamic_state {};
	dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.pNext             = nullptr;
	dynamic_state.flags             = 0;
	dynamic_state.dynamicStateCount = dynamic_states_count;
	dynamic_state.pDynamicStates    = dynamic_states;

	VkGraphicsPipelineCreateInfo pipeline_info {};
	pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.pNext               = nullptr;
	pipeline_info.flags               = 0;
	pipeline_info.stageCount          = has_fragment_stage ? 2u : 1u;
	pipeline_info.pStages             = shader_stages;
	pipeline_info.pVertexInputState   = &vertex_input_info;
	pipeline_info.pInputAssemblyState = &input_assembly;
	pipeline_info.pTessellationState  = nullptr;
	pipeline_info.pViewportState      = &viewport_state;
	pipeline_info.pRasterizationState = &rasterizer;
	pipeline_info.pMultisampleState   = &multisampling;
	pipeline_info.pDepthStencilState  = (static_params->with_depth ? &depth_stencil_info : nullptr);
	pipeline_info.pColorBlendState    = &color_blending;
	pipeline_info.pDynamicState       = &dynamic_state;
	pipeline_info.layout              = pipeline->pipeline_layout;
	pipeline_info.renderPass          = render_pass;
	pipeline_info.subpass             = 0;
	pipeline_info.basePipelineHandle  = nullptr;
	pipeline_info.basePipelineIndex   = -1;

	EXIT_IF(pipeline->pipeline != nullptr);

	const auto vk_create_start = std::chrono::steady_clock::now();
	const VkResult create_result =
	    vkCreateGraphicsPipelines(gctx->device, gctx->pipeline_cache, 1, &pipeline_info, nullptr, &pipeline->pipeline);
	const auto vk_create_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - vk_create_start).count();
	DebugStatsRecordVkPipelineCreate(DebugStatsPipelineKind::Graphics, static_cast<uint64_t>(vk_create_ns));

	if (pipeline->pipeline == nullptr || create_result != VK_SUCCESS)
	{
		// Bounded diagnostic dump for pipeline create failures (env-gated).
		if (const char* dump_dir = std::getenv("KYTY_PIPELINE_FAIL_DUMP"))
		{
			if (dump_dir[0] != '\0')
			{
				const auto write_spv = [&](const char* name, const Vector<uint32_t>& words) {
					char path[1024];
					std::snprintf(path, sizeof(path), "%s/%s.spv", dump_dir, name);
					if (FILE* f = std::fopen(path, "wb"))
					{
						if (!words.IsEmpty())
						{
							std::fwrite(words.GetDataConst(), sizeof(uint32_t), static_cast<size_t>(words.Size()), f);
						}
						std::fclose(f);
						std::fprintf(stderr, "KYTY_PIPELINE_FAIL_DUMP wrote %s words=%u\n", path,
						             static_cast<unsigned>(words.Size()));
					}
				};
				write_spv("fail_vs", vs_shader);
				write_spv("fail_ps", ps_shader);
				std::fprintf(stderr,
				             "KYTY_PIPELINE_FAIL_DUMP result=%d vs_words=%u ps_words=%u stages=%u color_targets=%u depth=%d\n",
				             static_cast<int>(create_result), static_cast<unsigned>(vs_shader.Size()),
				             static_cast<unsigned>(ps_shader.Size()), has_fragment_stage ? 2u : 1u,
				             static_params->color_targets_num, static_params->with_depth ? 1 : 0);
			}
		}
		EXIT("vkCreateGraphicsPipelines failed: result=%d pipeline=%p\n", static_cast<int>(create_result),
		     static_cast<void*>(pipeline->pipeline));
	}

	if (frag_shader_module != nullptr)
	{
		vkDestroyShaderModule(gctx->device, frag_shader_module, nullptr);
	}
	vkDestroyShaderModule(gctx->device, vert_shader_module, nullptr);

	return pipeline;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static VulkanPipeline* CreatePipelineInternal(const ShaderComputeInputInfo* input_info, const Vector<uint32_t>& cs_shader,
                                              const PipelineStaticParameters* static_params, PipelineDynamicParameters* dynamic_params)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(static_params == nullptr);
	EXIT_IF(dynamic_params == nullptr);

	auto* pipeline           = new VulkanPipeline;
	pipeline->static_params  = static_params;
	pipeline->dynamic_params = dynamic_params;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	VkShaderModule comp_shader_module = nullptr;

	VkShaderModuleCreateInfo create_info {};

	create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext    = nullptr;
	create_info.flags    = 0;
	create_info.codeSize = static_cast<size_t>(cs_shader.Size()) * 4;
	create_info.pCode    = cs_shader.GetDataConst();
	vkCreateShaderModule(gctx->device, &create_info, nullptr, &comp_shader_module);

	EXIT_NOT_IMPLEMENTED(comp_shader_module == nullptr);

	VkPipelineShaderStageCreateInfo comp_shader_stage_info {};
	comp_shader_stage_info.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	comp_shader_stage_info.pNext               = nullptr;
	comp_shader_stage_info.flags               = 0;
	comp_shader_stage_info.stage               = VK_SHADER_STAGE_COMPUTE_BIT;
	comp_shader_stage_info.module              = comp_shader_module;
	comp_shader_stage_info.pName               = "main";
	comp_shader_stage_info.pSpecializationInfo = nullptr;

	VkDescriptorSetLayout set_layouts[1]  = {};
	uint32_t              set_layouts_num = 0;

	VkPushConstantRange push_constant_info[1];
	uint32_t            push_constant_info_num = 0;

	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num,
	             input_info->bind, /*additional_params->cs_bind,*/
	             VK_SHADER_STAGE_COMPUTE_BIT, DescriptorCache::Stage::Compute);

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = 0;
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges    = push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr, &pipeline->pipeline_layout);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline_layout == nullptr);

	VkComputePipelineCreateInfo info {};
	info.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.pNext              = nullptr;
	info.flags              = 0;
	info.stage              = comp_shader_stage_info;
	info.layout             = pipeline->pipeline_layout;
	info.basePipelineHandle = nullptr;
	info.basePipelineIndex  = -1;

	EXIT_IF(pipeline->pipeline != nullptr);

	const auto vk_create_start = std::chrono::steady_clock::now();
	const VkResult create_result =
	    vkCreateComputePipelines(gctx->device, gctx->pipeline_cache, 1, &info, nullptr, &pipeline->pipeline);
	const auto vk_create_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - vk_create_start).count();
	DebugStatsRecordVkPipelineCreate(DebugStatsPipelineKind::Compute, static_cast<uint64_t>(vk_create_ns));

	if (pipeline->pipeline == nullptr || create_result != VK_SUCCESS)
	{
		if (const char* dump_dir = std::getenv("KYTY_PIPELINE_FAIL_DUMP"))
		{
			if (dump_dir[0] != '\0')
			{
				char path[1024];
				std::snprintf(path, sizeof(path), "%s/fail_cs.spv", dump_dir);
				if (FILE* f = std::fopen(path, "wb"))
				{
					if (!cs_shader.IsEmpty())
					{
						std::fwrite(cs_shader.GetDataConst(), sizeof(uint32_t), static_cast<size_t>(cs_shader.Size()), f);
					}
					std::fclose(f);
					std::fprintf(stderr, "KYTY_PIPELINE_FAIL_DUMP wrote %s words=%u result=%d\n", path,
					             static_cast<unsigned>(cs_shader.Size()), static_cast<int>(create_result));
				}
			}
		}
		EXIT("vkCreateComputePipelines failed: result=%d pipeline=%p\n", static_cast<int>(create_result),
		     static_cast<void*>(pipeline->pipeline));
	}

	vkDestroyShaderModule(gctx->device, comp_shader_module, nullptr);

	return pipeline;
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const
{
	// NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c)
	return (0 == memcmp(this, &other, sizeof(struct PipelineStaticParameters)));
}

bool PipelineDynamicParameters::operator==(const PipelineDynamicParameters& other) const
{
	EXIT_IF(vk_dynamic_state_line_width != other.vk_dynamic_state_line_width ||
	        vk_dynamic_state_stencil_compare_mask != other.vk_dynamic_state_stencil_compare_mask ||
	        vk_dynamic_state_stencil_write_mask != other.vk_dynamic_state_stencil_write_mask ||
	        vk_dynamic_state_stencil_reference != other.vk_dynamic_state_stencil_reference ||
	        vk_dynamic_state_color_write_enable_ext != other.vk_dynamic_state_color_write_enable_ext ||
	        vk_dynamic_state_viewport != other.vk_dynamic_state_viewport || vk_dynamic_state_scissor != other.vk_dynamic_state_scissor ||
	        vk_dynamic_state_blend_constants != other.vk_dynamic_state_blend_constants ||
	        vk_dynamic_state_depth_bias != other.vk_dynamic_state_depth_bias);

	if (!vk_dynamic_state_line_width)
	{
		if (line_width != other.line_width)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_stencil_compare_mask)
	{
		if (stencil_front.compareMask != other.stencil_front.compareMask || stencil_back.compareMask != other.stencil_back.compareMask)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_stencil_write_mask)
	{
		if (stencil_front.writeMask != other.stencil_front.writeMask || stencil_back.writeMask != other.stencil_back.writeMask)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_stencil_reference)
	{
		if (stencil_front.reference != other.stencil_front.reference || stencil_back.reference != other.stencil_back.reference)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_color_write_enable_ext)
	{
		if (color_write_enable != other.color_write_enable)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_viewport)
	{
		if (viewport_scale[0] != other.viewport_scale[0] || viewport_scale[1] != other.viewport_scale[1] ||
		    viewport_scale[2] != other.viewport_scale[2] || viewport_offset[0] != other.viewport_offset[0] ||
		    viewport_offset[1] != other.viewport_offset[1] || viewport_offset[2] != other.viewport_offset[2] ||
		    viewport_depth_clamp[0] != other.viewport_depth_clamp[0] || viewport_depth_clamp[1] != other.viewport_depth_clamp[1])
		{
			return false;
		}
	}
	if (!vk_dynamic_state_scissor)
	{
		if (scissor_ltrb[0] != other.scissor_ltrb[0] || scissor_ltrb[1] != other.scissor_ltrb[1] ||
		    scissor_ltrb[2] != other.scissor_ltrb[2] || scissor_ltrb[3] != other.scissor_ltrb[3])
		{
			return false;
		}
	}
	if (!vk_dynamic_state_blend_constants)
	{
		if (blend_color_red != other.blend_color_red || blend_color_green != other.blend_color_green ||
		    blend_color_blue != other.blend_color_blue || blend_color_alpha != other.blend_color_alpha)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_depth_bias)
	{
		if (depth_bias_constant_factor != other.depth_bias_constant_factor || depth_bias_clamp != other.depth_bias_clamp ||
		    depth_bias_slope_factor != other.depth_bias_slope_factor)
		{
			return false;
		}
	}
	return true;
}

void PipelineCache::DeletePipelineInternal(uint32_t id)
{
	EXIT_NOT_IMPLEMENTED(!m_pipelines.IndexValid(id));

	Pipeline& p = m_pipelines[id];

	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(p.pipeline == nullptr);
	EXIT_IF(p.pipeline->pipeline == nullptr);
	EXIT_IF(p.pipeline->pipeline_layout == nullptr);
	EXIT_IF(p.static_params == nullptr);
	EXIT_IF(p.dynamic_params == nullptr);

	EXIT_IF(p.pipeline->static_params != p.static_params);
	EXIT_IF(p.pipeline->dynamic_params != p.dynamic_params);

	DumpPipeline("delete", id);

	delete p.static_params;
	delete p.dynamic_params;

	p.static_params  = nullptr;
	p.dynamic_params = nullptr;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	vkDestroyPipeline(gctx->device, p.pipeline->pipeline, nullptr);
	vkDestroyPipelineLayout(gctx->device, p.pipeline->pipeline_layout, nullptr);

	delete p.pipeline;

	p.pipeline = nullptr;
}

VulkanPipeline* PipelineCache::Find(const Pipeline& p) const
{
	for (const auto& pn: m_pipelines)
	{
		if (pn.pipeline != nullptr && p.render_pass_id == pn.render_pass_id && p.vs_shader_id == pn.vs_shader_id &&
		    p.ps_shader_id == pn.ps_shader_id && p.cs_shader_id == pn.cs_shader_id && *p.static_params == *pn.static_params &&
		    *p.dynamic_params == *pn.dynamic_params)
		{
			return pn.pipeline;
		}
	}
	return nullptr;
}

void PipelineCache::SaveDriverCacheIfDue()
{
	auto* ctx = g_render_ctx->GetGraphicCtx();
	if (ctx == nullptr || ctx->physical_device == nullptr || ctx->device == nullptr || ctx->pipeline_cache == nullptr)
	{
		return;
	}
	if (m_driver_cache_write_budget_exhausted)
	{
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	const auto elapsed_seconds =
	    m_driver_cache_attempted_once
	        ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now - m_last_driver_cache_attempt).count())
	        : 0u;
	if (!PipelineCacheStoreCheckpointDue(m_driver_cache_dirty, m_driver_cache_attempted_once, elapsed_seconds))
	{
		return;
	}

	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(ctx->physical_device, &properties);

	const size_t budget           = PipelineCacheStoreSessionWriteBudgetBytes();
	const size_t remaining_budget = m_driver_cache_bytes_attempted < budget ? budget - m_driver_cache_bytes_attempted : 0u;
	size_t       attempted_size   = 0;
	const auto   save_start       = std::chrono::steady_clock::now();
	const auto   save_result      = PipelineCacheStoreSave(ctx->device, ctx->pipeline_cache, properties, remaining_budget, &attempted_size);
	const auto   save_elapsed_ns =
	    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - save_start).count());
	DebugStatsPipelineCacheCheckpointOutcome checkpoint_outcome = DebugStatsPipelineCacheCheckpointOutcome::Failed;
	if (save_result == PipelineCacheStoreSaveResult::Written)
	{
		checkpoint_outcome = DebugStatsPipelineCacheCheckpointOutcome::Written;
	} else if (save_result == PipelineCacheStoreSaveResult::BudgetExceeded)
	{
		checkpoint_outcome = DebugStatsPipelineCacheCheckpointOutcome::BudgetExceeded;
	}
	DebugStatsRecordPipelineCacheCheckpoint(checkpoint_outcome, static_cast<uint64_t>(attempted_size), save_elapsed_ns);
	m_driver_cache_attempted_once         = true;
	m_last_driver_cache_attempt           = now;
	m_driver_cache_bytes_attempted        = PipelineCacheStoreAccountWriteAttempt(m_driver_cache_bytes_attempted, attempted_size);
	m_driver_cache_write_budget_exhausted = m_driver_cache_bytes_attempted >= budget;
	if (save_result == PipelineCacheStoreSaveResult::Written)
	{
		m_driver_cache_dirty = false;
		if (Config::GetPrintfDirection() != Log::Direction::Silent)
		{
			printf("Saved Vulkan pipeline cache: %zu bytes\n", attempted_size);
		}
	} else if (save_result == PipelineCacheStoreSaveResult::BudgetExceeded)
	{
		m_driver_cache_write_budget_exhausted = true;
	}
}

VulkanPipeline* PipelineCache::CreatePipeline(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
                                              const ShaderVertexInputInfo* vs_input_info, HW::Context* ctx, HW::Shader* sh_ctx,
                                              const ShaderPixelInputInfo* ps_input_info, VkPrimitiveTopology topology,
                                              const VulkanSampleLocationState& sample_locations)
{
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(framebuffer == nullptr);
	EXIT_IF(depth == nullptr);
	EXIT_IF(color == nullptr);

	Core::LockGuard lock(m_mutex);

	const auto&                vs_regs = sh_ctx->GetVs();
	const auto&                ps_regs = sh_ctx->GetPs();
	const auto&                sh_regs = ctx->GetShaderRegisters();
	const HW::BlendColor&      bclr    = ctx->GetBlendColor();
	const HW::ScreenViewport&  vp      = ctx->GetScreenViewport();
	const HW::ScanModeControl& smc     = ctx->GetScanModeControl();
	const HW::ModeControl&     mc      = ctx->GetModeControl();

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		ShaderDbgDumpInputInfo(vs_input_info);
		ShaderDbgDumpInputInfo(ps_input_info);
	}

	auto vs_id = ShaderGetIdVS(&vs_regs, vs_input_info);
	auto ps_id = ShaderGetIdPS(&ps_regs, ps_input_info);
	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	Pipeline p {};
	p.render_pass_id = framebuffer->render_pass_id;
	p.ps_shader_id   = ps_id;
	p.vs_shader_id   = vs_id;
	p.static_params  = new PipelineStaticParameters {};
	p.dynamic_params = new PipelineDynamicParameters {};

	p.dynamic_params->vk_dynamic_state_line_width           = true;
	p.dynamic_params->vk_dynamic_state_stencil_compare_mask = true;
	p.dynamic_params->vk_dynamic_state_stencil_reference    = true;
	p.dynamic_params->vk_dynamic_state_stencil_write_mask   = true;
	p.dynamic_params->vk_dynamic_state_viewport             = true;
	p.dynamic_params->vk_dynamic_state_scissor              = true;
	p.dynamic_params->vk_dynamic_state_blend_constants      = true;
	p.dynamic_params->vk_dynamic_state_depth_bias           = true;
	p.dynamic_params->color_write_enable                    = true;

	EXIT_NOT_IMPLEMENTED(depth->depth_test_enable && ps_input_info->ps_execute_on_noop);

	const auto scissor = State::ResolveScissor(vp, smc, 0);

	p.dynamic_params->viewport_scale[0]  = vp.viewports[0].xscale;
	p.dynamic_params->viewport_scale[1]  = vp.viewports[0].yscale;
	p.dynamic_params->viewport_scale[2]  = vp.viewports[0].zscale;
	p.dynamic_params->viewport_offset[0] = vp.viewports[0].xoffset;
	p.dynamic_params->viewport_offset[1] = vp.viewports[0].yoffset;
	p.dynamic_params->viewport_offset[2] = vp.viewports[0].zoffset;
	p.dynamic_params->viewport_depth_clamp[0] = vp.viewports[0].zmin;
	p.dynamic_params->viewport_depth_clamp[1] = vp.viewports[0].zmax;
	p.static_params->dx_clip_space            = ctx->GetClipControl().dx_clip_space;
	p.dynamic_params->scissor_ltrb[0]    = scissor.left;
	p.dynamic_params->scissor_ltrb[1]    = scissor.top;
	p.dynamic_params->scissor_ltrb[2]    = scissor.right;
	p.dynamic_params->scissor_ltrb[3]    = scissor.bottom;
	if (color->targets_num == 1 && color->attachment[0].type == RenderColorType::DisplayBuffer &&
	    color->attachment[0].vulkan_buffer != nullptr && color->attachment[0].vulkan_buffer->IsResolutionScaled())
	{
		RenderResolutionTransform transform;
		const ResolutionExtent        guest {color->attachment[0].width, color->attachment[0].height};
		const ResolutionExtent        host {color->attachment[0].vulkan_buffer->extent.width, color->attachment[0].vulkan_buffer->extent.height};
		EXIT_NOT_IMPLEMENTED(CreateRenderResolutionTransform(guest, host, &transform) != RenderResolutionTransformStatus::Success);

		const auto         xy = State::ResolveViewportXy(p.dynamic_params->viewport_scale[0], p.dynamic_params->viewport_offset[0],
		                                                 p.dynamic_params->viewport_scale[1], p.dynamic_params->viewport_offset[1]);
		ResolutionViewport guest_viewport {xy.x, xy.y, xy.width, xy.height, 0.0, 1.0};
		ResolutionViewport host_viewport;
		EXIT_NOT_IMPLEMENTED(MapRenderResolutionViewport(transform, guest_viewport, &host_viewport) != RenderResolutionTransformStatus::Success);
		p.dynamic_params->viewport_scale[0]  = static_cast<float>(host_viewport.width * 0.5);
		p.dynamic_params->viewport_scale[1]  = static_cast<float>(host_viewport.height * 0.5);
		p.dynamic_params->viewport_offset[0] = static_cast<float>(host_viewport.x + host_viewport.width * 0.5);
		p.dynamic_params->viewport_offset[1] = static_cast<float>(host_viewport.y + host_viewport.height * 0.5);

		ResolutionScissorRect       host_scissor;
		const ResolutionScissorRect guest_scissor {scissor.left, scissor.top, scissor.right, scissor.bottom};
		EXIT_NOT_IMPLEMENTED(MapRenderResolutionScissor(transform, guest_scissor, &host_scissor) != RenderResolutionTransformStatus::Success);
		p.dynamic_params->scissor_ltrb[0] = static_cast<int>(host_scissor.left);
		p.dynamic_params->scissor_ltrb[1] = static_cast<int>(host_scissor.top);
		p.dynamic_params->scissor_ltrb[2] = static_cast<int>(host_scissor.right);
		p.dynamic_params->scissor_ltrb[3] = static_cast<int>(host_scissor.bottom);
	}
	const auto framebuffer_scissor = State::ClampScissorToExtent(
	    {p.dynamic_params->scissor_ltrb[0], p.dynamic_params->scissor_ltrb[1], p.dynamic_params->scissor_ltrb[2],
	     p.dynamic_params->scissor_ltrb[3]},
	    framebuffer->extent.width, framebuffer->extent.height);
	p.dynamic_params->scissor_ltrb[0] = framebuffer_scissor.left;
	p.dynamic_params->scissor_ltrb[1] = framebuffer_scissor.top;
	p.dynamic_params->scissor_ltrb[2] = framebuffer_scissor.right;
	p.dynamic_params->scissor_ltrb[3] = framebuffer_scissor.bottom;
	p.static_params->topology                 = topology;
	p.static_params->rasterization_samples    = resolve_render_attachment_sample_count(*color, *depth);
	p.static_params->sample_locations         = sample_locations;
	EXIT_NOT_IMPLEMENTED(p.static_params->sample_locations.sample_count != p.static_params->rasterization_samples);
	p.static_params->sample_shading_enable    = p.static_params->rasterization_samples != VK_SAMPLE_COUNT_1_BIT &&
	                                        ctx->GetEqaaControl().ps_iter_samples != 0;
	EXIT_NOT_IMPLEMENTED(p.static_params->sample_shading_enable && !gctx->sample_rate_shading_supported);
	p.static_params->with_depth               = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	p.static_params->depth_test_enable        = depth->depth_test_enable;
	p.static_params->depth_write_enable       = (depth->depth_write_enable && !depth->suppress_depth_write);
	p.static_params->depth_compare_op         = depth->depth_compare_op;
	p.static_params->depth_bounds_test_enable = depth->depth_bounds_test_enable;
	p.static_params->depth_min_bounds         = depth->depth_min_bounds;
	p.static_params->depth_max_bounds         = depth->depth_max_bounds;
	p.static_params->stencil_test_enable      = depth->stencil_test_enable;
	p.static_params->stencil_front            = depth->stencil_static_front;
	p.static_params->stencil_back             = depth->stencil_static_back;
	p.static_params->color_targets_num        = color->targets_num;
	if (!RenderColorHasActiveTarget(*color))
	{
		// Depth-only: FramebufferCache attaches one dummy color image so the
		// Vulkan render pass stays valid; the pipeline must match that count
		// with a zero write mask (no color output).
		EXIT_NOT_IMPLEMENTED(!p.static_params->with_depth);
		p.static_params->color_targets_num = 1;
		p.static_params->color_mask[0]     = 0;
		p.static_params->blend_enable[0]   = false;
	} else
	{
		EXIT_NOT_IMPLEMENTED(p.static_params->color_targets_num > 8);
		for (uint32_t rt = 0; rt < p.static_params->color_targets_num; rt++)
		{
			if (!RenderColorSlotConfigured(*color, rt))
			{
				p.static_params->color_mask[rt]   = 0;
				p.static_params->blend_enable[rt] = false;
				continue;
			}
			const auto& blend                         = ctx->GetBlendControl(rt);
			p.static_params->color_mask[rt]           =
			    State::ResolveColorWriteMask(ctx->GetRenderTargetMask(), sh_regs.m_cbShaderMask, rt);
			p.static_params->color_srcblend[rt]       = blend.color_srcblend;
			p.static_params->color_comb_fcn[rt]       = blend.color_comb_fcn;
			p.static_params->color_destblend[rt]      = blend.color_destblend;
			p.static_params->alpha_srcblend[rt]       = blend.alpha_srcblend;
			p.static_params->alpha_comb_fcn[rt]       = blend.alpha_comb_fcn;
			p.static_params->alpha_destblend[rt]      = blend.alpha_destblend;
			p.static_params->separate_alpha_blend[rt] = blend.separate_alpha_blend;
			p.static_params->blend_enable[rt]         = blend.enable;
			p.static_params->blend_bypass[rt]         = ctx->GetRenderTarget(rt).info.blend_bypass;
		}
	}
	p.static_params->cull_back  = mc.cull_back;
	p.static_params->cull_front = mc.cull_front;
	p.static_params->face       = mc.face;
	const auto depth_bias       = State::ResolveDepthBias(mc, ctx->GetPolygonOffset());
	EXIT_NOT_IMPLEMENTED(depth_bias.enabled && (!std::isfinite(depth_bias.constant_factor) || !std::isfinite(depth_bias.clamp) ||
	                                            !std::isfinite(depth_bias.slope_factor)));
	EXIT_NOT_IMPLEMENTED(depth_bias.enabled && depth_bias.clamp != 0.0f && !g_render_ctx->GetGraphicCtx()->depth_bias_clamp_supported);
	p.static_params->depth_bias_enable           = depth_bias.enabled;
	p.dynamic_params->depth_bias_constant_factor = depth_bias.constant_factor;
	p.dynamic_params->depth_bias_clamp           = depth_bias.clamp;
	p.dynamic_params->depth_bias_slope_factor    = depth_bias.slope_factor;
	p.dynamic_params->blend_color_red            = bclr.red;
	p.dynamic_params->blend_color_green          = bclr.green;
	p.dynamic_params->blend_color_blue           = bclr.blue;
	p.dynamic_params->blend_color_alpha          = bclr.alpha;

	p.dynamic_params->line_width    = ctx->GetLineWidth();
	p.dynamic_params->stencil_front = depth->stencil_dynamic_front;
	p.dynamic_params->stencil_back  = depth->stencil_dynamic_back;
	// CB_COLOR_CONTROL.mode selects special color operations. The guest's
	// per-attachment write permission comes from CB_TARGET_MASK above; using
	// mode here disables valid scanout and render-target draws on other modes.
	p.dynamic_params->color_write_enable = true;

	const auto lookup_start = std::chrono::steady_clock::now();
	auto*      found        = Find(p);
	const auto lookup_ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - lookup_start).count();
	DebugStatsRecordPipelineLookup(DebugStatsPipelineKind::Graphics, found != nullptr, static_cast<uint64_t>(lookup_ns));

	if (found != nullptr)
	{
		*found->dynamic_params = *p.dynamic_params;
		delete p.static_params;
		delete p.dynamic_params;
		SaveDriverCacheIfDue();
		return found;
	}

	const auto miss_start = std::chrono::steady_clock::now();

	auto* translation_cache = g_render_ctx->GetShaderTranslationCache();
	EXIT_IF(translation_cache == nullptr);
	const auto optimization = Config::GetShaderOptimizationType();
	const bool next_gen     = Config::IsNextGen();
	const bool debug_printf = Config::SpirvDebugPrintfEnabled();
	const auto vs_translation =
	    translation_cache->GetOrCompile(ShaderModuleKey::Create(vs_id, ShaderModuleStage::Vertex, optimization, next_gen, debug_printf),
	                                    [&]
	                                    {
		                                    auto vs_code = ShaderParseVS(&vs_regs, &sh_regs);
		                                    return ShaderRecompileVS(vs_code, vs_input_info);
	                                    });
	DebugStatsRecordShaderTranslationCache(vs_translation.hit, vs_translation.evicted);
	ShaderTranslationCacheResult ps_translation;
	if (ps_input_info->stage_enabled)
	{
		ps_translation =
		    translation_cache->GetOrCompile(ShaderModuleKey::Create(ps_id, ShaderModuleStage::Pixel, optimization, next_gen, debug_printf),
		                                    [&]
		                                    {
			                                    auto ps_code = ShaderParsePS(&ps_regs, &sh_regs);
			                                    return ShaderRecompilePS(ps_code, ps_input_info);
		                                    });
		DebugStatsRecordShaderTranslationCache(ps_translation.hit, ps_translation.evicted);
	}

	EXIT_IF(vs_translation.binary.IsEmpty());
	if (ps_input_info->stage_enabled && ps_translation.binary.IsEmpty())
	{
		std::fprintf(stderr,
		             "Pixel shader translation returned no binary: addr=0x%016" PRIx64 " checksum=0x%016" PRIx64 " hash=0x%08" PRIx32
		             " crc=0x%08" PRIx32 " ids=%u cache_hit=%u\n",
		             ps_regs.ps_regs.data_addr, ps_regs.ps_regs.chksum, ps_id.hash0, ps_id.crc32, static_cast<uint32_t>(ps_id.ids.Size()),
		             ps_translation.hit ? 1u : 0u);
	}
	EXIT_IF(ps_input_info->stage_enabled && ps_translation.binary.IsEmpty());

	p.pipeline = CreatePipelineInternal(framebuffer->render_pass, vs_input_info, vs_translation.binary, ps_input_info,
	                                    ps_translation.binary, p.static_params, p.dynamic_params);

	EXIT_NOT_IMPLEMENTED(p.pipeline == nullptr);
	p.pipeline->framebuffer_extent = framebuffer->extent;

	bool updated = false;
	int  index   = 0;
	for (auto& pn: m_pipelines)
	{
		if (pn.pipeline == nullptr)
		{
			pn      = p;
			updated = true;

			DumpPipeline("create", index);

			break;
		}
		index++;
	}

	if (!updated)
	{
		if (m_pipelines.Size() >= PipelineCache::MAX_PIPELINES)
		{
			// Bound host pipeline object growth for long sessions: recycle a
			// slot instead of EXIT_NOT_IMPLEMENTED when variants exceed the cap.
			const uint32_t evict = PipelineCacheNextEvictIndex(m_pipelines.Size(), &m_evict_cursor);
			DebugStatsRecordPipelineEviction();
			DeletePipelineInternal(evict);
			m_pipelines[static_cast<int>(evict)] = p;
			DumpPipeline("create", static_cast<int>(evict));
		} else
		{
			EXIT_IF(m_pipelines.Size() != static_cast<uint32_t>(index));

			m_pipelines.Add(p);

			DumpPipeline("create", index);
		}
	}

	m_driver_cache_dirty = true;
	SaveDriverCacheIfDue();
	const auto miss_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - miss_start).count();
	DebugStatsRecordPipelineMiss(DebugStatsPipelineKind::Graphics, static_cast<uint64_t>(miss_ns));
	return p.pipeline;
}

VulkanPipeline* PipelineCache::CreatePipeline(const ShaderComputeInputInfo* input_info, const HW::ComputeShaderInfo* cs_regs,
                                              const HW::ShaderRegisters* sh_regs)
{
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(cs_regs == nullptr);
	EXIT_IF(sh_regs == nullptr);

	Core::LockGuard lock(m_mutex);

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cs_id = ShaderGetIdCS(cs_regs, input_info);

	Pipeline p {};
	p.cs_shader_id   = cs_id;
	p.static_params  = new PipelineStaticParameters {};
	p.dynamic_params = new PipelineDynamicParameters {};

	p.dynamic_params->vk_dynamic_state_line_width           = true;
	p.dynamic_params->vk_dynamic_state_stencil_compare_mask = true;
	p.dynamic_params->vk_dynamic_state_stencil_reference    = true;
	p.dynamic_params->vk_dynamic_state_stencil_write_mask   = true;
	p.dynamic_params->color_write_enable                    = true;

	const auto lookup_start = std::chrono::steady_clock::now();
	auto*      found        = Find(p);
	const auto lookup_ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - lookup_start).count();
	DebugStatsRecordPipelineLookup(DebugStatsPipelineKind::Compute, found != nullptr, static_cast<uint64_t>(lookup_ns));

	if (found != nullptr)
	{
		*found->dynamic_params = *p.dynamic_params;
		delete p.static_params;
		delete p.dynamic_params;
		SaveDriverCacheIfDue();
		return found;
	}

	const auto miss_start = std::chrono::steady_clock::now();

	auto* translation_cache = g_render_ctx->GetShaderTranslationCache();
	EXIT_IF(translation_cache == nullptr);
	const auto cs_translation =
	    translation_cache->GetOrCompile(ShaderModuleKey::Create(cs_id, ShaderModuleStage::Compute, Config::GetShaderOptimizationType(),
	                                                            Config::IsNextGen(), Config::SpirvDebugPrintfEnabled()),
	                                    [&]
	                                    {
		                                    auto cs_code = ShaderParseCS(cs_regs, sh_regs);
		                                    return ShaderRecompileCS(cs_code, input_info);
	                                    });
	DebugStatsRecordShaderTranslationCache(cs_translation.hit, cs_translation.evicted);
	EXIT_IF(cs_translation.binary.IsEmpty());

	p.pipeline = CreatePipelineInternal(input_info, cs_translation.binary, p.static_params, p.dynamic_params /*, params2*/);

	EXIT_NOT_IMPLEMENTED(p.pipeline == nullptr);

	bool updated = false;
	for (auto& pn: m_pipelines)
	{
		if (pn.pipeline == nullptr)
		{
			pn      = p;
			updated = true;
			break;
		}
	}

	if (!updated)
	{
		if (m_pipelines.Size() >= PipelineCache::MAX_PIPELINES)
		{
			const uint32_t evict = PipelineCacheNextEvictIndex(m_pipelines.Size(), &m_evict_cursor);
			DebugStatsRecordPipelineEviction();
			DeletePipelineInternal(evict);
			m_pipelines[static_cast<int>(evict)] = p;
		} else
		{
			m_pipelines.Add(p);
		}
	}

	m_driver_cache_dirty = true;
	SaveDriverCacheIfDue();
	const auto miss_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - miss_start).count();
	DebugStatsRecordPipelineMiss(DebugStatsPipelineKind::Compute, static_cast<uint64_t>(miss_ns));
	return p.pipeline;
}

void PipelineCache::DeletePipeline(VulkanPipeline* pipeline)
{
	Core::LockGuard lock(m_mutex);

	auto index = m_pipelines.Find(pipeline, [](auto r, auto p) { return p == r.pipeline; });

	EXIT_IF(!m_pipelines.IndexValid(index));

	if (m_pipelines.IndexValid(index))
	{
		DeletePipelineInternal(index);
	}
}

void PipelineCache::DeletePipelines(VulkanFramebuffer* framebuffer)
{
	EXIT_IF(framebuffer == nullptr);

	Core::LockGuard lock(m_mutex);

	int index = 0;
	for (auto& p: m_pipelines)
	{
		if (p.pipeline != nullptr && p.render_pass_id == framebuffer->render_pass_id)
		{
			DeletePipelineInternal(index);
		}
		index++;
	}
}

void PipelineCache::DeleteAllPipelines()
{
	Core::LockGuard lock(m_mutex);

	for (uint32_t index = 0; index < m_pipelines.Size(); index++)
	{
		DeletePipelineInternal(index);
	}
}

void PipelineCache::DumpToFile(Core::File* f, const Pipeline& p) {}

void PipelineCache::DumpPipeline(const char* action, uint32_t id)
{
	EXIT_IF(!m_pipelines.IndexValid(id));
	EXIT_IF(action == nullptr);

	static std::atomic_int dump_id = 0;

	if (Config::PipelineDumpEnabled())
	{
		Core::File f;
		String     file_name = Config::GetPipelineDumpFolder().FixDirectorySlash() +
		                       String::FromPrintf("%04d_%04d_pipeline_%u_%s.log", GraphicsRunGetFrameNum(), dump_id++, id, action);
		Core::File::CreateDirectories(file_name.DirectoryWithoutFilename());
		f.Create(file_name);
		if (f.IsInvalid())
		{
			printf(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, file_name.C_Str());
			return;
		}
		Pipeline& p = m_pipelines[id];
		DumpToFile(&f, p);
		f.Close();
	}
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
