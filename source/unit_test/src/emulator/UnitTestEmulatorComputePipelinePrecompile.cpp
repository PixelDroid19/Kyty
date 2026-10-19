#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ComputePipelinePrecompile.h"

#include <vector>

UT_BEGIN(EmulatorComputePipelinePrecompile);

using namespace Libs::Graphics;

static ComputePipelineRecipe SampleRecipe()
{
	ComputePipelineRecipe recipe;
	recipe.vendor_id              = 0x8086u;
	recipe.device_id              = 0x56a0u;
	recipe.stage_flags            = 2u;
	recipe.required_subgroup_size = 32u;
	recipe.push_constant_offset   = 0u;
	recipe.push_constant_size     = 128u;
	recipe.has_set_layout         = true;
	recipe.plan.binding_count     = 2;
	recipe.plan.bindings[0]       = {0u, ShaderDescriptorLimits::DescriptorType::StorageBuffer, 6u};
	recipe.plan.bindings[1]       = {3u, ShaderDescriptorLimits::DescriptorType::SampledImage, 10u};
	recipe.spirv                  = {0x07230203u, 0x00010500u, 0u, 16u, 0u, 0x00020011u};
	return recipe;
}

TEST(EmulatorComputePipelinePrecompile, RecipeSurvivesEncoding)
{
	const auto recipe = SampleRecipe();
	const auto words  = ComputePipelineRecipeEncode(recipe);

	ComputePipelineRecipe decoded;
	ASSERT_TRUE(ComputePipelineRecipeDecode(words.data(), words.size(), &decoded));
	EXPECT_EQ(decoded.vendor_id, recipe.vendor_id);
	EXPECT_EQ(decoded.device_id, recipe.device_id);
	EXPECT_EQ(decoded.stage_flags, recipe.stage_flags);
	EXPECT_EQ(decoded.required_subgroup_size, recipe.required_subgroup_size);
	EXPECT_EQ(decoded.push_constant_size, recipe.push_constant_size);
	EXPECT_TRUE(decoded.has_set_layout);
	ASSERT_EQ(decoded.plan.binding_count, 2u);
	EXPECT_EQ(decoded.plan.bindings[1].binding, 3u);
	EXPECT_EQ(decoded.plan.bindings[1].type, ShaderDescriptorLimits::DescriptorType::SampledImage);
	EXPECT_EQ(decoded.plan.bindings[1].count, 10u);
	EXPECT_EQ(decoded.spirv, recipe.spirv);
	EXPECT_EQ(ComputePipelineRecipeHash(decoded), ComputePipelineRecipeHash(recipe));
}

TEST(EmulatorComputePipelinePrecompile, HashFollowsEveryCreateInput)
{
	const auto base       = ComputePipelineRecipeHash(SampleRecipe());
	auto       flags      = SampleRecipe();
	auto       subgroup   = SampleRecipe();
	auto       binding    = SampleRecipe();
	flags.stage_flags     = 0u;
	subgroup.required_subgroup_size = 0u;
	binding.plan.bindings[0].count  = 5u;
	EXPECT_NE(ComputePipelineRecipeHash(flags), base);
	EXPECT_NE(ComputePipelineRecipeHash(subgroup), base);
	EXPECT_NE(ComputePipelineRecipeHash(binding), base);
}

TEST(EmulatorComputePipelinePrecompile, RejectsDamagedRecipes)
{
	const auto            words = ComputePipelineRecipeEncode(SampleRecipe());
	ComputePipelineRecipe decoded;

	EXPECT_FALSE(ComputePipelineRecipeDecode(words.data(), words.size() - 1u, &decoded));

	auto magic = words;
	magic[0] ^= 1u;
	EXPECT_FALSE(ComputePipelineRecipeDecode(magic.data(), magic.size(), &decoded));

	auto bindings = words;
	bindings[10]  = ShaderDescriptorLayoutPlan::MAX_BINDINGS + 1u;
	EXPECT_FALSE(ComputePipelineRecipeDecode(bindings.data(), bindings.size(), &decoded));

	auto type = words;
	type[12]  = 0u;
	EXPECT_FALSE(ComputePipelineRecipeDecode(type.data(), type.size(), &decoded));

	auto spirv = words;
	spirv[11 + 2 * 3 + 1] = 0u;
	EXPECT_FALSE(ComputePipelineRecipeDecode(spirv.data(), spirv.size(), &decoded));

	auto grown = words;
	grown.push_back(0u);
	EXPECT_FALSE(ComputePipelineRecipeDecode(grown.data(), grown.size(), &decoded));
}

UT_END();
