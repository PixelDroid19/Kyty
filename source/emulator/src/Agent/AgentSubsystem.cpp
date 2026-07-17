#include "Emulator/Agent/AgentServer.h"
#include "Emulator/Agent/AgentSubsystem.h"
#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Agent/EventRing.h"

#include "Kyty/Core/BringUp.h"

namespace Kyty::Emulator::Agent {

KYTY_SUBSYSTEM_INIT(AgentTools)
{
	// Install lifecycle hooks even when the socket is unset (events still accumulate
	// for later diagnostics if a client attaches; observation never mutates guest).
	Lifecycle::InstallHooks();
	const auto cfg = Core::BringUp::GetConfig();
	Lifecycle::EmitStartupConfig(cfg.mode == Core::BringUp::Mode::Unsafe ? "unsafe" : "strict",
	                             cfg.explicitly_configured);

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
