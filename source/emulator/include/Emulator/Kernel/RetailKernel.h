#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_RETAILKERNEL_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_RETAILKERNEL_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel {

// Retail (non-devkit) sceKernelGetGPI contract: return 0 (ORBIS_OK) with no GPI
// state. Name↔NID 4oXYe9Xmk0Q from public PS5-3.20_Libs / sce_stubs. Pure helper
// for HLE and focused unit tests — does not invent SetGPI / debug GPI payloads.
[[nodiscard]] inline int KernelRetailGetGpiResult()
{
	return 0;
}

} // namespace Kyty::Libs::LibKernel

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_RETAILKERNEL_H_ */
