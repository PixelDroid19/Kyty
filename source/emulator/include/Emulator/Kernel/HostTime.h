#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_HOSTTIME_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_HOSTTIME_H_

#include <cstdint>
#include <ctime>

// Host-only calendar conversion primitives used by the libkernel clock
// exports. Keeping these routines outside LibKernel prevents guest ABI data
// and registration state from leaking into the platform implementation.
namespace Kyty::Kernel::HostTime {

[[nodiscard]] int64_t CivilToUnixSeconds(const std::tm& value);
[[nodiscard]] bool    LocaltimeFromUtc(int64_t utc_seconds, std::tm* out);
[[nodiscard]] bool    GmtimeFromUnixSeconds(int64_t seconds, std::tm* out);

} // namespace Kyty::Kernel::HostTime

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_HOSTTIME_H_ */
