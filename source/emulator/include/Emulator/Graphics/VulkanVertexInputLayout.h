#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANVERTEXINPUTLAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANVERTEXINPUTLAYOUT_H_

#include "Emulator/Common.h"

#include <vulkan/vulkan_core.h>

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct ShaderVertexInputInfo;

struct VulkanVertexInputLayout
{
	static constexpr uint32_t MAX_ATTRIBUTES = 16;

	VkVertexInputAttributeDescription attributes[MAX_ATTRIBUTES] = {};
	VkVertexInputBindingDescription   bindings[MAX_ATTRIBUTES]   = {};
	uint32_t                          attribute_count            = 0;
	uint32_t                          binding_count              = 0;
};

// Translate guest vertex descriptors into the Vulkan input layout shared by
// regular and fixed-function expansion pipelines.
[[nodiscard]] bool VulkanBuildVertexInputLayout(const ShaderVertexInputInfo& input, VulkanVertexInputLayout* layout);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VULKANVERTEXINPUTLAYOUT_H_
