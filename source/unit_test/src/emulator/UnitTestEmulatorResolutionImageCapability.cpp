#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ResolutionImageCapability.h"

UT_BEGIN(EmulatorResolutionImageCapability);

using namespace Libs::Graphics;

namespace {

constexpr uint64_t kColorAttachmentFeature = 1ull << 0u;
constexpr uint64_t kDepthAttachmentFeature = 1ull << 1u;

constexpr uint32_t kSampleCount1 = 1u;
constexpr uint32_t kSampleCount4 = 4u;

constexpr ResolutionImageUsage kColorUsage = ResolutionImageUsage::ColorAttachment | ResolutionImageUsage::Sampled;

ResolutionHostImageCapabilities FullCapabilities()
{
	return {
	    4096,
	    kColorAttachmentFeature | kDepthAttachmentFeature,
	    ResolutionImageUsage::ColorAttachment | ResolutionImageUsage::DepthStencilAttachment | ResolutionImageUsage::Sampled |
	        ResolutionImageUsage::TransferSource | ResolutionImageUsage::TransferDestination,
	    kSampleCount1 | kSampleCount4,
	};
}

ResolutionImageCapabilityRequest ColorRequest()
{
	return {{1280, 720}, kColorAttachmentFeature, kColorUsage, kSampleCount1};
}

} // namespace

TEST(EmulatorResolutionImageCapability, AcceptsAnExactSupportedRequest)
{
	const auto decision = EvaluateResolutionImageCapability(FullCapabilities(), ColorRequest());

	EXPECT_EQ(decision.status, ResolutionImageCapabilityStatus::Supported);
	EXPECT_EQ(decision.reason, ResolutionImageCapabilityReason::None);
	EXPECT_EQ(decision.extent, (ResolutionExtent {1280, 720}));
	EXPECT_EQ(decision.sample_count, kSampleCount1);
}

TEST(EmulatorResolutionImageCapability, RejectsZeroExtent)
{
	auto request   = ColorRequest();
	request.extent = {0, 720};

	const auto decision = EvaluateResolutionImageCapability(FullCapabilities(), request);

	EXPECT_EQ(decision.status, ResolutionImageCapabilityStatus::InvalidRequest);
	EXPECT_EQ(decision.reason, ResolutionImageCapabilityReason::ZeroExtent);
}

TEST(EmulatorResolutionImageCapability, RejectsAnExtentBeyondTheDeviceLimit)
{
	auto request   = ColorRequest();
	request.extent = {4097, 720};

	const auto decision = EvaluateResolutionImageCapability(FullCapabilities(), request);

	EXPECT_EQ(decision.status, ResolutionImageCapabilityStatus::Unsupported);
	EXPECT_EQ(decision.reason, ResolutionImageCapabilityReason::ExceedsMaxImageDimension2D);
}

TEST(EmulatorResolutionImageCapability, RejectsMissingFormatFeatures)
{
	auto request                     = ColorRequest();
	request.required_format_features = kColorAttachmentFeature | (1ull << 7u);

	const auto decision = EvaluateResolutionImageCapability(FullCapabilities(), request);

	EXPECT_EQ(decision.status, ResolutionImageCapabilityStatus::Unsupported);
	EXPECT_EQ(decision.reason, ResolutionImageCapabilityReason::MissingFormatFeatures);
	EXPECT_EQ(decision.missing_format_features, (1ull << 7u));
}

TEST(EmulatorResolutionImageCapability, RejectsMissingImageUsage)
{
	auto request           = ColorRequest();
	request.required_usage = kColorUsage | ResolutionImageUsage::Storage;

	const auto decision = EvaluateResolutionImageCapability(FullCapabilities(), request);

	EXPECT_EQ(decision.status, ResolutionImageCapabilityStatus::Unsupported);
	EXPECT_EQ(decision.reason, ResolutionImageCapabilityReason::UnsupportedUsage);
	EXPECT_EQ(decision.missing_usage, ResolutionImageUsage::Storage);
}

TEST(EmulatorResolutionImageCapability, RejectsUnsupportedSampleCount)
{
	auto request                         = ColorRequest();
	request.sample_count                 = kSampleCount4;
	auto capabilities                    = FullCapabilities();
	capabilities.supported_sample_counts = kSampleCount1;

	const auto decision = EvaluateResolutionImageCapability(capabilities, request);

	EXPECT_EQ(decision.status, ResolutionImageCapabilityStatus::Unsupported);
	EXPECT_EQ(decision.reason, ResolutionImageCapabilityReason::UnsupportedSampleCount);
}

TEST(EmulatorResolutionImageCapability, RejectsMalformedRequestsAndCapabilities)
{
	auto request                     = ColorRequest();
	request.required_format_features = 0;
	EXPECT_EQ(EvaluateResolutionImageCapability(FullCapabilities(), request).reason,
	          ResolutionImageCapabilityReason::NoRequiredFormatFeatures);

	request                = ColorRequest();
	request.required_usage = ResolutionImageUsage::None;
	EXPECT_EQ(EvaluateResolutionImageCapability(FullCapabilities(), request).reason, ResolutionImageCapabilityReason::NoRequiredUsage);

	request              = ColorRequest();
	request.sample_count = 3;
	EXPECT_EQ(EvaluateResolutionImageCapability(FullCapabilities(), request).reason, ResolutionImageCapabilityReason::InvalidSampleCount);

	auto capabilities                   = FullCapabilities();
	capabilities.max_image_dimension_2d = 0;
	EXPECT_EQ(EvaluateResolutionImageCapability(capabilities, ColorRequest()).reason,
	          ResolutionImageCapabilityReason::InvalidHostCapabilities);
}

TEST(EmulatorResolutionImageCapability, UsesOnlyExplicitCapabilities)
{
	auto intel_like = FullCapabilities();
	auto amd_like   = FullCapabilities();
	auto apple_like = FullCapabilities();

	EXPECT_EQ(EvaluateResolutionImageCapability(intel_like, ColorRequest()), EvaluateResolutionImageCapability(amd_like, ColorRequest()));
	EXPECT_EQ(EvaluateResolutionImageCapability(amd_like, ColorRequest()), EvaluateResolutionImageCapability(apple_like, ColorRequest()));
}

TEST(EmulatorResolutionImageCapability, ExposesStableDiagnosticNames)
{
	EXPECT_STREQ(ResolutionImageCapabilityStatusName(ResolutionImageCapabilityStatus::Supported), "supported");
	EXPECT_STREQ(ResolutionImageCapabilityStatusName(ResolutionImageCapabilityStatus::InvalidRequest), "invalid_request");
	EXPECT_STREQ(ResolutionImageCapabilityStatusName(ResolutionImageCapabilityStatus::InvalidHostCapabilities),
	             "invalid_host_capabilities");
	EXPECT_STREQ(ResolutionImageCapabilityStatusName(ResolutionImageCapabilityStatus::Unsupported), "unsupported");

	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::None), "none");
	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::ZeroExtent), "zero_extent");
	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::NoRequiredFormatFeatures),
	             "no_required_format_features");
	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::NoRequiredUsage), "no_required_usage");
	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::InvalidSampleCount), "invalid_sample_count");
	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::InvalidHostCapabilities),
	             "invalid_host_capabilities");
	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::ExceedsMaxImageDimension2D),
	             "exceeds_max_image_dimension_2d");
	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::MissingFormatFeatures), "missing_format_features");
	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::UnsupportedUsage), "unsupported_usage");
	EXPECT_STREQ(ResolutionImageCapabilityReasonName(ResolutionImageCapabilityReason::UnsupportedSampleCount), "unsupported_sample_count");
}

UT_END();
