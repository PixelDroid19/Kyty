#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include "Kyty/Sys/SysWriteLease.h"

#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/VideoFrameMemory.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <unistd.h>
#endif

UT_BEGIN(EmulatorGraphicsDirtyTracking);

using Kyty::Core::VirtualMemory::Alloc;
using Kyty::Core::VirtualMemory::AllocFixed;
using Kyty::Core::VirtualMemory::CreateSharedBacking;
using Kyty::Core::VirtualMemory::DestroySharedBacking;
using Kyty::Core::VirtualMemory::Free;
using Kyty::Core::VirtualMemory::IsRangeReadable;
using Kyty::Core::VirtualMemory::IsRangeWritable;
using Kyty::Core::VirtualMemory::ProtectGuest;
using Kyty::Core::VirtualMemory::GetPageSize;
using Kyty::Core::VirtualMemory::MapSharedAligned;
using Kyty::Core::VirtualMemory::Mode;
using Kyty::Libs::Graphics::GpuDirtyPageProtectionOps;
using Kyty::Libs::Graphics::GpuDirtyPageTableIndex;
using Kyty::Libs::Graphics::GpuDirtyPageTracker;
using Kyty::Libs::Graphics::GpuDirtyProtectionState;
using Kyty::Libs::Graphics::GpuDirtyProtectionStateHandlesFault;
using Kyty::Libs::Graphics::GpuDirtyProtectionStateNeedsArmingRollback;
using Kyty::Libs::Graphics::GpuDirtyTrackingEnabledForProcess;
using Kyty::Libs::Graphics::GetGpuDirtyPageTracker;
using Kyty::Libs::Graphics::GpuDirtyPageTrackerHandleAccessFault;
using Kyty::Libs::Graphics::GpuDirtyPageTrackerNotifyFaultHandlerInstalled;
using Kyty::Libs::Graphics::GpuDirtyTrackingMode;
using Kyty::Libs::Graphics::GpuMemoryCheckAccessViolation;
using Kyty::Libs::Graphics::GpuMemoryNotifyHostWrite;
using Access = Kyty::Core::VirtualMemory::ExceptionHandler::AccessViolationType;

namespace {

// Whether the host mapping containing `address` is writable, from the kernel's
// own view of the process. Hosts without that view report `fallback`, which
// keeps the assertion neutral there.
bool NativeWritable(uint64_t address, bool fallback)
{
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	(void)fallback;
	std::ifstream maps("/proc/self/maps");
	std::string   line;
	while (std::getline(maps, line))
	{
		uint64_t begin     = 0;
		uint64_t end       = 0;
		char     perms[5]  = {};
		if (std::sscanf(line.c_str(), "%" SCNx64 "-%" SCNx64 " %4s", &begin, &end, perms) == 3 && address >= begin && address < end)
		{
			return perms[1] == 'w';
		}
	}
	return false;
#else
	(void)address;
	return fallback;
#endif
}

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
	bool                        fail_release            = false;
	// The tracker's lease authority, as handed to the last write removal.
	const Core::VirtualMemory::WriteLeaseAuthority* authority = nullptr;

	FakeProtection(uintptr_t address, size_t size, std::vector<Mode> modes)
	    : base(address), page_size(size), original_modes(std::move(modes)), current_modes(original_modes)
	{
		for (const auto mode: original_modes)
		{
			original_tokens.push_back(static_cast<uint32_t>(mode));
		}
	}

	static bool RemoveWrite(void* context, uintptr_t address, size_t size, uint32_t restore_token,
	                        const Core::VirtualMemory::WriteLeaseAuthority* authority) noexcept
	{
		auto*        self  = static_cast<FakeProtection*>(context);
		self->authority    = authority;
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
	    void* visitor_context, const Core::VirtualMemory::WriteLeaseAuthority* authority) noexcept
	{
		auto* self      = static_cast<FakeProtection*>(context);
		self->authority = authority;
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

	static bool ReleaseLeases(void* context, const Core::VirtualMemory::WriteLeaseAuthority* /*authority*/) noexcept
	{
		return !static_cast<FakeProtection*>(context)->fail_release;
	}

	[[nodiscard]] GpuDirtyPageProtectionOps Ops() noexcept
	{
		return {this, &RemoveWriteAndCapture, &RemoveWrite, &Restore, &RestoreSignalSafe, &ReleaseLeases};
	}
};


// Native protection over a FakeProtection's pages, for driving a test-owned
// write-lease registry with the tracker as its authority. One chosen step can
// fail, either after changing its first page or without changing anything.
struct FakeLeaseNative
{
	static inline FakeProtection* protection = nullptr;
	static inline uint32_t        calls      = 0;
	static inline uint32_t        fail_call  = 0;
	static inline bool            partial    = true;

	static void Use(FakeProtection* target, uint32_t failing_call, bool partially = true)
	{
		protection = target;
		calls      = 0;
		fail_call  = failing_call;
		partial    = partially;
	}

	static bool Protect(uint64_t address, uint64_t size, uint32_t token) noexcept
	{
		calls++;
		const bool   fail    = calls == fail_call;
		const size_t first   = (address - protection->base) / protection->page_size;
		const size_t pages   = size / protection->page_size;
		const size_t changed = fail ? (partial ? 1u : 0u) : pages;
		for (size_t page = 0; page < changed; page++)
		{
			protection->current_modes[first + page] = static_cast<Mode>(token);
		}
		return !fail;
	}

	static uint32_t RemoveWrite(uint32_t token) noexcept
	{
		const auto write = static_cast<uint32_t>(Mode::Write);
		if ((token & write) == 0u)
		{
			return token;
		}
		const uint32_t bits = token & ~write;
		return bits == 0u ? static_cast<uint32_t>(Mode::Read) : bits;
	}

	static constexpr Kyty::Core::SysWriteLeaseNative kNative {&Protect, &RemoveWrite};
};

#if defined(_WIN32)
constexpr int kExitStatus = 321;
#else
constexpr int kExitStatus = 65;
#endif

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

// Range admission is bounded by page metadata, not by a fixed range count.
// Each disjoint range keeps exact generation evidence for its own pages.
TEST(EmulatorGraphicsDirtyTracking, RegistersMoreThanFiveHundredTwelveDisjointRanges)
{
	constexpr uint64_t kRanges = 600;
	Mapping            mapping(kRanges);
	ASSERT_NE(mapping.address, 0u);
	const uint64_t      page_size = GetPageSize();
	GpuDirtyPageTracker tracker;
	for (uint64_t i = 0; i < kRanges; i++)
	{
		ASSERT_TRUE(tracker.RegisterRange(mapping.address + i * page_size, page_size)) << "range " << i;
	}
	ASSERT_TRUE(tracker.PrepareForRead(mapping.address, mapping.size));
	std::vector<uint64_t> snapshots(kRanges);
	for (uint64_t i = 0; i < kRanges; i++)
	{
		EXPECT_EQ(tracker.Mode(mapping.address + i * page_size, page_size), GpuDirtyTrackingMode::PageFault) << "range " << i;
		snapshots[i] = tracker.SnapshotGeneration(mapping.address + i * page_size, page_size);
	}

	constexpr uint64_t kWritten = 550;
	ASSERT_TRUE(tracker.NotifyWrite(mapping.address + kWritten * page_size, 1u));
	EXPECT_TRUE(tracker.ChangedSince(mapping.address + kWritten * page_size, page_size, snapshots[kWritten]));
	EXPECT_FALSE(tracker.ChangedSince(mapping.address + (kWritten - 1u) * page_size, page_size, snapshots[kWritten - 1u]));
	EXPECT_FALSE(tracker.ChangedSince(mapping.address + (kWritten + 1u) * page_size, page_size, snapshots[kWritten + 1u]));
	EXPECT_FALSE(tracker.ChangedSince(mapping.address, page_size, snapshots[0]));

	for (uint64_t i = 0; i < kRanges; i++)
	{
		EXPECT_TRUE(tracker.UnregisterRange(mapping.address + i * page_size, page_size)) << "range " << i;
	}
}

// The tracker may restore only the write permission it removed. A guest
// protection change while a page is armed is authoritative: neither a write
// fault nor the final unregister may re-enable a write the guest denied.
TEST(EmulatorGraphicsDirtyTracking, GuestProtectionChangeWhileArmedIsNotRestored)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Kyty::Core::VirtualMemory::Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	{
		GpuDirtyPageTracker tracker;
		ASSERT_TRUE(tracker.RegisterRange(address, page_size));
		ASSERT_TRUE(tracker.Rearm(address, page_size));
		ASSERT_TRUE(Kyty::Core::VirtualMemory::ProtectGuest(address, page_size, Mode::Read));

		EXPECT_FALSE(tracker.HandleWriteFault(address));
		(void)tracker.UnregisterRange(address, page_size);
		EXPECT_FALSE(Kyty::Core::VirtualMemory::IsRangeWritable(address, page_size));
	}
	EXPECT_TRUE(Free(address));
}

// A guest change to an executable read-only protection is equally authoritative:
// the write fault is the guest's, and no evidence or permission is invented.
TEST(EmulatorGraphicsDirtyTracking, GuestExecuteReadProtectionWhileArmedDeniesTheWrite)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	{
		GpuDirtyPageTracker tracker;
		ASSERT_TRUE(tracker.RegisterRange(address, page_size));
		ASSERT_TRUE(tracker.Rearm(address, page_size));
		const uint64_t before = tracker.SnapshotGeneration(address, page_size);
		ASSERT_TRUE(ProtectGuest(address, page_size, Mode::ExecuteRead));

		EXPECT_FALSE(tracker.HandleWriteFault(address));
		EXPECT_FALSE(tracker.ChangedSince(address, page_size, before));
		EXPECT_TRUE(IsRangeReadable(address, page_size));
		EXPECT_FALSE(IsRangeWritable(address, page_size));
		EXPECT_TRUE(tracker.UnregisterRange(address, page_size));
		EXPECT_FALSE(IsRangeWritable(address, page_size));
		EXPECT_FALSE(NativeWritable(address, false));
	}
	EXPECT_TRUE(Free(address));
}

// A writable re-protection over an armed page keeps write removed there, so
// tracking continues, while untracked neighbours receive the guest protection.
TEST(EmulatorGraphicsDirtyTracking, WritableReprotectWhileArmedKeepsTrackingAndSparesNeighbours)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size * 3u, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	const uint64_t tracked = address + page_size;
	{
		GpuDirtyPageTracker tracker;
		ASSERT_TRUE(tracker.RegisterRange(tracked, page_size));
		const auto observation = tracker.BeginRead(tracked, page_size);
		ASSERT_TRUE(observation.tracked);

		ASSERT_TRUE(ProtectGuest(address, page_size * 3u, Mode::ReadWrite));
		EXPECT_FALSE(NativeWritable(tracked, false));
		EXPECT_TRUE(NativeWritable(address, true));
		EXPECT_TRUE(NativeWritable(address + page_size * 2u, true));
		EXPECT_TRUE(IsRangeWritable(address, page_size * 3u));
		EXPECT_TRUE(tracker.ReadObservationIsStable(tracked, page_size, observation));

		ASSERT_TRUE(tracker.HandleWriteFault(tracked));
		*reinterpret_cast<volatile uint8_t*>(tracked) = 0x5a;
		EXPECT_TRUE(tracker.ChangedSince(tracked, page_size, observation.generation));
		EXPECT_TRUE(tracker.UnregisterRange(tracked, page_size));
	}
	EXPECT_TRUE(Free(address));
}

// Unmapping an armed page revokes its token: a fault on a new mapping at the
// same address is not the tracker's, and a writable remap is captured afresh.
TEST(EmulatorGraphicsDirtyTracking, UnmapRevokesArmedTokensAndRemapIsRecaptured)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(address, page_size));
	ASSERT_TRUE(tracker.Rearm(address, page_size));

	ASSERT_TRUE(Free(address));
	ASSERT_TRUE(AllocFixed(address, page_size, Mode::Read));
	EXPECT_FALSE(tracker.HandleWriteFault(address));
	EXPECT_FALSE(NativeWritable(address, false));
	EXPECT_FALSE(IsRangeWritable(address, page_size));

	ASSERT_TRUE(ProtectGuest(address, page_size, Mode::ReadWrite));
	ASSERT_TRUE(tracker.Rearm(address, page_size));
	EXPECT_FALSE(NativeWritable(address, false));
	ASSERT_TRUE(tracker.HandleWriteFault(address));
	EXPECT_TRUE(NativeWritable(address, true));
	EXPECT_TRUE(tracker.UnregisterRange(address, page_size));
	EXPECT_TRUE(NativeWritable(address, true));
	EXPECT_TRUE(Free(address));
}

// Restores race guest protection changes from another thread. Every restore
// either completes before a change or reads the changed token, so once the
// guest's last change denies writes nothing makes the page writable again.
TEST(EmulatorGraphicsDirtyTracking, ConcurrentRestoresNeverOutliveAGuestProtectionChange)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	{
		GpuDirtyPageTracker tracker;
		ASSERT_TRUE(tracker.RegisterRange(address, page_size));
		ASSERT_TRUE(tracker.Rearm(address, page_size));
		std::atomic<bool> stop {false};
		std::thread       restorer(
		    [&]
		    {
			    while (!stop.load(std::memory_order_acquire))
			    {
				    (void)tracker.HandleWriteFault(address);
				    (void)tracker.NotifyWrite(address, 1u);
				    (void)tracker.Rearm(address, page_size);
			    }
		    });
		bool protected_all = true;
		for (uint32_t i = 0; i < 2000u; i++)
		{
			protected_all = ProtectGuest(address, page_size, (i & 1u) == 0u ? Mode::Read : Mode::ReadWrite) && protected_all;
		}
		protected_all = ProtectGuest(address, page_size, Mode::Read) && protected_all;
		for (uint32_t i = 0; i < 200u; i++)
		{
			std::this_thread::yield();
		}
		stop.store(true, std::memory_order_release);
		restorer.join();

		EXPECT_TRUE(protected_all);
		EXPECT_FALSE(tracker.HandleWriteFault(address));
		EXPECT_FALSE(IsRangeWritable(address, page_size));
		EXPECT_FALSE(NativeWritable(address, false));
		(void)tracker.UnregisterRange(address, page_size);
		EXPECT_FALSE(NativeWritable(address, false));
	}
	EXPECT_TRUE(Free(address));
}

// Write tracking removes only write access, so a read or execute fault on an
// armed page is never the tracker's and leaves its evidence untouched.
TEST(EmulatorGraphicsDirtyTracking, ReadAndExecuteFaultsAreNeverClaimedByWriteTracking)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, mapping.size));
	const auto observation = tracker.BeginRead(mapping.address, mapping.size);
	ASSERT_TRUE(observation.tracked);

	EXPECT_FALSE(tracker.HandleAccessFault(mapping.address, Access::Read));
	EXPECT_FALSE(tracker.HandleAccessFault(mapping.address, Access::Execute));
	EXPECT_TRUE(tracker.ReadObservationIsStable(mapping.address, mapping.size, observation));
	EXPECT_FALSE(NativeWritable(mapping.address, false));

	EXPECT_TRUE(tracker.HandleAccessFault(mapping.address, Access::Write));
	EXPECT_FALSE(tracker.ReadObservationIsStable(mapping.address, mapping.size, observation));
	ASSERT_TRUE(tracker.Rearm(mapping.address, mapping.size));
	// Hosts that cannot classify the access report Unknown, handled as a write.
	EXPECT_TRUE(tracker.HandleAccessFault(mapping.address, Access::Unknown));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, mapping.size));
}

// Fully covered 64-page blocks answer from their summaries; pages in partially
// covered edge blocks are checked one by one, so a neighbour's write in the
// same block never reaches the range.
TEST(EmulatorGraphicsDirtyTracking, BlockSummariesKeepNeighbourWritesOutOfARange)
{
	constexpr uint64_t kPages = 224;
	Mapping            mapping(kPages);
	ASSERT_NE(mapping.address, 0u);
	const uint64_t page_size = GetPageSize();
	uint64_t       start     = mapping.address / page_size + 1u;
	while (start % 64u != 32u)
	{
		start++;
	}
	const uint64_t range   = start * page_size;
	const uint64_t size    = 150u * page_size;
	const uint64_t before  = range - page_size;
	const uint64_t after   = range + size;
	const uint64_t inside  = range + 40u * page_size;
	const uint64_t edge    = range + 149u * page_size;
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(range, size));
	ASSERT_TRUE(tracker.RegisterRange(before, page_size));
	ASSERT_TRUE(tracker.RegisterRange(after, page_size));
	const auto observed = tracker.BeginRead(range, size);
	ASSERT_TRUE(observed.tracked);
	ASSERT_TRUE(tracker.BeginRead(before, page_size).tracked);
	ASSERT_TRUE(tracker.BeginRead(after, page_size).tracked);
	EXPECT_EQ(tracker.Mode(range, size), GpuDirtyTrackingMode::PageFault);

	ASSERT_TRUE(tracker.NotifyWrite(before + page_size - 1u, 1u));
	ASSERT_TRUE(tracker.NotifyWrite(after, 1u));
	EXPECT_FALSE(tracker.ChangedSince(range, size, observed.generation));

	ASSERT_TRUE(tracker.NotifyWrite(inside, 1u));
	EXPECT_TRUE(tracker.ChangedSince(range, size, observed.generation));
	const uint64_t interior = tracker.SnapshotGeneration(range, size);
	ASSERT_TRUE(tracker.NotifyWrite(edge, 1u));
	EXPECT_TRUE(tracker.ChangedSince(range, size, interior));

	EXPECT_TRUE(tracker.UnregisterRange(range, size));
	EXPECT_TRUE(tracker.UnregisterRange(before, page_size));
	EXPECT_TRUE(tracker.UnregisterRange(after, page_size));
}

// A protection change that fails part way returns every run, leased or not, to
// the protection it had, and the tracker publishes nothing: its armed page
// still restores the old token on the next fault. A committed change replaces
// the tokens and keeps the armed page's write removed.
TEST(EmulatorGraphicsDirtyTracking, FailedLeasedProtectionRollsBackEveryRunAndPublishesNothing)
{
	Mapping mapping(4);
	ASSERT_NE(mapping.address, 0u);
	const uint64_t      page_size = GetPageSize();
	FakeProtection      protection(mapping.address, page_size, std::vector<Mode>(4, Mode::ReadWrite));
	GpuDirtyPageTracker tracker(protection.Ops());
	protection.tracker   = &tracker;
	const uint64_t armed = mapping.address + page_size;
	const uint64_t idle  = mapping.address + page_size * 2u;
	ASSERT_TRUE(tracker.RegisterRange(armed, page_size));
	ASSERT_TRUE(tracker.RegisterRange(idle, page_size));
	ASSERT_TRUE(tracker.Rearm(armed, page_size));
	ASSERT_TRUE(tracker.Rearm(idle, page_size));
	ASSERT_TRUE(tracker.HandleWriteFault(idle));
	ASSERT_NE(protection.authority, nullptr);
	ASSERT_EQ(protection.current_modes, (std::vector<Mode> {Mode::ReadWrite, Mode::Read, Mode::ReadWrite, Mode::ReadWrite}));

	const auto                            read_write = static_cast<uint32_t>(Mode::ReadWrite);
	const auto                            read       = static_cast<uint32_t>(Mode::Read);
	Kyty::Core::SysWriteLeases            leases;
	const std::vector<Kyty::Core::SysProtectionSpan> spans {{mapping.address, mapping.address + mapping.size, read_write, true}};
	leases.Add(armed, idle + page_size, read_write, protection.authority);

	// Steps: page 0, the armed page, the idle page (fails after changing it), page 3.
	FakeLeaseNative::Use(&protection, 3u);
	EXPECT_FALSE(leases.Protect(spans, read, Mode::Read, FakeLeaseNative::kNative));
	EXPECT_EQ(protection.current_modes, (std::vector<Mode> {Mode::ReadWrite, Mode::Read, Mode::ReadWrite, Mode::ReadWrite}));
	Kyty::Core::SysWriteLeaseRun run;
	ASSERT_TRUE(leases.Find(armed, &run));
	EXPECT_EQ(run.guest_token, read_write);
	EXPECT_TRUE(tracker.HandleWriteFault(armed));
	EXPECT_EQ(protection.current_modes[1], Mode::ReadWrite);

	ASSERT_TRUE(tracker.Rearm(armed, page_size));
	FakeLeaseNative::Use(&protection, 0u);
	EXPECT_TRUE(leases.Protect(spans, read, Mode::Read, FakeLeaseNative::kNative));
	EXPECT_EQ(protection.current_modes, std::vector<Mode>(4, Mode::Read));
	ASSERT_TRUE(leases.Find(armed, &run));
	EXPECT_EQ(run.guest_token, read);
	EXPECT_FALSE(tracker.HandleWriteFault(armed));
	EXPECT_FALSE(tracker.HandleWriteFault(idle));
	EXPECT_EQ(protection.current_modes[1], Mode::Read);

	EXPECT_TRUE(leases.Release(protection.authority, FakeLeaseNative::kNative));
	EXPECT_TRUE(tracker.UnregisterRange(armed, page_size));
	EXPECT_TRUE(tracker.UnregisterRange(idle, page_size));
}

// A fault in flight owns its page's identity: neither a slot that changes
// identity (unregistration) nor a mapping removal can complete until the fault
// left, and afterwards the revoked page is never restored again while a
// re-registered page is tracked afresh.
TEST(EmulatorGraphicsDirtyTracking, SlotAndMappingChangesWaitForAFaultInFlight)
{
	Mapping mapping(2);
	ASSERT_NE(mapping.address, 0u);
	const uint64_t      page_size = GetPageSize();
	FakeProtection      protection(mapping.address, page_size, std::vector<Mode>(2, Mode::ReadWrite));
	GpuDirtyPageTracker tracker(protection.Ops());
	const uint64_t      faulting = mapping.address;
	const uint64_t      recycled = mapping.address + page_size;
	ASSERT_TRUE(tracker.RegisterRange(faulting, page_size));
	ASSERT_TRUE(tracker.RegisterRange(recycled, page_size));
	ASSERT_TRUE(tracker.Rearm(faulting, page_size));
	ASSERT_NE(protection.authority, nullptr);
	Kyty::Core::SysWriteLeases leases;
	leases.Add(faulting, faulting + page_size, static_cast<uint32_t>(Mode::ReadWrite), protection.authority);

	protection.block_signal_safe.store(true);
	bool        fault_result = false;
	std::thread fault([&] { fault_result = tracker.HandleWriteFault(faulting); });
	while (!protection.signal_safe_entered.load())
	{
		std::this_thread::yield();
	}

	std::atomic<bool>                          unregistered {false};
	std::atomic<bool>                          fenced {false};
	std::vector<Kyty::Core::SysWriteLeaseRun> unmapped;
	std::thread unregister([&]
	                       {
		                       EXPECT_TRUE(tracker.UnregisterRange(recycled, page_size));
		                       unregistered.store(true);
	                       });
	std::thread unmap([&]
	                  {
		                  leases.BeginUnmap(faulting, faulting + page_size, &unmapped);
		                  fenced.store(true);
	                  });
	for (uint32_t spin = 0; spin < 20000u; spin++)
	{
		std::this_thread::yield();
	}
	EXPECT_FALSE(unregistered.load());
	EXPECT_FALSE(fenced.load());

	protection.release_signal_safe.store(true);
	fault.join();
	unregister.join();
	unmap.join();
	EXPECT_TRUE(fault_result);
	EXPECT_TRUE(unregistered.load());
	leases.EndUnmap(faulting, faulting + page_size, unmapped, true);
	EXPECT_FALSE(leases.Find(faulting, nullptr));

	// The revoked page is not the tracker's any more.
	const uint32_t restores = protection.signal_safe_calls;
	EXPECT_FALSE(tracker.HandleWriteFault(faulting));
	EXPECT_EQ(protection.signal_safe_calls, restores);

	// The recycled page is registered and armed afresh.
	ASSERT_TRUE(tracker.RegisterRange(recycled, page_size));
	const auto observation = tracker.BeginRead(recycled, page_size);
	ASSERT_TRUE(observation.tracked);
	EXPECT_TRUE(tracker.HandleWriteFault(recycled));
	EXPECT_TRUE(tracker.ChangedSince(recycled, page_size, observation.generation));
	EXPECT_TRUE(tracker.UnregisterRange(recycled, page_size));
	EXPECT_TRUE(tracker.UnregisterRange(faulting, page_size));
}

// A lease whose guest protection cannot be reapplied at release stays write
// protected and is never reported to the released authority again: the next
// guest change applies the guest protection on its own.
TEST(EmulatorGraphicsDirtyTracking, FailedLeaseReleaseDetachesTheAuthorityAndKeepsWriteRemoved)
{
	Mapping mapping(1);
	ASSERT_NE(mapping.address, 0u);
	const uint64_t      page_size = GetPageSize();
	FakeProtection      protection(mapping.address, page_size, {Mode::ReadWrite});
	GpuDirtyPageTracker tracker(protection.Ops());
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, page_size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, page_size));
	ASSERT_NE(protection.authority, nullptr);
	const auto                 read_write = static_cast<uint32_t>(Mode::ReadWrite);
	Kyty::Core::SysWriteLeases leases;
	leases.Add(mapping.address, mapping.address + page_size, read_write, protection.authority);

	FakeLeaseNative::Use(&protection, 1u, false);
	EXPECT_FALSE(leases.Release(protection.authority, FakeLeaseNative::kNative));
	EXPECT_EQ(protection.current_modes[0], Mode::Read);
	Kyty::Core::SysWriteLeaseRun run;
	ASSERT_TRUE(leases.Find(mapping.address, &run));
	EXPECT_EQ(run.authority, nullptr);

	// Were the tracker still consulted, its armed page would keep write removed.
	FakeLeaseNative::Use(&protection, 0u);
	EXPECT_TRUE(leases.Protect({{mapping.address, mapping.address + page_size, read_write, true}}, read_write, Mode::ReadWrite,
	                           FakeLeaseNative::kNative));
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_FALSE(leases.Find(mapping.address, nullptr));
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, page_size));
}

// A tracker that cannot give its leased pages back must not disappear quietly:
// those pages would stay write protected with nobody to restore them.
TEST(EmulatorGraphicsDirtyTracking, TrackerTeardownFailsWhenLeasedPagesCannotBeRestored)
{
	EXPECT_EXIT(
	    {
		    Mapping        mapping(1);
		    FakeProtection protection(mapping.address, GetPageSize(), {Mode::ReadWrite});
		    protection.fail_release = true;
		    {
			    GpuDirtyPageTracker tracker(protection.Ops());
			    (void)tracker.RegisterRange(mapping.address, mapping.size);
			    (void)tracker.Rearm(mapping.address, mapping.size);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(kExitStatus), "");
}

// While a fence is held the handler never waits. A fault the tracker owns is
// reported handled without any change so the access runs again; a fault on a
// page whose guest protection denies the write, or on a page the tracker never
// armed, stays the guest's.
TEST(EmulatorGraphicsDirtyTracking, FencedFaultsRetryOnlyWhenTheTrackerOwnsThePermission)
{
	Mapping mapping(3);
	ASSERT_NE(mapping.address, 0u);
	const uint64_t      page_size = GetPageSize();
	FakeProtection      protection(mapping.address, page_size, std::vector<Mode>(3, Mode::ReadWrite));
	GpuDirtyPageTracker tracker(protection.Ops());
	const uint64_t      owned    = mapping.address;
	const uint64_t      denied   = mapping.address + page_size;
	const uint64_t      unarmed  = mapping.address + page_size * 2u;
	for (const auto page: {owned, denied, unarmed})
	{
		ASSERT_TRUE(tracker.RegisterRange(page, page_size));
	}
	ASSERT_TRUE(tracker.Rearm(owned, page_size));
	ASSERT_TRUE(tracker.Rearm(denied, page_size));
	ASSERT_NE(protection.authority, nullptr);
	const auto                 read_write = static_cast<uint32_t>(Mode::ReadWrite);
	Kyty::Core::SysWriteLeases leases;
	leases.Add(owned, denied + page_size, read_write, protection.authority);
	FakeLeaseNative::Use(&protection, 0u);
	ASSERT_TRUE(leases.Protect({{denied, denied + page_size, read_write, true}}, static_cast<uint32_t>(Mode::Read), Mode::Read,
	                           FakeLeaseNative::kNative));
	const auto observation = tracker.BeginRead(owned, page_size);
	ASSERT_TRUE(observation.tracked);

	// Holding the fence of a pending mapping change.
	std::vector<Kyty::Core::SysWriteLeaseRun> fenced;
	leases.BeginUnmap(owned, owned + page_size, &fenced);
	const uint32_t restores = protection.signal_safe_calls;
	EXPECT_TRUE(tracker.HandleWriteFault(owned));
	EXPECT_FALSE(tracker.HandleWriteFault(denied));
	EXPECT_FALSE(tracker.HandleWriteFault(unarmed));
	EXPECT_EQ(protection.signal_safe_calls, restores);
	EXPECT_EQ(protection.current_modes[0], Mode::Read);
	leases.EndUnmap(owned, owned + page_size, fenced, false);

	EXPECT_TRUE(tracker.ReadObservationIsStable(owned, page_size, observation));
	EXPECT_TRUE(tracker.HandleWriteFault(owned));
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_FALSE(tracker.ReadObservationIsStable(owned, page_size, observation));
	EXPECT_TRUE(leases.Release(protection.authority, FakeLeaseNative::kNative));
	for (const auto page: {owned, denied, unarmed})
	{
		EXPECT_TRUE(tracker.UnregisterRange(page, page_size));
	}
}

// The process-wide tracker exists only after the startup thread reports the
// fault handler installed. Before that the fault route creates nothing and
// claims nothing, and early normal callers get a disabled tracker that does
// not decide the published one. Runs in its own process: publication is
// permanent by design.
TEST(EmulatorGraphicsDirtyTracking, ProcessTrackerIsPublishedOnlyAfterFaultHandlerInstallation)
{
	EXPECT_EXIT(
	    {
		    Mapping mapping(1);
		    bool    ok = mapping.address != 0u;
		    ok         = ok && !GpuDirtyPageTrackerHandleAccessFault(mapping.address, Access::Write);
		    auto& early = GetGpuDirtyPageTracker();
		    ok          = ok && !early.Enabled() && !early.RegisterRange(mapping.address, mapping.size);

		    GpuDirtyPageTrackerNotifyFaultHandlerInstalled();
		    auto&      published = GetGpuDirtyPageTracker();
		    const bool expected  = GpuDirtyTrackingEnabledForProcess(std::getenv("KYTY_DISABLE_GPU_DIRTY_TRACKING"), true);
		    ok = ok && &published != &early && &GpuDirtyPageTracker::Instance() == &published && published.Enabled() == expected;
		    GpuDirtyPageTrackerNotifyFaultHandlerInstalled();
		    ok = ok && &GetGpuDirtyPageTracker() == &published;
		    if (ok && expected)
		    {
			    ok = published.RegisterRange(mapping.address, mapping.size) && published.Rearm(mapping.address, mapping.size) &&
			         !GpuDirtyPageTrackerHandleAccessFault(mapping.address, Access::Read) &&
			         GpuDirtyPageTrackerHandleAccessFault(mapping.address, Access::Write) &&
			         published.UnregisterRange(mapping.address, mapping.size);
		    }
		    std::_Exit(ok ? 0 : 1);
	    },
	    ::testing::ExitedWithCode(0), "");
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
	uint64_t   page_before = 0;
	ASSERT_TRUE(tracker.PageGenerations(mapping.address, 64, &page_before, 1u));
	const auto restores_before = protection.signal_safe_calls;
	protection.block_next_protect.store(true);
	std::thread rearm([&] { (void)tracker.Rearm(mapping.address, 64); });
	while (!protection.protect_entered.load()) { std::this_thread::yield(); }
	// The native protect is paused while the rearm holds the registration
	// mutex. The wide notification neither takes that mutex nor waits for the
	// rearm: it restores the page and publishes the write at once. Range
	// queries do take the registration mutex, so while the rearm is paused the
	// evidence is read from the lock-free page generations. A notification that
	// waited would only miss the deadline; the controller still releases the
	// rearm, so the test cannot hang.
	std::atomic<bool> notified {false};
	bool              notify_result = false;
	std::thread       notifier([&]
	                     {
		                     notify_result = tracker.NotifyWrite(1, huge);
		                     notified.store(true, std::memory_order_release);
	                     });
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!notified.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	const bool notified_while_paused = notified.load(std::memory_order_acquire);
	EXPECT_TRUE(notified_while_paused);
	if (notified_while_paused)
	{
		uint64_t page_after = 0;
		EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
		EXPECT_TRUE(tracker.PageGenerations(mapping.address, 64, &page_after, 1u));
		EXPECT_GT(page_after, page_before);
	}
	protection.release_protect.store(true);
	rearm.join();
	notifier.join();
	EXPECT_TRUE(notify_result);
	EXPECT_GT(tracker.SnapshotGeneration(mapping.address, 64), before);
	EXPECT_EQ(protection.current_modes[0], Mode::ReadWrite);
	EXPECT_EQ(protection.signal_safe_calls, restores_before + 2u);
	EXPECT_TRUE(tracker.UnregisterRange(mapping.address, 64));
}

// Host notifications are ordinary writers: they never undo the guest's own
// protection. A page whose guest protection denies writes stays protected, and
// a notification that arrives during a mapping removal waits for it to publish
// and then finds the token revoked, so nothing is restored over the new
// mapping.
TEST(EmulatorGraphicsDirtyTracking, HostNotificationsNeverRestoreWriteTheGuestDeniedOrRevoked)
{
	Mapping mapping(2);
	ASSERT_NE(mapping.address, 0u);
	const uint64_t      page_size = GetPageSize();
	FakeProtection      protection(mapping.address, page_size, std::vector<Mode>(2, Mode::ReadWrite));
	GpuDirtyPageTracker tracker(protection.Ops());
	const uint64_t      denied  = mapping.address;
	const uint64_t      revoked = mapping.address + page_size;
	ASSERT_TRUE(tracker.RegisterRange(denied, page_size));
	ASSERT_TRUE(tracker.RegisterRange(revoked, page_size));
	ASSERT_TRUE(tracker.Rearm(denied, page_size));
	ASSERT_TRUE(tracker.Rearm(revoked, page_size));
	ASSERT_NE(protection.authority, nullptr);
	const auto                 read_write = static_cast<uint32_t>(Mode::ReadWrite);
	Kyty::Core::SysWriteLeases leases;
	leases.Add(denied, revoked + page_size, read_write, protection.authority);
	FakeLeaseNative::Use(&protection, 0u);
	ASSERT_TRUE(leases.Protect({{denied, denied + page_size, read_write, true}}, static_cast<uint32_t>(Mode::Read), Mode::Read,
	                           FakeLeaseNative::kNative));
	const uint32_t restores = protection.signal_safe_calls;

	EXPECT_TRUE(tracker.NotifyWrite(denied, 1u));
	EXPECT_EQ(protection.current_modes[0], Mode::Read);
	EXPECT_EQ(protection.signal_safe_calls, restores);

	std::vector<Kyty::Core::SysWriteLeaseRun> fenced;
	leases.BeginUnmap(revoked, revoked + page_size, &fenced);
	std::atomic<bool> started {false};
	std::atomic<bool> done {false};
	std::thread       notifier([&]
	                     {
		                     started.store(true, std::memory_order_release);
		                     (void)tracker.NotifyWrite(revoked, 1u);
		                     done.store(true, std::memory_order_release);
	                     });
	while (!started.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	for (uint32_t spin = 0; spin < 10000u && !done.load(std::memory_order_acquire); spin++)
	{
		std::this_thread::yield();
	}
	// The fence is held until the removal publishes, so the writer cannot be done.
	EXPECT_FALSE(done.load(std::memory_order_acquire));
	leases.EndUnmap(revoked, revoked + page_size, fenced, true);
	notifier.join();
	EXPECT_TRUE(done.load(std::memory_order_acquire));
	EXPECT_EQ(protection.current_modes[1], Mode::Read);
	EXPECT_EQ(protection.signal_safe_calls, restores);

	EXPECT_TRUE(leases.Release(protection.authority, FakeLeaseNative::kNative));
	EXPECT_TRUE(tracker.UnregisterRange(denied, page_size));
	EXPECT_TRUE(tracker.UnregisterRange(revoked, page_size));
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
	EXPECT_FALSE(GpuMemoryCheckAccessViolation(0, Access::Write));
	EXPECT_FALSE(GpuMemoryNotifyHostWrite(0, 0));
}

UT_END();
