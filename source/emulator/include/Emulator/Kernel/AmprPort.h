#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_AMPR_PORT_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_AMPR_PORT_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::AmprPort {

using SubmitCommandBufferFunction = int (*)(void* command_buffer, uintptr_t submit_ident);

// The HLE Ampr implementation installs its completion provider when the
// library is registered. Kernel APR calls use a validation-only fallback until
// then so the kernel layer does not import the HLE domain.
void Install(SubmitCommandBufferFunction provider) noexcept;

[[nodiscard]] int SubmitCommandBuffer(void* command_buffer, uintptr_t submit_ident) noexcept;

} // namespace Kyty::Kernel::AmprPort

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_AMPR_PORT_H_ */
