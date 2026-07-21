#include "Emulator/Graphics/ResolutionImageCapability.h"

namespace Kyty::Libs::Graphics {

namespace {

[[nodiscard]] bool IsValidSampleCount(uint32_t sample_count)
{
	constexpr uint32_t kKnownSampleCounts = 1u | 2u | 4u | 8u | 16u | 32u | 64u;
	return sample_count != 0 && (sample_count & (sample_count - 1u)) == 0 && (sample_count & kKnownSampleCounts) != 0;
}

} // namespace

ResolutionImageCapabilityDecision EvaluateResolutionImageCapability(const ResolutionHostImageCapabilities&  capabilities,
                                                                    const ResolutionImageCapabilityRequest& request)
{
	ResolutionImageCapabilityDecision decision {};
	decision.extent       = request.extent;
	decision.sample_count = request.sample_count;

	if (request.extent.width == 0 || request.extent.height == 0)
	{
		decision.reason = ResolutionImageCapabilityReason::ZeroExtent;
		return decision;
	}
	if (request.required_format_features == 0)
	{
		decision.reason = ResolutionImageCapabilityReason::NoRequiredFormatFeatures;
		return decision;
	}
	if (request.required_usage == ResolutionImageUsage::None)
	{
		decision.reason = ResolutionImageCapabilityReason::NoRequiredUsage;
		return decision;
	}
	if (!IsValidSampleCount(request.sample_count))
	{
		decision.reason = ResolutionImageCapabilityReason::InvalidSampleCount;
		return decision;
	}
	if (capabilities.max_image_dimension_2d == 0 || capabilities.supported_sample_counts == 0)
	{
		decision.status = ResolutionImageCapabilityStatus::InvalidHostCapabilities;
		decision.reason = ResolutionImageCapabilityReason::InvalidHostCapabilities;
		return decision;
	}
	if (request.extent.width > capabilities.max_image_dimension_2d || request.extent.height > capabilities.max_image_dimension_2d)
	{
		decision.status = ResolutionImageCapabilityStatus::Unsupported;
		decision.reason = ResolutionImageCapabilityReason::ExceedsMaxImageDimension2D;
		return decision;
	}

	decision.missing_format_features = request.required_format_features & ~capabilities.format_features;
	if (decision.missing_format_features != 0)
	{
		decision.status = ResolutionImageCapabilityStatus::Unsupported;
		decision.reason = ResolutionImageCapabilityReason::MissingFormatFeatures;
		return decision;
	}

	decision.missing_usage = request.required_usage & ~capabilities.supported_usage;
	if (decision.missing_usage != ResolutionImageUsage::None)
	{
		decision.status = ResolutionImageCapabilityStatus::Unsupported;
		decision.reason = ResolutionImageCapabilityReason::UnsupportedUsage;
		return decision;
	}
	if ((request.sample_count & capabilities.supported_sample_counts) == 0)
	{
		decision.status = ResolutionImageCapabilityStatus::Unsupported;
		decision.reason = ResolutionImageCapabilityReason::UnsupportedSampleCount;
		return decision;
	}

	decision.status = ResolutionImageCapabilityStatus::Supported;
	decision.reason = ResolutionImageCapabilityReason::None;
	return decision;
}

const char* ResolutionImageCapabilityStatusName(ResolutionImageCapabilityStatus status)
{
	switch (status)
	{
		case ResolutionImageCapabilityStatus::Supported: return "supported";
		case ResolutionImageCapabilityStatus::InvalidRequest: return "invalid_request";
		case ResolutionImageCapabilityStatus::InvalidHostCapabilities: return "invalid_host_capabilities";
		case ResolutionImageCapabilityStatus::Unsupported: return "unsupported";
	}
	return "unknown";
}

const char* ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason reason)
{
	switch (reason)
	{
		case ResolutionImageCapabilityReason::None: return "none";
		case ResolutionImageCapabilityReason::ZeroExtent: return "zero_extent";
		case ResolutionImageCapabilityReason::NoRequiredFormatFeatures: return "no_required_format_features";
		case ResolutionImageCapabilityReason::NoRequiredUsage: return "no_required_usage";
		case ResolutionImageCapabilityReason::InvalidSampleCount: return "invalid_sample_count";
		case ResolutionImageCapabilityReason::InvalidHostCapabilities: return "invalid_host_capabilities";
		case ResolutionImageCapabilityReason::ExceedsMaxImageDimension2D: return "exceeds_max_image_dimension_2d";
		case ResolutionImageCapabilityReason::MissingFormatFeatures: return "missing_format_features";
		case ResolutionImageCapabilityReason::UnsupportedUsage: return "unsupported_usage";
		case ResolutionImageCapabilityReason::UnsupportedSampleCount: return "unsupported_sample_count";
	}
	return "unknown";
}

} // namespace Kyty::Libs::Graphics
