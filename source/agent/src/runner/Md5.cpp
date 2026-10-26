#include "Kyty/Agent/Runner/Md5.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace Kyty::Agent::Runner {
namespace {

// RFC 1321 per-round shift amounts and sine-derived constants.
constexpr std::array<uint32_t, 64> kShift = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                             14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4,  11, 16, 23,
                                             4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

constexpr std::array<uint32_t, 64> kSine = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
    0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
    0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
    0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
    0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr uint32_t Rotl(uint32_t value, uint32_t bits)
{
	return (value << bits) | (value >> (32u - bits));
}

void Compress(std::array<uint32_t, 4>* state, const uint8_t* block)
{
	std::array<uint32_t, 16> m {};
	for (size_t i = 0; i < 16; ++i)
	{
		m[i] = static_cast<uint32_t>(block[i * 4]) | (static_cast<uint32_t>(block[i * 4 + 1]) << 8u) |
		       (static_cast<uint32_t>(block[i * 4 + 2]) << 16u) | (static_cast<uint32_t>(block[i * 4 + 3]) << 24u);
	}
	uint32_t a = (*state)[0];
	uint32_t b = (*state)[1];
	uint32_t c = (*state)[2];
	uint32_t d = (*state)[3];
	for (uint32_t i = 0; i < 64; ++i)
	{
		uint32_t f = 0;
		uint32_t g = 0;
		if (i < 16)
		{
			f = (b & c) | (~b & d);
			g = i;
		} else if (i < 32)
		{
			f = (d & b) | (~d & c);
			g = (5u * i + 1u) % 16u;
		} else if (i < 48)
		{
			f = b ^ c ^ d;
			g = (3u * i + 5u) % 16u;
		} else
		{
			f = c ^ (b | ~d);
			g = (7u * i) % 16u;
		}
		const uint32_t rotated = Rotl(a + f + kSine[i] + m[g], kShift[i]);
		a                      = d;
		d                      = c;
		c                      = b;
		b                      = b + rotated;
	}
	(*state)[0] += a;
	(*state)[1] += b;
	(*state)[2] += c;
	(*state)[3] += d;
}

} // namespace

std::string Md5Hex(const void* data, size_t size)
{
	std::array<uint32_t, 4> state = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
	const auto*             bytes = static_cast<const uint8_t*>(data);
	size_t                  used  = 0;
	for (; used + 64 <= size; used += 64)
	{
		Compress(&state, bytes + used);
	}
	// Final block(s): the tail, 0x80, zero padding and the little-endian bit length.
	std::array<uint8_t, 128> tail {};
	const size_t             rest = size - used;
	if (rest > 0)
	{
		std::memcpy(tail.data(), bytes + used, rest);
	}
	tail[rest]                 = 0x80;
	const size_t   tail_size   = rest < 56 ? 64 : 128;
	const uint64_t bit_length  = static_cast<uint64_t>(size) * 8u;
	for (size_t i = 0; i < 8; ++i)
	{
		tail[tail_size - 8 + i] = static_cast<uint8_t>(bit_length >> (8u * i));
	}
	for (size_t offset = 0; offset < tail_size; offset += 64)
	{
		Compress(&state, tail.data() + offset);
	}
	static constexpr char kHex[] = "0123456789abcdef";
	std::string           hex;
	hex.reserve(32);
	for (uint32_t word: state)
	{
		for (uint32_t i = 0; i < 4; ++i)
		{
			const auto byte = static_cast<uint8_t>(word >> (8u * i));
			hex += kHex[byte >> 4u];
			hex += kHex[byte & 0xfu];
		}
	}
	return hex;
}

} // namespace Kyty::Agent::Runner
