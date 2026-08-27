#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Kernel/GpuMappingLifecycle.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/SyncOnAddress.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <sys/mman.h>
#endif

UT_BEGIN(EmulatorKernelMemory);

using namespace Libs;
using namespace Kernel::Memory;

static void EnsureMemorySubsystemInitialized()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	// Keep this fixture independent from UnitTestMain's global registration
	// order. Kernel memory queries need a concrete guest platform, while a
	// filtered test invocation may not have run the emulator bootstrap first.
	if (Config::GetGuestPlatform() == GuestPlatform::Unknown)
	{
		ASSERT_TRUE(Config::SetGuestPlatform(GuestPlatform::Ps4));
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	static bool memory_inited = false;
	if (!memory_inited)
	{
		MemorySubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		memory_inited = true;
	}
}

class GuestWritableBlock
{
public:
	explicit GuestWritableBlock(size_t size): m_address(Core::VirtualMemory::Alloc(0, size, Core::VirtualMemory::Mode::ReadWrite)) {}
	~GuestWritableBlock()
	{
		if (m_address != 0)
		{
			(void)Core::VirtualMemory::Free(m_address);
		}
	}

	GuestWritableBlock(const GuestWritableBlock&)            = delete;
	GuestWritableBlock& operator=(const GuestWritableBlock&) = delete;

	[[nodiscard]] bool IsValid() const { return m_address != 0; }
	template <typename T> [[nodiscard]] T* Data() const { return reinterpret_cast<T*>(m_address); }

private:
	uint64_t m_address = 0;
};

TEST(EmulatorKernelMemory, GpuUnmapGateKeepsAdmissionsClosedThroughHostUnmap)
{
	Graphics::GpuSubmissionAdmissionGate gate;

	std::mutex              state_mutex;
	std::condition_variable state_condition;
	bool                    admission_started = false;
	std::atomic_bool        admission_entered  = false;
	bool                    drained            = false;
	bool                    detached           = false;
	bool                    host_mapping_live  = true;

	std::thread submitter;
	gate.RunQuiesced(
	    [&]
	    {
		    drained = true;
		    submitter = std::thread(
		        [&]
		        {
			        {
				        std::lock_guard<std::mutex> lock(state_mutex);
				        admission_started = true;
			        }
			        state_condition.notify_one();
			        gate.RunAdmitted([&] { admission_entered = true; });
		        });

		    std::unique_lock<std::mutex> lock(state_mutex);
		    state_condition.wait(lock, [&] { return admission_started; });
	    },
	    [&]
	    {
		    EXPECT_TRUE(drained);
		    EXPECT_FALSE(admission_entered.load());
		    EXPECT_TRUE(host_mapping_live);

		    detached = true;
		    EXPECT_FALSE(admission_entered.load());
		    EXPECT_TRUE(host_mapping_live);

		    host_mapping_live = false;
		    EXPECT_TRUE(detached);
		    EXPECT_FALSE(admission_entered.load());
	    });

	submitter.join();
	EXPECT_TRUE(admission_entered.load());
	EXPECT_FALSE(host_mapping_live);
}

namespace {

struct GpuMappingLifecycleTestState
{
	enum class Event : uint8_t
	{
		Register,
		Invalidate,
		Release,
		Complete,
	};

	std::array<Event, 4>     events {};
	uint32_t                 event_count      = 0;
	uint64_t                 register_vaddr   = 0;
	uint64_t                 register_size    = 0;
	uint64_t                 invalidate_vaddr = 0;
	uint64_t                 invalidate_size  = 0;
	uint64_t                 release_vaddr    = 0;
	uint64_t                 release_size     = 0;
	GpuMappingLifecyclePort* lifecycle        = nullptr;

	void Record(Event event)
	{
		ASSERT_LT(event_count, events.size());
		events[event_count++] = event;
	}

	static void RegisterRange(void* context, uint64_t vaddr, uint64_t size)
	{
		auto* state           = static_cast<GpuMappingLifecycleTestState*>(context);
		state->register_vaddr = vaddr;
		state->register_size  = size;
		state->Record(Event::Register);
	}

	static bool ReleaseRange(void* context, uint64_t vaddr, uint64_t size, KernelGpuMappingCompletion completion, void* data)
	{
		auto* state          = static_cast<GpuMappingLifecycleTestState*>(context);
		state->release_vaddr = vaddr;
		state->release_size  = size;
		state->Record(Event::Release);
		return completion(data);
	}

	static bool InvalidateRange(void* context, uint64_t vaddr, uint64_t size)
	{
		auto* state             = static_cast<GpuMappingLifecycleTestState*>(context);
		state->invalidate_vaddr = vaddr;
		state->invalidate_size  = size;
		state->Record(Event::Invalidate);
		return true;
	}

	static bool Complete(void* data)
	{
		auto* state = static_cast<GpuMappingLifecycleTestState*>(data);
		state->Record(Event::Complete);
		return true;
	}

	static bool ReentrantReleaseRange(void* context, uint64_t vaddr, uint64_t size, KernelGpuMappingCompletion completion, void* data)
	{
		auto* state          = static_cast<GpuMappingLifecycleTestState*>(context);
		state->release_vaddr = vaddr;
		state->release_size  = size;
		state->Record(Event::Release);
		if (!state->lifecycle->RegisterRange(vaddr + size, size))
		{
			return false;
		}
		return completion(data);
	}
};

} // namespace

// This catches a lifecycle port that bypasses the adapter, allows a partial
// bundle, or completes the kernel unmap before the adapter has quiesced it.
TEST(EmulatorKernelMemory, GpuMappingLifecyclePortForwardsReleaseCompletionInAdapterOrder)
{
	GpuMappingLifecyclePort      lifecycle;
	GpuMappingLifecycleCallbacks partial {};
	partial.register_range = GpuMappingLifecycleTestState::RegisterRange;
	EXPECT_FALSE(lifecycle.Install(partial));
	EXPECT_FALSE(lifecycle.IsInstalled());
	EXPECT_FALSE(lifecycle.RegisterRange(0x100000u, 0x4000u));
	EXPECT_FALSE(lifecycle.InvalidateRange(0x100000u, 0x4000u));
	EXPECT_FALSE(lifecycle.ReleaseRange(0x100000u, 0x4000u, GpuMappingLifecycleTestState::Complete, nullptr));

	GpuMappingLifecycleTestState state {};
	GpuMappingLifecycleCallbacks callbacks {};
	callbacks.context          = &state;
	callbacks.register_range   = GpuMappingLifecycleTestState::RegisterRange;
	callbacks.invalidate_range = GpuMappingLifecycleTestState::InvalidateRange;
	callbacks.release_range    = GpuMappingLifecycleTestState::ReleaseRange;
	ASSERT_TRUE(lifecycle.Install(callbacks));

	ASSERT_TRUE(lifecycle.RegisterRange(0x100000u, 0x4000u));
	ASSERT_TRUE(lifecycle.InvalidateRange(0x100000u, 0x4000u));
	ASSERT_TRUE(lifecycle.ReleaseRange(0x100000u, 0x4000u, GpuMappingLifecycleTestState::Complete, &state));

	EXPECT_EQ(state.register_vaddr, 0x100000u);
	EXPECT_EQ(state.register_size, 0x4000u);
	EXPECT_EQ(state.invalidate_vaddr, 0x100000u);
	EXPECT_EQ(state.invalidate_size, 0x4000u);
	EXPECT_EQ(state.release_vaddr, 0x100000u);
	EXPECT_EQ(state.release_size, 0x4000u);
	ASSERT_EQ(state.event_count, 4u);
	EXPECT_EQ(state.events[0], GpuMappingLifecycleTestState::Event::Register);
	EXPECT_EQ(state.events[1], GpuMappingLifecycleTestState::Event::Invalidate);
	EXPECT_EQ(state.events[2], GpuMappingLifecycleTestState::Event::Release);
	EXPECT_EQ(state.events[3], GpuMappingLifecycleTestState::Event::Complete);

	EXPECT_FALSE(lifecycle.Install(callbacks));
}

TEST(EmulatorKernelMemory, GpuMappingLifecyclePortAllowsAdapterReentrancy)
{
	GpuMappingLifecyclePort      lifecycle;
	GpuMappingLifecycleTestState state {};
	state.lifecycle = &lifecycle;
	GpuMappingLifecycleCallbacks callbacks {};
	callbacks.context          = &state;
	callbacks.register_range   = GpuMappingLifecycleTestState::RegisterRange;
	callbacks.invalidate_range = GpuMappingLifecycleTestState::InvalidateRange;
	callbacks.release_range    = GpuMappingLifecycleTestState::ReentrantReleaseRange;
	ASSERT_TRUE(lifecycle.Install(callbacks));

	ASSERT_TRUE(lifecycle.ReleaseRange(0x100000u, 0x4000u, GpuMappingLifecycleTestState::Complete, &state));
	ASSERT_EQ(state.event_count, 3u);
	EXPECT_EQ(state.events[0], GpuMappingLifecycleTestState::Event::Release);
	EXPECT_EQ(state.events[1], GpuMappingLifecycleTestState::Event::Register);
	EXPECT_EQ(state.events[2], GpuMappingLifecycleTestState::Event::Complete);
}

TEST(EmulatorKernelMemory, CheckedReleaseReportsGuestErrors)
{
	EnsureMemorySubsystemInitialized();

	int64_t address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(0x10000, 0x40000, 0x10000, 0x10000, 12, &address), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(address, 0x10000), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(address, 0x10000), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(KernelReleaseDirectMemory(address, 0x10000), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(address, 0), LibKernel::KERNEL_ERROR_EINVAL);
}

TEST(EmulatorKernelMemory, DirectMemorySizeTracksGuestGeneration)
{
	EnsureMemorySubsystemInitialized();

	Config::SetNextGen(false);
	EXPECT_EQ(KernelGetDirectMemorySize(), static_cast<size_t>(5376) * 1024 * 1024);

	Config::SetNextGen(true);
	EXPECT_EQ(KernelGetDirectMemorySize(), static_cast<size_t>(16) * 1024 * 1024 * 1024);

	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, AutomaticDirectMapUsesPs5UserAddressRange)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t   kSize           = 0x10000;
	constexpr uint64_t kGuestUserBegin = 0x2000000000ull;
	constexpr uint64_t kGuestUserEnd   = 0x40000000000ull;

	int64_t physical_address = 0;
	ASSERT_EQ(KernelAllocateMainDirectMemory(kSize, kSize, 12, &physical_address), OK);

	void*     mapping    = nullptr;
	const int map_result = KernelMapDirectMemory(&mapping, kSize, 0x02, 0, physical_address, kSize);
	EXPECT_EQ(map_result, OK);
	if (map_result == OK)
	{
		const auto address = reinterpret_cast<uint64_t>(mapping);
		EXPECT_GE(address, kGuestUserBegin);
		EXPECT_LT(address, kGuestUserEnd);
		EXPECT_EQ(KernelMunmap(address, kSize), OK);
	}

	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, VirtualQueryReportsReservedRangeAsUncommitted)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t kSize = 0x10000;
	void*            address = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&address, kSize, 0, kSize), OK);
	ASSERT_NE(address, nullptr);

	VirtualQueryInfo info {};
	ASSERT_EQ(KernelVirtualQuery(static_cast<uint8_t*>(address) + 0x4000, 0, &info, sizeof(info)), OK);
	EXPECT_EQ(info.start, reinterpret_cast<uintptr_t>(address));
	EXPECT_EQ(info.end, reinterpret_cast<uintptr_t>(address) + kSize);
	EXPECT_EQ(info.protection, 0);
	EXPECT_EQ(info.is_direct, 0u);
	EXPECT_EQ(info.is_flexible, 0u);
	EXPECT_EQ(info.is_committed, 0u);

	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(address), kSize), OK);
}

TEST(EmulatorKernelMemory, MunmapSplitsReservedVirtualRange)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t kSize     = 0x10000;
	constexpr size_t kPageSize = 0x4000;
	void*            address   = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&address, kSize, 0, kPageSize), OK);
	ASSERT_NE(address, nullptr);
	const auto base = reinterpret_cast<uint64_t>(address);

	ASSERT_EQ(KernelMunmap(base + kPageSize, kPageSize), OK);

	VirtualQueryInfo prefix {};
	VirtualQueryInfo suffix {};
	EXPECT_EQ(KernelVirtualQuery(reinterpret_cast<void*>(base), 0, &prefix, sizeof(prefix)), OK);
	EXPECT_EQ(prefix.start, base);
	EXPECT_EQ(prefix.end, base + kPageSize);
	EXPECT_EQ(KernelVirtualQuery(reinterpret_cast<void*>(base + 2 * kPageSize), 0, &suffix, sizeof(suffix)), OK);
	EXPECT_EQ(suffix.start, base + 2 * kPageSize);
	EXPECT_EQ(suffix.end, base + kSize);

	EXPECT_EQ(KernelMunmap(base, kPageSize), OK);
	EXPECT_EQ(KernelMunmap(base + 2 * kPageSize, kSize - 2 * kPageSize), OK);
}

TEST(EmulatorKernelMemory, FixedDirectMapCommitsOwnedReservation)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t kSize = 0x10000;
	void*            address = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&address, kSize, 0, kSize), OK);

	int64_t physical_address = 0;
	ASSERT_EQ(KernelAllocateMainDirectMemory(kSize, kSize, 12, &physical_address), OK);
	ASSERT_EQ(KernelMapDirectMemory(&address, kSize, 0x02, 0x10, physical_address, kSize), OK);

	VirtualQueryInfo info {};
	ASSERT_EQ(KernelVirtualQuery(address, 0, &info, sizeof(info)), OK);
	EXPECT_EQ(info.start, reinterpret_cast<uintptr_t>(address));
	EXPECT_EQ(info.end, reinterpret_cast<uintptr_t>(address) + kSize);
	EXPECT_EQ(info.is_direct, 1u);
	EXPECT_EQ(info.is_committed, 1u);

	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(address), kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, FixedDirectMapConsumesPrefixOfLargerReservation)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t kReservationSize = 0x40000;
	constexpr size_t kMappingSize     = 0x4000;
	void*            reservation      = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&reservation, kReservationSize, 0, kReservationSize), OK);
	const auto reservation_base = reinterpret_cast<uint64_t>(reservation);

	int64_t physical_address = 0;
	ASSERT_EQ(KernelAllocateMainDirectMemory(kMappingSize, 0, 12, &physical_address), OK);

	void*     mapping    = reservation;
	const int map_result = KernelMapDirectMemory(&mapping, kMappingSize, 0x02, 0x10, physical_address, 0x1000);
	EXPECT_EQ(map_result, OK);
	if (map_result == OK)
	{
		ASSERT_EQ(mapping, reservation);
		static_cast<uint8_t*>(mapping)[0] = 0x5a;
		EXPECT_FALSE(Core::VirtualMemory::TryDemandMap(reservation_base));
		EXPECT_EQ(static_cast<uint8_t*>(mapping)[0], 0x5a);

		VirtualQueryInfo mapped_info {};
		ASSERT_EQ(KernelVirtualQuery(mapping, 0, &mapped_info, sizeof(mapped_info)), OK);
		EXPECT_EQ(mapped_info.is_direct, 1u);
		EXPECT_EQ(mapped_info.is_committed, 1u);

		VirtualQueryInfo suffix_info {};
		ASSERT_EQ(KernelVirtualQuery(reinterpret_cast<void*>(reservation_base + kMappingSize), 0, &suffix_info,
		                             sizeof(suffix_info)),
		          OK);
		EXPECT_EQ(suffix_info.start, reservation_base + kMappingSize);
		EXPECT_EQ(suffix_info.end, reservation_base + kReservationSize);
		EXPECT_EQ(suffix_info.is_committed, 0u);
		EXPECT_TRUE(Core::VirtualMemory::TryDemandMap(reservation_base + kMappingSize));

		EXPECT_EQ(KernelMunmap(reservation_base, kMappingSize), OK);
		EXPECT_EQ(KernelMunmap(reservation_base + kMappingSize, kReservationSize - kMappingSize), OK);
	} else
	{
		EXPECT_EQ(KernelMunmap(reservation_base, kReservationSize), OK);
	}
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kMappingSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, ReservingDirectMappingTailDecommitsIt)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t kMappingSize = 0x40000;
	constexpr size_t kKeepSize    = 0x24000;
	constexpr size_t kTailSize    = kMappingSize - kKeepSize;
	void*            reservation  = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&reservation, kMappingSize, 0, 0x4000), OK);
	const auto base = reinterpret_cast<uint64_t>(reservation);

	int64_t physical_address = 0;
	ASSERT_EQ(KernelAllocateMainDirectMemory(kMappingSize, 0x4000, 12, &physical_address), OK);
	ASSERT_EQ(KernelMapDirectMemory(&reservation, kMappingSize, 0x02, 0x10, physical_address, 0x4000), OK);

	void* tail = reinterpret_cast<void*>(base + kKeepSize);
	ASSERT_EQ(KernelReserveVirtualRange(&tail, kTailSize, 0x400010, 0), OK);
	EXPECT_EQ(tail, reinterpret_cast<void*>(base + kKeepSize));

	VirtualQueryInfo prefix_info {};
	VirtualQueryInfo tail_info {};
	ASSERT_EQ(KernelVirtualQuery(reinterpret_cast<void*>(base), 0, &prefix_info, sizeof(prefix_info)), OK);
	ASSERT_EQ(KernelVirtualQuery(tail, 0, &tail_info, sizeof(tail_info)), OK);
	EXPECT_EQ(prefix_info.start, base);
	EXPECT_EQ(prefix_info.end, base + kKeepSize);
	EXPECT_EQ(prefix_info.is_direct, 1u);
	EXPECT_EQ(prefix_info.is_committed, 1u);
	EXPECT_EQ(tail_info.start, base + kKeepSize);
	EXPECT_EQ(tail_info.end, base + kMappingSize);
	EXPECT_EQ(tail_info.is_direct, 0u);
	EXPECT_EQ(tail_info.is_committed, 0u);

	ASSERT_EQ(KernelReleaseDirectMemory(physical_address + static_cast<int64_t>(kKeepSize), kTailSize), OK);
	EXPECT_EQ(KernelMunmap(base, kKeepSize), OK);
	EXPECT_EQ(KernelMunmap(base + kKeepSize, kTailSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kKeepSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, DirectMemoryAllocationFindsAFreeEarlierRange)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t  kSize      = 0x10000;
	constexpr int64_t kLowerBase = 0x08000000;
	constexpr int64_t kUpperBase = 0x10000000;
	int64_t           upper_addr = 0;
	int64_t           lower_addr = 0;

	ASSERT_EQ(KernelAllocateDirectMemory(kUpperBase, kUpperBase + kSize, kSize, kSize, 12, &upper_addr), OK);
	ASSERT_EQ(upper_addr, kUpperBase);
	ASSERT_EQ(KernelAllocateDirectMemory(kLowerBase, kLowerBase + kSize, kSize, kSize, 12, &lower_addr), OK);
	EXPECT_EQ(lower_addr, kLowerBase);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(lower_addr, kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(upper_addr, kSize), OK);
}

TEST(EmulatorKernelMemory, DirectMemoryAllocationReusesAlignedGapInsideSearchWindow)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t  kSize = 0x10000;
	constexpr int64_t kBase = 0x330000000;
	int64_t           first = 0;
	int64_t           middle = 0;
	int64_t           last = 0;
	int64_t           replacement = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, kSize, kSize, 12, &first), OK);
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, kSize, kSize, 12, &middle), OK);
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, kSize, kSize, 12, &last), OK);
	ASSERT_EQ(first, kBase);
	ASSERT_EQ(middle, kBase + static_cast<int64_t>(kSize));
	ASSERT_EQ(last, kBase + 2 * static_cast<int64_t>(kSize));

	ASSERT_EQ(KernelCheckedReleaseDirectMemory(middle, kSize), OK);
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, kSize, kSize, 12, &replacement), OK);
	EXPECT_EQ(replacement, middle);

	EXPECT_EQ(KernelCheckedReleaseDirectMemory(replacement, kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(last, kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(first, kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, DirectMemoryReleaseSplitsAnAllocatedRange)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t  kSize = 0x4000;
	constexpr int64_t kBase = 0x350000000;
	int64_t           allocation = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, 3 * kSize, kSize, 12, &allocation), OK);
	ASSERT_EQ(allocation, kBase);

	const int64_t middle = allocation + static_cast<int64_t>(kSize);
	ASSERT_EQ(KernelCheckedReleaseDirectMemory(middle, kSize), OK);

	int64_t replacement = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, kSize, kSize, 12, &replacement), OK);
	EXPECT_EQ(replacement, middle);

	EXPECT_EQ(KernelCheckedReleaseDirectMemory(allocation, kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(replacement, kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(allocation + 2 * static_cast<int64_t>(kSize), kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, DirectMemoryReleaseAcceptsAContiguousAllocationSpan)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t  kSize = 0x4000;
	constexpr int64_t kBase = 0x360000000;
	int64_t           first = 0;
	int64_t           second = 0;
	int64_t           third = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, kSize, kSize, 12, &first), OK);
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, kSize, kSize, 12, &second), OK);
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, kSize, kSize, 12, &third), OK);
	ASSERT_EQ(first, kBase);
	ASSERT_EQ(second, kBase + static_cast<int64_t>(kSize));
	ASSERT_EQ(third, kBase + 2 * static_cast<int64_t>(kSize));

	ASSERT_EQ(KernelCheckedReleaseDirectMemory(first, 3 * kSize), OK);

	int64_t coalesced = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 3 * kSize, 3 * kSize, kSize, 12, &coalesced), OK);
	EXPECT_EQ(coalesced, first);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(coalesced, 3 * kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, DirectMemoryQueryCoalescesAdjacentAllocationsOfTheSameType)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr int64_t kBase = 0x300000000;
	constexpr size_t  kSize = 0x4000;
	int64_t           first = 0;
	int64_t           second = 0;

	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 2 * kSize, kSize, kSize, 12, &first), OK);
	ASSERT_EQ(KernelAllocateDirectMemory(kBase, kBase + 2 * kSize, kSize, kSize, 12, &second), OK);
	ASSERT_EQ(first, kBase);
	ASSERT_EQ(second, kBase + static_cast<int64_t>(kSize));

	struct DirectMemoryInfo
	{
		int64_t start;
		int64_t end;
		int     memory_type;
	};
	DirectMemoryInfo info {};

	ASSERT_EQ(KernelDirectMemoryQuery(first, 1, &info, sizeof(info)), OK);
	EXPECT_EQ(info.start, first);
	EXPECT_EQ(info.end, first + 2 * static_cast<int64_t>(kSize));
	EXPECT_EQ(info.memory_type, 12);

	EXPECT_EQ(KernelCheckedReleaseDirectMemory(first, kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(second, kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, ReleaseDirectMemoryKeepsVirtualMappingUntilMunmap)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t kSize = 0x10000;
	int64_t          physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(0x40000, 0x80000, kSize, kSize, 12, &physical_address), OK);

	void* mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&mapping, kSize, 0x02, 0, physical_address, kSize), OK);
	ASSERT_NE(mapping, nullptr);

	void* mapping_start = nullptr;
	void* mapping_end   = nullptr;
	int   protection    = 0;
	ASSERT_EQ(KernelQueryMemoryProtection(mapping, &mapping_start, &mapping_end, &protection), OK);
	EXPECT_EQ(mapping_start, mapping);
	EXPECT_EQ(mapping_end, static_cast<uint8_t*>(mapping) + kSize - 1);
	EXPECT_EQ(protection, 0x02);

	ASSERT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kSize), OK);

	struct DirectMemoryInfo
	{
		int64_t start;
		int64_t end;
		int     memory_type;
	};
	DirectMemoryInfo info {};
	EXPECT_EQ(KernelDirectMemoryQuery(physical_address, 0, &info, sizeof(info)), LibKernel::KERNEL_ERROR_EACCES);

	mapping_start = nullptr;
	mapping_end   = nullptr;
	protection    = 0;
	ASSERT_EQ(KernelQueryMemoryProtection(mapping, &mapping_start, &mapping_end, &protection), OK);
	EXPECT_EQ(mapping_start, mapping);
	EXPECT_EQ(mapping_end, static_cast<uint8_t*>(mapping) + kSize - 1);
	EXPECT_EQ(protection, 0x02);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(mapping), kSize), OK);
	EXPECT_EQ(KernelQueryMemoryProtection(mapping, nullptr, nullptr, nullptr), LibKernel::KERNEL_ERROR_EACCES);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, MemorySnapshotTracksReleasedDirectMappingsUntilMunmap)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t  kSize        = 0x10000;
	constexpr int64_t kSearchStart = 0x310000000;
	const auto        before       = KernelGetMemorySnapshot();

	int64_t physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kSearchStart, kSearchStart + kSize, kSize, kSize, 12, &physical_address), OK);
	const auto after_allocate = KernelGetMemorySnapshot();
	EXPECT_EQ(after_allocate.direct_allocated_bytes, before.direct_allocated_bytes + kSize);
	EXPECT_EQ(after_allocate.direct_allocation_count, before.direct_allocation_count + 1);
	EXPECT_EQ(after_allocate.direct_mapped_bytes, before.direct_mapped_bytes);

	void* mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&mapping, kSize, 0x02, 0, physical_address, kSize), OK);
	const auto after_map = KernelGetMemorySnapshot();
	EXPECT_EQ(after_map.direct_mapped_bytes, before.direct_mapped_bytes + kSize);
	EXPECT_EQ(after_map.direct_mapping_count, before.direct_mapping_count + 1);
	EXPECT_EQ(after_map.direct_released_mapped_bytes, before.direct_released_mapped_bytes);

	ASSERT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kSize), OK);
	const auto after_release = KernelGetMemorySnapshot();
	EXPECT_EQ(after_release.direct_allocated_bytes, before.direct_allocated_bytes);
	EXPECT_EQ(after_release.direct_allocation_count, before.direct_allocation_count);
	EXPECT_EQ(after_release.direct_mapped_bytes, before.direct_mapped_bytes + kSize);
	EXPECT_EQ(after_release.direct_mapping_count, before.direct_mapping_count + 1);
	EXPECT_EQ(after_release.direct_released_mapped_bytes, before.direct_released_mapped_bytes + kSize);
	EXPECT_EQ(after_release.direct_released_mapping_count, before.direct_released_mapping_count + 1);

	ASSERT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(mapping), kSize), OK);
	const auto after_unmap = KernelGetMemorySnapshot();
	EXPECT_EQ(after_unmap.direct_mapped_bytes, before.direct_mapped_bytes);
	EXPECT_EQ(after_unmap.direct_mapping_count, before.direct_mapping_count);
	EXPECT_EQ(after_unmap.direct_released_mapped_bytes, before.direct_released_mapped_bytes);
	EXPECT_EQ(after_unmap.direct_released_mapping_count, before.direct_released_mapping_count);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, MemorySnapshotCountsAliasedPhysicalRangeOnce)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t  kSize        = 0x10000;
	constexpr int64_t kSearchStart = 0x320000000;
	const auto        before       = KernelGetMemorySnapshot();

	int64_t physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kSearchStart, kSearchStart + kSize, kSize, kSize, 12, &physical_address), OK);
	void* first_mapping = nullptr;
	void* second_mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&first_mapping, kSize, 0x02, 0, physical_address, kSize), OK);
	ASSERT_EQ(KernelMapDirectMemory(&second_mapping, kSize, 0x02, 0, physical_address, kSize), OK);

	const auto aliased = KernelGetMemorySnapshot();
	EXPECT_EQ(aliased.direct_mapped_bytes, before.direct_mapped_bytes + 2 * kSize);
	EXPECT_EQ(aliased.direct_unique_mapped_bytes, before.direct_unique_mapped_bytes + kSize);

	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(second_mapping), kSize), OK);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(first_mapping), kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, MemorySnapshotTracksFlexibleMappingLifetime)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t kSize  = 0x10000;
	const auto       before = KernelGetMemorySnapshot();
	void*            mapping = nullptr;
	ASSERT_EQ(KernelMapNamedFlexibleMemory(&mapping, kSize, 0x03, 0, "snapshot"), OK);

	const auto after_map = KernelGetMemorySnapshot();
	EXPECT_EQ(after_map.flexible_mapped_bytes, before.flexible_mapped_bytes + kSize);
	EXPECT_EQ(after_map.flexible_mapping_count, before.flexible_mapping_count + 1);

	ASSERT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(mapping), kSize), OK);
	const auto after_unmap = KernelGetMemorySnapshot();
	EXPECT_EQ(after_unmap.flexible_mapped_bytes, before.flexible_mapped_bytes);
	EXPECT_EQ(after_unmap.flexible_mapping_count, before.flexible_mapping_count);
}

TEST(EmulatorKernelMemory, FixedDirectMapReplacesOwnedMappingAfterPhysicalRelease)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t  kSize       = 0x4000;
	constexpr int64_t kFirstPhys  = 0x02000000;
	constexpr int64_t kSecondPhys = 0x03000000;

	int64_t first_physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kFirstPhys, kFirstPhys + kSize, kSize, kSize, 12, &first_physical_address), OK);

	void* mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&mapping, kSize, 0x02, 0, first_physical_address, kSize), OK);
	ASSERT_NE(mapping, nullptr);
	static_cast<uint8_t*>(mapping)[0] = 0x5a;
	ASSERT_EQ(KernelCheckedReleaseDirectMemory(first_physical_address, kSize), OK);
	EXPECT_EQ(KernelQueryMemoryProtection(mapping, nullptr, nullptr, nullptr), OK);

	int64_t second_physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kSecondPhys, kSecondPhys + kSize, kSize, kSize, 12, &second_physical_address), OK);

	void* remapped = mapping;
	ASSERT_EQ(KernelMapDirectMemory(&remapped, kSize, 0x02, 0x10, second_physical_address, kSize), OK);
	ASSERT_EQ(remapped, mapping);
	VirtualQueryInfo info {};
	ASSERT_EQ(KernelVirtualQuery(remapped, 0, &info, sizeof(info)), OK);
	EXPECT_EQ(info.offset, static_cast<uint64_t>(second_physical_address));
	EXPECT_EQ(info.memory_type, 12);
	static_cast<uint8_t*>(remapped)[0] = 0xc3;
	EXPECT_EQ(static_cast<uint8_t*>(remapped)[0], 0xc3);

	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(remapped), kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(second_physical_address, kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, ReusedDirectMemoryStartsZeroedAndKeepsVirtualAliasesCoherent)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t  kSize        = 0x10000;
	constexpr int64_t kSearchStart = 0x100000;
	constexpr int64_t kSearchEnd   = kSearchStart + kSize;
	int64_t           first_physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kSearchStart, kSearchEnd, kSize, kSize, 12, &first_physical_address), OK);
	ASSERT_EQ(first_physical_address, kSearchStart);

	void* first_mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&first_mapping, kSize, 0x02, 0, first_physical_address, kSize), OK);
	ASSERT_NE(first_mapping, nullptr);
	auto* first_bytes = static_cast<uint8_t*>(first_mapping);
	first_bytes[0]    = 0x5a;
	first_bytes[1]    = 0xc3;

	ASSERT_EQ(KernelCheckedReleaseDirectMemory(first_physical_address, kSize), OK);

	int64_t second_physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kSearchStart, kSearchEnd, kSize, kSize, 12, &second_physical_address), OK);
	ASSERT_EQ(second_physical_address, first_physical_address);

	EXPECT_EQ(KernelQueryMemoryProtection(first_mapping, nullptr, nullptr, nullptr), OK);
	EXPECT_EQ(first_bytes[0], 0u);
	EXPECT_EQ(first_bytes[1], 0u);

	void* second_mapping = nullptr;
	const int remap_result = KernelMapDirectMemory(&second_mapping, kSize, 0x07, 0, second_physical_address, kSize);
	#if defined(__APPLE__)
	// macOS can reject an executable writable MAP_SHARED view even when the
	// requested address is free. Keep the host policy explicit.
	if (remap_result == LibKernel::KERNEL_ERROR_EBUSY)
	{
		ASSERT_EQ(KernelCheckedReleaseDirectMemory(second_physical_address, kSize), OK);
		GTEST_SKIP() << "macOS rejected the shared ExecuteReadWrite remap";
	}
	#endif
	ASSERT_EQ(remap_result, OK);
	ASSERT_NE(second_mapping, nullptr);
	ASSERT_NE(second_mapping, first_mapping);
	auto* second_bytes = static_cast<uint8_t*>(second_mapping);
	EXPECT_EQ(second_bytes[0], 0u);
	EXPECT_EQ(second_bytes[1], 0u);
	second_bytes[2] = 0x7e;
	EXPECT_EQ(first_bytes[2], 0x7e);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(second_mapping), kSize), OK);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(first_mapping), kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(second_physical_address, kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, FixedDirectMemoryRemapsFreedReadWriteView)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t  kSize        = 0x10000;
	constexpr int64_t kSearchStart = 0x01800000;
	int64_t           physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kSearchStart, kSearchStart + kSize, kSize, kSize, 12, &physical_address), OK);

	void* first_mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&first_mapping, kSize, 0x02, 0, physical_address, kSize), OK);
	ASSERT_NE(first_mapping, nullptr);
	ASSERT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(first_mapping), kSize), OK);

	void* remapped = first_mapping;
	ASSERT_EQ(KernelMapDirectMemory(&remapped, kSize, 0x02, 0x10, physical_address, kSize), OK);
	EXPECT_EQ(remapped, first_mapping);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(remapped), kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kSize), OK);
}

TEST(EmulatorKernelMemory, InternalNamedFlexibleMemoryNidUsesOutPointerAbi)
{
	EnsureMemorySubsystemInitialized();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"4h6F1LLbTiw";
	query.library              = U"libkernel";
	query.library_version      = 1;
	query.module               = U"libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* rec            = symbols.Find(query);
	ASSERT_NE(rec, nullptr);

	static std::array<uint8_t, 0x4000> out_storage {};
	auto**                             out_addr = reinterpret_cast<void**>(out_storage.data());
	*out_addr                                  = nullptr;

	using map_named_flexible_internal_fn_t = int (*)(void**, size_t, int, int, const char*);
	auto* map_named_flexible_internal = reinterpret_cast<map_named_flexible_internal_fn_t>(static_cast<uintptr_t>(rec->vaddr));
	ASSERT_NE(map_named_flexible_internal, nullptr);

	constexpr size_t kSize = 0x4000;
	ASSERT_EQ(map_named_flexible_internal(out_addr, kSize, 0x03, 0x8000, "internal-test"), OK);
	ASSERT_NE(*out_addr, nullptr);

	void* start      = nullptr;
	void* end        = nullptr;
	int   protection = 0;
	EXPECT_EQ(KernelQueryMemoryProtection(*out_addr, &start, &end, &protection), OK);
	EXPECT_EQ(start, *out_addr);
	EXPECT_EQ(end, static_cast<uint8_t*>(*out_addr) + kSize - 1);
	EXPECT_EQ(protection, 0x03);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(*out_addr), kSize), OK);
}

TEST(EmulatorKernelMemory, GuestMemoryValidationReturnsKernelErrors)
{
	EnsureMemorySubsystemInitialized();

	void* address = nullptr;
	EXPECT_EQ(KernelMapNamedFlexibleMemory(nullptr, 0x4000, 0x03, 0, ""), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMapNamedFlexibleMemory(&address, 0, 0x03, 0, ""), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMapNamedFlexibleMemory(&address, 0x4000, 0x99, 0, ""), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMapNamedFlexibleMemory(&address, 0x4000, 0x03, 0x4000, ""), LibKernel::KERNEL_ERROR_EINVAL);

	EXPECT_EQ(KernelMunmap(0, 0x4000), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMunmap(std::numeric_limits<uint64_t>::max() - 0x1000, 0x2000), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMunmap(0x0000030000000000, 0x4000), LibKernel::KERNEL_ERROR_ENOENT);

	EXPECT_EQ(KernelQueryMemoryProtection(nullptr, nullptr, nullptr, nullptr), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelAvailableFlexibleMemorySize(nullptr), LibKernel::KERNEL_ERROR_EINVAL);

	EXPECT_EQ(KernelMapDirectMemory(nullptr, 0x4000, 0x03, 0, 0, 0x4000), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMapDirectMemory(&address, 0x4000, 0x99, 0, 0, 0x4000), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMapDirectMemory(&address, 0x4000, 0x03, 0, 0x70000000, 0x4000),
	          LibKernel::KERNEL_ERROR_EACCES);

	EXPECT_EQ(KernelMprotect(nullptr, 0x4000, 0x03), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMprotect(reinterpret_cast<void*>(0x0000030000000000), 0, 0x03), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMprotect(reinterpret_cast<void*>(0x0000030000000000), 0x4000, 0x99),
	          LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelMprotect(reinterpret_cast<void*>(0x0000030000000000), 0x4000, 0x03),
	          LibKernel::KERNEL_ERROR_ENOENT);
}

TEST(EmulatorKernelMemory, MprotectRejectsUnmanagedHostMapping)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX || defined(__APPLE__)
	GTEST_SKIP() << "raw mmap ownership regression is Linux-specific";
#else
	EnsureMemorySubsystemInitialized();

	constexpr size_t kGuestPage = 0x4000;
	auto* const external = static_cast<uint8_t*>(
	    ::mmap(nullptr, kGuestPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	ASSERT_NE(reinterpret_cast<void*>(external), MAP_FAILED);
	EXPECT_FALSE(Core::VirtualMemory::IsRangeGuestOwned(reinterpret_cast<uint64_t>(external), kGuestPage));
	EXPECT_EQ(KernelMprotect(external, kGuestPage, 0x03), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(::munmap(external, kGuestPage), 0);
#endif
}

TEST(EmulatorKernelMemory, FixedFlexibleMapPreservesReservationSuffix)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t kReservationSize = 0x10000;
	constexpr size_t kMappingSize     = 0x4000;
	void*            reservation      = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&reservation, kReservationSize, 0, kReservationSize), OK);
	const auto reservation_base = reinterpret_cast<uint64_t>(reservation);

	void* mapping = reservation;
	ASSERT_EQ(KernelMapNamedFlexibleMemory(&mapping, kMappingSize, 0x03, 0x10, "fixed-flexible"), OK);
	ASSERT_EQ(mapping, reservation);
	static_cast<uint8_t*>(mapping)[0] = 0xc3;
	EXPECT_FALSE(Core::VirtualMemory::TryDemandMap(reservation_base));
	EXPECT_EQ(static_cast<uint8_t*>(mapping)[0], 0xc3);

	VirtualQueryInfo mapped_info {};
	ASSERT_EQ(KernelVirtualQuery(mapping, 0, &mapped_info, sizeof(mapped_info)), OK);
	EXPECT_EQ(mapped_info.start, reservation_base);
	EXPECT_EQ(mapped_info.end, reservation_base + kMappingSize);
	EXPECT_EQ(mapped_info.protection, 0x03);
	EXPECT_EQ(mapped_info.is_flexible, 1u);
	EXPECT_EQ(mapped_info.is_committed, 1u);

	VirtualQueryInfo suffix_info {};
	ASSERT_EQ(KernelVirtualQuery(reinterpret_cast<void*>(reservation_base + kMappingSize), 0, &suffix_info,
	                             sizeof(suffix_info)),
	          OK);
	EXPECT_EQ(suffix_info.start, reservation_base + kMappingSize);
	EXPECT_EQ(suffix_info.end, reservation_base + kReservationSize);
	EXPECT_EQ(suffix_info.is_committed, 0u);
	EXPECT_TRUE(Core::VirtualMemory::TryDemandMap(reservation_base + kMappingSize));

	EXPECT_EQ(KernelMunmap(reservation_base, kMappingSize), OK);
	EXPECT_EQ(KernelMunmap(reservation_base + kMappingSize, kReservationSize - kMappingSize), OK);
}

TEST(EmulatorKernelMemory, MprotectUpdatesTrackedProtectionSlices)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t kSize = 0x10000;
	void*            mapping = nullptr;
	ASSERT_EQ(KernelMapNamedFlexibleMemory(&mapping, kSize, 0x03, 0, "protection-slices"), OK);
	ASSERT_NE(mapping, nullptr);
	const auto base = reinterpret_cast<uint64_t>(mapping);

	ASSERT_EQ(KernelMprotect(reinterpret_cast<void*>(base + 0x4000), 0x4000, 0x01), OK);

	void* start = nullptr;
	void* end   = nullptr;
	int   protection = 0;
	ASSERT_EQ(KernelQueryMemoryProtection(mapping, &start, &end, &protection), OK);
	EXPECT_EQ(start, mapping);
	EXPECT_EQ(end, reinterpret_cast<void*>(base + 0x3fff));
	EXPECT_EQ(protection, 0x03);

	ASSERT_EQ(KernelQueryMemoryProtection(reinterpret_cast<void*>(base + 0x4000), &start, &end, &protection), OK);
	EXPECT_EQ(start, reinterpret_cast<void*>(base + 0x4000));
	EXPECT_EQ(end, reinterpret_cast<void*>(base + 0x7fff));
	EXPECT_EQ(protection, 0x01);

	ASSERT_EQ(KernelQueryMemoryProtection(reinterpret_cast<void*>(base + 0x8000), &start, &end, &protection), OK);
	EXPECT_EQ(start, reinterpret_cast<void*>(base + 0x8000));
	EXPECT_EQ(end, reinterpret_cast<void*>(base + kSize - 1));
	EXPECT_EQ(protection, 0x03);

	EXPECT_EQ(KernelMunmap(base, kSize), OK);
}

// Covers the explicit Gen5 protection family observed in one allocation path.
// The pure decoder is the shipped decision path used by KernelMprotect.
TEST(EmulatorKernelMemory, DecodesGen5MprotectProtectionFamily)
{
	Core::VirtualMemory::Mode       mode {};
	KernelGpuMappingAccessMode      gpu {};

	ASSERT_TRUE(KernelDecodeMprotectProt(0x0, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::NoAccess);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::NoAccess);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x03, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::NoAccess);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x11, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::Read);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::Read);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x12, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::Read);

	ASSERT_TRUE(KernelDecodeMprotectProt(0xC2, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::ReadWrite);

	ASSERT_TRUE(KernelDecodeMprotectProt(0xF3, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::ReadWrite);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x3F2, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::ReadWrite);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x3F3, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::ReadWrite);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x42, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::Read);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x82, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, KernelGpuMappingAccessMode::Write);

	EXPECT_FALSE(KernelDecodeMprotectProt(0x99, &mode, &gpu));
}

TEST(EmulatorKernelMemory, V8ReleasePagesMprotectTail)
{
	EnsureMemorySubsystemInitialized();

	// Reproduce V8 BoundedPageAllocator::ReleasePages tail SetPermissions(NoAccess)
	//  base 0x1000000000 size 0x40000 new_size 0x24000 tail 0x1c000
	constexpr size_t kReservationSize = 0x40000;
	constexpr size_t kKeepSize        = 0x24000;
	constexpr size_t kTailSize        = kReservationSize - kKeepSize;
	void* reservation = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&reservation, kReservationSize, 0, 0x4000), OK);
	ASSERT_NE(reservation, nullptr);
	const auto base = reinterpret_cast<uint64_t>(reservation);

	// Simulate V8 allocating the reservation as flexible RW (typical for page allocator)
	void* mapping = reservation;
	ASSERT_EQ(KernelMapNamedFlexibleMemory(&mapping, kReservationSize, 0x03, 0x10, "v8-release-test"), OK);
	ASSERT_EQ(mapping, reservation);

	// Tail should be releasable via mprotect NoAccess (0x0)
	const auto tail = base + kKeepSize;
	ASSERT_EQ(KernelMprotect(reinterpret_cast<void*>(tail), kTailSize, 0x00), OK);

	// Verify protection slices
	GuestWritableBlock start_storage(sizeof(void*));
	GuestWritableBlock end_storage(sizeof(void*));
	GuestWritableBlock prot_storage(sizeof(int));
	ASSERT_TRUE(start_storage.IsValid());
	ASSERT_TRUE(end_storage.IsValid());
	ASSERT_TRUE(prot_storage.IsValid());
	ASSERT_EQ(KernelQueryMemoryProtection(reinterpret_cast<void*>(base), start_storage.Data<void*>(), end_storage.Data<void*>(),
	                                      prot_storage.Data<int>()),
	          OK);
	EXPECT_EQ(*prot_storage.Data<int>(), 0x03);
	ASSERT_EQ(KernelQueryMemoryProtection(reinterpret_cast<void*>(tail), start_storage.Data<void*>(), end_storage.Data<void*>(),
	                                      prot_storage.Data<int>()),
	          OK);
	EXPECT_EQ(*prot_storage.Data<int>(), 0x00);

	// Cleanup: unmap keep + tail Opt: tail is NoAccess but still part of flexible mapping, so whole unmap should work via Munmap?
	// Our current Flexible unmap requires exact size, so unmap whole reservation via Munmap? It will be Consume for reserved? For flexible, we need exact.
	EXPECT_EQ(KernelMunmap(base, kReservationSize), OK);
}

TEST(EmulatorKernelMemory, GpuVisibleMprotectMarksContainingMappingUntilUnmap)
{
	constexpr uint64_t       mapping_base = 0x100000u;
	constexpr uint64_t       mapping_size = 0x10000u;
	KernelGpuMappingAccessMode cleanup_mode = KernelGpuMappingAccessMode::NoAccess;

	for (const auto requested:
	     {KernelGpuMappingAccessMode::Read, KernelGpuMappingAccessMode::Write, KernelGpuMappingAccessMode::ReadWrite})
	{
		auto fresh_mode = KernelGpuMappingAccessMode::NoAccess;
		EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base + 0x1000u, 0x1000u, requested,
		                                      &fresh_mode),
		          KernelGpuMappingPromotionStatus::Promoted);
		EXPECT_EQ(fresh_mode, requested);
	}

	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base + 0x2000u, 0x3000u,
	                                      KernelGpuMappingAccessMode::Read, &cleanup_mode),
	          KernelGpuMappingPromotionStatus::Promoted);
	EXPECT_EQ(cleanup_mode, KernelGpuMappingAccessMode::Read);

	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base + 0x2000u, 0x3000u,
	                                      KernelGpuMappingAccessMode::NoAccess, &cleanup_mode),
	          KernelGpuMappingPromotionStatus::Retained);
	EXPECT_EQ(cleanup_mode, KernelGpuMappingAccessMode::Read);

	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base + 0xf000u, 0x2000u,
	                                      KernelGpuMappingAccessMode::Write, &cleanup_mode),
	          KernelGpuMappingPromotionStatus::NotContained);
	EXPECT_EQ(cleanup_mode, KernelGpuMappingAccessMode::Read);
	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base - 1u, 1u,
	                                      KernelGpuMappingAccessMode::Write, &cleanup_mode),
	          KernelGpuMappingPromotionStatus::NotContained);
	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base, 0u, KernelGpuMappingAccessMode::Write,
	                                      &cleanup_mode),
	          KernelGpuMappingPromotionStatus::InvalidArgument);
	EXPECT_EQ(KernelPromoteGpuMappingRange(UINT64_MAX - 3u, 8u, mapping_base, 4u, KernelGpuMappingAccessMode::Write,
	                                      &cleanup_mode),
	          KernelGpuMappingPromotionStatus::InvalidArgument);

	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::Promoted),
	          KernelGpuMappingRegistrationAction::RegisterOwnerMapping);
	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::Retained),
	          KernelGpuMappingRegistrationAction::Retain);
	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::NotContained),
	          KernelGpuMappingRegistrationAction::RegisterProtectedRange);
	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::InvalidArgument),
	          KernelGpuMappingRegistrationAction::Reject);
	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::UnmapPending),
	          KernelGpuMappingRegistrationAction::Reject);
}

// Share_v1 NIDs from second-title first strict fail must resolve after InitShare_1.
TEST(EmulatorKernelMemory, ResolvesShareV1ExportsForGen5Boot)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libShare_1", &symbols));

	const char* nids[] = {"nBDD66kiFW8", "5wjxESwX68I", "T64o-315wbg", "YBiIdcDPrxs", "7QZtURYnXG4"};
	for (const char* nid: nids)
	{
		Loader::SymbolResolve query {};
		query.name                 = String::FromUtf8(nid);
		query.library              = U"Share";
		query.library_version      = 1;
		query.module               = U"Share";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		ASSERT_NE(symbols.Find(query), nullptr) << nid;
	}
}

// AMPR measure APIs match the compact command-stream records consumed at submit.
TEST(EmulatorKernelMemory, AmprMeasureCommandSizesMatchRecordLayout)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libAmpr_1", &symbols));

	struct Case
	{
		const char* nid;
		uint64_t    size;
	};
	const Case cases[] = {
	    {"vWU-odnS+fU", 0x14u},
	    {"sSAUCCU1dv4", 0x20u},
	    {"C+IEj+BsAFM", 0x20u},
	};

	for (const auto& c: cases)
	{
		Loader::SymbolResolve query {};
		query.name                 = String::FromUtf8(c.nid);
		query.library              = U"Ampr";
		query.library_version      = 1;
		query.module               = U"Ampr";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		const auto* rec            = symbols.Find(query);
		ASSERT_NE(rec, nullptr) << c.nid;
		using measure_fn_t = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
		auto* fn           = reinterpret_cast<measure_fn_t>(static_cast<uintptr_t>(rec->vaddr));
		ASSERT_NE(fn, nullptr);
		EXPECT_EQ(fn(0, 0x100000, 0x1000, 0, 0, 0), c.size) << c.nid;
	}

	Loader::SymbolResolve read_query {};
	read_query.name                 = U"vWU-odnS+fU";
	read_query.library              = U"Ampr";
	read_query.library_version      = 1;
	read_query.module               = U"Ampr";
	read_query.module_version_major = 1;
	read_query.module_version_minor = 1;
	read_query.type                 = Loader::SymbolType::Func;
	const auto* read_rec            = symbols.Find(read_query);
	ASSERT_NE(read_rec, nullptr);
	using read_measure_fn_t = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);
	auto* read_measure      = reinterpret_cast<read_measure_fn_t>(static_cast<uintptr_t>(read_rec->vaddr));
	EXPECT_EQ(read_measure(0, 0x100000, 0x1000, UINT64_C(0x100000000)), 0x18u);
}

TEST(EmulatorKernelMemory, AmprCommandBufferLifecycleUsesCompactHeader)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libAmpr_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"8aI7R7WaOlc";
	query.library              = U"Ampr";
	query.library_version      = 1;
	query.module               = U"Ampr";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* ctor_rec       = symbols.Find(query);
	ASSERT_NE(ctor_rec, nullptr);
	query.name                 = U"N-FSPA4S3nI";
	const auto* set_buffer_rec = symbols.Find(query);
	ASSERT_NE(set_buffer_rec, nullptr);

	using ctor_fn_t       = int (*)(void*);
	using set_buffer_fn_t = int (*)(void*, void*, uint32_t);
	auto* ctor            = reinterpret_cast<ctor_fn_t>(static_cast<uintptr_t>(ctor_rec->vaddr));
	auto* set_buffer      = reinterpret_cast<set_buffer_fn_t>(static_cast<uintptr_t>(set_buffer_rec->vaddr));

	alignas(8) uint8_t cmd_mem[0x28] {};
	alignas(8) uint8_t data_mem[64] {};
	std::memset(cmd_mem, 0xff, sizeof(cmd_mem));
	EXPECT_EQ(ctor(cmd_mem), OK);
	for (uint32_t i = 0; i < 0x18; ++i)
	{
		EXPECT_EQ(cmd_mem[i], 0u);
	}
	EXPECT_EQ(set_buffer(cmd_mem, data_mem, sizeof(data_mem)), OK);
	uint32_t type = 1;
	uint32_t offset = 1;
	int32_t  count = 1;
	uint32_t size = 0;
	uint64_t data = 0;
	std::memcpy(&type, cmd_mem + 0x00, sizeof(type));
	std::memcpy(&offset, cmd_mem + 0x04, sizeof(offset));
	std::memcpy(&count, cmd_mem + 0x08, sizeof(count));
	std::memcpy(&size, cmd_mem + 0x0c, sizeof(size));
	std::memcpy(&data, cmd_mem + 0x10, sizeof(data));
	EXPECT_EQ(type, 0u);
	EXPECT_EQ(offset, 0u);
	EXPECT_EQ(count, 0);
	EXPECT_EQ(data, reinterpret_cast<uint64_t>(data_mem));
	EXPECT_EQ(size, 64u);
}

// libc vsnprintf NID Q2V+iqvjgC0 resolves on libc_1 / libc_internal_1.
TEST(EmulatorKernelMemory, ResolvesLibcVsnprintfExport)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"Q2V+iqvjgC0";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	ASSERT_NE(symbols.Find(query), nullptr);
}

// libkernel_1: sceKernelNanosleep must resolve to the existing validated
// KernelNanosleep implementation rather than the generic missing-symbol path.
TEST(EmulatorKernelMemory, ResolvesKernelNanosleepExport)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"QvsZxomvUHs";
	query.library              = U"libkernel";
	query.library_version      = 1;
	query.module               = U"libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* rec = symbols.Find(query);
	ASSERT_NE(rec, nullptr);

	using nanosleep_fn_t = int (*)(const Kernel::KernelTimespec*, Kernel::KernelTimespec*);
	auto* nanosleep      = reinterpret_cast<nanosleep_fn_t>(static_cast<uintptr_t>(rec->vaddr));
	ASSERT_NE(nanosleep, nullptr);
	EXPECT_EQ(nanosleep(nullptr, nullptr), LibKernel::KERNEL_ERROR_EFAULT);
	Kernel::KernelTimespec invalid {-1, 0};
	EXPECT_EQ(nanosleep(&invalid, nullptr), LibKernel::KERNEL_ERROR_EINVAL);
}

// Gen5 AudioOut2_v1 / AudioOut_v1.1: core context lifecycle exports resolve,
// but unmeasured context layouts reject without writing guest storage.
TEST(EmulatorKernelMemory, ResolvesAudioOut2ContextLifecycle)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libAudio_1", &symbols));

	const char* nids[] = {
	    "g2tViFIohHE", // Initialize
	    "t5YrizufpQc", // ContextResetParam
	    "pDmme7Bgm6E", // ContextQueryMemory
	    "0x6o1VVAYSY", // ContextCreate
	    "on6ZH7Abo10", // ContextDestroy
	    "JK2wamZPzwM", // PortCreate
	    "xywYcRB7nbQ", // UserCreate
	};
	for (const char* nid: nids)
	{
		Loader::SymbolResolve query {};
		query.name                 = String::FromUtf8(nid);
		query.library              = U"AudioOut2";
		query.library_version      = 1;
		query.module               = U"AudioOut";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		EXPECT_NE(symbols.Find(query), nullptr) << nid;
	}

	Loader::SymbolResolve init_q {};
	init_q.name                 = U"g2tViFIohHE";
	init_q.library              = U"AudioOut2";
	init_q.library_version      = 1;
	init_q.module               = U"AudioOut";
	init_q.module_version_major = 1;
	init_q.module_version_minor = 1;
	init_q.type                 = Loader::SymbolType::Func;
	const auto* init_rec        = symbols.Find(init_q);
	ASSERT_NE(init_rec, nullptr);
	using init_fn_t = int (*)();
	EXPECT_EQ(reinterpret_cast<init_fn_t>(static_cast<uintptr_t>(init_rec->vaddr))(), 0);

	Loader::SymbolResolve qmem_q = init_q;
	qmem_q.name                  = U"pDmme7Bgm6E";
	const auto* qmem_rec         = symbols.Find(qmem_q);
	ASSERT_NE(qmem_rec, nullptr);
	using qmem_fn_t = int (*)(const void*, uint64_t*);
	GuestWritableBlock size_block(sizeof(uint64_t));
	ASSERT_TRUE(size_block.IsValid());
	auto* size = size_block.Data<uint64_t>();
	*size      = 0x1122334455667788ull;
	EXPECT_EQ(reinterpret_cast<qmem_fn_t>(static_cast<uintptr_t>(qmem_rec->vaddr))(nullptr, size),
	          LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(*size, 0x1122334455667788ull);

	Loader::SymbolResolve create_q = init_q;
	create_q.name                  = U"0x6o1VVAYSY";
	const auto* create_rec         = symbols.Find(create_q);
	ASSERT_NE(create_rec, nullptr);
	using create_fn_t = int (*)(const void*, void*, uint64_t, int32_t*);
	GuestWritableBlock workspace(64);
	GuestWritableBlock handle_block(sizeof(int32_t));
	ASSERT_TRUE(workspace.IsValid());
	ASSERT_TRUE(handle_block.IsValid());
	std::memset(workspace.Data<void>(), 0x5a, 64);
	auto* handle = handle_block.Data<int32_t>();
	*handle      = 0x12345678;
	EXPECT_EQ(reinterpret_cast<create_fn_t>(static_cast<uintptr_t>(create_rec->vaddr))(
	              nullptr, workspace.Data<void>(), 64, handle),
	          LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(*handle, 0x12345678);
}

// Residual Ampr NIDs from second-title boot resolve under libAmpr_1.
TEST(EmulatorKernelMemory, ResolvesAmprResidualBootNids)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libAmpr_1", &symbols));

	const char* nids[] = {"Zi3dBUjgyXI", "4muPEJ-x5N8", "qesF88X4DRg", "8aI7R7WaOlc", "GuchCTefuZw",
	                      "0BMj1hgG+kE", "NNIZ-FMyz3M", "VGkEj4d6-Kg", "Eul7AGEpjLo", "X169CE6G3Y4",
	                      "RPCAhx-aabE", "tNn5WBkta60", "mZSbNJVJpV8"};
	for (const char* nid: nids)
	{
		Loader::SymbolResolve query {};
		query.name                 = String::FromUtf8(nid);
		query.library              = U"Ampr";
		query.library_version      = 1;
		query.module               = U"Ampr";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		EXPECT_NE(symbols.Find(query), nullptr) << nid;
	}
}

TEST(EmulatorKernelMemory, ResolvesLibcSincosfWithFloatResults)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"pztV4AF18iI";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* record         = symbols.Find(query);
	ASSERT_NE(record, nullptr);

	using sincosf_fn_t = void (*)(float, float*, float*);
	float sine          = 0.0f;
	float cosine        = 0.0f;
	reinterpret_cast<sincosf_fn_t>(static_cast<uintptr_t>(record->vaddr))(0.0f, &sine, &cosine);
	EXPECT_FLOAT_EQ(sine, 0.0f);
	EXPECT_FLOAT_EQ(cosine, 1.0f);
}

TEST(EmulatorKernelMemory, ResolvesLibcExpfWithFloatResult)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"8zsu04XNsZ4";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* record         = symbols.Find(query);
	ASSERT_NE(record, nullptr);

	using expf_fn_t = float (*)(float);
	EXPECT_FLOAT_EQ(reinterpret_cast<expf_fn_t>(static_cast<uintptr_t>(record->vaddr))(0.0f), 1.0f);
}

TEST(EmulatorKernelMemory, CondWaitDiagnosticsStayInactiveWithoutOptIn)
{
	Kernel::PthreadCondWaitDiagnostics diagnostics {};
	EXPECT_FALSE(Kernel::PthreadGetCondWaitDiagnostics(&diagnostics));
	EXPECT_FALSE(diagnostics.enabled);
	EXPECT_EQ(diagnostics.blocked_count, 0u);
	EXPECT_EQ(diagnostics.blocked[0].signal_count, 0u);
}

TEST(EmulatorKernelMemory, ThreadDiagnosticsAreUnavailableWithoutPthreadContext)
{
	Kernel::PthreadThreadDiagnostics diagnostics {};

	EXPECT_FALSE(Kernel::PthreadGetThreadDiagnostics(&diagnostics));
	EXPECT_FALSE(diagnostics.available);
	EXPECT_EQ(diagnostics.allocated_count, 0u);
	EXPECT_EQ(diagnostics.thread_count, 0u);
}

TEST(EmulatorKernelMemory, SyncOnAddressReturnsImmediatelyWhenValueDiffers)
{
	using namespace Kernel::SyncOnAddress;
	EnsureMemorySubsystemInitialized();

	uint32_t value   = 1;
	uint32_t timeout = 0;
	EXPECT_EQ(KernelSyncOnAddressWait(reinterpret_cast<uint64_t>(&value), 0, &timeout, 0), OK);
}

TEST(EmulatorKernelMemory, SyncOnAddressReturnsKernelTimeoutWhenValueDoesNotChange)
{
	using namespace Kernel::SyncOnAddress;
	EnsureMemorySubsystemInitialized();

	uint32_t value   = 0;
	uint32_t timeout = 0;
	EXPECT_EQ(KernelSyncOnAddressWait(reinterpret_cast<uint64_t>(&value), 0, &timeout, 0), LibKernel::KERNEL_ERROR_ETIMEDOUT);
}

TEST(EmulatorKernelMemory, SyncOnAddressRejectsUnsupportedFlagsAndUnalignedAddress)
{
	using namespace Kernel::SyncOnAddress;
	EnsureMemorySubsystemInitialized();

	uint32_t value   = 0;
	uint32_t timeout = 0;
	EXPECT_EQ(KernelSyncOnAddressWait(reinterpret_cast<uint64_t>(&value), 0, &timeout, 1), LibKernel::KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelSyncOnAddressWait(reinterpret_cast<uint64_t>(&value) + 1u, 0, &timeout, 0), LibKernel::KERNEL_ERROR_EINVAL);
}

TEST(EmulatorKernelMemory, SyncOnAddressWakeReleasesMatchingWaiter)
{
	using namespace Kernel::SyncOnAddress;
	EnsureMemorySubsystemInitialized();

	uint32_t        value   = 0;
	uint32_t        timeout = 1000000;
	std::atomic_bool waiting = false;
	int              result  = -1;
	std::thread waiter([&]
	                   {
		                   waiting = true;
		                   result = KernelSyncOnAddressWait(reinterpret_cast<uint64_t>(&value), 0, &timeout, 0);
	                   });
	while (!waiting.load())
	{
		std::this_thread::yield();
	}
	__atomic_store_n(&value, 1u, __ATOMIC_RELEASE);
	EXPECT_EQ(KernelSyncOnAddressWake(reinterpret_cast<uint64_t>(&value), 1), OK);
	waiter.join();
	EXPECT_EQ(result, OK);
}

TEST(EmulatorKernelMemory, SyncOnAddressZeroWakeCountReleasesAllMatchingWaiters)
{
	using namespace Kernel::SyncOnAddress;
	EnsureMemorySubsystemInitialized();

	alignas(uint32_t) uint32_t value   = 0;
	const uint32_t             timeout = 500000;
	std::atomic_int            ready {0};
	std::atomic_int            first_result {INT32_MIN};
	std::atomic_int            second_result {INT32_MIN};

	auto wait = [&](std::atomic_int* result)
	{
		ready.fetch_add(1, std::memory_order_release);
		result->store(KernelSyncOnAddressWait(reinterpret_cast<uint64_t>(&value), 0, &timeout, 0), std::memory_order_release);
	};

	std::thread first(wait, &first_result);
	std::thread second(wait, &second_result);
	while (ready.load(std::memory_order_acquire) != 2)
	{
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	EXPECT_EQ(KernelSyncOnAddressWake(reinterpret_cast<uint64_t>(&value), 0), OK);
	first.join();
	second.join();

	EXPECT_EQ(first_result.load(std::memory_order_acquire), OK);
	EXPECT_EQ(second_result.load(std::memory_order_acquire), OK);
}

// Live EventFlag registry: Wait/Set/Delete on garbage handles must return
// ESRCH without dereferencing (Linux VibrationTrackThread poison pointer).
// Create/Wait/Set/Delete on a real flag exercises the shipped registry path.
TEST(EmulatorKernelMemory, EventFlagRejectsUnregisteredHandles)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	static bool threads_inited = false;
	if (!threads_inited)
	{
		Core::ThreadsSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		threads_inited = true;
	}

	using namespace Kernel::EventFlag;

	// Poison pointer observed on Linux vibration wait before CreateEventFlag.
	auto* poison = reinterpret_cast<KernelEventFlag>(static_cast<uintptr_t>(0xcccccccc00007fffULL));
	EXPECT_EQ(KernelWaitEventFlag(poison, 1, 0x21, nullptr, nullptr), LibKernel::KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelSetEventFlag(poison, 1), LibKernel::KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelDeleteEventFlag(poison), LibKernel::KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelWaitEventFlag(nullptr, 1, 0x21, nullptr, nullptr), LibKernel::KERNEL_ERROR_ESRCH);

	KernelEventFlag ef = nullptr;
	ASSERT_EQ(KernelCreateEventFlag(&ef, "UnitTestThreadFlag", 0x10, 0, nullptr), OK);
	ASSERT_NE(ef, nullptr);

	// Timeout=0 poll-style wait on empty bits returns TimedOut path.
	Kernel::KernelUseconds zero = 0;
	EXPECT_EQ(KernelWaitEventFlag(ef, 1, 0x01, nullptr, &zero), LibKernel::KERNEL_ERROR_ETIMEDOUT);
	EXPECT_EQ(KernelSetEventFlag(ef, 1), OK);
	zero = 0;
	EXPECT_EQ(KernelWaitEventFlag(ef, 1, 0x21, nullptr, &zero), OK);
	EXPECT_EQ(KernelDeleteEventFlag(ef), OK);
	// After delete, same pointer is no longer live.
	EXPECT_EQ(KernelWaitEventFlag(ef, 1, 0x01, nullptr, nullptr), LibKernel::KERNEL_ERROR_ESRCH);
}

UT_END();
