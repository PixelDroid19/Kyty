#include "VulkanComputeProbe.h"
#include "VulkanComputeProbeInternal.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace Kyty::Libs::Graphics {

namespace VulkanComputeProbeInternal {

void SetVkError(std::string* message, const char* operation, VkResult result)
{
	if (message != nullptr) { *message = std::string(operation) + " failed with VkResult " + std::to_string(static_cast<int>(result)); }
}

namespace {

constexpr uint64_t     kFenceTimeoutNs  = 5'000'000'000ull;

bool CheckVk(VkResult result, const char* operation, std::string* message)
{
	if (result == VK_SUCCESS) { return true; }
	SetVkError(message, operation, result);
	return false;
}

bool FindMemoryType(const VkPhysicalDeviceMemoryProperties& properties, uint32_t memory_type_bits,
                    VkMemoryPropertyFlags required, uint32_t* memory_type)
{
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((memory_type_bits & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & required) == required)
		{
			*memory_type = index;
			return true;
		}
	}
	return false;
}

// Covers the whole allocation, so flush and invalidate need no atom-alignment arithmetic.
VkMappedMemoryRange WholeMemoryRange(VkDeviceMemory memory)
{
	VkMappedMemoryRange range {};
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = memory;
	range.offset = 0;
	range.size = VK_WHOLE_SIZE;
	return range;
}

// Allocates, binds and maps host-visible memory for the buffer. A compatible HOST_COHERENT type is preferred;
// otherwise a compatible noncoherent type is used and reported through `coherent`.
VulkanComputeProbe::Result AllocateHostVisibleMemory(VkDevice device, VkPhysicalDevice physical_device, VkBuffer buffer,
                                                     VkDeviceMemory* memory, void** mapped, bool* coherent, std::string* message)
{
	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(device, buffer, &requirements);
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
	const auto host_visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	uint32_t memory_type = 0;
	if (!FindMemoryType(properties, requirements.memoryTypeBits, host_visible | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &memory_type) &&
	    !FindMemoryType(properties, requirements.memoryTypeBits, host_visible, &memory_type))
	{
		*message = "no buffer-compatible host-visible Vulkan memory type";
		return VulkanComputeProbe::Result::Unavailable;
	}
	*coherent = (properties.memoryTypes[memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

	VkMemoryAllocateInfo allocation {};
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = memory_type;
	if (!CheckVk(vkAllocateMemory(device, &allocation, nullptr, memory), "vkAllocateMemory", message) ||
	    !CheckVk(vkBindBufferMemory(device, buffer, *memory, 0), "vkBindBufferMemory", message) ||
	    !CheckVk(vkMapMemory(device, *memory, 0, requirements.size, 0, mapped), "vkMapMemory", message))
	{
		return VulkanComputeProbe::Result::Failure;
	}
	return VulkanComputeProbe::Result::Success;
}

} // namespace

DispatchObjects::~DispatchObjects()
{
	if (mapped != nullptr) { vkUnmapMemory(device, memory); }
	if (metadata_mapped != nullptr) { vkUnmapMemory(device, metadata_memory); }
	if (fence != VK_NULL_HANDLE) { vkDestroyFence(device, fence, nullptr); }
	if (command_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(device, command_pool, nullptr); }
	if (pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, pipeline, nullptr); }
	if (pipeline_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, pipeline_layout, nullptr); }
	if (descriptor_pool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, descriptor_pool, nullptr); }
	if (set_layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, set_layout, nullptr); }
	if (shader != VK_NULL_HANDLE) { vkDestroyShaderModule(device, shader, nullptr); }
	if (metadata_buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, metadata_buffer, nullptr); }
	if (buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, buffer, nullptr); }
	if (metadata_memory != VK_NULL_HANDLE) { vkFreeMemory(device, metadata_memory, nullptr); }
	if (memory != VK_NULL_HANDLE) { vkFreeMemory(device, memory, nullptr); }
}

VulkanComputeProbe::Result CreateProbeBuffer(VkDevice device, VkPhysicalDevice physical_device,
	                                         const void* initial_data, VkDeviceSize buffer_size,
	                                         DispatchObjects* objects, std::string* message)
{
	if (initial_data == nullptr || buffer_size == 0 || objects == nullptr || message == nullptr)
	{
		if (message != nullptr) { *message = "invalid probe storage buffer input"; }
		return VulkanComputeProbe::Result::Failure;
	}
	VkPhysicalDeviceProperties device_properties {};
	vkGetPhysicalDeviceProperties(physical_device, &device_properties);
	if (buffer_size > device_properties.limits.maxStorageBufferRange)
	{
		*message = "probe storage buffer exceeds maxStorageBufferRange";
		return VulkanComputeProbe::Result::Unavailable;
	}
	VkBufferCreateInfo info {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = buffer_size;
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	objects->buffer_size = buffer_size;
	if (!CheckVk(vkCreateBuffer(device, &info, nullptr, &objects->buffer), "vkCreateBuffer", message))
	{
		return VulkanComputeProbe::Result::Failure;
	}

	const auto allocated = AllocateHostVisibleMemory(device, physical_device, objects->buffer, &objects->memory,
	                                                 &objects->mapped, &objects->memory_coherent, message);
	if (allocated != VulkanComputeProbe::Result::Success) { return allocated; }
	std::memcpy(objects->mapped, initial_data, buffer_size);
	objects->output_host_dirty = true;
	return VulkanComputeProbe::Result::Success;
}

VulkanComputeProbe::Result CreateMetadataBuffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
	                                             DispatchObjects* objects, std::string* message)
{
	VkBufferCreateInfo info {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!CheckVk(vkCreateBuffer(device, &info, nullptr, &objects->metadata_buffer), "vkCreateBuffer(metadata)", message))
	{
		return VulkanComputeProbe::Result::Failure;
	}

	return AllocateHostVisibleMemory(device, physical_device, objects->metadata_buffer, &objects->metadata_memory,
	                                 &objects->metadata_mapped, &objects->metadata_coherent, message);
}

VulkanComputeProbe::Result CreateProbePipeline(VkDevice device, const uint32_t* spirv, size_t word_count,
	                                           VkDeviceSize storage_buffer_size, const ShaderBindResources* bind,
	                                           DispatchObjects* objects, std::string* message,
	                                           const ShaderComputeWaveLayout* wave_layout,
	                                           const ShaderComputeWaveCapabilities* wave_capabilities,
	                                           int output_binding)
{
	if ((wave_layout == nullptr) != (wave_capabilities == nullptr))
	{
		*message = "incomplete paired-wave pipeline state";
		return VulkanComputeProbe::Result::Failure;
	}
	const bool use_uniform = bind != nullptr && bind->vsharp_uniform_buffer;
	if (bind != nullptr && bind->descriptor_set_slot != 0)
	{
		*message = "the compute probe supports descriptor set zero only";
		return VulkanComputeProbe::Result::Failure;
	}
	if (use_uniform && bind->vsharp_binding_index < 0)
	{
		*message = "program-base UBO metadata has no descriptor binding";
		return VulkanComputeProbe::Result::Failure;
	}
	const bool use_resource_storage = output_binding >= 0;
	if (use_resource_storage &&
	    (bind == nullptr || use_uniform || bind->storage_buffers.buffers_num != 1 || bind->storage_buffers.binding_index < 0 ||
	     static_cast<uint32_t>(bind->storage_buffers.binding_index) == static_cast<uint32_t>(output_binding)))
	{
		*message = "metadata wave probe requires one storage resource at a binding distinct from its output";
		return VulkanComputeProbe::Result::Failure;
	}
	const uint32_t probe_binding = use_resource_storage ? static_cast<uint32_t>(output_binding) :
	                               use_uniform ? static_cast<uint32_t>(bind->vsharp_binding_index + 1) : 0u;
	VkDescriptorSetLayoutBinding bindings[3] {};
	uint32_t binding_count = 0;
	if (use_resource_storage)
	{
		auto& resource_binding = bindings[binding_count++];
		resource_binding.binding = static_cast<uint32_t>(bind->storage_buffers.binding_index);
		resource_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		resource_binding.descriptorCount = 1;
		resource_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	if (use_uniform)
	{
		auto& uniform_binding = bindings[binding_count++];
		uniform_binding.binding = static_cast<uint32_t>(bind->vsharp_binding_index);
		uniform_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		uniform_binding.descriptorCount = 1;
		uniform_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	auto& storage_binding = bindings[binding_count++];
	storage_binding.binding = probe_binding;
	storage_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	storage_binding.descriptorCount = 1;
	storage_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo set_layout_info {};
	set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout_info.bindingCount = binding_count;
	set_layout_info.pBindings = bindings;
	if (!CheckVk(vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &objects->set_layout),
	             "vkCreateDescriptorSetLayout", message)) { return VulkanComputeProbe::Result::Failure; }

	VkPipelineLayoutCreateInfo layout_info {};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &objects->set_layout;
	VkPushConstantRange push_range {};
	if (bind != nullptr && !use_uniform && bind->push_constant_size > 0)
	{
		push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		push_range.offset = bind->push_constant_offset;
		push_range.size = bind->push_constant_size;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = &push_range;
	}
	if (!CheckVk(vkCreatePipelineLayout(device, &layout_info, nullptr, &objects->pipeline_layout),
	             "vkCreatePipelineLayout", message)) { return VulkanComputeProbe::Result::Failure; }

	VkDescriptorPoolSize pool_sizes[2] {};
	uint32_t pool_size_count = 0;
	if (use_uniform)
	{
		pool_sizes[pool_size_count].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		pool_sizes[pool_size_count++].descriptorCount = 1;
	}
	pool_sizes[pool_size_count].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_sizes[pool_size_count++].descriptorCount = use_resource_storage ? 2u : 1u;
	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = pool_size_count;
	pool_info.pPoolSizes = pool_sizes;
	if (!CheckVk(vkCreateDescriptorPool(device, &pool_info, nullptr, &objects->descriptor_pool),
	             "vkCreateDescriptorPool", message)) { return VulkanComputeProbe::Result::Failure; }
	VkDescriptorSetAllocateInfo set_info {};
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool = objects->descriptor_pool;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts = &objects->set_layout;
	if (!CheckVk(vkAllocateDescriptorSets(device, &set_info, &objects->descriptor_set),
	             "vkAllocateDescriptorSets", message)) { return VulkanComputeProbe::Result::Failure; }
	VkDescriptorBufferInfo buffer_info {};
	buffer_info.buffer = objects->buffer;
	buffer_info.range = storage_buffer_size;
	VkWriteDescriptorSet writes[3] {};
	uint32_t write_count = 0;
	VkDescriptorBufferInfo metadata_info {};
	if (use_uniform)
	{
		if (objects->metadata_buffer == VK_NULL_HANDLE || bind->push_constant_size == 0)
		{
			*message = "program-base UBO metadata buffer is missing or empty";
			return VulkanComputeProbe::Result::Failure;
		}
		metadata_info.buffer = objects->metadata_buffer;
		metadata_info.range = bind->push_constant_size;
		auto& write = writes[write_count++];
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = objects->descriptor_set;
		write.dstBinding = static_cast<uint32_t>(bind->vsharp_binding_index);
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &metadata_info;
	}
	if (use_resource_storage)
	{
		// The metadata-only fixture does not issue resource accesses, so both
		// slots may refer to the same bounded buffer while preserving distinct
		// descriptor bindings in the generated production SPIR-V.
		auto& resource_write = writes[write_count++];
		resource_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		resource_write.dstSet = objects->descriptor_set;
		resource_write.dstBinding = static_cast<uint32_t>(bind->storage_buffers.binding_index);
		resource_write.descriptorCount = 1;
		resource_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		resource_write.pBufferInfo = &buffer_info;
	}
	auto& output_write = writes[write_count++];
	output_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	output_write.dstSet = objects->descriptor_set;
	output_write.dstBinding = probe_binding;
	output_write.descriptorCount = 1;
	output_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	output_write.pBufferInfo = &buffer_info;
	// vkUpdateDescriptorSets consumes pBufferInfo immediately.
	vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);

	VkShaderModuleCreateInfo shader_info {};
	shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader_info.codeSize = word_count * sizeof(uint32_t);
	shader_info.pCode = spirv;
	if (!CheckVk(vkCreateShaderModule(device, &shader_info, nullptr, &objects->shader), "vkCreateShaderModule", message))
	{
		return VulkanComputeProbe::Result::Failure;
	}
	VkPipelineShaderStageCreateInfo stage {};
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = objects->shader;
	stage.pName = "main";
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required_subgroup_size {};
	if (wave_layout != nullptr &&
	    !ShaderComputeWaveVulkanAttachRequiredSubgroupSize(*wave_layout, *wave_capabilities, &stage,
	                                                     &required_subgroup_size, true, spirv[1]))
	{
		*message = "wave compute stage failed subgroup-size or host-limit validation";
		return VulkanComputeProbe::Result::Unavailable;
	}
	VkComputePipelineCreateInfo pipeline_info {};
	pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage = stage;
	pipeline_info.layout = objects->pipeline_layout;
	return CheckVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &objects->pipeline),
	               "vkCreateComputePipelines", message) ? VulkanComputeProbe::Result::Success : VulkanComputeProbe::Result::Failure;
}

VulkanComputeProbe::Result SubmitAndRead(VkDevice device, VkQueue queue, uint32_t queue_family,
	                                     DispatchObjects* objects, const ShaderBindResources* bind,
	                                     const std::vector<uint32_t>* metadata,
	                                     size_t output_word_count, void* result_words, std::string* message,
	                                     const std::array<uint32_t, 3>& groups)
{
	if (output_word_count == 0 || output_word_count > std::numeric_limits<VkDeviceSize>::max() / sizeof(uint32_t))
	{
		if (message != nullptr) { *message = "invalid probe output word count"; }
		return VulkanComputeProbe::Result::Failure;
	}
	const VkDeviceSize output_size = output_word_count * sizeof(uint32_t);
	if (objects == nullptr || result_words == nullptr || message == nullptr || objects->buffer == VK_NULL_HANDLE ||
	    output_size > objects->buffer_size)
	{
		if (message != nullptr) { *message = "invalid or out-of-bounds probe submission output"; }
		return VulkanComputeProbe::Result::Failure;
	}
	if (bind != nullptr && bind->push_constant_size > 0 &&
	    (metadata == nullptr || metadata->size() * sizeof(uint32_t) != bind->push_constant_size))
	{
		*message = "program-base metadata size does not match the generated binding layout";
		return VulkanComputeProbe::Result::Failure;
	}
	if (bind != nullptr && bind->vsharp_uniform_buffer)
	{
		if (objects->metadata_mapped == nullptr || metadata == nullptr)
		{
			*message = "program-base UBO storage is unavailable";
			return VulkanComputeProbe::Result::Failure;
		}
		std::memcpy(objects->metadata_mapped, metadata->data(), bind->push_constant_size);
		if (!objects->metadata_coherent)
		{
			const auto range = WholeMemoryRange(objects->metadata_memory);
			if (!CheckVk(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges(metadata)", message))
			{
				return VulkanComputeProbe::Result::Failure;
			}
		}
	}
	// The seed is written only at creation, so it is flushed once; later submissions keep the device-written contents.
	if (objects->output_host_dirty)
	{
		if (!objects->memory_coherent)
		{
			const auto range = WholeMemoryRange(objects->memory);
			if (!CheckVk(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges", message))
			{
				return VulkanComputeProbe::Result::Failure;
			}
		}
		objects->output_host_dirty = false;
	}
	if (objects->command_pool == VK_NULL_HANDLE)
	{
		VkCommandPoolCreateInfo pool_info {};
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		pool_info.queueFamilyIndex = queue_family;
		if (!CheckVk(vkCreateCommandPool(device, &pool_info, nullptr, &objects->command_pool), "vkCreateCommandPool", message))
		{
			return VulkanComputeProbe::Result::Failure;
		}
	}
	VkCommandBufferAllocateInfo allocation {};
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = objects->command_pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	VkCommandBuffer command = VK_NULL_HANDLE;
	if (!CheckVk(vkAllocateCommandBuffers(device, &allocation, &command), "vkAllocateCommandBuffers", message))
	{
		return VulkanComputeProbe::Result::Failure;
	}
	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (!CheckVk(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer", message))
	{
		return VulkanComputeProbe::Result::Failure;
	}
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, objects->pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, objects->pipeline_layout, 0, 1,
	                        &objects->descriptor_set, 0, nullptr);
	if (bind != nullptr && !bind->vsharp_uniform_buffer && bind->push_constant_size > 0)
	{
		vkCmdPushConstants(command, objects->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
		                   bind->push_constant_offset, bind->push_constant_size, metadata->data());
	}
	vkCmdDispatch(command, groups[0], groups[1], groups[2]);
	VkBufferMemoryBarrier barrier {};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = objects->buffer;
	barrier.size = output_size;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
	                     &barrier, 0, nullptr);
	if (!CheckVk(vkEndCommandBuffer(command), "vkEndCommandBuffer", message)) { return VulkanComputeProbe::Result::Failure; }
	if (objects->fence == VK_NULL_HANDLE)
	{
		VkFenceCreateInfo fence_info {};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (!CheckVk(vkCreateFence(device, &fence_info, nullptr, &objects->fence), "vkCreateFence", message))
		{
			return VulkanComputeProbe::Result::Failure;
		}
	} else if (!CheckVk(vkResetFences(device, 1, &objects->fence), "vkResetFences", message))
	{
		return VulkanComputeProbe::Result::Failure;
	}
	VkSubmitInfo submit {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	if (!CheckVk(vkQueueSubmit(queue, 1, &submit, objects->fence), "vkQueueSubmit", message))
	{
		return VulkanComputeProbe::Result::Failure;
	}
	const VkResult waited = vkWaitForFences(device, 1, &objects->fence, VK_TRUE, kFenceTimeoutNs);
	if (waited == VK_TIMEOUT)
	{
		std::fprintf(stderr, "Vulkan compute probe fence exceeded the bounded 5-second wait\n");
		std::fflush(stderr);
		std::_Exit(EXIT_FAILURE);
	}
	if (!CheckVk(waited, "vkWaitForFences", message)) { return VulkanComputeProbe::Result::Failure; }
	if (!objects->memory_coherent)
	{
		const auto range = WholeMemoryRange(objects->memory);
		if (!CheckVk(vkInvalidateMappedMemoryRanges(device, 1, &range), "vkInvalidateMappedMemoryRanges", message))
		{
			return VulkanComputeProbe::Result::Failure;
		}
	}
	std::memcpy(result_words, objects->mapped, output_size);
	return VulkanComputeProbe::Result::Success;
}

} // namespace VulkanComputeProbeInternal

VulkanComputeProbe::~VulkanComputeProbe()
{
	if (device_ != VK_NULL_HANDLE)
	{
		vkDestroyDevice(device_, nullptr);
	}
	if (instance_ != VK_NULL_HANDLE)
	{
		vkDestroyInstance(instance_, nullptr);
	}
}

VulkanComputeProbe::Result VulkanComputeProbe::Dispatch(const uint32_t* spirv, size_t word_count,
	                                                        const std::array<uint32_t, 4>& initial_words,
	                                                        std::array<uint32_t, 4>* result_words,
	                                                        std::string* message) const
{
	if (device_ == VK_NULL_HANDLE || spirv == nullptr || word_count == 0 || result_words == nullptr || message == nullptr ||
	    word_count > std::numeric_limits<size_t>::max() / sizeof(uint32_t))
	{
		if (message != nullptr) { *message = "invalid Vulkan compute dispatch input"; }
		return Result::Failure;
	}
	*message = "";
	VulkanComputeProbeInternal::DispatchObjects objects {};
	objects.device = device_;
	auto result = VulkanComputeProbeInternal::CreateProbeBuffer(device_, physical_device_, initial_words.data(),
	                                                           VulkanComputeProbeInternal::kBasicProbeBufferSize,
	                                                           &objects, message);
	if (result != Result::Success) { return result; }
	result = VulkanComputeProbeInternal::CreateProbePipeline(device_, spirv, word_count,
	                                                        VulkanComputeProbeInternal::kBasicProbeBufferSize,
	                                                        nullptr, &objects, message);
	if (result != Result::Success) { return result; }
	return VulkanComputeProbeInternal::SubmitAndRead(device_, queue_, queue_family_index_, &objects,
	                                                nullptr, nullptr, result_words->size(), result_words->data(), message);
}

} // namespace Kyty::Libs::Graphics
