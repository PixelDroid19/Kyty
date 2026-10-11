#include "Emulator/Libs/Errno.h"

#include "Emulator/Kernel/Pthread.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Posix {

KYTY_SYSV_ABI int* GetErrorAddr()
{
	return Kyty::Kernel::GetThreadErrorAddr();
}

} // namespace Kyty::Libs::Posix

#endif
