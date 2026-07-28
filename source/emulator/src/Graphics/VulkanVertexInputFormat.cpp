#include "Emulator/Graphics/VulkanVertexInputFormat.h"

#include <array>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

struct Gen5VertexInputFormatEntry
{
	uint8_t  guest_format;
	VkFormat vulkan_format;
	uint32_t component_count;
};

constexpr std::array<Gen5VertexInputFormatEntry, 6> k_gen5_formats {{
    {20, VK_FORMAT_R32_UINT, 1},
    {22, VK_FORMAT_R32_SFLOAT, 1},
    {56, VK_FORMAT_R8G8B8A8_UNORM, 4},
    {64, VK_FORMAT_R32G32_SFLOAT, 2},
    {74, VK_FORMAT_R32G32B32_SFLOAT, 3},
    {77, VK_FORMAT_R32G32B32A32_SFLOAT, 4},
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
			return {entry.vulkan_format, entry.component_count};
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
			return {entry.vulkan_format, entry.component_count};
		}
	}
	return {};
}

} // namespace Kyty::Libs::Graphics

#endif
