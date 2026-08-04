#include "Emulator/Common.h"
#include "Emulator/Kernel/Fiber.h"
#include "Emulator/Libs/Libs.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Fiber", 1, "Fiber", 1, 1);

LIB_DEFINE(InitFiber_1)
{
	LIB_FUNC("hVYD7Ou2pCQ", ::Kyty::Kernel::Fiber::FiberInitialize);
	LIB_FUNC("7+OJIpko9RY", ::Kyty::Kernel::Fiber::FiberInitializeInternal);
	LIB_FUNC("asjUJJ+aa8s", ::Kyty::Kernel::Fiber::FiberOptParamInitialize);
	LIB_FUNC("JeNX5F-NzQU", ::Kyty::Kernel::Fiber::FiberFinalize);
	LIB_FUNC("a0LLrZWac0M", ::Kyty::Kernel::Fiber::FiberRun);
	LIB_FUNC("PFT2S-tJ7Uk", ::Kyty::Kernel::Fiber::FiberSwitch);
	LIB_FUNC("p+zLIOg27zU", ::Kyty::Kernel::Fiber::FiberGetSelf);
	LIB_FUNC("B0ZX2hx9DMw", ::Kyty::Kernel::Fiber::FiberReturnToThread);
	// AttachContext aliases used by some titles map to Run/Switch.
	LIB_FUNC("avfGJ94g36Q", ::Kyty::Kernel::Fiber::FiberRun);
	LIB_FUNC("ZqhZFuzKT6U", ::Kyty::Kernel::Fiber::FiberSwitch);
	LIB_FUNC("uq2Y5BFz0PE", ::Kyty::Kernel::Fiber::FiberGetInfo);
	LIB_FUNC("Lcqty+QNWFc", ::Kyty::Kernel::Fiber::FiberStartContextSizeCheck);
	LIB_FUNC("Kj4nXMpnM8Y", ::Kyty::Kernel::Fiber::FiberStopContextSizeCheck);
	LIB_FUNC("JzyT91ucGDc", ::Kyty::Kernel::Fiber::FiberRename);
	LIB_FUNC("0dy4JtMUcMQ", ::Kyty::Kernel::Fiber::FiberGetThreadFramePointerAddress);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
