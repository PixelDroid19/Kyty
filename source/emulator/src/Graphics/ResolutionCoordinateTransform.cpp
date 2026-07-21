#include "Emulator/Graphics/ResolutionCoordinateTransform.h"

#include <cmath>
#include <cstdint>

namespace Kyty::Libs::Graphics {

namespace {

[[nodiscard]] bool IsValidExtent(ResolutionExtent extent)
{
	return extent.width != 0 && extent.height != 0;
}

[[nodiscard]] ResolutionCoordinateStatus ValidateTransform(const ResolutionCoordinateTransform& transform)
{
	if (!IsValidExtent(transform.guest_extent) || !IsValidExtent(transform.host_extent))
	{
		return ResolutionCoordinateStatus::InvalidExtent;
	}

	const double expected_guest_to_host_x = static_cast<double>(transform.host_extent.width) / transform.guest_extent.width;
	const double expected_host_to_guest_x = static_cast<double>(transform.guest_extent.width) / transform.host_extent.width;
	const double expected_guest_to_host_y = static_cast<double>(transform.host_extent.height) / transform.guest_extent.height;
	const double expected_host_to_guest_y = static_cast<double>(transform.guest_extent.height) / transform.host_extent.height;
	if (transform.x.guest_to_host != expected_guest_to_host_x || transform.x.host_to_guest != expected_host_to_guest_x ||
	    transform.y.guest_to_host != expected_guest_to_host_y || transform.y.host_to_guest != expected_host_to_guest_y)
	{
		return ResolutionCoordinateStatus::InvalidTransform;
	}
	return ResolutionCoordinateStatus::Success;
}

[[nodiscard]] bool MapViewportAxis(double origin, double length, double scale, double* mapped_origin, double* mapped_length)
{
	const double end = origin + length;
	if (!std::isfinite(end))
	{
		return false;
	}

	const double scaled_origin = origin * scale;
	const double scaled_end    = end * scale;
	if (!std::isfinite(scaled_origin) || !std::isfinite(scaled_end))
	{
		return false;
	}

	*mapped_origin = scaled_origin;
	*mapped_length = scaled_end - scaled_origin;
	return std::isfinite(*mapped_origin) && std::isfinite(*mapped_length);
}

[[nodiscard]] uint32_t ClipEndpoint(int64_t endpoint, uint32_t limit)
{
	if (endpoint <= 0)
	{
		return 0;
	}
	if (static_cast<uint64_t>(endpoint) >= limit)
	{
		return limit;
	}
	return static_cast<uint32_t>(endpoint);
}

[[nodiscard]] uint32_t ScaleFloor(uint32_t value, uint32_t guest_extent, uint32_t host_extent)
{
	const uint64_t product = static_cast<uint64_t>(value) * host_extent;
	return static_cast<uint32_t>(product / guest_extent);
}

[[nodiscard]] uint32_t ScaleCeil(uint32_t value, uint32_t guest_extent, uint32_t host_extent)
{
	const uint64_t product   = static_cast<uint64_t>(value) * host_extent;
	const uint64_t quotient  = product / guest_extent;
	const uint64_t remainder = product % guest_extent;
	return static_cast<uint32_t>(quotient + (remainder == 0 ? 0 : 1));
}

void MapScissorAxis(int64_t guest_origin, int64_t guest_end, uint32_t guest_extent, uint32_t host_extent, int64_t* host_origin,
                    int64_t* host_end)
{
	const uint32_t clipped_origin = ClipEndpoint(guest_origin, guest_extent);
	const bool     empty          = guest_end <= guest_origin;
	uint32_t       clipped_end    = empty ? clipped_origin : ClipEndpoint(guest_end, guest_extent);
	if (clipped_end < clipped_origin)
	{
		clipped_end = clipped_origin;
	}

	*host_origin = ScaleFloor(clipped_origin, guest_extent, host_extent);
	*host_end    = empty ? *host_origin : ScaleCeil(clipped_end, guest_extent, host_extent);
}

} // namespace

ResolutionCoordinateStatus CreateResolutionCoordinateTransform(ResolutionExtent guest_extent, ResolutionExtent host_extent,
                                                               ResolutionCoordinateTransform* transform)
{
	if (transform == nullptr)
	{
		return ResolutionCoordinateStatus::InvalidArgument;
	}
	if (!IsValidExtent(guest_extent) || !IsValidExtent(host_extent))
	{
		return ResolutionCoordinateStatus::InvalidExtent;
	}

	ResolutionCoordinateTransform result {};
	result.guest_extent    = guest_extent;
	result.host_extent     = host_extent;
	result.x.guest_to_host = static_cast<double>(host_extent.width) / guest_extent.width;
	result.x.host_to_guest = static_cast<double>(guest_extent.width) / host_extent.width;
	result.y.guest_to_host = static_cast<double>(host_extent.height) / guest_extent.height;
	result.y.host_to_guest = static_cast<double>(guest_extent.height) / host_extent.height;
	*transform             = result;
	return ResolutionCoordinateStatus::Success;
}

ResolutionCoordinateStatus MapResolutionViewport(const ResolutionCoordinateTransform& transform, const ResolutionViewport& guest_viewport,
                                                 ResolutionViewport* host_viewport)
{
	if (host_viewport == nullptr)
	{
		return ResolutionCoordinateStatus::InvalidArgument;
	}
	const auto transform_status = ValidateTransform(transform);
	if (transform_status != ResolutionCoordinateStatus::Success)
	{
		return transform_status;
	}
	if (!std::isfinite(guest_viewport.x) || !std::isfinite(guest_viewport.y) || !std::isfinite(guest_viewport.width) ||
	    !std::isfinite(guest_viewport.height) || !std::isfinite(guest_viewport.min_depth) || !std::isfinite(guest_viewport.max_depth))
	{
		return ResolutionCoordinateStatus::NonFiniteViewport;
	}
	if (guest_viewport.width <= 0.0 || guest_viewport.height == 0.0)
	{
		return ResolutionCoordinateStatus::InvalidViewport;
	}

	ResolutionViewport result {};
	if (!MapViewportAxis(guest_viewport.x, guest_viewport.width, transform.x.guest_to_host, &result.x, &result.width) ||
	    !MapViewportAxis(guest_viewport.y, guest_viewport.height, transform.y.guest_to_host, &result.y, &result.height))
	{
		return ResolutionCoordinateStatus::ArithmeticOverflow;
	}
	result.min_depth = guest_viewport.min_depth;
	result.max_depth = guest_viewport.max_depth;
	*host_viewport   = result;
	return ResolutionCoordinateStatus::Success;
}

ResolutionCoordinateStatus MapResolutionScissor(const ResolutionCoordinateTransform& transform, const ResolutionScissorRect& guest_scissor,
                                                ResolutionScissorRect* host_scissor)
{
	if (host_scissor == nullptr)
	{
		return ResolutionCoordinateStatus::InvalidArgument;
	}
	const auto transform_status = ValidateTransform(transform);
	if (transform_status != ResolutionCoordinateStatus::Success)
	{
		return transform_status;
	}

	ResolutionScissorRect result {};
	MapScissorAxis(guest_scissor.left, guest_scissor.right, transform.guest_extent.width, transform.host_extent.width, &result.left,
	               &result.right);
	MapScissorAxis(guest_scissor.top, guest_scissor.bottom, transform.guest_extent.height, transform.host_extent.height, &result.top,
	               &result.bottom);
	*host_scissor = result;
	return ResolutionCoordinateStatus::Success;
}

} // namespace Kyty::Libs::Graphics
