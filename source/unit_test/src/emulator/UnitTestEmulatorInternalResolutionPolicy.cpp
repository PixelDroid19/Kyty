#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/InternalResolutionPolicy.h"

#include <cstdint>
#include <limits>

UT_BEGIN(EmulatorInternalResolutionPolicy);

using namespace Libs::Graphics;

TEST(EmulatorInternalResolutionPolicy, Uses720pAsTheDefaultTarget)
{
	InternalResolutionPolicy policy;

	EXPECT_EQ(policy.GetTargetExtent(), (ResolutionExtent {1280, 720}));
}

TEST(EmulatorInternalResolutionPolicy, Scales4kAttachmentsTo720p)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const auto decision = policy.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});

	EXPECT_EQ(decision.classification, ResolutionClassification::Scaled);
	EXPECT_EQ(decision.host_extent, (ResolutionExtent {1280, 720}));
	EXPECT_EQ(decision.scale, (ResolutionScale {1, 3}));
}

TEST(EmulatorInternalResolutionPolicy, SupportsCommonDownscaleTargets)
{
	InternalResolutionPolicy full_hd({1920, 1080});
	ASSERT_EQ(full_hd.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const auto full_hd_decision = full_hd.Evaluate({3840, 2160}, {ResolutionResourceKind::DepthStencilAttachment, false});

	EXPECT_EQ(full_hd_decision.classification, ResolutionClassification::Scaled);
	EXPECT_EQ(full_hd_decision.host_extent, (ResolutionExtent {1920, 1080}));
	EXPECT_EQ(full_hd_decision.scale, (ResolutionScale {1, 2}));

	InternalResolutionPolicy quad_hd({2560, 1440});
	ASSERT_EQ(quad_hd.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	const auto quad_hd_decision = quad_hd.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});
	EXPECT_EQ(quad_hd_decision.host_extent, (ResolutionExtent {2560, 1440}));
	EXPECT_EQ(quad_hd_decision.scale, (ResolutionScale {2, 3}));
}

TEST(EmulatorInternalResolutionPolicy, SupportsUpscalingFrom1080pTo4k)
{
	InternalResolutionPolicy policy({3840, 2160});
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({1920, 1080}), ResolutionPolicyStatus::Success);

	const auto decision = policy.Evaluate({1920, 1080}, {ResolutionResourceKind::ColorAttachment, false});

	EXPECT_EQ(decision.classification, ResolutionClassification::Scaled);
	EXPECT_EQ(decision.host_extent, (ResolutionExtent {3840, 2160}));
	EXPECT_EQ(decision.scale, (ResolutionScale {2, 1}));
}

TEST(EmulatorInternalResolutionPolicy, PreservesAspectRatioInsideAMismatchedTarget)
{
	InternalResolutionPolicy policy({1024, 768});
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({1920, 1080}), ResolutionPolicyStatus::Success);

	const auto decision = policy.Evaluate({1920, 1080}, {ResolutionResourceKind::ColorAttachment, false});

	EXPECT_EQ(decision.classification, ResolutionClassification::Scaled);
	EXPECT_EQ(decision.host_extent, (ResolutionExtent {1024, 576}));
	EXPECT_EQ(decision.scale, (ResolutionScale {8, 15}));
}

TEST(EmulatorInternalResolutionPolicy, RoundsOddExtentsOutwardWithoutDroppingCoverage)
{
	InternalResolutionPolicy policy({1280, 720});
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({1919, 1079}), ResolutionPolicyStatus::Success);

	const auto decision = policy.Evaluate({1919, 1079}, {ResolutionResourceKind::ColorAttachment, false});

	EXPECT_EQ(decision.host_extent, (ResolutionExtent {1280, 720}));

	ResolutionRect mapped;
	ASSERT_EQ(policy.MapRect(decision, {1, 1, 1, 1}, &mapped), ResolutionPolicyStatus::Success);
	EXPECT_EQ(mapped, (ResolutionRect {0, 0, 2, 2}));
}

TEST(EmulatorInternalResolutionPolicy, MapsRectOriginsDownAndEndsUp)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	const auto decision = policy.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});

	ResolutionRect mapped;
	ASSERT_EQ(policy.MapRect(decision, {4, 4, 5, 5}, &mapped), ResolutionPolicyStatus::Success);

	EXPECT_EQ(mapped, (ResolutionRect {1, 1, 2, 2}));

	ASSERT_EQ(policy.MapRect(decision, {0, 0, 3840, 2160}, &mapped), ResolutionPolicyStatus::Success);
	EXPECT_EQ(mapped, (ResolutionRect {0, 0, 1280, 720}));
}

TEST(EmulatorInternalResolutionPolicy, KeepsUnsafeResourceClassesNativeByDefault)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const ResolutionResourceInfo resources[] = {
	    {ResolutionResourceKind::SampledImage, false},
	    {ResolutionResourceKind::StorageImage, false},
	    {ResolutionResourceKind::Buffer, false},
	    {ResolutionResourceKind::ColorAttachment, true},
	    {ResolutionResourceKind::DepthStencilAttachment, true},
	};

	for (const auto& resource: resources)
	{
		const auto decision = policy.Evaluate({3840, 2160}, resource);
		EXPECT_EQ(decision.classification, ResolutionClassification::Native);
		EXPECT_EQ(decision.host_extent, (ResolutionExtent {3840, 2160}));
		EXPECT_NE(decision.native_reason, ResolutionNativeReason::None);
	}
}

TEST(EmulatorInternalResolutionPolicy, RequiresASimpleTwoDimensionalSingleMipAttachment)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	ResolutionResourceInfo resource {ResolutionResourceKind::ColorAttachment, false};
	resource.dimension       = ResolutionImageDimension::ThreeD;
	resource.mip_levels      = 2;
	resource.sample_count    = 4;
	resource.shader_writable = true;
	resource.cpu_transfer    = true;
	resource.ambiguous_alias = true;

	auto decision = policy.Evaluate({3840, 2160}, resource);
	EXPECT_EQ(decision.classification, ResolutionClassification::Native);
	EXPECT_EQ(decision.native_reason, ResolutionNativeReason::UnsupportedDimension);

	resource.dimension = ResolutionImageDimension::TwoD;
	decision           = policy.Evaluate({3840, 2160}, resource);
	EXPECT_EQ(decision.native_reason, ResolutionNativeReason::Mipmapped);

	resource.mip_levels = 1;
	decision            = policy.Evaluate({3840, 2160}, resource);
	EXPECT_EQ(decision.native_reason, ResolutionNativeReason::Multisampled);

	resource.sample_count = 1;
	decision              = policy.Evaluate({3840, 2160}, resource);
	EXPECT_EQ(decision.native_reason, ResolutionNativeReason::ShaderWritable);

	resource.shader_writable = false;
	decision                 = policy.Evaluate({3840, 2160}, resource);
	EXPECT_EQ(decision.native_reason, ResolutionNativeReason::CpuTransfer);

	resource.cpu_transfer = false;
	decision              = policy.Evaluate({3840, 2160}, resource);
	EXPECT_EQ(decision.native_reason, ResolutionNativeReason::AmbiguousAlias);
}

TEST(EmulatorInternalResolutionPolicy, AllowsScalingToBeDisabledWithoutChangingGuestState)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);
	policy.SetScaleMode(ResolutionScaleMode::Native);

	const auto decision = policy.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});

	EXPECT_EQ(decision.classification, ResolutionClassification::Native);
	EXPECT_EQ(decision.native_reason, ResolutionNativeReason::PolicyDisabled);
	EXPECT_EQ(decision.guest_extent, (ResolutionExtent {3840, 2160}));
	EXPECT_EQ(decision.host_extent, decision.guest_extent);
}

TEST(EmulatorInternalResolutionPolicy, ExposesStableNativeReasonNames)
{
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::None), "none");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::PolicyDisabled), "policy_disabled");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::ResourceKind), "resource_kind");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::Compressed), "compressed");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::UnsupportedDimension), "unsupported_dimension");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::Mipmapped), "mipmapped");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::Multisampled), "multisampled");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::ShaderWritable), "shader_writable");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::CpuTransfer), "cpu_transfer");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::AmbiguousAlias), "ambiguous_alias");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::IdentityScale), "identity_scale");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::InvalidExtent), "invalid_extent");
	EXPECT_STREQ(ResolutionNativeReasonName(ResolutionNativeReason::ArithmeticOverflow), "arithmetic_overflow");
}

TEST(EmulatorInternalResolutionPolicy, KeepsOneToOneAttachmentsNative)
{
	InternalResolutionPolicy policy({1920, 1080});
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({1920, 1080}), ResolutionPolicyStatus::Success);

	const auto decision = policy.Evaluate({1920, 1080}, {ResolutionResourceKind::ColorAttachment, false});

	EXPECT_EQ(decision.classification, ResolutionClassification::Native);
	EXPECT_EQ(decision.host_extent, decision.guest_extent);
}

TEST(EmulatorInternalResolutionPolicy, RejectsZeroAndOverflowingInputs)
{
	InternalResolutionPolicy invalid_target({0, 720});
	EXPECT_EQ(invalid_target.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::InvalidExtent);
	EXPECT_EQ(invalid_target.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false}).classification,
	          ResolutionClassification::Unsupported);

	InternalResolutionPolicy policy;
	EXPECT_EQ(policy.RegisterGuestDisplayExtent({3840, 0}), ResolutionPolicyStatus::InvalidExtent);
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const auto     decision = policy.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});
	ResolutionRect mapped;
	EXPECT_EQ(policy.MapRect(decision, {std::numeric_limits<uint32_t>::max(), 0, 2, 1}, &mapped),
	          ResolutionPolicyStatus::ArithmeticOverflow);
}

TEST(EmulatorInternalResolutionPolicy, RejectsHostExtentsThatDoNotFitThePublicType)
{
	InternalResolutionPolicy policy({std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()});
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({1, 1}), ResolutionPolicyStatus::Success);

	const auto decision = policy.Evaluate({std::numeric_limits<uint32_t>::max(), 2}, {ResolutionResourceKind::ColorAttachment, false});

	EXPECT_EQ(decision.classification, ResolutionClassification::Unsupported);
}

TEST(EmulatorInternalResolutionPolicy, ProducesStableAndScaleSensitiveIdentity)
{
	InternalResolutionPolicy policy;
	ASSERT_EQ(policy.RegisterGuestDisplayExtent({3840, 2160}), ResolutionPolicyStatus::Success);

	const auto first  = policy.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});
	const auto second = policy.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});
	EXPECT_EQ(first.identity, second.identity);

	ASSERT_EQ(policy.SetTargetExtent({2560, 1440}), ResolutionPolicyStatus::Success);
	const auto changed = policy.Evaluate({3840, 2160}, {ResolutionResourceKind::ColorAttachment, false});
	EXPECT_NE(first.identity, changed.identity);
}

UT_END();
