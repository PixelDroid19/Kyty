#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOLUTIONIMAGECAPABILITY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOLUTIONIMAGECAPABILITY_H_

#include "Emulator/Graphics/InternalResolutionPolicy.h"

#include <cstdint>

namespace Kyty::Libs::Graphics {

// Host-neutral usage vocabulary. A platform adapter translates its graphics API
// usage flags to this mask before the pure capability decision is evaluated.
enum class ResolutionImageUsage : uint32_t
{
	None                   = 0,
	ColorAttachment        = 1u << 0u,
	DepthStencilAttachment = 1u << 1u,
	Sampled                = 1u << 2u,
	Storage                = 1u << 3u,
	TransferSource         = 1u << 4u,
	TransferDestination    = 1u << 5u,
};

[[nodiscard]] constexpr ResolutionImageUsage operator|(ResolutionImageUsage lhs, ResolutionImageUsage rhs)
{
	return static_cast<ResolutionImageUsage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

[[nodiscard]] constexpr ResolutionImageUsage operator&(ResolutionImageUsage lhs, ResolutionImageUsage rhs)
{
	return static_cast<ResolutionImageUsage>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

[[nodiscard]] constexpr ResolutionImageUsage operator~(ResolutionImageUsage value)
{
	return static_cast<ResolutionImageUsage>(~static_cast<uint32_t>(value));
}

enum class ResolutionImageCapabilityStatus : uint8_t
{
	Supported,
	InvalidRequest,
	InvalidHostCapabilities,
	Unsupported,
};

enum class ResolutionImageCapabilityReason : uint8_t
{
	None,
	ZeroExtent,
	NoRequiredFormatFeatures,
	NoRequiredUsage,
	InvalidSampleCount,
	InvalidHostCapabilities,
	ExceedsMaxImageDimension2D,
	MissingFormatFeatures,
	UnsupportedUsage,
	UnsupportedSampleCount,
};

struct ResolutionHostImageCapabilities
{
	uint32_t             max_image_dimension_2d  = 0;
	uint64_t             format_features         = 0;
	ResolutionImageUsage supported_usage         = ResolutionImageUsage::None;
	uint32_t             supported_sample_counts = 0;
};

struct ResolutionImageCapabilityRequest
{
	ResolutionExtent     extent;
	uint64_t             required_format_features = 0;
	ResolutionImageUsage required_usage           = ResolutionImageUsage::None;
	uint32_t             sample_count             = 0;
};

struct ResolutionImageCapabilityDecision
{
	ResolutionImageCapabilityStatus status = ResolutionImageCapabilityStatus::InvalidRequest;
	ResolutionImageCapabilityReason reason = ResolutionImageCapabilityReason::ZeroExtent;
	ResolutionExtent                extent;
	uint32_t                        sample_count            = 0;
	uint64_t                        missing_format_features = 0;
	ResolutionImageUsage            missing_usage           = ResolutionImageUsage::None;
};

[[nodiscard]] constexpr bool operator==(const ResolutionImageCapabilityDecision& lhs, const ResolutionImageCapabilityDecision& rhs)
{
	return lhs.status == rhs.status && lhs.reason == rhs.reason && lhs.extent == rhs.extent && lhs.sample_count == rhs.sample_count &&
	       lhs.missing_format_features == rhs.missing_format_features && lhs.missing_usage == rhs.missing_usage;
}

[[nodiscard]] constexpr bool operator!=(const ResolutionImageCapabilityDecision& lhs, const ResolutionImageCapabilityDecision& rhs)
{
	return !(lhs == rhs);
}

// Validates only explicit host limits and exact requested properties. Device
// names and vendor identifiers are intentionally absent from this contract.
[[nodiscard]] ResolutionImageCapabilityDecision EvaluateResolutionImageCapability(const ResolutionHostImageCapabilities&  capabilities,
                                                                                  const ResolutionImageCapabilityRequest& request);
[[nodiscard]] const char*                       ResolutionImageCapabilityStatusName(ResolutionImageCapabilityStatus status);
[[nodiscard]] const char*                       ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason reason);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOLUTIONIMAGECAPABILITY_H_ */
