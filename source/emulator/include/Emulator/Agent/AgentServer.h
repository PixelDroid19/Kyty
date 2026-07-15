#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSERVER_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSERVER_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {

// Opt-in realtime agent control plane. Starts only when KYTY_AGENT_SOCK is an
// absolute Unix socket path. Disabled by default (no thread, no behavior change).
bool StartFromEnv();
void Stop();
[[nodiscard]] bool Active();

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSERVER_H_ */
