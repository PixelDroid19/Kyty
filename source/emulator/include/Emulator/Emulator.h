#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_

#include "Kyty/Core/Subsystems.h"

namespace Kyty::Emulator {

class EmulatorSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<EmulatorSubsystem>::Instance(); }
	const char*       Id() override { return "Emulator"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


} // namespace Kyty::Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
