#include "Emulator/Emulator.h"

#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"

namespace Kyty::Emulator {

// The Lua bridge lives in the fc_script CLI layer (KytyEmulator.cpp), which
// registers its kyty_* functions after the subsystem graph is initialized.

KYTY_SUBSYSTEM_INIT(Emulator) {}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Emulator) {}

KYTY_SUBSYSTEM_DESTROY(Emulator) {}

} // namespace Kyty::Emulator
