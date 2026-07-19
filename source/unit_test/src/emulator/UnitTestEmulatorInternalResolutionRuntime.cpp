#include "Kyty/UnitTest.h"

#include "Emulator/Agent/Protocol.h"
#include "Emulator/Graphics/InternalResolutionRuntime.h"

#include <string>

UT_BEGIN(EmulatorInternalResolutionRuntime);

using namespace Emulator::Agent;
using namespace Libs::Graphics;

TEST(EmulatorInternalResolutionRuntime, OwnsConfiguredTargetBeforeGuestRegistration)
{
	InternalResolutionRuntime runtime({1600, 900});

	const auto snapshot = runtime.GetSnapshot();

	EXPECT_EQ(snapshot.target_extent, (ResolutionExtent {1600, 900}));
	EXPECT_FALSE(snapshot.guest_registered);
	EXPECT_FALSE(snapshot.scaling_applied);
	EXPECT_EQ(snapshot.candidate_decision.classification, ResolutionClassification::Unsupported);
}

TEST(EmulatorInternalResolutionRuntime, RecordsCandidateDecisionWithoutApplyingScaling)
{
	InternalResolutionRuntime runtime({1280, 720});

	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	const auto snapshot = runtime.GetSnapshot();

	EXPECT_TRUE(snapshot.guest_registered);
	EXPECT_EQ(snapshot.guest_display_extent, (ResolutionExtent {3840, 2160}));
	EXPECT_EQ(snapshot.candidate_decision.classification, ResolutionClassification::Scaled);
	EXPECT_EQ(snapshot.candidate_decision.host_extent, (ResolutionExtent {1280, 720}));
	EXPECT_EQ(snapshot.candidate_decision.scale, (ResolutionScale {1, 3}));
	EXPECT_FALSE(snapshot.scaling_applied);
}

TEST(EmulatorInternalResolutionRuntime, InvalidGuestExtentDoesNotReplacePriorEvidence)
{
	InternalResolutionRuntime runtime({1280, 720});
	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({1920, 1080}), ResolutionPolicyStatus::Success);

	EXPECT_EQ(runtime.RegisterGuestDisplayExtent({0, 1080}), ResolutionPolicyStatus::InvalidExtent);

	const auto snapshot = runtime.GetSnapshot();
	EXPECT_EQ(snapshot.guest_display_extent, (ResolutionExtent {1920, 1080}));
	EXPECT_EQ(snapshot.candidate_decision.host_extent, (ResolutionExtent {1280, 720}));
}

TEST(EmulatorInternalResolutionRuntime, TelemetryUsesBoundedStableFieldsAndReportsNotApplied)
{
	InternalResolutionRuntime runtime({1280, 720});
	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	std::string json;
	AppendInternalResolutionPerformanceJson(runtime.GetSnapshot(), &json);

	EXPECT_EQ(
	    json,
	    R"("internal_resolution":{"target":{"width":1280,"height":720},"guest_display":{"registered":true,"width":3840,"height":2160},"candidate_host":{"width":1280,"height":720},"scale":{"numerator":1,"denominator":3},"classification":"scaled","native_reason":"none","scaling_applied":false})");
}

UT_END();
