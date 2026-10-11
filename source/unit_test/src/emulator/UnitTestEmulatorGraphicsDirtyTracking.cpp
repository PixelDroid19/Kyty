#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/GuestMemory.h"
#include "Emulator/Log.h"
#include "Emulator/VideoFrameMemory.h"

// Test-only private include: GpuMemoryCalcHash is declared in this internal header,
// not in the public GpuMemory.h. It adds no production test API; the hash-span probes
// need the exact function that owns the guard.
#include "../../../emulator/src/Graphics/Objects/GpuMemoryInternal.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

UT_BEGIN(EmulatorGraphicsDirtyTracking);

using Kyty::Core::VirtualMemory::CreateSharedBacking;
using Kyty::Core::VirtualMemory::DestroySharedBacking;
using Kyty::Core::VirtualMemory::Free;
using Kyty::Core::VirtualMemory::GetPageSize;
using Kyty::Core::VirtualMemory::MapSharedAligned;
using Kyty::Core::VirtualMemory::MapSharedFixed;
using Kyty::Core::VirtualMemory::Mode;
using Kyty::Core::VirtualMemory::Protect;
using Kyty::Libs::Graphics::GpuDirtyPageProtectionOps;
using Kyty::Libs::Graphics::GpuDirtyPageTableIndex;
using Kyty::Libs::Graphics::GpuDirtyPageTracker;
using Kyty::Libs::Graphics::GpuDirtyPageTrackerNotifyFaultHandlerInstalled;
using Kyty::Libs::Graphics::GpuDirtyProtectionState;
using Kyty::Libs::Graphics::GpuDirtyProtectionStateHandlesFault;
using Kyty::Libs::Graphics::GpuDirtyProtectionStateNeedsArmingRollback;
using Kyty::Libs::Graphics::GpuDirtyTrackingEnabledForProcess;
using Kyty::Libs::Graphics::GpuDirtyTrackingMode;
using Kyty::Libs::Graphics::GpuMemoryCalcHash;
using Kyty::Libs::Graphics::GpuMemoryCheckAccessViolation;
using Kyty::Libs::Graphics::GpuMemoryCreateObject;
using Kyty::Libs::Graphics::GpuMemoryFree;
using Kyty::Libs::Graphics::GpuMemoryInit;
using Kyty::Libs::Graphics::GpuMemoryIsHostGuestMallocRange;
using Kyty::Libs::Graphics::GpuMemoryNotifyHostWrite;
using Kyty::Libs::Graphics::GpuMemoryObjectType;
using Kyty::Libs::Graphics::GpuMemorySetAllocatedRange;
using Kyty::Libs::Graphics::GpuObject;
using Kyty::Libs::Graphics::GraphicContext;
using Kyty::Libs::Graphics::VulkanMemory;

namespace {

GpuDirtyPageTracker* g_host_write_tracker = nullptr;

struct HostWriteCallbacks
{
	bool installed = false;
	explicit HostWriteCallbacks(GpuDirtyPageTracker* tracker)
	{
		g_host_write_tracker = tracker;
		installed = Kyty::Emulator::VideoFrameMemory::InstallCallbacks({
		    [](uint64_t, size_t, uint32_t) {}, [](uint64_t) {},
		    [](uint64_t address, uint64_t size) { (void)g_host_write_tracker->NotifyWrite(address, size); },
		    [](uint64_t address, uint64_t size)
		    {
			    errno = ERANGE;
			    return g_host_write_tracker->BeginHostWrite(address, size);
		    },
		    [](uint64_t token)
		    {
			    g_host_write_tracker->EndHostWrite(token);
			    errno = ERANGE;
		    }});
	}
	~HostWriteCallbacks()
	{
		(void)Kyty::Emulator::VideoFrameMemory::InstallCallbacks({});
		g_host_write_tracker = nullptr;
	}
};

TEST(EmulatorGraphicsDirtyTracking, ArmingWindowIsHandledAsTrackerFault)
{
	EXPECT_FALSE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Writable));
	EXPECT_FALSE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Capturing));
	EXPECT_TRUE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Arming));
	EXPECT_TRUE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::ArmingRollback));
	EXPECT_TRUE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Armed));
	EXPECT_TRUE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Disarming));
	EXPECT_TRUE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::Writable));
	EXPECT_FALSE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::Capturing));
	EXPECT_FALSE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::Arming));
	EXPECT_TRUE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::ArmingRollback));
	EXPECT_FALSE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::Armed));
	EXPECT_FALSE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::Disarming));
	EXPECT_TRUE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::Retired));
}

struct Mapping
{
	uint64_t                                  size    = 0;
	uint64_t                                  address = 0;
	Kyty::Core::VirtualMemory::SharedBacking* backing = nullptr;
	explicit Mapping(uint64_t pages = 2u): size(GetPageSize() * pages), address(0), backing(CreateSharedBacking(size))
	{
		if (backing != nullptr)
		{
			address = MapSharedAligned(backing, 0, 0, size, Mode::ReadWrite, GetPageSize());
		}
	}
	~Mapping()
	{
		if (address != 0)
		{
			Free(address);
		}
		if (backing != nullptr)
		{
			DestroySharedBacking(backing);
		}
	}
};

struct ProtectionCall
{
	uintptr_t address = 0;
	size_t    size    = 0;
	Mode      mode    = Mode::NoAccess;
	uint32_t  token   = 0;
};

struct FakeProtection
{
	uintptr_t                   base      = 0;
	size_t                      page_size = 0;
	std::vector<Mode>           original_modes;
	std::vector<Mode>           current_modes;
	std::vector<uint32_t>       original_tokens;
	std::vector<uint32_t>       signal_safe_tokens;
	std::vector<ProtectionCall> calls;
	GpuDirtyPageTracker*        tracker                       = nullptr;
	uintptr_t                   fault_address                 = 0;
	bool                        fault_result                  = false;
	uintptr_t                   fault_before_capture_address  = 0;
	uintptr_t                   notify_before_capture_address = 0;
	uint32_t                    signal_safe_calls             = 0;
	std::atomic<bool>           fail_next_protect {false};
	std::atomic<bool>           block_next_protect {false};
	std::atomic<bool>           protect_entered {false};
	std::atomic<bool>           release_protect {false};
	std::atomic<bool>           protect_completed {false};
	std::atomic<bool>           block_signal_safe {false};
	std::atomic<bool>           signal_safe_entered {false};
	std::atomic<bool>           release_signal_safe {false};
	std::atomic<uint32_t>       fail_signal_safe_call {0};
	std::atomic<bool>           fail_next_restore {false};
	std::atomic<bool>           block_restore {false};
	std::atomic<bool>           restore_entered {false};
	std::atomic<bool>           release_restore {false};
	uint32_t                    fail_capture_after_runs = 0;

	FakeProtection(uintptr_t address, size_t size, std::vector<Mode> modes)
	    : base(address), page_size(size), original_modes(std::move(modes)), current_modes(original_modes)
	{
		for (const auto mode: original_modes)
		{
			original_tokens.push_back(static_cast<uint32_t>(mode));
		}
	}

	static bool RemoveWrite(void* context, uintptr_t address, size_t size, uint32_t restore_token) noexcept
	{
		auto*        self  = static_cast<FakeProtection*>(context);
		const size_t first = (address - self->base) / self->page_size;
		const auto   bits  = static_cast<uint32_t>(self->original_modes[first]) & ~static_cast<uint32_t>(Mode::Write);
		const Mode   mode  = static_cast<Mode>(bits == 0u ? static_cast<uint32_t>(Mode::Read) : bits);
		self->calls.push_back({address, size, mode, restore_token});
		if (self->block_next_protect.exchange(false))
		{
			self->protect_entered.store(true);
			while (!self->release_protect.load())
			{
				std::this_thread::yield();
			}
		}
		const size_t pages         = size / self->page_size;
		const bool   fail          = self->fail_next_protect.exchange(false);
		const size_t applied_pages = fail ? 1u : pages;
		for (size_t page = 0; page < applied_pages; page++)
		{
			self->current_modes[first + page] = mode;
		}
		self->protect_completed.store(true);
		if (self->tracker != nullptr && self->fault_address >= address && self->fault_address - address < size)
		{
			const uintptr_t fault = self->fault_address;
			self->fault_address   = 0;
			self->fault_result    = self->tracker->HandleWriteFault(fault);
		}
		return !fail;
	}

	static bool RestoreSignalSafe(void* context, uintptr_t address, size_t size, uint32_t restore_token) noexcept
	{
		auto* self = static_cast<FakeProtection*>(context);
		self->signal_safe_calls++;
		self->signal_safe_tokens.push_back(restore_token);
		const bool   fail  = self->signal_safe_calls == self->fail_signal_safe_call.load();
		const size_t first = (address - self->base) / self->page_size;
		const size_t pages = size / self->page_size + (size % self->page_size != 0u ? 1u : 0u);
		for (size_t page = 0; !fail && page < pages; page++)
		{
			self->current_modes[first + page] =
			    restore_token == self->original_tokens[first + page] ? self->original_modes[first + page] : Mode::NoAccess;
		}
		if (self->block_signal_safe.load())
		{
			self->signal_safe_entered.store(true);
			while (!self->release_signal_safe.load())
			{
				std::this_thread::yield();
			}
		}
		return !fail;
	}

	static Core::VirtualMemory::ProtectionChangeResult RemoveWriteAndCapture(
	    void* context, uintptr_t address, size_t size, Core::VirtualMemory::CapturedProtectionVisitor visitor,
	    void* visitor_context) noexcept
	{
		auto* self = static_cast<FakeProtection*>(context);
		Core::VirtualMemory::ProtectionChangeResult result {};
		if (visitor == nullptr || size == 0)
		{
			return result;
		}
		const size_t first = (address - self->base) / self->page_size;
		const size_t pages = size / self->page_size + (size % self->page_size != 0u ? 1u : 0u);
		struct PendingRun
		{
			Core::VirtualMemory::CapturedProtectionRun captured;
			Mode target = Mode::NoAccess;
		};
		std::vector<PendingRun> runs;
		for (size_t begin = 0; begin < pages;)
		{
			size_t end = begin + 1u;
			while (end < pages && self->original_modes[first + end] == self->original_modes[first + begin] &&
			       self->original_tokens[first + end] == self->original_tokens[first + begin])
			{
				end++;
			}
			const Mode original = self->original_modes[first + begin];
			const auto bits = static_cast<uint32_t>(original) & ~static_cast<uint32_t>(Mode::Write);
			const Mode target = static_cast<Mode>(bits == 0u ? static_cast<uint32_t>(Mode::Read) : bits);
			const uintptr_t run_address = address + begin * self->page_size;
			const size_t run_size = (end - begin) * self->page_size;
			Core::VirtualMemory::CapturedProtectionRun run {run_address, run_size, original,
			                                                     self->original_tokens[first + begin]};
			runs.push_back({run, target});
			begin = end;
		}
		uint32_t visited = 0;
		for (const auto& run: runs)
		{
			std::atomic<bool> notify_started {false};
			std::atomic<bool> notify_done {false};
			std::thread notifier;
			if (self->tracker != nullptr && self->notify_before_capture_address >= run.captured.address &&
			    self->notify_before_capture_address - run.captured.address < run.captured.size)
			{
				const uintptr_t notify_address = self->notify_before_capture_address;
				self->notify_before_capture_address = 0;
				notifier = std::thread([&]
				{
					notify_started.store(true, std::memory_order_release);
					(void)self->tracker->NotifyWrite(notify_address, 1u);
					notify_done.store(true, std::memory_order_release);
				});
				while (!notify_started.load(std::memory_order_acquire)) { std::this_thread::yield(); }
				for (uint32_t spin = 0; spin < 10000u && !notify_done.load(std::memory_order_acquire); spin++)
				{
					std::this_thread::yield();
				}
			}
			if (!visitor(visitor_context, run.captured))
			{
				if (notifier.joinable()) { notifier.join(); }
				result.status = Core::VirtualMemory::ProtectionChangeStatus::ApplyFailedRolledBack;
				return result;
			}
			if (notifier.joinable()) { notifier.join(); }
			visited++;
			if (self->fail_capture_after_runs != 0u && visited == self->fail_capture_after_runs)
			{
				result.status = Core::VirtualMemory::ProtectionChangeStatus::ApplyFailedRolledBack;
				return result;
			}
		}
		for (const auto& run: runs)
		{
			if (self->tracker != nullptr && self->fault_before_capture_address >= run.captured.address &&
			    self->fault_before_capture_address - run.captured.address < run.captured.size)
			{
				const uintptr_t fault = self->fault_before_capture_address;
				self->fault_before_capture_address = 0;
				(void)self->tracker->HandleWriteFault(fault);
			}
			self->calls.push_back({run.captured.address, static_cast<size_t>(run.captured.size), run.target,
			                       run.captured.restore_token});
			const size_t run_first = (run.captured.address - self->base) / self->page_size;
			const size_t run_pages = run.captured.size / self->page_size;
			for (size_t page = 0; page < run_pages; page++)
			{
				self->current_modes[run_first + page] = run.target;
			}
			result.applied_runs++;
			result.applied_bytes += run.captured.size;
		}
		result.status = Core::VirtualMemory::ProtectionChangeStatus::Success;
		return result;
	}

	static bool Restore(void* context, uintptr_t address, size_t size, uint32_t restore_token) noexcept
	{
		auto* self = static_cast<FakeProtection*>(context);
		const size_t first = (address - self->base) / self->page_size;
		const size_t pages = size / self->page_size;
		self->calls.push_back({address, size, self->original_modes[first], restore_token});
		if (self->block_restore.load())
		{
			self->restore_entered.store(true);
			while (!self->release_restore.load())
			{
				std::this_thread::yield();
			}
		}
		const bool   fail          = self->fail_next_restore.exchange(false);
		const size_t applied_pages = fail ? 1u : pages;
		for (size_t page = 0; page < applied_pages; page++)
		{
			const size_t index = first + page;
			self->current_modes[index] = restore_token == self->original_tokens[index] ? self->original_modes[index] : Mode::NoAccess;
		}
		return !fail;
	}

	[[nodiscard]] GpuDirtyPageProtectionOps Ops() noexcept
	{
		return {this, &RemoveWriteAndCapture, &RemoveWrite, &Restore, &RestoreSignalSafe};
	}
};

} // namespace

TEST(EmulatorGraphicsDirtyTracking, FaultAndRearmAdvanceGeneration)
{
	Mapping mapping;
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.PrepareForRead(mapping.address, mapping.size));
	const uint64_t before = tracker.SnapshotGeneration(mapping.address, mapping.size);
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address));
	*reinterpret_cast<uint8_t*>(mapping.address) = 0x5a;
	EXPECT_TRUE(tracker.ChangedSince(mapping.address, mapping.size, before));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address + mapping.size / 2u));
	*reinterpret_cast<uint8_t*>(mapping.address + mapping.size / 2u) = 0xc3;
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, TombstoneProbeFindsExistingArmedPageBeforeReuse)
{
	Mapping mapping(2);
	ASSERT_NE(mapping.address, 0u);
	constexpr size_t kPageTableMask = (1u << 18u) - 1u;
	const size_t     target_slot    = GpuDirtyPageTableIndex(mapping.address, kPageTableMask);
	const uintptr_t  mapping_end    = mapping.address + mapping.size;
	uintptr_t        tombstone_page = GetPageSize();
	while (GpuDirtyPageTableIndex(tombstone_page, kPageTableMask) != target_slot ||
	       (tombstone_page >= mapping.address && tombstone_page < mapping_end))
	{
		tombstone_page += GetPageSize();
	}

	FakeProtection      protection {mapping.address, GetPageSize(), {Mode::ReadWrite, Mode::ReadWrite}};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(tombstone_page, GetPageSize()));
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, GetPageSize()));
	ASSERT_TRUE(tracker.UnregisterRange(tombstone_page, GetPageSize()));
	ASSERT_TRUE(tracker.Rearm(mapping.address, GetPageSize()));
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));

	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, GetPageSize()));
}

TEST(EmulatorGraphicsDirtyTracking, RearmCoalescesContiguousPagesWithUniformOriginalMode)
{
	Mapping mapping(4);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address, GetPageSize(), std::vector<Mode>(4, Mode::ReadWrite)};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	for (uint64_t page = 0; page < 4; page++)
	{
		ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + page * GetPageSize()));
	}
	protection.calls.clear();

	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_EQ(protection.calls.size(), 1u);
	EXPECT_EQ(protection.calls[0].address, mapping.address);
	EXPECT_EQ(protection.calls[0].size, mapping.size);
	EXPECT_EQ(protection.calls[0].mode, Mode::Read);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, RearmSplitsRunsAtOriginalPermissionBoundaries)
{
	Mapping mapping(5);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address,
	                                GetPageSize(),
	                                {Mode::ReadWrite, Mode::ReadWrite, Mode::ExecuteReadWrite, Mode::ExecuteReadWrite, Mode::ReadWrite}};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	for (uint64_t page = 0; page < 5; page++)
	{
		ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + page * GetPageSize()));
	}
	protection.calls.clear();

	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_EQ(protection.calls.size(), 3u);
	EXPECT_EQ(protection.calls[0].size, GetPageSize() * 2u);
	EXPECT_EQ(protection.calls[0].mode, Mode::Read);
	EXPECT_EQ(protection.calls[1].size, GetPageSize() * 2u);
	EXPECT_EQ(protection.calls[1].mode, Mode::ExecuteRead);
	EXPECT_EQ(protection.calls[2].size, GetPageSize());
	EXPECT_EQ(protection.calls[2].mode, Mode::Read);
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	EXPECT_EQ(protection.calls.size(), 3u);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, RearmSplitsRunsAtNativeProtectionBoundaries)
{
	Mapping mapping(2);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection protection {mapping.address, GetPageSize(), {Mode::ReadWrite, Mode::ReadWrite}};
	protection.original_tokens = {0x104u, 0x204u};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address));
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + GetPageSize()));
	protection.calls.clear();

	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_EQ(protection.calls.size(), 2u);
	EXPECT_EQ(protection.calls[0].token, 0x104u);
	EXPECT_EQ(protection.calls[1].token, 0x204u);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, WriteFaultRestoresCapturedNativeProtection)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection protection {mapping.address, GetPageSize(), {Mode::ExecuteReadWrite}};
	protection.original_tokens = {0x407u};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));

	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address));
	ASSERT_EQ(protection.signal_safe_tokens.size(), 1u);
	EXPECT_EQ(protection.signal_safe_tokens[0], 0x407u);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, QueuedSiblingFaultAfterRestoreRemainsHandled)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address, GetPageSize(), {Mode::ReadWrite}};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));

	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address));
	const uint32_t restores_after_first_fault = protection.signal_safe_calls;
	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address));
	EXPECT_EQ(protection.signal_safe_calls, restores_after_first_fault + 1u);
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, FirstArmPublishesNativeProtectionBeforeWriteCanFault)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection protection {mapping.address, GetPageSize(), {Mode::ExecuteReadWrite}};
	protection.original_tokens = {0x407u};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker                      = &tracker;
	protection.fault_before_capture_address = mapping.address;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));

	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_EQ(protection.signal_safe_tokens.size(), 2u);
	EXPECT_EQ(protection.signal_safe_tokens[0], 0x407u);
	EXPECT_EQ(protection.current_modes[0], Mode::ExecuteReadWrite);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, FirstArmPublishesNativeProtectionBeforeHostWriteReturns)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection protection {mapping.address, GetPageSize(), {Mode::ExecuteReadWrite}};
	protection.original_tokens = {0x407u};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	protection.notify_before_capture_address = mapping.address;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));

	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_FALSE(protection.signal_safe_tokens.empty());
	EXPECT_EQ(protection.signal_safe_tokens[0], 0x407u);
	EXPECT_EQ(protection.current_modes[0], Mode::ExecuteReadWrite);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, FailedCaptureDiscardsPartialNativeProtectionMetadata)
{
	Mapping mapping(2);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection protection {mapping.address, GetPageSize(), {Mode::ReadWrite, Mode::ReadWrite}};
	protection.original_tokens = {0x104u, 0x204u};
	protection.fail_capture_after_runs = 1;
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_FALSE(tracker.Rearm(mapping.address, mapping.size));

	protection.fail_capture_after_runs = 0;
	protection.original_tokens = {0x304u, 0x404u};
	protection.calls.clear();
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_EQ(protection.calls.size(), 2u);
	EXPECT_EQ(protection.calls[0].token, 0x304u);
	EXPECT_EQ(protection.calls[1].token, 0x404u);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, OverlappingRearmDoesNotProtectAnArmedPageTwice)
{
	Mapping mapping(4);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address, GetPageSize(), std::vector<Mode>(4, Mode::ReadWrite)};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, GetPageSize() * 3u));
	ASSERT_TRUE(tracker.RegisterRange(mapping.address + GetPageSize(), GetPageSize() * 3u));
	ASSERT_TRUE(tracker.Rearm(mapping.address, GetPageSize() * 3u));
	ASSERT_TRUE(tracker.Rearm(mapping.address + GetPageSize(), GetPageSize() * 3u));
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + GetPageSize() * 2u));
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + GetPageSize() * 3u));
	protection.calls.clear();

	ASSERT_TRUE(tracker.Rearm(mapping.address + GetPageSize(), GetPageSize() * 3u));
	ASSERT_EQ(protection.calls.size(), 1u);
	EXPECT_EQ(protection.calls[0].address, mapping.address + GetPageSize() * 2u);
	EXPECT_EQ(protection.calls[0].size, GetPageSize() * 2u);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, GetPageSize() * 3u));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address + GetPageSize(), GetPageSize() * 3u));
}

TEST(EmulatorGraphicsDirtyTracking, FaultDuringBatchedProtectKeepsOnlyRacedPageWritable)
{
	Mapping mapping(3);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address, GetPageSize(), std::vector<Mode>(3, Mode::ReadWrite)};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	for (uint64_t page = 0; page < 3; page++)
	{
		ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + page * GetPageSize()));
	}
	protection.calls.clear();
	protection.signal_safe_calls = 0;
	protection.fault_address     = mapping.address + GetPageSize();

	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_EQ(protection.calls.size(), 1u);
	EXPECT_EQ(protection.signal_safe_calls, 2u);
	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address));
	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address + GetPageSize()));
	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address + GetPageSize() * 2u));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, FailedArmingRollbackRemainsFaultHandled)
{
	Mapping mapping(3);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection(mapping.address, GetPageSize(), std::vector<Mode>(3, Mode::ReadWrite));
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	for (uint64_t page = 0; page < 3; page++)
	{
		ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + page * GetPageSize()));
	}
	protection.signal_safe_calls = 0;
	protection.fail_signal_safe_call.store(2);
	protection.fault_address = mapping.address + GetPageSize();

	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	EXPECT_EQ(tracker.Mode(mapping.address, mapping.size), GpuDirtyTrackingMode::HashFallback);
	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address + GetPageSize()));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, RearmRestoresWriteAfterConcurrentDisarmingCompletes)
{
	Mapping mapping(2);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection(mapping.address, GetPageSize(), std::vector<Mode>(2, Mode::ReadWrite));
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address));
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + GetPageSize()));

	protection.block_next_protect.store(true);
	protection.block_signal_safe.store(true);
	bool        rearm_result = false;
	bool        write_result = false;
	std::thread rearm([&] { rearm_result = tracker.Rearm(mapping.address, mapping.size); });
	while (!protection.protect_entered.load())
	{
		std::this_thread::yield();
	}
	std::thread writer([&] { write_result = tracker.NotifyWrite(mapping.address, 1u); });
	while (!protection.signal_safe_entered.load())
	{
		std::this_thread::yield();
	}
	protection.release_protect.store(true);
	while (!protection.protect_completed.load())
	{
		std::this_thread::yield();
	}
	protection.release_signal_safe.store(true);
	rearm.join();
	writer.join();

	EXPECT_TRUE(rearm_result);
	EXPECT_TRUE(write_result);
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_EQ(protection.current_modes[1], Mode::Read);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, FaultInArmingRollbackWindowRestoresWritablePage)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection(mapping.address, GetPageSize(), {Mode::ReadWrite});
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address));

	protection.block_next_protect.store(true);
	protection.fault_address = mapping.address;
	bool        rearm_result = false;
	std::thread rearm([&] { rearm_result = tracker.Rearm(mapping.address, mapping.size); });
	while (!protection.protect_entered.load())
	{
		std::this_thread::yield();
	}
	ASSERT_TRUE(tracker.NotifyWrite(mapping.address, 1u));
	ASSERT_EQ(protection.current_modes[0], Mode::ReadWrite);
	protection.release_protect.store(true);
	rearm.join();

	EXPECT_TRUE(rearm_result);
	EXPECT_TRUE(protection.fault_result);
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, FailedBatchedProtectRestoresPartiallyChangedPages)
{
	Mapping mapping(3);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection(mapping.address, GetPageSize(), std::vector<Mode>(3, Mode::ReadWrite));
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	for (uint64_t page = 0; page < 3; page++)
	{
		ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + page * GetPageSize()));
	}
	protection.fail_next_protect.store(true);

	EXPECT_FALSE(tracker.Rearm(mapping.address, mapping.size));
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_EQ(protection.current_modes[1], Mode::ReadWrite);
	EXPECT_EQ(protection.current_modes[2], Mode::ReadWrite);
	EXPECT_EQ(tracker.Mode(mapping.address, mapping.size), GpuDirtyTrackingMode::HashFallback);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, RearmLastAddressPageDoesNotWrap)
{
	const uintptr_t     page_size = GetPageSize();
	const uintptr_t     address   = std::numeric_limits<uintptr_t>::max() - page_size + 1u;
	FakeProtection      protection(address, page_size, {Mode::ReadWrite});
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(address, page_size - 1u));
	ASSERT_TRUE(tracker.Rearm(address, page_size - 1u));
	ASSERT_TRUE(tracker.HandleWriteFault(address));

	EXPECT_TRUE(tracker.Rearm(address, page_size - 1u));
	EXPECT_EQ(protection.current_modes[0], Mode::Read);
	EXPECT_TRUE(tracker.UnregisterRange(address, page_size - 1u));
}

TEST(EmulatorGraphicsDirtyTracking, LastAddressRestoreFailureMarksFallback)
{
	const uintptr_t     page_size = GetPageSize();
	const uintptr_t     address   = std::numeric_limits<uintptr_t>::max() - page_size + 1u;
	FakeProtection      protection(address, page_size, {Mode::ReadWrite});
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(address, page_size - 1u));
	ASSERT_TRUE(tracker.Rearm(address, page_size - 1u));
	ASSERT_TRUE(tracker.HandleWriteFault(address));
	protection.signal_safe_calls = 0;
	protection.fail_signal_safe_call.store(2);
	protection.fault_address = address;

	ASSERT_TRUE(tracker.Rearm(address, page_size - 1u));
	EXPECT_EQ(tracker.Mode(address, page_size - 1u), GpuDirtyTrackingMode::HashFallback);
	EXPECT_TRUE(tracker.HandleWriteFault(address));
	EXPECT_TRUE(tracker.UnregisterRange(address, page_size - 1u));
}

TEST(EmulatorGraphicsDirtyTracking, HostNotificationMarksEveryOverlappingPage)
{
	Mapping mapping;
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.PrepareForRead(mapping.address, mapping.size));
	const uint64_t before = tracker.SnapshotGeneration(mapping.address, mapping.size);
	(void)tracker.NotifyWrite(mapping.address + mapping.size - 1u, 2u);
	EXPECT_TRUE(tracker.ChangedSince(mapping.address, mapping.size, before));
	EXPECT_EQ(tracker.Mode(mapping.address, mapping.size), GpuDirtyTrackingMode::PageFault);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

// Partial uploads copy only the pages whose generation moved: a write must
// move exactly the pages it touched, a fault included, and nothing else.
TEST(EmulatorGraphicsDirtyTracking, PageGenerationsMoveOnlyForWrittenPages)
{
	Mapping mapping(4u);
	ASSERT_NE(mapping.address, 0u);
	const uint64_t      page = GetPageSize();
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.PrepareForRead(mapping.address, mapping.size));
	ASSERT_EQ(tracker.PageCount(mapping.address + 1u, page), 2u);
	std::array<uint64_t, 4> before {};
	ASSERT_TRUE(tracker.PageGenerations(mapping.address, mapping.size, before.data(), before.size()));
	EXPECT_FALSE(tracker.PageGenerations(mapping.address, mapping.size, before.data(), before.size() - 1u));

	(void)tracker.NotifyWrite(mapping.address + page + 8u, 4u);
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address + page * 3u));
	std::array<uint64_t, 4> after {};
	ASSERT_TRUE(tracker.PageGenerations(mapping.address, mapping.size, after.data(), after.size()));
	EXPECT_EQ(after[0], before[0]);
	EXPECT_NE(after[1], before[1]);
	EXPECT_EQ(after[2], before[2]);
	EXPECT_NE(after[3], before[3]);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, OverlappingRangesShareGenerationEvidence)
{
	Mapping mapping;
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.RegisterRange(mapping.address + mapping.size / 2u, mapping.size / 2u));
	ASSERT_TRUE(tracker.PrepareForRead(mapping.address, mapping.size));
	const uint64_t first = tracker.SnapshotGeneration(mapping.address, mapping.size);
	(void)tracker.NotifyWrite(mapping.address + mapping.size / 2u, 1u);
	EXPECT_TRUE(tracker.ChangedSince(mapping.address, mapping.size, first));
	EXPECT_TRUE(tracker.ChangedSince(mapping.address + mapping.size / 2u, mapping.size, first));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address + mapping.size / 2u, mapping.size / 2u));
}

TEST(EmulatorGraphicsDirtyTracking, WriteOnFirstUnalignedPageAdvancesRangeGeneration)
{
	Mapping mapping;
	ASSERT_NE(mapping.address, 0u);
	const uint64_t page_size = GetPageSize();
	ASSERT_GT(page_size, 1u);

	const uint64_t address = mapping.address + 1u;
	const uint64_t size    = page_size;

	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(address, size));
	ASSERT_TRUE(tracker.PrepareForRead(address, size));
	const uint64_t before = tracker.SnapshotGeneration(address, size);

	ASSERT_TRUE(tracker.NotifyWrite(address, 1u));
	EXPECT_TRUE(tracker.ChangedSince(address, size, before));
	EXPECT_TRUE(tracker.UnregisterRange(address, size));
}

TEST(EmulatorGraphicsDirtyTracking, ReadObservationDoesNotAcknowledgeConcurrentWrite)
{
	Mapping mapping;
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));

	const auto observation = tracker.BeginRead(mapping.address, mapping.size);
	ASSERT_TRUE(observation.tracked);
	EXPECT_FALSE(tracker.ChangedSince(mapping.address, mapping.size, observation.generation));
	EXPECT_TRUE(tracker.ReadObservationIsStable(mapping.address, mapping.size, observation));

	ASSERT_TRUE(tracker.NotifyWrite(mapping.address, 1u));
	const uint64_t after_write = tracker.SnapshotGeneration(mapping.address, mapping.size);
	EXPECT_GT(after_write, observation.generation);
	EXPECT_TRUE(tracker.ChangedSince(mapping.address, mapping.size, observation.generation));
	EXPECT_FALSE(tracker.ReadObservationIsStable(mapping.address, mapping.size, observation));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, ReadObservationFailsClosedWhenWriteStartsWhileArming)
{
	{
		Mapping mapping(1);
		ASSERT_NE(mapping.address, 0u);
		FakeProtection      protection {mapping.address, GetPageSize(), {Mode::ReadWrite}};
		GpuDirtyPageTracker tracker(protection.Ops());
		protection.tracker                       = &tracker;
		protection.notify_before_capture_address = mapping.address;
		ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));

		const auto raced = tracker.BeginRead(mapping.address, mapping.size);
		EXPECT_FALSE(raced.tracked);
		EXPECT_FALSE(tracker.ReadObservationIsStable(mapping.address, mapping.size, raced));

		// The injected writer has completed its tracker transaction. A later arm
		// with no writer in its window can establish a fresh stable observation.
		const auto stable = tracker.BeginRead(mapping.address, mapping.size);
		ASSERT_TRUE(stable.tracked);
		EXPECT_TRUE(tracker.ReadObservationIsStable(mapping.address, mapping.size, stable));
		EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
	}
	{
		Mapping mapping(1);
		ASSERT_NE(mapping.address, 0u);
		FakeProtection      protection {mapping.address, GetPageSize(), {Mode::ReadWrite}};
		GpuDirtyPageTracker tracker(protection.Ops());
		protection.tracker                     = &tracker;
		protection.fault_before_capture_address = mapping.address;
		ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));

		const auto raced = tracker.BeginRead(mapping.address, mapping.size);
		EXPECT_FALSE(raced.tracked);
		EXPECT_FALSE(tracker.ReadObservationIsStable(mapping.address, mapping.size, raced));
		EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
	}
}

TEST(EmulatorGraphicsDirtyTracking, UnregisterRetainsLateWritableFaultEvidence)
{
	Mapping mapping;
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.PrepareForRead(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address));
	EXPECT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	EXPECT_TRUE(tracker.PrepareForRead(mapping.address, mapping.size));
	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, ReregisteredRetiredPageRecapturesNativeProtection)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address, GetPageSize(), {Mode::ReadWrite}};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;

	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));

	protection.original_modes[0] = Mode::ExecuteReadWrite;
	protection.current_modes[0]  = Mode::ExecuteReadWrite;
	protection.original_tokens[0] = 0x407u;
	protection.calls.clear();

	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
	ASSERT_EQ(protection.calls.size(), 2u);
	EXPECT_EQ(protection.calls[0].mode, Mode::ExecuteRead);
	EXPECT_EQ(protection.calls[1].mode, Mode::ExecuteReadWrite);
	EXPECT_EQ(protection.calls[0].token, 0x407u);
	EXPECT_EQ(protection.calls[1].token, 0x407u);
}

TEST(EmulatorGraphicsDirtyTracking, UnregisterCoalescesContiguousPagesWithEqualNativeProtection)
{
	Mapping mapping(4);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address, GetPageSize(), std::vector<Mode>(4, Mode::ReadWrite)};
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	protection.calls.clear();

	ASSERT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
	ASSERT_EQ(protection.calls.size(), 1u);
	EXPECT_EQ(protection.calls[0].address, mapping.address);
	EXPECT_EQ(protection.calls[0].size, mapping.size);
	EXPECT_EQ(protection.calls[0].token, static_cast<uint32_t>(Mode::ReadWrite));
}

TEST(EmulatorGraphicsDirtyTracking, UnregisterSplitsRunsAtNativeProtectionBoundaries)
{
	Mapping mapping(4);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection protection {mapping.address, GetPageSize(), std::vector<Mode>(4, Mode::ReadWrite)};
	protection.original_tokens = {0x104u, 0x104u, 0x204u, 0x204u};
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	protection.calls.clear();

	ASSERT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
	ASSERT_EQ(protection.calls.size(), 2u);
	EXPECT_EQ(protection.calls[0].address, mapping.address);
	EXPECT_EQ(protection.calls[0].size, GetPageSize() * 2u);
	EXPECT_EQ(protection.calls[0].token, 0x104u);
	EXPECT_EQ(protection.calls[1].address, mapping.address + GetPageSize() * 2u);
	EXPECT_EQ(protection.calls[1].size, GetPageSize() * 2u);
	EXPECT_EQ(protection.calls[1].token, 0x204u);
}

TEST(EmulatorGraphicsDirtyTracking, UnregisterDoesNotRestorePagesCoveredByAnotherRange)
{
	Mapping mapping(4);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address, GetPageSize(), std::vector<Mode>(4, Mode::ReadWrite)};
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, GetPageSize() * 3u));
	ASSERT_TRUE(tracker.RegisterRange(mapping.address + GetPageSize(), GetPageSize() * 3u));
	ASSERT_TRUE(tracker.Rearm(mapping.address, GetPageSize() * 3u));
	ASSERT_TRUE(tracker.Rearm(mapping.address + GetPageSize(), GetPageSize() * 3u));
	protection.calls.clear();

	ASSERT_TRUE(tracker.UnregisterRange(mapping.address, GetPageSize() * 3u));
	ASSERT_EQ(protection.calls.size(), 1u);
	EXPECT_EQ(protection.calls[0].address, mapping.address);
	EXPECT_EQ(protection.calls[0].size, GetPageSize());
	ASSERT_TRUE(tracker.UnregisterRange(mapping.address + GetPageSize(), GetPageSize() * 3u));
	ASSERT_EQ(protection.calls.size(), 2u);
	EXPECT_EQ(protection.calls[1].address, mapping.address + GetPageSize());
	EXPECT_EQ(protection.calls[1].size, GetPageSize() * 3u);
}

TEST(EmulatorGraphicsDirtyTracking, FailedBatchedUnregisterRetriesEveryPage)
{
	Mapping mapping(3);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address, GetPageSize(), std::vector<Mode>(3, Mode::ReadWrite)};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	protection.calls.clear();
	protection.fail_next_restore.store(true);

	EXPECT_FALSE(tracker.UnregisterRange(mapping.address, mapping.size));
	ASSERT_EQ(protection.calls.size(), 4u);
	EXPECT_EQ(protection.calls[0].size, mapping.size);
	for (size_t page = 0; page < 3u; page++)
	{
		EXPECT_EQ(protection.calls[page + 1u].address, mapping.address + page * GetPageSize());
		EXPECT_EQ(protection.calls[page + 1u].size, GetPageSize());
		EXPECT_EQ(protection.current_modes[page], Mode::ReadWrite);
		EXPECT_TRUE(tracker.HandleWriteFault(mapping.address + page * GetPageSize()));
	}
}

TEST(EmulatorGraphicsDirtyTracking, NotifyWriteWaitsForConcurrentRetirement)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection      protection {mapping.address, GetPageSize(), {Mode::ReadWrite}};
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	protection.block_restore.store(true);

	bool              unregister_result = false;
	bool              notify_result     = false;
	std::atomic<bool> notify_started {false};
	std::atomic<bool> notify_done {false};
	std::thread unregister([&] { unregister_result = tracker.UnregisterRange(mapping.address, mapping.size); });
	while (!protection.restore_entered.load()) { std::this_thread::yield(); }
	std::thread notifier([&]
	{
		notify_started.store(true, std::memory_order_release);
		notify_result = tracker.NotifyWrite(mapping.address, 1u);
		notify_done.store(true, std::memory_order_release);
	});
	while (!notify_started.load(std::memory_order_acquire)) { std::this_thread::yield(); }
	for (uint32_t spin = 0; spin < 10000u && !notify_done.load(std::memory_order_acquire); spin++)
	{
		std::this_thread::yield();
	}
	EXPECT_FALSE(notify_done.load(std::memory_order_acquire));
	protection.release_restore.store(true);
	unregister.join();
	notifier.join();

	EXPECT_TRUE(unregister_result);
	EXPECT_TRUE(notify_result);
	EXPECT_TRUE(notify_done.load(std::memory_order_acquire));
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
}

TEST(EmulatorGraphicsDirtyTracking, InvalidRangeFallsBack)
{
	GpuDirtyPageTracker tracker;
	EXPECT_FALSE(tracker.RegisterRange(0, 1));
	EXPECT_EQ(tracker.Mode(0, 1), GpuDirtyTrackingMode::HashFallback);
}

TEST(EmulatorGraphicsDirtyTracking, HostWriteLeaseBlocksDisjointBytesOnSharedPage)
{
	Mapping mapping(2);
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, 64));
	const auto before = tracker.BeginRead(mapping.address, 64);
	ASSERT_TRUE(before.tracked);
	const auto lease = tracker.BeginHostWrite(mapping.address + 128, mapping.size - 128);
	ASSERT_NE(lease, 0u);
	EXPECT_FALSE(tracker.BeginRead(mapping.address, 64).tracked);
	EXPECT_FALSE(tracker.ReadObservationIsStable(mapping.address, 64, before));
	std::memset(reinterpret_cast<void*>(mapping.address + 128), 0x5a, mapping.size - 128);
	tracker.EndHostWrite(lease);
	const auto after = tracker.BeginRead(mapping.address, 64);
	EXPECT_TRUE(after.tracked);
	EXPECT_GT(after.generation, before.generation);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, 64));
}

TEST(EmulatorGraphicsDirtyTracking, HostWriteLeaseCoversNewRegistrationsAndNestedPageSpans)
{
	Mapping mapping(3);
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	const auto first = tracker.BeginHostWrite(mapping.address + 64, GetPageSize());
	const auto nested = tracker.BeginHostWrite(mapping.address + 128, GetPageSize());
	const auto overlap = tracker.BeginHostWrite(mapping.address + GetPageSize() + 64, GetPageSize());
	ASSERT_NE(first, 0u);
	ASSERT_EQ(nested, first);
	ASSERT_NE(overlap, first);
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	EXPECT_FALSE(tracker.BeginRead(mapping.address, mapping.size).tracked);
	tracker.EndHostWrite(first);
	EXPECT_FALSE(tracker.Rearm(mapping.address, GetPageSize()));
	tracker.EndHostWrite(nested);
	EXPECT_TRUE(tracker.Rearm(mapping.address, GetPageSize()));
	EXPECT_FALSE(tracker.Rearm(mapping.address + GetPageSize(), GetPageSize()));
	tracker.EndHostWrite(overlap);
	EXPECT_TRUE(tracker.BeginRead(mapping.address, mapping.size).tracked);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, HostWriteLeaseWaitsForNativeRearmCommit)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection protection(mapping.address, GetPageSize(), {Mode::ReadWrite});
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	ASSERT_TRUE(tracker.NotifyWrite(mapping.address, 1));
	protection.block_next_protect.store(true);
	std::thread rearm([&] { (void)tracker.Rearm(mapping.address, mapping.size); });
	while (!protection.protect_entered.load()) { std::this_thread::yield(); }
	uint64_t token = 0;
	std::thread writer([&] { token = tracker.BeginHostWrite(mapping.address + 128, 64); });
	// The native protection call is held at its commit boundary. The lease
	// must restore its result before acquisition returns, in either schedule.
	protection.release_protect.store(true);
	rearm.join();
	writer.join();
	ASSERT_NE(token, 0u);
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_FALSE(tracker.Rearm(mapping.address, 64));
	tracker.EndHostWrite(token);
	EXPECT_TRUE(tracker.Rearm(mapping.address, 64));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

TEST(EmulatorGraphicsDirtyTracking, HostWriteLeaseDisabledAndEmptyRangesAreNoOps)
{
	GpuDirtyPageTracker disabled(false);
	EXPECT_EQ(disabled.BeginHostWrite(1, 1), 0u);
	disabled.EndHostWrite(0);
	GpuDirtyPageTracker enabled;
	EXPECT_EQ(enabled.BeginHostWrite(0, 1), 0u);
	EXPECT_EQ(enabled.BeginHostWrite(1, 0), 0u);
	EXPECT_EQ(enabled.BeginHostWrite(UINTPTR_MAX - 1, 4), 0u);
}

TEST(EmulatorGraphicsDirtyTracking, WideNotifyWritePreservesSparsePageEffectsAndSpanBoundaries)
{
	Mapping mapping(3);
	ASSERT_NE(mapping.address, 0u);
	const uintptr_t page_size = GetPageSize();
	const size_t huge = static_cast<size_t>(std::numeric_limits<int64_t>::max());
	const uintptr_t start = mapping.address + page_size + 128;
	ASSERT_LE(start, UINTPTR_MAX - huge);
	ASSERT_LT(mapping.address + mapping.size, static_cast<uintptr_t>(huge) + 1u);
	FakeProtection protection(mapping.address, page_size, {Mode::ReadWrite, Mode::ExecuteReadWrite, Mode::ReadWrite});
	GpuDirtyPageTracker tracker(protection.Ops());
	const uintptr_t watched[] = {mapping.address, mapping.address + page_size, mapping.address + page_size + 512,
	                             mapping.address + 2 * page_size};
	for (const auto address: watched)
	{
		ASSERT_TRUE(tracker.RegisterRange(address, 64));
		ASSERT_TRUE(tracker.BeginRead(address, 64).tracked);
	}
	const auto outside_generation = tracker.SnapshotGeneration(watched[0], 64);

	// The I/O bytes begin after the first watched resource on page 1. Both
	// resources on that page must be dirtied/restored; page 0 remains protected.
	ASSERT_TRUE(tracker.NotifyWrite(start, huge));
	EXPECT_EQ(protection.current_modes[0], Mode::Read);
	EXPECT_EQ(protection.current_modes[1], Mode::ExecuteReadWrite);
	EXPECT_EQ(protection.current_modes[2], Mode::ReadWrite);
	EXPECT_EQ(protection.signal_safe_calls, 2u);
	EXPECT_EQ(tracker.SnapshotGeneration(watched[0], 64), outside_generation);
	for (size_t i = 1; i < 4; ++i) { EXPECT_GT(tracker.SnapshotGeneration(watched[i], 64), 0u); }

	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	// All watched pages are far from this span's start. Limiting notifications
	// to a prefix would return quickly but lose the actual permission effects.
	ASSERT_TRUE(tracker.NotifyWrite(1, huge));
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_EQ(protection.current_modes[1], Mode::ExecuteReadWrite);
	EXPECT_EQ(protection.current_modes[2], Mode::ReadWrite);
	EXPECT_EQ(protection.signal_safe_calls, 5u);
	EXPECT_GT(tracker.SnapshotGeneration(watched[0], 64), outside_generation);
	for (const auto address: watched) { EXPECT_TRUE(tracker.UnregisterRange(address, 64)); }
}

TEST(EmulatorGraphicsDirtyTracking, WideHostWriteLeaseCoversLateRegistrationAndNestedSpans)
{
	Mapping mapping(4);
	ASSERT_NE(mapping.address, 0u);
	const uintptr_t page_size = GetPageSize();
	const size_t huge = static_cast<size_t>(std::numeric_limits<int64_t>::max());
	const uintptr_t overlapping_start = mapping.address + page_size + 128;
	ASSERT_LE(overlapping_start, UINTPTR_MAX - huge);
	ASSERT_LT(mapping.address + mapping.size, static_cast<uintptr_t>(huge) + 1u);
	FakeProtection protection(mapping.address, page_size, std::vector<Mode>(4, Mode::ReadWrite));
	GpuDirtyPageTracker tracker(protection.Ops());
	const uintptr_t late = mapping.address + 2 * page_size;
	const uintptr_t last = mapping.address + 3 * page_size;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, 64));
	ASSERT_TRUE(tracker.RegisterRange(last, 64));
	ASSERT_TRUE(tracker.BeginRead(mapping.address, 64).tracked);
	ASSERT_TRUE(tracker.BeginRead(last, 64).tracked);
	const auto first = tracker.BeginHostWrite(1, huge);
	const auto nested = tracker.BeginHostWrite(1, huge);
	const auto overlap = tracker.BeginHostWrite(overlapping_start, huge);
	ASSERT_NE(first, 0u);
	EXPECT_EQ(first, nested);
	EXPECT_NE(first, overlap);
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_EQ(protection.current_modes[3], Mode::ReadWrite);
	ASSERT_TRUE(tracker.RegisterRange(late, 64));
	EXPECT_FALSE(tracker.BeginRead(late, 64).tracked);
	EXPECT_EQ(tracker.SnapshotGeneration(late, 64), 0u);
	EXPECT_EQ(protection.current_modes[2], Mode::ReadWrite);

	tracker.EndHostWrite(first);
	// Completion must still visit a newly registered page even though the
	// original byte address rounded down to page zero and another lease remains.
	const auto first_completion = tracker.SnapshotGeneration(late, 64);
	EXPECT_GT(first_completion, 0u);
	EXPECT_FALSE(tracker.BeginRead(mapping.address, 64).tracked);
	EXPECT_FALSE(tracker.BeginRead(late, 64).tracked);
	tracker.EndHostWrite(nested);
	EXPECT_GT(tracker.SnapshotGeneration(late, 64), first_completion);
	const auto outside = tracker.BeginRead(mapping.address, 64);
	EXPECT_TRUE(outside.tracked);
	EXPECT_FALSE(tracker.BeginRead(late, 64).tracked);
	const auto before_last_completion = tracker.SnapshotGeneration(late, 64);
	tracker.EndHostWrite(overlap);
	EXPECT_GT(tracker.SnapshotGeneration(late, 64), before_last_completion);
	EXPECT_TRUE(tracker.ReadObservationIsStable(mapping.address, 64, outside));
	EXPECT_TRUE(tracker.BeginRead(late, 64).tracked);
	EXPECT_TRUE(tracker.BeginRead(last, 64).tracked);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, 64));
	EXPECT_TRUE(tracker.UnregisterRange(late, 64));
	EXPECT_TRUE(tracker.UnregisterRange(last, 64));
}

TEST(EmulatorGraphicsDirtyTracking, WideNotifyWriteRetainsNativeArmingRollbackHandshake)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	const size_t huge = static_cast<size_t>(std::numeric_limits<int64_t>::max());
	ASSERT_LT(mapping.address + mapping.size, static_cast<uintptr_t>(huge) + 1u);
	FakeProtection protection(mapping.address, GetPageSize(), {Mode::ReadWrite});
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, 64));
	ASSERT_TRUE(tracker.BeginRead(mapping.address, 64).tracked);
	ASSERT_TRUE(tracker.NotifyWrite(mapping.address, 64));
	const auto before = tracker.SnapshotGeneration(mapping.address, 64);
	const auto restores_before = protection.signal_safe_calls;
	protection.block_next_protect.store(true);
	std::thread rearm([&] { (void)tracker.Rearm(mapping.address, 64); });
	while (!protection.protect_entered.load()) { std::this_thread::yield(); }
	// The native protect is paused while holding the registration mutex. The
	// wide notification must not take that mutex or wait for this rearm to end.
	EXPECT_TRUE(tracker.NotifyWrite(1, huge));
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_GT(tracker.SnapshotGeneration(mapping.address, 64), before);
	protection.release_protect.store(true);
	rearm.join();
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_EQ(protection.signal_safe_calls, restores_before + 2u);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, 64));
}

TEST(EmulatorGraphicsDirtyTracking, WideHostWriteLeaseAtAddressLimitDoesNotWrap)
{
	const uintptr_t page_size = GetPageSize();
	const uintptr_t last_page = UINTPTR_MAX - page_size + 1u;
	const uintptr_t start = UINTPTR_MAX / 2u + 1u;
	const size_t huge = static_cast<size_t>(std::numeric_limits<int64_t>::max());
	ASSERT_EQ(start + huge, UINTPTR_MAX);
	// Only one fake protection page and the fixed tracker metadata are needed;
	// the large virtual request is never mapped, allocated or dereferenced.
	FakeProtection protection(last_page, page_size, {Mode::ReadWrite});
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(last_page, page_size - 1u));
	ASSERT_TRUE(tracker.BeginRead(last_page, page_size - 1u).tracked);
	const auto lease = tracker.BeginHostWrite(start, huge);
	ASSERT_NE(lease, 0u);
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_FALSE(tracker.BeginRead(last_page, page_size - 1u).tracked);
	const auto before_end = tracker.SnapshotGeneration(last_page, page_size - 1u);
	tracker.EndHostWrite(lease);
	EXPECT_GT(tracker.SnapshotGeneration(last_page, page_size - 1u), before_end);
	EXPECT_EQ(protection.signal_safe_calls, 1u);
	EXPECT_TRUE(tracker.BeginRead(last_page, page_size - 1u).tracked);
	EXPECT_TRUE(tracker.UnregisterRange(last_page, page_size - 1u));
}

TEST(EmulatorGraphicsDirtyTracking, HostWriteLeaseScopePreservesErrnoAndPairedCallbackIdentity)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	HostWriteCallbacks callbacks(&tracker);
	ASSERT_TRUE(callbacks.installed);
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, 64));
	errno = EDOM;
	{
		const Kyty::Emulator::VideoFrameMemory::HostWriteLease lease(mapping.address + 128, 64);
		EXPECT_EQ(errno, EDOM);
		EXPECT_FALSE(tracker.Rearm(mapping.address, 64));
		// Completion uses the captured end callback even if dispatch is replaced
		// during a blocking operation. A removed bundle must not leak the lease.
		EXPECT_TRUE(Kyty::Emulator::VideoFrameMemory::InstallCallbacks({}));
		errno = EIO;
	}
	EXPECT_EQ(errno, EIO);
	EXPECT_TRUE(tracker.Rearm(mapping.address, 64));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, 64));
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
TEST(EmulatorGraphicsDirtyTracking, WideHostWriteLeaseReachesHostErrorWithoutAllocatingRequestedSpan)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	const size_t huge = static_cast<size_t>(std::numeric_limits<int64_t>::max());
	ASSERT_LE(mapping.address + 128, UINTPTR_MAX - huge);
	GpuDirtyPageTracker tracker;
	HostWriteCallbacks callbacks(&tracker);
	ASSERT_TRUE(callbacks.installed);
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, 64));
	const auto before = tracker.BeginRead(mapping.address, 64);
	ASSERT_TRUE(before.tracked);
	{
		const Kyty::Emulator::VideoFrameMemory::HostWriteLease lease(mapping.address + 128, huge);
		EXPECT_FALSE(tracker.BeginRead(mapping.address, 64).tracked);
		// The host decides the actual transfer result. The lease neither walks
		// nor allocates the requested span, and it must let this syscall execute.
		errno = 0;
		EXPECT_EQ(::read(-1, reinterpret_cast<void*>(mapping.address + 128), huge), -1);
		EXPECT_EQ(errno, EBADF);
	}
	EXPECT_EQ(errno, EBADF);
	const auto after = tracker.BeginRead(mapping.address, 64);
	EXPECT_TRUE(after.tracked);
	EXPECT_GT(after.generation, before.generation);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, 64));
}

TEST(EmulatorGraphicsDirtyTracking, HostWriteLeaseAllowsDirectKernelCopyAndReleasesOnShortReadAndError)
{
	Mapping mapping(2);
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, 64));
	ASSERT_TRUE(tracker.BeginRead(mapping.address, 64).tracked);
	HostWriteCallbacks callbacks(&tracker);
	ASSERT_TRUE(callbacks.installed);
	int descriptors[2] {};
	ASSERT_EQ(::pipe(descriptors), 0);
	const char payload[] = "direct kernel copy";
	EXPECT_EQ(::write(descriptors[1], payload, sizeof(payload)), static_cast<ssize_t>(sizeof(payload)));
	EXPECT_EQ(::close(descriptors[1]), 0);
	{
		const Kyty::Emulator::VideoFrameMemory::HostWriteLease lease(mapping.address + 128, mapping.size - 128);
		EXPECT_FALSE(tracker.BeginRead(mapping.address, 64).tracked);
		EXPECT_EQ(::read(descriptors[0], reinterpret_cast<void*>(mapping.address + 128), mapping.size - 128),
		          static_cast<ssize_t>(sizeof(payload)));
		EXPECT_EQ(std::memcmp(reinterpret_cast<void*>(mapping.address + 128), payload, sizeof(payload)), 0);
		EXPECT_EQ(::read(descriptors[0], reinterpret_cast<void*>(mapping.address + 128), mapping.size - 128), 0);
		EXPECT_EQ(::close(descriptors[0]), 0);
		errno = 0;
		EXPECT_EQ(::read(-1, reinterpret_cast<void*>(mapping.address + 128), 64), -1);
		EXPECT_EQ(errno, EBADF);
	}
	EXPECT_EQ(errno, EBADF);
	EXPECT_TRUE(tracker.BeginRead(mapping.address, 64).tracked);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, 64));
}
#endif

TEST(EmulatorGraphicsDirtyTracking, DisabledTrackerFallsBackWithoutMetadata)
{
	GpuDirtyPageTracker tracker(false);
	EXPECT_FALSE(tracker.Enabled());
	EXPECT_FALSE(tracker.RegisterRange(1, 1));
	EXPECT_FALSE(tracker.UnregisterRange(1, 1));
	EXPECT_FALSE(tracker.PrepareForRead(1, 1));
	EXPECT_FALSE(tracker.Rearm(1, 1));
	EXPECT_FALSE(tracker.HandleWriteFault(1));
	EXPECT_FALSE(tracker.NotifyWrite(1, 1));
	EXPECT_EQ(tracker.SnapshotGeneration(1, 1), 0u);
	EXPECT_TRUE(tracker.ChangedSince(1, 1, 0));
	EXPECT_EQ(tracker.Mode(1, 1), GpuDirtyTrackingMode::HashFallback);
}

TEST(EmulatorGraphicsDirtyTracking, DefaultPolicyRequiresAnExplicitDisableValue)
{
	EXPECT_FALSE(GpuDirtyTrackingEnabledForProcess(nullptr, false));
	EXPECT_TRUE(GpuDirtyTrackingEnabledForProcess(nullptr, true));
	EXPECT_TRUE(GpuDirtyTrackingEnabledForProcess("", true));
	EXPECT_TRUE(GpuDirtyTrackingEnabledForProcess("0", true));
	EXPECT_FALSE(GpuDirtyTrackingEnabledForProcess("1", true));
	EXPECT_FALSE(GpuDirtyTrackingEnabledForProcess("true", true));
}

TEST(EmulatorGraphicsDirtyTracking, PublicWriteRoutesRejectInvalidRanges)
{
	EXPECT_FALSE(GpuMemoryCheckAccessViolation(0));
	EXPECT_FALSE(GpuMemoryNotifyHostWrite(0, 0));
}

namespace {

// Byte held by a fake backend after create or update: what a later draw reads.
struct UploadedByteBacking
{
	uint8_t uploaded = 0;
};

// Armed by the probe. The first create callback performs one host write: it
// announces the write with NotifyWrite and then stores the new guest byte.
struct CreateRaceState
{
	uint8_t write_value     = 0;
	bool    write_on_create = false;
};

CreateRaceState g_create_race;

// Non-staged check_hash object: VertexBuffer without read_only skips the
// stable-create staging path. The write window under test is inside create_func:
// after it has copied the guest byte into its backing, and before the create
// returns, a host write lands. The reuse must refresh from the byte left behind.
struct CreateRaceGpuObject final: public GpuObject
{
	CreateRaceGpuObject()
	{
		type       = GpuMemoryObjectType::VertexBuffer;
		params[0]  = 1u;
		read_only  = false;
		check_hash = true;
	}

	bool Equal(const uint64_t* other) const override { return other != nullptr && other[0] == params[0]; }

	create_func_t GetCreateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* /*params*/, const uint64_t* vaddr, const uint64_t* /*size*/, int /*vaddr_num*/,
		          VulkanMemory* /*mem*/) -> void*
		{
			auto* backing     = new UploadedByteBacking;
			backing->uploaded = *reinterpret_cast<const uint8_t*>(vaddr[0]);
			if (g_create_race.write_on_create)
			{
				g_create_race.write_on_create = false;
				(void)GpuDirtyPageTracker::Instance().NotifyWrite(vaddr[0], 1u);
				*reinterpret_cast<uint8_t*>(vaddr[0]) = g_create_race.write_value;
			}
			return backing;
		};
	}

	create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; }

	write_back_func_t GetWriteBackFunc() const override { return nullptr; }

	delete_func_t GetDeleteFunc() const override
	{
		return [](GraphicContext* /*ctx*/, void* obj, VulkanMemory* /*mem*/) { delete static_cast<UploadedByteBacking*>(obj); };
	}

	update_func_t GetUpdateFunc() const override
	{
		return [](GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* obj, const uint64_t* vaddr, const uint64_t* /*size*/,
		          int /*vaddr_num*/)
		{ static_cast<UploadedByteBacking*>(obj)->uploaded = *reinterpret_cast<const uint8_t*>(vaddr[0]); };
	}
};

// Runs in a fresh process. The tracker singleton reads its fault-handler gate
// once, and GpuMemory is process-global with a one-shot init, so neither can be
// reset between tests in this binary.
void RunNonStagedCreateRaceProbe()
{
	// Child stdout is not captured by the death test. Route it to stderr, unbuffered,
	// so the probe's diagnostics appear in the captured output.
	std::fflush(stdout);
#if defined(_WIN32)
	if (::_dup2(::_fileno(stderr), ::_fileno(stdout)) < 0) { std::_Exit(125); }
#else
	if (::dup2(::fileno(stderr), ::fileno(stdout)) < 0) { std::_Exit(125); }
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	GpuDirtyPageTrackerNotifyFaultHandlerInstalled();
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	GpuMemoryInit();

	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	GpuMemorySetAllocatedRange(mapping.address, mapping.size);
	ASSERT_TRUE(GpuDirtyPageTracker::Instance().Enabled());
	std::fprintf(stderr, "[dirty-tracking-evidence] preconditions enabled=1 mapping=set\n");
	std::fflush(stderr);
	*reinterpret_cast<uint8_t*>(mapping.address) = 0x11;
	g_create_race.write_value                    = 0x22;
	g_create_race.write_on_create                = true;

	GraphicContext      ctx {};
	const CreateRaceGpuObject info;
	void* const first = GpuMemoryCreateObject(1u, &ctx, nullptr, mapping.address, mapping.size, info);
	ASSERT_TRUE(first != nullptr);
	ASSERT_EQ(GpuDirtyPageTracker::Instance().Mode(mapping.address, mapping.size), GpuDirtyTrackingMode::PageFault);
	ASSERT_EQ(static_cast<unsigned>(*reinterpret_cast<const uint8_t*>(mapping.address)), 0x22u);
	std::fprintf(stderr, "[dirty-tracking-evidence] afterfirstcreation guest=%u\n",
	             static_cast<unsigned>(*reinterpret_cast<const uint8_t*>(mapping.address)));
	std::fflush(stderr);

	// A newer submit on the same exact range reuses the object. Its upload must
	// match the guest byte that the create-time write left behind.
	void* const second = GpuMemoryCreateObject(2u, &ctx, nullptr, mapping.address, mapping.size, info);
	ASSERT_EQ(second, first);
	const auto guest    = *reinterpret_cast<const uint8_t*>(mapping.address);
	const auto uploaded = static_cast<const UploadedByteBacking*>(second)->uploaded;
	std::fprintf(stderr, "[dirty-tracking-evidence] reuse uploaded=%u guest=%u\n", static_cast<unsigned>(uploaded),
	             static_cast<unsigned>(guest));
	std::fflush(stderr);
	EXPECT_EQ(static_cast<unsigned>(uploaded), static_cast<unsigned>(guest));

	GpuMemoryFree(&ctx, mapping.address, mapping.size);
}

} // namespace

TEST(EmulatorGraphicsDirtyTracking, NonStagedCreateWriteDuringCreateIsRefreshedOnReuse)
{
	ASSERT_EXIT({ RunNonStagedCreateRaceProbe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

namespace {

// Test-owned synthetic guest-memory state for the split-readable contract. The
// port callbacks are plain function pointers, so they read this file-scope state.
// Two adjacent single-page segments are CPU-readable; no single record covers both.
constexpr uint64_t kSplitFixtureAddress = 0x0000'1000'0000'0000ull;
constexpr uint32_t kSplitCpuReadWrite   = 0x3u; // production treats either low CPU bit as readable

struct SplitGuestSegment
{
	uint64_t base = 0;
	uint64_t size = 0;
};

std::array<SplitGuestSegment, 2> g_split_segments {};

const SplitGuestSegment* FindSplitSegment(uint64_t address)
{
	for (const auto& segment: g_split_segments)
	{
		if (segment.size != 0 && address >= segment.base && address - segment.base < segment.size)
		{
			return &segment;
		}
	}
	return nullptr;
}

bool SplitQueryMappedRange(uint64_t address, uint64_t size, Kyty::Emulator::GuestMemory::MappedRange* out)
{
	const SplitGuestSegment* segment = FindSplitSegment(address);
	// A request crossing the segment boundary is rejected, as the production query
	// rejects it: one record never describes both pages.
	if (segment == nullptr || out == nullptr || size == 0 || size > segment->size - (address - segment->base))
	{
		return false;
	}
	out->kind = Kyty::Emulator::GuestMemory::MappedRangeKind::Flexible;
	out->base = segment->base;
	out->size = segment->size;
	return true;
}

int SplitQueryProtection(void* address, void** start, void** end, int* protection)
{
	const SplitGuestSegment* segment = FindSplitSegment(reinterpret_cast<uint64_t>(address));
	if (segment == nullptr)
	{
		return -1;
	}
	if (start != nullptr)
	{
		*start = reinterpret_cast<void*>(segment->base);
	}
	// Inclusive end, as KernelQueryMemoryProtection reports it.
	if (end != nullptr)
	{
		*end = reinterpret_cast<void*>(segment->base + segment->size - 1u);
	}
	if (protection != nullptr)
	{
		*protection = static_cast<int>(kSplitCpuReadWrite);
	}
	return 0;
}

// Runs in a fresh process: the guest-memory port installs once per process.
void RunSplitReadableRefreshProbe()
{
	// Child stdout is not captured by the death test. Route it to stderr, unbuffered,
	// so the probe's diagnostics appear in the captured output.
	std::fflush(stdout);
#if defined(_WIN32)
	if (::_dup2(::_fileno(stderr), ::_fileno(stdout)) < 0) { std::_Exit(125); }
#else
	if (::dup2(::fileno(stderr), ::fileno(stdout)) < 0) { std::_Exit(125); }
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	GpuDirtyPageTrackerNotifyFaultHandlerInstalled();
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	GpuMemoryInit();

	const uint64_t page    = GetPageSize();
	const uint64_t size    = page * 2u;
	const uint64_t address = kSplitFixtureAddress;
	// Inside the host-heap window of GpuMemoryIsHostGuestMallocRange the port is
	// never consulted and the hash runs directly, which would mask this contract.
	ASSERT_FALSE(GpuMemoryIsHostGuestMallocRange(address, size));
	Kyty::Core::VirtualMemory::SharedBacking* const backing = CreateSharedBacking(size);
	ASSERT_TRUE(backing != nullptr);
	ASSERT_TRUE(MapSharedFixed(backing, address, 0, size, Mode::ReadWrite));
	GpuMemorySetAllocatedRange(address, size);
	ASSERT_TRUE(GpuDirtyPageTracker::Instance().Enabled());

	g_split_segments = {SplitGuestSegment {address, page}, SplitGuestSegment {address + page, page}};
	Kyty::Emulator::GuestMemory::Callbacks callbacks;
	callbacks.query_mapped_range = &SplitQueryMappedRange;
	callbacks.query_protection   = &SplitQueryProtection;
	auto& guest_memory           = Kyty::Emulator::GuestMemory::GetPort();
	ASSERT_TRUE(guest_memory.Install(callbacks));
	Kyty::Emulator::GuestMemory::MappedRange whole {};
	ASSERT_FALSE(guest_memory.QueryMappedRange(address, size, &whole));
	Kyty::Emulator::GuestMemory::MappedRange first_page {};
	ASSERT_TRUE(guest_memory.QueryMappedRange(address, 1u, &first_page));
	ASSERT_EQ(first_page.base, address);
	ASSERT_EQ(first_page.size, page);

	// The initial create copies 0x11 and performs no callback write.
	g_create_race.write_on_create = false;
	*reinterpret_cast<uint8_t*>(address) = 0x11;
	GraphicContext ctx {};
	const CreateRaceGpuObject info;
	void* const first = GpuMemoryCreateObject(1u, &ctx, nullptr, address, size, info);
	ASSERT_TRUE(first != nullptr);
	ASSERT_EQ(GpuDirtyPageTracker::Instance().Mode(address, size), GpuDirtyTrackingMode::PageFault);
	ASSERT_EQ(static_cast<unsigned>(static_cast<const UploadedByteBacking*>(first)->uploaded), 0x11u);

	// The tracker learns of the host write before the guest byte changes to 0x22.
	ASSERT_TRUE(GpuDirtyPageTracker::Instance().NotifyWrite(address, 1u));
	*reinterpret_cast<uint8_t*>(address) = 0x22;

	// A newer submit on the same exact range reuses the object. Every byte is
	// CPU-readable, so the refresh must upload the guest byte 0x22.
	void* const second = GpuMemoryCreateObject(2u, &ctx, nullptr, address, size, info);
	ASSERT_EQ(second, first);
	const auto guest    = *reinterpret_cast<const uint8_t*>(address);
	const auto uploaded = static_cast<const UploadedByteBacking*>(second)->uploaded;
	std::fprintf(stderr, "[dirty-tracking-evidence] split-readable-refresh uploaded=%u guest=%u\n", static_cast<unsigned>(uploaded),
	             static_cast<unsigned>(guest));
	std::fflush(stderr);
	EXPECT_EQ(static_cast<unsigned>(uploaded), static_cast<unsigned>(guest));

	GpuMemoryFree(&ctx, address, size);
	Free(address);
	DestroySharedBacking(backing);
}

} // namespace

TEST(EmulatorGraphicsDirtyTracking, SplitReadableMappingRefreshesOnReuse)
{
	ASSERT_EXIT({ RunSplitReadableRefreshProbe(); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

namespace {

// GpuMemoryCalcHash probes over two test-owned pages. Page 1 is CPU-readable; page 2
// is made NoAccess with the real VM API after the port is installed. A guard that
// checks protection only at the first byte and then hashes the whole span faults on
// page 2 instead of returning 0.
enum class HashSpanProbeMode : uint8_t
{
	// One port record describes both pages while the tail page is NoAccess.
	WholeRecordNoAccessTail,
	// The port has no record covering the tail page, so the cursor query fails there.
	// The protection query still reports the tail as CPU-readable. This is a deliberately
	// different mock, not native kernel metadata: it isolates the mapped-range guard from
	// the protection guard. The real OS tail page stays NoAccess, so a walker that ignores
	// the missing mapping and hashes the tail faults instead of returning 0.
	MetadataHoleNoAccessTail,
};

struct HashSpanPortState
{
	uint64_t base              = 0;
	uint64_t page              = 0;
	uint64_t record_size       = 0;
	bool     tail_cpu_readable = false;
};

HashSpanPortState g_hash_span {};

bool HashSpanQueryMappedRange(uint64_t address, uint64_t size, Kyty::Emulator::GuestMemory::MappedRange* out)
{
	const HashSpanPortState& s = g_hash_span;
	if (out == nullptr || size == 0 || address < s.base || address - s.base >= s.record_size ||
	    size > s.record_size - (address - s.base))
	{
		return false;
	}
	out->kind = Kyty::Emulator::GuestMemory::MappedRangeKind::Flexible;
	out->base = s.base;
	out->size = s.record_size;
	return true;
}

int HashSpanQueryProtection(void* address, void** start, void** end, int* protection)
{
	const HashSpanPortState& s    = g_hash_span;
	const auto               addr = reinterpret_cast<uint64_t>(address);
	if (addr < s.base || addr - s.base >= s.page * 2u)
	{
		return -1;
	}
	const bool     first     = addr - s.base < s.page;
	const uint64_t page_base = first ? s.base : s.base + s.page;
	if (start != nullptr)
	{
		*start = reinterpret_cast<void*>(page_base);
	}
	// Inclusive end, as KernelQueryMemoryProtection reports it.
	if (end != nullptr)
	{
		*end = reinterpret_cast<void*>(page_base + s.page - 1u);
	}
	if (protection != nullptr)
	{
		*protection = first || s.tail_cpu_readable ? static_cast<int>(kSplitCpuReadWrite) : 0;
	}
	return 0;
}

// Owns the two-page fixture of one hash-span probe. Teardown restores the tail page
// to ReadWrite before Free, so no protection outlives the probe.
struct HashSpanFixture
{
	uint64_t                                  address        = 0;
	uint64_t                                  page           = 0;
	Kyty::Core::VirtualMemory::SharedBacking* backing        = nullptr;
	bool                                      tail_protected = false;

	~HashSpanFixture()
	{
		if (tail_protected)
		{
			(void)Protect(address + page, page, Mode::ReadWrite);
		}
		if (address != 0)
		{
			(void)Free(address);
		}
		if (backing != nullptr)
		{
			DestroySharedBacking(backing);
		}
	}
};

// Runs in a fresh process: the guest-memory port installs once per process. These
// probes install no fault handler and enable no GPU tracking, so only the guard
// inside GpuMemoryCalcHash is under test. Outcome is the exit code; the stderr
// marker is debug evidence only.
void RunHashSpanProbe(HashSpanProbeMode mode)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	const uint64_t page    = GetPageSize();
	const uint64_t size    = page * 2u;
	const uint64_t address = kSplitFixtureAddress;
	// Outside the host-heap exemption, so GpuMemoryCalcHash must consult the port.
	ASSERT_FALSE(GpuMemoryIsHostGuestMallocRange(address, size));

	HashSpanFixture fixture;
	fixture.page    = page;
	fixture.backing = CreateSharedBacking(size);
	ASSERT_TRUE(fixture.backing != nullptr);
	ASSERT_TRUE(MapSharedFixed(fixture.backing, address, 0, size, Mode::ReadWrite));
	fixture.address                      = address;
	*reinterpret_cast<uint8_t*>(address) = 0x5a;

	g_hash_span.base        = address;
	g_hash_span.page        = page;
	g_hash_span.record_size       = mode == HashSpanProbeMode::WholeRecordNoAccessTail ? size : page;
	g_hash_span.tail_cpu_readable = mode == HashSpanProbeMode::MetadataHoleNoAccessTail;

	Kyty::Emulator::GuestMemory::Callbacks callbacks;
	callbacks.query_mapped_range = &HashSpanQueryMappedRange;
	callbacks.query_protection   = &HashSpanQueryProtection;
	auto& guest_memory           = Kyty::Emulator::GuestMemory::GetPort();
	ASSERT_TRUE(guest_memory.Install(callbacks));
	ASSERT_TRUE(guest_memory.IsInstalled());

	// Real VM state: the tail page moves from ReadWrite to NoAccess through the owned-page API.
	Mode old_mode = Mode::NoAccess;
	ASSERT_TRUE(Protect(address + page, page, Mode::NoAccess, &old_mode));
	fixture.tail_protected = true;
	ASSERT_EQ(old_mode, Mode::ReadWrite);

	// The probe must reach the branch under test. The first page is a valid query that
	// contains it, the tail protection matches the mode, and the metadata shape matches the mode.
	void* prot_start = nullptr;
	void* prot_end   = nullptr;
	int   protection = 0;
	ASSERT_EQ(guest_memory.QueryProtection(reinterpret_cast<void*>(address), &prot_start, &prot_end, &protection), 0);
	ASSERT_LE(reinterpret_cast<uint64_t>(prot_start), address);
	ASSERT_GE(reinterpret_cast<uint64_t>(prot_end), address + page - 1u);
	ASSERT_NE(protection & 0x3, 0);
	ASSERT_EQ(guest_memory.QueryProtection(reinterpret_cast<void*>(address + page), &prot_start, &prot_end, &protection), 0);
	const int expected_tail_protection = mode == HashSpanProbeMode::MetadataHoleNoAccessTail ? static_cast<int>(kSplitCpuReadWrite) : 0;
	ASSERT_EQ(protection, expected_tail_protection);
	Kyty::Emulator::GuestMemory::MappedRange mapped {};
	const bool whole_known  = guest_memory.QueryMappedRange(address, size, &mapped);
	const bool tail_known   = guest_memory.QueryMappedRange(address + page, 1u, &mapped);
	const bool whole_record = mode == HashSpanProbeMode::WholeRecordNoAccessTail;
	ASSERT_EQ(whole_known, whole_record);
	ASSERT_EQ(tail_known, whole_record);

	// Positive control: the same entry point still hashes the readable first page, so a
	// walker that refuses everything cannot satisfy the zero expectation below.
	EXPECT_NE(GpuMemoryCalcHash(GpuMemoryObjectType::VertexBuffer, reinterpret_cast<const uint8_t*>(address), page), 0u);

	// Whole span with a NoAccess tail. The refusal must return 0 before any byte is
	// hashed; a walker that hashes the span faults here rather than returning.
	const uint64_t hash = GpuMemoryCalcHash(GpuMemoryObjectType::VertexBuffer, reinterpret_cast<const uint8_t*>(address), size);
	std::fprintf(stderr, "[dirty-tracking-evidence] hash-span mode=%u hash=%llu\n", static_cast<unsigned>(mode),
	             static_cast<unsigned long long>(hash));
	std::fflush(stderr);
	EXPECT_EQ(hash, 0u);
}

} // namespace

TEST(EmulatorGraphicsDirtyTracking, HashSpanWalkerRefusesWholeRecordWithNoAccessTail)
{
	ASSERT_EXIT({ RunHashSpanProbe(HashSpanProbeMode::WholeRecordNoAccessTail); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorGraphicsDirtyTracking, HashSpanWalkerRefusesMetadataHoleAtNoAccessTail)
{
	ASSERT_EXIT({ RunHashSpanProbe(HashSpanProbeMode::MetadataHoleNoAccessTail); std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
}

UT_END();
