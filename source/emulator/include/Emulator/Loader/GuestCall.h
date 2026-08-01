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

// Call an x86-64 guest function with the first four SysV arguments. This is
// required by guest callbacks whose ABI extends beyond the three-argument
// HLE helper boundary.
uint64_t KYTY_SYSV_ABI Invoke4(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);

// Call an x86-64 guest function after switching to an explicit guest stack.
// The stack pointer must refer to writable guest memory owned by the current
// emulated thread.
uint64_t KYTY_SYSV_ABI InvokeOnStack(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* stack_top);

} // namespace Kyty::Loader::GuestCall

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_GUESTCALL_H_ */
