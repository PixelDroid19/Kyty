#include "Emulator/Agent/AgentServer.h"
#include "Emulator/Agent/AgentSubsystem.h"
#include "Emulator/Agent/EventRing.h"

namespace Kyty::Emulator::Agent {

KYTY_SUBSYSTEM_INIT(AgentTools)
{
	if (!StartFromEnv())
	{
		KYTY_SUBSYSTEM_FAIL("agent tools failed to start from KYTY_AGENT_SOCK");
	}
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(AgentTools)
{
	Stop();
}

KYTY_SUBSYSTEM_DESTROY(AgentTools)
{
	Stop();
}

} // namespace Kyty::Emulator::Agent
