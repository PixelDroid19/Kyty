#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Share", 1, "Share", 1, 1);

namespace Share {

// Process-local init flag for Share utility. Matches observed boot order where
// additional Share exports may run before or after sceShareInitialize.
static std::atomic_int g_share_initialized {0};

static KYTY_SYSV_ABI int ShareInitialize(size_t memory_size, int priority, uint64_t affinity_mask)
{
	PRINT_NAME();
	printf("\t memory_size         = 0x%016" PRIx64 "\n", static_cast<uint64_t>(memory_size));
	printf("\t priority            = %d\n", priority);
	printf("\t affinity_mask       = 0x%016" PRIx64 "\n", affinity_mask);
	if (memory_size == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	g_share_initialized.store(1, std::memory_order_relaxed);
	return OK;
}

// Behavioral contract (reimplemented from public PS5 export naming evidence;
// no third-party implementation code copied): content param is a guest UTF-8
// C-string. Store is host-side only; networking/share upload is not modeled.
static KYTY_SYSV_ABI int ShareSetContentParam(const char* content_param)
{
	PRINT_NAME();
	printf("\t content_param = %s\n", content_param != nullptr ? content_param : "(null)");
	if (content_param == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	if (g_share_initialized.load(std::memory_order_relaxed) == 0)
	{
		printf("\t note: set_content_param before initialize\n");
	}
	return OK;
}

// Share_v1 Func NIDs observed on a second private Gen5 title boot path, sitting
// next to nBDD66kiFW8 (ShareInitialize) in the eboot import table. Public NID
// databases and Share export lists consulted do not yet name these symbols.
// Log SysV argument registers and return SCE_OK so boot can reach the next
// evidenced fail. Refine ABI when a name/signature is triangulated.
static KYTY_SYSV_ABI int ShareLogAndOk(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	PRINT_NAME();
	printf("\t a0 = 0x%016" PRIx64 "\n", a0);
	printf("\t a1 = 0x%016" PRIx64 "\n", a1);
	printf("\t a2 = 0x%016" PRIx64 "\n", a2);
	printf("\t a3 = 0x%016" PRIx64 "\n", a3);
	return OK;
}

} // namespace Share

LIB_DEFINE(InitShare_1)
{
	LIB_FUNC("nBDD66kiFW8", Share::ShareInitialize);
	LIB_FUNC("7QZtURYnXG4", Share::ShareSetContentParam);
	// Contiguous Share_v1 Func NIDs from Gen5 eboot import table (order as
	// encoded next to ShareInitialize). Names unresolved; log-and-OK until
	// ABI is triangulated.
	LIB_FUNC("YBiIdcDPrxs", Share::ShareLogAndOk);
	LIB_FUNC("5wjxESwX68I", Share::ShareLogAndOk);
	LIB_FUNC("T64o-315wbg", Share::ShareLogAndOk);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
