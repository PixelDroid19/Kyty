#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/VulkanResolutionCapability.h"

UT_BEGIN(EmulatorVulkanResolutionCapability);

using namespace Libs::Graphics;

namespace {

VulkanResolutionAttachmentRequest ColorRequest()
{
	return {
	    {1280, 720},
	    VK_FORMAT_R8G8B8A8_UNORM,
	    VK_IMAGE_TILING_OPTIMAL,
	    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	    0,
	    VK_SAMPLE_COUNT_1_BIT,
	};
}

} // namespace

TEST(EmulatorVulkanResolutionCapability, NormalizesExactColorAttachmentRequirements)
{
	VulkanResolutionNormalizedRequest normalized;
	ASSERT_EQ(NormalizeVulkanResolutionAttachmentRequest(ColorRequest(), &normalized), VulkanResolutionCapabilityStatus::Success);

	EXPECT_EQ(normalized.capability_request.extent, (ResolutionExtent {1280, 720}));
	EXPECT_EQ(normalized.capability_request.required_usage, ResolutionImageUsage::ColorAttachment | ResolutionImageUsage::Sampled |
	                                                            ResolutionImageUsage::TransferSource |
	                                                            ResolutionImageUsage::TransferDestination);
	EXPECT_EQ(normalized.capability_request.required_format_features,
	          static_cast<uint64_t>(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT));
	EXPECT_EQ(normalized.capability_request.sample_count, static_cast<uint32_t>(VK_SAMPLE_COUNT_1_BIT));
}

TEST(EmulatorVulkanResolutionCapability, NormalizesExactDepthAttachmentRequirements)
{
	auto request   = ColorRequest();
	request.format = VK_FORMAT_D32_SFLOAT;
	request.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	VulkanResolutionNormalizedRequest normalized;
	ASSERT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::Success);

	EXPECT_EQ(normalized.capability_request.required_usage,
	          ResolutionImageUsage::DepthStencilAttachment | ResolutionImageUsage::Sampled | ResolutionImageUsage::TransferSource);
	EXPECT_EQ(normalized.capability_request.required_format_features,
	          static_cast<uint64_t>(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT));
}

TEST(EmulatorVulkanResolutionCapability, RejectsInvalidOrAmbiguousAttachmentRequests)
{
	VulkanResolutionNormalizedRequest normalized;
	auto                              request = ColorRequest();

	request.format = VK_FORMAT_UNDEFINED;
	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::InvalidFormat);

	request        = ColorRequest();
	request.extent = {0, 720};
	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::InvalidExtent);

	request        = ColorRequest();
	request.tiling = static_cast<VkImageTiling>(99);
	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::InvalidTiling);

	request       = ColorRequest();
	request.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized),
	          VulkanResolutionCapabilityStatus::ConflictingAttachmentUsage);

	request        = ColorRequest();
	request.format = VK_FORMAT_D32_SFLOAT;
	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::FormatUsageMismatch);

	request       = ColorRequest();
	request.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::FormatUsageMismatch);

	request       = ColorRequest();
	request.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::MissingAttachmentUsage);

	request              = ColorRequest();
	request.sample_count = static_cast<VkSampleCountFlagBits>(3);
	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::InvalidSampleCount);

	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(ColorRequest(), nullptr), VulkanResolutionCapabilityStatus::InvalidArgument);
}

TEST(EmulatorVulkanResolutionCapability, RejectsUnknownUsageBits)
{
	auto request = ColorRequest();
	request.usage |= static_cast<VkImageUsageFlags>(1u << 31u);

	VulkanResolutionNormalizedRequest normalized;
	EXPECT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::UnsupportedUsageBits);
}

TEST(EmulatorVulkanResolutionCapability, BuildsCapabilitiesFromExactQueryEvidence)
{
	const auto                        request = ColorRequest();
	VulkanResolutionNormalizedRequest normalized;
	ASSERT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::Success);

	VulkanResolutionHostEvidence evidence {};
	evidence.device_max_image_dimension_2d        = 8192;
	evidence.exact_usage_supported                = true;
	evidence.tiling_format_features               = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	                                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	evidence.image_format_properties.maxExtent    = {4096, 2048, 1};
	evidence.image_format_properties.sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;

	const auto capabilities = BuildResolutionHostImageCapabilities(evidence, normalized);

	EXPECT_EQ(capabilities.max_image_dimension_2d, 8192u);
	EXPECT_EQ(capabilities.format_features, static_cast<uint64_t>(evidence.tiling_format_features));
	EXPECT_EQ(capabilities.supported_usage, normalized.capability_request.required_usage);
	EXPECT_EQ(capabilities.supported_sample_counts, static_cast<uint32_t>(VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT));
}

TEST(EmulatorVulkanResolutionCapability, EvaluatesFormatSpecificMaxExtentPerAxis)
{
	auto request   = ColorRequest();
	request.extent = {3000, 1000};
	VulkanResolutionNormalizedRequest normalized;
	ASSERT_EQ(NormalizeVulkanResolutionAttachmentRequest(request, &normalized), VulkanResolutionCapabilityStatus::Success);

	VulkanResolutionHostEvidence evidence {};
	evidence.device_max_image_dimension_2d     = 8192;
	evidence.exact_usage_supported             = true;
	evidence.tiling_format_features            = static_cast<VkFormatFeatureFlags>(normalized.capability_request.required_format_features);
	evidence.image_format_properties.maxExtent = {4096, 2048, 1};
	evidence.image_format_properties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;

	EXPECT_EQ(EvaluateVulkanResolutionHostEvidence(evidence, normalized).status, VulkanResolutionCapabilityStatus::Success);

	normalized.capability_request.extent = {4097, 1000};
	EXPECT_EQ(EvaluateVulkanResolutionHostEvidence(evidence, normalized).status, VulkanResolutionCapabilityStatus::ImageExtentNotSupported);

	normalized.capability_request.extent = {3000, 2049};
	EXPECT_EQ(EvaluateVulkanResolutionHostEvidence(evidence, normalized).status, VulkanResolutionCapabilityStatus::ImageExtentNotSupported);
}

TEST(EmulatorVulkanResolutionCapability, EvaluatesNormalizedEvidenceWithoutAGpu)
{
	VulkanResolutionNormalizedRequest normalized;
	ASSERT_EQ(NormalizeVulkanResolutionAttachmentRequest(ColorRequest(), &normalized), VulkanResolutionCapabilityStatus::Success);

	VulkanResolutionHostEvidence evidence {};
	evidence.device_max_image_dimension_2d     = 4096;
	evidence.exact_usage_supported             = true;
	evidence.tiling_format_features            = static_cast<VkFormatFeatureFlags>(normalized.capability_request.required_format_features);
	evidence.image_format_properties.maxExtent = {4096, 4096, 1};
	evidence.image_format_properties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;

	const auto evaluation = EvaluateVulkanResolutionHostEvidence(evidence, normalized);

	EXPECT_EQ(evaluation.status, VulkanResolutionCapabilityStatus::Success);
	EXPECT_EQ(evaluation.decision.status, ResolutionImageCapabilityStatus::Supported);
	EXPECT_EQ(evaluation.decision.reason, ResolutionImageCapabilityReason::None);
}

TEST(EmulatorVulkanResolutionCapability, RejectsMissingContextsBeforeAnyVulkanQuery)
{
	const auto request = ColorRequest();

	EXPECT_EQ(EvaluateVulkanResolutionAttachment(static_cast<const GraphicContext*>(nullptr), request).status,
	          VulkanResolutionCapabilityStatus::InvalidContext);
	EXPECT_EQ(EvaluateVulkanResolutionAttachment(static_cast<VkPhysicalDevice>(nullptr), request).status,
	          VulkanResolutionCapabilityStatus::InvalidPhysicalDevice);
}

TEST(EmulatorVulkanResolutionCapability, ExposesStableStructuredStatusNames)
{
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::Success), "success");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::InvalidArgument), "invalid_argument");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::InvalidContext), "invalid_context");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::InvalidPhysicalDevice), "invalid_physical_device");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::InvalidExtent), "invalid_extent");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::InvalidFormat), "invalid_format");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::InvalidTiling), "invalid_tiling");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::InvalidSampleCount), "invalid_sample_count");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::MissingAttachmentUsage),
	             "missing_attachment_usage");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::ConflictingAttachmentUsage),
	             "conflicting_attachment_usage");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::FormatUsageMismatch), "format_usage_mismatch");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::UnsupportedUsageBits), "unsupported_usage_bits");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::ImageExtentNotSupported),
	             "image_extent_not_supported");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::ImageFormatNotSupported),
	             "image_format_not_supported");
	EXPECT_STREQ(VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus::VulkanQueryFailed), "vulkan_query_failed");
}

UT_END();
