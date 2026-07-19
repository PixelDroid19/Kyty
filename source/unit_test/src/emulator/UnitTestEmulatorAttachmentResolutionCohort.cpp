#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/AttachmentResolutionCohort.h"
#include "Emulator/Graphics/GraphicContext.h"

UT_BEGIN(EmulatorAttachmentResolutionCohort);

using namespace Libs::Graphics;

namespace {

ResolutionAttachmentCandidate Color(ResolutionExtent extent)
{
	return {extent, {ResolutionResourceKind::ColorAttachment, false}};
}

ResolutionAttachmentCandidate Depth(ResolutionExtent extent)
{
	return {extent, {ResolutionResourceKind::DepthStencilAttachment, false}};
}

} // namespace

TEST(EmulatorAttachmentResolutionCohort, ScalesOnlyACompleteColorDepthCohort)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const ResolutionAttachmentCandidate attachments[] = {Color({3840, 2160}), Depth({3840, 2160})};
	const ResolutionCohortInput         input {attachments, 2, 2, {}};

	const auto decision = EvaluateResolutionCohort(policy, input);

	EXPECT_EQ(decision.classification, ResolutionClassification::Scaled);
	EXPECT_EQ(decision.reason, ResolutionCohortReason::None);
	EXPECT_EQ(decision.guest_extent, (ResolutionExtent {3840, 2160}));
	EXPECT_EQ(decision.host_extent, (ResolutionExtent {1280, 720}));
	EXPECT_EQ(decision.scale, (ResolutionScale {1, 3}));
	EXPECT_EQ(decision.attachment_count, 2u);
}

TEST(EmulatorAttachmentResolutionCohort, RejectsAnIncompleteAttachmentSet)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const auto color    = Color({3840, 2160});
	const auto decision = EvaluateResolutionCohort(policy, {&color, 1, 2, {}});

	EXPECT_EQ(decision.classification, ResolutionClassification::Native);
	EXPECT_EQ(decision.reason, ResolutionCohortReason::Incomplete);
}

TEST(EmulatorAttachmentResolutionCohort, RejectsMismatchedAttachmentExtents)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const ResolutionAttachmentCandidate attachments[] = {Color({3840, 2160}), Depth({1920, 1080})};
	const auto                          decision      = EvaluateResolutionCohort(policy, {attachments, 2, 2, {}});

	EXPECT_EQ(decision.classification, ResolutionClassification::Native);
	EXPECT_EQ(decision.reason, ResolutionCohortReason::MismatchedGuestExtent);
}

TEST(EmulatorAttachmentResolutionCohort, RejectsAnyUnsafeAttachment)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	auto color                                        = Color({3840, 2160});
	color.resource.cpu_transfer                       = true;
	const ResolutionAttachmentCandidate attachments[] = {color, Depth({3840, 2160})};
	const auto                          decision      = EvaluateResolutionCohort(policy, {attachments, 2, 2, {}});

	EXPECT_EQ(decision.classification, ResolutionClassification::Native);
	EXPECT_EQ(decision.reason, ResolutionCohortReason::AttachmentNotScalable);
	EXPECT_EQ(decision.attachment_native_reason, ResolutionNativeReason::CpuTransfer);
	EXPECT_EQ(decision.guest_extent, (ResolutionExtent {3840, 2160}));
	EXPECT_EQ(decision.host_extent, decision.guest_extent);
	EXPECT_EQ(decision.scale, (ResolutionScale {1, 1}));
}

TEST(EmulatorAttachmentResolutionCohort, RejectsShaderCoordinatesUntilTranslationSupportsScaling)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const auto color = Color({3840, 2160});

	for (const auto usage: {ResolutionShaderCoordinateUsage {true, false, false}, ResolutionShaderCoordinateUsage {false, true, false},
	                        ResolutionShaderCoordinateUsage {false, false, true}})
	{
		const auto decision = EvaluateResolutionCohort(policy, {&color, 1, 1, usage});
		EXPECT_EQ(decision.classification, ResolutionClassification::Native);
		EXPECT_EQ(decision.reason, ResolutionCohortReason::ShaderCoordinateAccess);
	}
}

TEST(EmulatorAttachmentResolutionCohort, RejectsEmptyAndInvalidInputsStructurally)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	EXPECT_EQ(EvaluateResolutionCohort(policy, {}).reason, ResolutionCohortReason::Empty);
	EXPECT_EQ(EvaluateResolutionCohort(policy, {nullptr, 1, 1, {}}).reason, ResolutionCohortReason::InvalidInput);
}

TEST(EmulatorAttachmentResolutionCohort, VulkanImagesKeepGuestAndHostExtentsSeparate)
{
	RenderTextureVulkanImage image;
	image.SetNativeExtent(3840, 2160);

	EXPECT_EQ(image.guest_extent.width, 3840u);
	EXPECT_EQ(image.guest_extent.height, 2160u);
	EXPECT_EQ(image.GetHostExtent().width, 3840u);
	EXPECT_EQ(image.GetHostExtent().height, 2160u);
	EXPECT_FALSE(image.IsResolutionScaled());

	image.SetHostExtent(1280, 720);

	EXPECT_EQ(image.guest_extent.width, 3840u);
	EXPECT_EQ(image.guest_extent.height, 2160u);
	EXPECT_EQ(image.GetHostExtent().width, 1280u);
	EXPECT_EQ(image.GetHostExtent().height, 720u);
	EXPECT_TRUE(image.IsResolutionScaled());
}

UT_END();
