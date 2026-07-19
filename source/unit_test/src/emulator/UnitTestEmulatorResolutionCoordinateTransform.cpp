#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ResolutionCoordinateTransform.h"

#include <cstdint>
#include <limits>

UT_BEGIN(EmulatorResolutionCoordinateTransform);

using namespace Libs::Graphics;

namespace {

ResolutionCoordinateTransform MakeTransform(ResolutionExtent guest_extent = {3840, 2160}, ResolutionExtent host_extent = {1280, 720})
{
	ResolutionCoordinateTransform transform;
	EXPECT_EQ(CreateResolutionCoordinateTransform(guest_extent, host_extent, &transform), ResolutionCoordinateStatus::Success);
	return transform;
}

} // namespace

TEST(EmulatorResolutionCoordinateTransform, DerivesIndependentExactAxisScales)
{
	ResolutionCoordinateTransform transform;
	ASSERT_EQ(CreateResolutionCoordinateTransform({1919, 1079}, {1280, 720}, &transform), ResolutionCoordinateStatus::Success);

	EXPECT_EQ(transform.guest_extent, (ResolutionExtent {1919, 1079}));
	EXPECT_EQ(transform.host_extent, (ResolutionExtent {1280, 720}));
	EXPECT_DOUBLE_EQ(transform.x.guest_to_host, 1280.0 / 1919.0);
	EXPECT_DOUBLE_EQ(transform.x.host_to_guest, 1919.0 / 1280.0);
	EXPECT_DOUBLE_EQ(transform.y.guest_to_host, 720.0 / 1079.0);
	EXPECT_DOUBLE_EQ(transform.y.host_to_guest, 1079.0 / 720.0);
	EXPECT_NE(transform.x.host_to_guest, transform.y.host_to_guest);
}

TEST(EmulatorResolutionCoordinateTransform, RejectsInvalidExtentsAndNullOutput)
{
	ResolutionCoordinateTransform transform;

	EXPECT_EQ(CreateResolutionCoordinateTransform({0, 1080}, {1280, 720}, &transform), ResolutionCoordinateStatus::InvalidExtent);
	EXPECT_EQ(CreateResolutionCoordinateTransform({1920, 1080}, {0, 720}, &transform), ResolutionCoordinateStatus::InvalidExtent);
	EXPECT_EQ(CreateResolutionCoordinateTransform({1920, 1080}, {1280, 720}, nullptr), ResolutionCoordinateStatus::InvalidArgument);
}

TEST(EmulatorResolutionCoordinateTransform, RejectsATransformWhoseCachedScaleDoesNotMatchItsExtents)
{
	auto               transform = MakeTransform({1920, 1080}, {1280, 720});
	ResolutionViewport viewport {7.0, 8.0, 9.0, 10.0, 0.25, 0.75};
	const auto         sentinel = viewport;
	transform.x.guest_to_host   = 1.0;

	EXPECT_EQ(MapResolutionViewport(transform, viewport, &viewport), ResolutionCoordinateStatus::InvalidTransform);
	EXPECT_EQ(viewport, sentinel);
}

TEST(EmulatorResolutionCoordinateTransform, MapsViewportOriginsDownAndEndsUp)
{
	const auto               transform = MakeTransform({1919, 1079}, {1280, 720});
	const ResolutionViewport guest {1.0, 1.0, 1.0, 1.0, 0.2, 0.8};
	ResolutionViewport       host;

	ASSERT_EQ(MapResolutionViewport(transform, guest, &host), ResolutionCoordinateStatus::Success);

	EXPECT_DOUBLE_EQ(host.x, 0.0);
	EXPECT_DOUBLE_EQ(host.y, 0.0);
	EXPECT_DOUBLE_EQ(host.width, 2.0);
	EXPECT_DOUBLE_EQ(host.height, 2.0);
	EXPECT_DOUBLE_EQ(host.min_depth, guest.min_depth);
	EXPECT_DOUBLE_EQ(host.max_depth, guest.max_depth);
}

TEST(EmulatorResolutionCoordinateTransform, PreservesInvertedViewportOrientation)
{
	const auto         transform = MakeTransform({1920, 1080}, {1280, 720});
	ResolutionViewport host;

	ASSERT_EQ(MapResolutionViewport(transform, {0.0, 1080.0, 1920.0, -1080.0, 0.0, 1.0}, &host), ResolutionCoordinateStatus::Success);
	EXPECT_DOUBLE_EQ(host.x, 0.0);
	EXPECT_DOUBLE_EQ(host.width, 1280.0);
	EXPECT_DOUBLE_EQ(host.y, 720.0);
	EXPECT_DOUBLE_EQ(host.height, -720.0);

	ASSERT_EQ(MapResolutionViewport(transform, {3.0, 1079.0, 3.0, -1.0, 0.0, 1.0}, &host), ResolutionCoordinateStatus::Success);
	EXPECT_DOUBLE_EQ(host.x, 2.0);
	EXPECT_DOUBLE_EQ(host.width, 2.0);
	EXPECT_DOUBLE_EQ(host.y, 720.0);
	EXPECT_DOUBLE_EQ(host.height, -2.0);
}

TEST(EmulatorResolutionCoordinateTransform, SupportsUpscalingAndKeepsViewportBoundsSemantic)
{
	const auto         transform = MakeTransform({1280, 720}, {1920, 1080});
	ResolutionViewport host;

	ASSERT_EQ(MapResolutionViewport(transform, {-10.0, -20.0, 110.0, 120.0, 0.0, 1.0}, &host), ResolutionCoordinateStatus::Success);
	EXPECT_EQ(host, (ResolutionViewport {-15.0, -30.0, 165.0, 180.0, 0.0, 1.0}));
}

TEST(EmulatorResolutionCoordinateTransform, RejectsInvalidViewportWithoutChangingOutput)
{
	const auto               transform = MakeTransform();
	const ResolutionViewport sentinel {7.0, 8.0, 9.0, 10.0, 0.25, 0.75};
	ResolutionViewport       host = sentinel;

	auto invalid  = sentinel;
	invalid.width = 0.0;
	EXPECT_EQ(MapResolutionViewport(transform, invalid, &host), ResolutionCoordinateStatus::InvalidViewport);
	EXPECT_EQ(host, sentinel);

	invalid        = sentinel;
	invalid.height = 0.0;
	EXPECT_EQ(MapResolutionViewport(transform, invalid, &host), ResolutionCoordinateStatus::InvalidViewport);
	EXPECT_EQ(host, sentinel);

	invalid   = sentinel;
	invalid.x = std::numeric_limits<double>::infinity();
	EXPECT_EQ(MapResolutionViewport(transform, invalid, &host), ResolutionCoordinateStatus::NonFiniteViewport);
	EXPECT_EQ(host, sentinel);

	const auto upscale = MakeTransform({1, 1}, {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()});
	invalid            = sentinel;
	invalid.x          = std::numeric_limits<double>::max();
	EXPECT_EQ(MapResolutionViewport(upscale, invalid, &host), ResolutionCoordinateStatus::ArithmeticOverflow);
	EXPECT_EQ(host, sentinel);

	EXPECT_EQ(MapResolutionViewport(transform, sentinel, nullptr), ResolutionCoordinateStatus::InvalidArgument);
}

TEST(EmulatorResolutionCoordinateTransform, ClipsScissorBeforeMapping)
{
	const auto            transform = MakeTransform();
	ResolutionScissorRect host;

	ASSERT_EQ(MapResolutionScissor(transform, {-100, -100, 101, 101}, &host), ResolutionCoordinateStatus::Success);
	EXPECT_EQ(host, (ResolutionScissorRect {0, 0, 34, 34}));

	ASSERT_EQ(MapResolutionScissor(transform, {3800, 2100, 5000, 3000}, &host), ResolutionCoordinateStatus::Success);
	EXPECT_EQ(host, (ResolutionScissorRect {1266, 700, 1280, 720}));
}

TEST(EmulatorResolutionCoordinateTransform, KeepsEmptyScissorsEmptyAndValid)
{
	const auto            transform = MakeTransform();
	ResolutionScissorRect host;

	ASSERT_EQ(MapResolutionScissor(transform, {120, 200, 120, 500}, &host), ResolutionCoordinateStatus::Success);
	EXPECT_EQ(host.left, host.right);
	EXPECT_EQ(host.top, 66);
	EXPECT_EQ(host.bottom, 167);

	ASSERT_EQ(MapResolutionScissor(transform, {5000, 5000, 4000, 4000}, &host), ResolutionCoordinateStatus::Success);
	EXPECT_EQ(host, (ResolutionScissorRect {1280, 720, 1280, 720}));
}

TEST(EmulatorResolutionCoordinateTransform, UpscalesScissorAndRejectsNullOutputWithoutMutation)
{
	const auto            transform = MakeTransform({1280, 720}, {1920, 1080});
	ResolutionScissorRect host {7, 8, 9, 10};

	ASSERT_EQ(MapResolutionScissor(transform, {1, 1, 2, 2}, &host), ResolutionCoordinateStatus::Success);
	EXPECT_EQ(host, (ResolutionScissorRect {1, 1, 3, 3}));

	EXPECT_EQ(MapResolutionScissor(transform, {0, 0, 1, 1}, nullptr), ResolutionCoordinateStatus::InvalidArgument);
}

TEST(EmulatorResolutionCoordinateTransform, ClipsExtremeScissorEndpointsWithoutOverflow)
{
	const auto            transform = MakeTransform();
	ResolutionScissorRect host;

	ASSERT_EQ(MapResolutionScissor(transform,
	                               {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min(),
	                                std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()},
	                               &host),
	          ResolutionCoordinateStatus::Success);
	EXPECT_EQ(host, (ResolutionScissorRect {0, 0, 1280, 720}));
}

UT_END();
