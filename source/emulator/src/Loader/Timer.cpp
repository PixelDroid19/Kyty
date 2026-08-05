#include "Kyty/Core/Timer.h"

#include "Kyty/Core/DateTime.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/TimePort.h"
#include "Emulator/Loader/Timer.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader::Timer {

static Core::Timer g_timer;

void TimerSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	Start();
	Kernel::TimePort::Install({&GetTimeMs, &GetCounter, &GetFrequency});
}

void TimerSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void TimerSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

void Start()
{
	g_timer.Start();
}

double GetTimeMs()
{
	return g_timer.GetTimeMs();
}

Core::Time GetTime()
{
	return Core::Time(static_cast<int>(GetTimeMs()));
}

uint64_t GetCounter()
{
	return g_timer.GetTicks();
}

uint64_t GetFrequency()
{
	return g_timer.GetFrequency();
}

} // namespace Kyty::Loader::Timer

#endif // KYTY_EMU_ENABLED
