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

// Historical BC1 package fixtures use catalog identifier 133, which is
// RGB565 in the raw T# namespace. Only catalog readers/fixtures may
// translate this value; descriptor consumers always pass the raw hardware ID.
enum class Gen5CatalogImageFormat: uint16_t
{
	Bc1Unorm = 133,
};

[[nodiscard]] constexpr uint16_t Gen5ImageFormatFromCatalog(Gen5CatalogImageFormat format)
{
	return format == Gen5CatalogImageFormat::Bc1Unorm ? 169u : 0u;
}

// fmt is the raw nine-bit Gen5 T# FORMAT field. fmt=0 selects legacy dfmt/nfmt.
// Unsupported usage/format combinations return VK_FORMAT_UNDEFINED; callers
// must reject them instead of substituting another host format.
[[nodiscard]] VkFormat VulkanResolveGuestImageFormat(GuestImageUsage usage, uint8_t dfmt, uint8_t nfmt, uint16_t fmt);

// The explicit choice is the effective host interpretation and must be part
// of the sampled image's object identity.
[[nodiscard]] VkFormat VulkanResolveGuestImageFormat(GuestImageUsage usage, uint8_t dfmt, uint8_t nfmt, uint16_t fmt, bool use_srgb);

// Resolve the host color interpretation from raw Gen5 sampler gamma controls.
[[nodiscard]] bool VulkanGen5SampleUsesSrgb(uint16_t fmt, bool force_degamma, bool skip_degamma);

[[nodiscard]] bool VulkanSupportsGen5ImageFormat(GuestImageUsage usage, uint16_t fmt);

// Image views shared with guest render surfaces must retain their numeric
// interpretation. This comparison accepts the regular and degamma sampled
// variants declared by the central format table.
[[nodiscard]] bool VulkanGen5SampleFormatMatches(uint16_t fmt, VkFormat format);

// Immutable-image compatibility for one resolved sampler gamma mode: the
// resolved format, or the host format that stores the same texel bytes with
// red and blue exchanged (a render target written with the alternate swap).
[[nodiscard]] bool VulkanGen5SampleFormatMatchesEffective(uint16_t fmt, bool use_srgb, VkFormat format);
// A BGRA selection of this guest storage format is the red/blue-exchanged host image with identity components.
[[nodiscard]] bool VulkanStorageHasRedBlueView(uint8_t dfmt, uint8_t nfmt, uint16_t fmt);

// The selectors a sampled view of `surface` needs to read what the guest
// selectors read from the sample format: red and blue are exchanged when the
// surface stores the sample's bytes with those channels exchanged.
[[nodiscard]] uint32_t VulkanGen5SampleSurfaceSelectors(uint16_t fmt, bool use_srgb, VkFormat surface, uint32_t selectors);

[[nodiscard]] GuestImageNumericType VulkanGen5ImageNumericType(uint16_t fmt);

// Texel size of an uncompressed color format of the guest format table; zero
// for block-compressed, depth and unknown formats.
[[nodiscard]] uint32_t VulkanColorTexelBytes(VkFormat format);

// Guest memory is untyped: a storage image's bytes read through another
// format of the same texel size are what the hardware samples. Such a view is
// valid on an image created mutable with this format among its view formats.
[[nodiscard]] bool VulkanColorFormatsShareTexels(VkFormat a, VkFormat b);

// Every host format of the guest format table sharing the texel size of
// format, format first; the view-format list of a mutable storage image.
[[nodiscard]] uint32_t VulkanColorTexelFormatList(VkFormat format, VkFormat* out, uint32_t capacity);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_VULKANIMAGEFORMAT_H_ */
