#include "Emulator/Graphics/ShaderBufferFormat.h"

#include <array>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

using N = ShaderBufferNumber;

// `channels` channels of `bits` bits each, X at the lowest address.
constexpr ShaderBufferFormat Uniform(uint8_t format, uint8_t channels, uint8_t bits, ShaderBufferNumber number)
{
	ShaderBufferFormat f {};
	f.format   = format;
	f.bytes    = static_cast<uint8_t>(channels * bits / 8u);
	f.channels = channels;
	f.number   = number;
	for (uint8_t c = 0; c < channels; c++)
	{
		f.layout[c] = {static_cast<uint8_t>(c * bits), bits};
	}
	return f;
}

// One dword packing X..W widths from the low bits up; a zero width ends the
// channel list. 2_10_10_10 and 10_11_11 name channels from the high bits.
constexpr ShaderBufferFormat Packed(uint8_t format, ShaderBufferNumber number, uint8_t x, uint8_t y, uint8_t z, uint8_t w)
{
	ShaderBufferFormat f {};
	f.format              = format;
	f.bytes               = 4;
	f.number              = number;
	const uint8_t widths[] = {x, y, z, w};
	uint8_t       offset   = 0;
	for (uint8_t width: widths)
	{
		if (width == 0)
		{
			break;
		}
		f.layout[f.channels++] = {offset, width};
		offset                 = static_cast<uint8_t>(offset + width);
	}
	return f;
}

// RDNA2 ISA table 47, buffer formats. Gaps in the numbering are not formats.
constexpr std::array k_formats = {
    Uniform(1, 1, 8, N::Unorm),    Uniform(2, 1, 8, N::Snorm),    Uniform(3, 1, 8, N::Uscaled),  Uniform(4, 1, 8, N::Sscaled),
    Uniform(5, 1, 8, N::Uint),     Uniform(6, 1, 8, N::Sint),     Uniform(7, 1, 16, N::Unorm),   Uniform(8, 1, 16, N::Snorm),
    Uniform(9, 1, 16, N::Uscaled), Uniform(10, 1, 16, N::Sscaled), Uniform(11, 1, 16, N::Uint),  Uniform(12, 1, 16, N::Sint),
    Uniform(13, 1, 16, N::Float),  Uniform(14, 2, 8, N::Unorm),   Uniform(15, 2, 8, N::Snorm),   Uniform(16, 2, 8, N::Uscaled),
    Uniform(17, 2, 8, N::Sscaled), Uniform(18, 2, 8, N::Uint),    Uniform(19, 2, 8, N::Sint),    Uniform(20, 1, 32, N::Uint),
    Uniform(21, 1, 32, N::Sint),   Uniform(22, 1, 32, N::Float),  Uniform(23, 2, 16, N::Unorm),  Uniform(24, 2, 16, N::Snorm),
    Uniform(25, 2, 16, N::Uscaled), Uniform(26, 2, 16, N::Sscaled), Uniform(27, 2, 16, N::Uint), Uniform(28, 2, 16, N::Sint),
    Uniform(29, 2, 16, N::Float),
    // 10_11_11_FLOAT: X in the low 11 bits (the B10G11R11 image format).
    Packed(36, N::Float, 11, 11, 10, 0), Packed(43, N::Float, 10, 11, 11, 0),
    Packed(44, N::Unorm, 2, 10, 10, 10), Packed(45, N::Snorm, 2, 10, 10, 10), Packed(48, N::Uint, 2, 10, 10, 10),
    Packed(49, N::Sint, 2, 10, 10, 10),
    // 2_10_10_10: X in the low 10 bits (the A2B10G10R10 image format).
    Packed(50, N::Unorm, 10, 10, 10, 2), Packed(51, N::Snorm, 10, 10, 10, 2), Packed(52, N::Uscaled, 10, 10, 10, 2),
    Packed(53, N::Sscaled, 10, 10, 10, 2), Packed(54, N::Uint, 10, 10, 10, 2), Packed(55, N::Sint, 10, 10, 10, 2),
    Uniform(56, 4, 8, N::Unorm),   Uniform(57, 4, 8, N::Snorm),   Uniform(58, 4, 8, N::Uscaled), Uniform(59, 4, 8, N::Sscaled),
    Uniform(60, 4, 8, N::Uint),    Uniform(61, 4, 8, N::Sint),    Uniform(62, 2, 32, N::Uint),   Uniform(63, 2, 32, N::Sint),
    Uniform(64, 2, 32, N::Float),  Uniform(65, 4, 16, N::Unorm),  Uniform(66, 4, 16, N::Snorm),  Uniform(67, 4, 16, N::Uscaled),
    Uniform(68, 4, 16, N::Sscaled), Uniform(69, 4, 16, N::Uint),  Uniform(70, 4, 16, N::Sint),   Uniform(71, 4, 16, N::Float),
    Uniform(72, 3, 32, N::Uint),   Uniform(73, 3, 32, N::Sint),   Uniform(74, 3, 32, N::Float),  Uniform(75, 4, 32, N::Uint),
    Uniform(76, 4, 32, N::Sint),   Uniform(77, 4, 32, N::Float),
};

} // namespace

const ShaderBufferFormat* ShaderGen5BufferFormat(uint32_t format)
{
	for (const auto& entry: k_formats)
	{
		if (entry.format == format)
		{
			return &entry;
		}
	}
	return nullptr;
}

const ShaderBufferFormat* ShaderGen5BufferFormats(uint32_t* count)
{
	*count = static_cast<uint32_t>(k_formats.size());
	return k_formats.data();
}

} // namespace Kyty::Libs::Graphics

#endif
