#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Rudp", 1, "Rudp", 1, 1);

namespace Rudp {

using RudpEventHandler = void (*)(int ctx_id, int event_id, int error_code, void* arg);

static RudpEventHandler g_event_handler = nullptr;
static void*            g_event_arg     = nullptr;
static bool             g_initialized   = false;

static int KYTY_SYSV_ABI RudpInit(void* mem_pool, int mem_pool_size)
{
	PRINT_NAME();
	printf("\t mem_pool      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(mem_pool));
	printf("\t mem_pool_size = %d\n", mem_pool_size);
	g_initialized = true;
	return OK;
}

static int KYTY_SYSV_ABI RudpEnableInternalIOThread(uint32_t stack_size, uint32_t priority)
{
	PRINT_NAME();
	printf("\t stack_size = %" PRIu32 "\n", stack_size);
	printf("\t priority   = %" PRIu32 "\n", priority);
	return OK;
}

static int KYTY_SYSV_ABI RudpSetEventHandler(RudpEventHandler handler, void* arg)
{
	PRINT_NAME();
	printf("\t handler = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handler));
	printf("\t arg     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(arg));
	g_event_handler = handler;
	g_event_arg     = arg;
	return OK;
}

} // namespace Rudp

LIB_DEFINE(InitRudp_1)
{
	LIB_FUNC("amuBfI-AQc4", Rudp::RudpInit);
	LIB_FUNC("6PBNpsgyaxw", Rudp::RudpEnableInternalIOThread);
	LIB_FUNC("SUEVes8gvmw", Rudp::RudpSetEventHandler);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
