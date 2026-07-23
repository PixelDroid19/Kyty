#include "Emulator/Graphics/AttachmentResolutionCohort.h"

#include "Emulator/Graphics/Shader.h"

namespace Kyty::Libs::Graphics {

const char* ResolutionCohortReasonName(ResolutionCohortReason reason)
{
	switch (reason)
	{
		case ResolutionCohortReason::None: return "none";
		case ResolutionCohortReason::Empty: return "empty";
		case ResolutionCohortReason::InvalidInput: return "invalid_input";
		case ResolutionCohortReason::Incomplete: return "incomplete";
		case ResolutionCohortReason::AttachmentNotScalable: return "attachment_not_scalable";
		case ResolutionCohortReason::MismatchedGuestExtent: return "mismatched_guest_extent";
		case ResolutionCohortReason::MismatchedHostExtent: return "mismatched_host_extent";
		case ResolutionCohortReason::MismatchedScale: return "mismatched_scale";
		case ResolutionCohortReason::ShaderCoordinateAccess: return "shader_coordinate_access";
		case ResolutionCohortReason::ColorCapabilityUnsupported: return "color_capability_unsupported";
		case ResolutionCohortReason::DepthCapabilityUnsupported: return "depth_capability_unsupported";
	}
	return "unknown";
}

namespace {

ResolutionCohortDecision NativeDecision(ResolutionCohortReason reason, uint32_t attachment_count,
                                        ResolutionNativeReason attachment_reason = ResolutionNativeReason::None,
                                        ResolutionExtent       guest_extent      = {}, uint32_t blocking_attachment_index = UINT32_MAX)
{
	ResolutionCohortDecision decision;
	decision.classification           = ResolutionClassification::Native;
	decision.reason                   = reason;
	decision.attachment_native_reason = attachment_reason;
	decision.guest_extent             = guest_extent;
	decision.host_extent              = guest_extent;
	decision.attachment_count         = attachment_count;
	decision.blocking_attachment_index = blocking_attachment_index;
	return decision;
}

} // namespace

ResolutionCohortDecision EvaluateResolutionCohort(const InternalResolutionPolicy& policy, const ResolutionCohortInput& input)
{
	if (input.attachment_count == 0 && input.expected_count == 0)
	{
		return NativeDecision(ResolutionCohortReason::Empty, 0);
	}
	if (input.attachments == nullptr)
	{
		return NativeDecision(ResolutionCohortReason::InvalidInput, input.attachment_count);
	}
	if (input.attachment_count != input.expected_count)
	{
		return NativeDecision(ResolutionCohortReason::Incomplete, input.attachment_count, ResolutionNativeReason::None,
		                      input.attachments[0].guest_extent);
	}
	if ((input.shader_usage.fragment_coordinates && !input.shader_usage.fragment_coordinates_supported) ||
	    input.shader_usage.integer_image_coordinates || input.shader_usage.image_size_query)
	{
		return NativeDecision(ResolutionCohortReason::ShaderCoordinateAccess, input.attachment_count, ResolutionNativeReason::None,
		                      input.attachments[0].guest_extent);
	}

	const ResolutionExtent first_guest_extent = input.attachments[0].guest_extent;
	for (uint32_t index = 1; index < input.attachment_count; index++)
	{
		if (input.attachments[index].guest_extent != first_guest_extent)
		{
			return NativeDecision(ResolutionCohortReason::MismatchedGuestExtent, input.attachment_count, ResolutionNativeReason::None,
			                      first_guest_extent, index);
		}
	}

	const ResolutionDecision first = policy.Evaluate(first_guest_extent, input.attachments[0].resource);
	if (first.classification != ResolutionClassification::Scaled)
	{
		return NativeDecision(ResolutionCohortReason::AttachmentNotScalable, input.attachment_count, first.native_reason,
		                      first_guest_extent, 0);
	}

	for (uint32_t index = 1; index < input.attachment_count; index++)
	{
		const ResolutionDecision current = policy.Evaluate(input.attachments[index].guest_extent, input.attachments[index].resource);
		if (current.classification != ResolutionClassification::Scaled)
		{
			return NativeDecision(ResolutionCohortReason::AttachmentNotScalable, input.attachment_count, current.native_reason,
			                      first_guest_extent, index);
		}
		if (current.host_extent != first.host_extent)
		{
			return NativeDecision(ResolutionCohortReason::MismatchedHostExtent, input.attachment_count, ResolutionNativeReason::None,
			                      first_guest_extent, index);
		}
		if (current.scale != first.scale)
		{
			return NativeDecision(ResolutionCohortReason::MismatchedScale, input.attachment_count, ResolutionNativeReason::None,
			                      first_guest_extent, index);
		}
	}

	ResolutionCohortDecision decision;
	decision.classification   = ResolutionClassification::Scaled;
	decision.reason           = ResolutionCohortReason::None;
	decision.guest_extent     = first.guest_extent;
	decision.host_extent      = first.host_extent;
	decision.scale            = first.scale;
	decision.attachment_count = input.attachment_count;
	return decision;
}

ResolutionCohortDecision EvaluateNativeDisplayExtentCompatibility(ResolutionExtent guest_extent,
                                                                  ResolutionExtent registered_host_extent)
{
	ResolutionCohortDecision decision;
	decision.guest_extent     = guest_extent;
	decision.host_extent      = registered_host_extent;
	decision.scale            = {1, 1};
	decision.attachment_count = 1;
	if (guest_extent.width == 0 || guest_extent.height == 0 || registered_host_extent.width == 0 ||
	    registered_host_extent.height == 0)
	{
		decision.classification = ResolutionClassification::Unsupported;
		decision.reason         = ResolutionCohortReason::InvalidInput;
		return decision;
	}
	if (registered_host_extent != guest_extent)
	{
		decision.classification            = ResolutionClassification::Unsupported;
		decision.reason                    = ResolutionCohortReason::MismatchedHostExtent;
		decision.blocking_attachment_index = 0;
		return decision;
	}

	decision.classification = ResolutionClassification::Native;
	decision.reason         = ResolutionCohortReason::None;
	return decision;
}

ResolutionCohortDecision EvaluateDepthOnlyDisplayExtentCompatibility(
    ResolutionExtent guest_extent, ResolutionExtent registered_host_extent, const ResolutionCohortDecision& scalable_candidate)
{
	if (registered_host_extent == guest_extent)
	{
		return EvaluateNativeDisplayExtentCompatibility(guest_extent, registered_host_extent);
	}

	ResolutionCohortDecision decision = scalable_candidate;
	decision.guest_extent             = guest_extent;
	decision.host_extent              = registered_host_extent;
	decision.attachment_count         = 1;
	if (guest_extent.width == 0 || guest_extent.height == 0 || registered_host_extent.width == 0 ||
	    registered_host_extent.height == 0)
	{
		decision.classification = ResolutionClassification::Unsupported;
		decision.reason         = ResolutionCohortReason::InvalidInput;
		return decision;
	}
	if (scalable_candidate.classification != ResolutionClassification::Scaled ||
	    scalable_candidate.guest_extent != guest_extent || scalable_candidate.host_extent != registered_host_extent)
	{
		decision.classification = ResolutionClassification::Unsupported;
		decision.reason = scalable_candidate.classification == ResolutionClassification::Scaled
		                      ? ResolutionCohortReason::MismatchedHostExtent
		                      : (scalable_candidate.reason == ResolutionCohortReason::None
		                             ? ResolutionCohortReason::AttachmentNotScalable
		                             : scalable_candidate.reason);
		return decision;
	}

	return decision;
}

ResolutionShaderCoordinateUsage AnalyzeResolutionShaderUsage(const ShaderCode& code)
{
	ResolutionShaderCoordinateUsage usage;
	usage.integer_image_coordinates =
	    code.HasAnyOf({ShaderInstructionType::ImageLoad, ShaderInstructionType::ImageStore, ShaderInstructionType::ImageStoreMip});
	// image_get_resinfo is still a structured unsupported parser opcode and has
	// no ShaderInstructionType. A supported decoder must set this bit when that
	// opcode is introduced; it cannot currently reach this analysis silently.
	return usage;
}

} // namespace Kyty::Libs::Graphics
