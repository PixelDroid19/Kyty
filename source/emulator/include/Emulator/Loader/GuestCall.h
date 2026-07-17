#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_GUESTCALL_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_GUESTCALL_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

#include <cstdint>

namespace Kyty::Loader::GuestCall {

// Call an x86-64 guest function with a synthetic zero-return RBP frame below
// the guest callee. arg0, arg1, and arg2 are passed as the guest's first three
// SysV arguments; unused arguments must be zero.
uint64_t KYTY_SYSV_ABI Invoke(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2);

} // namespace Kyty::Loader::GuestCall

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_GUESTCALL_H_ */
