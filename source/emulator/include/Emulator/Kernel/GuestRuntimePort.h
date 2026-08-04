#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_GUEST_RUNTIME_PORT_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_GUEST_RUNTIME_PORT_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::GuestRuntimePort {

// Loader-owned program handles are intentionally opaque to Kernel. The
// loader may use any stable object identity; Kernel only associates it with a
// statically initialized pthread object until the loader asks for cleanup.
using ProgramHandle = const void*;

using FindProgramByAddrFunction = ProgramHandle (*)(uint64_t vaddr);
using InvokeFunction            = uint64_t KYTY_SYSV_ABI (*)(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2);
using InvokeOnStackFunction = uint64_t KYTY_SYSV_ABI (*)(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* stack_top);

struct Provider
{
	FindProgramByAddrFunction find_program_by_addr = nullptr;
	InvokeFunction            invoke              = nullptr;
	InvokeOnStackFunction     invoke_on_stack      = nullptr;
};

// RuntimeLinker installs the provider during construction. Empty callbacks
// restore safe fallbacks for focused tests and early bring-up.
void Install(const Provider& provider) noexcept;

[[nodiscard]] ProgramHandle FindProgramByAddr(uint64_t vaddr) noexcept;
[[nodiscard]] uint64_t      Invoke(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2) noexcept;
[[nodiscard]] uint64_t      InvokeOnStack(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* stack_top) noexcept;

} // namespace Kyty::Kernel::GuestRuntimePort

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_GUEST_RUNTIME_PORT_H_ */
