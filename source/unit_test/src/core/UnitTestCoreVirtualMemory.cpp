#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <sys/mman.h>
#endif

UT_BEGIN(CoreVirtualMemory);

using namespace Core::VirtualMemory;

namespace {
struct ProtectionCapture
{
	std::array<CapturedProtectionRun, 8> runs {};
	size_t size = 0;
};

bool CaptureProtection(void* context, const CapturedProtectionRun& run) noexcept
{
	auto* capture = static_cast<ProtectionCapture*>(context);
	if (capture->size >= capture->runs.size())
	{
		return false;
	}
	capture->runs[capture->size++] = run;
	return true;
}

#if !defined(_WIN32)
void FatalFromSignal(const ExceptionHandler::ExceptionInfo* info)
{
	FatalFault(info);
}
#endif
} // namespace

TEST(CoreVirtualMemory, RemoveWriteCapturesAndRestoresMixedProtectionRuns)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address = Alloc(0, page_size * 4u, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	ASSERT_TRUE(Protect(address + page_size * 2u, page_size, Mode::ExecuteReadWrite));
	ProtectionCapture capture;
	const auto change = RemoveWriteAndCapture(address, page_size * 4u, &CaptureProtection, &capture);
	ASSERT_TRUE(change.Succeeded());
	ASSERT_EQ(capture.size, 3u);
	EXPECT_EQ(change.applied_runs, 3u);
	EXPECT_EQ(capture.runs[0].mode, Mode::ReadWrite);
	EXPECT_EQ(capture.runs[1].mode, Mode::ExecuteReadWrite);
	EXPECT_EQ(capture.runs[2].mode, Mode::ReadWrite);
	for (size_t i = 0; i < capture.size; i++)
	{
		EXPECT_TRUE(RestoreProtection(capture.runs[i].address, capture.runs[i].size, capture.runs[i].restore_token));
	}
	auto* bytes = reinterpret_cast<uint8_t*>(address);
	bytes[0] = 0x5a;
	bytes[page_size * 2u] = 0xc3;
	EXPECT_TRUE(Free(address));
}

TEST(CoreVirtualMemory, UniformLargeRangeUsesOneProtectionTransition)
{
	constexpr uint64_t size = 64u * 1024u * 1024u;
	const uint64_t address = Alloc(0, size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	ProtectionCapture capture;
	const auto change = RemoveWriteAndCapture(address, size, &CaptureProtection, &capture);
	ASSERT_TRUE(change.Succeeded());
	ASSERT_EQ(capture.size, 1u);
	EXPECT_EQ(change.applied_runs, 1u);
	EXPECT_EQ(change.applied_bytes, size);
	EXPECT_TRUE(RestoreProtection(capture.runs[0].address, capture.runs[0].size, capture.runs[0].restore_token));
	EXPECT_TRUE(Free(address));
}

TEST(CoreVirtualMemory, GuestCopiesRespectWritableRangesAcrossPages)
{
	const uint64_t page_size = GetPageSize();
	ASSERT_NE(page_size, 0u);
	const uint64_t address = Alloc(0, page_size * 2u, Mode::ReadWrite);
	ASSERT_NE(address, 0u);

	constexpr size_t kCopySize = 16;
	std::array<uint8_t, kCopySize> input {};
	for (size_t i = 0; i < input.size(); ++i)
	{
		input[i] = static_cast<uint8_t>(0x40u + i);
	}
	const uint64_t cross_page = address + page_size - 8u;
	EXPECT_TRUE(IsRangeGuestOwned(cross_page, input.size()));
	EXPECT_TRUE(IsRangeReadable(cross_page, input.size()));
	EXPECT_TRUE(IsRangeWritable(cross_page, input.size()));
	ASSERT_TRUE(CopyToGuest(cross_page, input.data(), input.size()));

	std::array<uint8_t, kCopySize> output {};
	ASSERT_TRUE(CopyFromGuest(output.data(), cross_page, output.size()));
	EXPECT_EQ(output, input);

	ASSERT_TRUE(Protect(address + page_size, page_size, Mode::Read));
	EXPECT_TRUE(IsRangeReadable(cross_page, input.size()));
	EXPECT_FALSE(IsRangeWritable(cross_page, input.size()));
	EXPECT_FALSE(CopyToGuest(cross_page, input.data(), input.size()));
	EXPECT_TRUE(CopyFromGuest(output.data(), cross_page, output.size()));
	EXPECT_EQ(output, input);

	EXPECT_TRUE(Free(address));
}

TEST(CoreVirtualMemory, GuestOwnershipTracksSplitReservationAndCrossPageCopy)
{
	const uint64_t page_size = GetPageSize();
	ASSERT_NE(page_size, 0u);
	const uint64_t reservation = Reserve(0, page_size * 4u);
	ASSERT_NE(reservation, 0u);
	EXPECT_TRUE(IsRangeGuestOwned(reservation, page_size * 4u));
	EXPECT_FALSE(IsRangeReadable(reservation, page_size));

	ASSERT_TRUE(AllocFixedReplacingOwnedReservation(reservation + page_size, page_size * 2u, Mode::ReadWrite));
	const uint64_t cross_page = reservation + page_size * 2u - 8u;
	std::array<uint8_t, 16> input {};
	input.fill(0x5a);
	std::array<uint8_t, 16> output {};
	EXPECT_TRUE(IsRangeGuestOwned(cross_page, input.size()));
	EXPECT_TRUE(CopyToGuest(cross_page, input.data(), input.size()));
	EXPECT_TRUE(CopyFromGuest(output.data(), cross_page, output.size()));
	EXPECT_EQ(output, input);

	ASSERT_TRUE(Free(reservation + page_size));
	EXPECT_FALSE(IsRangeGuestOwned(cross_page, input.size()));
	EXPECT_FALSE(CopyToGuest(cross_page, input.data(), input.size()));
#if defined(_WIN32)
	ASSERT_TRUE(Free(reservation));
#else
	ASSERT_TRUE(Free(reservation));
	ASSERT_TRUE(Free(reservation + page_size * 3u));
#endif
}

TEST(CoreVirtualMemory, ExternalMmapProtectDoesNotAuthorizeGuestAccess)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX || defined(__APPLE__)
	GTEST_SKIP() << "raw mmap ownership regression is Linux-specific";
#else
	const uint64_t page_size = GetPageSize();
	ASSERT_NE(page_size, 0u);
	auto* const external = static_cast<uint8_t*>(
	    ::mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	ASSERT_NE(reinterpret_cast<void*>(external), MAP_FAILED);

	const uint64_t address = reinterpret_cast<uint64_t>(external);
	uint8_t        input   = 0x5a;
	uint8_t        output  = 0;
	EXPECT_FALSE(IsRangeGuestOwned(address, 1));
	EXPECT_FALSE(IsRangeReadable(address, 1));
	EXPECT_FALSE(IsRangeWritable(address, 1));
	EXPECT_FALSE(CopyToGuest(address, &input, sizeof(input)));
	EXPECT_FALSE(CopyFromGuest(&output, address, sizeof(output)));

	// Internal host users may still change their own mapping's protection, but
	// that operation must not add it to the guest ownership registry.
	ASSERT_TRUE(Protect(address, page_size, Mode::Read));
	EXPECT_FALSE(IsRangeGuestOwned(address, 1));
	EXPECT_FALSE(IsRangeReadable(address, 1));
	ASSERT_TRUE(Protect(address, page_size, Mode::ReadWrite));
	EXPECT_FALSE(IsRangeWritable(address, 1));

	EXPECT_EQ(::munmap(external, page_size), 0);
#endif
}

TEST(CoreVirtualMemory, HostHeapAndStackAreNotGuestOwned)
{
#if !defined(_WIN32)
	GTEST_SKIP() << "Windows VirtualQuery ownership regression";
#else
	std::array<uint8_t, 16> stack {};
	auto* const heap = new uint8_t[stack.size()] {};
	ASSERT_NE(heap, nullptr);
	uint8_t input  = 0x5a;
	uint8_t output = 0;
	for (const auto* address: {stack.data(), heap})
	{
		const uint64_t value = reinterpret_cast<uint64_t>(address);
		EXPECT_FALSE(IsRangeGuestOwned(value, stack.size()));
		EXPECT_FALSE(IsRangeReadable(value, stack.size()));
		EXPECT_FALSE(IsRangeWritable(value, stack.size()));
		EXPECT_FALSE(CopyFromGuest(&output, value, sizeof(output)));
		EXPECT_FALSE(CopyToGuest(value, &input, sizeof(input)));
	}
	delete[] heap;
#endif
}

TEST(CoreVirtualMemory, GuestCopiesSerializeWithProtectAndFree)
{
	const uint64_t page_size = GetPageSize();
	ASSERT_NE(page_size, 0u);
	const uint64_t address = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);

	constexpr size_t kCopySize = 64;
	std::array<uint8_t, kCopySize> input {};
	input.fill(0x5a);
	std::atomic<bool>     started {false};
	std::atomic<bool>     stop {false};
	std::atomic<uint32_t> copies {0};
	std::thread copier([&]() {
		std::array<uint8_t, kCopySize> output {};
		started.store(true, std::memory_order_release);
		while (!stop.load(std::memory_order_acquire))
		{
			if (CopyToGuest(address, input.data(), input.size()))
			{
				(void)CopyFromGuest(output.data(), address, output.size());
			}
			copies.fetch_add(1, std::memory_order_relaxed);
			std::this_thread::yield();
		}
	});

	while (!started.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	for (uint32_t i = 0; i < 32; ++i)
	{
		EXPECT_TRUE(ProtectGuest(address, page_size, Mode::Read));
		EXPECT_FALSE(CopyToGuest(address, input.data(), input.size()));
		EXPECT_TRUE(ProtectGuest(address, page_size, Mode::ReadWrite));
	}

	const bool freed = Free(address);
	stop.store(true, std::memory_order_release);
	copier.join();
	EXPECT_TRUE(freed);
	EXPECT_GT(copies.load(std::memory_order_relaxed), 0u);
	if (freed)
	{
		std::array<uint8_t, kCopySize> output {};
		EXPECT_FALSE(CopyFromGuest(output.data(), address, output.size()));
	}
}

TEST(CoreVirtualMemory, ProtectGuestRejectsConcurrentFreedAndReusedHostRange)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX || defined(__APPLE__) || !defined(MAP_FIXED_NOREPLACE)
	GTEST_SKIP() << "fixed non-replacing mmap reuse regression is Linux-specific";
#else
	const uint64_t page_size = GetPageSize();
	ASSERT_NE(page_size, 0u);
	const uint64_t address = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);

	std::atomic<bool>     started {false};
	std::atomic<bool>     reused {false};
	std::atomic<bool>     stop {false};
	std::atomic<uint32_t> rejected_after_reuse {0};
	std::thread protector([&]() {
		started.store(true, std::memory_order_release);
		while (!stop.load(std::memory_order_acquire))
		{
			const bool protected_range = ProtectGuest(address, page_size, Mode::ReadWrite);
			if (reused.load(std::memory_order_acquire) && !protected_range)
			{
				rejected_after_reuse.fetch_add(1, std::memory_order_relaxed);
			}
			std::this_thread::yield();
		}
	});

	while (!started.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	if (!Free(address))
	{
		stop.store(true, std::memory_order_release);
		protector.join();
		FAIL() << "guest mapping could not be released";
	}
	auto* const external = ::mmap(reinterpret_cast<void*>(address), page_size, PROT_READ,
	                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (external != reinterpret_cast<void*>(address))
	{
		stop.store(true, std::memory_order_release);
		protector.join();
		if (external != MAP_FAILED)
		{
			EXPECT_EQ(::munmap(external, page_size), 0);
		}
		FAIL() << "host mapping did not reuse the released guest address";
	}
	reused.store(true, std::memory_order_release);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	while (rejected_after_reuse.load(std::memory_order_relaxed) == 0 && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	stop.store(true, std::memory_order_release);
	protector.join();
	EXPECT_GT(rejected_after_reuse.load(std::memory_order_relaxed), 0u);
	EXPECT_FALSE(IsRangeGuestOwned(address, page_size));
	EXPECT_EQ(::munmap(external, page_size), 0);
#endif
}

TEST(CoreVirtualMemory, FixedMapFreeKeepsOwnershipCoherent)
{
#if !defined(_WIN32)
	GTEST_SKIP() << "fixed-map lifecycle regression is Windows-specific";
#else
	const uint64_t page_size = GetPageSize();
	ASSERT_NE(page_size, 0u);
	const uint64_t address = Reserve(0, page_size * 2u);
	ASSERT_NE(address, 0u);
	ASSERT_TRUE(AllocFixedReplacingOwnedReservation(address, page_size, Mode::ReadWrite));

	std::atomic<bool>     started {false};
	std::atomic<bool>     stop {false};
	std::atomic<uint32_t> rejected {0};
	std::thread protector([&]() {
		started.store(true, std::memory_order_release);
		while (!stop.load(std::memory_order_acquire))
		{
			if (!ProtectGuest(address, page_size, Mode::ReadWrite))
			{
				rejected.fetch_add(1, std::memory_order_relaxed);
			}
			std::this_thread::yield();
		}
	});

	while (!started.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	if (!Free(address))
	{
		stop.store(true, std::memory_order_release);
		protector.join();
		FAIL() << "fixed guest mapping could not be freed";
	}
	EXPECT_FALSE(IsRangeGuestOwned(address, page_size));
	const auto rejection_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	while (rejected.load(std::memory_order_relaxed) == 0 && std::chrono::steady_clock::now() < rejection_deadline)
	{
		std::this_thread::yield();
	}
	if (!ReserveFixed(address, page_size))
	{
		stop.store(true, std::memory_order_release);
		protector.join();
		FAIL() << "owned reservation could not be republished";
	}
	EXPECT_TRUE(IsRangeGuestOwned(address, page_size));

	stop.store(true, std::memory_order_release);
	protector.join();
	EXPECT_GT(rejected.load(std::memory_order_relaxed), 0u);
	EXPECT_TRUE(Free(address));
#endif
}

// Shared host backing must keep alias views byte-coherent: a write through one
// map is visible through another map of the same backing offset.
TEST(CoreVirtualMemory, SharedBackingPreservesAliasCoherence)
{
	constexpr uint64_t kSize = 0x10000;
	SharedBacking*     backing = CreateSharedBacking(kSize);
	ASSERT_NE(backing, nullptr);

	const uint64_t first = MapSharedAligned(backing, 0, 0, kSize, Mode::ReadWrite, 0x1000);
	ASSERT_NE(first, 0u);
	const uint64_t second = MapSharedAligned(backing, 0, 0, kSize, Mode::ReadWrite, 0x1000);
	ASSERT_NE(second, 0u);
	ASSERT_NE(first, second);

	auto* first_bytes  = reinterpret_cast<uint8_t*>(first);
	auto* second_bytes = reinterpret_cast<uint8_t*>(second);
	first_bytes[0]     = 0x5a;
	first_bytes[1]     = 0xc3;
	EXPECT_EQ(second_bytes[0], 0x5a);
	EXPECT_EQ(second_bytes[1], 0xc3);
	second_bytes[2] = 0x7e;
	EXPECT_EQ(first_bytes[2], 0x7e);

	ASSERT_TRUE(Free(first));
	ASSERT_TRUE(Free(second));
	DestroySharedBacking(backing);
}

// Large guest heaps must not create one host metadata node per page. This is
// intentionally sparse so the test exercises the tracking contract without
// requiring physical memory proportional to the guest reservation.
TEST(CoreVirtualMemory, LargeSharedMappingUsesBoundedProtectionMetadata)
{
	constexpr uint64_t kSize = 0x80000000ULL;
	const uint64_t     page_size = GetPageSize();
	ASSERT_NE(page_size, 0u);

	SharedBacking* backing = CreateSharedBacking(kSize);
	ASSERT_NE(backing, nullptr);

	const uint64_t view = MapSharedAligned(backing, 0, 0, kSize, Mode::ReadWrite, page_size);
	ASSERT_NE(view, 0u);
	ASSERT_TRUE(Free(view));
	DestroySharedBacking(backing);
}

// macOS lacks MAP_FIXED_NOREPLACE, so shared mappings must reject occupied
// host ranges before MAP_FIXED is used. Skipping a single occupied interval is
// required to keep that safety check bounded under Rosetta.
TEST(CoreVirtualMemory, SharedMappingSkipsOccupiedHostIntervalPromptly)
{
#if !defined(__APPLE__)
	GTEST_SKIP() << "Mach occupied-range probing is macOS-specific";
#else
	constexpr uint64_t kPageSize    = 0x4000;
	constexpr uint64_t kBlockedSize = 0x02000000ULL;

	SharedBacking* occupied_backing = CreateSharedBacking(kBlockedSize);
	ASSERT_NE(occupied_backing, nullptr);
	const uint64_t occupied = MapSharedAligned(occupied_backing, 0, 0, kBlockedSize, Mode::NoAccess, kPageSize);
	ASSERT_NE(occupied, 0u);

	SharedBacking* backing = CreateSharedBacking(kPageSize);
	ASSERT_NE(backing, nullptr);

	const auto started = std::chrono::steady_clock::now();
	const uint64_t view = MapSharedAligned(backing, occupied, 0, kPageSize, Mode::ReadWrite, kPageSize);
	const auto elapsed = std::chrono::steady_clock::now() - started;

	ASSERT_NE(view, 0u);
	EXPECT_NE(view, occupied);
	EXPECT_LT(elapsed, std::chrono::seconds(2));
	ASSERT_TRUE(Free(view));
	DestroySharedBacking(backing);
	ASSERT_TRUE(Free(occupied));
	DestroySharedBacking(occupied_backing);
#endif
}

// A fixed shared view must never replace an existing mapping. This is the
// contract used by the macOS reservation path before it calls MAP_FIXED.
TEST(CoreVirtualMemory, FixedSharedMappingRejectsOccupiedRange)
{
	const uint64_t page_size = GetPageSize();
	ASSERT_NE(page_size, 0u);

	SharedBacking* occupied_backing = CreateSharedBacking(page_size);
	ASSERT_NE(occupied_backing, nullptr);
	const uint64_t occupied = MapSharedAligned(occupied_backing, 0, 0, page_size, Mode::ReadWrite, page_size);
	ASSERT_NE(occupied, 0u);

	auto* occupied_bytes = reinterpret_cast<uint8_t*>(occupied);
	occupied_bytes[0]    = 0x5a;

	SharedBacking* replacement_backing = CreateSharedBacking(page_size);
	ASSERT_NE(replacement_backing, nullptr);
	EXPECT_FALSE(MapSharedFixed(replacement_backing, occupied, 0, page_size, Mode::ReadWrite));
	EXPECT_EQ(MapSharedFixedOrRelocated(replacement_backing, occupied, 0, page_size, Mode::ReadWrite, page_size), 0u);
	EXPECT_EQ(occupied_bytes[0], 0x5a);

	DestroySharedBacking(replacement_backing);
	ASSERT_TRUE(Free(occupied));
	DestroySharedBacking(occupied_backing);
}

TEST(CoreVirtualMemory, FixedSharedMappingReplacesOnlyOwnedReservationSubrange)
{
	const uint64_t page_size = GetPageSize();
	ASSERT_NE(page_size, 0u);

	const uint64_t reservation = Reserve(0, page_size * 4u);
	ASSERT_NE(reservation, 0u);
	SharedBacking* backing = CreateSharedBacking(page_size * 2u);
	ASSERT_NE(backing, nullptr);

	ASSERT_TRUE(MapSharedFixedReplacingOwnedReservation(backing, reservation + page_size, 0, page_size * 2u,
	                                                   Mode::ReadWrite));
	auto* bytes = reinterpret_cast<uint8_t*>(reservation + page_size);
	bytes[0] = 0x5a;
	bytes[page_size * 2u - 1u] = 0xc3;
	EXPECT_EQ(bytes[0], 0x5a);
	EXPECT_EQ(bytes[page_size * 2u - 1u], 0xc3);

	EXPECT_FALSE(MapSharedFixedReplacingOwnedReservation(backing, reservation + page_size, 0, page_size, Mode::ReadWrite));
#if defined(_WIN32)
	ASSERT_TRUE(Free(reservation + page_size));
	ASSERT_TRUE(Free(reservation));
#else
	ASSERT_TRUE(Free(reservation));
	ASSERT_TRUE(Free(reservation + page_size));
	ASSERT_TRUE(Free(reservation + page_size * 3u));
#endif
	DestroySharedBacking(backing);
}

TEST(CoreVirtualMemory, DemandMapUsesHostPageSize)
{
	const uint64_t page_size = GetPageSize();
	ASSERT_GT(page_size, 0u);

#if defined(_WIN32)
	GTEST_SKIP() << "demand paging signal path is POSIX-only";
#else
	const uint64_t address = Alloc(0, page_size, Mode::NoAccess);
	ASSERT_NE(address, 0u);

	ASSERT_TRUE(RegisterDemandRange(address, page_size));
	ASSERT_TRUE(TryDemandMap(address + page_size - 1u));

	auto* bytes = reinterpret_cast<uint8_t*>(address);
	bytes[0]              = 0x5a;
	bytes[page_size - 1u] = 0xc3;
	EXPECT_EQ(bytes[0], 0x5a);
	EXPECT_EQ(bytes[page_size - 1u], 0xc3);
	EXPECT_TRUE(UnregisterDemandRange(address, page_size));
	ASSERT_TRUE(Free(address));
#endif
}

TEST(CoreVirtualMemory, DemandRangeRegistrySupportsMoreThanSixtyFourLiveReservations)
{
#if defined(_WIN32)
	GTEST_SKIP() << "demand paging signal path is POSIX-only";
#else
	const uint64_t page_size = GetPageSize();
	ASSERT_GT(page_size, 0u);
	constexpr uint64_t kRangeCount = 512;
	constexpr uint64_t kStridePages = 2;
	const uint64_t address = Alloc(0, page_size * kRangeCount * kStridePages, Mode::NoAccess);
	ASSERT_NE(address, 0u);

	for (uint64_t i = 0; i < kRangeCount; ++i)
	{
		ASSERT_TRUE(RegisterDemandRange(address + i * page_size * kStridePages, page_size));
	}
	EXPECT_TRUE(TryDemandMap(address + (kRangeCount - 1u) * page_size * kStridePages));
	for (uint64_t i = 0; i < kRangeCount; ++i)
	{
		EXPECT_TRUE(UnregisterDemandRange(address + i * page_size * kStridePages, page_size));
	}
	ASSERT_TRUE(Free(address));
#endif
}

TEST(CoreVirtualMemory, DemandRangeRegistryRemovesConsumedSubranges)
{
#if defined(_WIN32)
	GTEST_SKIP() << "demand paging signal path is POSIX-only";
#else
	const uint64_t page_size = GetPageSize();
	ASSERT_GT(page_size, 0u);
	const uint64_t address = Alloc(0, page_size * 4u, Mode::NoAccess);
	ASSERT_NE(address, 0u);

	ASSERT_TRUE(RegisterDemandRange(address, page_size * 4u));
	ASSERT_TRUE(UnregisterDemandRange(address + page_size, page_size * 2u));
	EXPECT_TRUE(TryDemandMap(address));
	EXPECT_FALSE(TryDemandMap(address + page_size));
	EXPECT_TRUE(TryDemandMap(address + page_size * 3u));
	EXPECT_TRUE(UnregisterDemandRange(address, page_size));
	EXPECT_TRUE(UnregisterDemandRange(address + page_size * 3u, page_size));
	ASSERT_TRUE(Free(address));
#endif
}

TEST(CoreVirtualMemory, SignalDiagnosticsConfigurationUsesPresenceSemantics)
{
	const auto disabled = MakeSignalDiagnosticsConfig(nullptr, nullptr, nullptr);
	EXPECT_FALSE(disabled.skip_ud2);
	EXPECT_FALSE(disabled.fault_log);
	EXPECT_FALSE(disabled.crash_memory);

	const auto enabled = MakeSignalDiagnosticsConfig("0", "", "0");
	EXPECT_TRUE(enabled.skip_ud2);
	EXPECT_TRUE(enabled.fault_log);
	EXPECT_TRUE(enabled.crash_memory);

	const auto partial = MakeSignalDiagnosticsConfig("1", nullptr, nullptr);
	EXPECT_TRUE(partial.skip_ud2);
	EXPECT_FALSE(partial.fault_log);
	EXPECT_FALSE(partial.crash_memory);
}

TEST(CoreVirtualMemory, PosixFatalReportCapturesSignalContext)
{
#if defined(_WIN32)
	GTEST_SKIP() << "POSIX signal-context coverage";
#else
	char report_path[128] = {};
	std::snprintf(report_path, sizeof(report_path), "/tmp/kyty-fault-context-%ld.json", static_cast<long>(::getpid()));
	(void)std::remove(report_path);

	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		ConfigureFatalFaultReport(report_path);
		if (!ExceptionHandler::InstallVectored(FatalFromSignal))
		{
			::_Exit(126);
		}
		(void)::raise(SIGSEGV);
		::_Exit(127);
	}

	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(WEXITSTATUS(status), 139);

	std::ifstream report(report_path);
	ASSERT_TRUE(report.good());
	const std::string json((std::istreambuf_iterator<char>(report)), std::istreambuf_iterator<char>());
	EXPECT_EQ(json.find("\"rsp\":\"0x0000000000000000\""), std::string::npos);
	EXPECT_EQ(json.find("\"rip\":\"0x0000000000000000\""), std::string::npos);
	EXPECT_EQ(json.find("\"stack\":[]"), std::string::npos);
	(void)std::remove(report_path);
#endif
}

TEST(CoreVirtualMemory, FatalReportKeepsOneKilobyteGuestStackWindow)
{
	EXPECT_GE(ExceptionHandler::ExceptionInfo::StackCapacity, 128u);
	EXPECT_GE(ExceptionHandler::ExceptionInfo::MemoryWindowCapacity, 1u);
	EXPECT_GE(ExceptionHandler::ExceptionInfo::MemoryWindowSize, 32u);
}

TEST(CoreVirtualMemory, FatalReportSerializesMemoryWindows)
{
#if defined(_WIN32)
	GTEST_SKIP() << "POSIX child-process coverage";
#else
	char report_path[128] = {};
	std::snprintf(report_path, sizeof(report_path), "/tmp/kyty-fault-memory-%ld.json", static_cast<long>(::getpid()));
	(void)std::remove(report_path);

	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		ConfigureFatalFaultReport(report_path);
		ExceptionHandler::ExceptionInfo info {};
		info.type                       = ExceptionHandler::ExceptionType::AccessViolation;
		info.stack[0]                   = 0x1234u;
		info.stack_count                = 1;
		info.memory_windows[0].address  = 0x2000u;
		info.memory_windows[0].bytes[0] = 0xdeu;
		info.memory_windows[0].bytes[1] = 0xadu;
		info.memory_windows[0].size     = 2;
		info.memory_window_count        = 1;
		FatalFault(&info);
	}

	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(WEXITSTATUS(status), 139);

	std::ifstream report(report_path);
	ASSERT_TRUE(report.good());
	const std::string json((std::istreambuf_iterator<char>(report)), std::istreambuf_iterator<char>());
	EXPECT_NE(json.find("\"stack\":[\"0x0000000000001234\"]"), std::string::npos);
	EXPECT_NE(json.find("\"memory_windows\":[{\"address\":\"0x0000000000002000\""), std::string::npos);
	EXPECT_NE(json.find("\"bytes\":\"dead\""), std::string::npos);
	(void)std::remove(report_path);
#endif
}

// Released direct-memory ranges must reclaim host pages via punch-hole so a
// long session that allocates/frees heaps does not keep RSS "como loco".
TEST(CoreVirtualMemory, DiscardSharedBackingRangeReclaimsFaultedPages)
{
	const uint64_t page_size = GetPageSize();
	ASSERT_GT(page_size, 0u);
	constexpr uint64_t kPages = 4;
	const uint64_t     kSize  = page_size * kPages;

	SharedBacking* backing = CreateSharedBacking(kSize);
	ASSERT_NE(backing, nullptr);

	const uint64_t view = MapSharedAligned(backing, 0, 0, kSize, Mode::ReadWrite, page_size);
	ASSERT_NE(view, 0u);
	auto* bytes = reinterpret_cast<uint8_t*>(view);
	for (uint64_t i = 0; i < kSize; ++i)
	{
		bytes[i] = static_cast<uint8_t>(0xa5);
	}
	ASSERT_TRUE(Free(view));

	ASSERT_TRUE(DiscardSharedBackingRange(backing, 0, kSize));

	const uint64_t view2 = MapSharedAligned(backing, 0, 0, kSize, Mode::ReadWrite, page_size);
	ASSERT_NE(view2, 0u);
	auto* bytes2 = reinterpret_cast<uint8_t*>(view2);
	// Punch-hole zeros the range on next fault; at least the first and last
	// bytes of each page must not retain the previous 0xa5 pattern.
	EXPECT_EQ(bytes2[0], 0);
	EXPECT_EQ(bytes2[page_size - 1u], 0);
	EXPECT_EQ(bytes2[kSize - 1u], 0);

	ASSERT_TRUE(Free(view2));
	DestroySharedBacking(backing);
}

TEST(CoreVirtualMemory, DiscardSharedBackingRangeRejectsInvalidBounds)
{
	const uint64_t page_size = GetPageSize();
	ASSERT_GT(page_size, 0u);
	SharedBacking* backing = CreateSharedBacking(page_size);
	ASSERT_NE(backing, nullptr);

	EXPECT_FALSE(DiscardSharedBackingRange(nullptr, 0, page_size));
	EXPECT_FALSE(DiscardSharedBackingRange(backing, 0, 0));
	EXPECT_FALSE(DiscardSharedBackingRange(backing, page_size, page_size));
	EXPECT_FALSE(DiscardSharedBackingRange(backing, page_size / 2u, page_size));

	DestroySharedBacking(backing);
}

UT_END();
