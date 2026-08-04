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
	uint16_t              fmt;
	VkFormat              sampled;
	VkFormat              sampled_degamma;
	VkFormat              storage;
	GuestImageNumericType numeric_type;
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
    Gen5ImageFormat {1, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {5, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {7, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {13, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {14, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {20, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {22, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {36, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                     VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                     GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {56, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {62, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {65, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {66, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {69, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_UNDEFINED, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {70, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_UNDEFINED, GuestImageNumericType::SignedInteger},
    Gen5ImageFormat {71, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {75, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {133, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {169, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {170, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {171, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {172, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {173, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {174, VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {175, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {176, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {177, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {178, VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {179, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {180, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {181, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {182, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
};

struct Gen5SampleFormatAlias
{
	uint16_t fmt;
	VkFormat format;
};

constexpr std::array GEN5_SAMPLED_FORMAT_ALIASES = {
    Gen5SampleFormatAlias {56, VK_FORMAT_B8G8R8A8_UNORM},
    Gen5SampleFormatAlias {56, VK_FORMAT_B8G8R8A8_SRGB},
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

bool VulkanGen5SampleFormatMatches(uint16_t fmt, VkFormat format)
{
	for (const auto& entry: GEN5_IMAGE_FORMATS)
	{
		if (entry.fmt == fmt)
		{
			if (format == entry.sampled || format == entry.sampled_degamma)
			{
				return true;
			}
			break;
		}
	}
	for (const auto& alias: GEN5_SAMPLED_FORMAT_ALIASES)
	{
		if (alias.fmt == fmt && alias.format == format)
		{
			return true;
		}
	}
	return false;
}

GuestImageNumericType VulkanGen5ImageNumericType(uint16_t fmt)
{
	for (const auto& entry: GEN5_IMAGE_FORMATS)
	{
		if (entry.fmt == fmt)
		{
			return entry.numeric_type;
		}
	}
	return GuestImageNumericType::Unsupported;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
