#include "Emulator/Graphics/RenderResolutionImageCapability.h"

namespace Kyty::Libs::Graphics {

namespace {

[[nodiscard]] bool IsValidSampleCount(uint32_t sample_count)
{
	constexpr uint32_t kKnownSampleCounts = 1u | 2u | 4u | 8u | 16u | 32u | 64u;
	return sample_count != 0 && (sample_count & (sample_count - 1u)) == 0 && (sample_count & kKnownSampleCounts) != 0;
}

} // namespace

RenderResolutionImageCapabilityDecision EvaluateRenderResolutionImageCapability(const ResolutionHostImageCapabilities&  capabilities,
                                                                    const RenderResolutionImageCapabilityRequest& request)
{
	RenderResolutionImageCapabilityDecision decision {};
	decision.extent       = request.extent;
	decision.sample_count = request.sample_count;

	if (request.extent.width == 0 || request.extent.height == 0)
	{
		decision.reason = RenderResolutionImageCapabilityReason::ZeroExtent;
		return decision;
	}
	if (request.required_format_features == 0)
	{
		decision.reason = RenderResolutionImageCapabilityReason::NoRequiredFormatFeatures;
		return decision;
	}
	if (request.required_usage == ResolutionImageUsage::None)
	{
		decision.reason = RenderResolutionImageCapabilityReason::NoRequiredUsage;
		return decision;
	}
	if (!IsValidSampleCount(request.sample_count))
	{
		decision.reason = RenderResolutionImageCapabilityReason::InvalidSampleCount;
		return decision;
	}
	if (capabilities.max_image_dimension_2d == 0 || capabilities.supported_sample_counts == 0)
	{
		decision.status = RenderResolutionImageCapabilityStatus::InvalidHostCapabilities;
		decision.reason = RenderResolutionImageCapabilityReason::InvalidHostCapabilities;
		return decision;
	}
	if (request.extent.width > capabilities.max_image_dimension_2d || request.extent.height > capabilities.max_image_dimension_2d)
	{
		decision.status = RenderResolutionImageCapabilityStatus::Unsupported;
		decision.reason = RenderResolutionImageCapabilityReason::ExceedsMaxImageDimension2D;
		return decision;
	}

	decision.missing_format_features = request.required_format_features & ~capabilities.format_features;
	if (decision.missing_format_features != 0)
	{
		decision.status = RenderResolutionImageCapabilityStatus::Unsupported;
		decision.reason = RenderResolutionImageCapabilityReason::MissingFormatFeatures;
		return decision;
	}

	decision.missing_usage = request.required_usage & ~capabilities.supported_usage;
	if (decision.missing_usage != ResolutionImageUsage::None)
	{
		decision.status = RenderResolutionImageCapabilityStatus::Unsupported;
		decision.reason = RenderResolutionImageCapabilityReason::UnsupportedUsage;
		return decision;
	}
	if ((request.sample_count & capabilities.supported_sample_counts) == 0)
	{
		decision.status = RenderResolutionImageCapabilityStatus::Unsupported;
		decision.reason = RenderResolutionImageCapabilityReason::UnsupportedSampleCount;
		return decision;
	}

	decision.status = RenderResolutionImageCapabilityStatus::Supported;
	decision.reason = RenderResolutionImageCapabilityReason::None;
	return decision;
}

const char* RenderResolutionImageCapabilityStatusName(RenderResolutionImageCapabilityStatus status)
{
	switch (status)
	{
		case RenderResolutionImageCapabilityStatus::Supported: return "supported";
		case RenderResolutionImageCapabilityStatus::InvalidRequest: return "invalid_request";
		case RenderResolutionImageCapabilityStatus::InvalidHostCapabilities: return "invalid_host_capabilities";
		case RenderResolutionImageCapabilityStatus::Unsupported: return "unsupported";
	}
	return "unknown";
}

const char* RenderResolutionImageCapabilityReasonName(RenderResolutionImageCapabilityReason reason)
{
	switch (reason)
	{
		case RenderResolutionImageCapabilityReason::None: return "none";
		case RenderResolutionImageCapabilityReason::ZeroExtent: return "zero_extent";
		case RenderResolutionImageCapabilityReason::NoRequiredFormatFeatures: return "no_required_format_features";
		case RenderResolutionImageCapabilityReason::NoRequiredUsage: return "no_required_usage";
		case RenderResolutionImageCapabilityReason::InvalidSampleCount: return "invalid_sample_count";
		case RenderResolutionImageCapabilityReason::InvalidHostCapabilities: return "invalid_host_capabilities";
		case RenderResolutionImageCapabilityReason::ExceedsMaxImageDimension2D: return "exceeds_max_image_dimension_2d";
		case RenderResolutionImageCapabilityReason::MissingFormatFeatures: return "missing_format_features";
		case RenderResolutionImageCapabilityReason::UnsupportedUsage: return "unsupported_usage";
		case RenderResolutionImageCapabilityReason::UnsupportedSampleCount: return "unsupported_sample_count";
	}
	return "unknown";
}

} // namespace Kyty::Libs::Graphics
