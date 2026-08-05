#include "Emulator/Agent/AgentServer.h"
#include "Emulator/Agent/AgentSubsystem.h"
#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Agent/EventRing.h"

#include "Kyty/Core/BringUp.h"

namespace Kyty::Emulator::Agent {

void AgentToolsSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	// Install lifecycle hooks even when the socket is unset (events still accumulate
	// for later diagnostics if a client attaches; observation never mutates guest).
	Lifecycle::InstallHooks();
	const auto cfg = Core::BringUp::GetConfig();
	Lifecycle::EmitStartupConfig(cfg.mode == Core::BringUp::Mode::Unsafe ? "unsafe" : "strict",
	                             cfg.explicitly_configured);

	if (!StartFromEnv())
	{
		this->Fail("agent tools failed to start from KYTY_AGENT_ENDPOINT");
		return;
	}
}

void AgentToolsSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent)
{
	Stop();
}

void AgentToolsSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
{
	Stop();
}

} // namespace Kyty::Emulator::Agent
