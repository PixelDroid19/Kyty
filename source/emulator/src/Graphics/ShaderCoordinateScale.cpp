#include "Emulator/Graphics/ShaderCoordinateScale.h"

#include <limits>
#include <numeric>

namespace Kyty::Libs::Graphics {

namespace {

[[nodiscard]] bool ReduceAxis(uint64_t guest, uint64_t host, uint32_t* numerator, uint32_t* denominator)
{
	const uint64_t divisor            = std::gcd(guest, host);
	const uint64_t reduced_guest      = guest / divisor;
	const uint64_t reduced_host       = host / divisor;
	constexpr auto kShaderIdentityMax = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
	if (reduced_guest > kShaderIdentityMax || reduced_host > kShaderIdentityMax)
	{
		return false;
	}
	*numerator   = static_cast<uint32_t>(reduced_guest);
	*denominator = static_cast<uint32_t>(reduced_host);
	return true;
}

} // namespace

ShaderCoordinateScaleStatus BuildShaderHostToGuestScale(ShaderCoordinateExtent guest_extent, ShaderCoordinateExtent host_extent,
                                                        ShaderHostToGuestScale* scale)
{
	if (scale == nullptr)
	{
		return ShaderCoordinateScaleStatus::InvalidArgument;
	}
	if (guest_extent.width == 0 || guest_extent.height == 0 || host_extent.width == 0 || host_extent.height == 0)
	{
		return ShaderCoordinateScaleStatus::InvalidExtent;
	}

	ShaderHostToGuestScale result;
	if (!ReduceAxis(guest_extent.width, host_extent.width, &result.x_guest_numerator, &result.x_host_denominator) ||
	    !ReduceAxis(guest_extent.height, host_extent.height, &result.y_guest_numerator, &result.y_host_denominator))
	{
		return ShaderCoordinateScaleStatus::ArithmeticOverflow;
	}
	*scale = result;
	return ShaderCoordinateScaleStatus::Success;
}

} // namespace Kyty::Libs::Graphics
