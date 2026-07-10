#include "Emulator/Libs/Errno.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Posix {

// NOTE: This is intentionally NOT a C++ thread_local. Under Rosetta 2 a
// thread_local accessed from an HLE function running on a guest thread faults:
// tlv_get_addr resolves thread storage through the gs segment, but the guest
// thread's gs is managed by Rosetta and does not point at the macOS TSD, so the
// access lands on a bogus low address. A single global errno is correct enough
// during early single-threaded init; per-thread errno needs a segment-free store.
static int g_errno = 0;

KYTY_SYSV_ABI int* GetErrorAddr()
{
	return &g_errno;
}

} // namespace Kyty::Libs::Posix

#endif
