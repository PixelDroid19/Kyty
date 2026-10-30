#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Loader/GuestCall.h"
#include "Emulator/Log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

UT_BEGIN(EmulatorKernelLifetimes);

namespace {

using namespace std::chrono_literals;

void EnsureKernelLifetimesSubsystems()
{
	if (!Kyty::Config::IsInitialized())
	{
		Kyty::Config::ConfigSubsystem::Instance()->Init(Kyty::Core::SubsystemsList::Instance());
	}
	Kyty::Log::LogSubsystem::Instance()->Init(Kyty::Core::SubsystemsList::Instance());
	if (!Kyty::Kernel::PthreadIsInitialized())
	{
		Kyty::Kernel::PthreadSubsystem::Instance()->Init(Kyty::Core::SubsystemsList::Instance());
	}
}

void* KYTY_SYSV_ABI ReturnNull(void*)
{
	return nullptr;
}

struct ErrnoThreadContext
{
	int*             main_error_address = nullptr;
	std::atomic_int  error_value {0};
	std::atomic_bool separate_address {false};
};

class ScopedGuestThreadEntryInvoker
{
public:
	ScopedGuestThreadEntryInvoker()
	{
		Kyty::Emulator::GuestRuntimePort::Install({nullptr, nullptr, nullptr, Kyty::Loader::GuestCall::InvokeOnStack});
	}
	~ScopedGuestThreadEntryInvoker() { Kyty::Emulator::GuestRuntimePort::Install({}); }

	KYTY_CLASS_NO_COPY(ScopedGuestThreadEntryInvoker);
};

void* KYTY_SYSV_ABI SetGuestThreadErrno(void* arg)
{
	auto* context = static_cast<ErrnoThreadContext*>(arg);
	int*  error   = Kyty::Libs::Posix::GetErrorAddr();
	*error        = 73;
	context->error_value.store(*error, std::memory_order_release);
	context->separate_address.store(error != context->main_error_address, std::memory_order_release);
	return nullptr;
}

} // namespace

TEST(EmulatorKernelLifetimes, EventFlagCancelKeepsNewWaitersOutOfCanceledEpoch)
{
	EnsureKernelLifetimesSubsystems();

	using namespace Kyty::Kernel;
	using namespace Kyty::Kernel::EventFlag;

	constexpr int   kOldWaiterCount = 24;
	KernelEventFlag flag            = nullptr;
	ASSERT_EQ(KernelCreateEventFlag(&flag, "lifetime-eventflag-cancel", 0x20, 0, nullptr), OK);

	std::atomic_int               old_waiters_started {0};
	std::vector<std::future<int>> old_waiters;
	old_waiters.reserve(kOldWaiterCount);
	for (int i = 0; i < kOldWaiterCount; i++)
	{
		old_waiters.emplace_back(std::async(std::launch::async,
		                                    [flag, &old_waiters_started]
		                                    {
			                                    KernelUseconds timeout = 2'000'000;
			                                    old_waiters_started.fetch_add(1, std::memory_order_release);
			                                    return KernelWaitEventFlag(flag, 1, 0x01, nullptr, &timeout);
		                                    }));
	}
	const auto waiter_deadline = std::chrono::steady_clock::now() + 1s;
	while (old_waiters_started.load(std::memory_order_acquire) != kOldWaiterCount && std::chrono::steady_clock::now() < waiter_deadline)
	{
		std::this_thread::yield();
	}
	ASSERT_EQ(old_waiters_started.load(std::memory_order_acquire), kOldWaiterCount);
	std::this_thread::sleep_for(20ms);

	std::atomic_bool cancel_started {false};
	std::atomic_int  cancel_waiter_count {-1};
	auto             cancel = std::async(std::launch::async,
	                                     [flag, &cancel_started, &cancel_waiter_count]
	                                     {
		                         cancel_started.store(true, std::memory_order_release);
		                         int       waiting = -1;
		                         const int result  = KernelCancelEventFlag(flag, 0, &waiting);
		                         cancel_waiter_count.store(waiting, std::memory_order_release);
		                         return result;
	                                     });
	while (!cancel_started.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}

	std::atomic_bool fresh_waiter_started {false};
	KernelUseconds   fresh_timeout = 2'000'000;
	auto             fresh_waiter  = std::async(std::launch::async,
	                                            [flag, &fresh_waiter_started, fresh_timeout]() mutable
	                                            {
		                               fresh_waiter_started.store(true, std::memory_order_release);
		                               return KernelWaitEventFlag(flag, 1, 0x01, nullptr, &fresh_timeout);
	                                            });
	while (!fresh_waiter_started.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}

	EXPECT_EQ(cancel.wait_for(1s), std::future_status::ready);
	EXPECT_EQ(cancel.get(), OK);
	const int canceled_waiter_count = cancel_waiter_count.load(std::memory_order_acquire);
	EXPECT_GE(canceled_waiter_count, kOldWaiterCount);
	EXPECT_LE(canceled_waiter_count, kOldWaiterCount + 1);
	for (auto& waiter: old_waiters)
	{
		ASSERT_EQ(waiter.wait_for(1s), std::future_status::ready);
		EXPECT_EQ(waiter.get(), KERNEL_ERROR_ECANCELED);
	}

	if (canceled_waiter_count == kOldWaiterCount)
	{
		EXPECT_EQ(KernelSetEventFlag(flag, 1), OK);
	}
	ASSERT_EQ(fresh_waiter.wait_for(1s), std::future_status::ready);
	EXPECT_EQ(fresh_waiter.get(), canceled_waiter_count == kOldWaiterCount ? OK : KERNEL_ERROR_ECANCELED);
	EXPECT_EQ(KernelDeleteEventFlag(flag), OK);
}

TEST(EmulatorKernelLifetimes, EventFlagDeleteDrainsWaitersAndPinsConcurrentCalls)
{
	EnsureKernelLifetimesSubsystems();

	using namespace Kyty::Kernel;
	using namespace Kyty::Kernel::EventFlag;

	KernelEventFlag flag = nullptr;
	ASSERT_EQ(KernelCreateEventFlag(&flag, "lifetime-eventflag-delete", 0x20, 0, nullptr), OK);
	std::atomic_int waiter_started {0};
	KernelUseconds  timeout         = 2'000'000;
	auto            waiter          = std::async(std::launch::async,
	                                             [flag, &waiter_started, timeout]() mutable
	                                             {
		                         waiter_started.store(1, std::memory_order_release);
		                         return KernelWaitEventFlag(flag, 1, 0x01, nullptr, &timeout);
	                                             });
	const auto      waiter_deadline = std::chrono::steady_clock::now() + 1s;
	while (waiter_started.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < waiter_deadline)
	{
		std::this_thread::yield();
	}
	ASSERT_EQ(waiter_started.load(std::memory_order_acquire), 1);
	EXPECT_EQ(waiter.wait_for(20ms), std::future_status::timeout);

	std::atomic_bool         run_calls {true};
	std::atomic_int          unexpected_result {0};
	std::atomic_int          workers_ready {0};
	std::vector<std::thread> callers;
	for (int i = 0; i < 4; i++)
	{
		callers.emplace_back(
		    [flag, &run_calls, &unexpected_result, &workers_ready]
		    {
			    workers_ready.fetch_add(1, std::memory_order_release);
			    while (run_calls.load(std::memory_order_acquire))
			    {
				    const int set_result = KernelSetEventFlag(flag, 2);
				    if (set_result == KERNEL_ERROR_ESRCH || set_result == KERNEL_ERROR_EACCES)
				    {
					    break;
				    }
				    if (set_result != OK)
				    {
					    unexpected_result.store(set_result, std::memory_order_release);
					    break;
				    }
				    const int clear_result = KernelClearEventFlag(flag, 2);
				    if (clear_result == KERNEL_ERROR_ESRCH || clear_result == KERNEL_ERROR_EACCES)
				    {
					    break;
				    }
				    if (clear_result != OK)
				    {
					    unexpected_result.store(clear_result, std::memory_order_release);
					    break;
				    }
			    }
		    });
	}
	const auto callers_deadline = std::chrono::steady_clock::now() + 1s;
	while (workers_ready.load(std::memory_order_acquire) != static_cast<int>(callers.size()) &&
	       std::chrono::steady_clock::now() < callers_deadline)
	{
		std::this_thread::yield();
	}
	EXPECT_EQ(workers_ready.load(std::memory_order_acquire), static_cast<int>(callers.size()));
	std::this_thread::sleep_for(10ms);
	EXPECT_EQ(KernelDeleteEventFlag(flag), OK);
	run_calls.store(false, std::memory_order_release);
	for (auto& caller: callers)
	{
		caller.join();
	}
	EXPECT_EQ(unexpected_result.load(std::memory_order_acquire), 0);
	ASSERT_EQ(waiter.wait_for(1s), std::future_status::ready);
	EXPECT_EQ(waiter.get(), KERNEL_ERROR_EACCES);
	EXPECT_EQ(KernelSetEventFlag(flag, 1), KERNEL_ERROR_ESRCH);
}

TEST(EmulatorKernelLifetimes, EventFlagUnmatchedSetDoesNotCreateWakeStorm)
{
#if defined(__linux__)
	EnsureKernelLifetimesSubsystems();

	using namespace Kyty::Kernel;
	using namespace Kyty::Kernel::EventFlag;

	constexpr int                  kWaiterCount = 8;
	constexpr auto                 kSampleTime  = 250ms;
	constexpr std::int64_t         kCpuBudgetNs = 250'000'000; // One full CPU for the entire sample.
	KernelEventFlag                flag         = nullptr;
	ASSERT_EQ(KernelCreateEventFlag(&flag, "lifetime-eventflag-unmatched-set", 0x20, 0, nullptr), OK);

	std::atomic_int               waiters_started {0};
	std::vector<std::future<int>> waiters;
	waiters.reserve(kWaiterCount);
	for (int i = 0; i < kWaiterCount; i++)
	{
		waiters.emplace_back(std::async(std::launch::async,
		                                [flag, &waiters_started]
		                                {
			                                KernelUseconds timeout = 5'000'000;
			                                waiters_started.fetch_add(1, std::memory_order_release);
			                                return KernelWaitEventFlag(flag, 1, 0x01, nullptr, &timeout);
		                                }));
	}

	const auto waiter_deadline = std::chrono::steady_clock::now() + 1s;
	while (waiters_started.load(std::memory_order_acquire) != kWaiterCount && std::chrono::steady_clock::now() < waiter_deadline)
	{
		std::this_thread::yield();
	}
	EXPECT_EQ(waiters_started.load(std::memory_order_acquire), kWaiterCount);
	// Let each entered call reach the condition wait before measuring an unrelated Set.
	std::this_thread::sleep_for(50ms);

	struct rusage cpu_before {};
	struct rusage cpu_after {};
	const int     cpu_before_status = getrusage(RUSAGE_SELF, &cpu_before);
	const int     set_result        = KernelSetEventFlag(flag, 2);
	const auto    sample_start      = std::chrono::steady_clock::now();
	std::this_thread::sleep_for(kSampleTime);
	const auto sample_elapsed = std::chrono::steady_clock::now() - sample_start;
	const int  cpu_after_status = getrusage(RUSAGE_SELF, &cpu_after);
	const auto sample_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sample_elapsed).count();
	::testing::Test::RecordProperty("sample_elapsed_ns", std::to_string(sample_elapsed_ns));

	int       canceled_waiters = -1;
	const int cancel_result    = KernelCancelEventFlag(flag, 0, &canceled_waiters);
	// Release an unusually late call only on a failed waiter qualification path.
	const int release_result = canceled_waiters == kWaiterCount ? OK : KernelSetEventFlag(flag, 1);
	for (auto& waiter: waiters)
	{
		EXPECT_EQ(waiter.wait_for(1s), std::future_status::ready);
		const int wait_result = waiter.get();
		if (canceled_waiters == kWaiterCount)
		{
			EXPECT_EQ(wait_result, KERNEL_ERROR_ECANCELED);
		}
	}

	EXPECT_EQ(set_result, OK);
	EXPECT_EQ(cancel_result, OK);
	EXPECT_EQ(release_result, OK);
	EXPECT_EQ(canceled_waiters, kWaiterCount);
	EXPECT_EQ(KernelDeleteEventFlag(flag), OK);

	EXPECT_EQ(cpu_before_status, 0);
	EXPECT_EQ(cpu_after_status, 0);
	if (cpu_before_status == 0 && cpu_after_status == 0)
	{
		const auto process_cpu_ns = [](const rusage& usage)
		{
			return static_cast<std::int64_t>(usage.ru_utime.tv_sec) * 1'000'000'000 +
			       static_cast<std::int64_t>(usage.ru_utime.tv_usec) * 1'000 +
			       static_cast<std::int64_t>(usage.ru_stime.tv_sec) * 1'000'000'000 +
			       static_cast<std::int64_t>(usage.ru_stime.tv_usec) * 1'000;
		};
		const auto aggregate_cpu_ns = process_cpu_ns(cpu_after) - process_cpu_ns(cpu_before);
		::testing::Test::RecordProperty("aggregate_cpu_ns", std::to_string(aggregate_cpu_ns));
		EXPECT_LE(aggregate_cpu_ns, kCpuBudgetNs) << "aggregate process CPU during "
		                                         << std::chrono::duration_cast<std::chrono::milliseconds>(sample_elapsed).count() << " ms sample";
	}
#else
	GTEST_SKIP() << "Linux process CPU accounting is required for this regression check";
#endif
}

TEST(EmulatorKernelLifetimes, EventFlagHandlesDoNotAliasDeletedObjects)
{
	EnsureKernelLifetimesSubsystems();

	using namespace Kyty::Kernel;
	using namespace Kyty::Kernel::EventFlag;

	std::vector<KernelEventFlag> stale_handles;
	for (int i = 0; i < 16; i++)
	{
		KernelEventFlag current = nullptr;
		ASSERT_EQ(KernelCreateEventFlag(&current, "lifetime-eventflag-token", 0x20, 0, nullptr), OK);
		for (const auto stale: stale_handles)
		{
			EXPECT_NE(current, stale);
			EXPECT_EQ(KernelSetEventFlag(stale, 1), KERNEL_ERROR_ESRCH);
		}
		EXPECT_EQ(KernelDeleteEventFlag(current), OK);
		EXPECT_EQ(KernelSetEventFlag(current, 1), KERNEL_ERROR_ESRCH);
		stale_handles.push_back(current);
	}
}

TEST(EmulatorKernelLifetimes, SemaphoreSignalTakenByOneWaiterDoesNotCreateWakeStorm)
{
#if defined(__linux__)
	EnsureKernelLifetimesSubsystems();

	using namespace Kyty::Kernel;
	using namespace Kyty::Kernel::Semaphore;

	constexpr int          kWaiterCount = 8;
	constexpr auto         kSampleTime  = 250ms;
	constexpr std::int64_t kCpuBudgetNs = 250'000'000; // One full CPU for the entire sample.
	KernelSema             semaphore    = nullptr;
	ASSERT_EQ(KernelCreateSema(&semaphore, "lifetime-sema-wake-storm", 1, 0, kWaiterCount, nullptr), OK);

	std::atomic_int               waiters_started {0};
	std::vector<std::future<int>> waiters;
	waiters.reserve(kWaiterCount);
	for (int i = 0; i < kWaiterCount; i++)
	{
		waiters.emplace_back(std::async(std::launch::async,
		                                [semaphore, &waiters_started]
		                                {
			                                KernelUseconds timeout = 5'000'000;
			                                waiters_started.fetch_add(1, std::memory_order_release);
			                                return KernelWaitSema(semaphore, 1, &timeout);
		                                }));
	}

	const auto waiter_deadline = std::chrono::steady_clock::now() + 1s;
	while (waiters_started.load(std::memory_order_acquire) != kWaiterCount && std::chrono::steady_clock::now() < waiter_deadline)
	{
		std::this_thread::yield();
	}
	EXPECT_EQ(waiters_started.load(std::memory_order_acquire), kWaiterCount);
	// Let each entered call reach the condition wait before one waiter takes the count.
	std::this_thread::sleep_for(50ms);

	struct rusage cpu_before {};
	struct rusage cpu_after {};
	const int     cpu_before_status = getrusage(RUSAGE_SELF, &cpu_before);
	const int     signal_result     = KernelSignalSema(semaphore, 1);
	const auto    sample_start      = std::chrono::steady_clock::now();
	std::this_thread::sleep_for(kSampleTime);
	const auto sample_elapsed   = std::chrono::steady_clock::now() - sample_start;
	const int  cpu_after_status = getrusage(RUSAGE_SELF, &cpu_after);

	int       canceled_waiters = -1;
	const int cancel_result    = KernelCancelSema(semaphore, 0, &canceled_waiters);
	int       ok_results       = 0;
	for (auto& waiter: waiters)
	{
		EXPECT_EQ(waiter.wait_for(1s), std::future_status::ready);
		const int wait_result = waiter.get();
		ok_results += (wait_result == OK ? 1 : 0);
		EXPECT_TRUE(wait_result == OK || wait_result == KERNEL_ERROR_ECANCELED);
	}

	EXPECT_EQ(signal_result, OK);
	EXPECT_EQ(cancel_result, OK);
	EXPECT_EQ(canceled_waiters, kWaiterCount - 1);
	EXPECT_EQ(ok_results, 1);
	EXPECT_EQ(KernelDeleteSema(semaphore), OK);

	EXPECT_EQ(cpu_before_status, 0);
	EXPECT_EQ(cpu_after_status, 0);
	if (cpu_before_status == 0 && cpu_after_status == 0)
	{
		const auto process_cpu_ns = [](const rusage& usage)
		{
			return static_cast<std::int64_t>(usage.ru_utime.tv_sec) * 1'000'000'000 +
			       static_cast<std::int64_t>(usage.ru_utime.tv_usec) * 1'000 +
			       static_cast<std::int64_t>(usage.ru_stime.tv_sec) * 1'000'000'000 +
			       static_cast<std::int64_t>(usage.ru_stime.tv_usec) * 1'000;
		};
		const auto aggregate_cpu_ns = process_cpu_ns(cpu_after) - process_cpu_ns(cpu_before);
		EXPECT_LE(aggregate_cpu_ns, kCpuBudgetNs) << "aggregate process CPU during "
		                                         << std::chrono::duration_cast<std::chrono::milliseconds>(sample_elapsed).count() << " ms sample";
	}
#else
	GTEST_SKIP() << "Linux process CPU accounting is required for this regression check";
#endif
}

TEST(EmulatorKernelLifetimes, SemaphoreCancelKeepsNewWaitersOutOfCanceledEpoch)
{
	EnsureKernelLifetimesSubsystems();

	using namespace Kyty::Kernel;
	using namespace Kyty::Kernel::Semaphore;

	constexpr int kOldWaiterCount = 24;
	KernelSema    semaphore       = nullptr;
	ASSERT_EQ(KernelCreateSema(&semaphore, "lifetime-sema-cancel", 1, 0, 1, nullptr), OK);

	std::atomic_int               old_waiters_started {0};
	std::vector<std::future<int>> old_waiters;
	old_waiters.reserve(kOldWaiterCount);
	for (int i = 0; i < kOldWaiterCount; i++)
	{
		old_waiters.emplace_back(std::async(std::launch::async,
		                                    [semaphore, &old_waiters_started]
		                                    {
			                                    KernelUseconds timeout = 2'000'000;
			                                    old_waiters_started.fetch_add(1, std::memory_order_release);
			                                    return KernelWaitSema(semaphore, 1, &timeout);
		                                    }));
	}
	const auto waiter_deadline = std::chrono::steady_clock::now() + 1s;
	while (old_waiters_started.load(std::memory_order_acquire) != kOldWaiterCount && std::chrono::steady_clock::now() < waiter_deadline)
	{
		std::this_thread::yield();
	}
	ASSERT_EQ(old_waiters_started.load(std::memory_order_acquire), kOldWaiterCount);
	std::this_thread::sleep_for(20ms);

	std::atomic_bool cancel_started {false};
	std::atomic_int  cancel_waiter_count {-1};
	auto             cancel = std::async(std::launch::async,
	                                     [semaphore, &cancel_started, &cancel_waiter_count]
	                                     {
		                         cancel_started.store(true, std::memory_order_release);
		                         int       waiting = -1;
		                         const int result  = KernelCancelSema(semaphore, 0, &waiting);
		                         cancel_waiter_count.store(waiting, std::memory_order_release);
		                         return result;
	                                     });
	while (!cancel_started.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}

	std::atomic_bool fresh_waiter_started {false};
	KernelUseconds   fresh_timeout = 2'000'000;
	auto             fresh_waiter  = std::async(std::launch::async,
	                                            [semaphore, &fresh_waiter_started, fresh_timeout]() mutable
	                                            {
		                               fresh_waiter_started.store(true, std::memory_order_release);
		                               return KernelWaitSema(semaphore, 1, &fresh_timeout);
	                                            });
	while (!fresh_waiter_started.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}

	EXPECT_EQ(cancel.wait_for(1s), std::future_status::ready);
	EXPECT_EQ(cancel.get(), OK);
	const int canceled_waiter_count = cancel_waiter_count.load(std::memory_order_acquire);
	EXPECT_GE(canceled_waiter_count, kOldWaiterCount);
	EXPECT_LE(canceled_waiter_count, kOldWaiterCount + 1);
	for (auto& waiter: old_waiters)
	{
		ASSERT_EQ(waiter.wait_for(1s), std::future_status::ready);
		EXPECT_EQ(waiter.get(), KERNEL_ERROR_ECANCELED);
	}

	if (canceled_waiter_count == kOldWaiterCount)
	{
		EXPECT_EQ(KernelSignalSema(semaphore, 1), OK);
	}
	ASSERT_EQ(fresh_waiter.wait_for(1s), std::future_status::ready);
	EXPECT_EQ(fresh_waiter.get(), canceled_waiter_count == kOldWaiterCount ? OK : KERNEL_ERROR_ECANCELED);
	EXPECT_EQ(KernelDeleteSema(semaphore), OK);
}

TEST(EmulatorKernelLifetimes, SemaphoreHandlesAreTypedAndDoNotAliasDeletedObjects)
{
	EnsureKernelLifetimesSubsystems();

	using namespace Kyty::Kernel;
	using namespace Kyty::Kernel::EventFlag;
	using namespace Kyty::Kernel::Semaphore;

	EXPECT_EQ(KernelCreateSema(nullptr, "lifetime-sema-null-output", 0x02, 0, 1, nullptr), KERNEL_ERROR_EINVAL);

	KernelEventFlag flag      = nullptr;
	KernelSema      semaphore = nullptr;
	ASSERT_EQ(KernelCreateEventFlag(&flag, "lifetime-eventflag-typed", 0x20, 0, nullptr), OK);
	ASSERT_EQ(KernelCreateSema(&semaphore, "lifetime-sema-typed", 0x02, 0, 1, nullptr), OK);

	const auto flag_as_semaphore = reinterpret_cast<KernelSema>(flag);
	const auto semaphore_as_flag = reinterpret_cast<KernelEventFlag>(semaphore);
	EXPECT_NE(flag_as_semaphore, semaphore);
	EXPECT_NE(semaphore_as_flag, flag);
	EXPECT_EQ(KernelPollSema(flag_as_semaphore, 1), KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelSetEventFlag(semaphore_as_flag, 1), KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelDeleteSema(flag_as_semaphore), KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelDeleteEventFlag(semaphore_as_flag), KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelDeleteEventFlag(flag), OK);
	EXPECT_EQ(KernelDeleteSema(semaphore), OK);

	std::vector<KernelSema> stale_handles;
	for (int i = 0; i < 16; i++)
	{
		KernelSema current = nullptr;
		ASSERT_EQ(KernelCreateSema(&current, "lifetime-sema-token", 0x02, 0, 1, nullptr), OK);
		for (const auto stale: stale_handles)
		{
			EXPECT_NE(current, stale);
			EXPECT_EQ(KernelSignalSema(stale, 1), KERNEL_ERROR_ESRCH);
		}
		EXPECT_EQ(KernelDeleteSema(current), OK);
		EXPECT_EQ(KernelSignalSema(current, 1), KERNEL_ERROR_ESRCH);
		stale_handles.push_back(current);
	}
}

TEST(EmulatorKernelLifetimes, FailedPthreadDestroyPreservesNativeObject)
{
	// POSIX leaves destruction of a locked rwlock or a condition variable with
	// waiters undefined. This host's glibc returns success for the locked rwlock
	// case, so calling the wrapper and then using the handle would be unsafe.
#if defined(__GLIBC__)
	GTEST_SKIP()
	    << "glibc does not provide a safe EBUSY fixture for native pthread destroy; Linux busy-retention is not dynamically proven";
#else
	GTEST_SKIP() << "no documented native pthread destroy busy precondition is established for this host";
#endif
}

TEST(EmulatorKernelLifetimes, FailedPthreadCreateReleasesPoolSlot)
{
	EnsureKernelLifetimesSubsystems();

	using namespace Kyty::Kernel;

	PthreadAttr attr = nullptr;
	ASSERT_EQ(PthreadAttrInit(&attr), OK);
	ASSERT_EQ(PthreadAttrSetguardsize(&attr, std::numeric_limits<size_t>::max() / 2), OK);
	PthreadThreadDiagnostics before {};
	ASSERT_TRUE(PthreadGetThreadDiagnostics(&before));

	Pthread thread = nullptr;
	EXPECT_EQ(PthreadCreate(&thread, &attr, ReturnNull, nullptr, "lifetime-failed-create"), KERNEL_ERROR_EAGAIN);

	PthreadThreadDiagnostics after {};
	ASSERT_TRUE(PthreadGetThreadDiagnostics(&after));
	EXPECT_EQ(after.active_count, before.active_count);
	EXPECT_EQ(PthreadAttrDestroy(&attr), OK);
}

TEST(EmulatorKernelLifetimes, ErrnoAddressIsIsolatedAcrossGuestPthreads)
{
	EnsureKernelLifetimesSubsystems();
	ScopedGuestThreadEntryInvoker entry_invoker;

	auto* main_error_address = Kyty::Libs::Posix::GetErrorAddr();
	*main_error_address      = 19;
	ErrnoThreadContext context {};
	context.main_error_address = main_error_address;

	Kyty::Kernel::Pthread thread = nullptr;
	ASSERT_EQ(Kyty::Kernel::PthreadCreate(&thread, nullptr, SetGuestThreadErrno, &context, "lifetime-errno"), Kyty::Kernel::OK);
	EXPECT_EQ(Kyty::Kernel::PthreadJoin(thread, nullptr), Kyty::Kernel::OK);

	EXPECT_TRUE(context.separate_address.load(std::memory_order_acquire));
	EXPECT_EQ(context.error_value.load(std::memory_order_acquire), 73);
	EXPECT_EQ(*main_error_address, 19);
}

UT_END();
