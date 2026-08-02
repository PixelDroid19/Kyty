#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSERVER_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSERVER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {

struct Request;

namespace Internal {

// Read-only production dispatcher seam for process-isolated integration. It
// shares the realtime server's handlers without adding a second CLI surface.
[[nodiscard]] std::string DispatchRequest(const Request& req);

} // namespace Internal

// Opt-in realtime agent control plane. Starts only when KYTY_AGENT_ENDPOINT is
// set to a valid local endpoint. Disabled by default.
bool               StartFromEnv();
void               Stop();
[[nodiscard]] bool Active();

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSERVER_H_ */
