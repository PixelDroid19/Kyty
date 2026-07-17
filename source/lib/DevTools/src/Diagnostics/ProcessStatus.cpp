#include "Kyty/DevTools/Diagnostics/ProcessStatus.h"

namespace Kyty::DevTools {

bool ValidateProcessStatus(const ProcessStatus& status) noexcept
{
	if (status.error != ProcessStatusError::None)
	{
		if (status.liveness != ProcessLiveness::Unknown || status.termination != ProcessTermination::None || status.code_valid != 0)
		{
			return false;
		}
		if (status.error == ProcessStatusError::WaitFailed || status.error == ProcessStatusError::QueryFailed)
		{
			return status.platform_error != 0u;
		}
		// MalformedTerminalStatus may have zero platform_error.
		return true;
	}

	if (status.liveness == ProcessLiveness::Unknown || status.liveness == ProcessLiveness::Running)
	{
		return status.termination == ProcessTermination::None && status.code_valid == 0u && status.code == 0u &&
		       status.platform_error == 0u;
	}

	if (status.liveness != ProcessLiveness::Terminated)
	{
		return false;
	}

	switch (status.termination)
	{
		case ProcessTermination::ExitCode:
		case ProcessTermination::Signal:
		case ProcessTermination::UnhandledException:
			return status.code_valid == 1u && status.platform_error == 0u;
		case ProcessTermination::OpaquePlatformStatus:
			return status.code_valid == 0u && status.code == 0u;
		default: return false;
	}
}

} // namespace Kyty::DevTools
