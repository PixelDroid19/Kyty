#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderComputeWaveLayout.h"

#include <array>
#include <limits>

UT_BEGIN(EmulatorComputeWaveLayout);

using namespace Libs::Graphics;

static ShaderComputeWaveRequest SupportedRequest()
{
	return {{16, 16, 1}, {2, 3, 1}, 0x41, 1664, ShaderGuestLaneOrder::LinearXFirst};
}

static ShaderComputeWaveCapabilities SupportedCapabilities()
{
	return {true, true, true, true, 8, 32, {1024, 1024, 64}, {65535, 65535, 65535}, 1024, 128, 49152};
}

static ShaderComputeWaveLayout SentinelLayout()
{
	ShaderComputeWaveLayout layout {};
	layout.strategy             = ShaderComputeWaveStrategy::Native;
	layout.guest_local[0]       = 11;
	layout.guest_local[1]       = 12;
	layout.guest_local[2]       = 13;
	layout.physical_local[0]    = 21;
	layout.physical_local[1]    = 22;
	layout.physical_local[2]    = 23;
	layout.guest_wave_size      = 31;
	layout.native_subgroup_size = 32;
	layout.banks                = 33;
	layout.waves                = 34;
	layout.lds_dwords           = 35;
	return layout;
}

static void ExpectLayoutUnchanged(const ShaderComputeWaveLayout& actual, const ShaderComputeWaveLayout& expected)
{
	EXPECT_EQ(actual.strategy, expected.strategy);
	for (uint32_t i = 0; i < 3; ++i)
	{
		EXPECT_EQ(actual.guest_local[i], expected.guest_local[i]);
		EXPECT_EQ(actual.physical_local[i], expected.physical_local[i]);
	}
	EXPECT_EQ(actual.guest_wave_size, expected.guest_wave_size);
	EXPECT_EQ(actual.native_subgroup_size, expected.native_subgroup_size);
	EXPECT_EQ(actual.banks, expected.banks);
	EXPECT_EQ(actual.waves, expected.waves);
	EXPECT_EQ(actual.lds_dwords, expected.lds_dwords);
}

static void ExpectRejected(const ShaderComputeWaveRequest& request, const ShaderComputeWaveCapabilities& capabilities,
                           ShaderComputeWaveLayoutStatus expected_status)
{
	auto       layout = SentinelLayout();
	const auto before = layout;
	EXPECT_EQ(ShaderBuildPairedComputeWaveLayout(request, capabilities, &layout), expected_status);
	ExpectLayoutUnchanged(layout, before);
}

TEST(EmulatorComputeWaveLayout, PreservesGuestGroupAndPairsFourWaves)
{
	const auto              request      = SupportedRequest();
	const auto              capabilities = SupportedCapabilities();
	ShaderComputeWaveLayout layout {};

	ASSERT_EQ(ShaderBuildPairedComputeWaveLayout(request, capabilities, &layout), ShaderComputeWaveLayoutStatus::Supported);
	EXPECT_EQ(layout.strategy, ShaderComputeWaveStrategy::Paired64On32);
	EXPECT_EQ(layout.physical_local[0], 128u);
	EXPECT_EQ(layout.physical_local[1], 1u);
	EXPECT_EQ(layout.physical_local[2], 1u);
	EXPECT_EQ(layout.guest_local[0], 16u);
	EXPECT_EQ(layout.guest_local[1], 16u);
	EXPECT_EQ(layout.guest_local[2], 1u);
	EXPECT_EQ(layout.guest_wave_size, 64u);
	EXPECT_EQ(layout.native_subgroup_size, 32u);
	EXPECT_EQ(layout.waves, 4u);
	EXPECT_EQ(layout.banks, 2u);
	EXPECT_EQ(layout.lds_dwords, 1664u);
}

TEST(EmulatorComputeWaveLayout, AcceptsEitherDocumentedWaveLaunchOrder)
{
	for (const uint32_t mode: std::array<uint32_t, 2> {0x01u, 0x41u})
	{
		auto request          = SupportedRequest();
		request.dispatch_mode = mode;
		ShaderComputeWaveLayout layout {};
		EXPECT_EQ(ShaderBuildPairedComputeWaveLayout(request, SupportedCapabilities(), &layout), ShaderComputeWaveLayoutStatus::Supported);
	}
}

TEST(EmulatorComputeWaveLayout, MapsVerifiedFullWaveShapesToCheckedPhysicalX)
{
	struct Case
	{
		uint32_t local[3];
		uint32_t physical_x;
	};
	const std::array<Case, 3> cases {{{{64, 1, 1}, 32}, {{256, 1, 1}, 128}, {{16, 16, 1}, 128}}};

	for (const auto& test_case: cases)
	{
		auto request = SupportedRequest();
		for (uint32_t i = 0; i < 3; ++i)
		{
			request.local[i] = test_case.local[i];
		}

		ShaderComputeWaveLayout layout {};
		ASSERT_EQ(ShaderBuildPairedComputeWaveLayout(request, SupportedCapabilities(), &layout), ShaderComputeWaveLayoutStatus::Supported);
		EXPECT_EQ(layout.physical_local[0], test_case.physical_x);
		EXPECT_EQ(layout.physical_local[1], 1u);
		EXPECT_EQ(layout.physical_local[2], 1u);
	}
}

TEST(EmulatorComputeWaveLayout, ZeroGroupsReturnNoWorkWithoutChangingOutput)
{
	auto request          = SupportedRequest();
	request.local[0]      = 0;
	request.dispatch_mode = 0x43; // Partial-threadgroup control is irrelevant when the grid has no work.
	request.groups[1]     = 0;
	auto       layout     = SentinelLayout();
	const auto before     = layout;

	EXPECT_EQ(ShaderBuildPairedComputeWaveLayout(request, SupportedCapabilities(), &layout), ShaderComputeWaveLayoutStatus::NoWork);
	ExpectLayoutUnchanged(layout, before);
}

TEST(EmulatorComputeWaveLayout, RejectsNullOutput)
{
	EXPECT_EQ(ShaderBuildPairedComputeWaveLayout(SupportedRequest(), SupportedCapabilities(), nullptr),
	          ShaderComputeWaveLayoutStatus::InvalidArgument);
}

TEST(EmulatorComputeWaveLayout, RejectsInvalidOrOverflowingLocalProducts)
{
	const auto                                   capabilities = SupportedCapabilities();
	const std::array<std::array<uint32_t, 3>, 3> invalid_local {
	    {{0, 64, 1}, {std::numeric_limits<uint32_t>::max(), 2, 1}, {65536, 65535, 2}}};
	for (const auto& local: invalid_local)
	{
		auto request = SupportedRequest();
		for (uint32_t i = 0; i < 3; ++i)
		{
			request.local[i] = local[i];
		}
		ExpectRejected(request, capabilities, ShaderComputeWaveLayoutStatus::InvalidLocalSize);
	}
}

// Since 704f4ad3 a trailing partial wave is admitted: the wave count is the
// ceiling of the guest invocations over 64 and the missing lanes stay inactive
// through the valid-lane mask, so each wave still occupies a whole 32-lane bank.
TEST(EmulatorComputeWaveLayout, AdmitsPartialWavesAndSizesPhysicalXByCeilingWaves)
{
	struct Case
	{
		uint32_t invocations;
		uint32_t waves;
		uint32_t physical_x;
	};
	const std::array<Case, 6> cases {{{1, 1, 32}, {32, 1, 32}, {33, 1, 32}, {64, 1, 32}, {65, 2, 64}, {129, 3, 96}}};

	for (const auto& test_case: cases)
	{
		auto request     = SupportedRequest();
		request.local[0] = test_case.invocations;
		request.local[1] = 1;
		request.local[2] = 1;

		ShaderComputeWaveLayout layout {};
		ASSERT_EQ(ShaderBuildPairedComputeWaveLayout(request, SupportedCapabilities(), &layout), ShaderComputeWaveLayoutStatus::Supported)
		    << test_case.invocations;
		EXPECT_EQ(layout.waves, test_case.waves) << test_case.invocations;
		EXPECT_EQ(layout.physical_local[0], test_case.physical_x) << test_case.invocations;
		EXPECT_EQ(layout.guest_local[0], test_case.invocations);
	}
}

TEST(EmulatorComputeWaveLayout, StillChecksHostLimitsForPartialWaves)
{
	auto request     = SupportedRequest();
	request.local[0] = 65; // two waves, 64 physical lanes
	request.local[1] = 1;
	request.local[2] = 1;

	auto caps          = SupportedCapabilities();
	caps.max_subgroups = 1;
	ExpectRejected(request, caps, ShaderComputeWaveLayoutStatus::HostLimitExceeded);
}

TEST(EmulatorComputeWaveLayout, CeilingCannotWrapNonemptyWorkToZero)
{
	// The old rounding addition wraps throughout the last 63 uint32_t values.
	// Ordinary host limits must reject them without publishing a zero layout.
	for (uint32_t distance = 0; distance <= 64u; ++distance)
	{
		auto request     = SupportedRequest();
		request.local[0] = std::numeric_limits<uint32_t>::max() - distance;
		request.local[1] = request.local[2] = 1;
		ExpectRejected(request, SupportedCapabilities(), ShaderComputeWaveLayoutStatus::HostLimitExceeded);

		// A synthetic permissive host also pins the arithmetic result directly.
		auto caps              = SupportedCapabilities();
		caps.max_local_size[0] = std::numeric_limits<uint32_t>::max();
		caps.max_invocations  = std::numeric_limits<uint32_t>::max();
		caps.max_subgroups    = std::numeric_limits<uint32_t>::max();
		ShaderComputeWaveLayout layout {};
		ASSERT_EQ(ShaderBuildPairedComputeWaveLayout(request, caps, &layout), ShaderComputeWaveLayoutStatus::Supported);
		const uint64_t expected_waves = (static_cast<uint64_t>(request.local[0]) + 63u) / 64u;
		EXPECT_EQ(layout.waves, expected_waves);
		EXPECT_EQ(layout.physical_local[0], expected_waves * 32u);
	}
}

TEST(EmulatorComputeWaveLayout, RejectsUnverifiedGuestLaneOrder)
{
	auto request       = SupportedRequest();
	request.lane_order = ShaderGuestLaneOrder::Unverified;
	ExpectRejected(request, SupportedCapabilities(), ShaderComputeWaveLayoutStatus::UnverifiedLaneOrder);
}

TEST(EmulatorComputeWaveLayout, RejectsDeferredDispatchControlsBeforeInspectingLocalSize)
{
	for (const uint32_t mode: std::array<uint32_t, 7> {0x43u, 0x45u, 0x49u, 0x51u, 0x61u, 0x40u, 0xc1u})
	{
		auto request          = SupportedRequest();
		request.dispatch_mode = mode;
		request.local[0]      = 0;
		ExpectRejected(request, SupportedCapabilities(), ShaderComputeWaveLayoutStatus::UnsupportedDispatchMode);
	}
}

TEST(EmulatorComputeWaveLayout, RejectsWave32InitiatorBit)
{
	auto request = SupportedRequest();
	request.dispatch_mode |= 0x8000u;
	ExpectRejected(request, SupportedCapabilities(), ShaderComputeWaveLayoutStatus::UnsupportedWidth);
}

TEST(EmulatorComputeWaveLayout, RequiresEverySubgroupFeatureAndAHostNative32Range)
{
	const std::array<ShaderComputeWaveCapabilities, 6> cases = []
	{
		auto values = std::array<ShaderComputeWaveCapabilities, 6> {};
		for (auto& caps: values)
		{
			caps = SupportedCapabilities();
		}
		values[0].size_control_enabled             = false;
		values[1].full_subgroups_enabled           = false;
		values[2].compute_required_size_supported  = false;
		values[3].compute_ballot_shuffle_supported = false;
		values[4].min_subgroup_size                = 33;
		values[5].max_subgroup_size                = 31;
		return values;
	}();

	for (const auto& capabilities: cases)
	{
		ExpectRejected(SupportedRequest(), capabilities, ShaderComputeWaveLayoutStatus::MissingHostCapability);
	}
}

TEST(EmulatorComputeWaveLayout, RejectsEveryExceededPhysicalAndLdsLimit)
{
	auto caps              = SupportedCapabilities();
	auto request           = SupportedRequest();
	caps.max_local_size[0] = 127;
	ExpectRejected(request, caps, ShaderComputeWaveLayoutStatus::HostLimitExceeded);

	caps                   = SupportedCapabilities();
	caps.max_local_size[1] = 0;
	ExpectRejected(request, caps, ShaderComputeWaveLayoutStatus::HostLimitExceeded);

	caps                   = SupportedCapabilities();
	caps.max_local_size[2] = 0;
	ExpectRejected(request, caps, ShaderComputeWaveLayoutStatus::HostLimitExceeded);

	caps                 = SupportedCapabilities();
	caps.max_invocations = 127;
	ExpectRejected(request, caps, ShaderComputeWaveLayoutStatus::HostLimitExceeded);

	caps               = SupportedCapabilities();
	caps.max_subgroups = 3;
	ExpectRejected(request, caps, ShaderComputeWaveLayoutStatus::HostLimitExceeded);

	caps                  = SupportedCapabilities();
	caps.max_shared_bytes = 6655;
	ExpectRejected(request, caps, ShaderComputeWaveLayoutStatus::HostLimitExceeded);

	request.lds_dwords    = std::numeric_limits<uint32_t>::max() / 4u + 1u;
	caps                  = SupportedCapabilities();
	caps.max_shared_bytes = std::numeric_limits<uint32_t>::max();
	ExpectRejected(request, caps, ShaderComputeWaveLayoutStatus::HostLimitExceeded);
}

TEST(EmulatorComputeWaveLayout, ChecksEachDispatchAxisAtAndAboveItsLimit)
{
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		auto caps            = SupportedCapabilities();
		auto request         = SupportedRequest();
		request.groups[axis] = caps.max_group_count[axis];
		ShaderComputeWaveLayout layout {};
		ASSERT_EQ(ShaderBuildPairedComputeWaveLayout(request, caps, &layout), ShaderComputeWaveLayoutStatus::Supported);

		request.groups[axis] += 1u;
		ExpectRejected(request, caps, ShaderComputeWaveLayoutStatus::HostLimitExceeded);
	}
}

UT_END();
