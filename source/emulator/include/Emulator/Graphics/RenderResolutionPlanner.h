#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONPLANNER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONPLANNER_H_

#include "Emulator/Graphics/RenderResolutionPolicy.h"

#include <cstdint>

namespace Kyty::Libs::Graphics {

class ShaderCode;

struct RenderResolutionAttachment
{
	ResolutionExtent       guest_extent;
	ResolutionResourceInfo resource;
};

struct RenderShaderCoordinateUsage
{
	bool fragment_coordinates           = false;
	bool fragment_coordinates_supported = false;
	bool integer_image_coordinates      = false;
	bool image_size_query               = false;
};

enum class RenderResolutionPlanReason : uint8_t
{
	None,
	Empty,
	InvalidInput,
	Incomplete,
	AttachmentNotScalable,
	MismatchedGuestExtent,
	MismatchedHostExtent,
	MismatchedScale,
	ShaderCoordinateAccess,
	ColorCapabilityUnsupported,
	DepthCapabilityUnsupported,
};

[[nodiscard]] const char* RenderResolutionPlanReasonName(RenderResolutionPlanReason reason);

struct RenderResolutionPlanInput
{
	const RenderResolutionAttachment* attachments      = nullptr;
	uint32_t                             attachment_count = 0;
	uint32_t                             expected_count   = 0;
	RenderShaderCoordinateUsage      shader_usage;
};

struct RenderResolutionPlan
{
	ResolutionClassification classification           = ResolutionClassification::Unsupported;
	RenderResolutionPlanReason   reason                   = RenderResolutionPlanReason::InvalidInput;
	ResolutionNativeReason   attachment_native_reason = ResolutionNativeReason::None;
	ResolutionExtent         guest_extent;
	ResolutionExtent         host_extent;
	ResolutionScale          scale;
	uint32_t                 attachment_count = 0;
	uint32_t                 blocking_attachment_index = UINT32_MAX;
};

// A render target is scaled only when every active color/depth attachment is
// present and resolves to one exact scale. This contract intentionally stays
// independent from Vulkan object creation so incomplete cohorts cannot resize
// a single attachment.
[[nodiscard]] RenderResolutionPlan EvaluateRenderResolutionPlan(const RenderResolutionPolicy& policy, const RenderResolutionPlanInput& input);
[[nodiscard]] RenderResolutionPlan EvaluateNativeRenderExtentCompatibility(ResolutionExtent guest_extent,
                                                                                 ResolutionExtent registered_host_extent);
[[nodiscard]] RenderResolutionPlan EvaluateDepthOnlyRenderExtentCompatibility(
    ResolutionExtent guest_extent, ResolutionExtent registered_host_extent, const RenderResolutionPlan& scalable_candidate);
// Only instruction-derived hazards are reported here. Fragment-coordinate use
// comes from normalized pixel input state, and its support is set only after a
// valid host-to-guest scale has been built.
[[nodiscard]] RenderShaderCoordinateUsage AnalyzeResolutionShaderUsage(const ShaderCode& code);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERRESOLUTIONPLANNER_H_ */
