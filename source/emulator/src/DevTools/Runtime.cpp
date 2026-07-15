#include "Emulator/DevTools/Runtime.h"

#include "Kyty/DevTools/Transport/Bootstrap.h"

#include <cstdlib>

namespace Kyty::Emulator::DevTools {
namespace {

struct RuntimeState
{
	Kyty::DevTools::WorkerSession session {};
	bool                         prepared = false;
};

RuntimeState& GetState() noexcept
{
	static RuntimeState state {};
	return state;
}

} // namespace

Kyty::DevTools::WorkerSessionResult PrepareFromBootstrap(
    const Kyty::DevTools::WorkerTelemetryOptions& options) noexcept
{
	auto& state = GetState();
	if (state.prepared)
	{
		return Kyty::DevTools::WorkerSessionResult::AlreadyActive;
	}

	const auto result = state.session.StartFromBootstrap(std::getenv(Kyty::DevTools::kBootstrapEnvName), options);
	if (result == Kyty::DevTools::WorkerSessionResult::MissingBootstrap ||
	    result == Kyty::DevTools::WorkerSessionResult::Attached)
	{
		state.prepared = true;
	}
	return result;
}

bool Initialize() noexcept
{
	return GetState().prepared;
}

bool Shutdown() noexcept
{
	auto& state = GetState();
	if (!state.prepared)
	{
		return true;
	}
	const bool result = state.session.Stop();
	state.prepared    = false;
	return result;
}

bool Active() noexcept
{
	return GetState().session.Active();
}

} // namespace Kyty::Emulator::DevTools
