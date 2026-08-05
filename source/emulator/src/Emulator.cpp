#include "Emulator/Emulator.h"

#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"

namespace Kyty::Emulator {

// The Lua bridge lives in the fc_script CLI layer (KytyEmulator.cpp), which
// registers its kyty_* functions after the subsystem graph is initialized.

void EmulatorSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent) {}

void EmulatorSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void EmulatorSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

} // namespace Kyty::Emulator
