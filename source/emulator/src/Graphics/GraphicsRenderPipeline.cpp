#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"

#include "GraphicsRenderInternal.h"
#include "GraphicsRenderPipelineLimits.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ComputePipelinePrecompile.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/DiagnosticDump.h"
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

#include "spirv/unified1/spirv.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

// Device selection requires maxSamplerAnisotropy >= 16, so every level is valid.
float HostTextureAnisotropyRatio()
{
	switch (Config::GetTextureAnisotropy())
	{
		case Config::TextureAnisotropy::Guest: return 1.0f;
		case Config::TextureAnisotropy::X2: return 2.0f;
		case Config::TextureAnisotropy::X4: return 4.0f;
		case Config::TextureAnisotropy::X8: return 8.0f;
		case Config::TextureAnisotropy::X16: return 16.0f;
	}
	return 1.0f;
}

} // namespace

// SamplerCache::GetSamplerId, PipelineCache, CreatePipelineInternal

VulkanColorAttachmentBlendAdmission VulkanValidateColorAttachmentBlendRequest(VkPhysicalDevice physical_device,
                                                                              VkFormat attachment_format, bool blend_enable,
                                                                              bool blend_bypass)
{
	if (physical_device == VK_NULL_HANDLE || attachment_format == VK_FORMAT_UNDEFINED)
	{
		return VulkanColorAttachmentBlendAdmission::InvalidInput;
	}
	if (VulkanResolveColorAttachmentBlendEnable(blend_enable, blend_bypass) == VK_FALSE)
	{
		return VulkanColorAttachmentBlendAdmission::Supported;
	}

	VkFormatProperties properties {};
	vkGetPhysicalDeviceFormatProperties(physical_device, attachment_format, &properties);
	return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0u
	           ? VulkanColorAttachmentBlendAdmission::Supported
	           : VulkanColorAttachmentBlendAdmission::MissingFormatFeature;
}

uint64_t SamplerCache::GetSamplerId(const ShaderSamplerResource& r, State::ImageSampleOperation operation, bool allow_unnormalized,
                                    bool uses_r_coordinate)
{
	if (operation == State::ImageSampleOperation::Mixed)
	{
		EXIT("unsupported sampler contract: operation=mixed\n");
	}
	const auto& enabled_sampler     = g_render_ctx->GetGraphicCtx()->enabled_sampler_features;
	const auto  unnormalized_policy = State::ResolveUnnormalizedSamplerPolicy(r.ForceUnormCoords(), allow_unnormalized);
	VkSamplerAddressMode address[3] {};
	if (!VulkanResolveSamplerAddressMode(State::ResolveSamplerAddressMode(r.ClampX()), enabled_sampler, r.ForceUnormCoords(), &address[0]) ||
	    !VulkanResolveSamplerAddressMode(State::ResolveSamplerAddressMode(r.ClampY()), enabled_sampler, r.ForceUnormCoords(), &address[1]) ||
	    (uses_r_coordinate &&
	     !VulkanResolveSamplerAddressMode(State::ResolveSamplerAddressMode(r.ClampZ()), enabled_sampler, r.ForceUnormCoords(), &address[2])))
	{
		EXIT("unsupported sampler coordinate lowering: clamp=%u,%u,%u unnormalized=%u view_allows_unnormalized=%u operation=%u "
		     "mirror_clamp_enabled=%u uses_r=%u sampler=%08x %08x %08x %08x\n",
		     r.ClampX(), r.ClampY(), r.ClampZ(), r.ForceUnormCoords() ? 1u : 0u, allow_unnormalized ? 1u : 0u,
		     static_cast<uint32_t>(operation), enabled_sampler.mirror_clamp_to_edge, uses_r_coordinate ? 1u : 0u, r.fields[0],
		     r.fields[1], r.fields[2], r.fields[3]);
	}
	Core::LockGuard lock(m_mutex);
	uint32_t        m_samplers_size = m_samplers.Size();
	for (uint32_t i = 0; i < m_samplers_size; i++)
	{
		const auto& s = m_samplers.At(i);
		if (s.r.fields[0] == r.fields[0] && s.r.fields[1] == r.fields[1] && s.r.fields[2] == r.fields[2] &&
		    s.r.fields[3] == r.fields[3] && s.operation == operation && s.allow_unnormalized == allow_unnormalized &&
		    s.uses_r_coordinate == uses_r_coordinate)
		{
			return i;
		}
	}
	Sampler s;
	s.r                   = r;
	s.operation           = operation;
	s.allow_unnormalized  = allow_unnormalized;
	s.uses_r_coordinate   = uses_r_coordinate;
	s.vk                  = nullptr;

	auto mag_filter = r.XyMagFilter();
	auto min_filter = r.XyMinFilter();

	const auto anisotropy = State::ResolveSamplerAnisotropy(mag_filter, min_filter, r.MaxAnisoRatio(), HostTextureAnisotropyRatio());

	auto              mip_filter = r.MipFilter();
	const auto        lod_range  = State::ResolveSamplerLodRange(r.MinLod(), r.MaxLod(), r.LodBias(), mip_filter);
	const float       min_lod    = lod_range.min_lod;
	const float       max_lod    = lod_range.max_lod;

	VkSamplerCreateInfo sampler_info {};
	const auto          sampler_comparison = State::ResolveSamplerComparison(r.DepthCompareFunc(), operation);
	if (sampler_comparison.enabled && unnormalized_policy.disable_comparison)
	{
		EXIT("unsupported sampler contract: operation=%u unnormalized=1\n", static_cast<uint32_t>(operation));
	}

	auto get_compare_op = [](State::SamplerCompareOp compare)
	{
		switch (compare)
		{
			case State::SamplerCompareOp::Never: return VK_COMPARE_OP_NEVER;
			case State::SamplerCompareOp::Less: return VK_COMPARE_OP_LESS;
			case State::SamplerCompareOp::Equal: return VK_COMPARE_OP_EQUAL;
			case State::SamplerCompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
			case State::SamplerCompareOp::Greater: return VK_COMPARE_OP_GREATER;
			case State::SamplerCompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
			case State::SamplerCompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case State::SamplerCompareOp::Always: return VK_COMPARE_OP_ALWAYS;
		}
		return VK_COMPARE_OP_NEVER;
	};

	VkBorderColor border = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
	switch (r.BorderColorType())
	{
		case 0: border = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK; break;
		case 1: border = VK_BORDER_COLOR_INT_OPAQUE_BLACK; break;
		case 2:
			border = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
			break;
			KYTY_LOG_DEBUG("WARNING: unknown border color (continuing)\n");
	}

	sampler_info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.pNext                   = nullptr;
	sampler_info.flags                   = 0;
	sampler_info.magFilter               = (mag_filter == 0 || mag_filter == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	sampler_info.minFilter               = (min_filter == 0 || min_filter == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	sampler_info.mipmapMode              = (mip_filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST);
	sampler_info.addressModeU            = address[0];
	sampler_info.addressModeV            = address[1];
	sampler_info.addressModeW            = address[2];
	sampler_info.mipLodBias              = lod_range.lod_bias;
	sampler_info.anisotropyEnable        = (anisotropy.enabled ? VK_TRUE : VK_FALSE);
	sampler_info.maxAnisotropy           = anisotropy.max_ratio;
	sampler_info.compareEnable           = sampler_comparison.enabled ? VK_TRUE : VK_FALSE;
	sampler_info.compareOp               = get_compare_op(sampler_comparison.function);
	sampler_info.minLod                  = min_lod;
	sampler_info.maxLod                  = max_lod;
	sampler_info.borderColor             = border;
	sampler_info.unnormalizedCoordinates = (unnormalized_policy.enabled ? VK_TRUE : VK_FALSE);
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
	if (s.vk == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s.vk == nullptr condition ignored (continuing)\n"); }

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
		default: KYTY_LOG_DEBUG("WARNING: unknown blend factor %u (continuing)\n", factor); break;
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
		default: KYTY_LOG_DEBUG("WARNING: unknown blend operation %u (continuing)\n", op); break;
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
		EXIT_IF(set_layouts[*set_layouts_num] == VK_NULL_HANDLE);
		(*set_layouts_num)++;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static VulkanPipeline* CreatePipelineInternal(VkRenderPass render_pass, const ShaderVertexInputInfo* vs_input_info,
                                              const Vector<uint32_t>& vs_shader, const ShaderPixelInputInfo* ps_input_info,
                                              const Vector<uint32_t>& ps_shader, const Vector<uint32_t>& geometry_shader,
                                              const PipelineStaticParameters* static_params,
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
	if (static_params->sample_shading_enable && !gctx->sample_rate_shading_supported) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: static_params->sample_shading_enable && !gctx->sample_rate_shading_supported condition ignored (continuing)\n"); }

	VkShaderModule vert_shader_module = nullptr;
	VkShaderModule frag_shader_module = nullptr;
	VkShaderModule geometry_shader_module = nullptr;

	VkShaderModuleCreateInfo create_info {};

	create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext = nullptr;
	create_info.flags = 0;

	const bool has_fragment_stage = !ps_shader.IsEmpty();
	const auto check_subgroups = [gctx](const Vector<uint32_t>& binary, VkShaderStageFlagBits stage)
	{
		const auto check = ShaderCheckSubgroupModule(binary.GetDataConst(), binary.Size(), stage, gctx->subgroup_stages,
		                                              gctx->subgroup_operations, gctx->compute_wave_vulkan_state,
		                                              gctx->shader_maximal_reconvergence_enabled);
		if (!check.supported)
		{
			EXIT("subgroup-module admission stage=0x%x word=%zu required_ops=0x%x reason=%s\n",
			     static_cast<uint32_t>(stage), check.word, check.required_operations, check.reason);
		}
		return check.required_operations;
	};
	// Both cache hits and newly emitted modules pass through this boundary.
	const auto vertex_ops = check_subgroups(vs_shader, VK_SHADER_STAGE_VERTEX_BIT);
	const auto fragment_ops = has_fragment_stage ? check_subgroups(ps_shader, VK_SHADER_STAGE_FRAGMENT_BIT) : 0u;
	if (!geometry_shader.IsEmpty()) { (void)check_subgroups(geometry_shader, VK_SHADER_STAGE_GEOMETRY_BIT); }
	const auto select_native = [gctx](const ShaderNativeWaveInfo& wave, uint32_t requested, VkShaderStageFlagBits stage,
	                                  uint32_t operations, uint32_t spirv_version, const String8& detail)
	{
		if (operations == 0u) { return ShaderNativeSubgroupSelection {true, 0, false}; } // Host-built embedded module.
		// These stages use flags0. SPIR-V 1.6 itself permits varying widths.
		const auto selection = ShaderSelectNativeSubgroup(gctx->compute_wave_vulkan_state, stage, gctx->subgroup_size,
		                                                  wave.guest_wave_size, wave.proof == ShaderNativeWaveProof::LaneLocal ||
		                                                  wave.proof == ShaderNativeWaveProof::QuadLocal ||
		                                                  wave.proof == ShaderNativeWaveProof::FragmentPartialWave,
		                                                  wave.proof == ShaderNativeWaveProof::FragmentNeutral32,
		                                                  spirv_version >= 0x00010600u);
		const char* reason = wave.refusal_reason;
		if (reason == nullptr && wave.proof == ShaderNativeWaveProof::Unclassified) { reason = "unclassified guest wave"; }
		if (reason == nullptr && requested != 0 && requested != wave.guest_wave_size) { reason = "inconsistent guest width metadata"; }
		if (reason == nullptr && !selection.supported) { reason = "exact guest lane map or explicit width-neutral proof unavailable"; }
		if (reason != nullptr)
		{
			EXIT("native-wave admission stage=0x%x guest=%u proof=%u requested=%u host_default=%u host_range=%u..%u "
			     "spirv=0x%08x pc=0x%x reason=%s %s\n",
			     static_cast<uint32_t>(stage), wave.guest_wave_size, static_cast<uint32_t>(wave.proof), requested,
			     gctx->subgroup_size, gctx->compute_wave_vulkan_state.min_subgroup_size,
			     gctx->compute_wave_vulkan_state.max_subgroup_size, spirv_version, wave.refusal_pc, reason, detail.c_str());
		}
		return selection;
	};
	const auto vertex_mapping = select_native(vs_input_info->native_wave, vs_input_info->required_subgroup_size,
	                                          VK_SHADER_STAGE_VERTEX_BIT, vertex_ops, vs_shader.At(1),
	                                          ShaderVertexNggFrontRefusal(*vs_input_info));
	ShaderNativeSubgroupSelection fragment_mapping;
	if (has_fragment_stage)
	{
		fragment_mapping = select_native(ps_input_info->native_wave, ps_input_info->required_subgroup_size,
		                                 VK_SHADER_STAGE_FRAGMENT_BIT, fragment_ops, ps_shader.At(1), String8());
	}
	create_info.codeSize = static_cast<size_t>(vs_shader.Size()) * 4;
	create_info.pCode    = vs_shader.GetDataConst();
	vkCreateShaderModule(gctx->device, &create_info, nullptr, &vert_shader_module);
	if (has_fragment_stage)
	{
		create_info.codeSize = static_cast<size_t>(ps_shader.Size()) * 4;
		create_info.pCode    = ps_shader.GetDataConst();
		vkCreateShaderModule(gctx->device, &create_info, nullptr, &frag_shader_module);
	}

	if (vert_shader_module == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: vert_shader_module == nullptr condition ignored (continuing)\n"); }
	if (has_fragment_stage && frag_shader_module == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: has_fragment_stage && frag_shader_module == nullptr condition ignored (continuing)\n"); }

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
	frag_shader_stage_info.pNext               = nullptr;
	frag_shader_stage_info.flags               = 0;
	frag_shader_stage_info.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
	frag_shader_stage_info.module              = frag_shader_module;
	frag_shader_stage_info.pName               = "main";
	frag_shader_stage_info.pSpecializationInfo = nullptr;

	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT vert_required_subgroup_size {};
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT frag_required_subgroup_size {};
	if (vertex_ops != 0u)
	{
		EXIT_IF(!ShaderAttachNativeSubgroup(gctx->compute_wave_vulkan_state, vertex_mapping, &vert_shader_stage_info,
		                                    &vert_required_subgroup_size));
	}
	if (fragment_ops != 0u)
	{
		EXIT_IF(!ShaderAttachNativeSubgroup(gctx->compute_wave_vulkan_state, fragment_mapping, &frag_shader_stage_info,
		                                    &frag_required_subgroup_size));
	}

	VkPipelineShaderStageCreateInfo shader_stages[3] = {vert_shader_stage_info, frag_shader_stage_info};
	uint32_t shader_stage_count = has_fragment_stage ? 2u : 1u;
	if (!geometry_shader.IsEmpty())
	{
		create_info.codeSize = static_cast<size_t>(geometry_shader.Size()) * sizeof(uint32_t);
		create_info.pCode = geometry_shader.GetDataConst();
		EXIT_IF(vkCreateShaderModule(gctx->device, &create_info, nullptr, &geometry_shader_module) != VK_SUCCESS);
		auto& stage = shader_stages[shader_stage_count++];
		stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
		stage.module = geometry_shader_module;
		stage.pName = "main";
	}


	VulkanVertexInputLayout input_layout {};
	if (!VulkanBuildVertexInputLayout(*vs_input_info, &input_layout))
	{
		const bool log_vil_failure = VertexInputLayoutLogEnabled() &&
		                             g_vertex_input_layout_log_count.fetch_add(1, std::memory_order_relaxed) < 32u;
		if (log_vil_failure)
		{
			if (FILE* f = fopen("/tmp/kyty_vil.log", "a"))
			{
				// Decoded counts are reported as-is; the fixed arrays bound the traversal.
				const int buffer_count   = std::min(vs_input_info->buffers_num, ShaderVertexInputInfo::RES_MAX);
				const int resource_count = std::min(vs_input_info->resources_num, ShaderVertexInputInfo::RES_MAX);
				fprintf(f, "VIL FAIL: resources=%d buffers=%d\n", vs_input_info->resources_num, vs_input_info->buffers_num);
				for (int bi = 0; bi < buffer_count; bi++)
				{
					const auto& b = vs_input_info->buffers[bi];
					fprintf(f, "  buf[%d] addr=0x%llx stride=%u records=%u attr_num=%d\n", bi, (unsigned long long)b.addr, b.stride,
					        b.num_records, b.attr_num);
					for (int ai = 0; ai < b.attr_num && ai < ShaderVertexInputBuffer::ATTR_MAX; ai++)
					{
						fprintf(f, "    attr[%d] resource=%d offset=%u\n", ai, b.attr_indices[ai], b.attr_offsets[ai]);
					}
				}
				for (int ri = 0; ri < resource_count; ri++)
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
		EXIT("unsupported vertex input layout: resources=%d buffers=%d\n", vs_input_info->resources_num,
		     vs_input_info->buffers_num);
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
	if (!static_params->dx_clip_space && !depth_clip_control_supported) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !static_params->dx_clip_space && !depth_clip_control_supported condition ignored (continuing)\n"); }

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

	// Diagnostic A/B only, never acceptance: KYTY_AB_NO_CULL=1 removes face culling for every pipeline.
	static const bool ab_no_cull = [] { const char* v = std::getenv("KYTY_AB_NO_CULL"); return v != nullptr && v[0] == '1'; }();
	if (ab_no_cull) { cull_mode = VK_CULL_MODE_NONE; }

	VkFrontFace front_face = (static_params->face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);

	const bool depth_clip_supported = g_render_ctx->GetGraphicCtx()->depth_clip_enable_supported;

	VkPipelineRasterizationDepthClipStateCreateInfoEXT clip_ext {};
	clip_ext.sType           = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
	clip_ext.pNext           = nullptr;
	clip_ext.flags           = 0;
	clip_ext.depthClipEnable = (static_params->depth_clip_enable ? VK_TRUE : VK_FALSE);
	// Diagnostic A/B only, never acceptance: KYTY_AB_NO_DEPTH_CLIP=1 clamps instead of clipping depth for every pipeline.
	static const bool ab_no_depth_clip = [] { const char* v = std::getenv("KYTY_AB_NO_DEPTH_CLIP"); return v != nullptr && v[0] == '1'; }();
	if (ab_no_depth_clip) { clip_ext.depthClipEnable = VK_FALSE; }

	VkPipelineRasterizationStateCreateInfo rasterizer {};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.pNext                   = (depth_clip_supported ? &clip_ext : nullptr);
	rasterizer.flags                   = 0;
	rasterizer.depthClampEnable        = (static_params->depth_clamp_enable || ab_no_depth_clip ? VK_TRUE : VK_FALSE);
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
		if (!VulkanSampleLocationsPopulateInfo(static_params->sample_locations, sample_location_values,
		                                                       &sample_location_info)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !VulkanSampleLocationsPopulateInfo(static_params->sample_locations, sample_locat condition ignored (continuing)\n"); }
		sample_location_pipeline_state.sType                 = VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT;
		sample_location_pipeline_state.sampleLocationsEnable = VK_TRUE;
		sample_location_pipeline_state.sampleLocationsInfo   = sample_location_info;
		multisampling.pNext                                  = &sample_location_pipeline_state;
	}

	// CB_TARGET_MASK: 4 bits per MRT (RGBA). One blend attachment per active target.
	EXIT_IF(static_params->color_targets_num > 8u);
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
		att.blendEnable         = VulkanResolveColorAttachmentBlendEnable(static_params->blend_enable[rt], static_params->blend_bypass[rt]);
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

	// A dual-source variant writes MRT1 as Location 0 Index 1, so only Location 0 is a fragment output location.
	const uint32_t secondary_output_mask = ps_input_info->dual_source_blend ? 1u : 0u;
	uint32_t       fragment_output_mask  = has_fragment_stage ? 1u : 0u;
	for (uint32_t rt = 1u; rt < 8u; ++rt)
	{
		if (ps_input_info->target_output_mode[rt] != 0u && !(secondary_output_mask != 0u && rt == 1u)) { fragment_output_mask |= 1u << rt; }
	}
	const auto blend_admission = VulkanValidateBlendAttachments(gctx->blend_capabilities, color_blend_attachments,
	                                                            static_params->color_targets_num, secondary_output_mask, fragment_output_mask);
	if (blend_admission != VulkanBlendAdmission::Supported)
	{
		EXIT("unsupported graphics blend state: reason=%u targets=%u independent_enabled=%u dual_source_enabled=%u "
		     "max_targets=%u max_dual_source_targets=%u secondary_outputs=%u\n",
		     static_cast<uint32_t>(blend_admission), static_params->color_targets_num,
		     gctx->blend_capabilities.enabled.independent_blend, gctx->blend_capabilities.enabled.dual_source_blend,
		     gctx->blend_capabilities.max_color_attachments, gctx->blend_capabilities.max_dual_source_attachments,
		     secondary_output_mask);
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

	VkDescriptorSetLayout set_layouts[3]  = {};
	uint32_t              set_layouts_num = 0;

	VkPushConstantRange push_constant_info[2];
	uint32_t            push_constant_info_num = 0;

	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num, vs_input_info->bind,
	             /*additional_params->vs_bind,*/ VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);
	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num, ps_input_info->bind,
	             /*additional_params->ps_bind,*/ VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);
	if (vs_input_info->clip_probe.enabled || ps_input_info->input0_probe.enabled)
	{
		const uint32_t descriptor_set = vs_input_info->clip_probe.enabled ? vs_input_info->clip_probe_descriptor_set
		                                                               : ps_input_info->input0_probe_descriptor_set;
		EXIT_IF((vs_input_info->clip_probe.enabled &&
		         (!vs_input_info->clip_probe.draw_scoped || vs_input_info->clip_probe_descriptor_set != descriptor_set)) ||
		        (ps_input_info->input0_probe.enabled &&
		         (!ps_input_info->input0_probe.draw_scoped || ps_input_info->input0_probe_descriptor_set != descriptor_set)) ||
		        descriptor_set == kVertexClipProbeInvalidDescriptorSet || descriptor_set != set_layouts_num || set_layouts_num >= 3u);
		auto* vertex_clip_probe = g_render_ctx->GetVertexClipProbeRenderer();
		EXIT_IF(vertex_clip_probe == nullptr || vertex_clip_probe->GetDescriptorSetLayout() == VK_NULL_HANDLE);
		set_layouts[set_layouts_num++] = vertex_clip_probe->GetDescriptorSetLayout();
	}

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = 0;
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges    = push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	const ShaderBindResources* descriptor_stages[] = {&vs_input_info->bind, &ps_input_info->bind};
	// Declared color Output variables, including a dual-source Index 1 secondary.
	// maxFragmentCombinedOutputResources is not a Location count.
	uint32_t fragment_outputs = 1;
	for (uint32_t rt = 1; rt < 8; ++rt)
	{
		fragment_outputs += ps_input_info->target_output_mode[rt] != 0 ? 1u : 0u;
	}
	ValidatePipelineDescriptorLimits(gctx, descriptor_stages, 2,
	                                 vs_input_info->clip_probe.enabled || ps_input_info->input0_probe.enabled,
	                                 static_params->color_targets_num, fragment_outputs);
	const auto layout_result = vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr, &pipeline->pipeline_layout);
	if (layout_result != VK_SUCCESS || pipeline->pipeline_layout == VK_NULL_HANDLE)
	{
		EXIT("graphics pipeline layout creation failed: VkResult=%d\n", static_cast<int>(layout_result));
	}

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
	pipeline_info.stageCount          = shader_stage_count;
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

	const auto     vk_create_start = DebugStatsClock::now();
	const VkResult create_result =
	    vkCreateGraphicsPipelines(gctx->device, gctx->pipeline_cache, 1, &pipeline_info, nullptr, &pipeline->pipeline);
	const auto vk_create_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - vk_create_start).count();
	DebugStatsRecordVkPipelineCreate(DebugStatsPipelineKind::Graphics, static_cast<uint64_t>(vk_create_ns));

	if (pipeline->pipeline == nullptr || create_result != VK_SUCCESS)
	{
		static const char* cached_dump_dir = nullptr;
		static bool        cached_dump_checked = false;
		if (!cached_dump_checked)
		{
			cached_dump_dir = std::getenv("KYTY_PIPELINE_FAIL_DUMP");
			cached_dump_checked = true;
		}
		if (const char* dump_dir = cached_dump_dir)
		{
			if (dump_dir[0] != '\0')
			{
				const auto write_spv = [&](const char* name, const Vector<uint32_t>& words) {
					const auto status = DiagnosticDumpProcessWriter().Write(dump_dir, name, words.IsEmpty() ? nullptr : words.GetDataConst(),
					                                                        static_cast<size_t>(words.Size()) * sizeof(uint32_t));
					KYTY_LOG_DEBUG("KYTY_PIPELINE_FAIL_DUMP %s words=%u status=%s\n", name, static_cast<unsigned>(words.Size()),
					               DiagnosticDumpStatusName(status));
				};
				write_spv("fail_vs.spv", vs_shader);
				write_spv("fail_ps.spv", ps_shader);
				KYTY_LOG_DEBUG(
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
	if (geometry_shader_module != VK_NULL_HANDLE) { vkDestroyShaderModule(gctx->device, geometry_shader_module, nullptr); }

	return pipeline;
}

// The Vulkan inputs of one compute pipeline, decided on the dispatching thread.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static ComputePipelineRecipe BuildComputePipelineRecipe(const ShaderComputeInputInfo* input_info, const Vector<uint32_t>& cs_shader)
{
	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	const auto subgroup_check = ShaderCheckSubgroupModule(cs_shader.GetDataConst(), cs_shader.Size(),
	                                                       VK_SHADER_STAGE_COMPUTE_BIT, gctx->subgroup_stages,
	                                                       gctx->subgroup_operations, gctx->compute_wave_vulkan_state,
	                                                       gctx->shader_maximal_reconvergence_enabled);
	if (!subgroup_check.supported)
	{
		EXIT("subgroup-module admission stage=compute word=%zu required_ops=0x%x reason=%s\n",
		     subgroup_check.word, subgroup_check.required_operations, subgroup_check.reason);
	}

	VkPipelineShaderStageCreateInfo comp_shader_stage_info {};
	comp_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	comp_shader_stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required_subgroup_size {};
	if (input_info->native_wave_sensitive)
	{
		if (input_info->wave_layout.strategy != ShaderComputeWaveStrategy::Native ||
		    input_info->wave_layout.guest_wave_size != 32u || input_info->required_subgroup_size != 32u)
		{
			EXIT("native-wave compute sensitivity has inconsistent guest-width metadata\n");
		}
		const auto selection = ShaderSelectNativeSubgroup(gctx->compute_wave_vulkan_state, VK_SHADER_STAGE_COMPUTE_BIT,
		                                                   gctx->subgroup_size, input_info->required_subgroup_size,
		                                                   false, false, cs_shader.At(1) >= 0x00010600u);
		if (!selection.supported)
		{
			EXIT("native-wave compute shader requires exact subgroup width 32, unavailable from the default width or enabled compute subgroup-size control\n");
		}
		auto native_layout = input_info->wave_layout;
		native_layout.native_subgroup_size = input_info->required_subgroup_size;
		const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(gctx->compute_wave_vulkan_state);
		if (!ShaderComputeWaveVulkanAttachRequiredSubgroupSize(native_layout, capabilities, &comp_shader_stage_info,
		                                                     &required_subgroup_size, selection.require_size, cs_shader.At(1)))
		{
			EXIT("native-wave compute layout lacks full subgroups or exceeds exact-width workgroup limits\n");
		}
	} else if (input_info->required_subgroup_size != 0u)
	{
		EXIT("compute subgroup-size metadata requires a native-wave sensitivity proof\n");
	} else if (input_info->wave_layout.strategy == ShaderComputeWaveStrategy::Paired64On32)
	{
		const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(gctx->compute_wave_vulkan_state);
		const uint64_t shared_dwords = static_cast<uint64_t>(input_info->lds_dwords) + input_info->barrier_workspace_dwords;
		if (shared_dwords * sizeof(uint32_t) > capabilities.max_shared_bytes)
		{
			EXIT("paired-wave compute barrier workspace exceeds the Vulkan shared-memory limit\n");
		}
		if (!ShaderComputeWaveVulkanAttachRequiredSubgroupSize(input_info->wave_layout, capabilities,
		                                                     &comp_shader_stage_info, &required_subgroup_size))
		{
			EXIT("paired-wave compute layout is unsupported by the enabled Vulkan subgroup features or limits\n");
		}
	}

	const auto&                bind                 = input_info->bind;
	const ShaderBindResources* descriptor_stages[] = {&bind};
	ValidatePipelineDescriptorLimits(gctx, descriptor_stages, 1, false);

	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(gctx->physical_device, &properties);

	ComputePipelineRecipe recipe;
	recipe.vendor_id              = properties.vendorID;
	recipe.device_id              = properties.deviceID;
	recipe.stage_flags            = comp_shader_stage_info.flags;
	recipe.required_subgroup_size = comp_shader_stage_info.pNext == &required_subgroup_size ? required_subgroup_size.requiredSubgroupSize : 0u;
	if (bind.push_constant_size != 0 && !bind.vsharp_uniform_buffer)
	{
		recipe.push_constant_offset = bind.push_constant_offset;
		recipe.push_constant_size   = bind.push_constant_size;
	}
	recipe.has_set_layout = ShaderBindRequiresDescriptorSet(bind);
	EXIT_IF(recipe.has_set_layout && bind.push_constant_size == 0);
	EXIT_IF(recipe.has_set_layout && bind.descriptor_set_slot != 0);
	EXIT_IF(recipe.has_set_layout && !ShaderBuildDescriptorLayoutPlan(bind, &recipe.plan));
	recipe.spirv.assign(cs_shader.GetDataConst(), cs_shader.GetDataConst() + cs_shader.Size());
	return recipe;
}

// Creates the pipeline a recipe describes. Any thread may call it: it uses only
// the device, the shared pipeline cache and the locked descriptor-layout cache.
static VkResult CreateComputePipelineFromRecipe(GraphicContext* gctx, const ComputePipelineRecipe& recipe, VkPipelineLayout* layout,
                                                VkPipeline* pipeline)
{
	VkShaderModuleCreateInfo create_info {};
	create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.codeSize = recipe.spirv.size() * sizeof(uint32_t);
	create_info.pCode    = recipe.spirv.data();

	VkShaderModule comp_shader_module = VK_NULL_HANDLE;
	VkResult       result             = vkCreateShaderModule(gctx->device, &create_info, nullptr, &comp_shader_module);
	if (result != VK_SUCCESS)
	{
		return result;
	}

	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required_subgroup_size {};
	VkPipelineShaderStageCreateInfo                        comp_shader_stage_info {};
	comp_shader_stage_info.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	comp_shader_stage_info.flags  = recipe.stage_flags;
	comp_shader_stage_info.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	comp_shader_stage_info.module = comp_shader_module;
	comp_shader_stage_info.pName  = "main";
	if (recipe.required_subgroup_size != 0u)
	{
		required_subgroup_size.sType                = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
		required_subgroup_size.requiredSubgroupSize = recipe.required_subgroup_size;
		comp_shader_stage_info.pNext                = &required_subgroup_size;
	}

	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	if (recipe.has_set_layout)
	{
		set_layout = g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(DescriptorCache::Stage::Compute, recipe.plan);
	}
	const VkPushConstantRange push_constant_info {VK_SHADER_STAGE_COMPUTE_BIT, recipe.push_constant_offset, recipe.push_constant_size};

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.setLayoutCount         = recipe.has_set_layout ? 1u : 0u;
	pipeline_layout_info.pSetLayouts            = recipe.has_set_layout ? &set_layout : nullptr;
	pipeline_layout_info.pushConstantRangeCount = recipe.push_constant_size != 0u ? 1u : 0u;
	pipeline_layout_info.pPushConstantRanges    = recipe.push_constant_size != 0u ? &push_constant_info : nullptr;

	result = recipe.has_set_layout && set_layout == VK_NULL_HANDLE ? VK_ERROR_INITIALIZATION_FAILED
	                                                               : vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr, layout);
	if (result == VK_SUCCESS)
	{
		VkComputePipelineCreateInfo info {};
		info.sType             = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		info.stage             = comp_shader_stage_info;
		info.layout            = *layout;
		info.basePipelineIndex = -1;
		result                 = vkCreateComputePipelines(gctx->device, gctx->pipeline_cache, 1, &info, nullptr, pipeline);
	}
	vkDestroyShaderModule(gctx->device, comp_shader_module, nullptr);
	return result;
}

// Background compile of a recipe recorded in an earlier session; the driver
// caches keep the result for the dispatch that needs it.
static bool PrecompileComputePipeline(const ComputePipelineRecipe& recipe)
{
	auto*            gctx     = g_render_ctx->GetGraphicCtx();
	VkPipelineLayout layout   = VK_NULL_HANDLE;
	VkPipeline       pipeline = VK_NULL_HANDLE;
	const auto       result   = CreateComputePipelineFromRecipe(gctx, recipe, &layout, &pipeline);
	if (pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(gctx->device, pipeline, nullptr);
	}
	if (layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(gctx->device, layout, nullptr);
	}
	return result == VK_SUCCESS;
}

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

	const auto recipe = BuildComputePipelineRecipe(input_info, cs_shader);
	ComputePipelinePrecompileStart(ComputePipelinePrecompileDirectory(), recipe.vendor_id, recipe.device_id, PrecompileComputePipeline);
	ComputePipelinePrecompileClaim(recipe);

	EXIT_IF(pipeline->pipeline_layout != nullptr);
	EXIT_IF(pipeline->pipeline != nullptr);

	const auto     vk_create_start = DebugStatsClock::now();
	const VkResult create_result   = CreateComputePipelineFromRecipe(gctx, recipe, &pipeline->pipeline_layout, &pipeline->pipeline);
	const auto     vk_create_ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - vk_create_start).count();
	DebugStatsRecordVkPipelineCreate(DebugStatsPipelineKind::Compute, static_cast<uint64_t>(vk_create_ns));

	if (pipeline->pipeline_layout == VK_NULL_HANDLE)
	{
		EXIT("compute pipeline layout creation failed: VkResult=%d\n", static_cast<int>(create_result));
	}
	if (pipeline->pipeline == nullptr || create_result != VK_SUCCESS)
	{
		if (const char* dump_dir = std::getenv("KYTY_PIPELINE_FAIL_DUMP"))
		{
			if (dump_dir[0] != '\0')
			{
				const auto status = DiagnosticDumpProcessWriter().Write(dump_dir, "fail_cs.spv", cs_shader.IsEmpty() ? nullptr : cs_shader.GetDataConst(),
				                                                        static_cast<size_t>(cs_shader.Size()) * sizeof(uint32_t));
				KYTY_LOG_DEBUG("KYTY_PIPELINE_FAIL_DUMP fail_cs.spv words=%u result=%d status=%s\n", static_cast<unsigned>(cs_shader.Size()),
				               static_cast<int>(create_result), DiagnosticDumpStatusName(status));
			}
		}
		EXIT("vkCreateComputePipelines failed: result=%d pipeline=%p\n", static_cast<int>(create_result),
		     static_cast<void*>(pipeline->pipeline));
	}

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
	if (!m_pipelines.IndexValid(id)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !m_pipelines.IndexValid(id) condition ignored (continuing)\n"); }

	Pipeline& p = m_pipelines[id];
	if (p.pipeline == nullptr)
	{
		return;
	}

	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(p.pipeline->pipeline == nullptr);
	EXIT_IF(p.pipeline->pipeline_layout == nullptr);
	EXIT_IF(p.static_params == nullptr);
	EXIT_IF(p.dynamic_params == nullptr);

	EXIT_IF(p.pipeline->static_params != p.static_params);
	EXIT_IF(p.pipeline->dynamic_params != p.dynamic_params);
	if (!p.lifetime.RequestRetirement())
	{
		return;
	}

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
	p = {};
}

PipelineCache::Pipeline* PipelineCache::Find(const Pipeline& p)
{
	for (auto& pn: m_pipelines)
	{
		if (pn.pipeline != nullptr && !pn.lifetime.RetirementRequested() && p.render_pass_id == pn.render_pass_id &&
		    p.vs_shader_id == pn.vs_shader_id &&
		    p.ps_shader_id == pn.ps_shader_id && p.cs_shader_id == pn.cs_shader_id && *p.static_params == *pn.static_params &&
		    *p.dynamic_params == *pn.dynamic_params)
		{
			return &pn;
		}
	}
	return nullptr;
}

uint32_t PipelineCache::FindAvailableSlot()
{
	for (uint32_t index = 0; index < m_pipelines.Size(); index++)
	{
		if (m_pipelines[index].pipeline == nullptr)
		{
			return index;
		}
	}

	if (m_pipelines.Size() < PipelineCache::MAX_PIPELINES)
	{
		return m_pipelines.Size();
	}

	const auto evict =
	    PipelineCacheNextEvictableIndex(m_pipelines.Size(), &m_evict_cursor, [this](uint32_t index)
	                                    { return m_pipelines[index].pipeline != nullptr && m_pipelines[index].lifetime.CanEvict(); });
	if (evict == UINT32_MAX)
	{
		EXIT("pipeline cache limit reached: all %u pipeline entries are leased by recorded command buffers\n",
		     PipelineCache::MAX_PIPELINES);
	}
	return evict;
}

void PipelineCache::AttachPipelineLease(CommandBuffer* recording_buffer, Pipeline& pipeline)
{
	EXIT_IF(recording_buffer == nullptr);
	EXIT_IF(pipeline.pipeline == nullptr);
	if (!pipeline.lifetime.Acquire())
	{
		EXIT("attempted to lease a retired or saturated Vulkan pipeline\n");
	}

	bool added = false;
	if (!recording_buffer->AttachPipelineLease(pipeline.pipeline, &added))
	{
		(void)pipeline.lifetime.Release();
		EXIT("failed to attach Vulkan pipeline to its recording command buffer\n");
	}
	if (!added)
	{
		EXIT_IF(pipeline.lifetime.Release());
	}
}

void PipelineCache::SaveDriverCacheIfDue()
{
	auto* ctx = g_render_ctx->GetGraphicCtx();
	if (ctx == nullptr || ctx->physical_device == nullptr || ctx->device == nullptr || ctx->pipeline_cache == nullptr)
	{
		return;
	}
	// A clean cache is never due, so its per-lookup call skips the clock read.
	if (m_driver_cache_write_budget_exhausted || !m_driver_cache_dirty)
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
	const auto   save_start       = DebugStatsClock::now();
	const auto   save_result      = PipelineCacheStoreSave(ctx->device, ctx->pipeline_cache, properties, remaining_budget, &attempted_size);
	if (save_result == PipelineCacheStoreSaveResult::Unchanged)
	{
		m_driver_cache_attempted_once = true;
		m_last_driver_cache_attempt   = now;
		m_driver_cache_dirty          = false;
		return;
	}
	const auto save_elapsed_ns =
	    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - save_start).count());
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
			KYTY_LOG_DEBUG("Saved Vulkan pipeline cache: %zu bytes\n", attempted_size);
		}
	} else if (save_result == PipelineCacheStoreSaveResult::BudgetExceeded)
	{
		m_driver_cache_write_budget_exhausted = true;
	}
}

// A resolution cohort shares one guest and one host extent, so its first
// attachment describes the framebuffer. Depth-only passes use the depth image.
static const VulkanImage* ResolutionScaledFramebufferImage(const RenderColorInfo& color, const RenderDepthInfo& depth)
{
	const auto*        attachment = RenderColorFirstConfiguredAttachment(color);
	const VulkanImage* image      = attachment != nullptr ? attachment->vulkan_buffer : depth.vulkan_buffer;
	return image != nullptr && image->IsResolutionScaled() ? image : nullptr;
}

VulkanPipeline* PipelineCache::CreatePipeline(CommandBuffer* recording_buffer, VulkanFramebuffer* framebuffer, RenderColorInfo* color,
                                              RenderDepthInfo* depth,
                                              const ShaderVertexInputInfo* vs_input_info, HW::Context* ctx, HW::Shader* sh_ctx,
                                              const ShaderPixelInputInfo* ps_input_info, VkPrimitiveTopology topology,
                                              const VulkanSampleLocationState& sample_locations)
{
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(recording_buffer == nullptr);
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

	if (ps_regs.ps_embedded && ps_input_info->stage_enabled && RenderColorSlotActive(*color, 0u))
	{
		const auto format = color->attachment[0].attachment_format;
		if (format == VK_FORMAT_R16_UINT || format == VK_FORMAT_R16G16_UINT || format == VK_FORMAT_R16G16B16A16_UINT)
		{
			// The embedded pixel template writes a floating-point output at location zero.
			EXIT("embedded pixel shader has no unsigned color export interface: format=%u\n", static_cast<unsigned>(format));
		}
	}

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		ShaderDbgDumpInputInfo(vs_input_info);
		ShaderDbgDumpInputInfo(ps_input_info);
	}

	ShaderRequireVertexProgram(&vs_regs, vs_input_info);
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

	if (depth->depth_test_enable && ps_input_info->ps_execute_on_noop) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: depth->depth_test_enable && ps_input_info->ps_execute_on_noop condition ignored (continuing)\n"); }

	const auto scissor = State::ResolveScissor(vp, smc, 0);

	p.dynamic_params->viewport_scale[0]  = vp.viewports[0].xscale;
	p.dynamic_params->viewport_scale[1]  = vp.viewports[0].yscale;
	p.dynamic_params->viewport_scale[2]  = vp.viewports[0].zscale;
	p.dynamic_params->viewport_offset[0] = vp.viewports[0].xoffset;
	p.dynamic_params->viewport_offset[1] = vp.viewports[0].yoffset;
	p.dynamic_params->viewport_offset[2] = vp.viewports[0].zoffset;
	p.dynamic_params->viewport_depth_clamp[0] = vp.viewports[0].zmin;
	p.dynamic_params->viewport_depth_clamp[1] = vp.viewports[0].zmax;
	const auto& clip_control                  = ctx->GetClipControl();
	p.static_params->dx_clip_space            = clip_control.dx_clip_space;
	const auto depth_clip_state =
	    State::ResolveDepthClipState(clip_control, gctx->depth_clip_enable_supported, gctx->depth_clamp_supported);
	p.static_params->depth_clip_enable  = depth_clip_state.depth_clip_enable;
	p.static_params->depth_clamp_enable = depth_clip_state.depth_clamp_enable;
	p.dynamic_params->scissor_ltrb[0]    = scissor.left;
	p.dynamic_params->scissor_ltrb[1]    = scissor.top;
	p.dynamic_params->scissor_ltrb[2]    = scissor.right;
	p.dynamic_params->scissor_ltrb[3]    = scissor.bottom;
	if (const auto* scaled_image = ResolutionScaledFramebufferImage(*color, *depth); scaled_image != nullptr)
	{
		RenderResolutionTransform transform;
		const ResolutionExtent        guest {scaled_image->guest_extent.width, scaled_image->guest_extent.height};
		const ResolutionExtent        host {scaled_image->extent.width, scaled_image->extent.height};
		if (CreateRenderResolutionTransform(guest, host, &transform) != RenderResolutionTransformStatus::Success) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: CreateRenderResolutionTransform(guest, host, &transform) != RenderResolutionTransformStatus::Success condition ignored (continuing)\n"); }

		const auto         xy = State::ResolveViewportXy(p.dynamic_params->viewport_scale[0], p.dynamic_params->viewport_offset[0],
		                                                 p.dynamic_params->viewport_scale[1], p.dynamic_params->viewport_offset[1]);
		ResolutionViewport guest_viewport {xy.x, xy.y, xy.width, xy.height, 0.0, 1.0};
		ResolutionViewport host_viewport;
		if (MapRenderResolutionViewport(transform, guest_viewport, &host_viewport) != RenderResolutionTransformStatus::Success) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: MapRenderResolutionViewport(transform, guest_viewport, &host_viewport) != RenderResolutionTransformStatus::Success condition ignored (continuing)\n"); }
		p.dynamic_params->viewport_scale[0]  = static_cast<float>(host_viewport.width * 0.5);
		p.dynamic_params->viewport_scale[1]  = static_cast<float>(host_viewport.height * 0.5);
		p.dynamic_params->viewport_offset[0] = static_cast<float>(host_viewport.x + host_viewport.width * 0.5);
		p.dynamic_params->viewport_offset[1] = static_cast<float>(host_viewport.y + host_viewport.height * 0.5);

		ResolutionScissorRect       host_scissor;
		const ResolutionScissorRect guest_scissor {scissor.left, scissor.top, scissor.right, scissor.bottom};
		if (MapRenderResolutionScissor(transform, guest_scissor, &host_scissor) != RenderResolutionTransformStatus::Success) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: MapRenderResolutionScissor(transform, guest_scissor, &host_scissor) != RenderResolutionTransformStatus::Success condition ignored (continuing)\n"); }
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
	if (p.static_params->sample_locations.sample_count != p.static_params->rasterization_samples) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: p.static_params->sample_locations.sample_count != p.static_params->rasterization_samples condition ignored (continuing)\n"); }
	p.static_params->sample_shading_enable    = p.static_params->rasterization_samples != VK_SAMPLE_COUNT_1_BIT &&
	                                        ctx->GetEqaaControl().ps_iter_samples != 0;
	if (p.static_params->sample_shading_enable && !gctx->sample_rate_shading_supported) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: p.static_params->sample_shading_enable && !gctx->sample_rate_shading_supported condition ignored (continuing)\n"); }
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
		if (!p.static_params->with_depth) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !p.static_params->with_depth condition ignored (continuing)\n"); }
		p.static_params->color_targets_num = 1;
		p.static_params->color_mask[0]     = 0;
		p.static_params->blend_enable[0]   = false;
	} else
	{
		if (p.static_params->color_targets_num > 8) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: p.static_params->color_targets_num > 8 condition ignored (continuing)\n"); }
		for (uint32_t rt = 0; rt < p.static_params->color_targets_num; rt++)
		{
			// A NULL export sends no color even when the guest leaves CB masks set.
			// Vulkan must not write an undefined fragment output to those attachments.
			if (!RenderColorSlotConfigured(*color, rt) || ps_input_info->has_only_null_exports)
			{
				p.static_params->color_mask[rt]   = 0;
				p.static_params->blend_enable[rt] = false;
				continue;
			}
			const uint8_t requested_mask = State::ResolveColorWriteAgainstDepth(
			    State::ResolveColorWriteMask(ctx->GetRenderTargetMask(), sh_regs.m_cbShaderMask, rt),
			    ctx->GetDepthControl().color_writes_on_depth_pass_disable, ctx->GetDepthControl().color_writes_on_depth_fail_enable);
			const uint8_t export_enable           = ps_input_info->color_exports.enable[rt];
			const bool    export_summary_available = !ps_regs.ps_embedded;
			const uint8_t export_admitted_mask =
			    State::ResolveColorWriteAgainstExport(requested_mask, export_enable, export_summary_available);
			if (export_summary_available && export_enable == 0u)
			{
				p.static_params->color_mask[rt]   = export_admitted_mask;
				p.static_params->blend_enable[rt] = false;
				continue;
			}
			const bool uint16_mrt_output = ps_input_info->target_output_mode[rt] == 7u &&
			                               ps_input_info->target_output_number[rt] == 4u &&
			                               ps_input_info->target_output_order[rt] == 0u;
			const auto& blend               = ctx->GetBlendControl(rt);
			p.static_params->color_mask[rt] = uint16_mrt_output
			                                      ? State::ResolveColorWriteAgainstUint16Export(export_admitted_mask, export_enable)
			                                      : export_admitted_mask;
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
	if (depth_bias.enabled && (!std::isfinite(depth_bias.constant_factor) || !std::isfinite(depth_bias.clamp) ||
	                                            !std::isfinite(depth_bias.slope_factor))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: depth_bias.enabled && (!std::isfinite(depth_bias.constant_factor) || !std::isfin condition ignored (continuing)\n"); }
	if (depth_bias.enabled && depth_bias.clamp != 0.0f && !g_render_ctx->GetGraphicCtx()->depth_bias_clamp_supported) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: depth_bias.enabled && depth_bias.clamp != 0.0f && !g_render_ctx->GetGraphicCtx()->depth_bias_clamp_supported condition ignored (continuing)\n"); }
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

	const auto lookup_start = DebugStatsClock::now();
	auto*      found        = Find(p);
	const auto lookup_ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - lookup_start).count();
	DebugStatsRecordPipelineLookup(DebugStatsPipelineKind::Graphics, found != nullptr, static_cast<uint64_t>(lookup_ns));

	if (found != nullptr)
	{
		*found->dynamic_params = *p.dynamic_params;
		delete p.static_params;
		delete p.dynamic_params;
		AttachPipelineLease(recording_buffer, *found);
		SaveDriverCacheIfDue();
		return found->pipeline;
	}

	const auto miss_start = DebugStatsClock::now();
	for (uint32_t rt = 0; rt < p.static_params->color_targets_num; rt++)
	{
		if (!RenderColorSlotActive(*color, rt))
		{
			continue;
		}

		const auto& attachment = color->attachment[rt];
		const auto  admission  = VulkanValidateColorAttachmentBlendRequest(gctx->physical_device, attachment.attachment_format,
		                                                                  p.static_params->blend_enable[rt],
		                                                                  p.static_params->blend_bypass[rt]);
		if (admission != VulkanColorAttachmentBlendAdmission::Supported)
		{
			EXIT("unsupported graphics color attachment blend: reason=%u target=%u format=%u blend_enable=%u blend_bypass=%u\n",
			     static_cast<uint32_t>(admission), rt, static_cast<uint32_t>(attachment.attachment_format),
			     p.static_params->blend_enable[rt] ? 1u : 0u, p.static_params->blend_bypass[rt] ? 1u : 0u);
		}
	}

	if (vs_input_info->position1_usage == ShaderVertexPosition1Usage::ClipDistance0)
	{
		VkPhysicalDeviceFeatures features {};
		vkGetPhysicalDeviceFeatures(gctx->physical_device, &features);
		EXIT_IF(features.shaderClipDistance != VK_TRUE);
	}
	if (ps_input_info->custom_interpolation.Enabled())
	{
		EXIT_IF(!gctx->geometry_shader_supported);
		EXIT_IF(p.static_params->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST &&
		        p.static_params->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP &&
		        p.static_params->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
		VkPhysicalDeviceProperties properties {};
		vkGetPhysicalDeviceProperties(gctx->physical_device, &properties);
		const auto& limits = properties.limits;
		const auto components = ps_input_info->custom_interpolation.location_count * 4u;
		EXIT_IF(components > limits.maxFragmentInputComponents || components + 5u > limits.maxGeometryOutputComponents ||
		        (components + 5u) * 3u > limits.maxGeometryTotalOutputComponents || limits.maxGeometryOutputVertices < 3u ||
		        static_cast<uint32_t>(vs_input_info->export_count) * 4u + 4u > limits.maxGeometryInputComponents);
	}

	if (ps_input_info->stage_enabled)
	{
		const auto ps_transport_code = ShaderParsePS(&ps_regs, &sh_regs);
		const auto requirement       = FragmentTransportRequire(ps_transport_code, *ps_input_info,
                                                          static_cast<uint32_t>(ps_regs.ps_regs.rsrc2.user_sgpr), *p.static_params,
                                                          framebuffer->extent);
		if (requirement.required)
		{
			char dump_note[160] {};
			if (const char* dump_dir = std::getenv("KYTY_TRANSPORT_DUMP"); dump_dir != nullptr && dump_dir[0] != '\0')
			{
				std::snprintf(dump_note, sizeof(dump_note), " (transport dump: %s)",
				              ShaderDumpGuestProgram(dump_dir, "ps", ps_regs.ps_regs.chksum, ps_regs.ps_regs.data_addr, ps_transport_code).c_str());
			}
			EXIT("fragment wave transport is required by the pixel stage but the renderer does not select it yet: %s%s\n",
			     requirement.facts.c_str(), dump_note);
		}
	}

	auto* translation_cache = g_render_ctx->GetShaderTranslationCache();
	EXIT_IF(translation_cache == nullptr);
	const auto optimization = Config::GetShaderOptimizationType();
	const bool next_gen     = Config::IsNextGen();
	const bool debug_printf = Config::SpirvDebugPrintfEnabled();
	const auto vs_translation =
	    translation_cache->GetOrCompile(ShaderModuleKey::Create(vs_id, ShaderModuleStage::Vertex, optimization, next_gen, debug_printf,
	                                                           vs_input_info->clip_probe.diagnostic_identity),
	                                    [&]
	                                    {
		                                    auto vs_code = ShaderParseVS(&vs_regs, &sh_regs, vs_input_info);
		                                    return ShaderRecompileVS(vs_code, vs_input_info);
	                                    });
	DebugStatsRecordShaderTranslationCache(vs_translation.hit, vs_translation.evicted);
	ShaderTranslationCacheResult ps_translation;
	if (ps_input_info->stage_enabled)
	{
		ps_translation =
		    translation_cache->GetOrCompile(ShaderModuleKey::Create(ps_id, ShaderModuleStage::Pixel, optimization, next_gen,
		                                                           debug_printf, ps_input_info->fragment_tap.diagnostic_identity ^
		                                                                             ps_input_info->input0_probe.diagnostic_identity),
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
		KYTY_LOG_DEBUG(
		             "Pixel shader translation returned no binary: addr=0x%016" PRIx64 " checksum=0x%016" PRIx64 " hash=0x%08" PRIx32
		             " crc=0x%08" PRIx32 " ids=%u cache_hit=%u\n",
		             ps_regs.ps_regs.data_addr, ps_regs.ps_regs.chksum, ps_id.hash0, ps_id.crc32, static_cast<uint32_t>(ps_id.ids.Size()),
		             ps_translation.hit ? 1u : 0u);
	}
	EXIT_IF(ps_input_info->stage_enabled && ps_translation.binary.IsEmpty());
	ShaderTranslationCacheResult geometry_translation;
	if (ps_input_info->custom_interpolation.Enabled())
	{
		geometry_translation = translation_cache->GetOrCompile(
		    ShaderModuleKey::Create(ps_id, ShaderModuleStage::Geometry, optimization, next_gen),
		    [&] { return ShaderCompileInterpolationGeometry(*ps_input_info); });
		DebugStatsRecordShaderTranslationCache(geometry_translation.hit, geometry_translation.evicted);
		EXIT_IF(geometry_translation.binary.IsEmpty());
	}

	const uint32_t slot = FindAvailableSlot();
	p.pipeline = CreatePipelineInternal(framebuffer->render_pass, vs_input_info, vs_translation.binary, ps_input_info,
	                                    ps_translation.binary, geometry_translation.binary, p.static_params, p.dynamic_params);

	if (p.pipeline == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: p.pipeline == nullptr condition ignored (continuing)\n"); }
	p.pipeline->framebuffer_extent = framebuffer->extent;

	if (slot < m_pipelines.Size())
	{
		if (m_pipelines[static_cast<int>(slot)].pipeline != nullptr)
		{
			DebugStatsRecordPipelineEviction();
			DeletePipelineInternal(slot);
		}
		m_pipelines[static_cast<int>(slot)] = p;
	} else
	{
		EXIT_IF(slot != m_pipelines.Size());
		m_pipelines.Add(p);
	}
	AttachPipelineLease(recording_buffer, m_pipelines[static_cast<int>(slot)]);
	DumpPipeline("create", static_cast<int>(slot));

	m_driver_cache_dirty = true;
	SaveDriverCacheIfDue();
	const auto miss_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - miss_start).count();
	DebugStatsRecordPipelineMiss(DebugStatsPipelineKind::Graphics, static_cast<uint64_t>(miss_ns));
	return m_pipelines[static_cast<int>(slot)].pipeline;
}

VulkanPipeline* PipelineCache::CreatePipeline(CommandBuffer* recording_buffer, const ShaderComputeInputInfo* input_info,
                                              const HW::ComputeShaderInfo* cs_regs, const HW::ShaderRegisters* sh_regs)
{
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(recording_buffer == nullptr);
	EXIT_IF(cs_regs == nullptr);
	EXIT_IF(sh_regs == nullptr);

	Core::LockGuard lock(m_mutex);

	ShaderDbgDumpInputInfo(input_info);

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

	const auto lookup_start = DebugStatsClock::now();
	auto*      found        = Find(p);
	const auto lookup_ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - lookup_start).count();
	DebugStatsRecordPipelineLookup(DebugStatsPipelineKind::Compute, found != nullptr, static_cast<uint64_t>(lookup_ns));

	if (found != nullptr)
	{
		*found->dynamic_params = *p.dynamic_params;
		delete p.static_params;
		delete p.dynamic_params;
		AttachPipelineLease(recording_buffer, *found);
		SaveDriverCacheIfDue();
		return found->pipeline;
	}

	const auto miss_start = DebugStatsClock::now();

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

	const uint32_t slot = FindAvailableSlot();
	p.pipeline = CreatePipelineInternal(input_info, cs_translation.binary, p.static_params, p.dynamic_params /*, params2*/);

	if (p.pipeline == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: p.pipeline == nullptr condition ignored (continuing)\n"); }

	if (slot < m_pipelines.Size())
	{
		if (m_pipelines[static_cast<int>(slot)].pipeline != nullptr)
		{
			DebugStatsRecordPipelineEviction();
			DeletePipelineInternal(slot);
		}
		m_pipelines[static_cast<int>(slot)] = p;
	} else
	{
		EXIT_IF(slot != m_pipelines.Size());
		m_pipelines.Add(p);
	}
	AttachPipelineLease(recording_buffer, m_pipelines[static_cast<int>(slot)]);
	DumpPipeline("create", static_cast<int>(slot));

	m_driver_cache_dirty = true;
	SaveDriverCacheIfDue();
	const auto miss_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(DebugStatsClock::now() - miss_start).count();
	DebugStatsRecordPipelineMiss(DebugStatsPipelineKind::Compute, static_cast<uint64_t>(miss_ns));
	return m_pipelines[static_cast<int>(slot)].pipeline;
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

void PipelineCache::ReleasePipelineLease(VulkanPipeline* pipeline)
{
	EXIT_IF(pipeline == nullptr);
	Core::LockGuard lock(m_mutex);
	const auto      index = m_pipelines.Find(pipeline, [](const auto& entry, const auto* value) { return value == entry.pipeline; });
	EXIT_IF(!m_pipelines.IndexValid(index));
	if (!m_pipelines.IndexValid(index))
	{
		return;
	}

	Pipeline& entry = m_pipelines[index];
	EXIT_IF(entry.lifetime.ReferenceCount() == 0);
	if (entry.lifetime.Release())
	{
		DeletePipelineInternal(static_cast<uint32_t>(index));
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
			KYTY_LOG_DEBUG(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, file_name.C_Str());
			return;
		}
		Pipeline& p = m_pipelines[id];
		DumpToFile(&f, p);
		f.Close();
	}
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
