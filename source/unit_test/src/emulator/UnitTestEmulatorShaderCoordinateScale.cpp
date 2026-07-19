#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderCoordinateScale.h"

#include <cstdint>
#include <limits>

UT_BEGIN(EmulatorShaderCoordinateScale);

using namespace Libs::Graphics;

TEST(EmulatorShaderCoordinateScale, ReducesEachHostToGuestAxisExactly)
{
	ShaderHostToGuestScale scale;
	ASSERT_EQ(BuildShaderHostToGuestScale({3840, 1080}, {1280, 720}, &scale), ShaderCoordinateScaleStatus::Success);

	EXPECT_EQ(scale.x_guest_numerator, 3u);
	EXPECT_EQ(scale.x_host_denominator, 1u);
	EXPECT_EQ(scale.y_guest_numerator, 3u);
	EXPECT_EQ(scale.y_host_denominator, 2u);
	EXPECT_TRUE(scale.IsValid());
	EXPECT_FALSE(scale.IsIdentity());
}

TEST(EmulatorShaderCoordinateScale, PreservesNonDivisibleRatioWithoutApproximating)
{
	ShaderHostToGuestScale scale;
	ASSERT_EQ(BuildShaderHostToGuestScale({362, 721}, {121, 480}, &scale), ShaderCoordinateScaleStatus::Success);

	EXPECT_EQ(scale.x_guest_numerator, 362u);
	EXPECT_EQ(scale.x_host_denominator, 121u);
	EXPECT_EQ(scale.y_guest_numerator, 721u);
	EXPECT_EQ(scale.y_host_denominator, 480u);
}

TEST(EmulatorShaderCoordinateScale, ProducesCanonicalIdentity)
{
	ShaderHostToGuestScale scale {7, 9, 11, 13};
	ASSERT_EQ(BuildShaderHostToGuestScale({1280, 720}, {1280, 720}, &scale), ShaderCoordinateScaleStatus::Success);

	EXPECT_EQ(scale.x_guest_numerator, 1u);
	EXPECT_EQ(scale.x_host_denominator, 1u);
	EXPECT_EQ(scale.y_guest_numerator, 1u);
	EXPECT_EQ(scale.y_host_denominator, 1u);
	EXPECT_TRUE(scale.IsIdentity());
}

TEST(EmulatorShaderCoordinateScale, RejectsZeroAndNullWithoutChangingOutput)
{
	const ShaderHostToGuestScale sentinel {7, 9, 11, 13};
	auto                         scale = sentinel;

	EXPECT_EQ(BuildShaderHostToGuestScale({0, 720}, {1280, 720}, &scale), ShaderCoordinateScaleStatus::InvalidExtent);
	EXPECT_EQ(scale, sentinel);
	EXPECT_EQ(BuildShaderHostToGuestScale({1280, 720}, {1280, 0}, &scale), ShaderCoordinateScaleStatus::InvalidExtent);
	EXPECT_EQ(scale, sentinel);
	EXPECT_EQ(BuildShaderHostToGuestScale({1280, 720}, {1280, 720}, nullptr), ShaderCoordinateScaleStatus::InvalidArgument);
}

TEST(EmulatorShaderCoordinateScale, ReportsReducedValuesThatDoNotFitShaderIdentity)
{
	const ShaderHostToGuestScale sentinel {7, 9, 11, 13};
	auto                         scale = sentinel;

	EXPECT_EQ(BuildShaderHostToGuestScale({std::numeric_limits<uint64_t>::max(), 1}, {1, 1}, &scale),
	          ShaderCoordinateScaleStatus::ArithmeticOverflow);
	EXPECT_EQ(scale, sentinel);
}

UT_END();
