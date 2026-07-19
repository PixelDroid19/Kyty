#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANRESOLUTIONCAPABILITY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANRESOLUTIONCAPABILITY_H_

#include "Emulator/Graphics/ResolutionImageCapability.h"

#include <vulkan/vulkan_core.h>

namespace Kyty::Libs::Graphics {

struct GraphicContext;

enum class VulkanResolutionCapabilityStatus : uint8_t
{
	Success,
	InvalidArgument,
	InvalidContext,
	InvalidPhysicalDevice,
	InvalidExtent,
	InvalidFormat,
	InvalidTiling,
	InvalidSampleCount,
	MissingAttachmentUsage,
	ConflictingAttachmentUsage,
	FormatUsageMismatch,
	UnsupportedUsageBits,
	ImageExtentNotSupported,
	ImageFormatNotSupported,
	VulkanQueryFailed,
};

struct VulkanResolutionAttachmentRequest
{
	ResolutionExtent      extent;
	VkFormat              format       = VK_FORMAT_UNDEFINED;
	VkImageTiling         tiling       = VK_IMAGE_TILING_OPTIMAL;
	VkImageUsageFlags     usage        = 0;
	VkImageCreateFlags    flags        = 0;
	VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT;
};

struct VulkanResolutionNormalizedRequest
{
	ResolutionImageCapabilityRequest capability_request;
};

// Query results are represented separately so flag translation and capability
// construction remain deterministic and testable without a GPU.
struct VulkanResolutionHostEvidence
{
	uint32_t                device_max_image_dimension_2d = 0;
	VkFormatFeatureFlags    tiling_format_features        = 0;
	VkImageFormatProperties image_format_properties       = {};
	bool                    exact_usage_supported         = false;
};

struct VulkanResolutionCapabilityEvaluation
{
	VulkanResolutionCapabilityStatus  status       = VulkanResolutionCapabilityStatus::InvalidArgument;
	VkResult                          query_result = VK_SUCCESS;
	VulkanResolutionNormalizedRequest normalized;
	VulkanResolutionHostEvidence      evidence;
	ResolutionHostImageCapabilities   capabilities;
	ResolutionImageCapabilityDecision decision;
};

[[nodiscard]] VulkanResolutionCapabilityStatus NormalizeVulkanResolutionAttachmentRequest(const VulkanResolutionAttachmentRequest& request,
                                                                                          VulkanResolutionNormalizedRequest* normalized);

[[nodiscard]] ResolutionHostImageCapabilities BuildResolutionHostImageCapabilities(const VulkanResolutionHostEvidence&      evidence,
                                                                                   const VulkanResolutionNormalizedRequest& normalized);

[[nodiscard]] VulkanResolutionCapabilityEvaluation
EvaluateVulkanResolutionHostEvidence(const VulkanResolutionHostEvidence& evidence, const VulkanResolutionNormalizedRequest& normalized);

[[nodiscard]] VulkanResolutionCapabilityEvaluation EvaluateVulkanResolutionAttachment(VkPhysicalDevice physical_device,
                                                                                      const VulkanResolutionAttachmentRequest& request);

[[nodiscard]] VulkanResolutionCapabilityEvaluation EvaluateVulkanResolutionAttachment(const GraphicContext*                    context,
                                                                                      const VulkanResolutionAttachmentRequest& request);

[[nodiscard]] const char* VulkanResolutionCapabilityStatusName(VulkanResolutionCapabilityStatus status);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANRESOLUTIONCAPABILITY_H_ */
