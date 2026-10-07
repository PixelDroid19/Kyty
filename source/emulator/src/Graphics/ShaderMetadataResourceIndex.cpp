#include "Emulator/Graphics/ShaderMetadataResourceIndex.h"

#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/GraphicsState.h"

#include <array>

namespace Kyty::Libs::Graphics {
namespace {

bool ValidResourceCounts(const ShaderBindResources& bind) noexcept
{
	const auto& t      = bind.textures2D;
	const int counts[] = {t.textures2d_sampled_num,     t.textures2d_sampled_depth_num, t.textures2d_array_sampled_num,
	                      t.textures3d_sampled_num,     t.textures2d_sampled_uint_num,  t.textures2d_array_sampled_uint_num,
	                      t.textures3d_sampled_uint_num};
	for (int count : counts)
	{
		if (count < 0 || count > ShaderTextureResources::RES_MAX)
		{
			return false;
		}
	}
	return bind.storage_buffers.buffers_num >= 0 && bind.storage_buffers.buffers_num <= ShaderStorageResources::BUFFERS_MAX &&
	       bind.textures2D.textures_num >= 0 && bind.textures2D.textures_num <= ShaderTextureResources::RES_MAX &&
	       bind.samplers.samplers_num >= 0 && bind.samplers.samplers_num <= ShaderSamplerResources::RES_MAX;
}

bool ImageBank(const ShaderTextureDescriptor& descriptor, bool next_gen, bool split_numeric, uint32_t* bank, uint32_t* tag) noexcept
{
	*bank = descriptor.textures2d_without_sampler ? 0u : 1u;
	*tag  = 0;
	if (!next_gen || descriptor.textures2d_without_sampler)
	{
		return true;
	}

	const auto numeric = VulkanGen5ImageNumericType(descriptor.texture.Format());
	if (numeric != GuestImageNumericType::FloatingPoint && numeric != GuestImageNumericType::UnsignedInteger)
	{
		return false;
	}
	const bool unsigned_image = numeric == GuestImageNumericType::UnsignedInteger;
	if (unsigned_image)
	{
		*tag = ShaderTextureResources::UNSIGNED_INTEGER_INDEX_TAG;
	}
	const bool unsigned_bank = unsigned_image && split_numeric;
	switch (ShaderResolvedSampledTextureShape(descriptor))
	{
		case ShaderGen5SampledTextureShape::TwoDimensional:
			*bank = descriptor.sample_operation == State::ImageSampleOperation::DepthReference ? 2u : (unsigned_bank ? 5u : 1u);
			break;
		case ShaderGen5SampledTextureShape::TwoDimensionalArray:
			*bank = unsigned_bank ? 6u : 3u;
			*tag |= ShaderTextureResources::TWO_DIMENSIONAL_ARRAY_INDEX_TAG;
			break;
		case ShaderGen5SampledTextureShape::ThreeDimensional:
			*bank = unsigned_bank ? 7u : 4u;
			*tag |= ShaderTextureResources::THREE_DIMENSIONAL_INDEX_TAG;
			break;
		default:
			return false;
	}
	return true;
}

bool ImageIndex(const ShaderTextureResources& textures, bool next_gen, int target, uint32_t* value) noexcept
{
	const int sampled = textures.textures2d_sampled_num + textures.textures2d_array_sampled_num + textures.textures3d_sampled_num;
	const int unsigned_sampled =
	    textures.textures2d_sampled_uint_num + textures.textures2d_array_sampled_uint_num + textures.textures3d_sampled_uint_num;
	const bool split = next_gen && unsigned_sampled > 0 && unsigned_sampled < sampled;
	std::array<uint32_t, 8> indices{};
	for (int i = 0; i <= target; ++i)
	{
		uint32_t bank = 0;
		uint32_t tag  = 0;
		if (!ImageBank(textures.desc[i], next_gen, split, &bank, &tag))
		{
			return false;
		}
		const uint32_t index = indices[bank]++;
		if (i == target)
		{
			*value = index | tag;
		}
	}
	return true;
}

} // namespace

bool ShaderKnownMetadataResourceIndex(const ShaderBindResources& bind, bool next_gen, int row, int field, uint32_t* value) noexcept
{
	if (value == nullptr || row < 0 || field != 0 || !ValidResourceCounts(bind))
	{
		return false;
	}
	const int texture_row  = row - bind.storage_buffers.buffers_num;
	const int texture_rows = bind.textures2D.textures_num * 2;
	if (texture_row >= 0 && texture_row < texture_rows && texture_row % 2 == 0)
	{
		return ImageIndex(bind.textures2D, next_gen, texture_row / 2, value);
	}
	const int sampler = texture_row - texture_rows;
	if (sampler < 0 || sampler >= bind.samplers.samplers_num)
	{
		return false;
	}
	*value = static_cast<uint32_t>(sampler);
	return true;
}

} // namespace Kyty::Libs::Graphics
