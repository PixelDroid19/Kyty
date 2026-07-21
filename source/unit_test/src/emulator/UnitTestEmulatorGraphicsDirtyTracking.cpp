#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

UT_BEGIN(EmulatorGraphicsDirtyTracking);

using Kyty::Core::VirtualMemory::CreateSharedBacking;
using Kyty::Core::VirtualMemory::DestroySharedBacking;
using Kyty::Core::VirtualMemory::Free;
using Kyty::Core::VirtualMemory::GetPageSize;
using Kyty::Core::VirtualMemory::MapSharedAligned;
using Kyty::Core::VirtualMemory::Mode;
using Kyty::Libs::Graphics::GpuDirtyPageProtectionOps;
using Kyty::Libs::Graphics::GpuDirtyPageTracker;
using Kyty::Libs::Graphics::GpuDirtyProtectionState;
using Kyty::Libs::Graphics::GpuDirtyProtectionStateHandlesFault;
using Kyty::Libs::Graphics::GpuDirtyProtectionStateNeedsArmingRollback;
using Kyty::Libs::Graphics::GpuDirtyTrackingEnabledForProcess;
using Kyty::Libs::Graphics::GpuDirtyTrackingMode;
using Kyty::Libs::Graphics::GpuMemoryCheckAccessViolation;
using Kyty::Libs::Graphics::GpuMemoryNotifyHostWrite;

namespace {

TEST(EmulatorGraphicsDirtyTracking, ArmingWindowIsHandledAsTrackerFault)
{
	EXPECT_FALSE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Writable));
	EXPECT_FALSE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Capturing));
	EXPECT_TRUE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Arming));
	EXPECT_TRUE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Armed));
	EXPECT_TRUE(GpuDirtyProtectionStateHandlesFault(GpuDirtyProtectionState::Disarming));
	EXPECT_TRUE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::Writable));
	EXPECT_FALSE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::Capturing));
	EXPECT_FALSE(GpuDirtyProtectionStateNeedsArmingRollback(GpuDirtyProtectionState::Arming));
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
	GpuDirtyPageTracker*        tracker           = nullptr;
	uintptr_t                   fault_address     = 0;
	uintptr_t                   fault_before_capture_address = 0;
	uintptr_t                   notify_before_capture_address = 0;
	uint32_t                    signal_safe_calls = 0;
	std::atomic<bool>           fail_next_protect {false};
	std::atomic<bool>           block_next_protect {false};
	std::atomic<bool>           protect_entered {false};
	std::atomic<bool>           release_protect {false};
	std::atomic<bool>           protect_completed {false};
	std::atomic<bool>           block_signal_safe {false};
	std::atomic<bool>           signal_safe_entered {false};
	std::atomic<bool>           release_signal_safe {false};
	std::atomic<uint32_t>       fail_signal_safe_call {0};
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
		auto* self = static_cast<FakeProtection*>(context);
		const size_t first = (address - self->base) / self->page_size;
		const auto bits = static_cast<uint32_t>(self->original_modes[first]) & ~static_cast<uint32_t>(Mode::Write);
		const Mode mode = static_cast<Mode>(bits == 0u ? static_cast<uint32_t>(Mode::Read) : bits);
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
			(void)self->tracker->HandleWriteFault(fault);
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
		const Mode mode = self->original_modes[first];
		self->calls.push_back({address, size, mode, restore_token});
		for (size_t page = 0; page < pages; page++)
		{
			self->current_modes[first + page] = mode;
		}
		return true;
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

TEST(EmulatorGraphicsDirtyTracking, FirstArmPublishesNativeProtectionBeforeWriteCanFault)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	FakeProtection protection {mapping.address, GetPageSize(), {Mode::ExecuteReadWrite}};
	protection.original_tokens = {0x407u};
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker = &tracker;
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
	EXPECT_FALSE(tracker.HandleWriteFault(mapping.address + GetPageSize()));
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

	ASSERT_TRUE(tracker.NotifyWrite(mapping.address, 1u));
	const uint64_t after_write = tracker.SnapshotGeneration(mapping.address, mapping.size);
	EXPECT_GT(after_write, observation.generation);
	EXPECT_TRUE(tracker.ChangedSince(mapping.address, mapping.size, observation.generation));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
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

TEST(EmulatorGraphicsDirtyTracking, InvalidRangeFallsBack)
{
	GpuDirtyPageTracker tracker;
	EXPECT_FALSE(tracker.RegisterRange(0, 1));
	EXPECT_EQ(tracker.Mode(0, 1), GpuDirtyTrackingMode::HashFallback);
}

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

UT_END();
