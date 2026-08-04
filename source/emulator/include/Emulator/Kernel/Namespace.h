#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_NAMESPACE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_NAMESPACE_H_

// Kernel implementations live in the neutral Kernel domain. The LibKernel
// namespace remains a compatibility view for guest-facing HLE registration
// until each registration unit is moved behind its own boundary.
namespace Kyty::Kernel {
}

namespace Kyty::Libs::LibKernel {
using namespace ::Kyty::Kernel;
}

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_NAMESPACE_H_ */
