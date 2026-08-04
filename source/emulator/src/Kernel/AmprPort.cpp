#include "Emulator/Kernel/AmprPort.h"

#include "Emulator/Kernel/Errors.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::AmprPort {
namespace {

int FallbackSubmitCommandBuffer(void* command_buffer, uintptr_t /*submit_ident*/) noexcept
{
	return command_buffer != nullptr ? OK : KERNEL_ERROR_EINVAL;
}

std::atomic<SubmitCommandBufferFunction> g_submit_command_buffer {FallbackSubmitCommandBuffer};

} // namespace

void Install(SubmitCommandBufferFunction provider) noexcept
{
	g_submit_command_buffer.store(provider != nullptr ? provider : FallbackSubmitCommandBuffer, std::memory_order_release);
}

int SubmitCommandBuffer(void* command_buffer, uintptr_t submit_ident) noexcept
{
	return g_submit_command_buffer.load(std::memory_order_acquire)(command_buffer, submit_ident);
}

} // namespace Kyty::Kernel::AmprPort

#endif // KYTY_EMU_ENABLED
