#ifndef EMULATOR_INCLUDE_EMULATOR_DEVTOOLS_RUNTIME_H_
#define EMULATOR_INCLUDE_EMULATOR_DEVTOOLS_RUNTIME_H_

#include "Kyty/DevTools/Telemetry/WorkerTelemetry.h"
#include "Kyty/DevTools/Transport/WorkerSession.h"

namespace Kyty::Emulator::DevTools {

// Prepares the process-lifetime worker session before emulator subsystems are
// registered. Missing bootstrap is the explicit standalone result; every
// present-but-invalid bootstrap remains a hard initialization failure.
[[nodiscard]] Kyty::DevTools::WorkerSessionResult PrepareFromBootstrap(
    const Kyty::DevTools::WorkerTelemetryOptions& options) noexcept;

// Called by RuntimeDiagnosticsSubsystem after Core is initialized. The
// transport is already validated by PrepareFromBootstrap; this function keeps
// the subsystem boundary explicit without reopening or reparsing it.
[[nodiscard]] bool Initialize() noexcept;

// Idempotent owner-side shutdown. It is safe after standalone initialization,
// a failed preparation, or an earlier Destroy/UnexpectedShutdown call.
[[nodiscard]] bool Shutdown() noexcept;

[[nodiscard]] bool Active() noexcept;

} // namespace Kyty::Emulator::DevTools

#endif /* EMULATOR_INCLUDE_EMULATOR_DEVTOOLS_RUNTIME_H_ */
