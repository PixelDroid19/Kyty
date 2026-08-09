#include "Emulator/Graphics/VulkanVertexInputFormat.h"

#include <array>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

struct Gen5VertexInputFormatEntry
{
	uint16_t                      attribute_format;
	uint8_t                       guest_format;
	VkFormat                      vulkan_format;
	uint32_t                      component_count;
	VulkanVertexInputNumericClass numeric_class;
};

// guest_format ids match Gen5 image/buffer data formats (see VulkanImageFormat).
// Format 29 (R16G16_SFLOAT) is required for interleaved UV streams in
// stride-24/32 layouts of {position f32x3, normal h16x4, UV h16x2}.
constexpr std::array<Gen5VertexInputFormatEntry, 9> k_gen5_formats {{
    {0, 20, VK_FORMAT_R32_UINT, 1, VulkanVertexInputNumericClass::Uint},
    {0, 22, VK_FORMAT_R32_SFLOAT, 1, VulkanVertexInputNumericClass::Float},
    {0x05d, 23, VK_FORMAT_R16G16_UNORM, 2, VulkanVertexInputNumericClass::Float},
    {0, 29, VK_FORMAT_R16G16_SFLOAT, 2, VulkanVertexInputNumericClass::Float},
    {0x0e3, 56, VK_FORMAT_R8G8B8A8_UNORM, 4, VulkanVertexInputNumericClass::Float},
    {0x101, 64, VK_FORMAT_R32G32_SFLOAT, 2, VulkanVertexInputNumericClass::Float},
    {0x11f, 71, VK_FORMAT_R16G16B16A16_SFLOAT, 4, VulkanVertexInputNumericClass::Float},
    {0x12a, 74, VK_FORMAT_R32G32B32_SFLOAT, 3, VulkanVertexInputNumericClass::Float},
    {0x137, 77, VK_FORMAT_R32G32B32A32_SFLOAT, 4, VulkanVertexInputNumericClass::Float},
}};

struct LegacyVertexInputFormatEntry
{
	uint8_t  dfmt;
	uint8_t  nfmt;
	VkFormat vulkan_format;
	uint32_t component_count;
};

constexpr std::array<LegacyVertexInputFormatEntry, 4> k_legacy_formats {{
    {10, 0, VK_FORMAT_R8G8B8A8_UNORM, 4},
    {11, 7, VK_FORMAT_R32G32_SFLOAT, 2},
    {13, 7, VK_FORMAT_R32G32B32_SFLOAT, 3},
    {14, 7, VK_FORMAT_R32G32B32A32_SFLOAT, 4},
}};

} // namespace

VulkanVertexInputFormat VulkanResolveGen5VertexInputFormat(uint8_t format)
{
	for (const auto& entry: k_gen5_formats)
	{
		if (entry.guest_format == format)
		{
			return {entry.guest_format, entry.vulkan_format, entry.component_count, entry.numeric_class};
		}
	}
	return {};
}

VulkanVertexInputFormat VulkanResolveGen5VertexAttribInputFormat(uint16_t format)
{
	for (const auto& entry: k_gen5_formats)
	{
		if (entry.attribute_format == format && entry.attribute_format != 0)
		{
			return {entry.guest_format, entry.vulkan_format, entry.component_count, entry.numeric_class};
		}
	}
	return {};
}

VulkanVertexInputFormat VulkanResolveLegacyVertexInputFormat(uint8_t dfmt, uint8_t nfmt)
{
	for (const auto& entry: k_legacy_formats)
	{
		if (entry.dfmt == dfmt && entry.nfmt == nfmt)
		{
			return {0, entry.vulkan_format, entry.component_count};
		}
	}
	return {};
}

} // namespace Kyty::Libs::Graphics

#endif
