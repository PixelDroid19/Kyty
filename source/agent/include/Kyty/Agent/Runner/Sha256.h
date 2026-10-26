#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_SHA256_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_SHA256_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Kyty::Agent::Runner {

// SHA-256 (FIPS 180-4), used to pin captures, references, contracts and binaries.
class Sha256
{
public:
	Sha256();
	void                      Update(const void* data, size_t size);
	[[nodiscard]] std::string FinalHex();

private:
	std::array<uint32_t, 8> m_state;
	std::array<uint8_t, 64> m_block {};
	size_t                  m_used  = 0;
	uint64_t                m_total = 0;
};

// Lowercase hex digests.
[[nodiscard]] std::string Sha256Hex(const void* data, size_t size);
[[nodiscard]] bool        Sha256File(const std::string& path, std::string* hex, std::string* error);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_SHA256_H_ */
