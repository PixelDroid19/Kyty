#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_INTERNALRESOLUTIONPOLICY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_INTERNALRESOLUTIONPOLICY_H_

#include <cstdint>

namespace Kyty::Libs::Graphics {

struct ResolutionExtent
{
	uint32_t width  = 0;
	uint32_t height = 0;
};

[[nodiscard]] constexpr bool operator==(ResolutionExtent lhs, ResolutionExtent rhs)
{
	return lhs.width == rhs.width && lhs.height == rhs.height;
}

[[nodiscard]] constexpr bool operator!=(ResolutionExtent lhs, ResolutionExtent rhs)
{
	return !(lhs == rhs);
}

struct ResolutionRect
{
	uint32_t x      = 0;
	uint32_t y      = 0;
	uint32_t width  = 0;
	uint32_t height = 0;
};

[[nodiscard]] constexpr bool operator==(ResolutionRect lhs, ResolutionRect rhs)
{
	return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

[[nodiscard]] constexpr bool operator!=(ResolutionRect lhs, ResolutionRect rhs)
{
	return !(lhs == rhs);
}

struct ResolutionScale
{
	uint32_t numerator   = 1;
	uint32_t denominator = 1;
};

[[nodiscard]] constexpr bool operator==(ResolutionScale lhs, ResolutionScale rhs)
{
	return lhs.numerator == rhs.numerator && lhs.denominator == rhs.denominator;
}

[[nodiscard]] constexpr bool operator!=(ResolutionScale lhs, ResolutionScale rhs)
{
	return !(lhs == rhs);
}

enum class ResolutionResourceKind : uint8_t
{
	ColorAttachment,
	DepthStencilAttachment,
	SampledImage,
	StorageImage,
	Buffer,
};

enum class ResolutionImageDimension : uint8_t
{
	OneD,
	TwoD,
	ThreeD,
	Cube,
};

enum class ResolutionScaleMode : uint8_t
{
	Automatic,
	Native,
};

struct ResolutionResourceInfo
{
	ResolutionResourceKind   kind            = ResolutionResourceKind::Buffer;
	bool                     compressed      = false;
	ResolutionImageDimension dimension       = ResolutionImageDimension::TwoD;
	uint32_t                 mip_levels      = 1;
	uint32_t                 sample_count    = 1;
	bool                     shader_writable = false;
	bool                     cpu_transfer    = false;
	bool                     ambiguous_alias = false;
};

[[nodiscard]] constexpr bool operator==(const ResolutionResourceInfo& lhs, const ResolutionResourceInfo& rhs)
{
	return lhs.kind == rhs.kind && lhs.compressed == rhs.compressed && lhs.dimension == rhs.dimension && lhs.mip_levels == rhs.mip_levels &&
	       lhs.sample_count == rhs.sample_count && lhs.shader_writable == rhs.shader_writable && lhs.cpu_transfer == rhs.cpu_transfer &&
	       lhs.ambiguous_alias == rhs.ambiguous_alias;
}

[[nodiscard]] constexpr bool operator!=(const ResolutionResourceInfo& lhs, const ResolutionResourceInfo& rhs)
{
	return !(lhs == rhs);
}

enum class ResolutionClassification : uint8_t
{
	Scaled,
	Native,
	Unsupported,
};

enum class ResolutionNativeReason : uint8_t
{
	None,
	PolicyDisabled,
	ResourceKind,
	Compressed,
	UnsupportedDimension,
	Mipmapped,
	Multisampled,
	ShaderWritable,
	CpuTransfer,
	AmbiguousAlias,
	IdentityScale,
	InvalidExtent,
	ArithmeticOverflow,
};

enum class ResolutionPolicyStatus : uint8_t
{
	Success,
	InvalidArgument,
	InvalidExtent,
	ArithmeticOverflow,
	RectOutOfBounds,
	Unsupported,
};

struct ResolutionIdentity
{
	ResolutionExtent       target_extent;
	ResolutionExtent       guest_display_extent;
	ResolutionExtent       guest_resource_extent;
	ResolutionExtent       host_resource_extent;
	ResolutionScale        scale;
	ResolutionResourceInfo resource;
	ResolutionScaleMode    mode = ResolutionScaleMode::Automatic;
};

[[nodiscard]] constexpr bool operator==(const ResolutionIdentity& lhs, const ResolutionIdentity& rhs)
{
	return lhs.target_extent == rhs.target_extent && lhs.guest_display_extent == rhs.guest_display_extent &&
	       lhs.guest_resource_extent == rhs.guest_resource_extent && lhs.host_resource_extent == rhs.host_resource_extent &&
	       lhs.scale == rhs.scale && lhs.resource == rhs.resource && lhs.mode == rhs.mode;
}

[[nodiscard]] constexpr bool operator!=(const ResolutionIdentity& lhs, const ResolutionIdentity& rhs)
{
	return !(lhs == rhs);
}

struct ResolutionDecision
{
	ResolutionClassification classification = ResolutionClassification::Unsupported;
	ResolutionNativeReason   native_reason  = ResolutionNativeReason::InvalidExtent;
	ResolutionExtent         guest_extent;
	ResolutionExtent         host_extent;
	ResolutionScale          scale;
	ResolutionIdentity       identity;
};

// Pure, capability-neutral policy. Guest extents remain immutable; the returned
// host extent is a separate cache/resource identity input.
class InternalResolutionPolicy
{
public:
	explicit InternalResolutionPolicy(ResolutionExtent target_extent = {1280, 720});

	ResolutionPolicyStatus SetTargetExtent(ResolutionExtent target_extent);
	void                   SetScaleMode(ResolutionScaleMode mode);
	ResolutionPolicyStatus RegisterGuestDisplayExtent(ResolutionExtent guest_extent);

	[[nodiscard]] ResolutionExtent       GetTargetExtent() const;
	[[nodiscard]] ResolutionDecision     Evaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource) const;
	[[nodiscard]] ResolutionPolicyStatus MapRect(const ResolutionDecision& decision, ResolutionRect guest_rect,
	                                             ResolutionRect* host_rect) const;

private:
	[[nodiscard]] static bool                   IsValidExtent(ResolutionExtent extent);
	[[nodiscard]] static ResolutionNativeReason NativeReason(const ResolutionResourceInfo& resource);
	[[nodiscard]] bool                          CalculateScale(ResolutionScale* scale) const;

	ResolutionExtent    m_target_extent;
	ResolutionExtent    m_guest_display_extent;
	ResolutionScaleMode m_mode                     = ResolutionScaleMode::Automatic;
	bool                m_guest_display_registered = false;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_INTERNALRESOLUTIONPOLICY_H_ */
