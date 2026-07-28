#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNCONADDRESS_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNCONADDRESS_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::SyncOnAddress {

// Blocks while the aligned 32-bit value at address equals expected_value.
// A null timeout waits indefinitely; the currently supported flags value is 0.
int KYTY_SYSV_ABI KernelSyncOnAddressWait(uint64_t address, uint32_t expected_value, const uint32_t* timeout, uint32_t flags);
int KYTY_SYSV_ABI KernelSyncOnAddressWake(uint64_t address, int64_t wake_count);

} // namespace Kyty::Libs::LibKernel::SyncOnAddress

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNCONADDRESS_H_ */
