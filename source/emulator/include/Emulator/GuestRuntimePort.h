#ifndef EMULATOR_INCLUDE_EMULATOR_GUEST_RUNTIME_PORT_H_
#define EMULATOR_INCLUDE_EMULATOR_GUEST_RUNTIME_PORT_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::GuestRuntimePort {

// Loader-owned program handles are intentionally opaque to consumers. The
// loader may use any stable object identity; callers only retain it for
// lifecycle association and never inspect its representation.
using ProgramHandle = const void*;

using FindProgramByAddrFunction = ProgramHandle (*)(uint64_t vaddr);
using InvokeFunction            = uint64_t KYTY_SYSV_ABI (*)(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2);
using Invoke4Function           = uint64_t KYTY_SYSV_ABI (*)(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
using InvokeOnStackFunction = uint64_t KYTY_SYSV_ABI (*)(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* stack_top);
using ReleaseThreadDynamicTlsFunction = void (*)(int thread_id);

struct Provider
{
	FindProgramByAddrFunction find_program_by_addr = nullptr;
	InvokeFunction            invoke              = nullptr;
	Invoke4Function           invoke4             = nullptr;
	InvokeOnStackFunction     invoke_on_stack     = nullptr;
	ReleaseThreadDynamicTlsFunction release_thread_dynamic_tls = nullptr;
};

// RuntimeLinker installs the provider during construction. Empty callbacks
// restore safe fallbacks for focused tests and early bring-up.
void Install(const Provider& provider) noexcept;

[[nodiscard]] ProgramHandle FindProgramByAddr(uint64_t vaddr) noexcept;
[[nodiscard]] uint64_t      Invoke(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2) noexcept;
[[nodiscard]] uint64_t      Invoke4(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) noexcept;
[[nodiscard]] uint64_t      InvokeOnStack(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* stack_top) noexcept;
void                         ReleaseThreadDynamicTls(int thread_id) noexcept;

} // namespace Kyty::Emulator::GuestRuntimePort

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GUEST_RUNTIME_PORT_H_ */
