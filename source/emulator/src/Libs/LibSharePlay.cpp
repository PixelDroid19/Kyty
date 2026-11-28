#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("SharePlay", 1, "SharePlay", 1, 1);

namespace SharePlay {

// Initialize and Terminate bracket the Share Play service; the session calls
// that would read its state are not provided, so no state is kept beyond the
// bracket. The codes for a repeated Initialize or a Terminate without
// Initialize are not established, so either one stops the run.
static std::atomic_bool g_initialized = false;

// The heap is the caller's working memory for the service.
static KYTY_SYSV_ABI int SharePlayInitialize(void* heap, size_t heap_size)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t heap = 0x%016" PRIx64 " heap_size = 0x%" PRIx64 "\n", reinterpret_cast<uint64_t>(heap),
	               static_cast<uint64_t>(heap_size));
	EXIT_IF(heap == nullptr);
	EXIT_IF(g_initialized.exchange(true));
	return OK;
}

static KYTY_SYSV_ABI int SharePlayTerminate()
{
	PRINT_NAME();
	EXIT_IF(!g_initialized.exchange(false));
	return OK;
}

} // namespace SharePlay

LIB_DEFINE(InitSharePlay_1)
{
	LIB_FUNC("isruqthpYcw", SharePlay::SharePlayInitialize);
	LIB_FUNC("UaLjloJinow", SharePlay::SharePlayTerminate);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
