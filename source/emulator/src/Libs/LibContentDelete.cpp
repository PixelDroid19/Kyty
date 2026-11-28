#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("ContentDelete", 1, "ContentDelete", 1, 1);

namespace ContentDelete {

// Initialize and Terminate bracket the content delete service, which is reached
// only through its delete calls. The codes for a repeated Initialize or a
// Terminate without Initialize are not published, so either one stops the run.
static std::atomic_bool g_initialized = false;

// The parameter is a 48-byte block; a caller fills only the word at +8.
static KYTY_SYSV_ABI int ContentDeleteInitialize(const uint64_t* param)
{
	PRINT_NAME();
	EXIT_IF(param == nullptr);
	KYTY_LOG_DEBUG("\t param[1] = 0x%016" PRIx64 "\n", param[1]);
	EXIT_IF(g_initialized.exchange(true));
	return OK;
}

static KYTY_SYSV_ABI int ContentDeleteTerminate()
{
	PRINT_NAME();
	EXIT_IF(!g_initialized.exchange(false));
	return OK;
}

} // namespace ContentDelete

LIB_DEFINE(InitContentDelete_1)
{
	LIB_FUNC("zoxb0wEChEM", ContentDelete::ContentDeleteInitialize);
	LIB_FUNC("5XLSih32qHA", ContentDelete::ContentDeleteTerminate);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
