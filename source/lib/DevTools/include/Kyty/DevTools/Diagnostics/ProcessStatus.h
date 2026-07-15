#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_DIAGNOSTICS_PROCESSSTATUS_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_DIAGNOSTICS_PROCESSSTATUS_H_

#include <cstdint>

namespace Kyty::DevTools {

enum class ProcessLiveness: uint8_t
{
	Unknown    = 0,
	Running    = 1,
	Terminated = 2
};

enum class ProcessTermination: uint8_t
{
	None                 = 0,
	ExitCode             = 1,
	Signal               = 2,
	UnhandledException   = 3,
	OpaquePlatformStatus = 4
};

enum class ProcessStatusError: uint8_t
{
	None                   = 0,
	WaitFailed             = 1,
	QueryFailed            = 2,
	MalformedTerminalStatus = 3
};

struct ProcessStatus
{
	ProcessLiveness     liveness     = ProcessLiveness::Unknown;
	ProcessTermination  termination  = ProcessTermination::None;
	ProcessStatusError  error        = ProcessStatusError::None;
	uint8_t             code_valid   = 0;
	uint32_t            code         = 0;
	uint32_t            platform_status = 0;
	uint32_t            platform_error  = 0;
};

[[nodiscard]] bool ValidateProcessStatus(const ProcessStatus& status) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_DIAGNOSTICS_PROCESSSTATUS_H_ */
