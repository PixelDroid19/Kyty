#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_TIME_PORT_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_TIME_PORT_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DateTime.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::TimePort {

using GetTimeMsFunction       = double (*)();
using GetCounterFunction      = uint64_t (*)();
using GetFrequencyFunction    = uint64_t (*)();

struct Provider
{
	GetTimeMsFunction    get_time_ms    = nullptr;
	GetCounterFunction   get_counter    = nullptr;
	GetFrequencyFunction get_frequency = nullptr;
};

// The Loader timer installs its provider during subsystem initialization. A
// monotonic Core::Timer fallback keeps kernel calls valid in focused tests and
// in early bring-up before the Loader subsystem is available.
void Install(const Provider& provider) noexcept;

[[nodiscard]] double     GetTimeMs() noexcept;
[[nodiscard]] Core::Time GetTime() noexcept;
[[nodiscard]] uint64_t   GetCounter() noexcept;
[[nodiscard]] uint64_t   GetFrequency() noexcept;

} // namespace Kyty::Kernel::TimePort

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_TIME_PORT_H_ */
