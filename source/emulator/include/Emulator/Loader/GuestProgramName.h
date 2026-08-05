#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_GUESTPROGRAMNAME_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_GUESTPROGRAMNAME_H_

#include "Kyty/Core/String.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

// The guest-visible process name. The loader owns the storage because it is
// the layer that observes the loaded executable; the HLE layer publishes the
// same storage through the libkernel g_progname data export (see LibKernel.cpp)
// so the guest reads exactly what the loader wrote.
namespace Kyty::Loader {

// Address-stable storage exported to the guest as g_progname (const char**).
extern const char* g_progname;

void SetGuestProgramName(const Core::String& name);

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_GUESTPROGRAMNAME_H_ */
