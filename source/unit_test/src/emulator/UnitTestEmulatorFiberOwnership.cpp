#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/FiberOwnership.h"
#include "Emulator/Log.h"

#include <atomic>
#include <future>
#include <thread>

UT_BEGIN(EmulatorFiberOwnership);

using namespace Kernel::Fiber;

namespace {

void EnsureLog()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

struct EntryState
{
	std::atomic_uint32_t entries {0};
	uint64_t             resumed_arg = 0;
	int32_t              return_result = FIBER_ERROR_INVALID;
};

KYTY_SYSV_ABI void CountEntry(uint64_t init, uint64_t /*run*/)
{
	auto& state = *reinterpret_cast<EntryState*>(init);
	state.entries.fetch_add(1);
	state.return_result = FiberReturnToThread(0x31, &state.resumed_arg);
	FiberReturnToThread(0x32, nullptr);
}

struct SwitchState
{
	FiberObject* target = nullptr;
	int32_t      result = FIBER_ERROR_INVALID;
};

KYTY_SYSV_ABI void ContendingSwitch(uint64_t init, uint64_t /*run*/)
{
	auto& state = *reinterpret_cast<SwitchState*>(init);
	state.result = FiberSwitch(state.target, 0, nullptr);
	FiberReturnToThread(0, nullptr);
}

struct RoundTripState
{
	FiberObject* first = nullptr;
	FiberObject* second = nullptr;
	uint32_t     first_observed_second = 0;
	uint32_t     second_observed_first = 0;
	int32_t      first_switch = FIBER_ERROR_INVALID;
	int32_t      second_switch = FIBER_ERROR_INVALID;
};

KYTY_SYSV_ABI void FirstEntry(uint64_t init, uint64_t /*run*/)
{
	auto& state = *reinterpret_cast<RoundTripState*>(init);
	state.first_switch = FiberSwitch(state.second, 0, nullptr);
	state.first_observed_second = FiberLoadState(state.second);
	FiberReturnToThread(0x41, nullptr);
}

KYTY_SYSV_ABI void SecondEntry(uint64_t init, uint64_t /*run*/)
{
	auto& state = *reinterpret_cast<RoundTripState*>(init);
	state.second_observed_first = FiberLoadState(state.first);
	state.second_switch = FiberSwitch(state.first, 0, nullptr);
	FiberReturnToThread(0x42, nullptr);
}

} // namespace

TEST(EmulatorFiberOwnership, ClaimedBeforeContextEntryCannotBeStolenByRunOrSwitch)
{
	EnsureLog();
	alignas(16) uint8_t target_stack[64 * 1024] {};
	alignas(16) uint8_t contender_stack[64 * 1024] {};
	FiberObject target {};
	FiberObject contender {};
	EntryState entry;
	SwitchState switch_state {&target};
	ASSERT_EQ(FiberInitialize(&target, "claimed", CountEntry, reinterpret_cast<uint64_t>(&entry), target_stack,
	                          sizeof(target_stack), nullptr, 0), Kernel::OK);
	ASSERT_EQ(FiberInitialize(&contender, "contender", ContendingSwitch, reinterpret_cast<uint64_t>(&switch_state), contender_stack,
	                          sizeof(contender_stack), nullptr, 0), Kernel::OK);

	std::promise<bool> claimed;
	std::promise<void> release_owner;
	auto claim = claimed.get_future();
	auto release = release_owner.get_future();
	std::thread owner([&] {
		// Pause at the exact shared Run CAS, before any context-entry work.
		claimed.set_value(FiberCompareExchangeState(&target, FIBER_STATE_IDLE, FIBER_STATE_RUNNING));
		release.wait();
		FiberStoreState(&target, FIBER_STATE_IDLE);
	});

	EXPECT_TRUE(claim.get());
	EXPECT_EQ(FiberRun(&target, 0, nullptr), FIBER_ERROR_STATE);
	EXPECT_EQ(FiberFinalize(&target), FIBER_ERROR_STATE);
	EXPECT_EQ(FiberRun(&contender, 0, nullptr), Kernel::OK);
	EXPECT_EQ(switch_state.result, FIBER_ERROR_STATE);
	EXPECT_EQ(entry.entries.load(), 0u);
	EXPECT_EQ(FiberLoadState(&target), FIBER_STATE_RUNNING);
	release_owner.set_value();
	owner.join();

	uint64_t returned = 0;
	EXPECT_EQ(FiberRun(&target, 0, &returned), Kernel::OK);
	EXPECT_EQ(returned, 0x31u);
	EXPECT_EQ(entry.entries.load(), 1u);
}

TEST(EmulatorFiberOwnership, ReleasedLeaseCannotClearTheNextClaim)
{
	FiberObject fiber {};
	FiberStoreState(&fiber, FIBER_STATE_IDLE);
	std::promise<void> released;
	std::promise<void> next_claimed;
	auto release = released.get_future();
	auto next = next_claimed.get_future();
	uint32_t saved_context_payload = 0;

	std::thread previous_owner([&] {
		EXPECT_TRUE(FiberCompareExchangeState(&fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING));
		saved_context_payload = 0x1234;
		FiberStoreState(&fiber, FIBER_STATE_IDLE);
		released.set_value();
		// The old owner is still alive after publication of IDLE. Its exit has
		// no late owner-map erasure capable of invalidating the next lease.
		next.wait();
	});

	release.wait();
	EXPECT_TRUE(FiberCompareExchangeState(&fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING));
	EXPECT_EQ(saved_context_payload, 0x1234u);
	next_claimed.set_value();
	previous_owner.join();
	EXPECT_EQ(FiberLoadState(&fiber), FIBER_STATE_RUNNING);
	EXPECT_FALSE(FiberCompareExchangeState(&fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING));
	FiberStoreState(&fiber, FIBER_STATE_IDLE);
}

TEST(EmulatorFiberOwnership, SwitchAndReturnPublishIdleAfterLeavingTheOldStack)
{
	EnsureLog();
	alignas(16) uint8_t first_stack[64 * 1024] {};
	alignas(16) uint8_t second_stack[64 * 1024] {};
	FiberObject first {};
	FiberObject second {};
	RoundTripState state {&first, &second};
	ASSERT_EQ(FiberInitialize(&first, "first", FirstEntry, reinterpret_cast<uint64_t>(&state), first_stack, sizeof(first_stack),
	                          nullptr, 0), Kernel::OK);
	ASSERT_EQ(FiberInitialize(&second, "second", SecondEntry, reinterpret_cast<uint64_t>(&state), second_stack, sizeof(second_stack),
	                          nullptr, 0), Kernel::OK);
	uint64_t returned = 0;
	EXPECT_EQ(FiberRun(&first, 0, &returned), Kernel::OK);
	EXPECT_EQ(returned, 0x41u);
	EXPECT_EQ(state.first_switch, Kernel::OK);
	EXPECT_EQ(state.second_observed_first, FIBER_STATE_IDLE);
	EXPECT_EQ(state.first_observed_second, FIBER_STATE_IDLE);
	EXPECT_EQ(FiberLoadState(&first), FIBER_STATE_IDLE);
	EXPECT_EQ(FiberRun(&second, 0, &returned), Kernel::OK);
	EXPECT_EQ(returned, 0x42u);
	EXPECT_EQ(state.second_switch, Kernel::OK);
	EXPECT_EQ(FiberLoadState(&second), FIBER_STATE_IDLE);
}

TEST(EmulatorFiberOwnership, SequentialCrossThreadResumeRetainsContext)
{
	EnsureLog();
	alignas(16) uint8_t stack[64 * 1024] {};
	FiberObject fiber {};
	EntryState state;
	ASSERT_EQ(FiberInitialize(&fiber, "handoff", CountEntry, reinterpret_cast<uint64_t>(&state), stack, sizeof(stack), nullptr, 0),
	          Kernel::OK);
	std::thread first_owner([&] { EXPECT_EQ(FiberRun(&fiber, 1, nullptr), Kernel::OK); });
	first_owner.join();
	ASSERT_EQ(FiberLoadState(&fiber), FIBER_STATE_IDLE);
	uint64_t returned = 0;
	EXPECT_EQ(FiberRun(&fiber, 0x52, &returned), Kernel::OK);
	EXPECT_EQ(returned, 0x32u);
	EXPECT_EQ(state.entries.load(), 1u);
	EXPECT_EQ(state.return_result, Kernel::OK);
	EXPECT_EQ(state.resumed_arg, 0x52u);
}

UT_END();
