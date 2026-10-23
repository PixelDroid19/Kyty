#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNCONADDRESS_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNCONADDRESS_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::SyncOnAddress {

// Blocks while the aligned 32-bit value at address equals expected_value.
// A null timeout waits indefinitely; the currently supported flags value is 0.
int KYTY_SYSV_ABI KernelSyncOnAddressWait(uint64_t address, uint32_t expected_value, const uint32_t* timeout, uint32_t flags);
int KYTY_SYSV_ABI KernelSyncOnAddressWake(uint64_t address, int64_t wake_count);

// _umtx_op: UMTX_OP_WAIT blocks while the 64-bit value at object equals value,
// with an optional relative timespec at timeout (timeout_size up to 16 bytes);
// UMTX_OP_WAKE releases up to value waiters. Returns OK or a kernel error.
int KernelUmtxOp(void* object, int operation, uint64_t value, uint64_t timeout_size, const void* timeout);

} // namespace Kyty::Kernel::SyncOnAddress

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNCONADDRESS_H_ */
