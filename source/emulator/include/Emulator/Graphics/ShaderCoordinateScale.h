#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERCOORDINATESCALE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERCOORDINATESCALE_H_

#include <cstdint>

namespace Kyty::Libs::Graphics {

struct ShaderCoordinateExtent
{
	uint64_t width  = 0;
	uint64_t height = 0;
};

// Converts host attachment coordinates back to guest coordinates. Reduced
// ratios ensure mathematically equivalent scales share shader cache identity.
struct ShaderHostToGuestScale
{
	uint32_t x_guest_numerator  = 1;
	uint32_t x_host_denominator = 1;
	uint32_t y_guest_numerator  = 1;
	uint32_t y_host_denominator = 1;

	[[nodiscard]] constexpr bool IsValid() const
	{
		return x_guest_numerator != 0 && x_host_denominator != 0 && y_guest_numerator != 0 && y_host_denominator != 0;
	}

	[[nodiscard]] constexpr bool IsIdentity() const
	{
		return x_guest_numerator == x_host_denominator && y_guest_numerator == y_host_denominator;
	}
};

[[nodiscard]] constexpr bool operator==(const ShaderHostToGuestScale& lhs, const ShaderHostToGuestScale& rhs)
{
	return lhs.x_guest_numerator == rhs.x_guest_numerator && lhs.x_host_denominator == rhs.x_host_denominator &&
	       lhs.y_guest_numerator == rhs.y_guest_numerator && lhs.y_host_denominator == rhs.y_host_denominator;
}

[[nodiscard]] constexpr bool operator!=(const ShaderHostToGuestScale& lhs, const ShaderHostToGuestScale& rhs)
{
	return !(lhs == rhs);
}

enum class ShaderCoordinateScaleStatus : uint8_t
{
	Success,
	InvalidArgument,
	InvalidExtent,
	ArithmeticOverflow,
};

[[nodiscard]] ShaderCoordinateScaleStatus BuildShaderHostToGuestScale(ShaderCoordinateExtent guest_extent,
                                                                      ShaderCoordinateExtent host_extent, ShaderHostToGuestScale* scale);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERCOORDINATESCALE_H_ */
