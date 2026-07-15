#include "Kyty/DevTools/Diagnostics/Checksum.h"

namespace Kyty::DevTools {
namespace {

constexpr uint64_t kPoly = 0x42F0E1EBA9EA3693ull;

} // namespace

uint64_t Crc64Ecma(const void* data, size_t size) noexcept
{
	const auto* bytes = static_cast<const uint8_t*>(data);
	uint64_t    crc   = 0;
	if (bytes == nullptr && size != 0u)
	{
		return 0;
	}
	for (size_t i = 0; i < size; ++i)
	{
		crc ^= (static_cast<uint64_t>(bytes[i]) << 56u);
		for (int b = 0; b < 8; ++b)
		{
			if ((crc & 0x8000000000000000ull) != 0u)
			{
				crc = (crc << 1u) ^ kPoly;
			} else
			{
				crc <<= 1u;
			}
		}
	}
	return crc;
}

} // namespace Kyty::DevTools
