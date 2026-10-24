#include "Emulator/Graphics/FragmentTransportRuntime.h"

#include "Emulator/Graphics/FragmentTransportShaders.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderSpirv.h"

#include "GraphicsRenderInternal.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"

#include <algorithm>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kTransportBindings = 5;
constexpr uint32_t kTransportPushBytes = sizeof(FragmentTransport::KernelParameters);

struct HostKernels
{
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout      scan_layout = VK_NULL_HANDLE;
	VkPipelineLayout      pack_layout = VK_NULL_HANDLE;
	VkPipeline            scan        = VK_NULL_HANDLE;
	VkPipeline            pack        = VK_NULL_HANDLE;
	bool                  attempted   = false;
	bool                  ready       = false;
};

HostKernels& Kernels()
{
	static HostKernels kernels;
	return kernels;
}

std::mutex& KernelsMutex()
{
	static std::mutex mutex;
	return mutex;
}

VkDescriptorSetLayout CreateTransportSetLayout(VkDevice device)
{
	VkDescriptorSetLayoutBinding bindings[kTransportBindings] {};
	for (uint32_t binding = 0; binding < kTransportBindings; ++binding)
	{
		bindings[binding].binding         = binding;
		bindings[binding].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[binding].descriptorCount = 1;
		bindings[binding].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo info {};
	info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = kTransportBindings;
	info.pBindings    = bindings;
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	return vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) == VK_SUCCESS ? layout : VK_NULL_HANDLE;
}

VkPipelineLayout CreateKernelLayout(VkDevice device, VkDescriptorSetLayout set_layout)
{
	VkPushConstantRange range {};
	range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	range.offset     = 0;
	range.size       = kTransportPushBytes;
	VkPipelineLayoutCreateInfo info {};
	info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	info.setLayoutCount         = 1;
	info.pSetLayouts            = &set_layout;
	info.pushConstantRangeCount = 1;
	info.pPushConstantRanges    = &range;
	VkPipelineLayout layout     = VK_NULL_HANDLE;
	return vkCreatePipelineLayout(device, &info, nullptr, &layout) == VK_SUCCESS ? layout : VK_NULL_HANDLE;
}

VkPipeline CreateKernelPipeline(VkDevice device, VkPipelineCache cache, VkPipelineLayout layout, FragmentTransport::Kernel kernel)
{
	const auto binary = FragmentTransport::GetKernelBinary(kernel);
	if (binary.words == nullptr || binary.word_count == 0)
	{
		return VK_NULL_HANDLE;
	}
	VkShaderModuleCreateInfo module_info {};
	module_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = binary.word_count * sizeof(uint32_t);
	module_info.pCode    = binary.words;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &module_info, nullptr, &module) != VK_SUCCESS)
	{
		return VK_NULL_HANDLE;
	}
	VkComputePipelineCreateInfo info {};
	info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName  = "main";
	info.layout       = layout;
	info.basePipelineIndex = -1;
	VkPipeline pipeline = VK_NULL_HANDLE;
	const auto result   = vkCreateComputePipelines(device, cache, 1, &info, nullptr, &pipeline);
	vkDestroyShaderModule(device, module, nullptr);
	return result == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

bool EnsureKernels()
{
	std::lock_guard<std::mutex> lock(KernelsMutex());
	auto&                       kernels = Kernels();
	if (kernels.attempted)
	{
		return kernels.ready;
	}
	kernels.attempted = true;
	auto* gctx        = g_render_ctx != nullptr ? g_render_ctx->GetGraphicCtx() : nullptr;
	if (gctx == nullptr || gctx->device == VK_NULL_HANDLE)
	{
		return false;
	}
	kernels.set_layout  = CreateTransportSetLayout(gctx->device);
	kernels.scan_layout = CreateKernelLayout(gctx->device, kernels.set_layout);
	kernels.pack_layout = CreateKernelLayout(gctx->device, kernels.set_layout);
	if (kernels.set_layout != VK_NULL_HANDLE && kernels.scan_layout != VK_NULL_HANDLE && kernels.pack_layout != VK_NULL_HANDLE)
	{
		kernels.scan = CreateKernelPipeline(gctx->device, gctx->pipeline_cache, kernels.scan_layout, FragmentTransport::Kernel::Scan);
		kernels.pack = CreateKernelPipeline(gctx->device, gctx->pipeline_cache, kernels.pack_layout, FragmentTransport::Kernel::Pack);
	}
	kernels.ready = kernels.scan != VK_NULL_HANDLE && kernels.pack != VK_NULL_HANDLE;
	return kernels.ready;
}

void Barrier(CommandBuffer* buffer, VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
             VkAccessFlags dst_access)
{
	VkMemoryBarrier barrier {};
	barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = src_access;
	barrier.dstAccessMask = dst_access;
	vkCmdPipelineBarrier(buffer->RecordOutsideRenderPass(), src_stage, dst_stage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

FragmentTransport::Limits HostLimits(const GraphicContext& context)
{
	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(context.physical_device, &properties);
	return {properties.limits.maxStorageBufferRange, FragmentTransport::MAX_TRANSIENT_BYTES, properties.limits.maxComputeWorkGroupCount[0]};
}

// The capture records one quad per rasterized 2x2 tile and per primitive. A
// band is planned for twice its pixel area so ordinary overdraw fits; a draw
// that exceeds the plan reports capacity_error in the control buffer.
uint32_t BandCapacity(VkExtent2D extent, uint32_t band_height)
{
	const uint64_t quads = static_cast<uint64_t>(extent.width / 2u) * ((static_cast<uint64_t>(band_height) + 1u) / 2u);
	const uint64_t capacity = quads * 2u + extent.width + 1024u;
	return static_cast<uint32_t>(std::min<uint64_t>(capacity, 0x00ffffffu));
}

bool PlanBand(VkExtent2D extent, uint32_t primitive_count, uint32_t lane_words, const FragmentTransport::Limits& limits,
              uint32_t* band_height, FragmentTransport::Layout* layout)
{
	uint32_t best = 0;
	uint32_t low  = 2;
	uint32_t high = std::max<uint32_t>(extent.height, 2u);
	while (low <= high)
	{
		const uint32_t middle = low + (high - low) / 2u;
		FragmentTransport::Layout candidate;
		const auto request = FragmentTransport::Request {BandCapacity(extent, middle), primitive_count, lane_words, 0u};
		if (FragmentTransport::BuildLayout(request, limits, &candidate).failure == FragmentTransport::Failure::None)
		{
			best = middle;
			*layout = candidate;
			low = middle + 1u;
		} else
		{
			if (middle == 0u)
			{
				break;
			}
			high = middle - 1u;
		}
	}
	*band_height = best;
	return best != 0u;
}

} // namespace

VkDescriptorSetLayout FragmentTransportDescriptorSetLayout()
{
	return EnsureKernels() ? Kernels().set_layout : VK_NULL_HANDLE;
}

VkPipeline FragmentTransportScanPipeline()
{
	return EnsureKernels() ? Kernels().scan : VK_NULL_HANDLE;
}

VkPipeline FragmentTransportPackPipeline()
{
	return EnsureKernels() ? Kernels().pack : VK_NULL_HANDLE;
}

bool FragmentTransportPrepareResources(CommandBuffer* buffer, VkExtent2D extent, uint32_t primitive_count, uint32_t lane_words,
                                       FragmentTransportResources* resources)
{
	EXIT_IF(buffer == nullptr || resources == nullptr || lane_words == 0u || primitive_count == 0u);
	if (!EnsureKernels())
	{
		return false;
	}
	auto* gctx = g_render_ctx->GetGraphicCtx();
	const auto limits = HostLimits(*gctx);
	FragmentTransport::Layout layout;
	uint32_t band_height = 0;
	if (!PlanBand(extent, primitive_count, lane_words, limits, &band_height, &layout))
	{
		return false;
	}

	constexpr uint32_t kBufferCount = static_cast<uint32_t>(FragmentTransport::Buffer::Count);

	VulkanBuffer** targets[kBufferCount]    = {&resources->records, &resources->control, &resources->references,
	                                           &resources->wave_input, &resources->wave_output};
	VkBuffer       buffers[kBufferCount]    = {};
	for (uint32_t index = 0; index < kBufferCount; ++index)
	{
		const uint64_t bytes = std::max<uint64_t>(layout.Bytes(static_cast<FragmentTransport::Buffer>(index)), 4u);
		*targets[index]      = buffer->AllocateTransientDeviceBuffer(
            bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		if (*targets[index] == nullptr)
		{
			return false;
		}
		buffers[index] = (*targets[index])->buffer;
	}
	resources->layout      = layout;
	resources->band_height = band_height;
	resources->extent      = extent;

	resources->set = buffer->AllocateTransportDescriptorSet(Kernels().set_layout);
	if (resources->set == VK_NULL_HANDLE)
	{
		return false;
	}
	VkDescriptorBufferInfo info[kBufferCount] {};
	VkWriteDescriptorSet   writes[kBufferCount] {};
	for (uint32_t index = 0; index < kBufferCount; ++index)
	{
		info[index].buffer            = buffers[index];
		info[index].offset            = 0;
		info[index].range             = VK_WHOLE_SIZE;
		writes[index].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet          = resources->set;
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[index].pBufferInfo     = &info[index];
	}
	vkUpdateDescriptorSets(gctx->device, kBufferCount, writes, 0, nullptr);
	return true;
}

void FragmentTransportBeginTile(CommandBuffer* buffer, const FragmentTransportResources& resources)
{
	// Order the previous tile's shader reads before reusing the storage.
	Barrier(buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

	auto cmd = buffer->RecordOutsideRenderPass();
	vkCmdFillBuffer(cmd, resources.control->buffer, 0, VK_WHOLE_SIZE, 0u);
	vkCmdFillBuffer(cmd, resources.references->buffer, 0, VK_WHOLE_SIZE, 0xffffffffu);

	Barrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void FragmentTransportDispatchScanPack(CommandBuffer* buffer, const FragmentTransportResources& resources)
{
	// The capture pass wrote records through the fragment stage.
	Barrier(buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

	auto*      gctx   = g_render_ctx->GetGraphicCtx();
	const auto params = FragmentTransport::GetKernelParameters(resources.layout);
	auto       cmd    = buffer->RecordOutsideRenderPass();
	auto&      kernels = Kernels();

	vkCmdPushConstants(cmd, kernels.scan_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, kTransportPushBytes, &params);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernels.scan);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernels.scan_layout, 0, 1, &resources.set, 0, nullptr);
	vkCmdDispatch(cmd, (resources.layout.wave_lookup_capacity + 63u) / 64u, 1u, 1u);

	Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

	cmd = buffer->RecordOutsideRenderPass();
	vkCmdPushConstants(cmd, kernels.pack_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, kTransportPushBytes, &params);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernels.pack);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernels.pack_layout, 0, 1, &resources.set, 0, nullptr);
	vkCmdDispatch(cmd, resources.layout.wave_capacity, 1u, 1u);

	Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void FragmentTransportRecordShade(CommandBuffer* buffer, const FragmentTransportResources& resources,
                                  const FragmentTransportPasses& passes, const ShaderBindResources& ps_bind,
                                  const ShaderPixelInputInfo& pixel)
{
	(void)ps_bind;
	(void)pixel;
	auto cmd = buffer->RecordOutsideRenderPass();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, passes.shade);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, passes.shade_layout, 2, 1, &resources.set, 0, nullptr);
	vkCmdDispatch(cmd, resources.layout.wave_capacity, 1u, 1u);

	// The resolve pass reads the shaded lanes as a fragment-stage storage buffer.
	Barrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void FragmentTransportEndTile(CommandBuffer* buffer)
{
	buffer->CloseRenderPass();
	Barrier(buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void FragmentTransportDestroyPasses(FragmentTransportPasses* passes)
{
	if (passes == nullptr)
	{
		return;
	}
	auto* gctx = g_render_ctx != nullptr ? g_render_ctx->GetGraphicCtx() : nullptr;
	if (gctx != nullptr)
	{
		auto destroy = [gctx](VulkanPipeline* pipeline)
		{
			if (pipeline == nullptr)
			{
				return;
			}
			if (pipeline->pipeline != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(gctx->device, pipeline->pipeline, nullptr);
			}
			if (pipeline->pipeline_layout != VK_NULL_HANDLE)
			{
				vkDestroyPipelineLayout(gctx->device, pipeline->pipeline_layout, nullptr);
			}
			delete pipeline;
		};
		destroy(passes->capture);
		if (passes->shade != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(gctx->device, passes->shade, nullptr);
		}
		if (passes->shade_layout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(gctx->device, passes->shade_layout, nullptr);
		}
	}
	delete passes->capture_static;
	delete passes->capture_dynamic;
	// resolve is owned by the pipeline cache entry.
	passes->resolve = nullptr;
	delete passes;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
