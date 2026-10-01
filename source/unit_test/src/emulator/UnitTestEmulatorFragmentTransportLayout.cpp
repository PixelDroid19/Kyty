#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/FragmentTransportLayout.h"

#include <limits>

UT_BEGIN(EmulatorFragmentTransportLayout);

using namespace Libs::Graphics::FragmentTransport;

static Limits DeviceLimits()
{
	return {MAX_TRANSIENT_BYTES, MAX_TRANSIENT_BYTES, 65535u};
}

TEST(EmulatorFragmentTransportLayout, PreservesPrimitivePartialWavesAndDisjointBufferSpans)
{
	Layout layout {};
	ASSERT_EQ(BuildLayout({1024u, 2u, 8u, 0u}, DeviceLimits(), &layout).failure, Failure::None);
	EXPECT_EQ(layout.quad_words, 36u);
	EXPECT_EQ(layout.wave_capacity, 65u);
	EXPECT_EQ(layout.quad_lookup_capacity, 2048u);
	EXPECT_EQ(layout.wave_lookup_capacity, 256u);
	EXPECT_EQ(layout.quad_lookup_offset, 20u);
	EXPECT_EQ(layout.wave_lookup_offset, 8212u);
	EXPECT_EQ(layout.active_wave_reference_offset, 4096u);
	EXPECT_EQ(layout.record_output_reference_offset, 4161u);
	EXPECT_EQ(layout.input_words_per_wave, 512u);
	EXPECT_EQ(layout.Bytes(Buffer::Records), 147456u);
	EXPECT_EQ(layout.Bytes(Buffer::Control), 33872u);
	EXPECT_EQ(layout.Bytes(Buffer::References), 20740u);
	EXPECT_EQ(layout.Bytes(Buffer::WaveInput), 133120u);
	EXPECT_EQ(layout.Bytes(Buffer::WaveOutput), 565760u);
	EXPECT_EQ(layout.total_bytes, 900948u);
}

TEST(EmulatorFragmentTransportLayout, ReservesAParameterHeaderAndOneWavePerNonemptyPrimitive)
{
	Layout layout {};
	ASSERT_EQ(BuildLayout({17u, 100u, 89u, 1u}, DeviceLimits(), &layout).failure, Failure::None);
	// Every record can belong to a different primitive; capacity must not use
	// ceil(total_quads / 16) alone when assembling primitive-owned waves.
	EXPECT_EQ(layout.wave_capacity, 17u);
	EXPECT_EQ(layout.input_words_per_wave, 5697u);
	EXPECT_EQ(layout.Bytes(Buffer::Records), 17u * 360u * 4u);
}

TEST(EmulatorFragmentTransportLayout, RejectsDeviceAndAggregateLimitsWithoutPublishingAPartialPlan)
{
	Layout layout {};
	layout.quad_words           = 77;
	auto limits                 = DeviceLimits();
	limits.storage_buffer_bytes = 128;
	EXPECT_EQ(BuildLayout({1024u, 2u, 8u, 0u}, limits, &layout).failure, Failure::StorageBufferLimit);
	EXPECT_EQ(layout.quad_words, 77u);
	limits             = DeviceLimits();
	limits.total_bytes = 128;
	EXPECT_EQ(BuildLayout({1024u, 2u, 8u, 0u}, limits, &layout).failure, Failure::TotalBudget);
	limits                   = DeviceLimits();
	limits.dispatch_groups_x = 64;
	EXPECT_EQ(BuildLayout({1024u, 2u, 8u, 0u}, limits, &layout).failure, Failure::DispatchLimit);
	limits = {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max(), UINT32_MAX};
	EXPECT_EQ(BuildLayout({0x100000u, 65535u, 769u, 1u}, limits, &layout).failure, Failure::TotalBudget);
}

TEST(EmulatorFragmentTransportLayout, RejectsUnencodableAndEmptyRequests)
{
	Layout layout {};
	for (const auto request: {Request {0u, 1u, 8u, 0u}, Request {1u, 0u, 8u, 0u}, Request {1u, 1u, 0u, 0u}, Request {0x100001u, 1u, 8u, 0u},
	                          Request {1u, 65536u, 8u, 0u}, Request {1u, 1u, 770u, 0u}, Request {1u, 1u, 8u, 2u}})
	{
		EXPECT_EQ(BuildLayout(request, DeviceLimits(), &layout).failure, Failure::InvalidArgument);
	}
	EXPECT_EQ(BuildLayout({1u, 1u, 8u, 0u}, {}, &layout).failure, Failure::InvalidArgument);
	EXPECT_EQ(BuildLayout({1u, 1u, 8u, 0u}, DeviceLimits(), nullptr).failure, Failure::InvalidArgument);
}

UT_END();
