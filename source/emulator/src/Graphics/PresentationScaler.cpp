#include "Emulator/Graphics/PresentationScaler.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/PresentationFsr.h"
#include "Emulator/Graphics/PresentationFsr3.h"
#include "Emulator/Graphics/Utils.h"

#include <algorithm>

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
		// FSR only upscales; a source larger than the swapchain is reduced with the linear filter.
		case Config::PresentationFilter::Fsr: return VK_FILTER_LINEAR;
		case Config::PresentationFilter::Fsr3:
			if (PresentationFsr3Available()) { return VK_FILTER_LINEAR; }
			break;
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

// One half-resolution copy for present downscales larger than 2:1 on an axis.
struct PresentationDownscaleStage
{
	VulkanImage  image {VulkanImageType::Unknown};
	VulkanMemory memory;
};

// The letterboxed view in the swapchain format, copied into the centre of the cleared swapchain image.
struct PresentationViewStage
{
	VulkanImage  image {VulkanImageType::Unknown};
	VulkanMemory memory;
};

namespace {

// The swapchain image, or the view stage, that the scaling paths write.
struct PresentationTarget
{
	VkImage    image  = nullptr;
	VkExtent2D extent = {};
	VkFormat   format = VK_FORMAT_UNDEFINED;
};

template <typename Stage>
void DestroyStage(GraphicContext* context, Stage** slot)
{
	if (*slot == nullptr) { return; }
	vkDestroyImage(context->device, (*slot)->image.image, nullptr);
	VulkanFree(context, &(*slot)->memory);
	delete *slot;
	*slot = nullptr;
}

// The stage is recreated whenever its format or extent changes; swapchain recreation waits for the device to idle,
// so a stage of an old extent is no longer read by any present here.
template <typename Stage>
Stage* EnsureStage(GraphicContext* context, Stage** slot, VkFormat format, VkExtent2D extent)
{
	auto* stage = *slot;
	if (stage != nullptr && stage->image.format == format && stage->image.extent.width == extent.width &&
	    stage->image.extent.height == extent.height)
	{
		return stage;
	}
	DestroyStage(context, slot);
	VulkanImageDescriptor descriptor {};
	descriptor.extent = {extent.width, extent.height, 1};
	descriptor.format = format;
	descriptor.usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	stage             = new Stage;
	if (!VulkanCreateDeviceImage(context, VulkanBuildImageCreateInfo(descriptor), &stage->image, &stage->memory))
	{
		delete stage;
		return nullptr;
	}
	stage->image.format = format;
	stage->image.extent = extent;
	stage->image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	*slot               = stage;
	return stage;
}

// Blit into the stage (decoded, filtered, sRGB-encoded), then copy its bytes into the UNORM target, which ends in
// TRANSFER_DST_OPTIMAL like a direct blit.
void BlitThroughEncodeStage(CommandBuffer* command_buffer, VulkanImage* source, const PresentationTarget& target,
                            PresentationEncodeStage* stage, VkFilter filter)
{
	auto* cmd = command_buffer->RecordOutsideRenderPass();
	// The previous present may still be copying out of the stage on this queue: order that read before this write.
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
	UtilBlitImageTo(command_buffer, source, stage->image.image, target.extent, filter);
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
	barriers[1].image         = target.image;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
	stage->image.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	VkImageCopy region {};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.extent         = {target.extent.width, target.extent.height, 1};
	// Same texel block size and byte order: the sRGB-encoded bytes reach the UNORM target unchanged.
	vkCmdCopyImage(cmd, stage->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	               &region);
}

// Clears the swapchain image black and copies the view into its centre; the image ends in TRANSFER_DST_OPTIMAL like a
// direct blit.
void CopyViewToSwapchain(CommandBuffer* command_buffer, PresentationViewStage* stage, VulkanSwapchain* swapchain, VkRect2D view)
{
	auto*         cmd             = command_buffer->RecordOutsideRenderPass();
	const VkImage swapchain_image = swapchain->swapchain_images[swapchain->current_index];

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
	barriers[1].image         = swapchain_image;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
	stage->image.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	const VkClearColorValue       black {{0.0f, 0.0f, 0.0f, 1.0f}};
	const VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(cmd, swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

	VkMemoryBarrier cleared {};
	cleared.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	cleared.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &cleared, 0, nullptr, 0, nullptr);

	VkImageCopy region {};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstOffset      = {view.offset.x, view.offset.y, 0};
	region.extent         = {view.extent.width, view.extent.height, 1};
	vkCmdCopyImage(cmd, stage->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	               &region);
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

VkExtent2D PresentationDownscaleExtent(VkExtent2D source, VkExtent2D target)
{
	if (source.width == 0 || source.height == 0 || target.width == 0 || target.height == 0)
	{
		return {0, 0};
	}
	// Only the axes that shrink by more than 2:1 need the extra pass.
	const bool wide = source.width > 2u * target.width;
	const bool high = source.height > 2u * target.height;
	if (!wide && !high)
	{
		return {0, 0};
	}
	VkExtent2D extent = source;
	if (wide)
	{
		extent.width = (source.width + 1u) / 2u;
	}
	if (high)
	{
		extent.height = (source.height + 1u) / 2u;
	}
	return extent;
}

VkRect2D PresentationViewport(VkExtent2D source, VkExtent2D target, Config::PresentationScaling scaling)
{
	VkRect2D view {{0, 0}, target};
	if (scaling == Config::PresentationScaling::Stretch || source.width == 0 || source.height == 0 || target.width == 0 ||
	    target.height == 0)
	{
		return view;
	}
	const bool fits = source.width <= target.width && source.height <= target.height;
	if (scaling == Config::PresentationScaling::Integer && fits)
	{
		const uint32_t scale = std::min(target.width / source.width, target.height / source.height);
		view.extent          = {source.width * scale, source.height * scale};
	} else if (static_cast<uint64_t>(source.width) * target.height > static_cast<uint64_t>(target.width) * source.height)
	{
		view.extent.height = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(target.width) * source.height / source.width));
	} else
	{
		view.extent.width = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(target.height) * source.width / source.height));
	}
	view.offset = {static_cast<int32_t>((target.width - view.extent.width) / 2u), static_cast<int32_t>((target.height - view.extent.height) / 2u)};
	return view;
}

namespace {

PresentationScaleStatus BlitToTarget(CommandBuffer* command_buffer, GraphicContext* context, VulkanImage* source, VulkanSwapchain* destination,
                                     const PresentationTarget& target, VkFilter filter)
{
	const auto     presentation_filter = Config::GetPresentationFilter();
	const bool     upscales            = PresentationFsrApplies(source->extent, target.extent);
	const VkFormat content_format      = source->format;
	VulkanImage*   temporal            = nullptr;
	if (presentation_filter == Config::PresentationFilter::Fsr3)
	{
		temporal = PresentationFsr3Record(command_buffer, context, source, target.extent, Config::GetPresentationFsrSharpness(),
		                                  &destination->fsr3_stage);
	}
	if (temporal != nullptr)
	{
		source = temporal;
	} else if (upscales && presentation_filter != Config::PresentationFilter::Nearest && presentation_filter != Config::PresentationFilter::Linear)
	{
		// The passes encode and decode sRGB themselves, so no encode stage is needed.
		return PresentationFsrRecord(command_buffer, context, source, target.image, target.extent, target.format,
		                             Config::GetPresentationFsrSharpness(), &destination->fsr_stage)
		           ? PresentationScaleStatus::Success
		           : PresentationScaleStatus::UnsupportedFilter;
	}
	// A 3:1 blit samples texel centres, so the game's one-texel dither survives as a static beat. Halve the source
	// first: the final blit is then at most 2:1, whose bilinear footprint is a 2x2 box.
	const VkExtent2D prefilter = PresentationDownscaleExtent(source->extent, target.extent);
	const VkFormatFeatureFlags prefilter_features =
	    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	if (filter == VK_FILTER_LINEAR && prefilter.width != 0 && FormatSupports(context->physical_device, source->format, prefilter_features))
	{
		if (auto* stage = EnsureStage(context, &destination->downscale_stage, source->format, prefilter); stage != nullptr)
		{
			auto* cmd = command_buffer->RecordOutsideRenderPass();
			// The previous present may still be reading this stage on the same queue.
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
			UtilBlitImageTo(command_buffer, source, stage->image.image, stage->image.extent, VK_FILTER_LINEAR);
			stage->image.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			source              = &stage->image;
		}
	}
	if (!PresentationNeedsEncodeStage(content_format, target.format))
	{
		UtilBlitImageTo(command_buffer, source, target.image, target.extent, filter);
		return PresentationScaleStatus::Success;
	}
	// A UNORM swapchain without an sRGB twin cannot show linear light correctly: refuse instead of darkening it.
	const VkFormat twin = PresentationSrgbTwin(target.format);
	if (twin == VK_FORMAT_UNDEFINED ||
	    !FormatSupports(context->physical_device, twin, VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
	{
		return PresentationScaleStatus::UnsupportedDestinationFormat;
	}
	auto* stage = EnsureStage(context, &destination->encode_stage, twin, target.extent);
	if (stage == nullptr) { return PresentationScaleStatus::UnsupportedDestinationFormat; }
	BlitThroughEncodeStage(command_buffer, source, target, stage, filter);
	return PresentationScaleStatus::Success;
}

} // namespace

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

	const VkRect2D view = PresentationViewport(source->extent, destination->swapchain_extent, Config::GetPresentationScaling());
	const bool     letterboxed =
	    view.extent.width != destination->swapchain_extent.width || view.extent.height != destination->swapchain_extent.height;
	if (!letterboxed)
	{
		const PresentationTarget target {destination->swapchain_images[destination->current_index], destination->swapchain_extent,
		                                 destination->swapchain_format};
		return BlitToTarget(command_buffer, context, source, destination, target, filter);
	}
	if (!FormatSupports(context->physical_device, destination->swapchain_format, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
	{
		return PresentationScaleStatus::UnsupportedDestinationFormat;
	}
	auto* stage = EnsureStage(context, &destination->view_stage, destination->swapchain_format, view.extent);
	if (stage == nullptr) { return PresentationScaleStatus::UnsupportedDestinationFormat; }
	// The previous present may still be copying out of the view on this queue: order that read before these writes.
	vkCmdPipelineBarrier(command_buffer->RecordOutsideRenderPass(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 0, nullptr, 0, nullptr);
	const auto blit_status =
	    BlitToTarget(command_buffer, context, source, destination, {stage->image.image, view.extent, destination->swapchain_format}, filter);
	if (blit_status == PresentationScaleStatus::Success)
	{
		CopyViewToSwapchain(command_buffer, stage, destination, view);
	}
	return blit_status;
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
