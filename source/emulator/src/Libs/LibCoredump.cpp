#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Coredump", 1, "Coredump", 1, 1);

namespace Coredump {

static void* g_handler         = nullptr;
static void* g_handler_context = nullptr;

static int KYTY_SYSV_ABI CoredumpRegisterHandler(void* handler, void* context)
{
	PRINT_NAME();
	printf("\t handler = 0x%016" PRIx64 " context = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handler),
	       reinterpret_cast<uint64_t>(context));
	g_handler         = handler;
	g_handler_context = context;
	return OK;
}

} // namespace Coredump

LIB_DEFINE(InitCoredump_1)
{
	LIB_FUNC("8zLSfEfW5AU", Coredump::CoredumpRegisterHandler);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
