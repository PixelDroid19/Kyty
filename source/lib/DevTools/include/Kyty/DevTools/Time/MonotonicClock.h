#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TIME_MONOTONICCLOCK_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TIME_MONOTONICCLOCK_H_

#include <cstdint>

namespace Kyty::DevTools {

// Platform monotonic clock for diagnostics only (CLOCK_MONOTONIC / QPC).
// Never wall time, never std::chrono::steady_clock.
[[nodiscard]] uint64_t MonotonicNowNs() noexcept;

// Checked conversion helpers for unit tests and platform adapters.
[[nodiscard]] bool MonotonicFromPosixTimespec(int64_t sec, int64_t nsec, uint64_t* out_ns) noexcept;
[[nodiscard]] bool MonotonicFromWindowsCounter(uint64_t counter, uint64_t frequency, uint64_t* out_ns) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_TIME_MONOTONICCLOCK_H_ */
