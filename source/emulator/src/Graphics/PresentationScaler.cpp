#include "Emulator/Graphics/PresentationScaler.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Utils.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

[[nodiscard]] VkFilter ConfiguredFilter(PresentationScaleStatus* status)
{
	if (status == nullptr)
	{
		return VK_FILTER_NEAREST;
	}
	switch (Config::GetPresentationFilter())
	{
		case Config::PresentationFilter::Nearest: return VK_FILTER_NEAREST;
		case Config::PresentationFilter::Linear: return VK_FILTER_LINEAR;
	}
	*status = PresentationScaleStatus::UnsupportedFilter;
	return VK_FILTER_NEAREST;
}

[[nodiscard]] bool FormatSupports(VkPhysicalDevice physical_device, VkFormat format, VkFormatFeatureFlags features)
{
	if (physical_device == nullptr || format == VK_FORMAT_UNDEFINED)
	{
		return false;
	}
	VkFormatProperties properties {};
	vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
	return (properties.optimalTilingFeatures & features) == features;
}

} // namespace

PresentationScaleStatus PresentationScalerBlitFinalImage(CommandBuffer* command_buffer, const GraphicContext* context, VulkanImage* source,
                                                         VulkanSwapchain* destination)
{
	if (command_buffer == nullptr || context == nullptr || source == nullptr || destination == nullptr || source->image == nullptr ||
	    destination->swapchain == nullptr || destination->current_index >= destination->swapchain_images_count)
	{
		return PresentationScaleStatus::InvalidArgument;
	}

	PresentationScaleStatus status = PresentationScaleStatus::Success;
	const VkFilter          filter = ConfiguredFilter(&status);
	if (status != PresentationScaleStatus::Success)
	{
		return status;
	}

	VkFormatFeatureFlags source_features = VK_FORMAT_FEATURE_BLIT_SRC_BIT;
	if (filter == VK_FILTER_LINEAR)
	{
		source_features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	}
	if (!FormatSupports(context->physical_device, source->format, source_features))
	{
		return PresentationScaleStatus::UnsupportedSourceFormat;
	}
	if (!FormatSupports(context->physical_device, destination->swapchain_format, VK_FORMAT_FEATURE_BLIT_DST_BIT))
	{
		return PresentationScaleStatus::UnsupportedDestinationFormat;
	}

	UtilBlitImage(command_buffer, source, destination, filter);
	return PresentationScaleStatus::Success;
}

const char* PresentationScaleStatusName(PresentationScaleStatus status)
{
	switch (status)
	{
		case PresentationScaleStatus::Success: return "success";
		case PresentationScaleStatus::InvalidArgument: return "invalid_argument";
		case PresentationScaleStatus::UnsupportedSourceFormat: return "unsupported_source_format";
		case PresentationScaleStatus::UnsupportedDestinationFormat: return "unsupported_destination_format";
		case PresentationScaleStatus::UnsupportedFilter: return "unsupported_filter";
	}
	return "unknown";
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
