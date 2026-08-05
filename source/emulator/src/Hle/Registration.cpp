#include "Emulator/Hle/Registration.h"

#include "Emulator/Kernel/TimePort.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Hle {

Core::Time GetTraceTime()
{
	return Kernel::TimePort::GetTime();
}

} // namespace Kyty::Hle

#endif // KYTY_EMU_ENABLED
