#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderDescriptorLayoutPlan.h"

UT_BEGIN(EmulatorShaderDescriptorLayoutPlan);

using namespace Libs::Graphics;
using DescriptorType = ShaderDescriptorLimits::DescriptorType;

static ShaderBindResources Resources()
{
	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num                 = 2;
	bind.storage_buffers.binding_index               = 0;
	bind.textures2D.textures_num                     = 12;
	bind.textures2D.textures2d_sampled_num           = 3;
	bind.textures2D.textures2d_sampled_depth_num     = 1;
	bind.textures2D.textures2d_array_sampled_num     = 4;
	bind.textures2D.textures3d_sampled_num           = 2;
	bind.textures2D.textures2d_storage_num           = 2;
	bind.textures2D.binding_sampled_index            = 1;
	bind.textures2D.binding_storage_index            = 2;
	bind.textures2D.binding_sampled_array_index      = 3;
	bind.textures2D.binding_sampled_3d_index         = 4;
	bind.textures2D.binding_sampled_uint_index       = 5;
	bind.textures2D.binding_sampled_array_uint_index = 6;
	bind.textures2D.binding_sampled_3d_uint_index    = 7;
	bind.textures2D.binding_sampled_depth_index      = 8;
	bind.samplers.samplers_num                       = 3;
	bind.samplers.binding_index                      = 9;
	bind.gds_pointers.pointers_num                   = 1;
	bind.gds_pointers.binding_index                  = 10;
	bind.vsharp_uniform_buffer                       = true;
	bind.vsharp_binding_index                        = 11;
	return bind;
}

static uint32_t Count(const ShaderDescriptorLayoutPlan& plan, uint32_t binding, DescriptorType type)
{
	for (size_t i = 0; i < plan.binding_count; ++i)
	{
		if (plan.bindings[i].binding == binding && plan.bindings[i].type == type)
		{
			return plan.bindings[i].count;
		}
	}
	return 0;
}

TEST(EmulatorShaderDescriptorLayoutPlan, MatchesSeparateShaderImageArraysWithoutUnusedNumericBanks)
{
	ShaderDescriptorLayoutPlan plan {};
	ASSERT_TRUE(ShaderBuildDescriptorLayoutPlan(Resources(), &plan));
	EXPECT_EQ(plan.binding_count, 9u);
	EXPECT_EQ(Count(plan, 1, DescriptorType::SampledImage), 3u);
	EXPECT_EQ(Count(plan, 8, DescriptorType::SampledImage), 1u);
	EXPECT_EQ(Count(plan, 3, DescriptorType::SampledImage), 4u);
	EXPECT_EQ(Count(plan, 4, DescriptorType::SampledImage), 2u);
	EXPECT_EQ(Count(plan, 5, DescriptorType::SampledImage), 0u);
	EXPECT_EQ(Count(plan, 6, DescriptorType::SampledImage), 0u);
	EXPECT_EQ(Count(plan, 7, DescriptorType::SampledImage), 0u);
	EXPECT_EQ(Count(plan, 2, DescriptorType::StorageImage), 2u);
	EXPECT_EQ(Count(plan, 11, DescriptorType::UniformBuffer), 1u);
	ShaderDescriptorLimits::DescriptorCounts counts {};
	ASSERT_EQ(ShaderCountDescriptorLayoutPlan(plan, &counts).failure, ShaderDescriptorLimits::Failure::None);
	EXPECT_EQ(counts.sampled_images, 10u);
	EXPECT_EQ(counts.storage_buffers, 3u);
}

TEST(EmulatorShaderDescriptorLayoutPlan, RetainsAllDeclaredNumericBanksForMixedImages)
{
	auto bind                                   = Resources();
	bind.textures2D.textures2d_sampled_uint_num = 1;
	ShaderDescriptorLayoutPlan plan {};
	ASSERT_TRUE(ShaderBuildDescriptorLayoutPlan(bind, &plan));
	// Numeric dispatch may statically reference both banks of every shape,
	// even when only one shape currently contains unsigned descriptors.
	EXPECT_EQ(Count(plan, 1, DescriptorType::SampledImage), 3u);
	EXPECT_EQ(Count(plan, 5, DescriptorType::SampledImage), 3u);
	EXPECT_EQ(Count(plan, 6, DescriptorType::SampledImage), 4u);
	EXPECT_EQ(Count(plan, 7, DescriptorType::SampledImage), 2u);
}

TEST(EmulatorShaderDescriptorLayoutPlan, DistinguishesEqualTotalsWithDifferentImageShapes)
{
	auto a                                    = Resources();
	auto b                                    = a;
	b.textures2D.textures2d_sampled_num       = 4;
	b.textures2D.textures2d_array_sampled_num = 3;
	ShaderDescriptorLayoutPlan first {}, second {};
	ASSERT_TRUE(ShaderBuildDescriptorLayoutPlan(a, &first));
	ASSERT_TRUE(ShaderBuildDescriptorLayoutPlan(b, &second));
	EXPECT_NE(first.CacheKey(1u), second.CacheKey(1u));
	EXPECT_NE(first.CacheKey(1u), first.CacheKey(2u));
}

TEST(EmulatorShaderDescriptorLayoutPlan, RejectsInvalidCountsAndBindingCollisions)
{
	auto                       bind = Resources();
	ShaderDescriptorLayoutPlan plan {};
	bind.textures2D.textures3d_sampled_num = -1;
	EXPECT_FALSE(ShaderBuildDescriptorLayoutPlan(bind, &plan));
	bind                                   = Resources();
	bind.textures2D.textures2d_sampled_num = ShaderTextureResources::RES_MAX + 1;
	EXPECT_FALSE(ShaderBuildDescriptorLayoutPlan(bind, &plan));
	bind                      = Resources();
	bind.vsharp_binding_index = bind.textures2D.binding_storage_index;
	EXPECT_FALSE(ShaderBuildDescriptorLayoutPlan(bind, &plan));
	EXPECT_FALSE(ShaderBuildDescriptorLayoutPlan(bind, nullptr));
}

UT_END();
