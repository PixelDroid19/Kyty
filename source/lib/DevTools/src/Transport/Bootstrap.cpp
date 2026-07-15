#include "Kyty/DevTools/Transport/Bootstrap.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace Kyty::DevTools {
namespace {

[[nodiscard]] int HexValue(char c) noexcept
{
	if (c >= '0' && c <= '9')
	{
		return c - '0';
	}
	if (c >= 'a' && c <= 'f')
	{
		return 10 + (c - 'a');
	}
	return -1;
}

[[nodiscard]] bool ParseHex32(const char* p, BootstrapNonce* nonce) noexcept
{
	for (int i = 0; i < 16; ++i)
	{
		const int hi = HexValue(p[i * 2]);
		const int lo = HexValue(p[i * 2 + 1]);
		if (hi < 0 || lo < 0)
		{
			return false;
		}
		nonce->bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
	}
	return true;
}

void WriteHex32(const BootstrapNonce& nonce, char* out) noexcept
{
	static constexpr char kDigits[] = "0123456789abcdef";
	for (int i = 0; i < 16; ++i)
	{
		out[i * 2]     = kDigits[(nonce.bytes[i] >> 4) & 0xf];
		out[i * 2 + 1] = kDigits[nonce.bytes[i] & 0xf];
	}
}

[[nodiscard]] bool ParseHexU64(const char* begin, const char* end, uint64_t* out) noexcept
{
	if (begin == nullptr || end == nullptr || out == nullptr || begin >= end)
	{
		return false;
	}
	const auto len = static_cast<size_t>(end - begin);
	if (len == 0u || len > 16u)
	{
		return false;
	}
	uint64_t v = 0;
	for (const char* p = begin; p != end; ++p)
	{
		const int d = HexValue(*p);
		if (d < 0)
		{
			return false;
		}
		v = (v << 4) | static_cast<uint64_t>(d);
	}
	if (v == 0u)
	{
		return false;
	}
	*out = v;
	return true;
}

} // namespace

bool NonceEqual(const BootstrapNonce& a, const BootstrapNonce& b) noexcept
{
	return std::memcmp(a.bytes, b.bytes, 16) == 0;
}

BootstrapParseResult ParseBootstrapMetadata(const char* value, BootstrapMetadata* out) noexcept
{
	if (out == nullptr)
	{
		return BootstrapParseResult::Malformed;
	}
	*out = {};
	if (value == nullptr || value[0] == '\0')
	{
		return BootstrapParseResult::Missing;
	}

	// POSIX: p:3:4:<32 hex>
	if (value[0] == 'p' && value[1] == ':' && value[2] == '3' && value[3] == ':' && value[4] == '4' && value[5] == ':')
	{
		const char* nonce = value + 6;
		if (std::strlen(nonce) != 32u)
		{
			return BootstrapParseResult::Malformed;
		}
		if (!ParseHex32(nonce, &out->nonce))
		{
			return BootstrapParseResult::Malformed;
		}
		out->platform        = BootstrapPlatform::Posix;
		out->mapping_handle  = 3;
		out->liveness_handle = 4;
		return BootstrapParseResult::Valid;
	}

	// Windows: w:<hex>:<hex>:<32 hex>
	if (value[0] == 'w' && value[1] == ':')
	{
		const char* p1 = value + 2;
		const char* c1 = std::strchr(p1, ':');
		if (c1 == nullptr)
		{
			return BootstrapParseResult::Malformed;
		}
		const char* p2 = c1 + 1;
		const char* c2 = std::strchr(p2, ':');
		if (c2 == nullptr)
		{
			return BootstrapParseResult::Malformed;
		}
		const char* nonce = c2 + 1;
		if (std::strlen(nonce) != 32u)
		{
			return BootstrapParseResult::Malformed;
		}
		uint64_t map_h = 0;
		uint64_t live_h = 0;
		if (!ParseHexU64(p1, c1, &map_h) || !ParseHexU64(p2, c2, &live_h) || !ParseHex32(nonce, &out->nonce))
		{
			return BootstrapParseResult::Malformed;
		}
		out->platform        = BootstrapPlatform::Windows;
		out->mapping_handle  = map_h;
		out->liveness_handle = live_h;
		return BootstrapParseResult::Valid;
	}

	return BootstrapParseResult::Malformed;
}

bool EncodeBootstrapMetadata(const BootstrapMetadata& meta, BootstrapText* out) noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	*out = {};
	char nonce_hex[33] = {};
	WriteHex32(meta.nonce, nonce_hex);
	nonce_hex[32] = '\0';

	int n = 0;
	if (meta.platform == BootstrapPlatform::Posix)
	{
		if (meta.mapping_handle != 3u || meta.liveness_handle != 4u)
		{
			return false;
		}
		n = std::snprintf(out->bytes, sizeof(out->bytes), "p:3:4:%s", nonce_hex);
	} else if (meta.platform == BootstrapPlatform::Windows)
	{
		if (meta.mapping_handle == 0u || meta.liveness_handle == 0u)
		{
			return false;
		}
		n = std::snprintf(out->bytes, sizeof(out->bytes), "w:%llx:%llx:%s",
		                  static_cast<unsigned long long>(meta.mapping_handle),
		                  static_cast<unsigned long long>(meta.liveness_handle), nonce_hex);
	} else
	{
		return false;
	}
	if (n <= 0 || static_cast<size_t>(n) >= sizeof(out->bytes))
	{
		return false;
	}
	out->size = static_cast<uint32_t>(n);
	return true;
}

} // namespace Kyty::DevTools
