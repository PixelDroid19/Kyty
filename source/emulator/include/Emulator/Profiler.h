#ifndef EMULATOR_INCLUDE_EMULATOR_PROFILER_H_
#define EMULATOR_INCLUDE_EMULATOR_PROFILER_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Profiling.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Profiler {

class ProfilerSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<ProfilerSubsystem>::Instance(); }
	const char*       Id() override { return "Profiler"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


} // namespace Kyty::Profiler

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_PROFILER_H_ */
