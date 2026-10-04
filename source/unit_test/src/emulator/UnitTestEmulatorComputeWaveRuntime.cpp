#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderComputeWaveRuntime.h"

#include <array>
#include <cstdint>
#include <limits>

UT_BEGIN(EmulatorComputeWaveRuntime);

using namespace Libs::Graphics;

static ShaderComputeWavePreflightRequest Request(bool next_gen = true, uint32_t mode = 0x41u)
{
	return {next_gen, mode, {2u, 3u, 1u}, {64u, 1u, 1u}, 1664u, ShaderGuestLaneOrder::LinearXFirst};
}

static ShaderComputeWaveCapabilities Capabilities()
{
	return {true, true, true, true, 8u, 32u, {1024u, 1024u, 64u}, {65535u, 65535u, 65535u}, 1024u, 128u, 49152u};
}

static ShaderComputeWaveDispatchPlan SentinelPlan()
{
	const ShaderComputeWaveLayout layout {
	    ShaderComputeWaveStrategy::Paired64On32, {11u, 12u, 13u}, {21u, 22u, 23u}, 31u, 32u, 33u, 34u, 35u};
	return {0xdeadbeefu, {7u, 8u, 9u}, layout};
}

static void ExpectUnchanged(const ShaderComputeWaveDispatchPlan& actual, const ShaderComputeWaveDispatchPlan& expected)
{
	EXPECT_EQ(actual.dispatch_mode, expected.dispatch_mode);
	for (uint32_t i = 0; i < 3u; ++i)
	{
		EXPECT_EQ(actual.group_count[i], expected.group_count[i]);
		EXPECT_EQ(actual.wave_layout.guest_local[i], expected.wave_layout.guest_local[i]);
		EXPECT_EQ(actual.wave_layout.physical_local[i], expected.wave_layout.physical_local[i]);
	}
	EXPECT_EQ(actual.wave_layout.strategy, expected.wave_layout.strategy);
	EXPECT_EQ(actual.wave_layout.guest_wave_size, expected.wave_layout.guest_wave_size);
	EXPECT_EQ(actual.wave_layout.native_subgroup_size, expected.wave_layout.native_subgroup_size);
	EXPECT_EQ(actual.wave_layout.banks, expected.wave_layout.banks);
	EXPECT_EQ(actual.wave_layout.waves, expected.wave_layout.waves);
	EXPECT_EQ(actual.wave_layout.lds_dwords, expected.wave_layout.lds_dwords);
}

static void ExpectRejected(const ShaderComputeWavePreflightRequest& request, const ShaderComputeWaveCapabilities& capabilities,
                           ShaderComputeWavePreflightReason reason)
{
	auto       plan   = SentinelPlan();
	const auto before = plan;
	const auto result = ShaderBuildComputeWaveDispatchPlan(request, capabilities, &plan);
	EXPECT_EQ(result.status, ShaderComputeWavePreflightStatus::Rejected);
	EXPECT_EQ(result.reason, reason);
	ExpectUnchanged(plan, before);
}

TEST(EmulatorComputeWaveRuntime, BuildsPaired64AndNative32LayoutsWithoutChangingNoWorkOutput)
{
	for (const auto& shape: std::array<std::array<uint32_t, 2>, 2> {{{64u, 32u}, {256u, 128u}}})
	{
		auto request          = Request();
		request.local_size[0] = shape[0];
		ShaderComputeWaveDispatchPlan plan {};
		const auto                    result = ShaderBuildComputeWaveDispatchPlan(request, Capabilities(), &plan);
		ASSERT_EQ(result.status, ShaderComputeWavePreflightStatus::Supported);
		EXPECT_EQ(plan.wave_layout.strategy, ShaderComputeWaveStrategy::Paired64On32);
		EXPECT_EQ(plan.wave_layout.physical_local[0], shape[1]);
		EXPECT_EQ(plan.group_count[0], 2u);
	}

	auto request                      = Request(true, 0x8041u);
	request.local_size[0]             = 16u;
	request.local_size[1]             = 8u;
	auto capabilities                 = Capabilities();
	capabilities.size_control_enabled = capabilities.full_subgroups_enabled = false;
	capabilities.compute_required_size_supported = capabilities.compute_ballot_shuffle_supported = false;
	ShaderComputeWaveDispatchPlan plan {};
	ASSERT_EQ(ShaderBuildComputeWaveDispatchPlan(request, capabilities, &plan).status, ShaderComputeWavePreflightStatus::Supported);
	EXPECT_EQ(plan.wave_layout.strategy, ShaderComputeWaveStrategy::Native);
	EXPECT_EQ(plan.wave_layout.guest_wave_size, 32u);
	EXPECT_EQ(plan.wave_layout.guest_local[0], 16u);
	EXPECT_EQ(plan.wave_layout.physical_local[0], 16u);

	request                       = Request(true, 0x43u);
	request.raw_dispatch_count[1] = 0u;
	request.local_size[0]         = 0u;
	plan                          = SentinelPlan();
	const auto before             = plan;
	const auto no_work            = ShaderBuildComputeWaveDispatchPlan(request, Capabilities(), &plan);
	EXPECT_EQ(no_work.status, ShaderComputeWavePreflightStatus::NoWork);
	ExpectUnchanged(plan, before);
}

TEST(EmulatorComputeWaveRuntime, RejectsUnsupportedGen5ModesBeforeLaneOrCountConversion)
{
	struct ModeCase
	{
		uint32_t             mode;
		ShaderGuestLaneOrder lane_order;
	};
	// Unknown bits, a missing COMPUTE_SHADER_EN and deferred partial-threadgroup
	// control are refused before the lane order or the (zero) local size is read,
	// for both the wave64 and the wave32 initiator.
	for (const auto& item: std::array<ModeCase, 8> {{{0x101u, ShaderGuestLaneOrder::LinearXFirst},
	                                                 {0x141u, ShaderGuestLaneOrder::Unverified},
	                                                 {0x40u, ShaderGuestLaneOrder::LinearXFirst},
	                                                 {0x43u, ShaderGuestLaneOrder::Unverified},
	                                                 {0x8101u, ShaderGuestLaneOrder::LinearXFirst},
	                                                 {0x8141u, ShaderGuestLaneOrder::Unverified},
	                                                 {0x8040u, ShaderGuestLaneOrder::LinearXFirst},
	                                                 {0x8003u, ShaderGuestLaneOrder::Unverified}}})
	{
		auto request          = Request(true, item.mode);
		request.local_size[0] = 0u;
		request.lane_order    = item.lane_order;
		ExpectRejected(request, Capabilities(), ShaderComputeWavePreflightReason::UnsupportedDispatchMode);
	}
}

// Since 704f4ad3 Gen5 USE_THREAD_DIMENSIONS is admitted: the raw counts are
// threads, converted with a checked ceiling division by the local size, so a
// zero local size is an invalid size rather than an unsupported mode.
TEST(EmulatorComputeWaveRuntime, ThreadDimensionsRejectAZeroLocalSizeInsteadOfDividing)
{
	for (const uint32_t mode: std::array<uint32_t, 2> {0x61u, 0x8021u})
	{
		auto request          = Request(true, mode);
		request.local_size[0] = 0u;
		ExpectRejected(request, Capabilities(), ShaderComputeWavePreflightReason::InvalidLocalSize);
	}
}

TEST(EmulatorComputeWaveRuntime, Gen5ThreadDimensionsLaunchCeilGroupsAndKeepTheThreadLimits)
{
	auto request                  = Request(true, 0x61u);
	request.raw_dispatch_count[0] = 100u;
	request.raw_dispatch_count[1] = 3u;
	request.raw_dispatch_count[2] = 1u;
	request.local_size[0]         = 64u;

	ShaderComputeWaveDispatchPlan plan {};
	const auto                    result = ShaderBuildComputeWaveDispatchPlan(request, Capabilities(), &plan);
	ASSERT_EQ(result.status, ShaderComputeWavePreflightStatus::Supported);
	EXPECT_EQ(plan.group_count[0], 2u);
	EXPECT_EQ(plan.group_count[1], 3u);
	EXPECT_EQ(plan.group_count[2], 1u);
	EXPECT_TRUE(plan.thread_limits_used);
	EXPECT_EQ(plan.thread_limits[0], 100u);
	EXPECT_EQ(plan.thread_limits[1], 3u);
	EXPECT_EQ(plan.thread_limits[2], 1u);
	EXPECT_EQ(plan.dispatch_mode & 0x20u, 0u);
}

// Without the paired-lane host features the plan is not refused outright: the
// per-lane route over the original guest workgroup remains, and admission must
// then prove the program wave-width independent (native_equivalence_required).
TEST(EmulatorComputeWaveRuntime, MissingPairedCapabilityFallsBackToTheProvenPerLaneRoute)
{
	auto request                      = Request();
	auto capabilities                 = Capabilities();
	capabilities.size_control_enabled = false;

	ShaderComputeWaveDispatchPlan plan {};
	const auto                    result = ShaderBuildComputeWaveDispatchPlan(request, capabilities, &plan);
	ASSERT_EQ(result.status, ShaderComputeWavePreflightStatus::Supported);
	EXPECT_EQ(result.reason, ShaderComputeWavePreflightReason::None);
	EXPECT_TRUE(plan.native_equivalence_required);
	EXPECT_TRUE(plan.native_equivalent_valid);
	EXPECT_EQ(plan.wave_layout.strategy, ShaderComputeWaveStrategy::Native);
	EXPECT_EQ(plan.wave_layout.guest_wave_size, 64u);
	EXPECT_EQ(plan.wave_layout.banks, 1u);
	EXPECT_EQ(plan.wave_layout.guest_local[0], 64u);
	EXPECT_EQ(plan.wave_layout.physical_local[0], 64u);
	EXPECT_EQ(plan.wave_layout.lds_dwords, 1664u);
	EXPECT_EQ(plan.group_count[0], 2u);
}

TEST(EmulatorComputeWaveRuntime, UnverifiedLaneOrderNeedsThePerLaneRouteToSurviveAPairedRejection)
{
	auto request       = Request();
	request.lane_order = ShaderGuestLaneOrder::Unverified;

	ShaderComputeWaveDispatchPlan plan {};
	const auto                    result = ShaderBuildComputeWaveDispatchPlan(request, Capabilities(), &plan);
	ASSERT_EQ(result.status, ShaderComputeWavePreflightStatus::Supported);
	EXPECT_TRUE(plan.native_equivalence_required);
	EXPECT_EQ(plan.wave_layout.strategy, ShaderComputeWaveStrategy::Native);
}

TEST(EmulatorComputeWaveRuntime, RejectsMissingPairedCapabilityAndUnverifiedLaneOrderWhenNoPerLaneRouteExists)
{
	// A local size beyond the host limits leaves neither route, so the paired
	// rejection reason is what the caller sees.
	auto capabilities              = Capabilities();
	capabilities.max_local_size[0] = 32u;

	auto request                      = Request();
	capabilities.size_control_enabled = false;
	ExpectRejected(request, capabilities, ShaderComputeWavePreflightReason::MissingHostCapability);

	capabilities.size_control_enabled = true;
	request.lane_order                = ShaderGuestLaneOrder::Unverified;
	ExpectRejected(request, capabilities, ShaderComputeWavePreflightReason::UnverifiedLaneOrder);
}

TEST(EmulatorComputeWaveRuntime, RejectsGen5Wave64WhenTgSizeSystemSgprIsEnabled)
{
	auto request       = Request();
	request.tg_size_en = true;
	ExpectRejected(request, Capabilities(), ShaderComputeWavePreflightReason::UnsupportedSystemSgpr);
}

TEST(EmulatorComputeWaveRuntime, TgSizeFlagDoesNotChangeNativeWave32Admission)
{
	auto request                      = Request(true, 0x8041u);
	request.tg_size_en                = true;
	request.local_size[0]             = 16u;
	request.local_size[1]             = 8u;
	auto capabilities                 = Capabilities();
	capabilities.size_control_enabled = capabilities.full_subgroups_enabled = false;
	capabilities.compute_required_size_supported = capabilities.compute_ballot_shuffle_supported = false;
	ShaderComputeWaveDispatchPlan plan {};
	EXPECT_EQ(ShaderBuildComputeWaveDispatchPlan(request, capabilities, &plan).status, ShaderComputeWavePreflightStatus::Supported);
	EXPECT_EQ(plan.wave_layout.strategy, ShaderComputeWaveStrategy::Native);
}

TEST(EmulatorComputeWaveRuntime, LegacyThreadCountsUseCheckedCeilAndRejectZeroOrOverLimit)
{
	auto request                      = Request(false, 0x8021u);
	request.raw_dispatch_count[0]     = 257u;
	request.raw_dispatch_count[1]     = 17u;
	request.local_size[0]             = 16u;
	request.local_size[1]             = 8u;
	auto capabilities                 = Capabilities();
	capabilities.size_control_enabled = capabilities.full_subgroups_enabled = false;
	capabilities.compute_required_size_supported = capabilities.compute_ballot_shuffle_supported = false;
	ShaderComputeWaveDispatchPlan plan {};
	ASSERT_EQ(ShaderBuildComputeWaveDispatchPlan(request, capabilities, &plan).status, ShaderComputeWavePreflightStatus::Supported);
	EXPECT_EQ(plan.dispatch_mode, 0x8021u);
	EXPECT_EQ(plan.group_count[0], 17u);
	EXPECT_EQ(plan.group_count[1], 3u);
	EXPECT_EQ(plan.group_count[2], 1u);

	request.local_size[2] = 0u;
	ExpectRejected(request, capabilities, ShaderComputeWavePreflightReason::InvalidLocalSize);
	request.local_size[2]           = 1u;
	request.raw_dispatch_count[0]   = std::numeric_limits<uint32_t>::max();
	request.raw_dispatch_count[1]   = 1u;
	request.local_size[0]           = 2u;
	capabilities.max_group_count[0] = 0x7fffffffu;
	ExpectRejected(request, capabilities, ShaderComputeWavePreflightReason::HostLimitExceeded);
}

TEST(EmulatorComputeWaveRuntime, NativeLayoutChecksLocalAndLdsLimitsForGen5AndLegacy)
{
	for (const bool next_gen: std::array<bool, 2> {false, true})
	{
		auto request                      = Request(next_gen, next_gen ? 0x8041u : 0x01u);
		request.local_size[0]             = 16u;
		request.local_size[1]             = 8u;
		auto capabilities                 = Capabilities();
		capabilities.size_control_enabled = capabilities.full_subgroups_enabled = false;
		capabilities.compute_required_size_supported = capabilities.compute_ballot_shuffle_supported = false;
		capabilities.max_local_size[0]                                                               = 15u;
		ExpectRejected(request, capabilities, ShaderComputeWavePreflightReason::HostLimitExceeded);

		capabilities                      = Capabilities();
		capabilities.size_control_enabled = capabilities.full_subgroups_enabled = false;
		capabilities.compute_required_size_supported = capabilities.compute_ballot_shuffle_supported = false;
		capabilities.max_invocations                                                                 = 127u;
		ExpectRejected(request, capabilities, ShaderComputeWavePreflightReason::HostLimitExceeded);

		capabilities                      = Capabilities();
		capabilities.size_control_enabled = capabilities.full_subgroups_enabled = false;
		capabilities.compute_required_size_supported = capabilities.compute_ballot_shuffle_supported = false;
		capabilities.max_shared_bytes                                                                = 6655u;
		ExpectRejected(request, capabilities, ShaderComputeWavePreflightReason::HostLimitExceeded);
	}
}

TEST(EmulatorComputeWaveRuntime, RejectsOverflowingNativeLocalProductWithoutMutatingOutput)
{
	for (const bool next_gen: std::array<bool, 2> {false, true})
	{
		auto request = Request(next_gen, next_gen ? 0x8041u : 0x01u);
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			request.local_size[axis] = std::numeric_limits<uint32_t>::max();
		}
		auto capabilities = Capabilities();
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			capabilities.max_local_size[axis] = std::numeric_limits<uint32_t>::max();
		}
		capabilities.max_invocations  = std::numeric_limits<uint32_t>::max();
		capabilities.max_shared_bytes = std::numeric_limits<uint32_t>::max();
		ExpectRejected(request, capabilities, ShaderComputeWavePreflightReason::HostLimitExceeded);
	}
}

UT_END();
