#include "Kyty/Agent/Runner/Sha256.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Kyty::Agent::Runner {
namespace {

// FIPS 180-4 round constants.
constexpr std::array<uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t Rotr(uint32_t value, uint32_t bits)
{
	return (value >> bits) | (value << (32u - bits));
}

void Compress(std::array<uint32_t, 8>* state, const uint8_t* block)
{
	std::array<uint32_t, 64> w {};
	for (size_t i = 0; i < 16; ++i)
	{
		w[i] = (static_cast<uint32_t>(block[i * 4]) << 24u) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16u) |
		       (static_cast<uint32_t>(block[i * 4 + 2]) << 8u) | static_cast<uint32_t>(block[i * 4 + 3]);
	}
	for (size_t i = 16; i < 64; ++i)
	{
		const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3u);
		const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10u);
		w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
	}
	auto v = *state;
	for (size_t i = 0; i < 64; ++i)
	{
		const uint32_t s1    = Rotr(v[4], 6) ^ Rotr(v[4], 11) ^ Rotr(v[4], 25);
		const uint32_t ch    = (v[4] & v[5]) ^ (~v[4] & v[6]);
		const uint32_t temp1 = v[7] + s1 + ch + kRound[i] + w[i];
		const uint32_t s0    = Rotr(v[0], 2) ^ Rotr(v[0], 13) ^ Rotr(v[0], 22);
		const uint32_t maj   = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
		v[7]                 = v[6];
		v[6]                 = v[5];
		v[5]                 = v[4];
		v[4]                 = v[3] + temp1;
		v[3]                 = v[2];
		v[2]                 = v[1];
		v[1]                 = v[0];
		v[0]                 = temp1 + s0 + maj;
	}
	for (size_t i = 0; i < 8; ++i)
	{
		(*state)[i] += v[i];
	}
}

} // namespace

Sha256::Sha256(): m_state {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::Update(const void* data, size_t size)
{
	const auto* bytes = static_cast<const uint8_t*>(data);
	m_total += size;
	while (size > 0)
	{
		const size_t take = std::min(size, m_block.size() - m_used);
		std::memcpy(m_block.data() + m_used, bytes, take);
		m_used += take;
		bytes += take;
		size -= take;
		if (m_used == m_block.size())
		{
			Compress(&m_state, m_block.data());
			m_used = 0;
		}
	}
}

std::string Sha256::FinalHex()
{
	const uint64_t bits = m_total * 8u;
	const uint8_t  pad  = 0x80;
	Update(&pad, 1);
	const uint8_t zero = 0;
	while (m_used != 56)
	{
		Update(&zero, 1);
	}
	for (int i = 7; i >= 0; --i)
	{
		const auto byte = static_cast<uint8_t>(bits >> (static_cast<uint32_t>(i) * 8u));
		Update(&byte, 1);
	}
	std::string hex;
	for (const uint32_t word: m_state)
	{
		char part[9];
		std::snprintf(part, sizeof(part), "%08x", word);
		hex += part;
	}
	return hex;
}

std::string Sha256Hex(const void* data, size_t size)
{
	Sha256 hash;
	hash.Update(data, size);
	return hash.FinalHex();
}

bool Sha256File(const std::string& path, std::string* hex, std::string* error)
{
	std::FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
	{
		*error = "cannot open " + path;
		return false;
	}
	Sha256                     hash;
	std::array<uint8_t, 65536> chunk {};
	size_t                     read = 0;
	while ((read = std::fread(chunk.data(), 1, chunk.size(), file)) > 0)
	{
		hash.Update(chunk.data(), read);
	}
	const bool failed = std::ferror(file) != 0;
	std::fclose(file);
	if (failed)
	{
		*error = "read failed: " + path;
		return false;
	}
	*hex = hash.FinalHex();
	return true;
}

} // namespace Kyty::Agent::Runner
