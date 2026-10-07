#include "Emulator/Common.h"
#include "Emulator/Kernel/SyncOnAddress.h"
#include "Emulator/Libs/Libs.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

// The libkernel module exports the address wait/wake pair from its own
// library: titles import them as [libkernel_sync_on_address_v1][libkernel_v1.1].
LIB_VERSION("libkernel_sync_on_address", 1, "libkernel", 1, 1);

LIB_DEFINE(InitSyncOnAddress_1)
{
	LIB_FUNC("Hc4CaR6JBL0", Kernel::SyncOnAddress::KernelSyncOnAddressWait);
	LIB_FUNC("q2y-wDIVWZA", Kernel::SyncOnAddress::KernelSyncOnAddressWake);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
