#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Profiler.h"

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static void update_func(GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* /*obj*/, const uint64_t* /*vaddr*/,
                        const uint64_t* /*size*/, int /*vaddr_num*/)
{
	KYTY_PROFILER_BLOCK("DepthStencilBufferObject::update_func");
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("DepthStencilBufferObject::Create");

	EXIT_IF(size == nullptr || vaddr == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto pixel_format = static_cast<VkFormat>(params[DepthStencilBufferObject::PARAM_FORMAT]);
	auto guest_width  = params[DepthStencilBufferObject::PARAM_GUEST_WIDTH];
	auto guest_height = params[DepthStencilBufferObject::PARAM_GUEST_HEIGHT];
	auto host_width   = params[DepthStencilBufferObject::PARAM_HOST_WIDTH];
	auto host_height  = params[DepthStencilBufferObject::PARAM_HOST_HEIGHT];
	bool htile = params[DepthStencilBufferObject::PARAM_HTILE] != 0;
	const auto usage = params[DepthStencilBufferObject::PARAM_USAGE];
	bool sampled = (usage & 0x1u) != 0;
	bool sample_locations_compatible = (usage & 0x2u) != 0;
	auto samples = static_cast<VkSampleCountFlagBits>(params[DepthStencilBufferObject::PARAM_SAMPLES]);

	EXIT_NOT_IMPLEMENTED(pixel_format == VK_FORMAT_UNDEFINED);
	EXIT_NOT_IMPLEMENTED(guest_width == 0);
	EXIT_NOT_IMPLEMENTED(guest_height == 0);
	EXIT_NOT_IMPLEMENTED(host_width == 0);
	EXIT_NOT_IMPLEMENTED(host_height == 0);

	VulkanResolutionAttachmentRequest capability_request {};
	capability_request.extent       = {static_cast<uint32_t>(host_width), static_cast<uint32_t>(host_height)};
	capability_request.format       = pixel_format;
	capability_request.usage        = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
	capability_request.flags        = (sample_locations_compatible ? VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT : 0);
	capability_request.sample_count = samples;
	const auto capability            = EvaluateVulkanResolutionAttachment(ctx, capability_request);
	EXIT_NOT_IMPLEMENTED(capability.status != VulkanRenderResolutionCapabilityStatus::Success ||
	                     capability.decision.status != RenderResolutionImageCapabilityStatus::Supported);

	auto* vk_obj = new DepthStencilVulkanImage;

	vk_obj->SetNativeExtent(guest_width, guest_height);
	vk_obj->SetHostExtent(host_width, host_height);
	vk_obj->format     = pixel_format;
	vk_obj->image      = nullptr;
	vk_obj->layout     = VK_IMAGE_LAYOUT_UNDEFINED;
	vk_obj->samples    = samples;
	vk_obj->guest_size = *size;
	vk_obj->sample_locations_compatible = sample_locations_compatible;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	vk_obj->compressed = !htile;

	VulkanImageDescriptor image_descriptor {};
	image_descriptor.extent         = {vk_obj->extent.width, vk_obj->extent.height, 1};
	image_descriptor.format         = vk_obj->format;
	image_descriptor.flags          = capability_request.flags;
	image_descriptor.initial_layout = vk_obj->layout;
	image_descriptor.samples        = vk_obj->samples;
	image_descriptor.usage = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0));
	const auto image_info  = VulkanBuildImageCreateInfo(image_descriptor);
	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImage(ctx, image_info, vk_obj, mem));

	// EXIT_NOT_IMPLEMENTED(mem->requirements.size > *size);

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	VulkanImageViewDescriptor view_descriptor {};
	view_descriptor.image       = vk_obj->image;
	view_descriptor.format      = vk_obj->format;
	view_descriptor.aspect_mask = DepthFormatAspectMask(vk_obj->format);
	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]));

	view_descriptor.components  = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
	view_descriptor.aspect_mask = VK_IMAGE_ASPECT_DEPTH_BIT;
	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_DEPTH_TEXTURE]));

	if (DepthFormatHasStencil(vk_obj->format))
	{
		view_descriptor.components  = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		                               VK_COMPONENT_SWIZZLE_IDENTITY};
		view_descriptor.aspect_mask = VK_IMAGE_ASPECT_STENCIL_BIT;
		EXIT_NOT_IMPLEMENTED(
		    !VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_STENCIL_TEXTURE]));
	}

	// First bind of an HTILE depth target: pending Vulkan clear. Leave layout
	// UNDEFINED so FindRenderDepthInfo → loadOp CLEAR can discard+clear. Non-HTILE
	// still transitions to ATTACHMENT once.
	if (htile)
	{
		const uint64_t htile_addr = params[DepthStencilBufferObject::PARAM_HTILE_ADDR];
		if (htile_addr != 0)
		{
			DepthMetaMarkClear(htile_addr);
		}
	} else
	{
		UtilSetDepthLayoutOptimal(vk_obj);
	}

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	KYTY_PROFILER_BLOCK("DepthStencilBufferObject::delete_func");

	auto* vk_obj = reinterpret_cast<DepthStencilVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);

	DeleteFramebuffer(vk_obj);

	for (auto& view: vk_obj->image_view)
	{
		if (view != nullptr)
		{
			vkDestroyImageView(ctx->device, view, nullptr);
			view = nullptr;
		}
	}

	vkDestroyImage(ctx->device, vk_obj->image, nullptr);

	VulkanFree(ctx, mem);

	delete vk_obj;
}

bool DepthStencilBufferObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_GUEST_WIDTH] == other[PARAM_GUEST_WIDTH] &&
	        params[PARAM_GUEST_HEIGHT] == other[PARAM_GUEST_HEIGHT] && params[PARAM_HTILE] == other[PARAM_HTILE] &&
	        params[PARAM_NEO] == other[PARAM_NEO] && (params[PARAM_USAGE] & 0x1u) == (other[PARAM_USAGE] & 0x1u) &&
	        params[PARAM_HTILE_ADDR] == other[PARAM_HTILE_ADDR] && params[PARAM_HTILE_SIZE] == other[PARAM_HTILE_SIZE] &&
	        params[PARAM_HOST_WIDTH] == other[PARAM_HOST_WIDTH] && params[PARAM_HOST_HEIGHT] == other[PARAM_HOST_HEIGHT] &&
	        params[PARAM_SAMPLES] == other[PARAM_SAMPLES]);
}

GpuObject::create_func_t DepthStencilBufferObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::delete_func_t DepthStencilBufferObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t DepthStencilBufferObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
