#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANBLEND_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANBLEND_H_

#include "Emulator/Common.h"

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct VulkanBlendFeatures
{
	VkBool32 independent_blend = VK_FALSE;
	VkBool32 dual_source_blend = VK_FALSE;
};

// Device discovery is not enablement. The returned bits must be passed to
// vkCreateDevice, and published as enabled only after that call succeeds.
[[nodiscard]] inline VulkanBlendFeatures VulkanPlanBlendFeatures(const VkPhysicalDeviceFeatures& supported,
                                                                 bool request_independent, bool request_dual_source)
{
	return {request_independent && supported.independentBlend ? VK_TRUE : VK_FALSE,
	        request_dual_source && supported.dualSrcBlend ? VK_TRUE : VK_FALSE};
}

struct VulkanBlendCapabilities
{
	VulkanBlendFeatures enabled;
	uint32_t max_color_attachments       = 0;
	uint32_t max_dual_source_attachments = 0;
};

enum class VulkanBlendAdmission
{
	Supported,
	InvalidAttachments,
	IndependentBlendNotEnabled,
	DualSourceBlendNotEnabled,
	DualSourceAttachmentLimit,
	MissingSecondaryOutput,
};

[[nodiscard]] inline bool VulkanBlendUsesSecondarySource(VkBlendFactor factor)
{
	return factor == VK_BLEND_FACTOR_SRC1_COLOR || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
	       factor == VK_BLEND_FACTOR_SRC1_ALPHA || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
}

[[nodiscard]] inline bool VulkanBlendAttachmentsEqual(const VkPipelineColorBlendAttachmentState& a,
                                                     const VkPipelineColorBlendAttachmentState& b)
{
	return a.blendEnable == b.blendEnable && a.srcColorBlendFactor == b.srcColorBlendFactor &&
	       a.dstColorBlendFactor == b.dstColorBlendFactor && a.colorBlendOp == b.colorBlendOp &&
	       a.srcAlphaBlendFactor == b.srcAlphaBlendFactor && a.dstAlphaBlendFactor == b.dstAlphaBlendFactor &&
	       a.alphaBlendOp == b.alphaBlendOp && a.colorWriteMask == b.colorWriteMask;
}

// VUID 00605 and 00608..00611 apply to the actual attachment structs, including
// disabled blending. A secondary export is additionally required when SRC1 is
// consumed. An MRT at Location 1 is not a Location 0, Index 1 output.
[[nodiscard]] inline VulkanBlendAdmission VulkanValidateBlendAttachments(
    const VulkanBlendCapabilities& capabilities, const VkPipelineColorBlendAttachmentState* attachments,
    uint32_t count, uint32_t secondary_output_mask, uint32_t fragment_output_mask = 0u)
{
	if (count > 8u || count > capabilities.max_color_attachments || (count != 0u && attachments == nullptr))
	{
		return VulkanBlendAdmission::InvalidAttachments;
	}
	for (uint32_t i = 0; i < count; ++i)
	{
		const auto& a = attachments[i];
		if (i != 0u && !capabilities.enabled.independent_blend && !VulkanBlendAttachmentsEqual(attachments[0], a))
		{
			return VulkanBlendAdmission::IndependentBlendNotEnabled;
		}
		if (VulkanBlendUsesSecondarySource(a.srcColorBlendFactor) || VulkanBlendUsesSecondarySource(a.dstColorBlendFactor) ||
		    VulkanBlendUsesSecondarySource(a.srcAlphaBlendFactor) || VulkanBlendUsesSecondarySource(a.dstAlphaBlendFactor))
		{
			if (!capabilities.enabled.dual_source_blend)
			{
				return VulkanBlendAdmission::DualSourceBlendNotEnabled;
			}
			if (a.blendEnable)
			{
				// The limit applies to statically used fragment output locations,
				// not the framebuffer's number of unused attachment slots (09239).
				const auto limit = capabilities.max_dual_source_attachments;
				const auto outputs = fragment_output_mask | secondary_output_mask | (1u << i);
				if (limit < 32u && (outputs >> limit) != 0u)
				{
					return VulkanBlendAdmission::DualSourceAttachmentLimit;
				}
				if ((secondary_output_mask & (1u << i)) == 0u)
				{
					return VulkanBlendAdmission::MissingSecondaryOutput;
				}
			}
		}
	}
	return VulkanBlendAdmission::Supported;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANBLEND_H_
