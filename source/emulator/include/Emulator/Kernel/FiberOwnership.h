#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_FIBEROWNERSHIP_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_FIBEROWNERSHIP_H_

#include "Emulator/Kernel/Fiber.h"

#include <atomic>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::Fiber {

// The guest state word is the execution lease. A successful IDLE -> RUNNING
// CAS owns the stack, including the interval before the context is entered.
// No auxiliary host metadata can revoke that lease. Release to IDLE only
// after execution has left the stack (FiberCommitDeferredIdle).
inline uint32_t FiberLoadState(const FiberObject* fiber)
{
	return reinterpret_cast<const std::atomic<uint32_t>*>(&fiber->state)->load(std::memory_order_acquire);
}

inline void FiberStoreState(FiberObject* fiber, uint32_t state)
{
	reinterpret_cast<std::atomic<uint32_t>*>(&fiber->state)->store(state, std::memory_order_release);
}

inline bool FiberCompareExchangeState(FiberObject* fiber, uint32_t expected, uint32_t desired,
                                     uint32_t* observed_state = nullptr)
{
	const bool claimed = reinterpret_cast<std::atomic<uint32_t>*>(&fiber->state)->compare_exchange_strong(
	    expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
	if (observed_state != nullptr)
	{
		*observed_state = expected;
	}
	return claimed;
}

} // namespace Kyty::Kernel::Fiber

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_FIBEROWNERSHIP_H_ */
