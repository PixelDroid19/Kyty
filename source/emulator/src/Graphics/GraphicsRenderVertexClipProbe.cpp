#include "GraphicsRenderInternal.h"

#include "Emulator/Agent/EventRing.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Log.h"

#include <cinttypes>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint64_t kAttachmentSnapshotMaxBytes = 32ull * 1024ull * 1024ull;

void DestroyAttachmentReadbackBuffer(GraphicContext* ctx, VulkanBuffer* buffer)
{
	if (ctx == nullptr || buffer == nullptr)
	{
		return;
	}
	if (buffer->buffer != VK_NULL_HANDLE)
	{
		vkDestroyBuffer(ctx->device, buffer->buffer, nullptr);
	}
	if (buffer->memory.memory != VK_NULL_HANDLE)
	{
		vkFreeMemory(ctx->device, buffer->memory.memory, nullptr);
	}
	*buffer = {};
}

bool CreateAttachmentReadbackBuffer(GraphicContext* ctx, uint64_t size, VulkanBuffer* buffer)
{
	if (ctx == nullptr || buffer == nullptr || ctx->device == VK_NULL_HANDLE || ctx->physical_device == VK_NULL_HANDLE ||
	    size == 0u || size > kAttachmentSnapshotMaxBytes || buffer->buffer != VK_NULL_HANDLE ||
	    buffer->memory.memory != VK_NULL_HANDLE)
	{
		return false;
	}

	VulkanBuffer candidate {};
	candidate.usage            = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	candidate.memory.property  = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	candidate.descriptor_range = size;

	VkBufferCreateInfo buffer_info {};
	buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size        = size;
	buffer_info.usage       = candidate.usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(ctx->device, &buffer_info, nullptr, &candidate.buffer) != VK_SUCCESS ||
	    candidate.buffer == VK_NULL_HANDLE)
	{
		DestroyAttachmentReadbackBuffer(ctx, &candidate);
		return false;
	}

	vkGetBufferMemoryRequirements(ctx->device, candidate.buffer, &candidate.memory.requirements);
	VkPhysicalDeviceMemoryProperties memory_properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &memory_properties);
	uint32_t memory_type = memory_properties.memoryTypeCount;
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
	{
		if ((candidate.memory.requirements.memoryTypeBits & (1u << i)) != 0u &&
		    (memory_properties.memoryTypes[i].propertyFlags & candidate.memory.property) == candidate.memory.property)
		{
			memory_type = i;
			break;
		}
	}
	if (memory_type == memory_properties.memoryTypeCount)
	{
		DestroyAttachmentReadbackBuffer(ctx, &candidate);
		return false;
	}

	VkMemoryAllocateInfo allocate_info {};
	allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize  = candidate.memory.requirements.size;
	allocate_info.memoryTypeIndex = memory_type;
	if (vkAllocateMemory(ctx->device, &allocate_info, nullptr, &candidate.memory.memory) != VK_SUCCESS ||
	    candidate.memory.memory == VK_NULL_HANDLE)
	{
		DestroyAttachmentReadbackBuffer(ctx, &candidate);
		return false;
	}
	candidate.memory.offset = 0u;
	candidate.memory.type   = memory_type;
	if (vkBindBufferMemory(ctx->device, candidate.buffer, candidate.memory.memory, candidate.memory.offset) != VK_SUCCESS)
	{
		DestroyAttachmentReadbackBuffer(ctx, &candidate);
		return false;
	}

	*buffer = candidate;
	return true;
}

bool VertexClipProbeStateHasPendingGpuWork(VertexClipProbeState state)
{
	return state == VertexClipProbeState::Reserved || state == VertexClipProbeState::Recording ||
	       state == VertexClipProbeState::PendingFence;
}

VertexClipProbeAttachmentFormat AttachmentReadbackFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return VertexClipProbeAttachmentFormat::B10G11R11Ufloat;
		case VK_FORMAT_R16G16B16A16_SFLOAT: return VertexClipProbeAttachmentFormat::Rgba16Sfloat;
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB: return VertexClipProbeAttachmentFormat::Rgba8;
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB: return VertexClipProbeAttachmentFormat::Bgra8;
		default: return VertexClipProbeAttachmentFormat::Unsupported;
	}
}

VertexClipProbeAttachmentStatus SelectAttachmentReadbackTarget(const RenderColorInfo* color, uint32_t target,
	                                                           VulkanImage** image,
	                                                           VertexClipProbeAttachmentFormat* format, uint32_t* width,
	                                                           uint32_t* height, uint64_t* bytes)
{
	if (image == nullptr || format == nullptr || width == nullptr || height == nullptr || bytes == nullptr)
	{
		return VertexClipProbeAttachmentStatus::InvalidData;
	}
	*image  = nullptr;
	*format = VertexClipProbeAttachmentFormat::Unsupported;
	*width  = 0u;
	*height = 0u;
	*bytes  = 0u;
	if (color == nullptr || target >= RenderColorInfo::TARGETS_MAX || !RenderColorSlotActive(*color, target))
	{
		return VertexClipProbeAttachmentStatus::TargetUnavailable;
	}

	const auto& attachment = color->attachment[target];
	auto*       selected   = attachment.vulkan_buffer;
	if (selected == nullptr || selected->image == VK_NULL_HANDLE)
	{
		return VertexClipProbeAttachmentStatus::TargetUnavailable;
	}
	if (attachment.samples != VK_SAMPLE_COUNT_1_BIT || selected->samples != VK_SAMPLE_COUNT_1_BIT)
	{
		return VertexClipProbeAttachmentStatus::Multisampled;
	}
	if ((selected->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0u)
	{
		return VertexClipProbeAttachmentStatus::TransferSourceUnavailable;
	}
	const auto selected_format = AttachmentReadbackFormat(selected->format);
	if (selected_format == VertexClipProbeAttachmentFormat::Unsupported)
	{
		return VertexClipProbeAttachmentStatus::UnsupportedFormat;
	}
	if (selected->extent.width == 0u || selected->extent.height == 0u)
	{
		return VertexClipProbeAttachmentStatus::ZeroExtent;
	}
	const uint32_t bytes_per_pixel = VertexClipProbeAttachmentFormatBytesPerPixel(selected_format);
	const uint64_t pixels = static_cast<uint64_t>(selected->extent.width) * selected->extent.height;
	if (bytes_per_pixel == 0u || pixels > kAttachmentSnapshotMaxBytes / bytes_per_pixel)
	{
		return VertexClipProbeAttachmentStatus::TooLarge;
	}

	*image  = selected;
	*format = selected_format;
	*width  = selected->extent.width;
	*height = selected->extent.height;
	*bytes  = pixels * bytes_per_pixel;
	return VertexClipProbeAttachmentStatus::Ok;
}

} // namespace

bool VertexClipProbeRenderer::Init(GraphicContext* ctx)
{
	Core::LockGuard lock(m_mutex);
	return InitLocked(ctx);
}

bool VertexClipProbeRenderer::InitLocked(GraphicContext* ctx)
{
	if (ctx == nullptr || ctx->device == VK_NULL_HANDLE)
	{
		return false;
	}
	if (m_context != nullptr)
	{
		return m_context == ctx && m_raw_stats_buffer.buffer != VK_NULL_HANDLE &&
		       m_descriptor_set_layout != VK_NULL_HANDLE && m_descriptor_pool != VK_NULL_HANDLE &&
		       m_descriptor_set != VK_NULL_HANDLE && m_depth_pass_query_pool != VK_NULL_HANDLE;
	}
	m_context = ctx;

	m_raw_stats_buffer.usage            = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	m_raw_stats_buffer.memory.property  = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	m_raw_stats_buffer.buffer           = VK_NULL_HANDLE;
	m_raw_stats_buffer.descriptor_range = 0;
	VulkanCreateBuffer(ctx, kRawStatsBytes, &m_raw_stats_buffer);
	if (m_raw_stats_buffer.buffer == VK_NULL_HANDLE || m_raw_stats_buffer.memory.memory == VK_NULL_HANDLE)
	{
		DoneLocked(ctx);
		return false;
	}

	VkQueryPoolCreateInfo query_pool_info {};
	query_pool_info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	query_pool_info.queryType  = VK_QUERY_TYPE_OCCLUSION;
	query_pool_info.queryCount = 1;
	if (vkCreateQueryPool(ctx->device, &query_pool_info, nullptr, &m_depth_pass_query_pool) != VK_SUCCESS ||
	    m_depth_pass_query_pool == VK_NULL_HANDLE)
	{
		DoneLocked(ctx);
		return false;
	}

	VkDescriptorSetLayoutBinding binding {};
	binding.binding            = 0;
	binding.descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount    = 1;
	binding.stageFlags         = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	binding.pImmutableSamplers = nullptr;

	VkDescriptorSetLayoutCreateInfo layout_info {};
	layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 1;
	layout_info.pBindings    = &binding;
	if (vkCreateDescriptorSetLayout(ctx->device, &layout_info, nullptr, &m_descriptor_set_layout) != VK_SUCCESS ||
	    m_descriptor_set_layout == VK_NULL_HANDLE)
	{
		DoneLocked(ctx);
		return false;
	}

	VkDescriptorPoolSize pool_size {};
	pool_size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_size.descriptorCount = 1;

	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes    = &pool_size;
	if (vkCreateDescriptorPool(ctx->device, &pool_info, nullptr, &m_descriptor_pool) != VK_SUCCESS ||
	    m_descriptor_pool == VK_NULL_HANDLE)
	{
		DoneLocked(ctx);
		return false;
	}

	VkDescriptorSetAllocateInfo allocate_info {};
	allocate_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate_info.descriptorPool     = m_descriptor_pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts        = &m_descriptor_set_layout;
	if (vkAllocateDescriptorSets(ctx->device, &allocate_info, &m_descriptor_set) != VK_SUCCESS ||
	    m_descriptor_set == VK_NULL_HANDLE)
	{
		DoneLocked(ctx);
		return false;
	}

	VkDescriptorBufferInfo buffer_info {};
	buffer_info.buffer = m_raw_stats_buffer.buffer;
	buffer_info.offset = 0;
	buffer_info.range  = kRawStatsBytes;

	VkWriteDescriptorSet write {};
	write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet          = m_descriptor_set;
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo     = &buffer_info;
	vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);

	return true;
}

bool VertexClipProbeRenderer::Done(GraphicContext* ctx)
{
	Core::LockGuard lock(m_mutex);
	if (m_context != nullptr && m_context != ctx)
	{
		return false;
	}
	if (VertexClipProbeStateHasPendingGpuWork(m_lifecycle.GetState()))
	{
		return false;
	}
	DoneLocked(ctx);
	return true;
}

void VertexClipProbeRenderer::DoneLocked(GraphicContext* ctx)
{
	if (m_context == nullptr)
	{
		return;
	}
	EXIT_IF(ctx == nullptr || ctx != m_context);

	if (m_descriptor_pool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(ctx->device, m_descriptor_pool, nullptr);
		m_descriptor_pool = VK_NULL_HANDLE;
	}
	if (m_descriptor_set_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(ctx->device, m_descriptor_set_layout, nullptr);
		m_descriptor_set_layout = VK_NULL_HANDLE;
	}
	if (m_depth_pass_query_pool != VK_NULL_HANDLE)
	{
		vkDestroyQueryPool(ctx->device, m_depth_pass_query_pool, nullptr);
		m_depth_pass_query_pool = VK_NULL_HANDLE;
	}
	if (m_raw_stats_buffer.buffer != VK_NULL_HANDLE)
	{
		VulkanDeleteBuffer(ctx, &m_raw_stats_buffer);
	}
	if (m_attachment_readback_buffer.buffer != VK_NULL_HANDLE ||
	    m_attachment_readback_buffer.memory.memory != VK_NULL_HANDLE)
	{
		DestroyAttachmentReadbackBuffer(ctx, &m_attachment_readback_buffer);
	}
	if (m_attachment_before_readback_buffer.buffer != VK_NULL_HANDLE ||
	    m_attachment_before_readback_buffer.memory.memory != VK_NULL_HANDLE)
	{
		DestroyAttachmentReadbackBuffer(ctx, &m_attachment_before_readback_buffer);
	}
	m_descriptor_set = VK_NULL_HANDLE;
	m_context        = nullptr;
	m_pending_draw   = {};
}

bool VertexClipProbeRenderer::Reserve(GraphicContext* ctx, CommandBuffer* buffer, uint64_t checksum, bool indexed,
                                     uint32_t guest_count, uint32_t descriptor_set, uint64_t pixel_checksum,
	                                     bool vertex_probe_enabled, bool pixel_probe_enabled, bool fixed_test_state_known,
	                                     bool depth_test_enabled, bool stencil_test_enabled, bool depth_bounds_test_enabled,
	                                     ShaderPixelProbeKind pixel_probe_kind, uint32_t pixel_probe_ordinal,
	                                     uint32_t match_ordinal, uint32_t pixel_probe_target, bool pixel_probe_sparse,
	                                     bool pixel_probe_attachment_readback,
	                                     uint32_t pixel_probe_attachment_min_invocations,
	                                     const RenderColorInfo* attachment_color,
	                                     VkAttachmentLoadOp attachment_load_op, VkImageLayout attachment_initial_layout)
{
	Core::LockGuard lock(m_mutex);
	const bool valid_pixel_probe_kind = pixel_probe_kind == ShaderPixelProbeKind::Input0 ||
	                                    pixel_probe_kind == ShaderPixelProbeKind::SampleResult ||
	                                    pixel_probe_kind == ShaderPixelProbeKind::FinalMrtResult;
	EXIT_IF(ctx == nullptr || buffer == nullptr || descriptor_set > 2u || (!vertex_probe_enabled && !pixel_probe_enabled) ||
	        (pixel_probe_enabled && !valid_pixel_probe_kind) ||
	        (!pixel_probe_enabled && (pixel_probe_kind != ShaderPixelProbeKind::None || pixel_probe_ordinal != 0u)) ||
	        (pixel_probe_kind == ShaderPixelProbeKind::Input0 && pixel_probe_ordinal != 0u) ||
	        (pixel_probe_sparse && pixel_probe_kind != ShaderPixelProbeKind::SampleResult) ||
	        (pixel_probe_attachment_readback &&
	         (!pixel_probe_enabled || pixel_probe_kind != ShaderPixelProbeKind::FinalMrtResult)) ||
	        pixel_probe_attachment_min_invocations == 0u ||
	        (!pixel_probe_attachment_readback && pixel_probe_attachment_min_invocations != 1u) ||
	        (pixel_probe_kind != ShaderPixelProbeKind::FinalMrtResult && pixel_probe_target != 0u) ||
	        (pixel_probe_kind == ShaderPixelProbeKind::FinalMrtResult && pixel_probe_target > 3u));
	if (pixel_probe_sparse &&
	    (((ctx->subgroup_stages & VK_SHADER_STAGE_FRAGMENT_BIT) == 0u) ||
	     ((ctx->subgroup_operations & VK_SUBGROUP_FEATURE_BASIC_BIT) == 0u)))
	{
		return false;
	}
	if (!m_lifecycle.Reserve(match_ordinal))
	{
		return false;
	}
	if (!InitLocked(ctx))
	{
		EXIT("vertex clip probe initialization failed\n");
	}
	m_pending_draw.buffer         = buffer;
	m_pending_draw.checksum       = checksum;
	m_pending_draw.pixel_checksum = pixel_checksum;
	m_pending_draw.indexed        = indexed;
	m_pending_draw.vertex_probe_enabled = vertex_probe_enabled;
	m_pending_draw.pixel_probe_enabled  = pixel_probe_enabled;
	m_pending_draw.pixel_probe_kind     = pixel_probe_kind;
	m_pending_draw.pixel_probe_ordinal  = pixel_probe_ordinal;
	m_pending_draw.pixel_probe_target   = pixel_probe_target;
	m_pending_draw.match_ordinal        = match_ordinal;
	m_pending_draw.pixel_probe_sparse   = pixel_probe_sparse;
	m_pending_draw.attachment_readback_requested = pixel_probe_attachment_readback;
	m_pending_draw.attachment_min_invocations     = pixel_probe_attachment_min_invocations;
	m_pending_draw.attachment_guest_addr =
	    pixel_probe_kind == ShaderPixelProbeKind::FinalMrtResult && attachment_color != nullptr &&
	            pixel_probe_target < RenderColorInfo::TARGETS_MAX
	        ? attachment_color->attachment[pixel_probe_target].base_addr
	        : 0u;
	m_pending_draw.attachment_load_op            = attachment_load_op;
	m_pending_draw.attachment_initial_layout      = attachment_initial_layout;
	m_pending_draw.fixed_test_state_known = fixed_test_state_known;
	m_pending_draw.depth_test_enabled     = depth_test_enabled;
	m_pending_draw.stencil_test_enabled   = stencil_test_enabled;
	m_pending_draw.depth_bounds_test_enabled = depth_bounds_test_enabled;
	m_pending_draw.guest_count    = guest_count;
	m_pending_draw.descriptor_set = descriptor_set;
	if (m_pending_draw.attachment_readback_requested)
	{
		VulkanImage* selected_image = nullptr;
		m_pending_draw.attachment_readback_status =
		    SelectAttachmentReadbackTarget(attachment_color, pixel_probe_target, &selected_image,
		                                 &m_pending_draw.attachment_readback_format,
		                                 &m_pending_draw.attachment_readback_width,
		                                 &m_pending_draw.attachment_readback_height,
		                                 &m_pending_draw.attachment_readback_bytes);
		m_pending_draw.attachment_delta_status = m_pending_draw.attachment_readback_status;
		if (m_pending_draw.attachment_readback_status == VertexClipProbeAttachmentStatus::Ok)
		{
			m_pending_draw.attachment_delta_status =
			    attachment_load_op != VK_ATTACHMENT_LOAD_OP_LOAD
			        ? VertexClipProbeAttachmentStatus::LoadDiscarded
			        : (attachment_initial_layout == VK_IMAGE_LAYOUT_UNDEFINED || selected_image->layout == VK_IMAGE_LAYOUT_UNDEFINED
			               ? VertexClipProbeAttachmentStatus::UndefinedLayout
			               : VertexClipProbeAttachmentStatus::Ok);
			EXIT_IF(m_attachment_readback_buffer.buffer != VK_NULL_HANDLE ||
			        m_attachment_before_readback_buffer.buffer != VK_NULL_HANDLE);
			if (!CreateAttachmentReadbackBuffer(ctx, m_pending_draw.attachment_readback_bytes,
			                                    &m_attachment_readback_buffer))
			{
				m_pending_draw.attachment_readback_status = VertexClipProbeAttachmentStatus::BufferUnavailable;
				m_pending_draw.attachment_delta_status    = VertexClipProbeAttachmentStatus::BufferUnavailable;
			} else if (m_pending_draw.attachment_delta_status == VertexClipProbeAttachmentStatus::Ok &&
			           !CreateAttachmentReadbackBuffer(ctx, m_pending_draw.attachment_readback_bytes,
			                                           &m_attachment_before_readback_buffer))
			{
				DestroyAttachmentReadbackBuffer(ctx, &m_attachment_readback_buffer);
				m_pending_draw.attachment_readback_status = VertexClipProbeAttachmentStatus::BufferUnavailable;
				m_pending_draw.attachment_delta_status    = VertexClipProbeAttachmentStatus::BufferUnavailable;
			} else
			{
				m_pending_draw.attachment_readback_image = selected_image;
			}
		}
	}
	return true;
}

void VertexClipProbeRenderer::InitializeRawStatsLocked()
{
	EXIT_IF(m_context == nullptr || m_raw_stats_buffer.buffer == VK_NULL_HANDLE);

	void* mapped = nullptr;
	VulkanMapMemory(m_context, &m_raw_stats_buffer.memory, &mapped);
	EXIT_IF(mapped == nullptr);
	const auto stats = VertexClipProbeInitialRawStats();
	static_assert(sizeof(stats) == kRawStatsBytes);
	std::memcpy(mapped, &stats, sizeof(stats));
	VulkanUnmapMemory(m_context, &m_raw_stats_buffer.memory);
}

void VertexClipProbeRenderer::Arm(CommandBuffer* buffer, VkPipelineLayout pipeline_layout)
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_lifecycle.GetState() != VertexClipProbeState::Reserved || m_pending_draw.buffer != buffer ||
	        m_pending_draw.descriptor_set > 2u || pipeline_layout == VK_NULL_HANDLE ||
	        m_descriptor_set == VK_NULL_HANDLE || m_raw_stats_buffer.buffer == VK_NULL_HANDLE ||
	        m_depth_pass_query_pool == VK_NULL_HANDLE);

	InitializeRawStatsLocked();

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	EXIT_IF(vk_buffer == VK_NULL_HANDLE);
	vkCmdBindDescriptorSets(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, m_pending_draw.descriptor_set, 1,
	                        &m_descriptor_set, 0, nullptr);

	VkBufferMemoryBarrier barrier {};
	barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
	barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = m_raw_stats_buffer.buffer;
	barrier.offset              = 0;
	barrier.size                = kRawStatsBytes;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_HOST_BIT,
	                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 1, &barrier, 0,
	                     nullptr);
	vkCmdResetQueryPool(vk_buffer, m_depth_pass_query_pool, 0u, 1u);

	EXIT_IF(!m_lifecycle.BeginRecording());
}

void VertexClipProbeRenderer::CaptureAttachmentBeforePass(CommandBuffer* buffer)
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_lifecycle.GetState() != VertexClipProbeState::Recording || m_pending_draw.buffer != buffer);
	if (!m_pending_draw.attachment_readback_requested || m_pending_draw.attachment_readback_image == nullptr ||
	    m_pending_draw.attachment_delta_status != VertexClipProbeAttachmentStatus::Ok)
	{
		return;
	}

	auto* attachment = m_pending_draw.attachment_readback_image;
	if (m_attachment_before_readback_buffer.buffer == VK_NULL_HANDLE || attachment->image == VK_NULL_HANDLE ||
	    attachment->layout == VK_IMAGE_LAYOUT_UNDEFINED || m_pending_draw.attachment_readback_bytes == 0u)
	{
		m_pending_draw.attachment_delta_status = attachment->layout == VK_IMAGE_LAYOUT_UNDEFINED
		                                                 ? VertexClipProbeAttachmentStatus::UndefinedLayout
		                                                 : VertexClipProbeAttachmentStatus::InvalidData;
		return;
	}

	const VkImageLayout restore_layout = attachment->layout;
	UtilImageToBuffer(buffer, attachment, &m_attachment_before_readback_buffer,
	                  m_pending_draw.attachment_readback_width, static_cast<uint64_t>(restore_layout), 0u);
	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	EXIT_IF(vk_buffer == VK_NULL_HANDLE);
	VkBufferMemoryBarrier before_barrier {};
	before_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	before_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	before_barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	before_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before_barrier.buffer              = m_attachment_before_readback_buffer.buffer;
	before_barrier.offset              = 0u;
	before_barrier.size                = m_pending_draw.attachment_readback_bytes;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0u, 0u, nullptr, 1u,
	                     &before_barrier, 0u, nullptr);
	m_pending_draw.attachment_before_recorded = true;
}

void VertexClipProbeRenderer::BeginDepthPassQuery(CommandBuffer* buffer)
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_lifecycle.GetState() != VertexClipProbeState::Recording || m_pending_draw.buffer != buffer ||
	        m_pending_draw.depth_query_active || m_pending_draw.depth_query_recorded ||
	        m_depth_pass_query_pool == VK_NULL_HANDLE);
	if (!m_pending_draw.depth_test_enabled && !m_pending_draw.stencil_test_enabled &&
	    !m_pending_draw.depth_bounds_test_enabled)
	{
		return;
	}
	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	EXIT_IF(vk_buffer == VK_NULL_HANDLE);
	vkCmdBeginQuery(vk_buffer, m_depth_pass_query_pool, 0u, 0u);
	m_pending_draw.depth_query_active = true;
}

void VertexClipProbeRenderer::EndDepthPassQuery(CommandBuffer* buffer)
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_lifecycle.GetState() != VertexClipProbeState::Recording || m_pending_draw.buffer != buffer ||
	        m_pending_draw.depth_query_recorded || m_depth_pass_query_pool == VK_NULL_HANDLE);
	if (!m_pending_draw.depth_test_enabled && !m_pending_draw.stencil_test_enabled &&
	    !m_pending_draw.depth_bounds_test_enabled)
	{
		EXIT_IF(m_pending_draw.depth_query_active);
		return;
	}
	EXIT_IF(!m_pending_draw.depth_query_active);
	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	EXIT_IF(vk_buffer == VK_NULL_HANDLE);
	vkCmdEndQuery(vk_buffer, m_depth_pass_query_pool, 0u);
	m_pending_draw.depth_query_active   = false;
	m_pending_draw.depth_query_recorded = true;
}

void VertexClipProbeRenderer::Finish(CommandBuffer* buffer)
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_lifecycle.GetState() != VertexClipProbeState::Recording || m_pending_draw.buffer != buffer ||
	        m_pending_draw.depth_query_active || m_raw_stats_buffer.buffer == VK_NULL_HANDLE);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];
	EXIT_IF(vk_buffer == VK_NULL_HANDLE);
	if (m_pending_draw.attachment_readback_requested && m_pending_draw.attachment_readback_image != nullptr)
	{
		auto* attachment = m_pending_draw.attachment_readback_image;
		if (m_pending_draw.attachment_readback_status != VertexClipProbeAttachmentStatus::Ok ||
		    m_attachment_readback_buffer.buffer == VK_NULL_HANDLE || attachment->image == VK_NULL_HANDLE ||
		    m_pending_draw.attachment_readback_bytes == 0u)
		{
			m_pending_draw.attachment_readback_status = VertexClipProbeAttachmentStatus::InvalidData;
		} else
		{
			const VkImageLayout restore_layout = attachment->layout;
			// This is the selected attachment's only diagnostic copy. The utility
			// transitions from its tracked layout and restores that exact layout.
			UtilImageToBuffer(buffer, attachment, &m_attachment_readback_buffer, m_pending_draw.attachment_readback_width,
			                  static_cast<uint64_t>(restore_layout), 0u);
			VkBufferMemoryBarrier attachment_barrier {};
			attachment_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			attachment_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
			attachment_barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
			attachment_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			attachment_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			attachment_barrier.buffer              = m_attachment_readback_buffer.buffer;
			attachment_barrier.offset              = 0u;
			attachment_barrier.size                = m_pending_draw.attachment_readback_bytes;
			vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0u, 0u, nullptr, 1u,
			                     &attachment_barrier, 0u, nullptr);
			m_pending_draw.attachment_readback_recorded = true;
		}
		// The attachment can be invalidated after Finish. Completion consumes only
		// the host buffer and copied metadata after the exact owning fence.
		m_pending_draw.attachment_readback_image = nullptr;
	}

	VkBufferMemoryBarrier barrier {};
	barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = m_raw_stats_buffer.buffer;
	barrier.offset              = 0;
	barrier.size                = kRawStatsBytes;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0,
	                     nullptr);

	EXIT_IF(!m_lifecycle.MarkPendingFence());
}

void VertexClipProbeRenderer::LogCompletedRawStatsLocked(const VertexClipProbeRawStats& stats)
{
	VertexClipProbeResultInfo result_info {};
	result_info.checksum       = m_pending_draw.checksum;
	result_info.pixel_checksum = m_pending_draw.pixel_checksum;
	result_info.indexed        = m_pending_draw.indexed;
	result_info.guest_count    = m_pending_draw.guest_count;
	result_info.descriptor_set = m_pending_draw.descriptor_set;
	if (m_pending_draw.pixel_probe_enabled)
	{
		if (m_pending_draw.pixel_probe_kind == ShaderPixelProbeKind::Input0)
		{
			char input0_message[Emulator::Agent::kAgentEventMessageMax] {};
			EXIT_IF(!VertexClipProbeFormatPixelInput0ResultMessage(result_info, stats, input0_message, sizeof(input0_message)));
			Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "ps_input0_probe", input0_message);
			if (VertexClipProbeHasFinitePixelInput0Extrema(stats))
			{
				KYTY_LOG_INFO(
				    "KYTY_PS_INPUT0_PROBE_RESULT checksum=%016" PRIx64
				    " indexed=%u guest_count=%u set=%u samples=%u nonfinite=%u finite=1"
				    " x_min=%.9g x_max=%.9g y_min=%.9g y_max=%.9g\n",
				    m_pending_draw.pixel_checksum, m_pending_draw.indexed ? 1u : 0u, m_pending_draw.guest_count,
				    m_pending_draw.descriptor_set, stats.pixel_input0_samples, stats.pixel_input0_nonfinite,
				    VertexClipProbeDecodeOrderedFloat(stats.min_pixel_input0_x),
				    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_input0_x),
				    VertexClipProbeDecodeOrderedFloat(stats.min_pixel_input0_y),
				    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_input0_y));
			} else
			{
				KYTY_LOG_INFO("KYTY_PS_INPUT0_PROBE_RESULT checksum=%016" PRIx64
				              " indexed=%u guest_count=%u set=%u samples=%u nonfinite=%u finite=0\n",
				              m_pending_draw.pixel_checksum, m_pending_draw.indexed ? 1u : 0u, m_pending_draw.guest_count,
				              m_pending_draw.descriptor_set, stats.pixel_input0_samples, stats.pixel_input0_nonfinite);
			}
		} else if (m_pending_draw.pixel_probe_kind == ShaderPixelProbeKind::SampleResult)
		{
			char sample_message[Emulator::Agent::kAgentEventMessageMax] {};
			EXIT_IF(!VertexClipProbeFormatPixelSampleResultMessage(result_info, m_pending_draw.pixel_probe_ordinal, stats,
			                                                     sample_message, sizeof(sample_message)));
			if (m_pending_draw.match_ordinal != 0u)
			{
				const auto sample_length = std::strlen(sample_message);
				EXIT_IF(sample_length >= sizeof(sample_message));
				const auto remaining = sizeof(sample_message) - sample_length;
				const int  appended  = std::snprintf(sample_message + sample_length, remaining, " m=%" PRIu32,
				                                    m_pending_draw.match_ordinal);
				EXIT_IF(appended < 0 || static_cast<size_t>(appended) >= remaining);
			}
			if (m_pending_draw.pixel_probe_sparse)
			{
				const auto sample_length = std::strlen(sample_message);
				EXIT_IF(sample_length >= sizeof(sample_message));
				const auto remaining = sizeof(sample_message) - sample_length;
				const int  appended  = std::snprintf(sample_message + sample_length, remaining, " sparse=1");
				EXIT_IF(appended < 0 || static_cast<size_t>(appended) >= remaining);
			}
			Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "ps_sample_probe", sample_message);
			if (VertexClipProbeHasFinitePixelSampleExtrema(stats))
			{
				KYTY_LOG_INFO(
				    "KYTY_PS_SAMPLE_PROBE_RESULT checksum=%016" PRIx64
				    " indexed=%u guest_count=%u set=%u ordinal=%u samples=%u nonfinite=%u finite=1"
				    " r_min=%.9g r_max=%.9g g_min=%.9g g_max=%.9g b_min=%.9g b_max=%.9g a_min=%.9g a_max=%.9g\n",
				    m_pending_draw.pixel_checksum, m_pending_draw.indexed ? 1u : 0u, m_pending_draw.guest_count,
				    m_pending_draw.descriptor_set, m_pending_draw.pixel_probe_ordinal, stats.pixel_sample_invocations,
				    stats.pixel_sample_nonfinite, VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_r),
				    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_r),
				    VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_g),
				    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_g),
				    VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_b),
				    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_b),
				    VertexClipProbeDecodeOrderedFloat(stats.min_pixel_sample_a),
				    VertexClipProbeDecodeOrderedFloat(stats.max_pixel_sample_a));
			} else
			{
				KYTY_LOG_INFO("KYTY_PS_SAMPLE_PROBE_RESULT checksum=%016" PRIx64
				              " indexed=%u guest_count=%u set=%u ordinal=%u samples=%u nonfinite=%u finite=0\n",
				              m_pending_draw.pixel_checksum, m_pending_draw.indexed ? 1u : 0u, m_pending_draw.guest_count,
				              m_pending_draw.descriptor_set, m_pending_draw.pixel_probe_ordinal, stats.pixel_sample_invocations,
				              stats.pixel_sample_nonfinite);
			}
		} else if (m_pending_draw.pixel_probe_kind == ShaderPixelProbeKind::FinalMrtResult)
		{
			char mrt_message[Emulator::Agent::kAgentEventMessageMax] {};
			EXIT_IF(!VertexClipProbeFormatPixelMrtResultMessage(
			    result_info, m_pending_draw.pixel_probe_target, m_pending_draw.pixel_probe_ordinal, stats, mrt_message,
			    sizeof(mrt_message)));
			if (m_pending_draw.match_ordinal != 0u)
			{
				const auto message_length = std::strlen(mrt_message);
				EXIT_IF(message_length >= sizeof(mrt_message));
				const auto remaining = sizeof(mrt_message) - message_length;
				const int appended = std::snprintf(mrt_message + message_length, remaining, " m=%" PRIu32,
				                                  m_pending_draw.match_ordinal);
				EXIT_IF(appended < 0 || static_cast<size_t>(appended) >= remaining);
			}
			Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "ps_mrt_probe", mrt_message);
			char coverage_message[Emulator::Agent::kAgentEventMessageMax] {};
			EXIT_IF(!VertexClipProbeFormatPixelMrtCoverageResultMessage(
			    result_info, m_pending_draw.pixel_probe_target, m_pending_draw.pixel_probe_ordinal, stats, coverage_message,
			    sizeof(coverage_message)));
			if (m_pending_draw.match_ordinal != 0u)
			{
				const auto message_length = std::strlen(coverage_message);
				EXIT_IF(message_length >= sizeof(coverage_message));
				const auto remaining = sizeof(coverage_message) - message_length;
				const int appended = std::snprintf(coverage_message + message_length, remaining, " m=%" PRIu32,
				                                  m_pending_draw.match_ordinal);
				EXIT_IF(appended < 0 || static_cast<size_t>(appended) >= remaining);
			}
			Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "ps_mrt_coverage",
			                                                  coverage_message);
		} else
		{
			EXIT_IF(true);
		}
	}
	if (!m_pending_draw.vertex_probe_enabled)
	{
		return;
	}
	char resolver_message[Emulator::Agent::kAgentEventMessageMax] {};
	EXIT_IF(!VertexClipProbeFormatResolverResultMessage(result_info, stats, resolver_message, sizeof(resolver_message)));
	Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "vs_resolver_probe", resolver_message);
	char param0_message[Emulator::Agent::kAgentEventMessageMax] {};
	EXIT_IF(!VertexClipProbeFormatParam0ResultMessage(result_info, stats, param0_message, sizeof(param0_message)));
	Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "vs_param0_probe", param0_message);
	if (VertexClipProbeHasFiniteParam0Extrema(stats))
	{
		KYTY_LOG_INFO(
		    "KYTY_VS_PARAM0_PROBE_RESULT checksum=%016" PRIx64
		    " indexed=%u guest_count=%u set=%u exports=%u nonfinite=%u finite=1"
		    " x_min=%.9g x_max=%.9g y_min=%.9g y_max=%.9g\n",
		    m_pending_draw.checksum, m_pending_draw.indexed ? 1u : 0u, m_pending_draw.guest_count,
		    m_pending_draw.descriptor_set, stats.param0_exports, stats.param0_nonfinite,
		    VertexClipProbeDecodeOrderedFloat(stats.min_param0_x), VertexClipProbeDecodeOrderedFloat(stats.max_param0_x),
		    VertexClipProbeDecodeOrderedFloat(stats.min_param0_y), VertexClipProbeDecodeOrderedFloat(stats.max_param0_y));
	} else
	{
		KYTY_LOG_INFO("KYTY_VS_PARAM0_PROBE_RESULT checksum=%016" PRIx64
		              " indexed=%u guest_count=%u set=%u exports=%u nonfinite=%u finite=0\n",
		              m_pending_draw.checksum, m_pending_draw.indexed ? 1u : 0u, m_pending_draw.guest_count,
		              m_pending_draw.descriptor_set, stats.param0_exports, stats.param0_nonfinite);
	}

	char result_message[Emulator::Agent::kAgentEventMessageMax] {};
	EXIT_IF(!VertexClipProbeFormatResultMessage(result_info, stats, result_message, sizeof(result_message)));
	Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "vs_clip_probe", result_message);
	char population_message[Emulator::Agent::kAgentEventMessageMax] {};
	EXIT_IF(!VertexClipProbeFormatPopulationResultMessage(result_info, stats, population_message, sizeof(population_message)));
	Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "vs_clip_population", population_message);

	if (!VertexClipProbeHasFiniteExtrema(stats))
	{
		KYTY_LOG_INFO("KYTY_VS_CLIP_PROBE_RESULT checksum=%016" PRIx64
		              " indexed=%u guest_count=%u set=%u invocations=%u nonfinite=%u finite=0\n",
		              m_pending_draw.checksum, m_pending_draw.indexed ? 1u : 0u, m_pending_draw.guest_count,
		              m_pending_draw.descriptor_set, stats.invocations, stats.nonfinite);
		return;
	}

	KYTY_LOG_INFO(
	    "KYTY_VS_CLIP_PROBE_RESULT checksum=%016" PRIx64
	    " indexed=%u guest_count=%u set=%u invocations=%u nonfinite=%u finite=1"
	    " w_min=%.9g w_max=%.9g x_w_min=%.9g x_w_max=%.9g y_w_min=%.9g y_w_max=%.9g z_w_min=%.9g z_w_max=%.9g"
	    " clip_w_nonpositive=%u clip_xy_outside=%u clip_z01_outside=%u clip_inside01=%u"
	    " clip_zn11_outside=%u clip_insiden11=%u\n",
	    m_pending_draw.checksum, m_pending_draw.indexed ? 1u : 0u, m_pending_draw.guest_count,
	    m_pending_draw.descriptor_set, stats.invocations, stats.nonfinite, VertexClipProbeDecodeOrderedFloat(stats.min_w),
	    VertexClipProbeDecodeOrderedFloat(stats.max_w), VertexClipProbeDecodeOrderedFloat(stats.min_x_w),
	    VertexClipProbeDecodeOrderedFloat(stats.max_x_w), VertexClipProbeDecodeOrderedFloat(stats.min_y_w),
	    VertexClipProbeDecodeOrderedFloat(stats.max_y_w), VertexClipProbeDecodeOrderedFloat(stats.min_z_w),
	    VertexClipProbeDecodeOrderedFloat(stats.max_z_w), stats.clip_w_nonpositive, stats.clip_xy_outside,
	    stats.clip_z_outside_zero_to_one, stats.clip_inside_zero_to_one,
	    stats.clip_z_outside_negative_one_to_one, stats.clip_inside_negative_one_to_one);
	}

void VertexClipProbeRenderer::LogCompletedAttachmentReadbackLocked(const VertexClipProbeRawStats& coverage)
{
	if (!m_pending_draw.attachment_readback_requested)
	{
		return;
	}

	VertexClipProbeResultInfo result_info {};
	result_info.checksum       = m_pending_draw.checksum;
	result_info.pixel_checksum = m_pending_draw.pixel_checksum;
	result_info.indexed        = m_pending_draw.indexed;
	result_info.guest_count    = m_pending_draw.guest_count;
	result_info.descriptor_set = m_pending_draw.descriptor_set;

	auto status       = m_pending_draw.attachment_readback_status;
	auto delta_status = m_pending_draw.attachment_delta_status;
	VertexClipProbeAttachmentReadbackStats readback_stats {};
	VertexClipProbeAttachmentDeltaStats    delta_stats {};
	const VertexClipProbeAttachmentReadbackStats* stats       = nullptr;
	const VertexClipProbeAttachmentDeltaStats*    delta       = nullptr;
	if (status == VertexClipProbeAttachmentStatus::Ok)
	{
		if (!m_pending_draw.attachment_readback_recorded || m_attachment_readback_buffer.buffer == VK_NULL_HANDLE ||
		    m_attachment_readback_buffer.memory.memory == VK_NULL_HANDLE || m_pending_draw.attachment_readback_bytes == 0u)
		{
			status = VertexClipProbeAttachmentStatus::InvalidData;
		} else
		{
			void* after_mapped = nullptr;
			const auto after_map_result = vkMapMemory(m_context->device, m_attachment_readback_buffer.memory.memory,
			                                          m_attachment_readback_buffer.memory.offset,
			                                          m_pending_draw.attachment_readback_bytes, 0u, &after_mapped);
			if (after_map_result != VK_SUCCESS || after_mapped == nullptr)
			{
				status = VertexClipProbeAttachmentStatus::MapFailed;
			} else
			{
				if (VertexClipProbeAggregateAttachmentReadback(
				        m_pending_draw.attachment_readback_format, static_cast<const uint8_t*>(after_mapped),
				        m_pending_draw.attachment_readback_bytes, m_pending_draw.attachment_readback_width,
				        m_pending_draw.attachment_readback_height, coverage, &readback_stats))
				{
					stats = &readback_stats;
				} else
				{
					status = VertexClipProbeAttachmentStatus::InvalidData;
				}

				if (status == VertexClipProbeAttachmentStatus::Ok &&
				    delta_status == VertexClipProbeAttachmentStatus::Ok)
				{
					if (!m_pending_draw.attachment_before_recorded ||
					    m_attachment_before_readback_buffer.buffer == VK_NULL_HANDLE ||
					    m_attachment_before_readback_buffer.memory.memory == VK_NULL_HANDLE)
					{
						delta_status = VertexClipProbeAttachmentStatus::InvalidData;
					} else
					{
						void* before_mapped = nullptr;
						const auto before_map_result = vkMapMemory(
						    m_context->device, m_attachment_before_readback_buffer.memory.memory,
						    m_attachment_before_readback_buffer.memory.offset,
						    m_pending_draw.attachment_readback_bytes, 0u, &before_mapped);
						if (before_map_result != VK_SUCCESS || before_mapped == nullptr)
						{
							delta_status = VertexClipProbeAttachmentStatus::MapFailed;
						} else
						{
							if (VertexClipProbeAggregateAttachmentDelta(
							        m_pending_draw.attachment_readback_format,
							        static_cast<const uint8_t*>(before_mapped),
							        static_cast<const uint8_t*>(after_mapped),
							        m_pending_draw.attachment_readback_bytes,
							        m_pending_draw.attachment_readback_width,
							        m_pending_draw.attachment_readback_height, coverage, &delta_stats))
							{
								delta = &delta_stats;
							} else
							{
								delta_status = VertexClipProbeAttachmentStatus::InvalidData;
							}
							vkUnmapMemory(m_context->device, m_attachment_before_readback_buffer.memory.memory);
						}
					}
				}
				vkUnmapMemory(m_context->device, m_attachment_readback_buffer.memory.memory);
			}
		}
	}
	if (status != VertexClipProbeAttachmentStatus::Ok && delta_status == VertexClipProbeAttachmentStatus::Ok)
	{
		delta_status = status;
	}
	m_pending_draw.attachment_readback_status = status;
	m_pending_draw.attachment_delta_status    = delta_status;
	char message[Emulator::Agent::kAgentEventMessageMax] {};
	EXIT_IF(!VertexClipProbeFormatPixelMrtAttachmentResultMessage(
	    result_info, m_pending_draw.pixel_probe_target, m_pending_draw.pixel_probe_ordinal, m_pending_draw.match_ordinal,
	    m_attachment_empty_retries, status, stats, message, sizeof(message)));
	Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "ps_mrt_attachment", message);
	char delta_message[Emulator::Agent::kAgentEventMessageMax] {};
	EXIT_IF(!VertexClipProbeFormatPixelMrtAttachmentDeltaMessage(
	    result_info, m_pending_draw.pixel_probe_target, m_pending_draw.pixel_probe_ordinal,
	    m_pending_draw.attachment_guest_addr, m_pending_draw.match_ordinal, m_attachment_empty_retries,
	    m_pending_draw.attachment_load_op,
	    delta_status, delta, delta_message, sizeof(delta_message)));
	Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "ps_mrt_delta", delta_message);
}

void VertexClipProbeRenderer::Complete(CommandBuffer* buffer)
{
	Core::LockGuard lock(m_mutex);
	if (m_lifecycle.GetState() != VertexClipProbeState::PendingFence || m_pending_draw.buffer != buffer)
	{
		return;
	}
	EXIT_IF(m_context == nullptr || m_raw_stats_buffer.buffer == VK_NULL_HANDLE);

	VertexClipProbeRawStats stats {};
	void*                   mapped = nullptr;
	VulkanMapMemory(m_context, &m_raw_stats_buffer.memory, &mapped);
	EXIT_IF(mapped == nullptr);
	std::memcpy(&stats, mapped, sizeof(stats));
	VulkanUnmapMemory(m_context, &m_raw_stats_buffer.memory);
	if (m_pending_draw.attachment_readback_requested &&
	    m_pending_draw.attachment_readback_status == VertexClipProbeAttachmentStatus::Ok &&
	    m_pending_draw.attachment_readback_recorded &&
	    VertexClipProbeAttachmentShouldRetry(stats, m_pending_draw.attachment_min_invocations,
	                                         m_attachment_empty_retries))
	{
		DestroyAttachmentReadbackBuffer(m_context, &m_attachment_readback_buffer);
		DestroyAttachmentReadbackBuffer(m_context, &m_attachment_before_readback_buffer);
		++m_attachment_empty_retries;
		EXIT_IF(!m_lifecycle.RetryAfterFence());
		m_pending_draw = {};
		return;
	}

	LogCompletedRawStatsLocked(stats);
	LogCompletedAttachmentReadbackLocked(stats);
	if (m_pending_draw.fixed_test_state_known)
	{
		struct QueryResult
		{
			uint64_t samples      = 0;
			uint64_t availability = 0;
		};
		const bool applicable = m_pending_draw.depth_test_enabled || m_pending_draw.stencil_test_enabled ||
		                        m_pending_draw.depth_bounds_test_enabled;
		QueryResult query {};
		VkResult    query_result = VK_SUCCESS;
		if (applicable)
		{
			EXIT_IF(!m_pending_draw.depth_query_recorded);
			query_result = vkGetQueryPoolResults(m_context->device, m_depth_pass_query_pool, 0u, 1u, sizeof(query), &query,
			                                     sizeof(QueryResult),
			                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		}
		const bool ready      = !applicable || (query_result == VK_SUCCESS && query.availability != 0u);
		const bool any_passed = applicable && ready && query.samples != 0u;
		char match_suffix[24] {};
		if (m_pending_draw.match_ordinal != 0u)
		{
			const int suffix_written =
			    std::snprintf(match_suffix, sizeof(match_suffix), " m=%" PRIu32, m_pending_draw.match_ordinal);
			EXIT_IF(suffix_written < 0 || static_cast<size_t>(suffix_written) >= sizeof(match_suffix));
		}
		char message[Emulator::Agent::kAgentEventMessageMax] {};
		const int written = std::snprintf(message, sizeof(message),
		                                  "cs=%016" PRIx64 " k=%c n=%" PRIu32 " s=%" PRIu32
		                                  " applicable=%u depth=%u stencil=%u bounds=%u ready=%u precise=0 any_passed=%u%s",
		                                  m_pending_draw.checksum, m_pending_draw.indexed ? 'i' : 'a',
		                                  m_pending_draw.guest_count, m_pending_draw.descriptor_set,
		                                  applicable ? 1u : 0u, m_pending_draw.depth_test_enabled ? 1u : 0u,
		                                  m_pending_draw.stencil_test_enabled ? 1u : 0u,
		                                  m_pending_draw.depth_bounds_test_enabled ? 1u : 0u, ready ? 1u : 0u,
		                                  any_passed ? 1u : 0u, match_suffix);
		EXIT_IF(written < 0 || static_cast<size_t>(written) >= sizeof(message));
		Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Info, "depth_stencil_probe", message);
	}
	EXIT_IF(!m_lifecycle.Complete());
	m_pending_draw = {};
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
