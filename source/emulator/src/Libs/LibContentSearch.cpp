#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("ContentSearch", 1, "ContentSearch", 1, 0);

namespace ContentSearch {

// Init and Term bracket the content search service, which is reached only
// through its query calls. The codes for a repeated Init or a Term without Init
// are not published, so either one stops the run.
static std::atomic_bool g_initialized = false;

// The parameter is one 8-byte field, the size a caller fills on the stack.
static KYTY_SYSV_ABI int ContentSearchInit(const uint64_t* param)
{
	PRINT_NAME();
	EXIT_IF(param == nullptr);
	KYTY_LOG_DEBUG("\t param = 0x%016" PRIx64 "\n", *param);
	EXIT_IF(g_initialized.exchange(true));
	return OK;
}

static KYTY_SYSV_ABI int ContentSearchTerm()
{
	PRINT_NAME();
	EXIT_IF(!g_initialized.exchange(false));
	return OK;
}

} // namespace ContentSearch

LIB_DEFINE(InitContentSearch_1)
{
	LIB_FUNC("dPj4ZtRcIWk", ContentSearch::ContentSearchInit);
	LIB_FUNC("1xSZodB2geA", ContentSearch::ContentSearchTerm);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
