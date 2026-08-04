#include "Emulator/GuestRuntimePort.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::GuestRuntimePort {
namespace {

ProgramHandle FallbackFindProgramByAddr(uint64_t /*vaddr*/)
{
	return nullptr;
}

uint64_t KYTY_SYSV_ABI FallbackInvoke(uint64_t /*target*/, uint64_t /*arg0*/, uint64_t /*arg1*/, uint64_t /*arg2*/)
{
	return 0;
}

uint64_t KYTY_SYSV_ABI FallbackInvoke4(uint64_t /*target*/, uint64_t /*arg0*/, uint64_t /*arg1*/, uint64_t /*arg2*/, uint64_t /*arg3*/)
{
	return 0;
}

uint64_t KYTY_SYSV_ABI FallbackInvokeOnStack(uint64_t /*target*/, uint64_t /*arg0*/, uint64_t /*arg1*/, uint64_t /*arg2*/, void* /*stack_top*/)
{
	return 0;
}

std::atomic<FindProgramByAddrFunction> g_find_program_by_addr {FallbackFindProgramByAddr};
std::atomic<InvokeFunction>            g_invoke {FallbackInvoke};
std::atomic<Invoke4Function>           g_invoke4 {FallbackInvoke4};
std::atomic<InvokeOnStackFunction>     g_invoke_on_stack {FallbackInvokeOnStack};

} // namespace

void Install(const Provider& provider) noexcept
{
	g_find_program_by_addr.store(provider.find_program_by_addr != nullptr ? provider.find_program_by_addr : FallbackFindProgramByAddr,
	                             std::memory_order_release);
	g_invoke.store(provider.invoke != nullptr ? provider.invoke : FallbackInvoke, std::memory_order_release);
	g_invoke4.store(provider.invoke4 != nullptr ? provider.invoke4 : FallbackInvoke4, std::memory_order_release);
	g_invoke_on_stack.store(provider.invoke_on_stack != nullptr ? provider.invoke_on_stack : FallbackInvokeOnStack,
	                        std::memory_order_release);
}

ProgramHandle FindProgramByAddr(uint64_t vaddr) noexcept
{
	return g_find_program_by_addr.load(std::memory_order_acquire)(vaddr);
}

uint64_t Invoke(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2) noexcept
{
	return g_invoke.load(std::memory_order_acquire)(target, arg0, arg1, arg2);
}

uint64_t Invoke4(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) noexcept
{
	return g_invoke4.load(std::memory_order_acquire)(target, arg0, arg1, arg2, arg3);
}

uint64_t InvokeOnStack(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* stack_top) noexcept
{
	return g_invoke_on_stack.load(std::memory_order_acquire)(target, arg0, arg1, arg2, stack_top);
}

} // namespace Kyty::Emulator::GuestRuntimePort

#endif // KYTY_EMU_ENABLED
