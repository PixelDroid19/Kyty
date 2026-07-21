#include "Emulator/Kernel/SyncOnAddress.h"

#include "Kyty/Core/Threads.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <chrono>
#include <climits>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::SyncOnAddress {

LIB_NAME("libkernel", "libkernel");

namespace {

constexpr auto kWaitSelfHealTimeout = std::chrono::milliseconds(100);

struct AddressWaitState
{
	std::mutex              mutex;
	std::condition_variable cv;
	int64_t                 generation = 0;
};

std::mutex g_address_map_mutex;
std::unordered_map<uint64_t, std::shared_ptr<AddressWaitState>> g_address_states;

std::shared_ptr<AddressWaitState> GetOrCreateState(uint64_t address)
{
	std::lock_guard lock(g_address_map_mutex);
	auto&           slot = g_address_states[address];
	if (!slot)
	{
		slot = std::make_shared<AddressWaitState>();
	}
	return slot;
}

} // namespace

int KYTY_SYSV_ABI KernelSyncOnAddressWait(uint64_t address)
{
	PRINT_NAME();

	if (address == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	printf("\t address = 0x%016" PRIx64 "\n", address);

	auto state = GetOrCreateState(address);

	int64_t observed_generation = 0;
	{
		std::lock_guard lock(state->mutex);
		observed_generation = state->generation;
	}

	std::unique_lock lock(state->mutex);
	state->cv.wait_for(lock, kWaitSelfHealTimeout,
	                   [&]() { return state->generation != observed_generation; });

	return OK;
}

int KYTY_SYSV_ABI KernelSyncOnAddressWake(uint64_t address, int64_t wake_count)
{
	PRINT_NAME();

	if (address == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	printf("\t address = 0x%016" PRIx64 " wake_count = %" PRId64 "\n", address, wake_count);

	auto state = GetOrCreateState(address);
	{
		std::lock_guard lock(state->mutex);
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

} // namespace Kyty::Libs::LibKernel::SyncOnAddress

#endif // KYTY_EMU_ENABLED
