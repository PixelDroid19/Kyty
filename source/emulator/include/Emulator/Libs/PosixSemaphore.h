#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_POSIX_SEMAPHORE_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_POSIX_SEMAPHORE_H_

#include "Emulator/Common.h"
#include "Emulator/Kernel/Time.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Posix {

// Gen5 Posix_v1 semaphore exports registered by libkernel.
int KYTY_SYSV_ABI sem_init(void* sem, int pshared, unsigned int value);
int KYTY_SYSV_ABI sem_destroy(void* sem);
int KYTY_SYSV_ABI sem_wait(void* sem);
int KYTY_SYSV_ABI sem_trywait(void* sem);
int KYTY_SYSV_ABI sem_timedwait(void* sem, const ::Kyty::Kernel::KernelTimespec* abstime);
int KYTY_SYSV_ABI sem_reltimedwait_np(void* sem, const ::Kyty::Kernel::KernelTimespec* reltime);
int KYTY_SYSV_ABI sem_post(void* sem);
int KYTY_SYSV_ABI sem_getvalue(void* sem, int* sval);

} // namespace Kyty::Libs::Posix

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_POSIX_SEMAPHORE_H_ */
