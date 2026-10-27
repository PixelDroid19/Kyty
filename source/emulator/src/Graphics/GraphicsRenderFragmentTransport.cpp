#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String8.h"

#include "Emulator/Graphics/FragmentTransportAdmission.h"
#include "Emulator/Graphics/FragmentTransportCapture.h"
#include "Emulator/Graphics/FragmentTransportLayout.h"
#include "Emulator/Graphics/FragmentTransportResolve.h"
#include "Emulator/Graphics/FragmentTransportShade.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"
#include "Emulator/Graphics/ShaderSpirv.h"

#include "GraphicsRenderInternal.h"
#include "ShaderSpirvToolchain.h"

#include <algorithm>
#include <cstdio>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kTransportDescriptorSet = 2;
constexpr uint32_t kWaveInputBinding       = 3;
constexpr uint32_t kWaveOutputBinding      = 4;
constexpr uint32_t kSystemInputFields      = 16;

uint32_t InitialVgprCount(const ShaderPixelInputInfo& pixel)
{
	uint32_t count = 0;
	for (uint32_t field = 0; field < kSystemInputFields; ++field)
	{
		if ((pixel.system_input_enable & (1u << field)) != 0u)
		{
			const uint32_t width = field == 3u ? 3u : (field < 7u ? 2u : 1u);
			count                = std::max(count, ShaderPixelSystemInputRegister(pixel, field) + width);
		}
	}
	return count;
}

uint32_t ColorWriteMask(const PipelineStaticParameters& state)
{
	uint32_t mask = 0;
	for (uint32_t target = 0; target < std::min(state.color_targets_num, 8u); ++target)
	{
		mask |= (state.color_mask[target] & 15u) << (target * 4u);
	}
	return mask;
}

FragmentTransport::Limits HostLimits(const GraphicContext& context)
{
	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(context.physical_device, &properties);
	return {properties.limits.maxStorageBufferRange, FragmentTransport::MAX_TRANSIENT_BYTES, properties.limits.maxComputeWorkGroupCount[0]};
}

uint32_t MaxQuadCapacity(uint32_t primitives, uint32_t lane_words, uint32_t header_words, const FragmentTransport::Limits& limits)
{
	uint32_t low  = 0;
	uint32_t high = 0x100000u;
	while (low < high)
	{
		const uint32_t            middle = low + (high - low + 1u) / 2u;
		FragmentTransport::Layout layout;
		const auto                check = FragmentTransport::BuildLayout({middle, primitives, lane_words, header_words}, limits, &layout);
		if (check.failure == FragmentTransport::Failure::None)
		{
			low = middle;
		} else
		{
			high = middle - 1u;
		}
	}
	return low;
}

String8 Assembled(const char* name, const String8& source)
{
	if (source.StartsWith("OpKyty"))
	{
		return String8::FromPrintf("%s=rejected(%s) ", name, source.ReplaceStr("\n", "").c_str());
	}
	Vector<uint32_t> binary;
	String8          error;
	if (!ShaderToolchain::Run(source, &binary, &error))
	{
		return String8::FromPrintf("%s=assembly_failed ", name);
	}
	return String8::FromPrintf("%s=%u_words ", name, static_cast<unsigned>(binary.Size()));
}

String8 Capacities(uint32_t lane_words, uint32_t header_words, const FragmentTransport::Limits& limits)
{
	String8 text = "quad_capacity_by_primitives{";
	for (uint32_t primitives: {1000u, 10000u, 30000u})
	{
		text += String8::FromPrintf("%u:%u ", primitives, MaxQuadCapacity(primitives, lane_words, header_words, limits));
	}
	return text + "} ";
}

String8 DrawState(const PipelineStaticParameters& state, const VkExtent2D& extent, const ShaderPixelInputInfo& pixel)
{
	return String8::FromPrintf(
	    "draw{extent=%ux%u samples=%u color_targets=%u write_mask=0x%08x depth=%u depth_write=%u blend0=%u input_num=%u "
	    "system_inputs=0x%x subgroup=%u} ",
	    extent.width, extent.height, static_cast<unsigned>(state.rasterization_samples), state.color_targets_num, ColorWriteMask(state),
	    state.with_depth ? 1u : 0u, state.depth_write_enable ? 1u : 0u, state.blend_enable[0] ? 1u : 0u, pixel.input_num,
	    pixel.system_input_enable, pixel.required_subgroup_size);
}

} // namespace

uint32_t FragmentTransportInitialVgprs(const ShaderPixelInputInfo& pixel)
{
	return InitialVgprCount(pixel);
}

uint32_t FragmentTransportLaneWords(const ShaderCode& code, const ShaderPixelInputInfo& pixel)
{
	return 1u + InitialVgprCount(pixel) + FragmentTransport::BuildLanePacking(code, pixel).words;
}

bool FragmentTransportBuildShadeModule(const ShaderCode& code, const ShaderPixelInputInfo& pixel, uint32_t user_sgpr_count,
                                       uint32_t lds_dwords, Vector<uint32_t>* binary, String8* error)
{
	EXIT_IF(binary == nullptr || error == nullptr);
	binary->Clear();
	error->Clear();

	ShaderFragmentComputeInfo info;
	info.descriptor_set     = kTransportDescriptorSet;
	info.input_binding      = kWaveInputBinding;
	info.output_binding     = kWaveOutputBinding;
	info.control_binding    = 1u;
	info.initial_vgpr_count = InitialVgprCount(pixel);
	info.user_sgpr_count    = user_sgpr_count;
	info.parameter_state    = ShaderFragmentParameterState::Virtualized;

	ShaderComputeInputInfo compute;
	compute.wave_layout.strategy             = ShaderComputeWaveStrategy::Split64On32;
	compute.wave_layout.guest_local[0]       = 64u;
	compute.wave_layout.guest_local[1]       = 1u;
	compute.wave_layout.guest_local[2]       = 1u;
	compute.wave_layout.physical_local[0]    = 64u;
	compute.wave_layout.physical_local[1]    = 1u;
	compute.wave_layout.physical_local[2]    = 1u;
	compute.wave_layout.guest_wave_size      = 64u;
	compute.wave_layout.native_subgroup_size = 32u;
	compute.wave_layout.banks                = 1u;
	compute.wave_layout.waves                = 1u;
	compute.wave_layout.lds_dwords           = lds_dwords;
	compute.native_wave_sensitive            = true;
	compute.required_subgroup_size           = 32u;
	compute.threads_num[0]                   = 64u;
	compute.threads_num[1]                   = 1u;
	compute.threads_num[2]                   = 1u;
	compute.lds_dwords                       = lds_dwords;
	compute.float_mode                       = pixel.float_mode;
	compute.dx10_clamp                       = pixel.dx10_clamp;
	compute.ieee_mode                        = pixel.ieee_mode;
	compute.fp_mode_known                    = true;
	compute.fp16_overflow                    = pixel.fp16_overflow;
	compute.fp16_overflow_known              = pixel.fp16_overflow_known;

	const auto source = SpirvGenerateFragmentComputeSource(code, pixel, compute, info);
	if (source.StartsWith("OpKyty"))
	{
		*error = source;
		return false;
	}
	return ShaderToolchain::Run(source, binary, error);
}

namespace {

VkDescriptorSetLayout CreateEmptySetLayout(VkDevice device)
{
	VkDescriptorSetLayoutCreateInfo info {};
	info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = 0;
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	return vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) == VK_SUCCESS ? layout : VK_NULL_HANDLE;
}

} // namespace

bool FragmentTransportCreateShadePipeline(VkDevice device, VkPipelineCache pipeline_cache, const ShaderBindResources& ps_bind,
                                          const Vector<uint32_t>& binary, bool require_subgroup_size,
                                          VkDescriptorSetLayout transport_layout, VkPipelineLayout* layout, VkPipeline* pipeline)
{
	EXIT_IF(layout == nullptr || pipeline == nullptr || binary.IsEmpty());
	*layout   = nullptr;
	*pipeline = nullptr;
	if (g_render_ctx == nullptr || transport_layout == VK_NULL_HANDLE)
	{
		return false;
	}
	auto* gctx = g_render_ctx->GetGraphicCtx();
	if (gctx == nullptr || gctx->device != device)
	{
		return false;
	}

	const VkDescriptorSetLayout empty_layout = CreateEmptySetLayout(device);
	if (empty_layout == VK_NULL_HANDLE)
	{
		return false;
	}

	ShaderDescriptorLayoutPlan plan;
	const bool                 bind_requires_set = ShaderBindRequiresDescriptorSet(ps_bind);
	const bool                 plan_ok           = ShaderBuildDescriptorLayoutPlan(ps_bind, &plan);
	const bool                 has_ps_set        = bind_requires_set && plan_ok;
	VkDescriptorSetLayout      ps_layout         = empty_layout;
	if (has_ps_set)
	{
		ps_layout = g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(DescriptorCache::Stage::Compute, plan);
	}
	const uint32_t ps_slot = ps_bind.descriptor_set_slot;
	if (ps_slot >= 2u || (has_ps_set && ps_layout == VK_NULL_HANDLE))
	{
		vkDestroyDescriptorSetLayout(device, empty_layout, nullptr);
		return false;
	}

	VkDescriptorSetLayout sets[3] = {empty_layout, empty_layout, transport_layout};
	if (has_ps_set)
	{
		sets[ps_slot] = ps_layout;
	}

	VkPushConstantRange push_constant {};
	uint32_t            push_constant_count = 0;
	if (!ps_bind.vsharp_uniform_buffer && ps_bind.push_constant_size > 0)
	{
		push_constant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		push_constant.offset     = ps_bind.push_constant_offset;
		push_constant.size       = ps_bind.push_constant_size;
		push_constant_count      = 1;
	}

	VkPipelineLayoutCreateInfo layout_info {};
	layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount         = 3;
	layout_info.pSetLayouts            = sets;
	layout_info.pushConstantRangeCount = push_constant_count;
	layout_info.pPushConstantRanges    = push_constant_count != 0 ? &push_constant : nullptr;
	VkResult result = vkCreatePipelineLayout(device, &layout_info, nullptr, layout);

	VkShaderModule module = VK_NULL_HANDLE;
	if (result == VK_SUCCESS)
	{
		VkShaderModuleCreateInfo module_info {};
		module_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		module_info.codeSize = binary.Size() * sizeof(uint32_t);
		module_info.pCode    = binary.GetDataConst();
		result               = vkCreateShaderModule(device, &module_info, nullptr, &module);
	}
	if (result == VK_SUCCESS)
	{
		VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
		required.sType                = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
		required.requiredSubgroupSize = 32;

		VkPipelineShaderStageCreateInfo stage {};
		stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage.pNext  = require_subgroup_size ? &required : nullptr;
		stage.flags  = require_subgroup_size ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0;
		stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
		stage.module = module;
		stage.pName  = "main";

		VkComputePipelineCreateInfo pipeline_info {};
		pipeline_info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipeline_info.stage        = stage;
		pipeline_info.layout       = *layout;
		pipeline_info.basePipelineIndex = -1;
		result                     = vkCreateComputePipelines(device, pipeline_cache, 1, &pipeline_info, nullptr, pipeline);
	}
	if (module != VK_NULL_HANDLE) { vkDestroyShaderModule(device, module, nullptr); }
	vkDestroyDescriptorSetLayout(device, empty_layout, nullptr);
	if (result != VK_SUCCESS)
	{
		if (*layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, *layout, nullptr); }
		*layout   = nullptr;
		*pipeline = nullptr;
	}
	return result == VK_SUCCESS;
}

FragmentTransportRequirement FragmentTransportRequire(const ShaderCode& code, const ShaderPixelInputInfo& pixel, uint32_t user_sgpr_count,
                                                      const PipelineStaticParameters& state, const VkExtent2D& extent)
{
	FragmentTransportRequirement requirement;
	if (!FragmentTransport::ProgramRequiresWaveTransport(code))
	{
		return requirement;
	}
	auto* context = g_render_ctx->GetGraphicCtx();
	const auto native = ShaderAnalyzeNativeWave(code, pixel.native_wave.guest_wave_size);
	// Preflight precedes translation: conservatively allow variation. The final
	// pipeline gate reads the actual checked binary version, including cache hits.
	const auto mapping = ShaderSelectNativeSubgroup(context->compute_wave_vulkan_state, VK_SHADER_STAGE_FRAGMENT_BIT,
	                                                context->subgroup_size, native.guest_wave_size,
	                                                native.proof == ShaderNativeWaveProof::LaneLocal || native.proof == ShaderNativeWaveProof::QuadLocal ||
	                                                    native.proof == ShaderNativeWaveProof::FragmentPartialWave,
	                                                native.proof == ShaderNativeWaveProof::FragmentNeutral32, true);
	// Width and participation are different obligations. A neutral value proof
	// does not make an unavailable/helper-only physical shuffle source defined.
	// The binary gate below pipeline creation checks the complete emitted op set.
	constexpr uint32_t kMaskOps = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
	                              VK_SUBGROUP_FEATURE_QUAD_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
	const bool mask_ops = (context->subgroup_stages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0 &&
	                      (context->subgroup_operations & kMaskOps) == kMaskOps;
	// A participation refusal only says that a narrower host subgroup misses
	// guest source lanes. When the device pins the exact guest width every lane
	// is a real host invocation, so the refusal does not apply; semantic
	// refusals (a mask used as a number and the like) still do.
	const bool  exact_host = native.proof == ShaderNativeWaveProof::ExactSubgroup && mapping.supported &&
	                        mapping.size == native.guest_wave_size;
	const char* refusal = exact_host && ShaderNativeWaveParticipationRefusal(native.refusal_reason) ? nullptr
	                                                                                               : native.refusal_reason;
	if (refusal == nullptr && mapping.supported && mask_ops &&
	    (pixel.required_subgroup_size == 0u || native.proof == ShaderNativeWaveProof::LaneLocal ||
	     native.proof == ShaderNativeWaveProof::QuadLocal || context->shader_maximal_reconvergence_enabled))
	{
		return requirement;
	}
	requirement.required = true;
	const auto host_width = !mapping.supported ? String8("unavailable") :
	                        mapping.size != 0 ? String8::FromPrintf("%u", mapping.size) :
	                                            String8::FromPrintf("varying[%u..%u]", context->compute_wave_vulkan_state.min_subgroup_size,
	                                                                context->compute_wave_vulkan_state.max_subgroup_size);
	String8 facts = String8::FromPrintf("native_admission{guest=%u proof=%u host=%s mapping=%u mask_ops=%u pc=0x%x reason=%s} ",
	                                    native.guest_wave_size, static_cast<uint32_t>(native.proof), host_width.c_str(),
	                                    mapping.supported ? 1u : 0u, mask_ops ? 1u : 0u, native.refusal_pc,
	                                    refusal != nullptr ? refusal :
	                                    (!mapping.supported ? "exact lane map or explicit width-neutral proof unavailable" : "emitted feature unavailable"));
	facts += DrawState(state, extent, pixel);
	if (refusal != nullptr)
	{
		requirement.facts = facts;
		return requirement;
	}
	const auto tier = ShaderAnalyzeFragmentNativeWaveTier(code);
	const char* missing = FragmentTransport::MissingHostCapability(*context);
	ShaderPixelInputInfo resolved = pixel;
	resolved.input_num            = SpirvResolvePixelParameterCount(code, pixel.input_num);
	ShaderFragmentComputeInfo info;
	info.descriptor_set     = kTransportDescriptorSet;
	info.input_binding      = kWaveInputBinding;
	info.output_binding     = kWaveOutputBinding;
	info.initial_vgpr_count = InitialVgprCount(pixel);
	info.user_sgpr_count    = user_sgpr_count;
	info.parameter_state    = ShaderFragmentParameterState::Virtualized;
	const uint32_t            lane_words = 1u + info.initial_vgpr_count + FragmentTransport::BuildLanePacking(code, resolved).words;
	const auto                limits     = HostLimits(*context);

	facts += String8::FromPrintf("missing_host_capability=%s ", missing != nullptr ? missing : "none");
	facts += String8::FromPrintf("native_wave_tier=%u reconvergence=%u tier_reason=%s ", tier.supported ? 1u : 0u,
	                             context->shader_maximal_reconvergence_enabled ? 1u : 0u,
	                             tier.supported ? "admitted" : tier.reason.c_str());
	facts += String8::FromPrintf("virtual_parameter_state=%u partial_wave_reads=%u lane_words=%u initial_vgprs=%u user_sgprs=%u ",
	                             ShaderAnalyzeFragmentVirtualParameterState(code, user_sgpr_count).supported ? 1u : 0u,
	                             ShaderAnalyzeFragmentPartialWaveReads(code).supported ? 1u : 0u, lane_words, info.initial_vgpr_count,
	                             user_sgpr_count);
	facts += Capacities(lane_words, 0u, limits);
	facts += Assembled("capture", FragmentTransport::GenerateCaptureSource(code, resolved, info));
	facts += Assembled("resolve", FragmentTransport::GenerateResolveSource(kTransportDescriptorSet, ColorWriteMask(state)));
	requirement.facts = facts;
	return requirement;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
