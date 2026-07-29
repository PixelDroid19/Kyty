#include "Emulator/Graphics/Objects/VulkanImageFormat.h"

#include <array>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

struct LegacyImageFormat
{
	uint8_t  dfmt;
	uint8_t  nfmt;
	VkFormat sampled;
	VkFormat storage;
};

struct Gen5ImageFormat
{
	uint16_t fmt;
	VkFormat sampled;
	VkFormat sampled_degamma;
	VkFormat storage;
};

constexpr std::array LEGACY_IMAGE_FORMATS = {
    LegacyImageFormat {10, 9, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB},
    LegacyImageFormat {10, 0, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
    LegacyImageFormat {1, 0, VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED},
    LegacyImageFormat {3, 0, VK_FORMAT_R8G8_UNORM, VK_FORMAT_UNDEFINED},
    LegacyImageFormat {37, 9, VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK},
    LegacyImageFormat {37, 0, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    LegacyImageFormat {36, 0, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    LegacyImageFormat {35, 0, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
};

constexpr std::array GEN5_IMAGE_FORMATS = {
    Gen5ImageFormat {1, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED},
    Gen5ImageFormat {5, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM},
    Gen5ImageFormat {7, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_UNDEFINED},
    Gen5ImageFormat {13, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_UNDEFINED},
    Gen5ImageFormat {14, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM},
    Gen5ImageFormat {20, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT},
    Gen5ImageFormat {56, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_UNDEFINED},
    Gen5ImageFormat {62, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT},
    Gen5ImageFormat {71, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED},
    Gen5ImageFormat {75, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT},
    Gen5ImageFormat {133, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    Gen5ImageFormat {173, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_UNDEFINED},
    Gen5ImageFormat {181, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
    Gen5ImageFormat {182, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_UNDEFINED},
};

VkFormat SelectFormat(GuestImageUsage usage, VkFormat sampled, VkFormat storage)
{
	return usage == GuestImageUsage::Sampled ? sampled : storage;
}

} // namespace

VkFormat VulkanResolveGuestImageFormat(GuestImageUsage usage, uint8_t dfmt, uint8_t nfmt, uint16_t fmt, bool force_degamma)
{
	if (fmt == 0)
	{
		for (const auto& entry: LEGACY_IMAGE_FORMATS)
		{
			if (entry.dfmt == dfmt && entry.nfmt == nfmt)
			{
				return SelectFormat(usage, entry.sampled, entry.storage);
			}
		}
		return VK_FORMAT_UNDEFINED;
	}

	for (const auto& entry: GEN5_IMAGE_FORMATS)
	{
		if (entry.fmt == fmt)
		{
			if (usage == GuestImageUsage::Storage)
			{
				return entry.storage;
			}
			return force_degamma ? entry.sampled_degamma : entry.sampled;
		}
	}
	return VK_FORMAT_UNDEFINED;
}

bool VulkanSupportsGen5ImageFormat(GuestImageUsage usage, uint16_t fmt)
{
	for (const auto& entry: GEN5_IMAGE_FORMATS)
	{
		if (entry.fmt == fmt)
		{
			return SelectFormat(usage, entry.sampled, entry.storage) != VK_FORMAT_UNDEFINED;
		}
	}
	return false;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
