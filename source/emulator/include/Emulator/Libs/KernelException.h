#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_KERNELEXCEPTION_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_KERNELEXCEPTION_H_

#include "Emulator/Kernel/Pthread.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel {

// Delivers the platform's collector/context-capture exception to a guest
// pthread.  PS5Util uses signal 30 for its thread-context handshake.
int KYTY_SYSV_ABI KernelRaiseException(::Kyty::Kernel::Pthread thread, int signum);

} // namespace Kyty::Libs::LibKernel

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_KERNELEXCEPTION_H_
