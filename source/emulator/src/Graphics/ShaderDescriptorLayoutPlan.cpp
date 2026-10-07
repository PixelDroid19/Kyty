#include "Emulator/Graphics/ShaderDescriptorLayoutPlan.h"

#include "Emulator/Graphics/Shader.h"

namespace Kyty::Libs::Graphics {
namespace {

using Type = ShaderDescriptorLimits::DescriptorType;

bool ValidCounts(const ShaderBindResources& bind) noexcept
{
	const auto& textures         = bind.textures2D;
	const int   texture_counts[] = {textures.textures_num,
	                                textures.textures2d_sampled_num,
	                                textures.textures2d_sampled_depth_num,
	                                textures.textures2d_array_sampled_num,
	                                textures.textures3d_sampled_num,
	                                textures.textures2d_sampled_uint_num,
	                                textures.textures2d_array_sampled_uint_num,
	                                textures.textures3d_sampled_uint_num,
	                                textures.textures2d_storage_num};
	for (int count: texture_counts)
	{
		if (count < 0 || count > ShaderTextureResources::RES_MAX)
		{
			return false;
		}
	}
	const int sampled = textures.textures2d_sampled_num + textures.textures2d_sampled_depth_num + textures.textures2d_array_sampled_num +
	                    textures.textures3d_sampled_num;
	const int unsigned_sampled =
	    textures.textures2d_sampled_uint_num + textures.textures2d_array_sampled_uint_num + textures.textures3d_sampled_uint_num;
	return sampled <= ShaderTextureResources::RES_MAX && unsigned_sampled <= sampled &&
	       textures.textures2d_sampled_uint_num <= textures.textures2d_sampled_num + textures.textures2d_sampled_depth_num &&
	       textures.textures2d_array_sampled_uint_num <= textures.textures2d_array_sampled_num &&
	       textures.textures3d_sampled_uint_num <= textures.textures3d_sampled_num && bind.storage_buffers.buffers_num >= 0 &&
	       bind.storage_buffers.buffers_num <= ShaderStorageResources::BUFFERS_MAX && bind.samplers.samplers_num >= 0 &&
	       bind.samplers.samplers_num <= ShaderSamplerResources::RES_MAX && bind.gds_pointers.pointers_num >= 0 &&
	       bind.gds_pointers.pointers_num <= 1;
}

bool Add(ShaderDescriptorLayoutPlan* plan, Type type, int binding, int count) noexcept
{
	if (count == 0)
	{
		return true;
	}
	if (binding < 0 || count < 0 || plan->binding_count >= plan->bindings.size())
	{
		return false;
	}
	for (size_t i = 0; i < plan->binding_count; ++i)
	{
		if (plan->bindings[i].binding == static_cast<uint32_t>(binding))
		{
			return false;
		}
	}
	plan->bindings[plan->binding_count++] = {static_cast<uint32_t>(binding), type, static_cast<uint32_t>(count)};
	return true;
}

bool AddSampledBindings(const ShaderTextureResources& textures, ShaderDescriptorLayoutPlan* plan) noexcept
{
	const bool unsigned_banks = textures.textures2d_sampled_uint_num > 0 || textures.textures2d_array_sampled_uint_num > 0 ||
	                            textures.textures3d_sampled_uint_num > 0;
	// The generator declares unsigned banks for every shape when any sampled
	// resource is unsigned. Retain them even if a particular bank is not written
	// by this bind: numeric dispatch can statically reference that bank.
	return Add(plan, Type::SampledImage, textures.binding_sampled_index, textures.textures2d_sampled_num) &&
	       Add(plan, Type::SampledImage, textures.binding_sampled_depth_index, textures.textures2d_sampled_depth_num) &&
	       Add(plan, Type::SampledImage, textures.binding_sampled_uint_index, unsigned_banks ? textures.textures2d_sampled_num : 0) &&
	       Add(plan, Type::SampledImage, textures.binding_sampled_array_uint_index,
	           unsigned_banks ? textures.textures2d_array_sampled_num : 0) &&
	       Add(plan, Type::SampledImage, textures.binding_sampled_3d_uint_index, unsigned_banks ? textures.textures3d_sampled_num : 0);
}

} // namespace

bool ShaderBuildDescriptorLayoutPlan(const ShaderBindResources& bind, ShaderDescriptorLayoutPlan* output) noexcept
{
	if (output == nullptr || !ValidCounts(bind))
	{
		return false;
	}
	ShaderDescriptorLayoutPlan plan {};
	const auto&                textures = bind.textures2D;
	if (!Add(&plan, Type::StorageBuffer, bind.storage_buffers.binding_index, bind.storage_buffers.buffers_num) ||
	    !AddSampledBindings(textures, &plan) ||
	    !Add(&plan, Type::StorageImage, textures.binding_storage_index, textures.textures2d_storage_num) ||
	    !Add(&plan, Type::Sampler, bind.samplers.binding_index, bind.samplers.samplers_num) ||
	    !Add(&plan, Type::StorageBuffer, bind.gds_pointers.binding_index, bind.gds_pointers.pointers_num) ||
	    !Add(&plan, Type::SampledImage, textures.binding_sampled_array_index, textures.textures2d_array_sampled_num) ||
	    !Add(&plan, Type::SampledImage, textures.binding_sampled_3d_index, textures.textures3d_sampled_num) ||
	    !Add(&plan, Type::UniformBuffer, bind.vsharp_binding_index, bind.vsharp_uniform_buffer ? 1 : 0))
	{
		return false;
	}
	*output = plan;
	return true;
}

ShaderDescriptorLimits::Check ShaderCountDescriptorLayoutPlan(const ShaderDescriptorLayoutPlan&         plan,
                                                              ShaderDescriptorLimits::DescriptorCounts* counts) noexcept
{
	if (plan.binding_count > plan.bindings.size())
	{
		return {ShaderDescriptorLimits::Failure::InvalidArgument, 0, 0};
	}
	std::array<ShaderDescriptorLimits::DescriptorBindingCount, ShaderDescriptorLayoutPlan::MAX_BINDINGS> bindings {};
	for (size_t i = 0; i < plan.binding_count; ++i)
	{
		bindings[i] = {plan.bindings[i].type, plan.bindings[i].count};
	}
	return ShaderDescriptorLimits::CountBindings(bindings.data(), plan.binding_count, counts);
}

ShaderDescriptorLayoutPlan::Key ShaderDescriptorLayoutPlan::CacheKey(uint32_t stage) const noexcept
{
	Key key {};
	key[0] = stage;
	for (size_t i = 0; i < binding_count && i < bindings.size(); ++i)
	{
		key[i * 3 + 1] = bindings[i].binding;
		key[i * 3 + 2] = static_cast<uint32_t>(bindings[i].type);
		key[i * 3 + 3] = bindings[i].count;
	}
	return key;
}

} // namespace Kyty::Libs::Graphics
