#include "VulkanComputeProbe.h"

#include "VulkanComputeProbeInternal.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderProgramAddress.h"

#include <limits>
#include <vector>

namespace Kyty::Libs::Graphics {

VulkanComputeProbe::Result VulkanComputeProbe::DispatchProgramBasePair(
	const uint32_t* spirv, size_t word_count, const std::array<uint32_t, 8>& initial_words,
	const ShaderComputeInputInfo& input_info, uint64_t first_program_base, uint64_t second_program_base,
	std::array<uint32_t, 8>* first_result, std::array<uint32_t, 8>* second_result, std::string* message) const
{
	if (device_ == VK_NULL_HANDLE || spirv == nullptr || word_count == 0 || first_result == nullptr || second_result == nullptr ||
	    message == nullptr || word_count > std::numeric_limits<size_t>::max() / sizeof(uint32_t) ||
	    !input_info.bind.program_base_used || input_info.bind.push_constant_size == 0 ||
	    (input_info.bind.push_constant_size & 3u) != 0 || (input_info.bind.push_constant_offset & 3u) != 0 ||
	    input_info.bind.push_constant_offset > std::numeric_limits<uint32_t>::max() - input_info.bind.push_constant_size)
	{
		if (message != nullptr) { *message = "invalid program-base Vulkan compute dispatch input"; }
		return Result::Failure;
	}
	*message = "";

	const auto& bind = input_info.bind;
	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(physical_device_, &properties);
	if ((!bind.vsharp_uniform_buffer && bind.push_constant_offset + bind.push_constant_size > properties.limits.maxPushConstantsSize) ||
	    (bind.vsharp_uniform_buffer && bind.push_constant_size > properties.limits.maxUniformBufferRange))
	{
		*message = "program-base metadata layout exceeds Vulkan push-constant or uniform-buffer limits";
		return Result::Unavailable;
	}
	if (bind.vsharp_uniform_buffer && (bind.descriptor_set_slot != 0 || bind.vsharp_binding_index < 0))
	{
		*message = "program-base UBO probe requires descriptor set zero and a valid VSharp binding";
		return Result::Failure;
	}

	auto build_metadata = [&](uint64_t program_base, std::vector<uint32_t>* metadata) {
		metadata->assign(bind.push_constant_size / sizeof(uint32_t), 0u);
		auto runtime_bind = bind;
		runtime_bind.program_base = program_base;
		return ShaderWriteProgramBaseMetadata(runtime_bind, metadata->data(), static_cast<uint32_t>(metadata->size()));
	};
	std::vector<uint32_t> first_metadata;
	std::vector<uint32_t> second_metadata;
	if (!build_metadata(first_program_base, &first_metadata) || !build_metadata(second_program_base, &second_metadata))
	{
		*message = "production program-base metadata writer rejected the configured probe layout";
		return Result::Failure;
	}

	VulkanComputeProbeInternal::DispatchObjects objects {};
	objects.device = device_;
	auto result = VulkanComputeProbeInternal::CreateProbeBuffer(device_, physical_device_, initial_words.data(),
	                                                           VulkanComputeProbeInternal::kProgramBaseProbeBufferSize,
	                                                           &objects, message);
	if (result != Result::Success) { return result; }
	if (bind.vsharp_uniform_buffer)
	{
		result = VulkanComputeProbeInternal::CreateMetadataBuffer(device_, physical_device_, bind.push_constant_size,
		                                                          &objects, message);
		if (result != Result::Success) { return result; }
	}
	result = VulkanComputeProbeInternal::CreateProbePipeline(device_, spirv, word_count,
	                                                         VulkanComputeProbeInternal::kProgramBaseProbeBufferSize,
	                                                         &bind, &objects, message);
	if (result != Result::Success) { return result; }

	// Both executions share the same pipeline, descriptor set, and output buffer.
	result = VulkanComputeProbeInternal::SubmitAndRead(device_, queue_, queue_family_index_, &objects,
	                                                  &bind, &first_metadata, first_result->size(),
	                                                  first_result->data(), message);
	if (result != Result::Success) { return result; }
	return VulkanComputeProbeInternal::SubmitAndRead(device_, queue_, queue_family_index_, &objects,
	                                                &bind, &second_metadata, second_result->size(),
	                                                second_result->data(), message);
}

} // namespace Kyty::Libs::Graphics
