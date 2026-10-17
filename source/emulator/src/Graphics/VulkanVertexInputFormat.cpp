#include "Emulator/Graphics/VulkanVertexInputFormat.h"

#include <array>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

struct Gen5VertexInputFormatEntry
{
	uint8_t                       guest_format;
	VkFormat                      vulkan_format;
	uint32_t                      component_count;
	uint32_t                      element_bytes;
	VulkanVertexInputNumericClass numeric_class;
};

// guest_format ids are the Gen5 buffer data formats of the RDNA2 ISA format
// table. Only formats with a float or unsigned-integer shader view and a
// mandatory Vulkan vertex-buffer equivalent are listed; signed-integer and
// scaled formats stay undefined until the input path models them.
// 2_10_10_10 names channels from the high bits: R is in the low 10 bits.
constexpr std::array<Gen5VertexInputFormatEntry, 30> k_gen5_formats {{
    {1, VK_FORMAT_R8_UNORM, 1, 1, VulkanVertexInputNumericClass::Float},
    {2, VK_FORMAT_R8_SNORM, 1, 1, VulkanVertexInputNumericClass::Float},
    {5, VK_FORMAT_R8_UINT, 1, 1, VulkanVertexInputNumericClass::Uint},
    {7, VK_FORMAT_R16_UNORM, 1, 2, VulkanVertexInputNumericClass::Float},
    {8, VK_FORMAT_R16_SNORM, 1, 2, VulkanVertexInputNumericClass::Float},
    {11, VK_FORMAT_R16_UINT, 1, 2, VulkanVertexInputNumericClass::Uint},
    {13, VK_FORMAT_R16_SFLOAT, 1, 2, VulkanVertexInputNumericClass::Float},
    {14, VK_FORMAT_R8G8_UNORM, 2, 2, VulkanVertexInputNumericClass::Float},
    {15, VK_FORMAT_R8G8_SNORM, 2, 2, VulkanVertexInputNumericClass::Float},
    {18, VK_FORMAT_R8G8_UINT, 2, 2, VulkanVertexInputNumericClass::Uint},
    {20, VK_FORMAT_R32_UINT, 1, 4, VulkanVertexInputNumericClass::Uint},
    {22, VK_FORMAT_R32_SFLOAT, 1, 4, VulkanVertexInputNumericClass::Float},
    {23, VK_FORMAT_R16G16_UNORM, 2, 4, VulkanVertexInputNumericClass::Float},
    {24, VK_FORMAT_R16G16_SNORM, 2, 4, VulkanVertexInputNumericClass::Float},
    {27, VK_FORMAT_R16G16_UINT, 2, 4, VulkanVertexInputNumericClass::Uint},
    {29, VK_FORMAT_R16G16_SFLOAT, 2, 4, VulkanVertexInputNumericClass::Float},
    {50, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 4, VulkanVertexInputNumericClass::Float},
    {56, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VulkanVertexInputNumericClass::Float},
    {57, VK_FORMAT_R8G8B8A8_SNORM, 4, 4, VulkanVertexInputNumericClass::Float},
    {60, VK_FORMAT_R8G8B8A8_UINT, 4, 4, VulkanVertexInputNumericClass::Uint},
    {62, VK_FORMAT_R32G32_UINT, 2, 8, VulkanVertexInputNumericClass::Uint},
    {64, VK_FORMAT_R32G32_SFLOAT, 2, 8, VulkanVertexInputNumericClass::Float},
    {65, VK_FORMAT_R16G16B16A16_UNORM, 4, 8, VulkanVertexInputNumericClass::Float},
    {66, VK_FORMAT_R16G16B16A16_SNORM, 4, 8, VulkanVertexInputNumericClass::Float},
    {69, VK_FORMAT_R16G16B16A16_UINT, 4, 8, VulkanVertexInputNumericClass::Uint},
    {71, VK_FORMAT_R16G16B16A16_SFLOAT, 4, 8, VulkanVertexInputNumericClass::Float},
    {72, VK_FORMAT_R32G32B32_UINT, 3, 12, VulkanVertexInputNumericClass::Uint},
    {74, VK_FORMAT_R32G32B32_SFLOAT, 3, 12, VulkanVertexInputNumericClass::Float},
    {75, VK_FORMAT_R32G32B32A32_UINT, 4, 16, VulkanVertexInputNumericClass::Uint},
    {77, VK_FORMAT_R32G32B32A32_SFLOAT, 4, 16, VulkanVertexInputNumericClass::Float},
}};

struct LegacyVertexInputFormatEntry
{
	uint8_t  dfmt;
	uint8_t  nfmt;
	VkFormat vulkan_format;
	uint32_t component_count;
	uint32_t element_bytes;
};

constexpr std::array<LegacyVertexInputFormatEntry, 4> k_legacy_formats {{
    {10, 0, VK_FORMAT_R8G8B8A8_UNORM, 4, 4},
    {11, 7, VK_FORMAT_R32G32_SFLOAT, 2, 8},
    {13, 7, VK_FORMAT_R32G32B32_SFLOAT, 3, 12},
    {14, 7, VK_FORMAT_R32G32B32A32_SFLOAT, 4, 16},
}};

} // namespace

VulkanVertexInputFormat VulkanResolveGen5VertexInputFormat(uint8_t format)
{
	for (const auto& entry: k_gen5_formats)
	{
		if (entry.guest_format == format)
		{
			return {entry.guest_format, entry.vulkan_format, entry.component_count, entry.element_bytes, entry.numeric_class};
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
			return {0, entry.vulkan_format, entry.component_count, entry.element_bytes};
		}
	}
	return {};
}

} // namespace Kyty::Libs::Graphics

#endif
