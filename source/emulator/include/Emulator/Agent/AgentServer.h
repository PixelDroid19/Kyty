#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSERVER_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSERVER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {

// Opt-in realtime agent control plane. Starts only when KYTY_AGENT_ENDPOINT is
// set to a valid local endpoint. Disabled by default.
bool               StartFromEnv();
void               Stop();
[[nodiscard]] bool Active();

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSERVER_H_ */
