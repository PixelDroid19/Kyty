#include "Kyty/Core/Threads.h"
#include "Kyty/UnitTest.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

UT_BEGIN(CoreThreadLifetime);

namespace {

constexpr auto kThreadWaitTimeout = std::chrono::seconds(2);
constexpr auto kCallbackHoldTimeout = std::chrono::seconds(5);

struct CallbackState
{
	std::atomic_bool entered {false};
	std::atomic_bool release {false};
	std::atomic_bool returned {false};
	std::atomic<int> callback_unique_id {0};
};

using CallbackStateRef = std::shared_ptr<CallbackState>;

CallbackStateRef TakeCallbackState(void* arg)
{
	std::unique_ptr<CallbackStateRef> state(static_cast<CallbackStateRef*>(arg));
	return std::move(*state);
}

void* MakeCallbackContext(const CallbackStateRef& state)
{
	return new CallbackStateRef(state);
}

bool WaitFor(const std::atomic_bool& value, std::chrono::steady_clock::duration timeout = kThreadWaitTimeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!value.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	return value.load(std::memory_order_acquire);
}

void BlockUntilReleased(void* arg)
{
	auto state = TakeCallbackState(arg);
	state->entered.store(true, std::memory_order_release);

	const auto deadline = std::chrono::steady_clock::now() + kCallbackHoldTimeout;
	while (!state->release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}

	state->returned.store(true, std::memory_order_release);
}

void ReturnImmediately(void* arg)
{
	auto state = TakeCallbackState(arg);
	state->callback_unique_id.store(Core::Thread::GetThreadIdUnique(), std::memory_order_release);
	state->returned.store(true, std::memory_order_release);
}

} // namespace

TEST(CoreThreadLifetime, DetachedCallbackCanReturnAfterWrapperDestruction)
{
	auto state = std::make_shared<CallbackState>();
	auto thread = std::make_unique<Core::Thread>(BlockUntilReleased, MakeCallbackContext(state));
	thread->Detach();

	const bool callback_entered = WaitFor(state->entered);
	std::atomic_bool wrapper_destroyed {false};
	std::thread destroyer([owned_thread = std::move(thread), &wrapper_destroyed]() mutable {
		owned_thread.reset();
		wrapper_destroyed.store(true, std::memory_order_release);
	});

	const bool destroyed_while_callback_blocked = WaitFor(wrapper_destroyed, std::chrono::milliseconds(250));
	state->release.store(true, std::memory_order_release);
	destroyer.join();
	const bool callback_returned = WaitFor(state->returned);

	EXPECT_TRUE(callback_entered);
	EXPECT_TRUE(destroyed_while_callback_blocked);
	EXPECT_TRUE(callback_returned);
}

TEST(CoreThreadLifetime, DetachedCallbackCanReturnBeforeWrapperDestruction)
{
	auto state = std::make_shared<CallbackState>();
	auto thread = std::make_unique<Core::Thread>(ReturnImmediately, MakeCallbackContext(state));
	const int unique_id = thread->GetUniqueId();
	thread->Detach();

	const bool callback_returned = WaitFor(state->returned);
	const int unique_id_after_callback = thread->GetUniqueId();
	thread.reset();

	EXPECT_TRUE(callback_returned);
	EXPECT_NE(unique_id, 0);
	EXPECT_EQ(unique_id_after_callback, unique_id);
}

TEST(CoreThreadLifetime, JoinRetainsTheCompletedThreadUniqueId)
{
	auto state = std::make_shared<CallbackState>();
	Core::Thread thread(ReturnImmediately, MakeCallbackContext(state));
	const int unique_id = thread.GetUniqueId();

	thread.Join();

	EXPECT_TRUE(state->returned.load(std::memory_order_acquire));
	EXPECT_EQ(state->callback_unique_id.load(std::memory_order_acquire), unique_id);
	EXPECT_EQ(thread.GetUniqueId(), unique_id);
}

UT_END();
