#ifndef EMULATOR_INCLUDE_EMULATOR_DEVTOOLS_RUNTIMESUBSYSTEM_H_
#define EMULATOR_INCLUDE_EMULATOR_DEVTOOLS_RUNTIMESUBSYSTEM_H_

#include "Kyty/Core/Subsystems.h"

namespace Kyty::Emulator::DevTools {

class RuntimeDiagnosticsSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<RuntimeDiagnosticsSubsystem>::Instance(); }
	const char*       Id() override { return "RuntimeDiagnostics"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


} // namespace Kyty::Emulator::DevTools

#endif /* EMULATOR_INCLUDE_EMULATOR_DEVTOOLS_RUNTIMESUBSYSTEM_H_ */
