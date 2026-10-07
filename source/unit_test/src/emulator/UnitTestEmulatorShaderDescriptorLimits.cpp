#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderDescriptorLimits.h"

#include <array>
#include <limits>

UT_BEGIN(EmulatorShaderDescriptorLimits);

using namespace Libs::Graphics::ShaderDescriptorLimits;

static DeviceLimits LimitsFor(const DescriptorCounts& counts, uint64_t per_stage_resources)
{
	DeviceLimits limits {};
	limits.max_per_stage_sampled_images  = counts.sampled_images;
	limits.max_per_stage_storage_images  = counts.storage_images;
	limits.max_per_stage_samplers        = counts.samplers;
	limits.max_per_stage_storage_buffers = counts.storage_buffers;
	limits.max_per_stage_uniform_buffers = counts.uniform_buffers;
	limits.max_per_stage_resources       = per_stage_resources;
	limits.max_pipeline_sampled_images   = counts.sampled_images;
	limits.max_pipeline_storage_images   = counts.storage_images;
	limits.max_pipeline_samplers         = counts.samplers;
	limits.max_pipeline_storage_buffers  = counts.storage_buffers;
	limits.max_pipeline_uniform_buffers  = counts.uniform_buffers;
	return limits;
}

static std::array<DescriptorBindingCount, 12> RealSixteenReadOnlyOneWritableLayout()
{
	return {{
	    {DescriptorType::StorageBuffer, 2},
	    {DescriptorType::SampledImage, 16},
	    {DescriptorType::SampledImage, 16},
	    {DescriptorType::SampledImage, 16},
	    {DescriptorType::SampledImage, 16},
	    {DescriptorType::SampledImage, 16},
	    {DescriptorType::StorageImage, 1},
	    {DescriptorType::Sampler, 2},
	    {DescriptorType::StorageBuffer, 1},
	    {DescriptorType::SampledImage, 16},
	    {DescriptorType::SampledImage, 16},
	    {DescriptorType::UniformBuffer, 1},
	}};
}

TEST(EmulatorShaderDescriptorLimits, CountsTheGeneratedSevenSampledBindingsForSixteenReadOnlyAndOneWritable)
{
	const auto                     bindings = RealSixteenReadOnlyOneWritableLayout();
	DescriptorCounts               counts {};
	const auto                     counted = CountBindings(bindings.data(), bindings.size(), &counts);
	const auto                     limits  = LimitsFor(DescriptorCounts {112, 1, 2, 3, 1}, 117);
	DescriptorCounts               expected {};
	const auto                     expected_result = CountLayout(2, 16, 1, 2, 1, true, &expected);

	ASSERT_EQ(counted.failure, Failure::None);
	ASSERT_EQ(expected_result.failure, Failure::None);
	EXPECT_EQ(counts.sampled_images, 112u);
	EXPECT_EQ(counts.storage_images, 1u);
	EXPECT_EQ(counts.samplers, 2u);
	EXPECT_EQ(counts.storage_buffers, 3u);
	EXPECT_EQ(counts.uniform_buffers, 1u);
	EXPECT_EQ(expected.sampled_images, counts.sampled_images);
	EXPECT_EQ(expected.storage_images, counts.storage_images);
	EXPECT_EQ(expected.samplers, counts.samplers);
	EXPECT_EQ(expected.storage_buffers, counts.storage_buffers);
	EXPECT_EQ(expected.uniform_buffers, counts.uniform_buffers);
	EXPECT_EQ(CheckPerStage(counts, limits).failure, Failure::None);
	EXPECT_EQ(CheckPipelineLayout(counts, limits).failure, Failure::None);
}

TEST(EmulatorShaderDescriptorLimits, ReportsTheFirstPerStageLimitAndItsRequestedCount)
{
	const auto       bindings = RealSixteenReadOnlyOneWritableLayout();
	DescriptorCounts counts {};
	ASSERT_EQ(CountBindings(bindings.data(), bindings.size(), &counts).failure, Failure::None);
	auto limits = LimitsFor(DescriptorCounts {112, 1, 2, 3, 1}, 117);
	limits.max_per_stage_sampled_images = 111;

	const auto result = CheckPerStage(counts, limits);
	EXPECT_EQ(result.failure, Failure::PerStageSampledImages);
	EXPECT_EQ(result.requested, 112u);
	EXPECT_EQ(result.limit, 111u);
}

TEST(EmulatorShaderDescriptorLimits, DoesNotCountStandaloneSamplersAsPerStageResources)
{
	const DescriptorBindingCount binding {DescriptorType::Sampler, 9};
	DescriptorCounts             counts {};
	ASSERT_EQ(CountBindings(&binding, 1, &counts).failure, Failure::None);
	auto limits                        = LimitsFor(DescriptorCounts {0, 0, 9, 0, 0}, 0);
	limits.max_per_stage_resources     = 0;
	limits.max_per_stage_samplers      = 9;

	EXPECT_EQ(CheckPerStage(counts, limits).failure, Failure::None);
}

TEST(EmulatorShaderDescriptorLimits, FragmentColorAttachmentsCountOnlyAsStageResources)
{
	const DescriptorCounts counts {2, 0, 2, 0, 0};
	auto limits = LimitsFor(counts, 2);
	EXPECT_EQ(CheckPerStage(counts, limits, 1).failure, Failure::PerStageResources);
	limits.max_per_stage_resources = 3;
	EXPECT_EQ(CheckPerStage(counts, limits, 1).failure, Failure::None);
	EXPECT_EQ(CheckPipelineLayout(counts, limits).failure, Failure::None);
}

TEST(EmulatorShaderDescriptorLimits, FragmentCombinedOutputsExcludeSampledImagesAndSamplers)
{
	const DescriptorCounts counts {7, 2, 5, 1, 0};
	auto limits = LimitsFor(counts, 12);
	limits.max_fragment_combined_output_resources = 4;
	EXPECT_EQ(CheckFragment(counts, limits, 1, 1).failure, Failure::None);
	EXPECT_EQ(CheckFragment(counts, limits, 2, 1).failure, Failure::None);
	const auto rejected = CheckFragment(counts, limits, 1, 2);
	EXPECT_EQ(rejected.failure, Failure::FragmentCombinedOutputResources);
	EXPECT_EQ(rejected.requested, 5u);
}

TEST(EmulatorShaderDescriptorLimits, PipelineStorageBufferLimitIncludesRegularAndGdsBindings)
{
	const std::array<DescriptorBindingCount, 2> bindings {{
	    {DescriptorType::StorageBuffer, 2},
	    {DescriptorType::StorageBuffer, 1},
	}};
	DescriptorCounts counts {};
	ASSERT_EQ(CountBindings(bindings.data(), bindings.size(), &counts).failure, Failure::None);
	auto limits = LimitsFor(DescriptorCounts {0, 0, 0, 3, 0}, 3);
	limits.max_pipeline_storage_buffers = 2;

	const auto result = CheckPipelineLayout(counts, limits);
	EXPECT_EQ(result.failure, Failure::PipelineStorageBuffers);
	EXPECT_EQ(result.requested, 3u);
	EXPECT_EQ(result.limit, 2u);
}

TEST(EmulatorShaderDescriptorLimits, ChecksPerStageStorageImageSamplerStorageBufferAndUniformBufferCaps)
{
	const std::array<DescriptorBindingCount, 4> bindings {{
	    {DescriptorType::StorageImage, 1},
	    {DescriptorType::Sampler, 2},
	    {DescriptorType::StorageBuffer, 3},
	    {DescriptorType::UniformBuffer, 1},
	}};
	DescriptorCounts counts {};
	ASSERT_EQ(CountBindings(bindings.data(), bindings.size(), &counts).failure, Failure::None);
	auto limits = LimitsFor(DescriptorCounts {0, 1, 2, 3, 1}, 5);

	limits.max_per_stage_storage_images = 0;
	EXPECT_EQ(CheckPerStage(counts, limits).failure, Failure::PerStageStorageImages);
	limits.max_per_stage_storage_images = 1;
	limits.max_per_stage_samplers      = 1;
	EXPECT_EQ(CheckPerStage(counts, limits).failure, Failure::PerStageSamplers);
	limits.max_per_stage_samplers        = 2;
	limits.max_per_stage_storage_buffers = 2;
	EXPECT_EQ(CheckPerStage(counts, limits).failure, Failure::PerStageStorageBuffers);
	limits.max_per_stage_storage_buffers = 3;
	limits.max_per_stage_uniform_buffers = 0;
	EXPECT_EQ(CheckPerStage(counts, limits).failure, Failure::PerStageUniformBuffers);
}

TEST(EmulatorShaderDescriptorLimits, ChecksEveryPipelineLayoutDescriptorClass)
{
	const DescriptorCounts counts {7, 5, 4, 3, 2};
	const auto             limits = LimitsFor(counts, 17);
	auto                   changed = limits;

	changed.max_pipeline_sampled_images = 6;
	EXPECT_EQ(CheckPipelineLayout(counts, changed).failure, Failure::PipelineSampledImages);
	changed = limits;
	changed.max_pipeline_storage_images = 4;
	EXPECT_EQ(CheckPipelineLayout(counts, changed).failure, Failure::PipelineStorageImages);
	changed = limits;
	changed.max_pipeline_samplers = 3;
	EXPECT_EQ(CheckPipelineLayout(counts, changed).failure, Failure::PipelineSamplers);
	changed = limits;
	changed.max_pipeline_storage_buffers = 2;
	EXPECT_EQ(CheckPipelineLayout(counts, changed).failure, Failure::PipelineStorageBuffers);
	changed = limits;
	changed.max_pipeline_uniform_buffers = 1;
	EXPECT_EQ(CheckPipelineLayout(counts, changed).failure, Failure::PipelineUniformBuffers);
}

TEST(EmulatorShaderDescriptorLimits, PipelineAggregateValidationDoesNotApplyPerStageCaps)
{
	const DescriptorCounts counts {112, 1, 2, 3, 1};
	auto                   limits = LimitsFor(counts, 117);
	limits.max_per_stage_sampled_images = 0;

	EXPECT_EQ(CheckPipelineLayout(counts, limits).failure, Failure::None);
}

TEST(EmulatorShaderDescriptorLimits, RejectsUnknownDescriptorTypesAndNullOutput)
{
	const DescriptorBindingCount binding {DescriptorType::Unknown, 1};
	DescriptorCounts             counts {};

	EXPECT_EQ(CountBindings(&binding, 1, &counts).failure, Failure::UnsupportedDescriptorType);
	EXPECT_EQ(CountBindings(&binding, 1, nullptr).failure, Failure::InvalidArgument);
}

TEST(EmulatorShaderDescriptorLimits, RejectsOverflowWhenAccumulatingCounts)
{
	const DescriptorBindingCount bindings[] {{DescriptorType::SampledImage, std::numeric_limits<uint64_t>::max()},
	                                        {DescriptorType::SampledImage, 1}};
	DescriptorCounts             counts {};

	EXPECT_EQ(CountBindings(bindings, 2, &counts).failure, Failure::ArithmeticOverflow);
	EXPECT_EQ(CountLayout(0, std::numeric_limits<int64_t>::max(), 0, 0, 0, false, &counts).failure,
	          Failure::ArithmeticOverflow);
	DescriptorCounts a {};
	DescriptorCounts b {};
	DescriptorCounts sum {};
	a.storage_buffers = std::numeric_limits<uint64_t>::max();
	b.storage_buffers = 1;
	EXPECT_EQ(AddCounts(a, b, &sum).failure, Failure::ArithmeticOverflow);
	DescriptorCounts resources_overflow {};
	resources_overflow.sampled_images = std::numeric_limits<uint64_t>::max();
	resources_overflow.storage_images = 1;
	DeviceLimits resource_limits {};
	resource_limits.max_per_stage_sampled_images = std::numeric_limits<uint64_t>::max();
	resource_limits.max_per_stage_storage_images = 1;
	resource_limits.max_per_stage_resources      = std::numeric_limits<uint64_t>::max();
	EXPECT_EQ(CheckPerStage(resources_overflow, resource_limits).failure, Failure::ArithmeticOverflow);
}

TEST(EmulatorShaderDescriptorLimits, RejectsNegativeLogicalDescriptorCounts)
{
	DescriptorCounts counts {};
	EXPECT_EQ(CountLayout(-1, 0, 0, 0, 0, false, &counts).failure, Failure::InvalidArgument);
}

UT_END();
