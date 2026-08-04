#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_ERRORS_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_ERRORS_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel {

// Kernel-domain results are independent from the guest-facing POSIX/error
// translation table. Libs adapters translate these values at their boundary.
constexpr int OK                      = 0;
constexpr int KERNEL_ERROR_EPERM      = -2147352575;
constexpr int KERNEL_ERROR_ENOENT     = -2147352574;
constexpr int KERNEL_ERROR_ESRCH      = -2147352573;
constexpr int KERNEL_ERROR_EIO        = -2147352571;
constexpr int KERNEL_ERROR_EBADF      = -2147352567;
constexpr int KERNEL_ERROR_EDEADLK    = -2147352565;
constexpr int KERNEL_ERROR_ENOMEM     = -2147352564;
constexpr int KERNEL_ERROR_EACCES     = -2147352563;
constexpr int KERNEL_ERROR_EFAULT     = -2147352562;
constexpr int KERNEL_ERROR_EBUSY      = -2147352560;
constexpr int KERNEL_ERROR_EEXIST     = -2147352559;
constexpr int KERNEL_ERROR_ENOTDIR    = -2147352556;
constexpr int KERNEL_ERROR_EISDIR     = -2147352555;
constexpr int KERNEL_ERROR_EINVAL     = -2147352554;
constexpr int KERNEL_ERROR_EAGAIN     = -2147352541;
constexpr int KERNEL_ERROR_EOPNOTSUPP = -2147352531;
constexpr int KERNEL_ERROR_ETIMEDOUT  = -2147352516;
constexpr int KERNEL_ERROR_ENAMETOOLONG = -2147352513;
constexpr int KERNEL_ERROR_ECANCELED  = -2147352491;

} // namespace Kyty::Kernel

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_ERRORS_H_ */
