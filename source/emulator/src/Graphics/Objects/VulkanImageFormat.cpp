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
	VkFormat              sampled_srgb;
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
    Gen5ImageFormat {1, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, GuestImageNumericType::FloatingPoint},
    // RDNA2 ISA Table 47: 5=8_UINT. Storage uses the exact R8ui declaration
    // with StorageImageExtendedFormats, subject to the host format query.
    Gen5ImageFormat {5, VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {7, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {11, VK_FORMAT_R16_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R16_UINT, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {13, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SFLOAT, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {14, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {20, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {22, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {23, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UNORM, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {29, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
                     GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {36, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                     VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                     GuestImageNumericType::FloatingPoint},
    // IMG_FORMAT 2_10_10_10_UNORM: channel 0 in the low ten bits (Vulkan A2B10G10R10).
    Gen5ImageFormat {50, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_UNDEFINED,
                     GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {56, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                     GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {60, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {62, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {64, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {65, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM,
                     GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {66, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {69, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_UNDEFINED, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {70, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_UNDEFINED, GuestImageNumericType::SignedInteger},
    Gen5ImageFormat {71, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {75, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, GuestImageNumericType::UnsignedInteger},
    Gen5ImageFormat {77, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {130, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    // Hardware X/Y/Z occupy low-to-high bits (AMD PAL ChNumFormat contract).
    Gen5ImageFormat {133, VK_FORMAT_B5G6R5_UNORM_PACK16, VK_FORMAT_B5G6R5_UNORM_PACK16, VK_FORMAT_UNDEFINED,
                      GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {169, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {170, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {171, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {172, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
    Gen5ImageFormat {173, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_UNDEFINED, GuestImageNumericType::FloatingPoint},
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

bool VulkanGen5SampleUsesSrgb(uint16_t fmt, bool force_degamma, bool skip_degamma)
{
	return fmt == 130u ? !skip_degamma : force_degamma && !skip_degamma;
}

VkFormat VulkanResolveGuestImageFormat(GuestImageUsage usage, uint8_t dfmt, uint8_t nfmt, uint16_t fmt)
{
	return VulkanResolveGuestImageFormat(usage, dfmt, nfmt, fmt, VulkanGen5SampleUsesSrgb(fmt, false, false));
}

VkFormat VulkanResolveGuestImageFormat(GuestImageUsage usage, uint8_t dfmt, uint8_t nfmt, uint16_t fmt, bool use_srgb)
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
			return use_srgb ? entry.sampled_srgb : entry.sampled;
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
			if (format == entry.sampled || format == entry.sampled_srgb)
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

// The host format storing the same texel bytes with red and blue exchanged.
static VkFormat RedBlueExchangedFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
		case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
		case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
		case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		default: return VK_FORMAT_UNDEFINED;
	}
}

bool VulkanStorageHasRedBlueView(uint8_t dfmt, uint8_t nfmt, uint16_t fmt)
{
	const VkFormat storage = VulkanResolveGuestImageFormat(GuestImageUsage::Storage, dfmt, nfmt, fmt);
	return storage == VK_FORMAT_R8G8B8A8_UNORM || storage == VK_FORMAT_R8G8B8A8_SRGB;
}

bool VulkanGen5SampleFormatMatchesEffective(uint16_t fmt, bool use_srgb, VkFormat format)
{
	const VkFormat expected = VulkanResolveGuestImageFormat(GuestImageUsage::Sampled, 0u, 0u, fmt, use_srgb);
	return expected != VK_FORMAT_UNDEFINED && (format == expected || format == RedBlueExchangedFormat(expected));
}

uint32_t VulkanGen5SampleSurfaceSelectors(uint16_t fmt, bool use_srgb, VkFormat surface, uint32_t selectors)
{
	const VkFormat expected = VulkanResolveGuestImageFormat(GuestImageUsage::Sampled, 0u, 0u, fmt, use_srgb);
	if (expected == VK_FORMAT_UNDEFINED || surface != RedBlueExchangedFormat(expected))
	{
		return selectors;
	}
	uint32_t exchanged = 0;
	for (uint32_t channel = 0; channel < 4u; channel++)
	{
		uint32_t select = (selectors >> (channel * 3u)) & 0x7u;
		select          = select == 4u ? 6u : (select == 6u ? 4u : select);
		exchanged |= select << (channel * 3u);
	}
	return exchanged;
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

uint32_t VulkanColorTexelBytes(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8_UNORM:
		case VK_FORMAT_R8_UINT: return 1u;
		case VK_FORMAT_R16_UNORM:
		case VK_FORMAT_R16_UINT:
		case VK_FORMAT_R16_SFLOAT:
		case VK_FORMAT_R8G8_UNORM:
		case VK_FORMAT_B5G6R5_UNORM_PACK16: return 2u;
		case VK_FORMAT_R32_UINT:
		case VK_FORMAT_R32_SFLOAT:
		case VK_FORMAT_R16G16_SFLOAT:
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_R8G8B8A8_UINT:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB: return 4u;
		case VK_FORMAT_R32G32_UINT:
		case VK_FORMAT_R32G32_SFLOAT:
		case VK_FORMAT_R16G16B16A16_UNORM:
		case VK_FORMAT_R16G16B16A16_SNORM:
		case VK_FORMAT_R16G16B16A16_UINT:
		case VK_FORMAT_R16G16B16A16_SINT:
		case VK_FORMAT_R16G16B16A16_SFLOAT: return 8u;
		case VK_FORMAT_R32G32B32A32_UINT:
		case VK_FORMAT_R32G32B32A32_SFLOAT: return 16u;
		default: return 0u;
	}
}

bool VulkanColorFormatsShareTexels(VkFormat a, VkFormat b)
{
	const uint32_t bytes = VulkanColorTexelBytes(a);
	return bytes != 0u && bytes == VulkanColorTexelBytes(b);
}

uint32_t VulkanColorTexelFormatList(VkFormat format, VkFormat* out, uint32_t capacity)
{
	if (out == nullptr || capacity == 0u || VulkanColorTexelBytes(format) == 0u)
	{
		return 0u;
	}
	uint32_t   count = 0;
	const auto add   = [&](VkFormat candidate)
	{
		if (candidate == VK_FORMAT_UNDEFINED || count == capacity || !VulkanColorFormatsShareTexels(format, candidate))
		{
			return;
		}
		for (uint32_t i = 0; i < count; i++)
		{
			if (out[i] == candidate)
			{
				return;
			}
		}
		out[count++] = candidate;
	};
	add(format);
	for (const auto& entry: GEN5_IMAGE_FORMATS)
	{
		add(entry.sampled);
		add(entry.sampled_srgb);
		add(entry.storage);
	}
	for (const auto& alias: GEN5_SAMPLED_FORMAT_ALIASES)
	{
		add(alias.format);
	}
	return count;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
