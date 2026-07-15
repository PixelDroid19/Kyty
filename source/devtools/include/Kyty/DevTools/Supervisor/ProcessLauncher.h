#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_PROCESSLAUNCHER_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_PROCESSLAUNCHER_H_

#include "Kyty/DevTools/Diagnostics/ProcessStatus.h"
#include "Kyty/DevTools/Supervisor/SharedMapping.h"
#include "Kyty/DevTools/Transport/Bootstrap.h"

#include <cstdint>
#include <memory>

namespace Kyty::DevTools {

struct ProcessUsage
{
	uint64_t user_ns   = 0;
	uint64_t system_ns = 0;
	uint8_t  valid     = 0;
};

struct ProcessObservation
{
	ProcessStatus status {};
	ProcessUsage  usage {};
};

class ProcessHandle
{
public:
	ProcessHandle() noexcept;
	ProcessHandle(ProcessHandle&&) noexcept;
	ProcessHandle& operator=(ProcessHandle&&) noexcept;
	~ProcessHandle();
	ProcessHandle(const ProcessHandle&)            = delete;
	ProcessHandle& operator=(const ProcessHandle&) = delete;
	[[nodiscard]] bool IsValid() const noexcept;

	// Internal: used by launcher implementations only.
	struct State;
	[[nodiscard]] State*       GetState() noexcept { return state_.get(); }
	[[nodiscard]] const State* GetState() const noexcept { return state_.get(); }
	void                       Reset(std::unique_ptr<State> s) noexcept;

private:
	std::unique_ptr<State> state_;
};

struct LaunchResult
{
	ProcessOperationError error          = ProcessOperationError::None;
	uint32_t              platform_error = 0;
	ProcessHandle         process {};
};

struct LaunchOptions
{
	const char*       executable = nullptr;
	const char* const* argv      = nullptr;
	uint32_t          argc       = 0;
	BootstrapText     bootstrap {};
	// Optional: parent provides mapping fd to remap to 3 and a liveness pipe read end for 4.
	int mapping_fd  = -1;
	int liveness_fd = -1;
};

class ProcessLauncher
{
public:
	static LaunchResult          Launch(const LaunchOptions& options) noexcept;
	static ProcessOperationError Poll(ProcessHandle* handle, ProcessObservation* out) noexcept;
	static ProcessOperationError Wait(ProcessHandle* handle, ProcessObservation* out) noexcept;
	static ProcessOperationError ForwardSignal(ProcessHandle* handle, uint32_t signal) noexcept;
};

// Linux /proc start-ticks identity helpers (pure parsing + query).
struct ProcessIdentity
{
	uint64_t pid         = 0;
	uint64_t start_token = 0;
};

enum class ProcessIdentityError: uint8_t
{
	None        = 0,
	Unavailable = 1,
	Malformed   = 2,
	Overflow    = 3
};

enum class ProcessIdentityProbe: uint8_t
{
	Dead                = 0,
	AliveMatch          = 1,
	AliveDifferentStart = 2,
	Unreadable          = 3,
	Malformed           = 4,
	Overflow            = 5
};

[[nodiscard]] ProcessIdentityError QueryProcessIdentity(const ProcessHandle& handle, ProcessIdentity* out) noexcept;
[[nodiscard]] ProcessIdentityProbe ProbeProcessIdentity(uint64_t pid, uint64_t expected_start_token) noexcept;

// Pure parser for Linux /proc/<pid>/stat field 22 (starttime) with parenthesized cmd.
[[nodiscard]] bool ParseLinuxProcStatStartTicks(const char* stat_line, uint64_t* start_ticks) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_SUPERVISOR_PROCESSLAUNCHER_H_ */
