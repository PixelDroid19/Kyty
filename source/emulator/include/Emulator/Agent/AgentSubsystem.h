#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSUBSYSTEM_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSUBSYSTEM_H_

#include "Kyty/Core/Subsystems.h"

namespace Kyty::Emulator::Agent {

class AgentToolsSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<AgentToolsSubsystem>::Instance(); }
	const char*       Id() override { return "AgentTools"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


} // namespace Kyty::Emulator::Agent

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_AGENTSUBSYSTEM_H_ */
