#include "Emulator/Graphics/PresentationScaler.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
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

[[nodiscard]] bool IsSrgbFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return true;
		default: return false;
	}
}

} // namespace

// The swapchain-sized sRGB image a linear-light source is blitted into before its bytes are copied to the swapchain.
struct PresentationEncodeStage
{
	VulkanImage  image {VulkanImageType::Unknown};
	VulkanMemory memory;
};

namespace {

void DestroyEncodeStage(GraphicContext* context, VulkanSwapchain* swapchain)
{
	if (swapchain->encode_stage == nullptr) { return; }
	vkDestroyImage(context->device, swapchain->encode_stage->image.image, nullptr);
	VulkanFree(context, &swapchain->encode_stage->memory);
	delete swapchain->encode_stage;
	swapchain->encode_stage = nullptr;
}

// Swapchain recreation waits for the device to idle before its extent changes, so a stage of the old extent is no
// longer referenced by any present when it is replaced here.
PresentationEncodeStage* EnsureEncodeStage(GraphicContext* context, VulkanSwapchain* swapchain, VkFormat format)
{
	auto* stage = swapchain->encode_stage;
	if (stage != nullptr && stage->image.format == format && stage->image.extent.width == swapchain->swapchain_extent.width &&
	    stage->image.extent.height == swapchain->swapchain_extent.height)
	{
		return stage;
	}
	DestroyEncodeStage(context, swapchain);
	VulkanImageDescriptor descriptor {};
	descriptor.extent = {swapchain->swapchain_extent.width, swapchain->swapchain_extent.height, 1};
	descriptor.format = format;
	descriptor.usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	stage             = new PresentationEncodeStage;
	if (!VulkanCreateDeviceImage(context, VulkanBuildImageCreateInfo(descriptor), &stage->image, &stage->memory))
	{
		delete stage;
		return nullptr;
	}
	stage->image.format = format;
	stage->image.extent = {descriptor.extent.width, descriptor.extent.height};
	stage->image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	swapchain->encode_stage = stage;
	return stage;
}

// Blit into the stage (decoded, filtered, sRGB-encoded), then copy its bytes into the UNORM swapchain image, which ends
// in TRANSFER_DST_OPTIMAL like a direct blit.
void BlitThroughEncodeStage(CommandBuffer* command_buffer, VulkanImage* source, VulkanSwapchain* swapchain, PresentationEncodeStage* stage,
                            VkFilter filter)
{
	auto* cmd = command_buffer->GetPool()->buffers[command_buffer->GetIndex()];
	// The previous present may still be copying out of the stage on this queue: order that read before this write.
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
	UtilBlitImageTo(command_buffer, source, stage->image.image, swapchain->swapchain_extent, filter);
	stage->image.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

	VkImageMemoryBarrier barriers[2] {};
	for (auto& barrier: barriers)
	{
		barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	}
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].image         = stage->image.image;
	barriers[1].srcAccessMask = 0;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].image         = swapchain->swapchain_images[swapchain->current_index];
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
	stage->image.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	VkImageCopy region {};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.extent         = {swapchain->swapchain_extent.width, swapchain->swapchain_extent.height, 1};
	// Same texel block size and byte order: the sRGB-encoded bytes reach the UNORM swapchain unchanged.
	vkCmdCopyImage(cmd, stage->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain->swapchain_images[swapchain->current_index],
	               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

} // namespace

bool PresentationSourceHoldsLinearLight(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R16G16B16A16_SFLOAT:
		case VK_FORMAT_R32G32B32A32_SFLOAT:
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return true;
		default: return IsSrgbFormat(format);
	}
}

VkFormat PresentationSrgbTwin(VkFormat swapchain_format)
{
	switch (swapchain_format)
	{
		case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
		case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
		case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
		default: return VK_FORMAT_UNDEFINED;
	}
}

bool PresentationNeedsEncodeStage(VkFormat source_format, VkFormat swapchain_format)
{
	return PresentationSourceHoldsLinearLight(source_format) && !IsSrgbFormat(swapchain_format);
}

PresentationScaleStatus PresentationScalerBlitFinalImage(CommandBuffer* command_buffer, GraphicContext* context, VulkanImage* source,
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

	if (!PresentationNeedsEncodeStage(source->format, destination->swapchain_format))
	{
		UtilBlitImage(command_buffer, source, destination, filter);
		return PresentationScaleStatus::Success;
	}
	// A UNORM swapchain without an sRGB twin cannot show linear light correctly: refuse instead of darkening it.
	const VkFormat twin = PresentationSrgbTwin(destination->swapchain_format);
	if (twin == VK_FORMAT_UNDEFINED ||
	    !FormatSupports(context->physical_device, twin, VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
	{
		return PresentationScaleStatus::UnsupportedDestinationFormat;
	}
	auto* stage = EnsureEncodeStage(context, destination, twin);
	if (stage == nullptr) { return PresentationScaleStatus::UnsupportedDestinationFormat; }
	BlitThroughEncodeStage(command_buffer, source, destination, stage, filter);
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
