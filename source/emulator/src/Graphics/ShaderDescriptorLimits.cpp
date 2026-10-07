#include "Emulator/Graphics/ShaderDescriptorLimits.h"

#include <limits>

namespace Kyty::Libs::Graphics::ShaderDescriptorLimits {

namespace {

[[nodiscard]] bool Add(uint64_t a, uint64_t b, uint64_t* result) noexcept
{
	if (result == nullptr || a > std::numeric_limits<uint64_t>::max() - b)
	{
		return false;
	}
	*result = a + b;
	return true;
}

[[nodiscard]] bool Multiply(uint64_t a, uint64_t b, uint64_t* result) noexcept
{
	if (result == nullptr || (a != 0 && b > std::numeric_limits<uint64_t>::max() / a))
	{
		return false;
	}
	*result = a * b;
	return true;
}

[[nodiscard]] Check LimitCheck(Failure failure, uint64_t requested, uint64_t limit) noexcept
{
	return Check {failure, requested, limit};
}

} // namespace

Check CountLayout(int64_t storage_buffers, int64_t sampled_descriptors, int64_t storage_images, int64_t samplers,
                  int64_t gds_buffers, bool has_vsharp_uniform_buffer, DescriptorCounts* counts) noexcept
{
	if (counts == nullptr || storage_buffers < 0 || sampled_descriptors < 0 || storage_images < 0 || samplers < 0 || gds_buffers < 0)
	{
		return LimitCheck(Failure::InvalidArgument, 0, 0);
	}

	constexpr uint64_t SAMPLED_IMAGE_BINDINGS = 7;
	DescriptorCounts result {};
	if (!Multiply(static_cast<uint64_t>(sampled_descriptors), SAMPLED_IMAGE_BINDINGS, &result.sampled_images) ||
	    !Add(static_cast<uint64_t>(storage_buffers), static_cast<uint64_t>(gds_buffers), &result.storage_buffers))
	{
		return LimitCheck(Failure::ArithmeticOverflow, 0, std::numeric_limits<uint64_t>::max());
	}
	result.storage_images  = static_cast<uint64_t>(storage_images);
	result.samplers        = static_cast<uint64_t>(samplers);
	result.uniform_buffers = has_vsharp_uniform_buffer ? 1u : 0u;
	*counts                = result;
	return {};
}

Check CountBindings(const DescriptorBindingCount* bindings, size_t binding_count, DescriptorCounts* counts) noexcept
{
	if (counts == nullptr || (bindings == nullptr && binding_count != 0))
	{
		return LimitCheck(Failure::InvalidArgument, 0, 0);
	}

	DescriptorCounts result {};
	for (size_t i = 0; i < binding_count; i++)
	{
		uint64_t* total = nullptr;
		switch (bindings[i].type)
		{
			case DescriptorType::SampledImage: total = &result.sampled_images; break;
			case DescriptorType::StorageImage: total = &result.storage_images; break;
			case DescriptorType::Sampler: total = &result.samplers; break;
			case DescriptorType::StorageBuffer: total = &result.storage_buffers; break;
			case DescriptorType::UniformBuffer: total = &result.uniform_buffers; break;
			default: return LimitCheck(Failure::UnsupportedDescriptorType, bindings[i].count, 0);
		}
		uint64_t next_total = 0;
		if (!Add(*total, bindings[i].count, &next_total))
		{
			return LimitCheck(Failure::ArithmeticOverflow, bindings[i].count, std::numeric_limits<uint64_t>::max());
		}
		*total = next_total;
	}

	*counts = result;
	return {};
}

Check AddCounts(const DescriptorCounts& a, const DescriptorCounts& b, DescriptorCounts* sum) noexcept
{
	if (sum == nullptr)
	{
		return LimitCheck(Failure::InvalidArgument, 0, 0);
	}

	DescriptorCounts result {};
	if (!Add(a.sampled_images, b.sampled_images, &result.sampled_images) ||
	    !Add(a.storage_images, b.storage_images, &result.storage_images) || !Add(a.samplers, b.samplers, &result.samplers) ||
	    !Add(a.storage_buffers, b.storage_buffers, &result.storage_buffers) ||
	    !Add(a.uniform_buffers, b.uniform_buffers, &result.uniform_buffers))
	{
		return LimitCheck(Failure::ArithmeticOverflow, 0, std::numeric_limits<uint64_t>::max());
	}

	*sum = result;
	return {};
}

Check CheckPerStage(const DescriptorCounts& counts, const DeviceLimits& limits, uint64_t color_attachments) noexcept
{
	if (counts.sampled_images > limits.max_per_stage_sampled_images)
	{
		return LimitCheck(Failure::PerStageSampledImages, counts.sampled_images, limits.max_per_stage_sampled_images);
	}
	if (counts.storage_images > limits.max_per_stage_storage_images)
	{
		return LimitCheck(Failure::PerStageStorageImages, counts.storage_images, limits.max_per_stage_storage_images);
	}
	if (counts.samplers > limits.max_per_stage_samplers)
	{
		return LimitCheck(Failure::PerStageSamplers, counts.samplers, limits.max_per_stage_samplers);
	}
	if (counts.storage_buffers > limits.max_per_stage_storage_buffers)
	{
		return LimitCheck(Failure::PerStageStorageBuffers, counts.storage_buffers, limits.max_per_stage_storage_buffers);
	}
	if (counts.uniform_buffers > limits.max_per_stage_uniform_buffers)
	{
		return LimitCheck(Failure::PerStageUniformBuffers, counts.uniform_buffers, limits.max_per_stage_uniform_buffers);
	}

	uint64_t resources = 0;
	if (!Add(counts.sampled_images, counts.storage_images, &resources) ||
	    !Add(resources, counts.storage_buffers, &resources) || !Add(resources, counts.uniform_buffers, &resources) ||
	    !Add(resources, color_attachments, &resources))
	{
		return LimitCheck(Failure::ArithmeticOverflow, 0, std::numeric_limits<uint64_t>::max());
	}
	if (resources > limits.max_per_stage_resources)
	{
		return LimitCheck(Failure::PerStageResources, resources, limits.max_per_stage_resources);
	}
	return {};
}

Check CheckFragment(const DescriptorCounts& counts, const DeviceLimits& limits, uint64_t framebuffer_color_attachments,
                    uint64_t shader_color_outputs) noexcept
{
	const auto stage = CheckPerStage(counts, limits, framebuffer_color_attachments);
	if (stage.failure != Failure::None) { return stage; }
	uint64_t outputs = 0;
	if (!Add(counts.storage_buffers, counts.storage_images, &outputs) || !Add(outputs, shader_color_outputs, &outputs))
	{
		return LimitCheck(Failure::ArithmeticOverflow, 0, std::numeric_limits<uint64_t>::max());
	}
	if (outputs > limits.max_fragment_combined_output_resources)
	{
		return LimitCheck(Failure::FragmentCombinedOutputResources, outputs, limits.max_fragment_combined_output_resources);
	}
	return {};
}

Check CheckPipelineLayout(const DescriptorCounts& counts, const DeviceLimits& limits) noexcept
{
	if (counts.sampled_images > limits.max_pipeline_sampled_images)
	{
		return LimitCheck(Failure::PipelineSampledImages, counts.sampled_images, limits.max_pipeline_sampled_images);
	}
	if (counts.storage_images > limits.max_pipeline_storage_images)
	{
		return LimitCheck(Failure::PipelineStorageImages, counts.storage_images, limits.max_pipeline_storage_images);
	}
	if (counts.samplers > limits.max_pipeline_samplers)
	{
		return LimitCheck(Failure::PipelineSamplers, counts.samplers, limits.max_pipeline_samplers);
	}
	if (counts.storage_buffers > limits.max_pipeline_storage_buffers)
	{
		return LimitCheck(Failure::PipelineStorageBuffers, counts.storage_buffers, limits.max_pipeline_storage_buffers);
	}
	if (counts.uniform_buffers > limits.max_pipeline_uniform_buffers)
	{
		return LimitCheck(Failure::PipelineUniformBuffers, counts.uniform_buffers, limits.max_pipeline_uniform_buffers);
	}
	return {};
}

const char* FailureName(Failure failure) noexcept
{
	switch (failure)
	{
		case Failure::None: return "none";
		case Failure::InvalidArgument: return "invalid descriptor limit preflight input";
		case Failure::UnsupportedDescriptorType: return "unsupported descriptor type";
		case Failure::ArithmeticOverflow: return "descriptor count arithmetic overflow";
		case Failure::PerStageSampledImages: return "maxPerStageDescriptorSampledImages";
		case Failure::PerStageStorageImages: return "maxPerStageDescriptorStorageImages";
		case Failure::PerStageSamplers: return "maxPerStageDescriptorSamplers";
		case Failure::PerStageStorageBuffers: return "maxPerStageDescriptorStorageBuffers";
		case Failure::PerStageUniformBuffers: return "maxPerStageDescriptorUniformBuffers";
		case Failure::PerStageResources: return "maxPerStageResources";
		case Failure::PipelineSampledImages: return "maxDescriptorSetSampledImages";
		case Failure::PipelineStorageImages: return "maxDescriptorSetStorageImages";
		case Failure::PipelineSamplers: return "maxDescriptorSetSamplers";
		case Failure::PipelineStorageBuffers: return "maxDescriptorSetStorageBuffers";
		case Failure::PipelineUniformBuffers: return "maxDescriptorSetUniformBuffers";
		case Failure::FragmentCombinedOutputResources: return "maxFragmentCombinedOutputResources";
	}
	return "unknown descriptor limit failure";
}

} // namespace Kyty::Libs::Graphics::ShaderDescriptorLimits
