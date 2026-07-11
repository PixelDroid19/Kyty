#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Share", 1, "Share", 1, 1);

namespace Share {

static KYTY_SYSV_ABI int ShareInitialize(size_t memory_size, int priority, uint64_t affinity_mask)
{
	PRINT_NAME();
	printf("\t memory_size         = 0x%016" PRIx64 "\n", static_cast<uint64_t>(memory_size));
	printf("\t priority            = %d\n", priority);
	printf("\t affinity_mask       = 0x%016" PRIx64 "\n", affinity_mask);
	return (memory_size != 0 ? OK : LibKernel::KERNEL_ERROR_EINVAL);
}

} // namespace Share

LIB_DEFINE(InitShare_1)
{
	LIB_FUNC("nBDD66kiFW8", Share::ShareInitialize);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
