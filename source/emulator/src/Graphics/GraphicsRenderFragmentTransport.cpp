#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String8.h"

#include "Emulator/Graphics/FragmentTransportAdmission.h"
#include "Emulator/Graphics/FragmentTransportCapture.h"
#include "Emulator/Graphics/FragmentTransportLayout.h"
#include "Emulator/Graphics/FragmentTransportResolve.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderSpirv.h"

#include "GraphicsRenderInternal.h"
#include "ShaderSpirvToolchain.h"

#include <algorithm>

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

FragmentTransportRequirement FragmentTransportRequire(const ShaderCode& code, const ShaderPixelInputInfo& pixel, uint32_t user_sgpr_count,
                                                      const PipelineStaticParameters& state, const VkExtent2D& extent)
{
	FragmentTransportRequirement requirement;
	if (!FragmentTransport::ProgramRequiresWaveTransport(code))
	{
		return requirement;
	}
	requirement.required          = true;
	auto*                context  = g_render_ctx->GetGraphicCtx();
	const char*          missing  = FragmentTransport::MissingHostCapability(*context);
	ShaderPixelInputInfo resolved = pixel;
	resolved.input_num            = SpirvResolvePixelParameterCount(code, pixel.input_num);
	ShaderFragmentComputeInfo info {kTransportDescriptorSet, kWaveInputBinding, kWaveOutputBinding,
	                                InitialVgprCount(pixel), user_sgpr_count,   ShaderFragmentParameterState::Virtualized};
	const uint32_t            lane_words = 1u + info.initial_vgpr_count + resolved.input_num * 16u;
	const auto                limits     = HostLimits(*context);

	String8 facts = String8::FromPrintf("missing_host_capability=%s ", missing != nullptr ? missing : "none");
	facts += String8::FromPrintf("virtual_parameter_state=%u partial_wave_reads=%u lane_words=%u initial_vgprs=%u user_sgprs=%u ",
	                             ShaderAnalyzeFragmentVirtualParameterState(code, user_sgpr_count).supported ? 1u : 0u,
	                             ShaderAnalyzeFragmentPartialWaveReads(code).supported ? 1u : 0u, lane_words, info.initial_vgpr_count,
	                             user_sgpr_count);
	facts += DrawState(state, extent, pixel);
	facts += Capacities(lane_words, 0u, limits);
	facts += Assembled("capture", FragmentTransport::GenerateCaptureSource(code, resolved, info));
	facts += Assembled("shade", SpirvGenerateFragmentComputeSource(code, pixel, info));
	facts += Assembled("resolve", FragmentTransport::GenerateResolveSource(kTransportDescriptorSet, ColorWriteMask(state)));
	requirement.facts = facts;
	return requirement;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
