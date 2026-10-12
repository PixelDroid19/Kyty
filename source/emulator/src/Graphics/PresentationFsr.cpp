#include "Emulator/Graphics/PresentationFsr.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/PresentationScaler.h"
#include "Emulator/Graphics/Utils.h"

#include <algorithm>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

#include "PresentationFsrShaders.inc"

// The passes filter and store in half float: storage support is mandatory for it, and it holds either side of the
// sRGB transfer without banding.
constexpr VkFormat kIntermediateFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kGroupSize          = 8;
constexpr uint32_t kFlagSourceLinear   = 1u;
constexpr uint32_t kFlagTargetLinear   = 2u;

// Matches the push-constant block of PresentationFsrCommon.glsl.
struct FsrParameters
{
	float    source_size[2];
	float    texture_size[2];
	float    target_size[2];
	float    sharpness;
	uint32_t flags;
};

struct FsrImage
{
	VkImage      image = nullptr;
	VkImageView  view  = nullptr;
	VulkanMemory memory;
	VkExtent2D   extent {};
};

enum FsrPass : uint32_t
{
	kPassEasu,
	kPassRcas,
	kPassCount,
};

} // namespace

// Pipelines for the device, and images for the current source and target extents.
struct PresentationFsrStage
{
	VkSampler             sampler         = nullptr;
	VkDescriptorSetLayout set_layout      = nullptr;
	VkPipelineLayout      pipeline_layout = nullptr;
	VkDescriptorPool      descriptor_pool = nullptr;
	VkPipeline            pipelines[kPassCount] {};
	VkDescriptorSet       sets[kPassCount] {};
	FsrImage              input;    // The source converted to half float, at the source extent.
	FsrImage              upscaled; // EASU output at the target extent.
	FsrImage              output;   // RCAS output at the target extent.
};

namespace {

bool QueueSupportsCompute(const GraphicContext* context, int queue_index)
{
	const uint32_t family = context->queues[queue_index].family;
	uint32_t       count  = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(context->physical_device, &count, nullptr);
	if (family >= count)
	{
		return false;
	}
	std::vector<VkQueueFamilyProperties> properties(count);
	vkGetPhysicalDeviceQueueFamilyProperties(context->physical_device, &count, properties.data());
	return (properties[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
}

bool CreatePipeline(VkDevice device, VkPipelineLayout layout, const uint32_t* words, size_t size, VkPipeline* pipeline)
{
	VkShaderModuleCreateInfo module_info {};
	module_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = size;
	module_info.pCode    = words;
	VkShaderModule module = nullptr;
	if (vkCreateShaderModule(device, &module_info, nullptr, &module) != VK_SUCCESS)
	{
		return false;
	}
	VkComputePipelineCreateInfo pipeline_info {};
	pipeline_info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline_info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName  = "main";
	pipeline_info.layout       = layout;
	// The renderer synchronizes its pipeline cache on its own; these two pipelines are built once without it.
	const bool created = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, pipeline) == VK_SUCCESS;
	vkDestroyShaderModule(device, module, nullptr);
	return created;
}

bool CreatePipelines(GraphicContext* context, PresentationFsrStage* stage)
{
	VkSamplerCreateInfo sampler_info {};
	sampler_info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter    = VK_FILTER_LINEAR;
	sampler_info.minFilter    = VK_FILTER_LINEAR;
	sampler_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(context->device, &sampler_info, nullptr, &stage->sampler) != VK_SUCCESS)
	{
		return false;
	}

	VkDescriptorSetLayoutBinding bindings[2] {};
	bindings[0].binding         = 0;
	bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding         = 1;
	bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo set_layout_info {};
	set_layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout_info.bindingCount = 2;
	set_layout_info.pBindings    = bindings;
	if (vkCreateDescriptorSetLayout(context->device, &set_layout_info, nullptr, &stage->set_layout) != VK_SUCCESS)
	{
		return false;
	}

	VkPushConstantRange push_range {};
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.size       = sizeof(FsrParameters);
	VkPipelineLayoutCreateInfo layout_info {};
	layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &stage->set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	if (vkCreatePipelineLayout(context->device, &layout_info, nullptr, &stage->pipeline_layout) != VK_SUCCESS)
	{
		return false;
	}
	if (!CreatePipeline(context->device, stage->pipeline_layout, kPresentationFsrEasu, sizeof(kPresentationFsrEasu),
	                    &stage->pipelines[kPassEasu]) ||
	    !CreatePipeline(context->device, stage->pipeline_layout, kPresentationFsrRcas, sizeof(kPresentationFsrRcas),
	                    &stage->pipelines[kPassRcas]))
	{
		return false;
	}

	VkDescriptorPoolSize pool_sizes[2] {};
	pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_sizes[0].descriptorCount = kPassCount;
	pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	pool_sizes[1].descriptorCount = kPassCount;
	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets       = kPassCount;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes    = pool_sizes;
	if (vkCreateDescriptorPool(context->device, &pool_info, nullptr, &stage->descriptor_pool) != VK_SUCCESS)
	{
		return false;
	}
	const VkDescriptorSetLayout layouts[kPassCount] = {stage->set_layout, stage->set_layout};
	VkDescriptorSetAllocateInfo set_info {};
	set_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool     = stage->descriptor_pool;
	set_info.descriptorSetCount = kPassCount;
	set_info.pSetLayouts        = layouts;
	return vkAllocateDescriptorSets(context->device, &set_info, stage->sets) == VK_SUCCESS;
}

void DestroyImage(GraphicContext* context, FsrImage* image)
{
	if (image->view != nullptr)
	{
		vkDestroyImageView(context->device, image->view, nullptr);
	}
	if (image->image != nullptr)
	{
		vkDestroyImage(context->device, image->image, nullptr);
		VulkanFree(context, &image->memory);
	}
	*image = FsrImage {};
}

bool CreateImage(GraphicContext* context, VkExtent2D extent, VkImageUsageFlags usage, FsrImage* image)
{
	VulkanImageDescriptor descriptor {};
	descriptor.extent = {extent.width, extent.height, 1};
	descriptor.format = kIntermediateFormat;
	descriptor.usage  = usage | VK_IMAGE_USAGE_SAMPLED_BIT;
	VulkanImage created(VulkanImageType::Unknown);
	if (!VulkanCreateDeviceImage(context, VulkanBuildImageCreateInfo(descriptor), &created, &image->memory))
	{
		return false;
	}
	image->image  = created.image;
	image->extent = extent;
	VulkanImageViewDescriptor view {};
	view.image  = image->image;
	view.format = kIntermediateFormat;
	return VulkanCreateDeviceImageView(context->device, view, &image->view);
}

bool EnsureImage(GraphicContext* context, VkExtent2D extent, VkImageUsageFlags usage, FsrImage* image, bool* recreated)
{
	if (image->image != nullptr && image->extent.width == extent.width && image->extent.height == extent.height)
	{
		return true;
	}
	DestroyImage(context, image);
	*recreated = true;
	return CreateImage(context, extent, usage, image);
}

void WriteSet(VkDevice device, VkSampler sampler, VkDescriptorSet set, const FsrImage& source, const FsrImage& target)
{
	VkDescriptorImageInfo infos[2] {};
	infos[0].sampler     = sampler;
	infos[0].imageView   = source.view;
	infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	infos[1].imageView   = target.view;
	infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	VkWriteDescriptorSet writes[2] {};
	for (uint32_t i = 0; i < 2; i++)
	{
		writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet          = set;
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = (i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writes[i].pImageInfo      = &infos[i];
	}
	vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}

// Resizes the images to the extents of this present and points the descriptor sets at them.
bool EnsureImages(GraphicContext* context, PresentationFsrStage* stage, VkExtent2D source_extent, VkExtent2D target_extent)
{
	bool recreated = false;
	if (!EnsureImage(context, source_extent, VK_IMAGE_USAGE_TRANSFER_DST_BIT, &stage->input, &recreated) ||
	    !EnsureImage(context, target_extent, VK_IMAGE_USAGE_STORAGE_BIT, &stage->upscaled, &recreated) ||
	    !EnsureImage(context, target_extent, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &stage->output, &recreated))
	{
		return false;
	}
	if (recreated)
	{
		WriteSet(context->device, stage->sampler, stage->sets[kPassEasu], stage->input, stage->upscaled);
		WriteSet(context->device, stage->sampler, stage->sets[kPassRcas], stage->upscaled, stage->output);
	}
	return true;
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access, VkImageLayout old_layout,
                                  VkImageLayout new_layout)
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
	barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	return barrier;
}

void Dispatch(VkCommandBuffer cmd, const PresentationFsrStage* stage, FsrPass pass, const FsrParameters& parameters, VkExtent2D extent)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, stage->pipelines[pass]);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, stage->pipeline_layout, 0, 1, &stage->sets[pass], 0, nullptr);
	vkCmdPushConstants(cmd, stage->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
	vkCmdDispatch(cmd, (extent.width + kGroupSize - 1) / kGroupSize, (extent.height + kGroupSize - 1) / kGroupSize, 1);
}

void RecordPasses(VkCommandBuffer cmd, const PresentationFsrStage* stage, const FsrParameters& parameters, VkImage target,
                  VkExtent2D target_extent)
{
	VkImageMemoryBarrier to_easu[2] = {
	    ImageBarrier(stage->input.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
	    ImageBarrier(stage->upscaled.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2,
	                     to_easu);
	Dispatch(cmd, stage, kPassEasu, parameters, target_extent);

	VkImageMemoryBarrier to_rcas[2] = {
	    ImageBarrier(stage->upscaled.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
	                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
	    ImageBarrier(stage->output.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2,
	                     to_rcas);
	FsrParameters rcas = parameters;
	rcas.texture_size[0] = static_cast<float>(target_extent.width);
	rcas.texture_size[1] = static_cast<float>(target_extent.height);
	Dispatch(cmd, stage, kPassRcas, rcas, target_extent);

	VkImageMemoryBarrier to_copy[2] = {
	    ImageBarrier(stage->output.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
	                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
	    ImageBarrier(target, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
	                     to_copy);
	// Same extent: the blit only converts half float to the target format, encoding when the target is sRGB.
	VkImageBlit region {};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.srcOffsets[1]  = {static_cast<int32_t>(target_extent.width), static_cast<int32_t>(target_extent.height), 1};
	region.dstSubresource = region.srcSubresource;
	region.dstOffsets[1]  = region.srcOffsets[1];
	vkCmdBlitImage(cmd, stage->output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
	               VK_FILTER_NEAREST);
}

bool IsSrgbTarget(VkFormat format)
{
	return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

} // namespace

bool PresentationFsrApplies(VkExtent2D source, VkExtent2D target)
{
	const bool fits    = source.width > 0 && source.height > 0 && source.width <= target.width && source.height <= target.height;
	const bool smaller = source.width < target.width || source.height < target.height;
	return fits && smaller;
}

float PresentationFsrSharpnessStops(uint32_t percent)
{
	return static_cast<float>(100u - std::min(percent, 100u)) / 50.0f;
}

bool PresentationFsrRecord(CommandBuffer* command_buffer, GraphicContext* context, VulkanImage* source, VkImage target,
                           VkExtent2D target_extent, VkFormat target_format, uint32_t sharpness_percent, PresentationFsrStage** stage)
{
	if (command_buffer == nullptr || context == nullptr || source == nullptr || source->image == nullptr || target == nullptr ||
	    stage == nullptr || !PresentationFsrApplies(source->extent, target_extent))
	{
		return false;
	}
	if (*stage == nullptr)
	{
		if (!QueueSupportsCompute(context, command_buffer->GetQueueIndex()))
		{
			return false;
		}
		*stage = new PresentationFsrStage;
		if (!CreatePipelines(context, *stage))
		{
			PresentationFsrDestroy(context, stage);
			return false;
		}
	}
	auto* fsr = *stage;
	if (!EnsureImages(context, fsr, source->extent, target_extent))
	{
		PresentationFsrDestroy(context, stage);
		return false;
	}

	// Same extent: the blit only converts the source to half float, decoding an sRGB source to linear light.
	UtilBlitImageTo(command_buffer, source, fsr->input.image, source->extent, VK_FILTER_NEAREST);

	FsrParameters parameters {};
	parameters.source_size[0]  = static_cast<float>(source->extent.width);
	parameters.source_size[1]  = static_cast<float>(source->extent.height);
	parameters.texture_size[0] = parameters.source_size[0];
	parameters.texture_size[1] = parameters.source_size[1];
	parameters.target_size[0]  = static_cast<float>(target_extent.width);
	parameters.target_size[1]  = static_cast<float>(target_extent.height);
	parameters.sharpness       = PresentationFsrSharpnessStops(sharpness_percent);
	parameters.flags           = (PresentationSourceHoldsLinearLight(source->format) ? kFlagSourceLinear : 0u) |
	                   (IsSrgbTarget(target_format) ? kFlagTargetLinear : 0u);
	RecordPasses(command_buffer->RecordOutsideRenderPass(), fsr, parameters, target, target_extent);
	return true;
}

void PresentationFsrDestroy(GraphicContext* context, PresentationFsrStage** stage)
{
	if (context == nullptr || stage == nullptr || *stage == nullptr)
	{
		return;
	}
	auto* fsr = *stage;
	DestroyImage(context, &fsr->input);
	DestroyImage(context, &fsr->upscaled);
	DestroyImage(context, &fsr->output);
	for (auto* pipeline: fsr->pipelines)
	{
		vkDestroyPipeline(context->device, pipeline, nullptr);
	}
	vkDestroyDescriptorPool(context->device, fsr->descriptor_pool, nullptr);
	vkDestroyPipelineLayout(context->device, fsr->pipeline_layout, nullptr);
	vkDestroyDescriptorSetLayout(context->device, fsr->set_layout, nullptr);
	vkDestroySampler(context->device, fsr->sampler, nullptr);
	delete fsr;
	*stage = nullptr;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
