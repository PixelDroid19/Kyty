#include "Emulator/Graphics/Utils.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Profiler.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

VkImageLayout UtilGetImageUploadSourceLayout(const VulkanImage* image)
{
	EXIT_IF(image == nullptr);
	return image->layout;
}

static void set_image_layout(VkCommandBuffer buffer, VulkanImage* dst_image, uint32_t base_level, uint32_t levels,
                             VkImageAspectFlags aspect_mask, VkImageLayout old_image_layout, VkImageLayout new_image_layout)
{
	EXIT_IF(buffer == nullptr);

	EXIT_IF(old_image_layout != VK_IMAGE_LAYOUT_UNDEFINED && dst_image->layout != old_image_layout);

	VkImageMemoryBarrier image_memory_barrier {};
	image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	image_memory_barrier.pNext                           = nullptr;
	image_memory_barrier.srcAccessMask                   = 0;
	image_memory_barrier.dstAccessMask                   = 0;
	image_memory_barrier.oldLayout                       = old_image_layout;
	image_memory_barrier.newLayout                       = new_image_layout;
	image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.image                           = dst_image->image;
	image_memory_barrier.subresourceRange.aspectMask     = aspect_mask;
	image_memory_barrier.subresourceRange.baseMipLevel   = base_level;
	image_memory_barrier.subresourceRange.levelCount     = levels;
	image_memory_barrier.subresourceRange.baseArrayLayer = 0;
	image_memory_barrier.subresourceRange.layerCount     = 1;

	VkPipelineStageFlags src_stages  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags dest_stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

	switch (old_image_layout)
	{
		case VK_IMAGE_LAYOUT_PREINITIALIZED:
			image_memory_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
			src_stages                         = VK_PIPELINE_STAGE_HOST_BIT;
			break;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			image_memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			src_stages                         = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			image_memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			src_stages                         = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			image_memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			src_stages                         = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			image_memory_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			src_stages                         = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
													   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;
		case VK_IMAGE_LAYOUT_GENERAL:
			image_memory_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			src_stages                         = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
													   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			image_memory_barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			src_stages                         = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
			image_memory_barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
			src_stages                         = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
													   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			break;
		default:
			image_memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			src_stages                         = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			break;
	}

	switch (new_image_layout)
	{
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			image_memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			dest_stages                        = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			image_memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			dest_stages                        = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			image_memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			dest_stages                        = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
													   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;
		case VK_IMAGE_LAYOUT_GENERAL:
			image_memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			dest_stages                        = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
													   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			image_memory_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dest_stages                        = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			image_memory_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dest_stages                        = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
			image_memory_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
			dest_stages                        = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
													   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			break;
		default:
			image_memory_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			dest_stages                        = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			break;
	}

	vkCmdPipelineBarrier(buffer, src_stages, dest_stages, 0, 0, nullptr, 0, nullptr, 1, &image_memory_barrier);

	dst_image->layout = new_image_layout;
}

void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, uint32_t src_pitch, VulkanImage* dst_image, uint64_t dst_layout)
{
	EXIT_IF(src_buffer == nullptr);
	EXIT_IF(src_buffer->buffer == nullptr);
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(dst_image->image == nullptr);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	set_image_layout(vk_buffer, dst_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, UtilGetImageUploadSourceLayout(dst_image),
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkBufferImageCopy region {};
	region.bufferOffset      = 0;
	region.bufferRowLength   = (src_pitch != dst_image->extent.width ? src_pitch : 0);
	region.bufferImageHeight = 0;

	region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel       = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount     = 1;

	region.imageOffset = {0, 0, 0};
	region.imageExtent = {dst_image->extent.width, dst_image->extent.height, 1};

	vkCmdCopyBufferToImage(vk_buffer, src_buffer->buffer, dst_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	set_image_layout(vk_buffer, dst_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                 static_cast<VkImageLayout>(dst_layout));
}

void UtilImageToBuffer(CommandBuffer* buffer, VulkanImage* src_image, VulkanBuffer* dst_buffer, uint32_t dst_pitch, uint64_t src_layout)
{
	EXIT_IF(dst_buffer == nullptr);
	EXIT_IF(dst_buffer->buffer == nullptr);
	EXIT_IF(src_image == nullptr);
	EXIT_IF(src_image->image == nullptr);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, src_image->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	VkBufferImageCopy region {};
	region.bufferOffset      = 0;
	region.bufferRowLength   = (dst_pitch != src_image->extent.width ? dst_pitch : 0);
	region.bufferImageHeight = 0;

	region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel       = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount     = 1;

	region.imageOffset = {0, 0, 0};
	region.imageExtent = {src_image->extent.width, src_image->extent.height, 1};

	vkCmdCopyImageToBuffer(vk_buffer, src_image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_buffer->buffer, 1, &region);

	set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                 static_cast<VkImageLayout>(src_layout));
}

void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, VulkanImage* dst_image, const Vector<BufferImageCopy>& regions,
                       uint64_t dst_layout)
{
	EXIT_IF(src_buffer == nullptr);
	EXIT_IF(src_buffer->buffer == nullptr);
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(dst_image->image == nullptr);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	EXIT_NOT_IMPLEMENTED(regions.Size() >= 16);

	VkBufferImageCopy region[16];

	uint32_t index = 0;
	for (const auto& r: regions)
	{
		region[index].bufferOffset                    = r.offset;
		region[index].bufferRowLength                 = (r.width != r.pitch ? r.pitch : 0);
		region[index].bufferImageHeight               = 0;
		region[index].imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region[index].imageSubresource.mipLevel       = r.dst_level;
		region[index].imageSubresource.baseArrayLayer = 0;
		region[index].imageSubresource.layerCount     = 1;
		region[index].imageOffset                     = {r.dst_x, r.dst_y, 0};
		region[index].imageExtent                     = {r.width, r.height, 1};
		index++;
	}

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT,
	                 UtilGetImageUploadSourceLayout(dst_image),
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	vkCmdCopyBufferToImage(vk_buffer, src_buffer->buffer, dst_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, index, region);

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                 static_cast<VkImageLayout>(dst_layout));
}

void UtilImageToImage(CommandBuffer* buffer, const Vector<ImageImageCopy>& regions, VulkanImage* dst_image, uint64_t dst_layout)
{
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(dst_image->image == nullptr);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	EXIT_NOT_IMPLEMENTED(regions.Size() >= 16);

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT,
	                 UtilGetImageUploadSourceLayout(dst_image),
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	for (const auto& r: regions)
	{
		VkImageCopy region;

		auto src_layout = r.src_image->layout;

		region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.mipLevel       = r.src_level;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount     = 1;
		region.srcOffset                     = {r.src_x, r.src_y, 0};
		region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.mipLevel       = r.dst_level;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount     = 1;
		region.dstOffset                     = {r.dst_x, r.dst_y, 0};
		region.extent                        = {r.width, r.height, 1};

		set_image_layout(vk_buffer, r.src_image, r.src_level, 1, VK_IMAGE_ASPECT_COLOR_BIT, src_layout,
		                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		vkCmdCopyImage(vk_buffer, r.src_image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image->image,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		set_image_layout(vk_buffer, r.src_image, r.src_level, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                 src_layout);
	}

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                 static_cast<VkImageLayout>(dst_layout));
}

void UtilBlitImage(CommandBuffer* buffer, VulkanImage* src_image, VulkanSwapchain* dst_swapchain)
{
	EXIT_IF(src_image == nullptr);
	EXIT_IF(src_image->image == nullptr);
	EXIT_IF(dst_swapchain == nullptr);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	VulkanImage swapchain_image(VulkanImageType::Unknown);

	swapchain_image.image  = dst_swapchain->swapchain_images[dst_swapchain->current_index];
	swapchain_image.layout = VK_IMAGE_LAYOUT_UNDEFINED;

	// Use the tracked source layout; hardcoding COLOR_ATTACHMENT_OPTIMAL fails
	// when the flip source is still GENERAL/TRANSFER_* (Linux present races).
	// Restore color-attachment so the next render pass sees a stable layout.
	const auto source_layout = src_image->layout;
	if (UtilBlitImageNeedsSourceInitialization(source_layout))
	{
		set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkClearColorValue clear_color {{0.0f, 0.0f, 0.0f, 1.0f}};
		VkImageSubresourceRange clear_range {};
		clear_range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		clear_range.baseMipLevel   = 0;
		clear_range.levelCount     = 1;
		clear_range.baseArrayLayer = 0;
		clear_range.layerCount     = 1;
		vkCmdClearColorImage(vk_buffer, src_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &clear_range);

		set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	} else
	{
		set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, source_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	}
	set_image_layout(vk_buffer, &swapchain_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkImageBlit region {};
	region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.mipLevel       = 0;
	region.srcSubresource.baseArrayLayer = 0;
	region.srcSubresource.layerCount     = 1;
	region.srcOffsets[0].x               = 0;
	region.srcOffsets[0].y               = 0;
	region.srcOffsets[0].z               = 0;
	region.srcOffsets[1].x               = static_cast<int>(src_image->extent.width);
	region.srcOffsets[1].y               = static_cast<int>(src_image->extent.height);
	region.srcOffsets[1].z               = 1;
	region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.dstSubresource.mipLevel       = 0;
	region.dstSubresource.baseArrayLayer = 0;
	region.dstSubresource.layerCount     = 1;
	region.dstOffsets[0].x               = 0;
	region.dstOffsets[0].y               = 0;
	region.dstOffsets[0].z               = 0;
	region.dstOffsets[1].x               = static_cast<int>(dst_swapchain->swapchain_extent.width);
	region.dstOffsets[1].y               = static_cast<int>(dst_swapchain->swapchain_extent.height);
	region.dstOffsets[1].z               = 1;

	vkCmdBlitImage(vk_buffer, src_image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain_image.image,
	               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

	set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void VulkanCreateBuffer(GraphicContext* gctx, uint64_t size, VulkanBuffer* buffer)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(gctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->buffer != nullptr);

	VkBufferCreateInfo buffer_info {};
	buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size        = size;
	buffer_info.usage       = buffer->usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	vkCreateBuffer(gctx->device, &buffer_info, nullptr, &buffer->buffer);
	EXIT_NOT_IMPLEMENTED(buffer->buffer == nullptr);

	vkGetBufferMemoryRequirements(gctx->device, buffer->buffer, &buffer->memory.requirements);

	bool allocated = VulkanAllocate(gctx, &buffer->memory);
	EXIT_NOT_IMPLEMENTED(!allocated);

	// vkBindBufferMemory(gctx->device, buffer->buffer, buffer->memory.memory, buffer->memory.offset);
	VulkanBindBufferMemory(gctx, buffer, &buffer->memory);
}

void VulkanDeleteBuffer(GraphicContext* gctx, VulkanBuffer* buffer)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffer == nullptr);
	EXIT_IF(gctx == nullptr);

	DeleteDescriptor(buffer);

	vkDestroyBuffer(gctx->device, buffer->buffer, nullptr);
	VulkanFree(gctx, &buffer->memory);
	buffer->buffer = nullptr;
}

void UtilFillImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size, uint32_t src_pitch,
                   uint64_t dst_layout)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(src_data == nullptr);

	const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, size);
	VulkanBuffer               staging_buffer {};
	staging_buffer.usage           = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	staging_buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VulkanCreateBuffer(ctx, size, &staging_buffer);

	void* data = nullptr;
	VulkanMapMemory(ctx, &staging_buffer.memory, &data);
	std::memcpy(data, src_data, size);
	VulkanUnmapMemory(ctx, &staging_buffer.memory);

	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();
	UtilBufferToImage(&buffer, &staging_buffer, src_pitch, dst_image, dst_layout);
	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();

	VulkanDeleteBuffer(ctx, &staging_buffer);
}

void UtilFillBuffer(GraphicContext* ctx, void* dst_data, uint64_t size, uint32_t dst_pitch, VulkanImage* src_image, uint64_t src_layout)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(src_image == nullptr);
	EXIT_IF(dst_data == nullptr);

	VulkanBuffer staging_buffer {};
	staging_buffer.usage           = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	staging_buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VulkanCreateBuffer(ctx, size, &staging_buffer);

	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();
	UtilImageToBuffer(&buffer, src_image, &staging_buffer, dst_pitch, src_layout);
	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();

	void* data = nullptr;
	VulkanMapMemory(ctx, &staging_buffer.memory, &data);
	std::memcpy(dst_data, data, size);
	VulkanUnmapMemory(ctx, &staging_buffer.memory);

	VulkanDeleteBuffer(ctx, &staging_buffer);
}

bool UtilDumpVulkanImageRgba8Bmp(GraphicContext* ctx, VulkanImage* image, const char* path_prefix, const char* tag)
{
	if (ctx == nullptr || image == nullptr || image->image == nullptr || path_prefix == nullptr || path_prefix[0] == '\0')
	{
		return false;
	}
	if (image->format != VK_FORMAT_R8G8B8A8_SRGB && image->format != VK_FORMAT_R8G8B8A8_UNORM &&
	    image->format != VK_FORMAT_B8G8R8A8_SRGB && image->format != VK_FORMAT_B8G8R8A8_UNORM)
	{
		return false;
	}
	const uint32_t w = image->extent.width;
	const uint32_t h = image->extent.height;
	if (w == 0 || h == 0 || w > 8192 || h > 8192)
	{
		return false;
	}
	static std::set<std::string> dumped;
	char                         key_buf[320];
	std::snprintf(key_buf, sizeof(key_buf), "%s|%llu|%ux%u", path_prefix,
	              static_cast<unsigned long long>(image->memory.unique_id), w, h);
	if (!dumped.insert(key_buf).second || dumped.size() > 64u)
	{
		return false;
	}

	const uint64_t       bytes = static_cast<uint64_t>(w) * h * 4u;
	std::vector<uint8_t> pixels(static_cast<size_t>(bytes));
	UtilFillBuffer(ctx, pixels.data(), bytes, w, image, static_cast<uint64_t>(image->layout));

	char path[256];
	std::snprintf(path, sizeof(path), "%s-%s-%ux%u-id%llu.bmp", path_prefix, (tag != nullptr ? tag : "img"), w, h,
	              static_cast<unsigned long long>(image->memory.unique_id));

	FILE* f = std::fopen(path, "wb");
	if (f == nullptr)
	{
		return false;
	}
	const uint32_t row_bytes = w * 4u;
	const uint32_t pad       = (4u - (row_bytes % 4u)) % 4u;
	const uint32_t dib       = 40u;
	const uint32_t off       = 14u + dib;
	const uint32_t size_img  = (row_bytes + pad) * h;
	const uint32_t file_size = off + size_img;
	uint8_t        hdr[54] {};
	hdr[0] = 'B';
	hdr[1] = 'M';
	std::memcpy(hdr + 2, &file_size, 4);
	std::memcpy(hdr + 10, &off, 4);
	std::memcpy(hdr + 14, &dib, 4);
	int32_t  wi     = static_cast<int32_t>(w);
	int32_t  hi     = -static_cast<int32_t>(h);
	uint16_t planes = 1;
	uint16_t bpp    = 32;
	std::memcpy(hdr + 18, &wi, 4);
	std::memcpy(hdr + 22, &hi, 4);
	std::memcpy(hdr + 26, &planes, 2);
	std::memcpy(hdr + 28, &bpp, 2);
	std::fwrite(hdr, 1, 54, f);
	std::vector<uint8_t> zero(pad, 0);
	const bool           swap_rb =
	    (image->format == VK_FORMAT_B8G8R8A8_SRGB || image->format == VK_FORMAT_B8G8R8A8_UNORM);
	for (uint32_t y = 0; y < h; y++)
	{
		const uint8_t* row = pixels.data() + static_cast<uint64_t>(y) * row_bytes;
		if (!swap_rb)
		{
			std::fwrite(row, 1, row_bytes, f);
		} else
		{
			for (uint32_t x = 0; x < w; x++)
			{
				const uint8_t* px     = row + x * 4u;
				const uint8_t  out[4] = {px[2], px[1], px[0], px[3]};
				std::fwrite(out, 1, 4, f);
			}
		}
		if (pad != 0)
		{
			std::fwrite(zero.data(), 1, pad, f);
		}
	}
	std::fclose(f);
	std::fprintf(stderr, "KYTY_DUMP_VK_IMAGE wrote %s layout=%u fmt=%d\n", path, static_cast<unsigned>(image->layout),
	             static_cast<int>(image->format));
	return true;
}

void UtilSetDepthLayoutOptimal(DepthStencilVulkanImage* image)
{
	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();

	auto* vk_buffer = buffer.GetPool()->buffers[buffer.GetIndex()];

	VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_DEPTH_BIT;

	if (image->format == VK_FORMAT_D24_UNORM_S8_UINT || image->format == VK_FORMAT_D32_SFLOAT_S8_UINT)
	{
		aspect_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	set_image_layout(vk_buffer, image, 0, 1, aspect_mask, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
}

void UtilSetImageLayoutOptimal(VulkanImage* image)
{
	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();

	auto* vk_buffer = buffer.GetPool()->buffers[buffer.GetIndex()];

	VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;

	set_image_layout(vk_buffer, image, 0, 1, aspect_mask, image->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
}

void UtilFillImage(GraphicContext* ctx, VulkanImage* image, const void* src_data, uint64_t size, const Vector<BufferImageCopy>& regions,
                   uint64_t dst_layout)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, size);
	VulkanBuffer               staging_buffer {};
	staging_buffer.usage           = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	staging_buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VulkanCreateBuffer(ctx, size, &staging_buffer);

	void* data = nullptr;
	VulkanMapMemory(ctx, &staging_buffer.memory, &data);
	std::memcpy(data, src_data, size);
	VulkanUnmapMemory(ctx, &staging_buffer.memory);

	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();
	UtilBufferToImage(&buffer, &staging_buffer, image, regions, dst_layout);
	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();

	VulkanDeleteBuffer(ctx, &staging_buffer);
}

void UtilFillImage(GraphicContext* ctx, const Vector<ImageImageCopy>& regions, VulkanImage* dst_image, uint64_t dst_layout)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(dst_image == nullptr);

	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();
	UtilImageToImage(&buffer, regions, dst_image, dst_layout);
	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
}

void UtilCopyBuffer(VulkanBuffer* src_buffer, VulkanBuffer* dst_buffer, uint64_t size)
{
	EXIT_IF(src_buffer == nullptr);
	EXIT_IF(src_buffer->buffer == nullptr);
	EXIT_IF(dst_buffer == nullptr);
	EXIT_IF(dst_buffer->buffer == nullptr);

	const DebugStatsScopedWork upload_work(DebugStatsRecordUpload, size);
	CommandBuffer              buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	auto* vk_buffer = buffer.GetPool()->buffers[buffer.GetIndex()];

	buffer.Begin();

	VkBufferCopy copy_region {};
	copy_region.srcOffset = 0;
	copy_region.dstOffset = 0;
	copy_region.size      = size;

	vkCmdCopyBuffer(vk_buffer, src_buffer->buffer, dst_buffer->buffer, 1, &copy_region);

	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
