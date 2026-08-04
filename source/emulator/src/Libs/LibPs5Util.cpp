#include "Emulator/Common.h"
#include "Emulator/Libs/KernelException.h"
#include "Emulator/Libs/Libs.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("PS5Util", 1, "PS5Util", 1, 1);

namespace Ps5Util {

// IL2CPP's conservative collector asks PS5Util to interrupt a peer thread
// before it waits for that thread's register-context acknowledgement. The
// accompanying PS5Util PRX installs the signal-30 handler and owns the
// suspend/resume semaphores; libkernel owns delivery to that handler.
static KYTY_SYSV_ABI int RequestThreadContext(void* thread)
{
	PRINT_NAME();
	printf("\t thread = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(thread));

	return LibKernel::KernelRaiseException(static_cast<LibKernel::Pthread>(thread), 30);
}

} // namespace Ps5Util

LIB_DEFINE(InitPs5Util_1)
{
	LIB_FUNC("J3edELK4FvM", Ps5Util::RequestThreadContext);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
