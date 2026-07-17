#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TRANSPORT_BOOTSTRAP_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TRANSPORT_BOOTSTRAP_H_

#include <cstdint>

namespace Kyty::DevTools {

struct BootstrapNonce
{
	uint8_t bytes[16] = {};
};

enum class BootstrapPlatform: uint8_t
{
	Posix   = 1,
	Windows = 2
};

enum class BootstrapParseResult: uint8_t
{
	Missing   = 0,
	Valid     = 1,
	Malformed = 2
};

struct BootstrapMetadata
{
	BootstrapPlatform platform        = BootstrapPlatform::Posix;
	uint64_t          mapping_handle  = 0;
	uint64_t          liveness_handle = 0;
	BootstrapNonce    nonce {};
};

struct BootstrapText
{
	char     bytes[80] = {};
	uint32_t size      = 0;
};

// Env var name is intentional; value is never logged by DevTools.
inline constexpr char kBootstrapEnvName[] = "KYTY_DEVTOOLS_BOOTSTRAP_V1";

// POSIX grammar: p:3:4: + 32 lowercase hex nonce chars.
// Windows grammar: w:<hex handle>:<hex handle>:<32 lowercase hex nonce>
[[nodiscard]] BootstrapParseResult ParseBootstrapMetadata(const char* value, BootstrapMetadata* out) noexcept;
[[nodiscard]] bool EncodeBootstrapMetadata(const BootstrapMetadata& meta, BootstrapText* out) noexcept;

[[nodiscard]] bool NonceEqual(const BootstrapNonce& a, const BootstrapNonce& b) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TRANSPORT_BOOTSTRAP_H_ */
