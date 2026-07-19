#include "Emulator/Graphics/VulkanResolutionCapability.h"

#include "Emulator/Graphics/GraphicContext.h"

#include <cstdint>

namespace Kyty::Libs::Graphics {

namespace {

constexpr VkImageUsageFlags kKnownAttachmentUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

[[nodiscard]] bool IsSingleSampleCount(VkSampleCountFlagBits sample_count)
{
	const auto         value              = static_cast<uint32_t>(sample_count);
	constexpr uint32_t kKnownSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT |
	                                        VK_SAMPLE_COUNT_16_BIT | VK_SAMPLE_COUNT_32_BIT | VK_SAMPLE_COUNT_64_BIT;
	return value != 0 && (value & (value - 1u)) == 0 && (value & kKnownSampleCounts) != 0;
}

[[nodiscard]] bool IsDepthStencilFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_X8_D24_UNORM_PACK32:
		case VK_FORMAT_D32_SFLOAT:
		case VK_FORMAT_S8_UINT:
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
		default: return false;
	}
}

void AddUsage(bool enabled, ResolutionImageUsage usage, VkFormatFeatureFlagBits feature, ResolutionImageUsage* normalized_usage,
              uint64_t* required_features)
{
	if (enabled)
	{
		*normalized_usage = *normalized_usage | usage;
		*required_features |= static_cast<uint64_t>(feature);
	}
}

} // namespace

VulkanResolutionCapabilityStatus NormalizeVulkanResolutionAttachmentRequest(const VulkanResolutionAttachmentRequest& request,
                                                                            VulkanResolutionNormalizedRequest*       normalized)
{
	if (normalized == nullptr)
	{
		return VulkanResolutionCapabilityStatus::InvalidArgument;
	}
	if (request.extent.width == 0 || request.extent.height == 0)
	{
		return VulkanResolutionCapabilityStatus::InvalidExtent;
	}
	if (request.format == VK_FORMAT_UNDEFINED)
	{
		return VulkanResolutionCapabilityStatus::InvalidFormat;
	}
	if (request.tiling != VK_IMAGE_TILING_LINEAR && request.tiling != VK_IMAGE_TILING_OPTIMAL)
	{
		return VulkanResolutionCapabilityStatus::InvalidTiling;
	}
	if (!IsSingleSampleCount(request.sample_count))
	{
		return VulkanResolutionCapabilityStatus::InvalidSampleCount;
	}
	if ((request.usage & ~kKnownAttachmentUsage) != 0)
	{
		return VulkanResolutionCapabilityStatus::UnsupportedUsageBits;
	}

	const bool color_attachment = (request.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
	const bool depth_attachment = (request.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
	if (color_attachment && depth_attachment)
	{
		return VulkanResolutionCapabilityStatus::ConflictingAttachmentUsage;
	}
	if (!color_attachment && !depth_attachment)
	{
		return VulkanResolutionCapabilityStatus::MissingAttachmentUsage;
	}
	if (IsDepthStencilFormat(request.format) != depth_attachment)
	{
		return VulkanResolutionCapabilityStatus::FormatUsageMismatch;
	}

	ResolutionImageUsage normalized_usage  = ResolutionImageUsage::None;
	uint64_t             required_features = 0;
	AddUsage(color_attachment, ResolutionImageUsage::ColorAttachment, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, &normalized_usage,
	         &required_features);
	AddUsage(depth_attachment, ResolutionImageUsage::DepthStencilAttachment, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
	         &normalized_usage, &required_features);
	AddUsage((request.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0, ResolutionImageUsage::Sampled, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
	         &normalized_usage, &required_features);
	AddUsage((request.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0, ResolutionImageUsage::Storage, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
	         &normalized_usage, &required_features);
	AddUsage((request.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0, ResolutionImageUsage::TransferSource,
	         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, &normalized_usage, &required_features);
	AddUsage((request.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0, ResolutionImageUsage::TransferDestination,
	         VK_FORMAT_FEATURE_TRANSFER_DST_BIT, &normalized_usage, &required_features);

	normalized->capability_request = {
	    request.extent,
	    required_features,
	    normalized_usage,
	    static_cast<uint32_t>(request.sample_count),
	};
	return VulkanResolutionCapabilityStatus::Success;
}

ResolutionHostImageCapabilities BuildResolutionHostImageCapabilities(const VulkanResolutionHostEvidence&      evidence,
                                                                     const VulkanResolutionNormalizedRequest& normalized)
{
	return {
	    evidence.device_max_image_dimension_2d,
	    static_cast<uint64_t>(evidence.tiling_format_features),
	    evidence.exact_usage_supported ? normalized.capability_request.required_usage : ResolutionImageUsage::None,
	    static_cast<uint32_t>(evidence.image_format_properties.sampleCounts),
	};
}

VulkanResolutionCapabilityEvaluation EvaluateVulkanResolutionHostEvidence(const VulkanResolutionHostEvidence&      evidence,
                                                                          const VulkanResolutionNormalizedRequest& normalized)
{
	VulkanResolutionCapabilityEvaluation evaluation {};
	evaluation.normalized = normalized;
	evaluation.evidence   = evidence;

	const auto extent = normalized.capability_request.extent;
	if (extent.width > evidence.image_format_properties.maxExtent.width ||
	    extent.height > evidence.image_format_properties.maxExtent.height)
	{
		evaluation.status = VulkanResolutionCapabilityStatus::ImageExtentNotSupported;
		return evaluation;
	}

	evaluation.capabilities = BuildResolutionHostImageCapabilities(evidence, normalized);
	evaluation.decision     = EvaluateResolutionImageCapability(evaluation.capabilities, normalized.capability_request);
	evaluation.status       = VulkanResolutionCapabilityStatus::Success;
	return evaluation;
}

VulkanResolutionCapabilityEvaluation EvaluateVulkanResolutionAttachment(VkPhysicalDevice                         physical_device,
                                                                        const VulkanResolutionAttachmentRequest& request)
{
	VulkanResolutionCapabilityEvaluation evaluation {};
	if (physical_device == nullptr)
	{
		evaluation.status = VulkanResolutionCapabilityStatus::InvalidPhysicalDevice;
		return evaluation;
	}

	evaluation.status = NormalizeVulkanResolutionAttachmentRequest(request, &evaluation.normalized);
	if (evaluation.status != VulkanResolutionCapabilityStatus::Success)
	{
		return evaluation;
	}

	VkPhysicalDeviceProperties device_properties {};
	vkGetPhysicalDeviceProperties(physical_device, &device_properties);
	evaluation.evidence.device_max_image_dimension_2d = device_properties.limits.maxImageDimension2D;

	VkFormatProperties format_properties {};
	vkGetPhysicalDeviceFormatProperties(physical_device, request.format, &format_properties);
	evaluation.evidence.tiling_format_features =
	    request.tiling == VK_IMAGE_TILING_OPTIMAL ? format_properties.optimalTilingFeatures : format_properties.linearTilingFeatures;

	evaluation.query_result =
	    vkGetPhysicalDeviceImageFormatProperties(physical_device, request.format, VK_IMAGE_TYPE_2D, request.tiling, request.usage,
	                                             request.flags, &evaluation.evidence.image_format_properties);
	if (evaluation.query_result != VK_SUCCESS)
	{
		evaluation.status = evaluation.query_result == VK_ERROR_FORMAT_NOT_SUPPORTED
		                        ? VulkanResolutionCapabilityStatus::ImageFormatNotSupported
		                        : VulkanResolutionCapabilityStatus::VulkanQueryFailed;
		return evaluation;
	}

	evaluation.evidence.exact_usage_supported = true;
	return EvaluateVulkanResolutionHostEvidence(evaluation.evidence, evaluation.normalized);
}

VulkanResolutionCapabilityEvaluation EvaluateVulkanResolutionAttachment(const GraphicContext*                    context,
                                                                        const VulkanResolutionAttachmentRequest& request)
{
	if (context == nullptr)
	{
		VulkanResolutionCapabilityEvaluation evaluation {};
		evaluation.status = VulkanResolutionCapabilityStatus::InvalidContext;
		return evaluation;
	}
	return EvaluateVulkanResolutionAttachment(context->physical_device, request);
}

const char* VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus status)
{
	switch (status)
	{
		case VulkanResolutionCapabilityStatus::Success: return "success";
		case VulkanResolutionCapabilityStatus::InvalidArgument: return "invalid_argument";
		case VulkanResolutionCapabilityStatus::InvalidContext: return "invalid_context";
		case VulkanResolutionCapabilityStatus::InvalidPhysicalDevice: return "invalid_physical_device";
		case VulkanResolutionCapabilityStatus::InvalidExtent: return "invalid_extent";
		case VulkanResolutionCapabilityStatus::InvalidFormat: return "invalid_format";
		case VulkanResolutionCapabilityStatus::InvalidTiling: return "invalid_tiling";
		case VulkanResolutionCapabilityStatus::InvalidSampleCount: return "invalid_sample_count";
		case VulkanResolutionCapabilityStatus::MissingAttachmentUsage: return "missing_attachment_usage";
		case VulkanResolutionCapabilityStatus::ConflictingAttachmentUsage: return "conflicting_attachment_usage";
		case VulkanResolutionCapabilityStatus::FormatUsageMismatch: return "format_usage_mismatch";
		case VulkanResolutionCapabilityStatus::UnsupportedUsageBits: return "unsupported_usage_bits";
		case VulkanResolutionCapabilityStatus::ImageExtentNotSupported: return "image_extent_not_supported";
		case VulkanResolutionCapabilityStatus::ImageFormatNotSupported: return "image_format_not_supported";
		case VulkanResolutionCapabilityStatus::VulkanQueryFailed: return "vulkan_query_failed";
	}
	return "unknown";
}

} // namespace Kyty::Libs::Graphics
