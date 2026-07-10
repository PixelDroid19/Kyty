#include "Kyty/Core/MemoryAlloc.h"
#include "Kyty/UnitTest.h"

#include <atomic>
#include <thread>
#include <vector>

UT_BEGIN(CoreMemoryAlloc);

TEST(CoreMemoryAlloc, RecursionStateUsesTheCompleteThreadToken)
{
	Core::MemoryAllocDetail::RecursionState state;
	constexpr uint64_t                      first_token  = 0x000000010000001eu;
	constexpr uint64_t                      second_token = 0x000000020000001eu;

	EXPECT_FALSE(state.IsOwnedBy(first_token));
	state.Enter(first_token);
	EXPECT_TRUE(state.IsOwnedBy(first_token));
	EXPECT_FALSE(state.IsOwnedBy(second_token));
	state.Enter(first_token);
	EXPECT_EQ(state.Depth(), 2u);
	state.Leave(first_token);
	state.Leave(first_token);

	state.Enter(second_token);
	EXPECT_FALSE(state.IsOwnedBy(first_token));
	EXPECT_TRUE(state.IsOwnedBy(second_token));
	state.Leave(second_token);
}

TEST(CoreMemoryAlloc, GuestDomainUsesCompleteThreadTokens)
{
	Core::MemoryAllocDetail::ThreadDomainRegistry registry;
	constexpr uint64_t                            first_token  = 0x000000010000001eu;
	constexpr uint64_t                            second_token = 0x000000020000001eu;

	EXPECT_TRUE(registry.Add(first_token));
	EXPECT_TRUE(registry.Add(second_token));
	EXPECT_TRUE(registry.Contains(first_token));
	EXPECT_TRUE(registry.Contains(second_token));
	EXPECT_TRUE(registry.Remove(first_token));
	EXPECT_FALSE(registry.Contains(first_token));
	EXPECT_TRUE(registry.Contains(second_token));
}

TEST(CoreMemoryAlloc, ReallocatesUntrackedSystemBlocks)
{
	Core::mem_tracker_disable();
	auto* memory = static_cast<uint8_t*>(Core::mem_alloc(32));
	for (uint8_t index = 0; index < 32; ++index)
	{
		memory[index] = index;
	}
	Core::mem_tracker_enable();

	memory = static_cast<uint8_t*>(Core::mem_realloc(memory, 64));
	ASSERT_NE(memory, nullptr);
	for (uint8_t index = 0; index < 32; ++index)
	{
		EXPECT_EQ(memory[index], index);
	}
	Core::mem_free(memory);
}

TEST(CoreMemoryAlloc, GuestDomainRoutesAllOperationsToTheSystemAllocator)
{
	// Outside a guest domain the query is false and the tracker owns allocations.
	EXPECT_FALSE(Core::mem_guest_thread_is_active());

	Core::mem_guest_thread_enter();
	EXPECT_TRUE(Core::mem_guest_thread_is_active());

	// During a guest domain every allocator operation must use the system heap and
	// stay out of the debug tracker: a guest thread can transfer control or exit
	// while native code is between calls, so it must never hold the tracker lock or
	// register blocks the tracker would later touch.
	auto* memory = static_cast<uint8_t*>(Core::mem_alloc(48));
	ASSERT_NE(memory, nullptr);
	for (uint8_t index = 0; index < 48; ++index)
	{
		memory[index] = index;
	}
	EXPECT_FALSE(Core::mem_check(memory));

	memory = static_cast<uint8_t*>(Core::mem_realloc(memory, 96));
	ASSERT_NE(memory, nullptr);
	for (uint8_t index = 0; index < 48; ++index)
	{
		EXPECT_EQ(memory[index], index);
	}
	EXPECT_FALSE(Core::mem_check(memory));

	Core::mem_free(memory);

	Core::mem_guest_thread_leave();
	EXPECT_FALSE(Core::mem_guest_thread_is_active());
}

TEST(CoreMemoryAlloc, TrackedBlockFreesCorrectlyInsideGuestDomain)
{
	// Allocated outside a guest domain, so the debug tracker owns the block.
	Core::mem_tracker_enable();
	Core::MemStats before {};
	Core::mem_get_stat(&before);
	auto* memory = static_cast<uint8_t*>(Core::mem_alloc(64));
	ASSERT_NE(memory, nullptr);
	Core::MemStats allocated {};
	Core::mem_get_stat(&allocated);
	ASSERT_EQ(allocated.blocks_num, before.blocks_num + 1);
	for (uint8_t index = 0; index < 64; ++index)
	{
		memory[index] = index;
	}

	// A different subsystem frees the same block while this thread is inside a
	// guest domain. The freed base must be the block's real allocation base
	// regardless of who allocated it, or the system allocator aborts.
	Core::mem_guest_thread_enter();
	Core::mem_free(memory);
	Core::mem_guest_thread_leave();

	Core::MemStats released {};
	Core::mem_get_stat(&released);
	EXPECT_EQ(released.blocks_num, before.blocks_num);
}

TEST(CoreMemoryAlloc, ConcurrentTrackerRecursionIsThreadIsolated)
{
	constexpr int thread_count = 320;
	constexpr int iterations   = 64;

	std::atomic<int>         ready {0};
	std::atomic<bool>        start {false};
	std::atomic<bool>        valid {true};
	std::vector<std::thread> threads;
	threads.reserve(thread_count);

	for (int thread_index = 0; thread_index < thread_count; ++thread_index)
	{
		threads.emplace_back(
		    [thread_index, &ready, &start, &valid]()
		    {
			    ready.fetch_add(1, std::memory_order_release);
			    while (!start.load(std::memory_order_acquire))
			    {
				    std::this_thread::yield();
			    }

			    for (int iteration = 0; iteration < iterations; ++iteration)
			    {
				    const auto size = static_cast<size_t>(32 + ((thread_index + iteration) & 63));
				    auto*      data = static_cast<uint8_t*>(Core::mem_alloc(size));
				    for (size_t index = 0; index < size; ++index)
				    {
					    data[index] = static_cast<uint8_t>(thread_index + iteration + static_cast<int>(index));
				    }
				    for (size_t index = 0; index < size; ++index)
				    {
					    if (data[index] != static_cast<uint8_t>(thread_index + iteration + static_cast<int>(index)))
					    {
						    valid.store(false, std::memory_order_relaxed);
					    }
				    }
				    Core::mem_free(data);
			    }
		    });
	}

	while (ready.load(std::memory_order_acquire) != thread_count)
	{
		std::this_thread::yield();
	}
	start.store(true, std::memory_order_release);

	for (auto& thread: threads)
	{
		thread.join();
	}

	EXPECT_TRUE(valid.load(std::memory_order_relaxed));
}

UT_END();
