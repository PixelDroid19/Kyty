#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DateTime.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader::Timer {

class TimerSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<TimerSubsystem>::Instance(); }
	const char*       Id() override { return "Timer"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


void       Start();
double     GetTimeMs();
Core::Time GetTime();
uint64_t   GetCounter();
uint64_t   GetFrequency();

} // namespace Kyty::Loader::Timer

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_ */
