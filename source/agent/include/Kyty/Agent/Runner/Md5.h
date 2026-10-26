#ifndef KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_MD5_H_
#define KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_MD5_H_

#include <cstddef>
#include <string>

namespace Kyty::Agent::Runner {

// Lowercase hex MD5 (RFC 1321). Binds trace sidecars to their exact input
// bytes, as the live producer does; it is not a security digest.
[[nodiscard]] std::string Md5Hex(const void* data, size_t size);

} // namespace Kyty::Agent::Runner

#endif /* KYTY_AGENT_INCLUDE_KYTY_AGENT_RUNNER_MD5_H_ */
