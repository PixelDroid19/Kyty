#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_DURABLEFILE_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_DURABLEFILE_H_

#include <cstdint>

namespace Kyty::DevTools {

enum class DurableIoResult: uint8_t
{
	Ok              = 0,
	InvalidArgument = 1,
	NotFound        = 2,
	Conflict        = 3,
	IoError         = 4,
	DurabilityError = 5,
	Unsupported     = 6,
	Retained        = 7
};

// Injectable clock for temp-directory age checks (nanoseconds, monotonic-ish).
using DurableClockNs = uint64_t (*)() noexcept;

// Default: CLOCK_REALTIME wall ns (age comparisons only; not hot-path).
[[nodiscard]] DurableClockNs DefaultDurableClock() noexcept;

// Write entire buffer with explicit loops; fsync file; close.
[[nodiscard]] DurableIoResult DurableWriteFile(const char* absolute_path, const void* data,
                                               uint64_t size) noexcept;

// fsync an existing file path (open read-only or write).
[[nodiscard]] DurableIoResult DurableFsyncPath(const char* absolute_path) noexcept;

// fsync directory by path (open O_DIRECTORY).
[[nodiscard]] DurableIoResult DurableFsyncDirectory(const char* absolute_dir) noexcept;

// Create directory (mode 0700). Fails with Conflict if exists.
[[nodiscard]] DurableIoResult DurableCreateDirectory(const char* absolute_dir) noexcept;

// Rename within one filesystem (no cross-device copy).
[[nodiscard]] DurableIoResult DurableRename(const char* from_absolute, const char* to_absolute) noexcept;

// Remove a single empty directory.
[[nodiscard]] DurableIoResult DurableRemoveEmptyDirectory(const char* absolute_dir) noexcept;

// Unlink one file.
[[nodiscard]] DurableIoResult DurableUnlinkFile(const char* absolute_path) noexcept;

// True if path exists (any type).
[[nodiscard]] bool DurablePathExists(const char* absolute_path) noexcept;

// Own-pattern temp cleanup under parent_dir. Pattern:
//   .kyty-bundle-tmp.<pid>.<start_token>.<generation>
// Removes only when owner is dead (matching start token), age >= 24h (via clock),
// and names parse cleanly. Live / young / unreadable / PID-reused owners are retained.
struct TempCleanupResult
{
	uint32_t removed = 0;
	uint32_t retained = 0;
	uint32_t scanned  = 0;
};

[[nodiscard]] DurableIoResult DurableCleanupOwnTemps(const char* parent_dir, DurableClockNs clock,
                                                     TempCleanupResult* out) noexcept;

// Current process identity for temp ownership (Linux /proc/self/stat).
[[nodiscard]] bool QuerySelfProcessIdentity(uint64_t* pid, uint64_t* start_token) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_DURABLEFILE_H_ */
