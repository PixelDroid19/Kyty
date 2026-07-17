#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("ContentExport", 1, "ContentExport", 1, 1);

namespace ContentExport {

constexpr int CONTENT_EXPORT_ERROR_INVALID_PARAM = -2137182186; /* 0x809D3016 */

// sceContentExportInitParam2 layout used by Gen5 share/capture paths.
struct ContentExportInitParam2
{
	void*   malloc_func;
	void*   free_func;
	void*   user_data;
	size_t  buffer_size;
	int64_t reserved0;
	int64_t reserved1;
};

static bool g_initialized = false;

// sceContentExportInit2 — NID 0GnN4QCgIfs
static int KYTY_SYSV_ABI ContentExportInit2(const ContentExportInitParam2* init_param)
{
	PRINT_NAME();
	printf("\t init_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));
	if (init_param == nullptr)
	{
		return CONTENT_EXPORT_ERROR_INVALID_PARAM;
	}
	printf("\t malloc_func = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param->malloc_func));
	printf("\t free_func   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param->free_func));
	printf("\t user_data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param->user_data));
	printf("\t buffer_size = 0x%016" PRIx64 "\n", static_cast<uint64_t>(init_param->buffer_size));
	g_initialized = true;
	return OK;
}

} // namespace ContentExport

LIB_DEFINE(InitContentExport_1)
{
	LIB_FUNC("0GnN4QCgIfs", ContentExport::ContentExportInit2);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
