#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_SUPERVISOR_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_SUPERVISOR_H_

#include "Kyty/DevTools/Diagnostics/ProcessStatus.h"
#include "Kyty/DevTools/Protocol/Protocol.h"

#include <cstdint>

namespace Kyty::DevTools {

enum class SupervisorOutcome: uint8_t
{
	ChildExited            = 0,
	ChildCrashed           = 1,
	ChildTerminated        = 2,
	StatusDecodeError      = 3,
	LaunchError            = 4,
	WorkerHandshakeFailed  = 5,
	ProtocolError          = 6,
	BundleError            = 7,
	UserInterrupted        = 8
};

struct SupervisorResult
{
	SupervisorOutcome outcome = SupervisorOutcome::ProtocolError;
	ProcessStatus     process {};
	uint32_t          platform_error = 0;
	// Sanitized absolute bundle path when one was written; empty otherwise.
	char              bundle_path[1024] = {};
	uint32_t          bundle_path_size  = 0;
	uint32_t          bundles_written   = 0;
};

struct SupervisorOptions
{
	const char*        absolute_output_dir = nullptr;
	const char*        worker              = nullptr;
	const char* const* worker_argv         = nullptr;
	uint32_t           worker_argc         = 0;
	RecordingMode      mode                = RecordingMode::Full;
	uint64_t           sample_period_ns    = 250'000'000ull; // 250 ms
	uint64_t           suspicion_ns        = 5'000'000'000ull;
	uint64_t           confirmation_ns     = 15'000'000'000ull;
	uint64_t           handshake_timeout_ns = 5'000'000'000ull;
	// When non-zero, stop sampling after this many samples (tests / self-test).
	uint64_t           max_samples         = 0;
	// When true, force one bundle on first confirmed stall then continue.
	// Production always continues after live stall capture.
	uint8_t            capture_stall_once  = 1;
};

[[nodiscard]] SupervisorResult RunSupervisor(const SupervisorOptions& options) noexcept;

// Duration-bounded measurement workflow (not stall recovery).
[[nodiscard]] SupervisorResult RunMeasurement(const SupervisorOptions& options,
                                              uint64_t duration_ns) noexcept;

// Internal synthetic self-test harness. Returns 0 on expected outcome.
[[nodiscard]] int RunSelfTest(const char* synthetic_mode, const char* absolute_output_dir,
                              uint64_t suspected_ms, uint64_t confirmed_ms,
                              uint64_t cleanup_ms) noexcept;

// Synthetic worker entry used by the kyty_devtools binary itself when argv0 mode is set.
// Returns process exit code.
[[nodiscard]] int RunSyntheticWorker(const char* mode, int argc, char** argv) noexcept;

// CLI entry for the kyty_devtools executable.
[[nodiscard]] int DevToolsMain(int argc, char** argv) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_SUPERVISOR_H_ */
