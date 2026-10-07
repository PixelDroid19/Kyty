#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/ShaderMetadataResourceIndex.h"

UT_BEGIN(EmulatorShaderMetadataResourceIndex);

using namespace Libs::Graphics;

static ShaderTextureDescriptor Image(ShaderGen5SampledTextureShape shape, bool unsigned_image = false, bool storage = false)
{
	ShaderTextureDescriptor image{};
	image.texture.fields[1]              = (unsigned_image ? 20u : 14u) << 20u;
	image.sampled_shape                  = shape;
	image.sampled_shape_from_instruction = true;
	image.textures2d_without_sampler     = storage;
	image.usage                          = storage ? ShaderTextureUsage::ReadWrite : ShaderTextureUsage::ReadOnly;
	return image;
}

static ShaderBindResources Resources()
{
	ShaderBindResources bind{};
	bind.storage_buffers.buffers_num             = 2;
	bind.textures2D.textures_num                 = 5;
	bind.textures2D.textures2d_sampled_num       = 2;
	bind.textures2D.textures2d_array_sampled_num = 1;
	bind.textures2D.textures3d_sampled_num       = 1;
	bind.textures2D.textures2d_storage_num       = 1;
	bind.textures2D.desc[0]                      = Image(ShaderGen5SampledTextureShape::TwoDimensional);
	bind.textures2D.desc[1]                      = Image(ShaderGen5SampledTextureShape::TwoDimensionalArray);
	bind.textures2D.desc[2]                      = Image(ShaderGen5SampledTextureShape::TwoDimensional, false, true);
	bind.textures2D.desc[3]                      = Image(ShaderGen5SampledTextureShape::ThreeDimensional);
	bind.textures2D.desc[4]                      = Image(ShaderGen5SampledTextureShape::TwoDimensional);
	bind.samplers.samplers_num                   = 2;
	return bind;
}

TEST(EmulatorShaderMetadataResourceIndex, UsesIndependentImageBanksAndDenseSamplerIndices)
{
	const auto bind           = Resources();
	uint32_t value            = 99;
	const uint32_t expected[] = {0u, ShaderTextureResources::TWO_DIMENSIONAL_ARRAY_INDEX_TAG, 0u,
	                             ShaderTextureResources::THREE_DIMENSIONAL_INDEX_TAG, 1u};
	for (int i = 0; i < 5; ++i)
	{
		ASSERT_TRUE(ShaderKnownMetadataResourceIndex(bind, true, 2 + 2 * i, 0, &value));
		EXPECT_EQ(value, expected[i]);
	}
	ASSERT_TRUE(ShaderKnownMetadataResourceIndex(bind, true, 13, 0, &value));
	EXPECT_EQ(value, 1u);
}

TEST(EmulatorShaderMetadataResourceIndex, PreservesMixedNumericBanksAndDepthBank)
{
	auto bind                                   = Resources();
	bind.textures2D.desc[4]                     = Image(ShaderGen5SampledTextureShape::TwoDimensional, true);
	bind.textures2D.textures2d_sampled_uint_num = 1;
	uint32_t value                              = 99;
	ASSERT_TRUE(ShaderKnownMetadataResourceIndex(bind, true, 10, 0, &value));
	EXPECT_EQ(value, ShaderTextureResources::UNSIGNED_INTEGER_INDEX_TAG);
	bind.textures2D.desc[0].sample_operation     = State::ImageSampleOperation::DepthReference;
	bind.textures2D.desc[4]                      = Image(ShaderGen5SampledTextureShape::TwoDimensional);
	bind.textures2D.textures2d_sampled_uint_num  = 0;
	bind.textures2D.textures2d_sampled_num       = 1;
	bind.textures2D.textures2d_sampled_depth_num = 1;
	ASSERT_TRUE(ShaderKnownMetadataResourceIndex(bind, true, 10, 0, &value));
	EXPECT_EQ(value, 0u);
}

TEST(EmulatorShaderMetadataResourceIndex, KeepsLegacyImagesInOneUntaggedSampledBank)
{
	const auto bind = Resources();
	uint32_t value  = 99;
	ASSERT_TRUE(ShaderKnownMetadataResourceIndex(bind, false, 10, 0, &value));
	EXPECT_EQ(value, 3u);
}

TEST(EmulatorShaderMetadataResourceIndex, LeavesMutableWordsAndInvalidInputsUnresolved)
{
	auto bind      = Resources();
	uint32_t value = 99;
	for (const int row : {0, 1, 3, 14, 99})
	{
		EXPECT_FALSE(ShaderKnownMetadataResourceIndex(bind, true, row, 0, &value));
	}
	EXPECT_FALSE(ShaderKnownMetadataResourceIndex(bind, true, 2, 1, &value));
	EXPECT_FALSE(ShaderKnownMetadataResourceIndex(bind, true, -1, 0, &value));
	EXPECT_FALSE(ShaderKnownMetadataResourceIndex(bind, true, 2, 0, nullptr));
	bind.textures2D.textures_num = ShaderTextureResources::RES_MAX + 1;
	EXPECT_FALSE(ShaderKnownMetadataResourceIndex(bind, true, 2, 0, &value));
	EXPECT_EQ(value, 99u);
}

UT_END();
