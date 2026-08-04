#include "Emulator/Graphics/GraphicsRender.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VulkanRenderResolutionCapability.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Profiler.h"
#include "Emulator/Log.h"

#include <atomic>
#include <cstdio>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// FramebufferCache + SamplerCache::GetSampler

VulkanFramebuffer* FramebufferCache::CreateFramebuffer(RenderColorInfo* color, RenderDepthInfo* depth,
                                                        DepthStencilAttachmentAccess depth_stencil_access)
{
	KYTY_PROFILER_FUNCTION();

	static std::atomic<uint64_t> seq = 0;

	Core::LockGuard lock(m_mutex);

	EXIT_IF(color == nullptr);
	EXIT_IF(depth == nullptr);
	EXIT_IF(g_render_ctx == nullptr);

	bool       with_depth              = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	bool       with_color              = (RenderColorHasActiveTarget(*color) && RenderColorFirstActiveImage(*color) != nullptr);
	// Guest depth size registers of 0 resolve to 1x1. Pairing that D/S image with a
	// full-size color RT collapses the framebuffer via min() and leaves scissor/
	// viewport outside the FB — observed as VK_ERROR_DEVICE_LOST on host GPUs.
	// Detach the undersized depth attachment; depth test without a valid surface
	// is disabled at the pipeline when with_depth is false.
	if (with_depth && with_color)
	{
		auto* color_img = RenderColorFirstActiveImage(*color);
		if (color_img != nullptr)
		{
			const auto dw = depth->vulkan_buffer->extent.width;
			const auto dh = depth->vulkan_buffer->extent.height;
			const auto cw = color_img->extent.width;
			const auto ch = color_img->extent.height;
			if ((dw < cw || dh < ch) && (dw <= 1u || dh <= 1u))
			{
				KYTY_LOG_DEBUG(
				             "KYTY_GRAPHICS: detaching undersized depth %ux%u from color %ux%u (guest depth size likely unset)\n", dw, dh,
				             cw, ch);
				with_depth = false;
			}
		}
	}
	const bool depth_stencil_read_only = (with_depth && depth_stencil_access == DepthStencilAttachmentAccess::ReadOnly);
	EXIT_NOT_IMPLEMENTED(depth_stencil_read_only && (depth->depth_clear_enable || depth->stencil_clear_enable));
	const auto attachment_samples = resolve_render_attachment_sample_count(*color, *depth);

	for (auto& f: m_framebuffers)
	{
		bool same_colors = (f.targets_num == color->targets_num);
		for (uint32_t slot = 0; same_colors && slot < color->targets_num; slot++)
		{
			if (!RenderColorSlotActive(*color, slot))
			{
				same_colors = (f.image_id[slot] == 0);
				continue;
			}
			const auto& attachment = color->attachment[slot];
			auto*       image      = attachment.vulkan_buffer;
			const auto  load_ops   = ResolveColorAttachmentLoadOps(image->layout, attachment.cmask_fast_clear_enable,
			                                                      attachment.clear_word0, attachment.clear_word1, image->format);
			same_colors         = (f.image_id[slot] == image->memory.unique_id) && (f.color_load_op[slot] == load_ops.load_op) &&
			              (f.color_initial_layout[slot] == load_ops.initial_layout);
		}
		if (f.framebuffer != nullptr && same_colors && f.depth_id == (with_depth ? depth->vulkan_buffer->memory.unique_id : 0) &&
		    f.depth_clear_enable == depth->depth_clear_enable && f.stencil_clear_enable == depth->stencil_clear_enable &&
		    f.depth_stencil_read_only == depth_stencil_read_only)
		{
			return f.framebuffer;
		}
	}

	auto* framebuffer        = new VulkanFramebuffer;
	framebuffer->render_pass = nullptr;
	framebuffer->framebuffer = nullptr;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	EXIT_NOT_IMPLEMENTED(!with_depth && !with_color);

	VulkanImage* vulkan_buffer =
	    (with_color ? RenderColorFirstActiveImage(*color)
	                : CreateDummyBuffer(VK_FORMAT_B8G8R8A8_SRGB, depth->vulkan_buffer->extent.width, depth->vulkan_buffer->extent.height,
	                                    attachment_samples));
	VkExtent2D framebuffer_extent {vulkan_buffer->extent.width, vulkan_buffer->extent.height};
	const uint32_t color_count      = (with_color ? color->targets_num : 1);
	uint32_t       attachment_count = 0;

	VkAttachmentDescription attachments[RenderColorInfo::TARGETS_MAX + 1]       = {};
	VkAttachmentReference   color_attachment_refs[RenderColorInfo::TARGETS_MAX] = {};
	VkImageView             views[RenderColorInfo::TARGETS_MAX + 1]             = {};
	for (uint32_t slot = 0; slot < color_count; slot++)
	{
		if (with_color && !RenderColorSlotActive(*color, slot))
		{
			color_attachment_refs[slot] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
			continue;
		}
		const auto* color_attachment = with_color ? &color->attachment[slot] : nullptr;
		auto*       image            = with_color ? color_attachment->vulkan_buffer : vulkan_buffer;
		EXIT_NOT_IMPLEMENTED(image->samples != attachment_samples);
		framebuffer_extent = IntersectFramebufferAttachmentExtent(framebuffer_extent, image);
		const auto load_ops       = ResolveColorAttachmentLoadOps(image->layout, with_color ? color_attachment->cmask_fast_clear_enable : false,
		                                                          with_color ? color_attachment->clear_word0 : 0u,
		                                                          with_color ? color_attachment->clear_word1 : 0u, image->format);
		attachments[attachment_count].flags   = 0;
		attachments[attachment_count].format  = image->format;
		attachments[attachment_count].samples = image->samples;
		attachments[attachment_count].loadOp  = load_ops.load_op;
		attachments[attachment_count].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[attachment_count].stencilLoadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[attachment_count].stencilStoreOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[attachment_count].initialLayout         = load_ops.initial_layout;
		attachments[attachment_count].finalLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color_attachment_refs[slot]                         = {attachment_count, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		views[attachment_count]                             = image->image_view[VulkanImage::VIEW_DEFAULT];
		framebuffer->color_load_op[slot]        = load_ops.load_op;
		framebuffer->color_initial_layout[slot] = load_ops.initial_layout;
		attachment_count++;
	}
	framebuffer->color_count            = color_count;
	framebuffer->attachment_count       = attachment_count + (with_depth ? 1u : 0u);
	framebuffer->depth_attachment_index = with_depth ? attachment_count : VK_ATTACHMENT_UNUSED;

	const auto depth_load_ops = ResolveDepthAttachmentLoadOps(depth->format, depth->depth_clear_enable, depth->stencil_clear_enable);
	const auto depth_stencil_layout =
	    (depth_stencil_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	attachments[attachment_count].flags   = 0;
	attachments[attachment_count].format  = depth->format;
	attachments[attachment_count].samples = with_depth ? depth->vulkan_buffer->samples : VK_SAMPLE_COUNT_1_BIT;
	attachments[attachment_count].loadOp  = depth_load_ops.depth_load;
	attachments[attachment_count].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[attachment_count].stencilLoadOp  = depth_load_ops.stencil_load;
	attachments[attachment_count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[attachment_count].initialLayout  = (depth_stencil_read_only ? depth_stencil_layout : depth_load_ops.initial_layout);
	attachments[attachment_count].finalLayout    = depth_stencil_layout;

	VkAttachmentReference depth_attachment_ref {};
	depth_attachment_ref.attachment = attachment_count;
	depth_attachment_ref.layout     = depth_stencil_layout;
	framebuffer->depth_stencil_layout = depth_stencil_layout;

	VkSubpassDescription subpass {};
	subpass.flags                   = 0;
	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.inputAttachmentCount    = 0;
	subpass.pInputAttachments       = nullptr;
	subpass.colorAttachmentCount    = color_count;
	subpass.pColorAttachments       = color_attachment_refs;
	subpass.pResolveAttachments     = nullptr;
	subpass.pDepthStencilAttachment = (with_depth ? &depth_attachment_ref : nullptr);
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments    = nullptr;

	VkRenderPassCreateInfo render_pass_info {};
	render_pass_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_info.pNext           = nullptr;
	render_pass_info.flags           = 0;
	render_pass_info.attachmentCount = attachment_count + (with_depth ? 1u : 0u);
	render_pass_info.pAttachments    = attachments;
	render_pass_info.subpassCount    = 1;
	render_pass_info.pSubpasses      = &subpass;
	render_pass_info.dependencyCount = 0;
	render_pass_info.pDependencies   = nullptr;

	vkCreateRenderPass(gctx->device, &render_pass_info, nullptr, &framebuffer->render_pass);

	framebuffer->render_pass_id = ++seq;

	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	if (with_depth)
	{
		EXIT_NOT_IMPLEMENTED(depth->vulkan_buffer->samples != attachment_samples);
		framebuffer_extent = IntersectFramebufferAttachmentExtent(framebuffer_extent, depth->vulkan_buffer);
		views[attachment_count] = depth->vulkan_buffer->image_view[VulkanImage::VIEW_DEFAULT];
	}

	VkFramebufferCreateInfo framebuffer_info {};
	framebuffer_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.pNext           = nullptr;
	framebuffer_info.flags           = 0;
	framebuffer_info.renderPass      = framebuffer->render_pass;
	framebuffer_info.attachmentCount = attachment_count + (with_depth ? 1u : 0u);
	framebuffer_info.pAttachments    = views;
	EXIT_NOT_IMPLEMENTED(framebuffer_extent.width == 0 || framebuffer_extent.height == 0);
	framebuffer->extent.width  = framebuffer_extent.width;
	framebuffer->extent.height = framebuffer_extent.height;
	framebuffer_info.width     = framebuffer->extent.width;
	framebuffer_info.height    = framebuffer->extent.height;
	framebuffer_info.layers = 1;

	vkCreateFramebuffer(gctx->device, &framebuffer_info, nullptr, &framebuffer->framebuffer);

	EXIT_NOT_IMPLEMENTED(framebuffer->framebuffer == nullptr);

	Framebuffer fnew;
	fnew.framebuffer = framebuffer;
	fnew.targets_num = color->targets_num;
	for (uint32_t slot = 0; slot < color->targets_num; slot++)
	{
		fnew.image_id[slot] = RenderColorSlotActive(*color, slot) ? color->attachment[slot].vulkan_buffer->memory.unique_id : 0;
	}
	fnew.depth_id             = (with_depth ? depth->vulkan_buffer->memory.unique_id : 0);
	fnew.depth_clear_enable   = depth->depth_clear_enable;
	fnew.stencil_clear_enable = depth->stencil_clear_enable;
	fnew.depth_stencil_read_only = depth_stencil_read_only;
	for (uint32_t slot = 0; slot < color->targets_num; slot++)
	{
		fnew.color_load_op[slot]        = framebuffer->color_load_op[slot];
		fnew.color_initial_layout[slot] = framebuffer->color_initial_layout[slot];
	}

	bool updated = false;

	for (auto& f: m_framebuffers)
	{
		if (f.framebuffer == nullptr)
		{
			f       = fnew;
			updated = true;
			break;
		}
	}

	if (!updated)
	{
		m_framebuffers.Add(fnew);
	}

	return framebuffer;
}

void FramebufferCache::FreeFramebufferByColor(VulkanImage* image)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(image == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& f: m_framebuffers)
	{
		if (f.framebuffer == nullptr)
		{
			continue;
		}
		bool contains_image = false;
		for (uint32_t slot = 0; slot < f.targets_num; slot++)
		{
			contains_image = contains_image || f.image_id[slot] == image->memory.unique_id;
		}
		if (!contains_image)
		{
			continue;
		}

		g_render_ctx->GetPipelineCache()->DeletePipelines(f.framebuffer);
		g_render_ctx->ReleaseDepthStencilCopyRenderPass(f.framebuffer->render_pass_id);

		auto* gctx = g_render_ctx->GetGraphicCtx();

		EXIT_IF(gctx == nullptr);

		vkDestroyFramebuffer(gctx->device, f.framebuffer->framebuffer, nullptr);

		vkDestroyRenderPass(gctx->device, f.framebuffer->render_pass, nullptr);

		delete f.framebuffer;

		f.framebuffer = nullptr;

		break;
	}
}

void FramebufferCache::FreeFramebufferByDepth(DepthStencilVulkanImage* image)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(image == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& f: m_framebuffers)
	{
		if (f.framebuffer != nullptr && f.depth_id == image->memory.unique_id)
		{
			g_render_ctx->GetPipelineCache()->DeletePipelines(f.framebuffer);
			g_render_ctx->ReleaseDepthStencilCopyRenderPass(f.framebuffer->render_pass_id);

			auto* gctx = g_render_ctx->GetGraphicCtx();

			EXIT_IF(gctx == nullptr);

			vkDestroyFramebuffer(gctx->device, f.framebuffer->framebuffer, nullptr);

			vkDestroyRenderPass(gctx->device, f.framebuffer->render_pass, nullptr);

			delete f.framebuffer;

			f.framebuffer = nullptr;

			break;
		}
	}
}

VideoOutVulkanImage* FramebufferCache::CreateDummyBuffer(VkFormat format, uint32_t width, uint32_t height, VkSampleCountFlagBits samples)
{
	for (auto* b: m_dummy_buffers)
	{
		if (b->extent.width == width && b->extent.height == height && b->format == format && b->samples == samples)
		{
			return b;
		}
	}

	auto* ctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(ctx == nullptr);

	auto* vk_obj = new VideoOutVulkanImage;

	vk_obj->SetNativeExtent(width, height);
	vk_obj->format = format;
	vk_obj->image  = nullptr;
	vk_obj->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	vk_obj->samples = samples;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	VulkanImageDescriptor image_descriptor {};
	image_descriptor.extent         = {vk_obj->extent.width, vk_obj->extent.height, 1};
	image_descriptor.format         = vk_obj->format;
	image_descriptor.initial_layout = vk_obj->layout;
	image_descriptor.usage          = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	image_descriptor.samples        = vk_obj->samples;
	const auto image_info           = VulkanBuildImageCreateInfo(image_descriptor);
	vk_obj->host_extent_selected    = true;

	VulkanMemory mem;
	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImage(ctx, image_info, vk_obj, &mem));

	VulkanImageViewDescriptor view_descriptor {};
	view_descriptor.image  = vk_obj->image;
	view_descriptor.format = vk_obj->format;
	EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImageView(ctx->device, view_descriptor, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]));

	UtilSetImageLayoutOptimal(vk_obj);

	m_dummy_buffers.Add(vk_obj);

	return vk_obj;
}

VkSampler SamplerCache::GetSampler(uint64_t id)
{
	Core::LockGuard lock(m_mutex);
	if (id < m_samplers.Size())
	{
		return m_samplers.At(id).vk;
	}
	return nullptr;
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
