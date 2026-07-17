#ifndef KYTY_INCLUDE_KYTY_AGENT_WIRECONTRACT_H_
#define KYTY_INCLUDE_KYTY_AGENT_WIRECONTRACT_H_

#include <cstddef>
#include <cstdint>

namespace Kyty::Agent {

inline constexpr uint32_t kProtocolVersion = 3u;
inline constexpr size_t   kRequestLineMax  = 4096u;
inline constexpr size_t   kResponseLineMax = 262144u;

} // namespace Kyty::Agent

#endif /* KYTY_INCLUDE_KYTY_AGENT_WIRECONTRACT_H_ */
