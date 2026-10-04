#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANSAMPLER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANSAMPLER_H_

#include "Emulator/Graphics/GraphicsState.h"

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct VulkanSamplerFeatures
{
	VkBool32 mirror_clamp_to_edge = VK_FALSE;
};

[[nodiscard]] constexpr VulkanSamplerFeatures VulkanPlanSamplerFeatures(const VkPhysicalDeviceVulkan12Features& supported)
{
	return {supported.samplerMirrorClampToEdge};
}

[[nodiscard]] inline bool VulkanResolveSamplerAddressMode(State::SamplerAddressMode mode, const VulkanSamplerFeatures& enabled,
                                                          bool force_unnormalized, VkSamplerAddressMode* address)
{
	if (address == nullptr || !State::SamplerAddressModeHasExactHostMapping(mode, enabled.mirror_clamp_to_edge != VK_FALSE,
	                                                                      force_unnormalized))
	{
		return false;
	}
	switch (mode)
	{
		case State::SamplerAddressMode::Repeat: *address = VK_SAMPLER_ADDRESS_MODE_REPEAT; return true;
		case State::SamplerAddressMode::MirroredRepeat: *address = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; return true;
		case State::SamplerAddressMode::ClampToEdge: *address = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; return true;
		case State::SamplerAddressMode::ClampToBorder: *address = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; return true;
		case State::SamplerAddressMode::MirrorOnceLastTexel: *address = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; return true;
		default: return false;
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANSAMPLER_H_
