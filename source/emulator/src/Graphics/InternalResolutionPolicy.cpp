#include "Emulator/Graphics/InternalResolutionPolicy.h"

#include <limits>
#include <numeric>

namespace Kyty::Libs::Graphics {

namespace {

[[nodiscard]] bool ScaleFloor(uint32_t value, ResolutionScale scale, uint32_t* result)
{
	if (result == nullptr || scale.denominator == 0)
	{
		return false;
	}

	const uint64_t scaled = static_cast<uint64_t>(value) * scale.numerator / scale.denominator;
	if (scaled > std::numeric_limits<uint32_t>::max())
	{
		return false;
	}
	*result = static_cast<uint32_t>(scaled);
	return true;
}

[[nodiscard]] bool ScaleCeil(uint32_t value, ResolutionScale scale, uint32_t* result)
{
	if (result == nullptr || scale.denominator == 0)
	{
		return false;
	}

	const uint64_t product = static_cast<uint64_t>(value) * scale.numerator;
	const uint64_t scaled  = product / scale.denominator + (product % scale.denominator != 0 ? 1u : 0u);
	if (scaled == 0 || scaled > std::numeric_limits<uint32_t>::max())
	{
		return false;
	}
	*result = static_cast<uint32_t>(scaled);
	return true;
}

} // namespace

InternalResolutionPolicy::InternalResolutionPolicy(ResolutionExtent target_extent): m_target_extent(target_extent) {}

ResolutionPolicyStatus InternalResolutionPolicy::SetTargetExtent(ResolutionExtent target_extent)
{
	if (!IsValidExtent(target_extent))
	{
		return ResolutionPolicyStatus::InvalidExtent;
	}
	m_target_extent = target_extent;
	return ResolutionPolicyStatus::Success;
}

void InternalResolutionPolicy::SetScaleMode(ResolutionScaleMode mode)
{
	m_mode = mode;
}

ResolutionPolicyStatus InternalResolutionPolicy::RegisterGuestDisplayExtent(ResolutionExtent guest_extent)
{
	if (!IsValidExtent(m_target_extent) || !IsValidExtent(guest_extent))
	{
		return ResolutionPolicyStatus::InvalidExtent;
	}
	m_guest_display_extent     = guest_extent;
	m_guest_display_registered = true;
	return ResolutionPolicyStatus::Success;
}

ResolutionExtent InternalResolutionPolicy::GetTargetExtent() const
{
	return m_target_extent;
}

ResolutionDecision InternalResolutionPolicy::Evaluate(ResolutionExtent guest_resource_extent, ResolutionResourceInfo resource) const
{
	ResolutionDecision decision;
	decision.guest_extent = guest_resource_extent;
	decision.host_extent  = guest_resource_extent;
	decision.identity = {m_target_extent, m_guest_display_extent, guest_resource_extent, guest_resource_extent, {1, 1}, resource, m_mode};

	if (!m_guest_display_registered || !IsValidExtent(m_target_extent) || !IsValidExtent(m_guest_display_extent) ||
	    !IsValidExtent(guest_resource_extent))
	{
		return decision;
	}

	if (m_mode == ResolutionScaleMode::Native)
	{
		decision.classification = ResolutionClassification::Native;
		decision.native_reason  = ResolutionNativeReason::PolicyDisabled;
		return decision;
	}

	const auto native_reason = NativeReason(resource);
	if (native_reason != ResolutionNativeReason::None)
	{
		decision.classification = ResolutionClassification::Native;
		decision.native_reason  = native_reason;
		return decision;
	}

	ResolutionScale scale;
	if (!CalculateScale(&scale))
	{
		decision.native_reason = ResolutionNativeReason::ArithmeticOverflow;
		return decision;
	}

	ResolutionExtent host_extent;
	if (!ScaleCeil(guest_resource_extent.width, scale, &host_extent.width) ||
	    !ScaleCeil(guest_resource_extent.height, scale, &host_extent.height))
	{
		decision.native_reason = ResolutionNativeReason::ArithmeticOverflow;
		return decision;
	}

	decision.host_extent                   = host_extent;
	decision.scale                         = scale;
	decision.identity.host_resource_extent = host_extent;
	decision.identity.scale                = scale;
	if (host_extent == guest_resource_extent)
	{
		decision.classification = ResolutionClassification::Native;
		decision.native_reason  = ResolutionNativeReason::IdentityScale;
	} else
	{
		decision.classification = ResolutionClassification::Scaled;
		decision.native_reason  = ResolutionNativeReason::None;
	}
	return decision;
}

ResolutionPolicyStatus InternalResolutionPolicy::MapRect(const ResolutionDecision& decision, ResolutionRect guest_rect,
                                                         ResolutionRect* host_rect) const
{
	if (host_rect == nullptr)
	{
		return ResolutionPolicyStatus::InvalidArgument;
	}
	if (decision.classification == ResolutionClassification::Unsupported)
	{
		return ResolutionPolicyStatus::Unsupported;
	}
	if (guest_rect.width == 0 || guest_rect.height == 0)
	{
		return ResolutionPolicyStatus::InvalidExtent;
	}

	const uint64_t guest_right  = static_cast<uint64_t>(guest_rect.x) + guest_rect.width;
	const uint64_t guest_bottom = static_cast<uint64_t>(guest_rect.y) + guest_rect.height;
	if (guest_right > std::numeric_limits<uint32_t>::max() || guest_bottom > std::numeric_limits<uint32_t>::max())
	{
		return ResolutionPolicyStatus::ArithmeticOverflow;
	}
	if (guest_right > decision.guest_extent.width || guest_bottom > decision.guest_extent.height)
	{
		return ResolutionPolicyStatus::RectOutOfBounds;
	}

	uint32_t left   = 0;
	uint32_t top    = 0;
	uint32_t right  = 0;
	uint32_t bottom = 0;
	if (!ScaleFloor(guest_rect.x, decision.scale, &left) || !ScaleFloor(guest_rect.y, decision.scale, &top) ||
	    !ScaleCeil(static_cast<uint32_t>(guest_right), decision.scale, &right) ||
	    !ScaleCeil(static_cast<uint32_t>(guest_bottom), decision.scale, &bottom) || right <= left || bottom <= top)
	{
		return ResolutionPolicyStatus::ArithmeticOverflow;
	}

	*host_rect = {left, top, right - left, bottom - top};
	return ResolutionPolicyStatus::Success;
}

bool InternalResolutionPolicy::IsValidExtent(ResolutionExtent extent)
{
	return extent.width != 0 && extent.height != 0;
}

ResolutionNativeReason InternalResolutionPolicy::NativeReason(const ResolutionResourceInfo& resource)
{
	if (resource.kind != ResolutionResourceKind::ColorAttachment && resource.kind != ResolutionResourceKind::DepthStencilAttachment)
	{
		return ResolutionNativeReason::ResourceKind;
	}
	if (resource.compressed)
	{
		return ResolutionNativeReason::Compressed;
	}
	if (resource.dimension != ResolutionImageDimension::TwoD)
	{
		return ResolutionNativeReason::UnsupportedDimension;
	}
	if (resource.mip_levels != 1)
	{
		return ResolutionNativeReason::Mipmapped;
	}
	if (resource.sample_count != 1)
	{
		return ResolutionNativeReason::Multisampled;
	}
	if (resource.shader_writable)
	{
		return ResolutionNativeReason::ShaderWritable;
	}
	if (resource.cpu_transfer)
	{
		return ResolutionNativeReason::CpuTransfer;
	}
	if (resource.ambiguous_alias)
	{
		return ResolutionNativeReason::AmbiguousAlias;
	}
	return ResolutionNativeReason::None;
}

bool InternalResolutionPolicy::CalculateScale(ResolutionScale* scale) const
{
	if (scale == nullptr || !m_guest_display_registered || !IsValidExtent(m_target_extent) || !IsValidExtent(m_guest_display_extent))
	{
		return false;
	}

	const uint64_t width_cross  = static_cast<uint64_t>(m_target_extent.width) * m_guest_display_extent.height;
	const uint64_t height_cross = static_cast<uint64_t>(m_target_extent.height) * m_guest_display_extent.width;

	uint32_t numerator   = 0;
	uint32_t denominator = 0;
	if (width_cross <= height_cross)
	{
		numerator   = m_target_extent.width;
		denominator = m_guest_display_extent.width;
	} else
	{
		numerator   = m_target_extent.height;
		denominator = m_guest_display_extent.height;
	}

	const uint32_t divisor = std::gcd(numerator, denominator);
	*scale                 = {numerator / divisor, denominator / divisor};
	return true;
}

} // namespace Kyty::Libs::Graphics
