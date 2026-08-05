#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <atomic>
#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Coredump", 1, "libkernel", 1, 1);

namespace Coredump {

static std::atomic<uint64_t> g_handler {0};
static std::atomic<uint64_t> g_handler_context {0};

static int KYTY_SYSV_ABI CoredumpRegisterHandler(uint64_t handler, size_t stack_size, uint64_t context)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handler = 0x%016" PRIx64 " stack = 0x%016" PRIx64 " context = 0x%016" PRIx64 "\n", handler,
	       static_cast<uint64_t>(stack_size), context);
	g_handler.store(handler, std::memory_order_release);
	g_handler_context.store(context, std::memory_order_release);
	return OK;
}

static int KYTY_SYSV_ABI CoredumpUnregisterHandler()
{
	PRINT_NAME();
	g_handler.store(0, std::memory_order_release);
	g_handler_context.store(0, std::memory_order_release);
	return OK;
}

} // namespace Coredump

LIB_DEFINE(InitCoredump_1)
{
	LIB_FUNC("8zLSfEfW5AU", Coredump::CoredumpRegisterHandler);
	LIB_FUNC("fFkhOgztiCA", Coredump::CoredumpUnregisterHandler);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
