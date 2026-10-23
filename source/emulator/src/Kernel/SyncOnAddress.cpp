#include "Emulator/Kernel/SyncOnAddress.h"

#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Time.h"
#include "Emulator/Kernel/Trace.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::SyncOnAddress {

KERNEL_LIB_NAME();

namespace {

constexpr uint32_t kSupportedWaitFlags = 0u;
constexpr int      kUmtxOpWait           = 2;
constexpr int      kUmtxOpWake           = 3;
constexpr int64_t  kNanosecondsPerSecond = 1000000000;

struct AddressWaitState
{
	std::mutex              mutex;
	std::condition_variable cv;
	uint64_t                generation = 0;
	uint32_t                waiter_count = 0;
};

std::mutex g_address_map_mutex;
std::unordered_map<uint64_t, std::shared_ptr<AddressWaitState>> g_address_states;

struct WaitRegistration
{
	std::shared_ptr<AddressWaitState> state;
	uint64_t                          generation = 0;
};

WaitRegistration RegisterWaiter(uint64_t address)
{
	std::unique_lock map_lock(g_address_map_mutex);
	auto&            slot = g_address_states[address];
	if (slot == nullptr)
	{
		slot = std::make_shared<AddressWaitState>();
	}

	std::lock_guard state_lock(slot->mutex);
	slot->waiter_count++;
	return {slot, slot->generation};
}

void UnregisterWaiter(uint64_t address, const std::shared_ptr<AddressWaitState>& state)
{
	std::unique_lock map_lock(g_address_map_mutex);
	const auto        it = g_address_states.find(address);
	if (it == g_address_states.end() || it->second != state)
	{
		return;
	}

	std::lock_guard state_lock(state->mutex);
	EXIT_IF(state->waiter_count == 0);
	state->waiter_count--;
	if (state->waiter_count == 0)
	{
		g_address_states.erase(it);
	}
}

int WakeRegisteredWaiters(uint64_t address, int64_t wake_count)
{
	std::shared_ptr<AddressWaitState> state;
	{
		std::unique_lock map_lock(g_address_map_mutex);
		const auto        it = g_address_states.find(address);
		if (it == g_address_states.end())
		{
			return OK;
		}

		state = it->second;
		std::lock_guard state_lock(state->mutex);
		state->generation++;
	}

	const int releases =
	    (wake_count > 0 && wake_count < INT64_MAX) ? static_cast<int>(std::min<int64_t>(wake_count, INT32_MAX)) : INT32_MAX;
	if (releases <= 1)
	{
		state->cv.notify_one();
	} else if (releases >= INT32_MAX)
	{
		state->cv.notify_all();
	} else
	{
		for (int i = 0; i < releases; i++)
		{
			state->cv.notify_one();
		}
	}

	return OK;
}

// Blocks while the value at address equals expected, until a wake, a change
// or the relative timeout (null waits indefinitely).
template <typename T>
int WaitWhileEqual(uint64_t address, T expected, const std::chrono::nanoseconds* timeout)
{
	const auto registration = RegisterWaiter(address);
	int        result       = OK;
	{
		std::unique_lock          state_lock(registration.state->mutex);
		PthreadInterruptibleScope wait_scope(&registration.state->cv, sizeof(registration.state->cv));
		auto*                     value      = reinterpret_cast<const T*>(address);
		const auto       load_value = [&]() { return __atomic_load_n(value, __ATOMIC_ACQUIRE); };
		if (load_value() == expected)
		{
			const auto changed = [&]() { return registration.state->generation != registration.generation || load_value() != expected; };
			if (timeout == nullptr)
			{
				registration.state->cv.wait(state_lock, changed);
			} else
			{
				const auto deadline = std::chrono::steady_clock::now() + *timeout;
				if (!registration.state->cv.wait_until(state_lock, deadline, changed) && load_value() == expected)
				{
					result = KERNEL_ERROR_ETIMEDOUT;
				}
			}
		}
	}

	UnregisterWaiter(address, registration.state);
	return result;
}

int UmtxWait(uint64_t address, uint64_t expected, uint64_t timeout_size, const void* timeout)
{
	if (timeout == nullptr)
	{
		return WaitWhileEqual(address, expected, nullptr);
	}
	// A size above a timespec selects struct _umtx_time (absolute or clock-selected
	// timeouts); no title has used it yet.
	EXIT_IF(timeout_size > sizeof(KernelTimespec));
	const auto* relative = static_cast<const KernelTimespec*>(timeout);
	if (relative->tv_sec < 0 || relative->tv_nsec < 0 || relative->tv_nsec >= kNanosecondsPerSecond)
	{
		return KERNEL_ERROR_EINVAL;
	}
	const auto timeout_ns = std::chrono::seconds(relative->tv_sec) + std::chrono::nanoseconds(relative->tv_nsec);
	return WaitWhileEqual(address, expected, &timeout_ns);
}

} // namespace

int KYTY_SYSV_ABI KernelSyncOnAddressWait(uint64_t address, uint32_t expected_value, const uint32_t* timeout, uint32_t flags)
{
	PRINT_NAME();

	if (address == 0 || (address % alignof(uint32_t)) != 0 || flags != kSupportedWaitFlags ||
	    (timeout != nullptr && (reinterpret_cast<uintptr_t>(timeout) % alignof(uint32_t)) != 0))
	{
		return KERNEL_ERROR_EINVAL;
	}

	KYTY_LOG_DEBUG("\t address = 0x%016" PRIx64 " expected = 0x%08" PRIx32 " timeout = %p flags = 0x%08" PRIx32 "\n", address,
	               expected_value, static_cast<const void*>(timeout), flags);

	if (timeout == nullptr)
	{
		return WaitWhileEqual(address, expected_value, nullptr);
	}
	const std::chrono::nanoseconds timeout_ns = std::chrono::microseconds(__atomic_load_n(timeout, __ATOMIC_ACQUIRE));
	return WaitWhileEqual(address, expected_value, &timeout_ns);
}

int KYTY_SYSV_ABI KernelSyncOnAddressWake(uint64_t address, int64_t wake_count)
{
	PRINT_NAME();

	if (address == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KYTY_LOG_DEBUG("\t address = 0x%016" PRIx64 " wake_count = %" PRId64 "\n", address, wake_count);
	return WakeRegisteredWaiters(address, wake_count);
}

int KernelUmtxOp(void* object, int operation, uint64_t value, uint64_t timeout_size, const void* timeout)
{
	PRINT_NAME();

	const auto address = reinterpret_cast<uint64_t>(object);
	KYTY_LOG_DEBUG("\t object = 0x%016" PRIx64 " op = %d value = 0x%016" PRIx64 " timeout_size = %" PRIu64 " timeout = %p\n", address,
	               operation, value, timeout_size, timeout);
	if (address == 0)
	{
		return KERNEL_ERROR_EFAULT;
	}
	switch (operation)
	{
		case kUmtxOpWait: return UmtxWait(address, value, timeout_size, timeout);
		// The count is an int; a wake releases at least one waiter.
		case kUmtxOpWake: return WakeRegisteredWaiters(address, std::max<int64_t>(1, static_cast<int32_t>(value)));
		default: EXIT("unimplemented _umtx_op operation %d\n", operation);
	}
	return KERNEL_ERROR_EINVAL;
}

} // namespace Kyty::Kernel::SyncOnAddress

#endif // KYTY_EMU_ENABLED
