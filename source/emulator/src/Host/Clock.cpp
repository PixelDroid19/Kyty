#include "Emulator/Host/Clock.h"

#include "Kyty/Core/Threads.h"

#include <chrono>
#include <limits>

namespace Kyty::Emulator::HostClock {

uint64_t NowMicroseconds()
{
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t NextPeriodicIntervalMicroseconds(uint32_t samples, uint32_t frequency, uint64_t* remainder)
{
	if (samples == 0 || frequency == 0 || remainder == nullptr)
	{
		return 0;
	}
	*remainder %= frequency;
	const uint64_t numerator = 1'000'000ull * samples + *remainder;
	*remainder               = numerator % frequency;
	return numerator / frequency;
}

void SleepUntil(uint64_t deadline_microseconds)
{
	constexpr uint64_t max_sleep_microseconds = std::numeric_limits<uint32_t>::max();
	for (;;)
	{
		const uint64_t now = NowMicroseconds();
		if (now >= deadline_microseconds)
		{
			return;
		}
		const uint64_t remaining = deadline_microseconds - now;
		Core::Thread::SleepMicro(static_cast<uint32_t>(remaining > max_sleep_microseconds ? max_sleep_microseconds : remaining));
	}
}

} // namespace Kyty::Emulator::HostClock
