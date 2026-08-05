#include "Emulator/DevTools/Runtime.h"
#include "Emulator/DevTools/RuntimeSubsystem.h"

namespace Kyty::Emulator::DevTools {

void RuntimeDiagnosticsSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	if (!Initialize())
	{
		this->Fail("runtime diagnostics was not prepared before emulator initialization");
		return;
	}
}

void RuntimeDiagnosticsSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent)
{
	(void)Shutdown();
}

void RuntimeDiagnosticsSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
{
	(void)Shutdown();
}

} // namespace Kyty::Emulator::DevTools
