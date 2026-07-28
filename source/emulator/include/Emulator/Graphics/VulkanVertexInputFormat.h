#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANVERTEXINPUTFORMAT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANVERTEXINPUTFORMAT_H_

#include "Emulator/Common.h"

#include "vulkan/vulkan_core.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct VulkanVertexInputFormat
{
	VkFormat format          = VK_FORMAT_UNDEFINED;
	uint32_t component_count = 0;
};

// Resolve the complete renderer contract in one table. Unknown guest formats
// stay undefined and must be rejected by the pipeline caller.
[[nodiscard]] VulkanVertexInputFormat VulkanResolveGen5VertexInputFormat(uint8_t format);
[[nodiscard]] VulkanVertexInputFormat VulkanResolveLegacyVertexInputFormat(uint8_t dfmt, uint8_t nfmt);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANVERTEXINPUTFORMAT_H_ */
