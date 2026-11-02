#include "Emulator/Graphics/PresentationFsr3.h"
#include "Emulator/Graphics/PresentationFsr.h"

#ifdef KYTY_EMU_ENABLED

#ifdef KYTY_FSR3_ENABLED

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Utils.h"

#include "GraphicsRenderInternal.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace Kyty::Libs::Graphics {
namespace {

constexpr VkFormat kColorFormat  = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kDepthFormat  = VK_FORMAT_D32_SFLOAT;
// A geometry pass writes several targets at once; lighting and post-processing passes write one or two.
constexpr uint32_t kGeometryPassMinTargets = 3;
// The upscaler only uses the camera to scale its disocclusion thresholds, and the guest camera is not known in general.
constexpr float kAssumedNear   = 0.1f;
constexpr float kAssumedFar    = 10000.0f;
constexpr float kAssumedFovRad = 1.0f;

struct FrameGuides
{
	uint32_t targets       = 0;
	uint64_t motion_vaddr  = 0;
	uint64_t motion_size   = 0;
	uint64_t depth_vaddr[3] {};
	uint64_t depth_size[3] {};
	int      depth_vaddr_num = 0;
	bool     depth_inverted  = false;
};

std::mutex  g_guides_mutex;
FrameGuides g_guides;

struct StageImage
{
	VkImage      image = nullptr;
	VulkanMemory memory;
	VkExtent2D   extent {};
	VkFormat     format = VK_FORMAT_UNDEFINED;
};

bool IsGuideDepthFormat(VkFormat format)
{
	return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D32_SFLOAT;
}

// The backend asks for an extension name, which resolves only when that extension is enabled; the device has the core function.
PFN_vkVoidFunction VKAPI_PTR BackendDeviceProcAddr(VkDevice device, const char* name)
{
	return vkGetDeviceProcAddr(device, std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0 ? "vkGetBufferMemoryRequirements2" : name);
}

bool SameExtent(VkExtent2D a, VkExtent2D b)
{
	return a.width == b.width && a.height == b.height;
}

} // namespace

struct PresentationFsr3Stage
{
	FfxFsr3UpscalerContext context {};
	bool                   context_live = false;
	std::vector<uint8_t>   scratch;
	VkDeviceContext        device_context {};
	VkExtent2D             render_extent {};
	VkExtent2D             display_extent {};
	bool                   depth_inverted = false;
	bool                   reset          = true;
	std::chrono::steady_clock::time_point last_present {};
	StageImage             input;
	StageImage             motion;
	StageImage             depth;
	// Written and read by the upscaler across frames; they stay in the general layout once initialized.
	StageImage             dilated_depth;
	StageImage             dilated_motion;
	StageImage             previous_depth;
	bool                   shared_initialized = false;
	VulkanImage            output {VulkanImageType::Unknown};
	VulkanMemory           output_memory;
};

namespace {

void DestroyImage(GraphicContext* context, StageImage* image)
{
	if (image->image != nullptr)
	{
		vkDestroyImage(context->device, image->image, nullptr);
		VulkanFree(context, &image->memory);
	}
	*image = StageImage {};
}

bool CreateImage(GraphicContext* context, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, StageImage* image)
{
	VulkanImageDescriptor descriptor {};
	descriptor.extent = {extent.width, extent.height, 1};
	descriptor.format = format;
	descriptor.usage  = usage;
	VulkanImage created(VulkanImageType::Unknown);
	if (!VulkanCreateDeviceImage(context, VulkanBuildImageCreateInfo(descriptor), &created, &image->memory))
	{
		return false;
	}
	image->image  = created.image;
	image->extent = extent;
	image->format = format;
	return true;
}

void DestroyResources(GraphicContext* context, PresentationFsr3Stage* stage)
{
	if (stage->context_live)
	{
		EXIT_IF(ffxFsr3UpscalerContextDestroy(&stage->context) != FFX_OK);
		stage->context_live = false;
	}
	DestroyImage(context, &stage->input);
	DestroyImage(context, &stage->motion);
	DestroyImage(context, &stage->depth);
	DestroyImage(context, &stage->dilated_depth);
	DestroyImage(context, &stage->dilated_motion);
	DestroyImage(context, &stage->previous_depth);
	if (stage->output.image != nullptr)
	{
		vkDestroyImage(context->device, stage->output.image, nullptr);
		VulkanFree(context, &stage->output_memory);
		stage->output.image = nullptr;
	}
}

bool CreateResources(GraphicContext* context, PresentationFsr3Stage* stage, VkExtent2D render, VkExtent2D display, bool depth_inverted)
{
	constexpr VkImageUsageFlags kGuideUsage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	constexpr VkImageUsageFlags kSharedUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; // cleared by the upscaler
	if (!CreateImage(context, render, kColorFormat, kGuideUsage, &stage->input) ||
	    !CreateImage(context, render, kMotionFormat, kGuideUsage, &stage->motion) ||
	    !CreateImage(context, render, kDepthFormat, kGuideUsage, &stage->depth) ||
	    !CreateImage(context, render, VK_FORMAT_R32_SFLOAT, kSharedUsage, &stage->dilated_depth) ||
	    !CreateImage(context, render, kMotionFormat, kSharedUsage, &stage->dilated_motion) ||
	    !CreateImage(context, render, VK_FORMAT_R32_UINT, kSharedUsage, &stage->previous_depth))
	{
		return false;
	}
	VulkanImageDescriptor descriptor {};
	descriptor.extent = {display.width, display.height, 1};
	descriptor.format = kColorFormat;
	// The present blit restores its source to COLOR_ATTACHMENT_OPTIMAL.
	descriptor.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (!VulkanCreateDeviceImage(context, VulkanBuildImageCreateInfo(descriptor), &stage->output, &stage->output_memory))
	{
		return false;
	}
	stage->output.extent = display;
	stage->output.format = kColorFormat;
	stage->output.layout = VK_IMAGE_LAYOUT_UNDEFINED;

	stage->device_context.vkDevice                                     = context->device;
	stage->device_context.vkPhysicalDevice                             = context->physical_device;
	stage->device_context.vkDeviceProcAddr                             = BackendDeviceProcAddr;
	stage->device_context.instanceFunctions.getPhysicalDeviceFeatures2 = vkGetPhysicalDeviceFeatures2;
	stage->device_context.instanceFunctions.enumerateDeviceExtensionProperties = vkEnumerateDeviceExtensionProperties;
	stage->device_context.instanceFunctions.getPhysicalDeviceMemoryProperties  = vkGetPhysicalDeviceMemoryProperties;
	stage->device_context.instanceFunctions.getPhysicalDeviceProperties2       = vkGetPhysicalDeviceProperties2;
	// The size query reads the instance functions that this call registers.
	const FfxDevice device = ffxGetDeviceVK(&stage->device_context);
	stage->scratch.resize(ffxGetScratchMemorySizeVK(context->physical_device, FFX_FSR3UPSCALER_CONTEXT_COUNT));

	FfxFsr3UpscalerContextDescription description {};
	description.flags          = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE | (depth_inverted ? FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED : 0u);
	description.maxRenderSize  = {render.width, render.height};
	description.maxUpscaleSize = {display.width, display.height};
	if (ffxGetInterfaceVK(&description.backendInterface, device, stage->scratch.data(),
	                      stage->scratch.size(), FFX_FSR3UPSCALER_CONTEXT_COUNT) != FFX_OK ||
	    ffxFsr3UpscalerContextCreate(&stage->context, &description) != FFX_OK)
	{
		return false;
	}
	stage->context_live   = true;
	stage->render_extent  = render;
	stage->display_extent = display;
	stage->depth_inverted = depth_inverted;
	stage->reset          = true;
	stage->shared_initialized = false;
	return true;
}

bool EnsureResources(GraphicContext* context, PresentationFsr3Stage* stage, VkExtent2D render, VkExtent2D display, bool depth_inverted)
{
	if (stage->context_live && SameExtent(stage->render_extent, render) && SameExtent(stage->display_extent, display) &&
	    stage->depth_inverted == depth_inverted)
	{
		return true;
	}
	DestroyResources(context, stage);
	if (CreateResources(context, stage, render, display, depth_inverted))
	{
		return true;
	}
	DestroyResources(context, stage);
	return false;
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageAspectFlags aspect, VkAccessFlags src_access, VkAccessFlags dst_access,
                                  VkImageLayout old_layout, VkImageLayout new_layout)
{
	VkImageMemoryBarrier barrier {};
	barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask       = src_access;
	barrier.dstAccessMask       = dst_access;
	barrier.oldLayout           = old_layout;
	barrier.newLayout           = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {aspect, 0, 1, 0, 1};
	return barrier;
}

VkImageAspectFlags DepthImageAspects(VkFormat format)
{
	return format == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
}

// Depth keeps its layout around the copy: the guest may keep using it in the next frame.
void CopyDepth(VkCommandBuffer cmd, const DepthStencilVulkanImage& source, const StageImage& target)
{
	const auto aspects = DepthImageAspects(source.format);
	const auto to_copy = ImageBarrier(source.image, aspects, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, source.layout,
	                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	const auto to_dst  = ImageBarrier(target.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	const VkImageMemoryBarrier before[2] = {to_copy, to_dst};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, before);
	VkImageCopy region {};
	region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
	region.dstSubresource = region.srcSubresource;
	region.extent         = {target.extent.width, target.extent.height, 1};
	vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	const auto back = ImageBarrier(source.image, aspects, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.layout);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
}

FfxResource GuideResource(VkImage image, VkExtent2D extent, FfxSurfaceFormat format, uint32_t usage, FfxResourceStates state,
                          const wchar_t* name)
{
	FfxResourceDescription description {};
	description.type     = FFX_RESOURCE_TYPE_TEXTURE2D;
	description.format   = format;
	description.width    = extent.width;
	description.height   = extent.height;
	description.depth    = 1;
	description.mipCount = 1;
	description.flags    = FFX_RESOURCE_FLAGS_NONE;
	description.usage    = static_cast<FfxResourceUsage>(usage);
	return ffxGetResourceVK(image, description, name, state);
}

FrameGuides TakeFrameGuides()
{
	std::lock_guard<std::mutex> lock(g_guides_mutex);
	FrameGuides                 guides = g_guides;
	g_guides                           = FrameGuides {};
	return guides;
}

} // namespace

bool PresentationFsr3Available()
{
	return true;
}

void PresentationFsr3NoteRenderPass(const RenderColorInfo& color, const RenderDepthInfo& depth)
{
	if (depth.vulkan_buffer == nullptr || depth.vaddr_num <= 0 || !IsGuideDepthFormat(depth.format) ||
	    depth.samples != VK_SAMPLE_COUNT_1_BIT)
	{
		return;
	}
	uint32_t                         targets = 0;
	const RenderColorAttachmentInfo* motion  = nullptr;
	for (const auto& attachment: color.attachment)
	{
		if (attachment.vulkan_buffer == nullptr)
		{
			continue;
		}
		targets++;
		// Geometry passes keep motion in the last half-float target.
		const bool half_float = attachment.vulkan_buffer->format == VK_FORMAT_R16G16_SFLOAT || attachment.vulkan_buffer->format == kColorFormat;
		if (half_float && attachment.samples == VK_SAMPLE_COUNT_1_BIT &&
		    SameExtent(attachment.vulkan_buffer->extent, depth.vulkan_buffer->extent))
		{
			motion = &attachment;
		}
	}
	if (motion == nullptr || targets < kGeometryPassMinTargets)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(g_guides_mutex);
	if (targets < g_guides.targets)
	{
		return;
	}
	g_guides.targets      = targets;
	g_guides.motion_vaddr = motion->base_addr;
	g_guides.motion_size  = motion->size;
	std::copy(std::begin(depth.vaddr), std::end(depth.vaddr), std::begin(g_guides.depth_vaddr));
	std::copy(std::begin(depth.size), std::end(depth.size), std::begin(g_guides.depth_size));
	g_guides.depth_vaddr_num = depth.vaddr_num;
	g_guides.depth_inverted  = depth.depth_compare_op == VK_COMPARE_OP_GREATER || depth.depth_compare_op == VK_COMPARE_OP_GREATER_OR_EQUAL;
}

VulkanImage* PresentationFsr3Record(CommandBuffer* command_buffer, GraphicContext* context, VulkanImage* source, VkExtent2D target_extent,
                                    uint32_t sharpness_percent, PresentationFsr3Stage** stage)
{
	EXIT_IF(command_buffer == nullptr || context == nullptr || source == nullptr || stage == nullptr);
	if (*stage == nullptr)
	{
		*stage = new PresentationFsr3Stage;
	}
	auto*      fsr    = *stage;
	const auto guides = TakeFrameGuides();
	const auto motion_objects = guides.targets != 0 ? GpuMemoryFindObjects(guides.motion_vaddr, guides.motion_size, GpuMemoryObjectType::RenderTexture, false, true)
	                                                : Vector<GpuMemoryObject> {};
	const auto depth_objects  = guides.targets != 0 ? GpuMemoryFindObjects(guides.depth_vaddr, guides.depth_size, guides.depth_vaddr_num,
	                                                                       GpuMemoryObjectType::DepthStencilBuffer, true, false)
	                                                : Vector<GpuMemoryObject> {};
	if (motion_objects.IsEmpty() || depth_objects.IsEmpty())
	{
		fsr->reset = true;
		return nullptr;
	}
	auto* motion = static_cast<RenderTextureVulkanImage*>(motion_objects.At(0).obj);
	auto* depth  = static_cast<DepthStencilVulkanImage*>(depth_objects.At(0).obj);
	// The presented image may be larger than the guides: the guest upscales its own frame before presenting it.
	const VkExtent2D grid = depth->extent;
	if (motion->layout == VK_IMAGE_LAYOUT_UNDEFINED || depth->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
	    !PresentationFsrApplies(grid, target_extent) ||
	    !EnsureResources(context, fsr, grid, target_extent, guides.depth_inverted))
	{
		fsr->reset = true;
		return nullptr;
	}

	UtilBlitImageTo(command_buffer, source, fsr->input.image, grid, VK_FILTER_LINEAR);
	UtilBlitImageTo(command_buffer, motion, fsr->motion.image, grid, VK_FILTER_NEAREST);
	auto* cmd = command_buffer->RecordOutsideRenderPass();
	CopyDepth(cmd, *depth, fsr->depth);

	const VkImageMemoryBarrier to_compute[4] = {
	    ImageBarrier(fsr->input.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
	    ImageBarrier(fsr->motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
	    ImageBarrier(fsr->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
	    ImageBarrier(fsr->output.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	                 VK_IMAGE_LAYOUT_GENERAL),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
	                     nullptr, 0, nullptr, 4, to_compute);

	if (!fsr->shared_initialized)
	{
		const VkImage shared[3] = {fsr->dilated_depth.image, fsr->dilated_motion.image, fsr->previous_depth.image};
		for (const VkImage image: shared)
		{
			const auto barrier = ImageBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			                                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		fsr->shared_initialized = true;
	}

	const auto now   = std::chrono::steady_clock::now();
	const auto delta = std::chrono::duration<float, std::milli>(now - fsr->last_present).count();
	fsr->last_present = now;

	const auto render  = fsr->render_extent;
	const auto display = fsr->display_extent;

	FfxFsr3UpscalerDispatchDescription dispatch {};
	dispatch.commandList = ffxGetCommandListVK(cmd);
	dispatch.color  = GuideResource(fsr->input.image, render, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_RESOURCE_USAGE_READ_ONLY,
	                                FFX_RESOURCE_STATE_COMPUTE_READ, L"KytyFsr3Color");
	dispatch.depth  = GuideResource(fsr->depth.image, render, FFX_SURFACE_FORMAT_R32_FLOAT,
	                                FFX_RESOURCE_USAGE_DEPTHTARGET,
	                                FFX_RESOURCE_STATE_COMPUTE_READ, L"KytyFsr3Depth");
	dispatch.motionVectors = GuideResource(fsr->motion.image, render, FFX_SURFACE_FORMAT_R16G16_FLOAT, FFX_RESOURCE_USAGE_READ_ONLY,
	                                       FFX_RESOURCE_STATE_COMPUTE_READ, L"KytyFsr3Motion");
	dispatch.output = GuideResource(fsr->output.image, display, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_RESOURCE_USAGE_UAV,
	                                FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"KytyFsr3Output");
	dispatch.dilatedDepth = GuideResource(fsr->dilated_depth.image, render, FFX_SURFACE_FORMAT_R32_FLOAT, FFX_RESOURCE_USAGE_UAV,
	                                      FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"KytyFsr3DilatedDepth");
	dispatch.dilatedMotionVectors = GuideResource(fsr->dilated_motion.image, render, FFX_SURFACE_FORMAT_R16G16_FLOAT, FFX_RESOURCE_USAGE_UAV,
	                                              FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"KytyFsr3DilatedMotion");
	dispatch.reconstructedPrevNearestDepth = GuideResource(fsr->previous_depth.image, render, FFX_SURFACE_FORMAT_R32_UINT, FFX_RESOURCE_USAGE_UAV,
	                                                       FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"KytyFsr3PreviousDepth");
	// The guest resolved its own jitter before the frame reaches presentation.
	dispatch.jitterOffset            = {0.0f, 0.0f};
	// Guest motion is normalized and points from the previous position to the current one.
	dispatch.motionVectorScale       = {-static_cast<float>(render.width), -static_cast<float>(render.height)};
	dispatch.renderSize              = {render.width, render.height};
	dispatch.upscaleSize             = {display.width, display.height};
	dispatch.enableSharpening        = sharpness_percent != 0;
	dispatch.sharpness               = static_cast<float>(std::min(sharpness_percent, 100u)) / 100.0f;
	dispatch.frameTimeDelta          = std::clamp(delta, 1.0f, 1000.0f);
	dispatch.preExposure             = 1.0f;
	dispatch.reset                   = fsr->reset;
	dispatch.cameraNear              = guides.depth_inverted ? kAssumedFar : kAssumedNear;
	dispatch.cameraFar               = guides.depth_inverted ? kAssumedNear : kAssumedFar;
	dispatch.cameraFovAngleVertical  = kAssumedFovRad;
	dispatch.viewSpaceToMetersFactor = 1.0f;
	if (ffxFsr3UpscalerContextDispatch(&fsr->context, &dispatch) != FFX_OK)
	{
		EXIT("FSR 3.1 dispatch was refused for render %ux%u and display %ux%u\n", render.width, render.height, display.width, display.height);
	}
	fsr->reset = false;

	const auto to_blit = ImageBarrier(fsr->output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	                                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_blit);
	fsr->output.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	return &fsr->output;
}

void PresentationFsr3Destroy(GraphicContext* context, PresentationFsr3Stage** stage)
{
	if (context == nullptr || stage == nullptr || *stage == nullptr)
	{
		return;
	}
	DestroyResources(context, *stage);
	delete *stage;
	*stage = nullptr;
}

} // namespace Kyty::Libs::Graphics

#else // KYTY_FSR3_ENABLED

namespace Kyty::Libs::Graphics {

bool PresentationFsr3Available()
{
	return false;
}

void PresentationFsr3NoteRenderPass(const RenderColorInfo& /*color*/, const RenderDepthInfo& /*depth*/) {}

VulkanImage* PresentationFsr3Record(CommandBuffer* /*command_buffer*/, GraphicContext* /*context*/, VulkanImage* /*source*/,
                                    VkExtent2D /*target_extent*/, uint32_t /*sharpness_percent*/, PresentationFsr3Stage** /*stage*/)
{
	return nullptr;
}

void PresentationFsr3Destroy(GraphicContext* /*context*/, PresentationFsr3Stage** /*stage*/) {}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_FSR3_ENABLED

#endif // KYTY_EMU_ENABLED
