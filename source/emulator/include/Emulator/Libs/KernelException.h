#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_KERNELEXCEPTION_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_KERNELEXCEPTION_H_

#include "Emulator/Kernel/Pthread.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel {

// Delivers the platform's collector/context-capture exception to a guest
// pthread.  PS5Util uses signal 30 for its thread-context handshake.
int KYTY_SYSV_ABI KernelRaiseException(::Kyty::Kernel::Pthread thread, int signum);

// Number of context requests not yet reclaimed. A request is reclaimed by a later
// raise once its trampoline has published completion; read-only, for diagnostics
// and tests. Hosts without the context bridge report zero.
size_t KernelHostContextRequestsPending();

} // namespace Kyty::Libs::LibKernel

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_KERNELEXCEPTION_H_
