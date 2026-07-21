#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOLUTIONCOORDINATETRANSFORM_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOLUTIONCOORDINATETRANSFORM_H_

#include "Emulator/Graphics/InternalResolutionPolicy.h"

#include <cstdint>

namespace Kyty::Libs::Graphics {

enum class ResolutionCoordinateStatus : uint8_t
{
	Success,
	InvalidArgument,
	InvalidExtent,
	InvalidTransform,
	InvalidViewport,
	NonFiniteViewport,
	ArithmeticOverflow,
};

struct ResolutionAxisTransform
{
	double guest_to_host = 1.0;
	double host_to_guest = 1.0;
};

struct ResolutionCoordinateTransform
{
	ResolutionExtent        guest_extent;
	ResolutionExtent        host_extent;
	ResolutionAxisTransform x;
	ResolutionAxisTransform y;
};

struct ResolutionViewport
{
	double x         = 0.0;
	double y         = 0.0;
	double width     = 0.0;
	double height    = 0.0;
	double min_depth = 0.0;
	double max_depth = 1.0;
};

[[nodiscard]] constexpr bool operator==(const ResolutionViewport& lhs, const ResolutionViewport& rhs)
{
	return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height && lhs.min_depth == rhs.min_depth &&
	       lhs.max_depth == rhs.max_depth;
}

[[nodiscard]] constexpr bool operator!=(const ResolutionViewport& lhs, const ResolutionViewport& rhs)
{
	return !(lhs == rhs);
}

struct ResolutionScissorRect
{
	int64_t left   = 0;
	int64_t top    = 0;
	int64_t right  = 0;
	int64_t bottom = 0;
};

[[nodiscard]] constexpr bool operator==(const ResolutionScissorRect& lhs, const ResolutionScissorRect& rhs)
{
	return lhs.left == rhs.left && lhs.top == rhs.top && lhs.right == rhs.right && lhs.bottom == rhs.bottom;
}

[[nodiscard]] constexpr bool operator!=(const ResolutionScissorRect& lhs, const ResolutionScissorRect& rhs)
{
	return !(lhs == rhs);
}

// The inverse host-to-guest factors are the exact per-axis FragCoord scales.
[[nodiscard]] ResolutionCoordinateStatus CreateResolutionCoordinateTransform(ResolutionExtent guest_extent, ResolutionExtent host_extent,
                                                                             ResolutionCoordinateTransform* transform);

// Vulkan permits a negative viewport height. Mapping expands coverage outwards
// while retaining endpoint order, so inverted viewports remain inverted.
[[nodiscard]] ResolutionCoordinateStatus MapResolutionViewport(const ResolutionCoordinateTransform& transform,
                                                               const ResolutionViewport& guest_viewport, ResolutionViewport* host_viewport);

// Guest scissors are clipped to their framebuffer before mapping. A rectangle
// with a non-positive axis remains a valid empty axis instead of wrapping.
[[nodiscard]] ResolutionCoordinateStatus MapResolutionScissor(const ResolutionCoordinateTransform& transform,
                                                              const ResolutionScissorRect&         guest_scissor,
                                                              ResolutionScissorRect*               host_scissor);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RESOLUTIONCOORDINATETRANSFORM_H_ */
