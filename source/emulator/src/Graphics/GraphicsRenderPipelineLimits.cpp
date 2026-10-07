#include "GraphicsRenderPipelineLimits.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderDescriptorLayoutPlan.h"
#include "Emulator/Graphics/ShaderDescriptorLimits.h"

#include "GraphicsRenderDescriptorLimits.h"

#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

namespace Limits = ShaderDescriptorLimits;

void RequireDescriptorLimit(Limits::Check result)
{
	if (result.failure != Limits::Failure::None)
	{
		EXIT("pipeline descriptor limit: %s requested=%" PRIu64 " limit=%" PRIu64 "\n",
		     Limits::FailureName(result.failure), result.requested, result.limit);
	}
}

Limits::DescriptorCounts CountStage(const ShaderBindResources& bind)
{
	Limits::DescriptorCounts counts {};
	if (!ShaderBindRequiresDescriptorSet(bind)) { return counts; }
	ShaderDescriptorLayoutPlan plan {};
	EXIT_IF(!ShaderBuildDescriptorLayoutPlan(bind, &plan));
	RequireDescriptorLimit(ShaderCountDescriptorLayoutPlan(plan, &counts));
	return counts;
}

Limits::DeviceLimits DeviceLimits(GraphicContext* context)
{
	EXIT_IF(context == nullptr || context->physical_device == VK_NULL_HANDLE);
	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(context->physical_device, &properties);
	return ShaderDescriptorLimitsFromVulkan(properties.limits);
}

} // namespace

void ValidatePipelineDescriptorLimits(GraphicContext* context, const ShaderBindResources* const* stages,
                                      size_t stage_count, bool graphics_probe, uint32_t color_attachments, uint32_t fragment_outputs)
{
	EXIT_IF(context == nullptr || context->physical_device == VK_NULL_HANDLE || stages == nullptr);
	EXIT_IF(stage_count == 0 || stage_count > 2 || (graphics_probe && stage_count != 2));
	EXIT_IF(stage_count == 1 && (color_attachments != 0 || fragment_outputs != 0));
	const auto limits = DeviceLimits(context);
	Limits::DescriptorCounts total {};
	const Limits::DescriptorCounts probe {0, 0, 0, graphics_probe ? 1u : 0u, 0};
	for (size_t stage = 0; stage < stage_count; ++stage)
	{
		EXIT_IF(stages[stage] == nullptr);
		const auto counts = CountStage(*stages[stage]);
		Limits::DescriptorCounts accessible {};
		RequireDescriptorLimit(Limits::AddCounts(counts, probe, &accessible));
		RequireDescriptorLimit(stage == 1 ? Limits::CheckFragment(accessible, limits, color_attachments, fragment_outputs)
		                                  : Limits::CheckPerStage(accessible, limits));
		RequireDescriptorLimit(Limits::AddCounts(total, counts, &total));
	}
	RequireDescriptorLimit(Limits::AddCounts(total, probe, &total));
	RequireDescriptorLimit(Limits::CheckPipelineLayout(total, limits));
}

void ValidateDepthCopyDescriptorLimits(GraphicContext* context, const ShaderBindResources* vertex_bind, bool expand_to_color)
{
	const auto limits = DeviceLimits(context);
	const auto vertex = vertex_bind == nullptr ? Limits::DescriptorCounts {} : CountStage(*vertex_bind);
	// The expansion fragment set contains two combined image samplers. Each
	// counts once as a sampled image and a sampler, but only once as a resource.
	const Limits::DescriptorCounts fragment = expand_to_color ? Limits::DescriptorCounts {2, 0, 2, 0, 0}
	                                                         : Limits::DescriptorCounts {};
	RequireDescriptorLimit(Limits::CheckPerStage(vertex, limits));
	RequireDescriptorLimit(Limits::CheckFragment(fragment, limits, expand_to_color ? 1u : 0u, expand_to_color ? 1u : 0u));
	Limits::DescriptorCounts total {};
	RequireDescriptorLimit(Limits::AddCounts(vertex, fragment, &total));
	RequireDescriptorLimit(Limits::CheckPipelineLayout(total, limits));
}

} // namespace Kyty::Libs::Graphics

#endif
