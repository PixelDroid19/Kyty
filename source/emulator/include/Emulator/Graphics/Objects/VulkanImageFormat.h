#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VULKANIMAGEFORMAT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VULKANIMAGEFORMAT_H_

#include "Emulator/Common.h"

#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

enum class GuestImageUsage
{
	Sampled,
	Storage,
};

enum class GuestImageNumericType
{
	Unsupported,
	FloatingPoint,
	UnsignedInteger,
	SignedInteger,
};

// Resolve both legacy dfmt/nfmt and Gen5 unified image formats from one table.
// Unsupported usage/format combinations return VK_FORMAT_UNDEFINED; callers
// must reject them instead of substituting another host format.
[[nodiscard]] VkFormat VulkanResolveGuestImageFormat(GuestImageUsage usage, uint8_t dfmt, uint8_t nfmt, uint16_t fmt,
                                                     bool force_degamma = false);

[[nodiscard]] bool VulkanSupportsGen5ImageFormat(GuestImageUsage usage, uint16_t fmt);

// Image views shared with guest render surfaces must retain their numeric
// interpretation. This comparison accepts the regular and degamma sampled
// variants declared by the central format table.
[[nodiscard]] bool VulkanGen5SampleFormatMatches(uint16_t fmt, VkFormat format);

[[nodiscard]] GuestImageNumericType VulkanGen5ImageNumericType(uint16_t fmt);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VULKANIMAGEFORMAT_H_ */
