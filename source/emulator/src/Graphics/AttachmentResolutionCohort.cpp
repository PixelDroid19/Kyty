#include "Emulator/Graphics/AttachmentResolutionCohort.h"

namespace Kyty::Libs::Graphics {

namespace {

ResolutionCohortDecision NativeDecision(ResolutionCohortReason reason, uint32_t attachment_count,
                                        ResolutionNativeReason attachment_reason = ResolutionNativeReason::None,
                                        ResolutionExtent       guest_extent      = {})
{
	ResolutionCohortDecision decision;
	decision.classification           = ResolutionClassification::Native;
	decision.reason                   = reason;
	decision.attachment_native_reason = attachment_reason;
	decision.guest_extent             = guest_extent;
	decision.host_extent              = guest_extent;
	decision.attachment_count         = attachment_count;
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
	if (input.shader_usage.fragment_coordinates || input.shader_usage.integer_image_coordinates || input.shader_usage.image_size_query)
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
			                      first_guest_extent);
		}
	}

	const ResolutionDecision first = policy.Evaluate(first_guest_extent, input.attachments[0].resource);
	if (first.classification != ResolutionClassification::Scaled)
	{
		return NativeDecision(ResolutionCohortReason::AttachmentNotScalable, input.attachment_count, first.native_reason,
		                      first_guest_extent);
	}

	for (uint32_t index = 1; index < input.attachment_count; index++)
	{
		const ResolutionDecision current = policy.Evaluate(input.attachments[index].guest_extent, input.attachments[index].resource);
		if (current.classification != ResolutionClassification::Scaled)
		{
			return NativeDecision(ResolutionCohortReason::AttachmentNotScalable, input.attachment_count, current.native_reason,
			                      first_guest_extent);
		}
		if (current.host_extent != first.host_extent)
		{
			return NativeDecision(ResolutionCohortReason::MismatchedHostExtent, input.attachment_count, ResolutionNativeReason::None,
			                      first_guest_extent);
		}
		if (current.scale != first.scale)
		{
			return NativeDecision(ResolutionCohortReason::MismatchedScale, input.attachment_count, ResolutionNativeReason::None,
			                      first_guest_extent);
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

} // namespace Kyty::Libs::Graphics
