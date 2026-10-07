#pragma once

#include "Emulator/Graphics/ShaderDescriptorLimits.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Kyty::Libs::Graphics {

struct ShaderBindResources;

struct ShaderDescriptorLayoutBinding
{
	uint32_t                               binding = 0;
	ShaderDescriptorLimits::DescriptorType type    = ShaderDescriptorLimits::DescriptorType::Unknown;
	uint32_t                               count   = 0;
};

struct ShaderDescriptorLayoutPlan
{
	static constexpr size_t MAX_BINDINGS = 13;
	using Key                            = std::array<uint32_t, MAX_BINDINGS * 3 + 1>;
	std::array<ShaderDescriptorLayoutBinding, MAX_BINDINGS> bindings {};
	size_t                                                  binding_count = 0;

	[[nodiscard]] Key CacheKey(uint32_t stage) const noexcept;
};

// Reserve the shader's separate image arrays while retaining its existing
// sparse binding numbers and numeric dispatch banks.
[[nodiscard]] bool ShaderBuildDescriptorLayoutPlan(const ShaderBindResources& bind, ShaderDescriptorLayoutPlan* plan) noexcept;
[[nodiscard]] ShaderDescriptorLimits::Check ShaderCountDescriptorLayoutPlan(const ShaderDescriptorLayoutPlan&         plan,
                                                                            ShaderDescriptorLimits::DescriptorCounts* counts) noexcept;

} // namespace Kyty::Libs::Graphics
