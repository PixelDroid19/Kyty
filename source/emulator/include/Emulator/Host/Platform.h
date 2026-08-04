#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_PLATFORM_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_PLATFORM_H_

#include <cstdint>
#include <string>

namespace Kyty::Emulator::Host {

[[nodiscard]] std::string UtcTimestamp();
[[nodiscard]] uint64_t    PeakRssBytes();

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_PLATFORM_H_ */
