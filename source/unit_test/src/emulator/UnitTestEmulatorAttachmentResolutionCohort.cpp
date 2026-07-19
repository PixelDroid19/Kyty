#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/AttachmentResolutionCohort.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Shader.h"

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

TEST(EmulatorAttachmentResolutionCohort, AllowsFragmentCoordinatesOnlyWhenTranslationSupportsScaling)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const auto color = Color({3840, 2160});

	ResolutionShaderCoordinateUsage fragment_unsupported;
	fragment_unsupported.fragment_coordinates = true;
	EXPECT_EQ(EvaluateResolutionCohort(policy, {&color, 1, 1, fragment_unsupported}).reason,
	          ResolutionCohortReason::ShaderCoordinateAccess);

	auto fragment_supported                           = fragment_unsupported;
	fragment_supported.fragment_coordinates_supported = true;
	const auto supported_decision                     = EvaluateResolutionCohort(policy, {&color, 1, 1, fragment_supported});
	EXPECT_EQ(supported_decision.classification, ResolutionClassification::Scaled);
	EXPECT_EQ(supported_decision.reason, ResolutionCohortReason::None);
}

TEST(EmulatorAttachmentResolutionCohort, KeepsIntegerCoordinatesAndImageSizeBlockedWithFragmentSupport)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const auto color = Color({3840, 2160});

	ResolutionShaderCoordinateUsage integer_coordinates;
	integer_coordinates.fragment_coordinates_supported = true;
	integer_coordinates.integer_image_coordinates      = true;
	EXPECT_EQ(EvaluateResolutionCohort(policy, {&color, 1, 1, integer_coordinates}).reason, ResolutionCohortReason::ShaderCoordinateAccess);

	ResolutionShaderCoordinateUsage image_size;
	image_size.fragment_coordinates_supported = true;
	image_size.image_size_query               = true;
	EXPECT_EQ(EvaluateResolutionCohort(policy, {&color, 1, 1, image_size}).reason, ResolutionCohortReason::ShaderCoordinateAccess);
}

TEST(EmulatorAttachmentResolutionCohort, ShaderAnalysisAllowsSamplingAndRejectsIntegerImageAccess)
{
	ShaderCode sampled;
	sampled.GetInstructions().Add({.type = ShaderInstructionType::ImageSample});
	const auto sampled_usage = AnalyzeResolutionShaderUsage(sampled);
	EXPECT_FALSE(sampled_usage.fragment_coordinates);
	EXPECT_FALSE(sampled_usage.fragment_coordinates_supported);
	EXPECT_FALSE(sampled_usage.integer_image_coordinates);
	EXPECT_FALSE(sampled_usage.image_size_query);

	ShaderCode loaded;
	loaded.GetInstructions().Add({.type = ShaderInstructionType::ImageLoad});
	EXPECT_TRUE(AnalyzeResolutionShaderUsage(loaded).integer_image_coordinates);

	ShaderCode stored;
	stored.GetInstructions().Add({.type = ShaderInstructionType::ImageStore});
	EXPECT_TRUE(AnalyzeResolutionShaderUsage(stored).integer_image_coordinates);
}

TEST(EmulatorAttachmentResolutionCohort, VideoOutHostExtentSelectionIsStickyBeforeMaterialization)
{
	VideoOutVulkanImage image;
	image.SetNativeExtent(3840, 2160);

	VideoOutHostExtentState state;
	EXPECT_EQ(VideoOutBufferSelectHostExtent(&image, 1280, 720, &state), VideoOutHostExtentStatus::Selected);
	EXPECT_EQ(state.width, 1280u);
	EXPECT_EQ(state.height, 720u);
	EXPECT_TRUE(state.selected);
	EXPECT_FALSE(state.materialized);

	EXPECT_EQ(VideoOutBufferSelectHostExtent(&image, 1920, 1080, &state), VideoOutHostExtentStatus::StickyMismatch);
	EXPECT_EQ(state.width, 1280u);
	EXPECT_EQ(state.height, 720u);
	EXPECT_EQ(image.GetGuestExtent().width, 3840u);
	EXPECT_EQ(image.GetGuestExtent().height, 2160u);
	EXPECT_EQ(image.GetHostExtent().width, 1280u);
	EXPECT_EQ(image.GetHostExtent().height, 720u);

	EXPECT_TRUE(VideoOutBufferGetHostExtentState(&image, &state));
	EXPECT_EQ(state.width, 1280u);
	EXPECT_EQ(state.height, 720u);
}

TEST(EmulatorAttachmentResolutionCohort, VideoOutMaterializedExtentCannotBeChangedWhenSelectionFlagIsInconsistent)
{
	VideoOutVulkanImage image;
	image.SetNativeExtent(3840, 2160);
	image.host_extent_selected = false;
	image.image                = reinterpret_cast<VkImage>(uintptr_t {1});

	VideoOutHostExtentState state {};
	EXPECT_EQ(VideoOutBufferSelectHostExtent(&image, 1280, 720, &state), VideoOutHostExtentStatus::StickyMismatch);
	EXPECT_TRUE(state.selected);
	EXPECT_TRUE(state.materialized);
	EXPECT_EQ(state.width, 3840u);
	EXPECT_EQ(state.height, 2160u);
	EXPECT_EQ(image.extent.width, 3840u);
	EXPECT_EQ(image.extent.height, 2160u);
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

TEST(EmulatorAttachmentResolutionCohort, ScaledVulkanImageMatchesItsGuestDescriptorExtent)
{
	RenderTextureVulkanImage image;
	image.SetNativeExtent(3840, 2160);
	image.SetHostExtent(1280, 720);

	EXPECT_TRUE(image.MatchesGuestExtent(3840, 2160));
	EXPECT_FALSE(image.MatchesGuestExtent(1280, 720));
}

TEST(EmulatorAttachmentResolutionCohort, ResolutionIdentityIncludesTheHostExtent)
{
	InternalResolutionPolicy hd({1280, 720});
	InternalResolutionPolicy full_hd({1920, 1080});
	ASSERT_EQ(hd.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	ASSERT_EQ(full_hd.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const auto hd_decision      = hd.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});
	const auto full_hd_decision = full_hd.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});

	EXPECT_EQ(hd_decision.identity.guest_resource_extent, full_hd_decision.identity.guest_resource_extent);
	EXPECT_NE(hd_decision.identity.host_resource_extent, full_hd_decision.identity.host_resource_extent);
	EXPECT_NE(hd_decision.identity, full_hd_decision.identity);
}

TEST(EmulatorAttachmentResolutionCohort, NativeDepthStencilIdentityUsesGuestExtentForHostExtent)
{
	const DepthStencilBufferObject native(VK_FORMAT_D32_SFLOAT, 3840, 2160, true, false, true, 0x1000, 0x2000);

	EXPECT_EQ(native.params[DepthStencilBufferObject::PARAM_GUEST_WIDTH], 3840u);
	EXPECT_EQ(native.params[DepthStencilBufferObject::PARAM_GUEST_HEIGHT], 2160u);
	EXPECT_EQ(native.params[DepthStencilBufferObject::PARAM_HOST_WIDTH], 3840u);
	EXPECT_EQ(native.params[DepthStencilBufferObject::PARAM_HOST_HEIGHT], 2160u);
}

TEST(EmulatorAttachmentResolutionCohort, ScaledDepthStencilIdentityDistinguishesHostExtent)
{
	const DepthStencilBufferObject native(VK_FORMAT_D32_SFLOAT, 3840, 2160, true, false, true, 0x1000, 0x2000);
	const DepthStencilBufferObject scaled_720p(VK_FORMAT_D32_SFLOAT, 3840, 2160, 1280, 720, true, false, true, 0x1000,
	                                           0x2000);
	const DepthStencilBufferObject same_scaled_720p(VK_FORMAT_D32_SFLOAT, 3840, 2160, 1280, 720, true, false, true, 0x1000,
	                                                0x2000);
	const DepthStencilBufferObject scaled_1080p(VK_FORMAT_D32_SFLOAT, 3840, 2160, 1920, 1080, true, false, true, 0x1000,
	                                            0x2000);

	EXPECT_FALSE(native.Equal(scaled_720p.params));
	EXPECT_TRUE(scaled_720p.Equal(same_scaled_720p.params));
	EXPECT_FALSE(scaled_720p.Equal(scaled_1080p.params));
}

UT_END();
