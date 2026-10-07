#pragma once

#include <cstddef>
#include <cstdint>

namespace Kyty::Libs::Graphics::ShaderDescriptorLimits {

enum class DescriptorType : uint8_t
{
	Unknown,
	SampledImage,
	StorageImage,
	Sampler,
	StorageBuffer,
	UniformBuffer,
};

struct DescriptorBindingCount
{
	DescriptorType type  = DescriptorType::Unknown;
	uint64_t       count = 0;
};

struct DescriptorCounts
{
	uint64_t sampled_images  = 0;
	uint64_t storage_images  = 0;
	uint64_t samplers        = 0;
	uint64_t storage_buffers = 0;
	uint64_t uniform_buffers = 0;
};

struct DeviceLimits
{
	uint64_t max_per_stage_sampled_images  = 0;
	uint64_t max_per_stage_storage_images  = 0;
	uint64_t max_per_stage_samplers        = 0;
	uint64_t max_per_stage_storage_buffers = 0;
	uint64_t max_per_stage_uniform_buffers = 0;
	uint64_t max_per_stage_resources       = 0;
	uint64_t max_pipeline_sampled_images   = 0;
	uint64_t max_pipeline_storage_images   = 0;
	uint64_t max_pipeline_samplers         = 0;
	uint64_t max_pipeline_storage_buffers  = 0;
	uint64_t max_pipeline_uniform_buffers  = 0;
	uint64_t max_fragment_combined_output_resources = 0;
};

enum class Failure : uint8_t
{
	None,
	InvalidArgument,
	UnsupportedDescriptorType,
	ArithmeticOverflow,
	PerStageSampledImages,
	PerStageStorageImages,
	PerStageSamplers,
	PerStageStorageBuffers,
	PerStageUniformBuffers,
	PerStageResources,
	PipelineSampledImages,
	PipelineStorageImages,
	PipelineSamplers,
	PipelineStorageBuffers,
	PipelineUniformBuffers,
	FragmentCombinedOutputResources,
};

struct Check
{
	Failure   failure  = Failure::None;
	uint64_t requested = 0;
	uint64_t limit     = 0;
};

// Builds the expected descriptor totals for a generated layout. sampled_descriptors is the
// count placed in each of the seven sampled-image bindings in GraphicsRenderDescriptor.cpp.
[[nodiscard]] Check CountLayout(int64_t storage_buffers, int64_t sampled_descriptors, int64_t storage_images,
                                int64_t samplers, int64_t gds_buffers, bool has_vsharp_uniform_buffer,
                                DescriptorCounts* counts) noexcept;

// Accumulates the descriptor types emitted by one generated VkDescriptorSetLayout.
// The input should be the same binding records passed to vkCreateDescriptorSetLayout.
[[nodiscard]] Check CountBindings(const DescriptorBindingCount* bindings, size_t binding_count,
                                  DescriptorCounts* counts) noexcept;

// Adds type totals for multiple descriptor-set layouts, as required by pipeline-layout limits.
[[nodiscard]] Check AddCounts(const DescriptorCounts& a, const DescriptorCounts& b,
                              DescriptorCounts* sum) noexcept;

// Validates one shader stage's accessible descriptors. Vulkan's maxPerStageResources excludes
// standalone sampler descriptors; it counts sampled/storage images and uniform/storage buffers.
// Framebuffer color attachments count as resources for the fragment stage,
// but must not be charged to any descriptor class or pipeline-set total.
[[nodiscard]] Check CheckPerStage(const DescriptorCounts& counts, const DeviceLimits& limits,
                                  uint64_t color_attachments = 0) noexcept;

[[nodiscard]] Check CheckFragment(const DescriptorCounts& counts, const DeviceLimits& limits,
                                  uint64_t framebuffer_color_attachments, uint64_t shader_color_outputs) noexcept;

// Validates descriptor totals across every set in one VkPipelineLayout. This deliberately does
// not apply per-stage limits to an aggregate across stages.
[[nodiscard]] Check CheckPipelineLayout(const DescriptorCounts& counts, const DeviceLimits& limits) noexcept;

[[nodiscard]] const char* FailureName(Failure failure) noexcept;

} // namespace Kyty::Libs::Graphics::ShaderDescriptorLimits
