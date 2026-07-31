#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("libkernel_write_throttling", 1, "libkernel", 1, 1);

namespace WriteThrottling {

static int KYTY_SYSV_ABI QueryDefaultPolicy(uint64_t* policy, const void* context)
{
	PRINT_NAME();
	if (policy == nullptr || context == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	*policy = 0;
	return OK;
}

} // namespace WriteThrottling

LIB_DEFINE(InitWriteThrottling_1)
{
	LIB_FUNC("YFC3dBBipj8", WriteThrottling::QueryDefaultPolicy);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
