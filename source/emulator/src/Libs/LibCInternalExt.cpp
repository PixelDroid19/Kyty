#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "LibCInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

namespace LibcInternalExt {

LIB_VERSION("LibcInternalExt", 1, "LibcInternal", 1, 1);

static uint64_t g_mspace_atomic_id_mask = 0;
static uint64_t g_mstate_table[64]      = {0};

struct Info
{
	uint64_t  size;
	uint32_t  unknown1;
	uint32_t  unknown2;
	uint64_t* mspace_atomic_id_mask;
	uint64_t* mstate_table;
};

void KYTY_SYSV_ABI LibcHeapGetTraceInfo(Info* info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(info->size != 32);

	info->mspace_atomic_id_mask = &g_mspace_atomic_id_mask;
	info->mstate_table          = g_mstate_table;
}

LIB_DEFINE(InitLibcInternalExt_1)
{
	LIB_FUNC("NWtTN10cJzE", LibcInternalExt::LibcHeapGetTraceInfo);
	LIB_FUNC("qBS714-Jr3g", LibC::c_cxa_thread_atexit);
}

} // namespace LibcInternalExt

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
