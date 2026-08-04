#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_CLOCK_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_CLOCK_H_

#include <cstdint>

namespace Kyty::Emulator::HostClock {

// AudioHost accepts at most one second of guest-controlled pacing per period.
// This preserves the known 48 kHz and 44.1 kHz paths while bounding host sleep.
inline constexpr uint64_t kMaxPeriodicIntervalMicroseconds = 1'000'000;

[[nodiscard]] uint64_t NowMicroseconds();
void                   SleepUntil(uint64_t deadline_microseconds);

[[nodiscard]] bool IsPeriodicIntervalValid(uint32_t samples, uint32_t frequency);

// Advances a samples/frequency periodic schedule without losing its fractional
// microseconds. Invalid inputs or a null remainder produce no interval.
[[nodiscard]] uint64_t NextPeriodicIntervalMicroseconds(uint32_t samples, uint32_t frequency, uint64_t* remainder);

} // namespace Kyty::Emulator::HostClock

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_CLOCK_H_ */
