#include "Emulator/Graphics/DebugOverlay.h"

#ifdef KYTY_EMU_ENABLED

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicContext.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

#include "SDL.h"
#include "SDL_events.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Kyty::Libs::Graphics {

namespace {

bool               g_initialized = false;
bool               g_visible     = true;
VkRenderPass       g_render_pass = VK_NULL_HANDLE;
std::vector<VkFramebuffer> g_framebuffers;
VkFormat           g_swapchain_format = VK_FORMAT_UNDEFINED;
uint32_t           g_image_count      = 0;

[[nodiscard]] bool EnvHudEnabledByDefault()
{
	const char* v = std::getenv("KYTY_HUD");
	if (v == nullptr)
	{
		return true;
	}
	return !(v[0] == '0' && v[1] == '\0');
}

void DestroyFramebuffers(VkDevice device)
{
	for (VkFramebuffer fb: g_framebuffers)
	{
		if (fb != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(device, fb, nullptr);
		}
	}
	g_framebuffers.clear();
}

bool CreateRenderPass(VkDevice device, VkFormat format)
{
	VkAttachmentDescription attachment {};
	attachment.format         = format;
	attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachment.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference color_ref {};
	color_ref.attachment = 0;
	color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass {};
	subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments    = &color_ref;

	VkSubpassDependency dependency {};
	dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass    = 0;
	dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo rp_info {};
	rp_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp_info.attachmentCount = 1;
	rp_info.pAttachments    = &attachment;
	rp_info.subpassCount    = 1;
	rp_info.pSubpasses      = &subpass;
	rp_info.dependencyCount = 1;
	rp_info.pDependencies   = &dependency;

	const VkResult result = vkCreateRenderPass(device, &rp_info, nullptr, &g_render_pass);
	return result == VK_SUCCESS && g_render_pass != VK_NULL_HANDLE;
}

bool CreateFramebuffers(VkDevice device, VulkanSwapchain* swapchain)
{
	DestroyFramebuffers(device);
	g_framebuffers.resize(swapchain->swapchain_images_count);
	for (uint32_t i = 0; i < swapchain->swapchain_images_count; i++)
	{
		VkFramebufferCreateInfo fb_info {};
		fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fb_info.renderPass      = g_render_pass;
		fb_info.attachmentCount = 1;
		fb_info.pAttachments    = &swapchain->swapchain_image_views[i];
		fb_info.width           = swapchain->swapchain_extent.width;
		fb_info.height          = swapchain->swapchain_extent.height;
		fb_info.layers          = 1;
		const VkResult result   = vkCreateFramebuffer(device, &fb_info, nullptr, &g_framebuffers[i]);
		if (result != VK_SUCCESS)
		{
			DestroyFramebuffers(device);
			return false;
		}
	}
	g_image_count       = swapchain->swapchain_images_count;
	g_swapchain_format  = swapchain->swapchain_format;
	return true;
}

void DrawHud(const DebugStatsSnapshot& snap)
{
	ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.55f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 1.0f, 0.35f, 1.0f));

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
	                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

	if (ImGui::Begin("##kyty_debug_hud", nullptr, flags))
	{
		ImGui::Text("FPS %.1f  FLIP %.1f  %.1f MS", snap.fps, snap.flip_per_sec, snap.frame_time_ms);
		ImGui::Text("DRAWS %.0f/S  %.0f/F  DISP %.0f/S", snap.draws_per_sec, snap.draws_per_frame, snap.dispatches_per_sec);
		ImGui::Text("ALLOC %.1f MB/S  GC %" PRIu64 "/%" PRIu64 "/%" PRIu64, snap.alloc_mib_per_sec, snap.creates_window, snap.frees_window,
		            snap.live_objects);
		ImGui::Text("CPU %.0f%%  HEAP %.0f MB  F1 HIDE", snap.cpu_percent, snap.heap_mib);
		// Guest display buffer (blit source) vs host swapchain — black UI with
		// nonzero flips often means VO content is empty/alpha-0, not present fail.
		if (snap.present_src_w != 0 && snap.present_src_h != 0)
		{
			ImGui::Text("VO %" PRIu32 "x%" PRIu32 " -> SW %" PRIu32 "x%" PRIu32 " lay=%" PRIu32, snap.present_src_w, snap.present_src_h,
			            snap.present_dst_w, snap.present_dst_h, snap.present_src_layout);
		}
	}
	ImGui::End();

	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}

} // namespace

void DebugOverlayInit(SDL_Window* window, GraphicContext* ctx, VulkanSwapchain* swapchain)
{
	EXIT_IF(window == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(swapchain == nullptr);
	EXIT_IF(g_initialized);

	g_visible = EnvHudEnabledByDefault();
	DebugStatsInit();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;

	ImGui::StyleColorsDark();

	if (!CreateRenderPass(ctx->device, swapchain->swapchain_format))
	{
		std::fprintf(stderr, "DebugOverlay: failed to create render pass\n");
		ImGui::DestroyContext();
		return;
	}
	if (!CreateFramebuffers(ctx->device, swapchain))
	{
		std::fprintf(stderr, "DebugOverlay: failed to create framebuffers\n");
		vkDestroyRenderPass(ctx->device, g_render_pass, nullptr);
		g_render_pass = VK_NULL_HANDLE;
		ImGui::DestroyContext();
		return;
	}

	ImGui_ImplSDL2_InitForVulkan(window);

	ImGui_ImplVulkan_InitInfo init_info {};
	init_info.Instance           = ctx->instance;
	init_info.PhysicalDevice     = ctx->physical_device;
	init_info.Device             = ctx->device;
	init_info.QueueFamily        = ctx->queues[GraphicContext::QUEUE_PRESENT].family;
	init_info.Queue              = ctx->queues[GraphicContext::QUEUE_PRESENT].vk_queue;
	init_info.RenderPass         = g_render_pass;
	init_info.MinImageCount      = swapchain->swapchain_images_count;
	init_info.ImageCount         = swapchain->swapchain_images_count;
	init_info.MSAASamples        = VK_SAMPLE_COUNT_1_BIT;
	// Let the backend allocate its own descriptor pool (font atlas sampler).
	init_info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE;

	if (!ImGui_ImplVulkan_Init(&init_info))
	{
		std::fprintf(stderr, "DebugOverlay: ImGui_ImplVulkan_Init failed\n");
		ImGui_ImplSDL2_Shutdown();
		DestroyFramebuffers(ctx->device);
		vkDestroyRenderPass(ctx->device, g_render_pass, nullptr);
		g_render_pass = VK_NULL_HANDLE;
		ImGui::DestroyContext();
		return;
	}

	g_initialized = true;
}

void DebugOverlayShutdown(GraphicContext* ctx)
{
	if (!g_initialized)
	{
		return;
	}
	EXIT_IF(ctx == nullptr || ctx->device == nullptr);

	vkDeviceWaitIdle(ctx->device);
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();

	DestroyFramebuffers(ctx->device);
	if (g_render_pass != VK_NULL_HANDLE)
	{
		vkDestroyRenderPass(ctx->device, g_render_pass, nullptr);
		g_render_pass = VK_NULL_HANDLE;
	}

	DebugStatsShutdown();
	g_initialized = false;
}

void DebugOverlayOnSwapchainRecreated(GraphicContext* ctx, VulkanSwapchain* swapchain)
{
	if (!g_initialized || ctx == nullptr || swapchain == nullptr)
	{
		return;
	}

	vkDeviceWaitIdle(ctx->device);

	if (swapchain->swapchain_format != g_swapchain_format)
	{
		ImGui_ImplVulkan_Shutdown();
		DestroyFramebuffers(ctx->device);
		if (g_render_pass != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(ctx->device, g_render_pass, nullptr);
			g_render_pass = VK_NULL_HANDLE;
		}
		if (!CreateRenderPass(ctx->device, swapchain->swapchain_format) || !CreateFramebuffers(ctx->device, swapchain))
		{
			g_initialized = false;
			return;
		}

		ImGui_ImplVulkan_InitInfo init_info {};
		init_info.Instance           = ctx->instance;
		init_info.PhysicalDevice     = ctx->physical_device;
		init_info.Device             = ctx->device;
		init_info.QueueFamily        = ctx->queues[GraphicContext::QUEUE_PRESENT].family;
		init_info.Queue              = ctx->queues[GraphicContext::QUEUE_PRESENT].vk_queue;
		init_info.RenderPass         = g_render_pass;
		init_info.MinImageCount      = swapchain->swapchain_images_count;
		init_info.ImageCount         = swapchain->swapchain_images_count;
		init_info.MSAASamples        = VK_SAMPLE_COUNT_1_BIT;
		init_info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE;
		if (!ImGui_ImplVulkan_Init(&init_info))
		{
			g_initialized = false;
			return;
		}
	} else
	{
		DestroyFramebuffers(ctx->device);
		if (!CreateFramebuffers(ctx->device, swapchain))
		{
			g_initialized = false;
			return;
		}
		ImGui_ImplVulkan_SetMinImageCount(swapchain->swapchain_images_count);
	}
}

void DebugOverlayProcessEvent(const SDL_Event* event)
{
	if (!g_initialized || event == nullptr)
	{
		return;
	}
	ImGui_ImplSDL2_ProcessEvent(event);
}

void DebugOverlayToggle()
{
	g_visible = !g_visible;
}

bool DebugOverlayIsVisible()
{
	return g_visible;
}

bool DebugOverlayRecord(GraphicContext* ctx, VulkanSwapchain* swapchain, VkCommandBuffer cmd, double now_seconds, double fps,
                        double frame_time_ms)
{
	if (!g_initialized)
	{
		return false;
	}

	DebugStatsRecordFlip(fps, frame_time_ms);
	const DebugStatsSnapshot snap = DebugStatsTick(now_seconds);

	if (!g_visible || ctx == nullptr || swapchain == nullptr || cmd == nullptr)
	{
		return false;
	}
	if (swapchain->current_index >= g_framebuffers.size())
	{
		return false;
	}

	// The backend lazily uploads its font atlas from NewFrame and performs its
	// own queue submit/wait. Serialize that host access with every other role
	// that resolves to the same physical Vulkan queue.
	auto& queue = ctx->queues[GraphicContext::QUEUE_PRESENT];
	EXIT_IF(queue.mutex == nullptr);
	{
		Core::LockGuard queue_lock(*queue.mutex);
		ImGui_ImplVulkan_NewFrame();
	}
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	DrawHud(snap);
	ImGui::Render();

	VkImageMemoryBarrier to_color {};
	to_color.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_color.srcAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_color.dstAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	to_color.oldLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_color.newLayout                       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	to_color.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_color.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_color.image                           = swapchain->swapchain_images[swapchain->current_index];
	to_color.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	to_color.subresourceRange.baseMipLevel   = 0;
	to_color.subresourceRange.levelCount     = 1;
	to_color.subresourceRange.baseArrayLayer = 0;
	to_color.subresourceRange.layerCount     = 1;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &to_color);

	VkRenderPassBeginInfo begin {};
	begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass        = g_render_pass;
	begin.framebuffer       = g_framebuffers[swapchain->current_index];
	begin.renderArea.extent = swapchain->swapchain_extent;
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	vkCmdEndRenderPass(cmd);

	// Leave image in COLOR_ATTACHMENT_OPTIMAL; caller barriers to PRESENT_SRC.
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
