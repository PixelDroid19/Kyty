#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANVERTEXINPUTFORMAT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANVERTEXINPUTFORMAT_H_

#include "Emulator/Common.h"

#include "vulkan/vulkan_core.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

enum class VulkanVertexInputNumericClass : uint8_t
{
	Float,
	Uint,
	Sint,
};

struct VulkanVertexInputFormat
{
	uint8_t                       unified_format  = 0;
	VkFormat                      format          = VK_FORMAT_UNDEFINED;
	uint32_t                      component_count = 0;
	VulkanVertexInputNumericClass numeric_class   = VulkanVertexInputNumericClass::Float;
};

// Resolve Gen5 descriptors and attribute encodings through one renderer-owned
// format table. Unknown values stay undefined and must be rejected by callers.
[[nodiscard]] VulkanVertexInputFormat VulkanResolveGen5VertexInputFormat(uint8_t format);
[[nodiscard]] VulkanVertexInputFormat VulkanResolveGen5VertexAttribInputFormat(uint16_t format);
[[nodiscard]] VulkanVertexInputFormat VulkanResolveLegacyVertexInputFormat(uint8_t dfmt, uint8_t nfmt);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANVERTEXINPUTFORMAT_H_ */
