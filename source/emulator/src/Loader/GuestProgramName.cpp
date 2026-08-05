#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/GuestProgramName.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

namespace {

constexpr size_t kProgNameMaxSize = 511;

char         g_progname_buf[kProgNameMaxSize + 1] = {0};

} // namespace

// Exported to the guest through the libkernel g_progname data export; the
// address must remain stable for the lifetime of the emulator process.
const char* g_progname = g_progname_buf;

void SetGuestProgramName(const Core::String& name)
{
	EXIT_IF(g_progname != g_progname_buf);

	strncpy(g_progname_buf, name.C_Str(), kProgNameMaxSize);
	g_progname_buf[kProgNameMaxSize] = 0;
}

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED
