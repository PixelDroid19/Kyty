#include "Emulator/DevTools/Runtime.h"
#include "Emulator/DevTools/RuntimeSubsystem.h"

namespace Kyty::Emulator::DevTools {

KYTY_SUBSYSTEM_INIT(RuntimeDiagnostics)
{
	if (!Initialize())
	{
		KYTY_SUBSYSTEM_FAIL("runtime diagnostics was not prepared before emulator initialization");
	}
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(RuntimeDiagnostics)
{
	(void)Shutdown();
}

KYTY_SUBSYSTEM_DESTROY(RuntimeDiagnostics)
{
	(void)Shutdown();
}

} // namespace Kyty::Emulator::DevTools
