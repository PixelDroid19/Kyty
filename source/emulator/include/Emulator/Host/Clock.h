#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_CLOCK_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_CLOCK_H_

#include <cstdint>

namespace Kyty::Emulator::HostClock {

[[nodiscard]] uint64_t NowMicroseconds();
void                   SleepUntil(uint64_t deadline_microseconds);

} // namespace Kyty::Emulator::HostClock

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_CLOCK_H_ */
