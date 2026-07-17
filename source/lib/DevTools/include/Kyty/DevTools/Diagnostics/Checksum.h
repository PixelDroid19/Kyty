#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_DIAGNOSTICS_CHECKSUM_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_DIAGNOSTICS_CHECKSUM_H_

#include <cstddef>
#include <cstdint>

namespace Kyty::DevTools {

// CRC-64/ECMA-182: poly 0x42F0E1EBA9EA3693, init 0, xorout 0, non-reflected.
// Check value for ASCII "123456789" is 0x6C40DF5F0B497347.
[[nodiscard]] uint64_t Crc64Ecma(const void* data, size_t size) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_DIAGNOSTICS_CHECKSUM_H_ */
