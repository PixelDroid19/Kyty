#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_PLATFORM_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_PLATFORM_H_

#include <cstdint>
#include <filesystem>
#include <string>

namespace Kyty::Emulator::Host {

[[nodiscard]] std::string UtcTimestamp();
[[nodiscard]] std::string ApplicationBasePath();
// Returns the platform-native per-user cache root.  Graphics caches append
// their own schema/version directory and do not need platform conditionals.
[[nodiscard]] std::filesystem::path DefaultCacheDirectory();
[[nodiscard]] uint64_t    PeakRssBytes();

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_PLATFORM_H_ */
