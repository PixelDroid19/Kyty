#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COLORDCCCLEAR_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COLORDCCCLEAR_H_

#include <array>
#include <cstdint>

namespace Kyty::Libs::Graphics {

// Gen5 color DCC keys: one metadata byte per 256-byte compressed block. A key
// that names a clear state makes every texel of its block read as that state.
enum class ColorDccClearCode: uint8_t
{
	Zero     = 0x00, // every channel 0
	Alpha    = 0x40, // color channels 0, alpha 1
	Color    = 0x80, // color channels 1, alpha 0
	All      = 0xc0, // every channel 1
	Register = 0x20, // CB_COLOR_CLEAR_WORD0/1; decoded by the color block only
};

// A uniform metadata fill writes one clear key to every byte of its range.
[[nodiscard]] inline bool ColorDccClearCodeFromFill(const std::array<uint32_t, 4>& words, ColorDccClearCode* code)
{
	const uint32_t key = words[0] & 0xffu;
	if (code == nullptr || words[0] != key * 0x01010101u || words[1] != words[0] || words[2] != words[0] || words[3] != words[0])
	{
		return false;
	}
	switch (key)
	{
		case 0x00u:
		case 0x20u:
		case 0x40u:
		case 0x80u:
		case 0xc0u: *code = static_cast<ColorDccClearCode>(key); return true;
		default: return false;
	}
}

// The metadata plane of a single-level, single-slice surface starts at its DCC
// base and holds one key per 256 bytes of texels, padded to whole metadata
// blocks. A fill from that base shorter than the key count cannot rewrite
// every key. The padding depends on the pipe configuration and is not modelled.
[[nodiscard]] inline bool ColorDccFillCoversSurface(uint64_t fill_address, uint64_t fill_size, uint64_t dcc_address, uint32_t width,
                                                    uint32_t height, uint32_t bytes_per_element)
{
	if (dcc_address == 0u || fill_address != dcc_address || width == 0u || height == 0u || bytes_per_element == 0u)
	{
		return false;
	}
	const uint64_t keys = ((static_cast<uint64_t>(width) * height * bytes_per_element) + 255u) / 256u;
	return fill_size >= keys;
}

// Single-sample color CMASK holds one nibble per 8x8 pixel tile; nibble 0
// marks a fast-cleared tile, which the color block reads as
// CB_COLOR_CLEAR_WORD0/1. A zero fill from the CMASK base that reaches every
// tile's nibble fast-clears the whole surface.
[[nodiscard]] inline bool ColorCmaskFillClearsSurface(const std::array<uint32_t, 4>& words, uint64_t fill_address, uint64_t fill_size,
                                                      uint64_t cmask_address, uint32_t width, uint32_t height)
{
	if (cmask_address == 0u || fill_address != cmask_address || width == 0u || height == 0u ||
	    (words[0] | words[1] | words[2] | words[3]) != 0u)
	{
		return false;
	}
	const uint64_t tiles = static_cast<uint64_t>((width + 7u) / 8u) * ((height + 7u) / 8u);
	return fill_size >= (tiles + 1u) / 2u;
}

// COMP_SWAP STD (0) and ALT (1) keep alpha in the most significant channel of
// a multi-channel color surface; STD_REV (2) and ALT_REV (3) do not.
[[nodiscard]] constexpr bool ColorDccAlphaOnMsb(uint32_t comp_swap)
{
	return comp_swap != 2u && comp_swap != 3u;
}

// The clear value in logical RGBA. Codes 0x00 and 0xc0 give every channel the
// same value. Codes 0x40 and 0x80 set alpha apart from the color channels; they
// are decoded only for four-channel surfaces with alpha in the most
// significant channel. Register clears are not decoded here.
[[nodiscard]] inline bool ColorDccClearValue(ColorDccClearCode code, bool alpha_on_msb, uint32_t channels, std::array<float, 4>* rgba)
{
	if (rgba == nullptr || code == ColorDccClearCode::Register)
	{
		return false;
	}
	const bool alpha_differs = code == ColorDccClearCode::Alpha || code == ColorDccClearCode::Color;
	if (alpha_differs && (channels != 4u || !alpha_on_msb))
	{
		return false;
	}
	const float color = (code == ColorDccClearCode::Color || code == ColorDccClearCode::All) ? 1.0f : 0.0f;
	const float alpha = (code == ColorDccClearCode::Alpha || code == ColorDccClearCode::All) ? 1.0f : 0.0f;
	*rgba             = {color, color, color, alpha};
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COLORDCCCLEAR_H_
