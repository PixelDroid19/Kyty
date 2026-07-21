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

TEST(EmulatorInternalResolutionRuntime, MarksScalingAppliedOnlyForTheRegisteredPhysicalCohort)
{
	InternalResolutionRuntime runtime({1280, 720});
	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	ResolutionCohortDecision native;
	native.classification = ResolutionClassification::Native;
	EXPECT_FALSE(runtime.MarkScalingApplied(native));
	EXPECT_FALSE(runtime.GetSnapshot().scaling_applied);

	ResolutionCohortDecision scaled;
	scaled.classification = ResolutionClassification::Scaled;
	scaled.guest_extent   = {3840, 2160};
	scaled.host_extent    = {1280, 720};
	EXPECT_TRUE(runtime.MarkScalingApplied(scaled));
	EXPECT_TRUE(runtime.GetSnapshot().scaling_applied);
}

TEST(EmulatorInternalResolutionRuntime, FirstDisplayDecisionIsStickyAcrossTheBufferSet)
{
	InternalResolutionRuntime runtime({1280, 720});
	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	ResolutionExtent         selected;
	ResolutionCohortDecision authorized;
	authorized.classification = ResolutionClassification::Scaled;
	authorized.guest_extent   = {3840, 2160};
	authorized.host_extent    = {1280, 720};
	EXPECT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {1280, 720}, &authorized, &selected),
	          InternalResolutionDisplaySelectionStatus::Selected);
	EXPECT_EQ(selected, (ResolutionExtent {1280, 720}));
	EXPECT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {3840, 2160}, nullptr, &selected),
	          InternalResolutionDisplaySelectionStatus::StickyMismatch);
	EXPECT_EQ(selected, (ResolutionExtent {1280, 720}));
}

TEST(EmulatorInternalResolutionRuntime, FirstUnsafeDrawCanFreezeTheBufferSetAtNativeExtent)
{
	InternalResolutionRuntime runtime({1280, 720});
	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	ResolutionExtent selected;
	EXPECT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {3840, 2160}, nullptr, &selected),
	          InternalResolutionDisplaySelectionStatus::Selected);
	ResolutionCohortDecision authorized;
	authorized.classification = ResolutionClassification::Scaled;
	authorized.guest_extent   = {3840, 2160};
	authorized.host_extent    = {1280, 720};
	EXPECT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {1280, 720}, &authorized, &selected),
	          InternalResolutionDisplaySelectionStatus::StickyMismatch);
	EXPECT_EQ(selected, (ResolutionExtent {3840, 2160}));
}

TEST(EmulatorInternalResolutionRuntime, RejectsScaledExtentWithoutAnExactCohortAuthorization)
{
	InternalResolutionRuntime runtime({1280, 720});
	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	ResolutionExtent selected;
	EXPECT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {1280, 720}, nullptr, &selected),
	          InternalResolutionDisplaySelectionStatus::UnauthorizedExtent);
}

TEST(EmulatorInternalResolutionRuntime, AdditionalBufferSetsKeepTheExistingStickyDecision)
{
	InternalResolutionRuntime runtime({1280, 720});
	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	ResolutionExtent selected;
	ASSERT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {3840, 2160}, nullptr, &selected),
	          InternalResolutionDisplaySelectionStatus::Selected);

	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	ResolutionCohortDecision authorized;
	authorized.classification = ResolutionClassification::Scaled;
	authorized.guest_extent   = {3840, 2160};
	authorized.host_extent    = {1280, 720};
	EXPECT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {1280, 720}, &authorized, &selected),
	          InternalResolutionDisplaySelectionStatus::StickyMismatch);
	EXPECT_EQ(selected, (ResolutionExtent {3840, 2160}));
}

TEST(EmulatorInternalResolutionRuntime, ChangedGuestDisplayStartsANewStickySelectionEpoch)
{
	InternalResolutionRuntime runtime({1280, 720});
	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	ResolutionExtent selected;
	ASSERT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {3840, 2160}, nullptr, &selected),
	          InternalResolutionDisplaySelectionStatus::Selected);

	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({1920, 1080}), ResolutionPolicyStatus::Success);
	EXPECT_EQ(runtime.SelectDisplayHostExtent({1920, 1080}, {1920, 1080}, nullptr, &selected),
	          InternalResolutionDisplaySelectionStatus::Selected);
	EXPECT_EQ(selected, (ResolutionExtent {1920, 1080}));
}

TEST(EmulatorInternalResolutionRuntime, DepthOnlyPrepassCanFreezeDisplayAtNativeBeforeColor)
{
	InternalResolutionRuntime runtime({1280, 720});
	ASSERT_EQ(runtime.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	ResolutionExtent selected;
	EXPECT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {3840, 2160}, nullptr, &selected),
	          InternalResolutionDisplaySelectionStatus::Selected);

	ResolutionCohortDecision later_scaled;
	later_scaled.classification = ResolutionClassification::Scaled;
	later_scaled.guest_extent   = {3840, 2160};
	later_scaled.host_extent    = {1280, 720};
	EXPECT_EQ(runtime.SelectDisplayHostExtent({3840, 2160}, {1280, 720}, &later_scaled, &selected),
	          InternalResolutionDisplaySelectionStatus::StickyMismatch);
	EXPECT_EQ(selected, (ResolutionExtent {3840, 2160}));
}

UT_END();
