#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <array>
#include <cinttypes>
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

// Records the lease notifications and answers every protection change with
// one decision for the whole run.
struct LeaseRecorder
{
	uint32_t         begins       = 0;
	uint32_t         decisions    = 0;
	uint32_t         ends         = 0;
	bool             remove_write = true;
	WriteLeaseChange last {};

	static void Begin(void* context, uint64_t /*address*/, uint64_t /*size*/) noexcept
	{
		static_cast<LeaseRecorder*>(context)->begins++;
	}

	static void Decide(void* context, const WriteLeaseChange& change, WriteLeaseDecision decision, void* decision_context) noexcept
	{
		auto* self = static_cast<LeaseRecorder*>(context);
		self->decisions++;
		(void)decision(decision_context, change.address, change.size, self->remove_write);
	}

	static void End(void* context, const WriteLeaseChange& change) noexcept
	{
		auto* self = static_cast<LeaseRecorder*>(context);
		self->ends++;
		self->last = change;
	}
};

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
// Whether the host mapping containing `address` is writable, from the kernel's
// own view of the process.
bool NativeWritable(uint64_t address)
{
	std::ifstream maps("/proc/self/maps");
	std::string   line;
	while (std::getline(maps, line))
	{
		uint64_t begin    = 0;
		uint64_t end      = 0;
		char     perms[5] = {};
		if (std::sscanf(line.c_str(), "%" SCNx64 "-%" SCNx64 " %4s", &begin, &end, perms) == 3 && address >= begin && address < end)
		{
			return perms[1] == 'w';
		}
	}
	return false;
}
#endif

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

// A lease removes write natively but never the guest's own protection: guest
// queries keep the guest view, every guest change over the leased page reaches
// the authority inside the transaction, and unmapping reports a revocation.
TEST(CoreVirtualMemory, WriteLeaseReportsGuestChangesAndKeepsGuestProtectionAuthoritative)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size * 2u, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	LeaseRecorder             recorder;
	const WriteLeaseAuthority authority {&recorder, &LeaseRecorder::Begin, &LeaseRecorder::Decide, &LeaseRecorder::End};
	ProtectionCapture         capture;
	ASSERT_TRUE(RemoveWriteAndCapture(address, page_size, &CaptureProtection, &capture, &authority).Succeeded());
	EXPECT_TRUE(IsRangeWritable(address, page_size * 2u));
	EXPECT_EQ(recorder.ends, 0u);

	ASSERT_TRUE(ProtectGuest(address, page_size * 2u, Mode::Read));
	EXPECT_EQ(recorder.begins, 1u);
	EXPECT_EQ(recorder.decisions, 1u);
	EXPECT_EQ(recorder.ends, 1u);
	EXPECT_EQ(recorder.last.kind, WriteLeaseChangeKind::Protect);
	EXPECT_EQ(recorder.last.mode, Mode::Read);
	EXPECT_EQ(recorder.last.address, address);
	EXPECT_EQ(recorder.last.size, page_size);
	EXPECT_TRUE(recorder.last.committed);
	EXPECT_FALSE(IsRangeWritable(address, page_size));

	ASSERT_TRUE(ProtectGuest(address, page_size * 2u, Mode::ReadWrite));
	EXPECT_EQ(recorder.ends, 2u);
	EXPECT_EQ(recorder.last.mode, Mode::ReadWrite);
	EXPECT_TRUE(IsRangeWritable(address, page_size * 2u));

	// The authoritative guest protection, not a holder token, is restored.
	EXPECT_TRUE(RestoreProtection(address, page_size, 0u));
	auto* bytes      = reinterpret_cast<volatile uint8_t*>(address);
	bytes[0]         = 0x5a;
	bytes[page_size] = 0xc3;

	ASSERT_TRUE(Free(address));
	EXPECT_EQ(recorder.begins, 3u);
	EXPECT_EQ(recorder.ends, 3u);
	EXPECT_EQ(recorder.last.kind, WriteLeaseChangeKind::Unmap);
	EXPECT_TRUE(recorder.last.committed);
}

TEST(CoreVirtualMemory, ReleasedWriteLeasesRestoreGuestProtectionAndStopReporting)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	LeaseRecorder             recorder;
	const WriteLeaseAuthority authority {&recorder, &LeaseRecorder::Begin, &LeaseRecorder::Decide, &LeaseRecorder::End};
	ProtectionCapture         capture;
	ASSERT_TRUE(RemoveWriteAndCapture(address, page_size, &CaptureProtection, &capture, &authority).Succeeded());

	ASSERT_TRUE(ReleaseWriteLeases(&authority));
	EXPECT_EQ(recorder.ends, 1u);
	EXPECT_FALSE(recorder.last.committed);
	reinterpret_cast<volatile uint8_t*>(address)[0] = 0x5a;

	ASSERT_TRUE(ProtectGuest(address, page_size, Mode::Read));
	ASSERT_TRUE(Free(address));
	EXPECT_EQ(recorder.ends, 1u);
}

// A lease without an authority has nobody to decide for it: the next guest
// protection change applies the guest protection and ends the lease.
TEST(CoreVirtualMemory, GuestProtectionReplacesAnUnownedWriteLease)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	ProtectionCapture capture;
	ASSERT_TRUE(RemoveWriteAndCapture(address, page_size, &CaptureProtection, &capture).Succeeded());
	ASSERT_TRUE(ProtectGuest(address, page_size, Mode::ReadWrite));
	reinterpret_cast<volatile uint8_t*>(address)[0] = 0x5a;
	EXPECT_TRUE(Free(address));
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
// A host protection change over a range that is partly guest memory and partly
// untracked host memory still reports the leased guest page to its authority
// and records the guest protection, while the host page only receives the
// native protection.
TEST(CoreVirtualMemory, MixedGuestAndHostProtectionKeepsLeaseAuthority)
{
	const uint64_t page_size = GetPageSize();
	void*          host      = mmap(nullptr, page_size * 2u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(host, MAP_FAILED);
	const auto guest = reinterpret_cast<uint64_t>(host);
	ASSERT_EQ(munmap(host, page_size), 0);
	if (!AllocFixed(guest, page_size, Mode::ReadWrite))
	{
		(void)munmap(reinterpret_cast<void*>(guest + page_size), page_size);
		GTEST_SKIP() << "the host placed the probe outside the guest window";
	}
	ASSERT_TRUE(IsRangeGuestOwned(guest, page_size));
	ASSERT_FALSE(IsRangeGuestOwned(guest + page_size, page_size));

	LeaseRecorder             recorder;
	const WriteLeaseAuthority authority {&recorder, &LeaseRecorder::Begin, &LeaseRecorder::Decide, &LeaseRecorder::End};
	ProtectionCapture         capture;
	ASSERT_TRUE(RemoveWriteAndCapture(guest, page_size, &CaptureProtection, &capture, &authority).Succeeded());
	EXPECT_FALSE(NativeWritable(guest));

	ASSERT_TRUE(Protect(guest, page_size * 2u, Mode::ReadWrite));
	EXPECT_EQ(recorder.ends, 1u);
	EXPECT_TRUE(recorder.last.committed);
	EXPECT_EQ(recorder.last.address, guest);
	EXPECT_EQ(recorder.last.size, page_size);
	EXPECT_FALSE(NativeWritable(guest));
	EXPECT_TRUE(NativeWritable(guest + page_size));
	EXPECT_TRUE(IsRangeWritable(guest, page_size));

	ASSERT_TRUE(Protect(guest, page_size * 2u, Mode::Read));
	EXPECT_EQ(recorder.ends, 2u);
	EXPECT_EQ(recorder.last.mode, Mode::Read);
	EXPECT_FALSE(IsRangeWritable(guest, page_size));
	EXPECT_FALSE(NativeWritable(guest + page_size));

	EXPECT_TRUE(ReleaseWriteLeases(&authority));
	EXPECT_TRUE(Free(guest));
	EXPECT_EQ(munmap(reinterpret_cast<void*>(guest + page_size), page_size), 0);
}
#endif

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

// A snapshot accepts copies while its mapping is unchanged. A size other than its
// own, a token that was never captured, and an altered token copy nothing.
TEST(CoreVirtualMemory, GuestMappingSnapshotAcceptsUnchangedMappingAndRefusesMalformedTokens)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size * 2u, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	const uint64_t          cross_page = address + page_size - 8u;
	std::array<uint8_t, 16> input {};
	input.fill(0x5a);
	GuestMappingSnapshot snapshot;
	ASSERT_TRUE(CaptureGuestMappingSnapshot(cross_page, input.size(), &snapshot));
	EXPECT_EQ(snapshot.segment_count, 1u);
	ASSERT_TRUE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	EXPECT_TRUE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));

	std::array<uint8_t, 16> other {};
	other.fill(0xc3);
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, other.data(), other.size() - 4u));
	EXPECT_FALSE(CopyToGuestIfMappingMatches(GuestMappingSnapshot {}, other.data(), other.size()));
	auto forged = snapshot;
	forged.segments[0].identity++;
	EXPECT_FALSE(CopyToGuestIfMappingMatches(forged, other.data(), other.size()));
	auto gapped = snapshot;
	gapped.segments[0].address += 4u;
	EXPECT_FALSE(CopyToGuestIfMappingMatches(gapped, other.data(), other.size()));
	auto overcounted          = snapshot;
	overcounted.segment_count = GuestMappingSnapshot::kMaxSegments + 1u;
	EXPECT_FALSE(CopyToGuestIfMappingMatches(overcounted, other.data(), other.size()));

	std::array<uint8_t, 16> output {};
	ASSERT_TRUE(CopyFromGuest(output.data(), cross_page, output.size()));
	EXPECT_EQ(output, input);
	EXPECT_TRUE(Free(address));
}

// Capture needs ownership only, so a read-only range is captured. Guest
// protection changes, write-lease removal and rearm, and a partial unmap
// elsewhere keep the mapping instance; each copy needs write access at its time.
TEST(CoreVirtualMemory, GuestMappingSnapshotSurvivesProtectionLeaseAndSplit)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size * 3u, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	const uint64_t          target = address + page_size;
	std::array<uint8_t, 16> input {};
	input.fill(0x5a);
	ASSERT_TRUE(ProtectGuest(target, page_size, Mode::Read));
	GuestMappingSnapshot snapshot;
	ASSERT_TRUE(CaptureGuestMappingSnapshot(target, input.size(), &snapshot));
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	EXPECT_EQ(reinterpret_cast<const volatile uint8_t*>(target)[0], 0u);

	ASSERT_TRUE(ProtectGuest(target, page_size, Mode::ReadWrite));
	EXPECT_TRUE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	ASSERT_TRUE(ProtectGuest(target, page_size, Mode::NoAccess));
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	ASSERT_TRUE(ProtectGuest(target, page_size, Mode::ReadWrite));
	EXPECT_TRUE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));

	ProtectionCapture capture;
	ASSERT_TRUE(RemoveWriteAndCapture(target, page_size, &CaptureProtection, &capture).Succeeded());
	ASSERT_TRUE(RestoreProtection(target, page_size, capture.runs[0].restore_token));
	EXPECT_TRUE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));

#if !defined(_WIN32)
	ASSERT_TRUE(FreeRange(address + page_size * 2u, page_size));
	EXPECT_TRUE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
#endif
	EXPECT_TRUE(Free(address));
}

// A new writable mapping at a released address passes ownership and write
// checks, but it is not the captured instance: the copy leaves it untouched.
TEST(CoreVirtualMemory, GuestMappingSnapshotRefusesAMappingThatReplacedTheAddress)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	std::array<uint8_t, 16> input {};
	input.fill(0x5a);
	GuestMappingSnapshot snapshot;
	ASSERT_TRUE(CaptureGuestMappingSnapshot(address, input.size(), &snapshot));
	ASSERT_TRUE(Free(address));
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));

	if (!AllocFixed(address, page_size, Mode::ReadWrite))
	{
		GTEST_SKIP() << "the host reused the released address";
	}
	auto* bytes = reinterpret_cast<uint8_t*>(address);
	std::fill(bytes, bytes + page_size, uint8_t {0xa5});
	ASSERT_TRUE(IsRangeWritable(address, input.size()));
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	EXPECT_TRUE(std::all_of(bytes, bytes + page_size, [](uint8_t value) { return value == 0xa5u; }));
	EXPECT_TRUE(Free(address));
}

// Replacing one of the two mappings a snapshot spans refuses the whole copy,
// including the bytes that still belong to the unchanged mapping.
TEST(CoreVirtualMemory, GuestMappingSnapshotRefusesAPartlyReplacedRangeWholly)
{
	const uint64_t page_size   = GetPageSize();
	const uint64_t reservation = Reserve(0, page_size * 3u);
	ASSERT_NE(reservation, 0u);
	const uint64_t first  = reservation + page_size;
	const uint64_t second = reservation + page_size * 2u;
	ASSERT_TRUE(AllocFixedReplacingOwnedReservation(first, page_size, Mode::ReadWrite));
	ASSERT_TRUE(AllocFixedReplacingOwnedReservation(second, page_size, Mode::ReadWrite));
	const uint64_t          target = second - 8u;
	std::array<uint8_t, 16> input {};
	input.fill(0x5a);
	GuestMappingSnapshot snapshot;
	ASSERT_TRUE(CaptureGuestMappingSnapshot(target, input.size(), &snapshot));
	EXPECT_EQ(snapshot.segment_count, 2u);

	ASSERT_TRUE(Free(second));
#if defined(_WIN32)
	ASSERT_TRUE(AllocFixedReplacingOwnedReservation(second, page_size, Mode::ReadWrite));
#else
	ASSERT_TRUE(AllocFixed(second, page_size, Mode::ReadWrite));
#endif
	auto* bytes = reinterpret_cast<uint8_t*>(target);
	std::fill(bytes, bytes + input.size(), uint8_t {0xa5});
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	EXPECT_TRUE(std::all_of(bytes, bytes + input.size(), [](uint8_t value) { return value == 0xa5u; }));

#if defined(_WIN32)
	ASSERT_TRUE(Free(first));
	ASSERT_TRUE(Free(second));
	ASSERT_TRUE(Free(reservation));
#else
	ASSERT_TRUE(Free(reservation));
	ASSERT_TRUE(Free(first));
	ASSERT_TRUE(Free(second));
#endif
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
// Decommit and a later commit of the same interval are each a new mapping instance.
TEST(CoreVirtualMemory, GuestMappingSnapshotRefusesDecommittedAndRecommittedRange)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	std::array<uint8_t, 16> input {};
	input.fill(0x5a);
	GuestMappingSnapshot snapshot;
	ASSERT_TRUE(CaptureGuestMappingSnapshot(address, input.size(), &snapshot));
	ASSERT_TRUE(DecommitGuestRange(address, page_size));
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));

	ASSERT_TRUE(AllocFixedReplacingOwnedReservation(address, page_size, Mode::ReadWrite));
	auto* bytes = reinterpret_cast<uint8_t*>(address);
	std::fill(bytes, bytes + input.size(), uint8_t {0xa5});
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	EXPECT_TRUE(std::all_of(bytes, bytes + input.size(), [](uint8_t value) { return value == 0xa5u; }));
	EXPECT_TRUE(Free(address));
}
#endif

// Capture needs every byte guest owned, within the byte budget, whatever its
// protection. A NoAccess reservation is captured, but committing it is a new
// mapping instance that the snapshot refuses.
TEST(CoreVirtualMemory, GuestMappingSnapshotCaptureRequiresOwnershipWithinBudget)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, GuestMappingSnapshot::kMaxBytes + page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	GuestMappingSnapshot snapshot;
	EXPECT_TRUE(CaptureGuestMappingSnapshot(address, GuestMappingSnapshot::kMaxBytes, &snapshot));
	EXPECT_FALSE(CaptureGuestMappingSnapshot(address, GuestMappingSnapshot::kMaxBytes + 1u, &snapshot));
	EXPECT_EQ(snapshot.segment_count, 0u);
	EXPECT_FALSE(CaptureGuestMappingSnapshot(address, 0, &snapshot));
	EXPECT_FALSE(CaptureGuestMappingSnapshot(address, 16, nullptr));
	ASSERT_TRUE(ProtectGuest(address, page_size, Mode::Read));
	EXPECT_TRUE(CaptureGuestMappingSnapshot(address + page_size - 8u, 16, &snapshot));
	EXPECT_TRUE(Free(address));

	const uint64_t reservation = Reserve(0, page_size);
	ASSERT_NE(reservation, 0u);
	std::array<uint8_t, 16> input {};
	input.fill(0x5a);
	ASSERT_TRUE(CaptureGuestMappingSnapshot(reservation, input.size(), &snapshot));
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	ASSERT_TRUE(AllocFixedReplacingOwnedReservation(reservation, page_size, Mode::ReadWrite));
	ASSERT_TRUE(IsRangeWritable(reservation, input.size()));
	EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	EXPECT_EQ(reinterpret_cast<const volatile uint8_t*>(reservation)[0], 0u);
#if defined(_WIN32)
	EXPECT_TRUE(Free(reservation));
#endif
	EXPECT_TRUE(Free(reservation));

	std::array<uint8_t, 16> host {};
	EXPECT_FALSE(CaptureGuestMappingSnapshot(reinterpret_cast<uint64_t>(host.data()), host.size(), &snapshot));
}

// An armed write lease removes write natively while the guest protection stays
// authoritative: capture succeeds without fencing the lease authority. The
// conditional copy does not lift the lease, so it runs only after the
// authority restores native write, and then succeeds for the same instance.
TEST(CoreVirtualMemory, GuestMappingSnapshotCapturesDuringAnArmedWriteLease)
{
	const uint64_t page_size = GetPageSize();
	const uint64_t address   = Alloc(0, page_size, Mode::ReadWrite);
	ASSERT_NE(address, 0u);
	LeaseRecorder             recorder;
	const WriteLeaseAuthority authority {&recorder, &LeaseRecorder::Begin, &LeaseRecorder::Decide, &LeaseRecorder::End};
	ProtectionCapture         capture;
	ASSERT_TRUE(RemoveWriteAndCapture(address, page_size, &CaptureProtection, &capture, &authority).Succeeded());
	ASSERT_EQ(capture.size, 1u);
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	EXPECT_FALSE(NativeWritable(address));
#endif
	EXPECT_TRUE(IsRangeWritable(address, 16));

	std::array<uint8_t, 16> input {};
	input.fill(0x5a);
	GuestMappingSnapshot snapshot;
	ASSERT_TRUE(CaptureGuestMappingSnapshot(address, input.size(), &snapshot));
	EXPECT_EQ(recorder.begins, 0u);

	ASSERT_TRUE(RestoreProtection(capture.runs[0].address, capture.runs[0].size, capture.runs[0].restore_token));
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	EXPECT_TRUE(NativeWritable(address));
#endif
	ASSERT_TRUE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	std::array<uint8_t, 16> output {};
	ASSERT_TRUE(CopyFromGuest(output.data(), address, output.size()));
	EXPECT_EQ(output, input);
	EXPECT_EQ(recorder.begins, 0u);

	EXPECT_TRUE(ReleaseWriteLeases(&authority));
	EXPECT_TRUE(Free(address));
}

// Another view of the same backing, its release, and fixed mappings refused over
// the captured range leave the snapshot valid. A new view of the same backing
// bytes at the same address is a new mapping instance.
TEST(CoreVirtualMemory, GuestMappingSnapshotFollowsMappingInstanceNotBackingBytes)
{
	const uint64_t page_size = GetPageSize();
	SharedBacking* backing   = CreateSharedBacking(page_size);
	SharedBacking* other     = CreateSharedBacking(page_size);
	ASSERT_NE(backing, nullptr);
	ASSERT_NE(other, nullptr);
	const uint64_t view = MapSharedAligned(backing, 0, 0, page_size, Mode::ReadWrite, page_size);
	ASSERT_NE(view, 0u);
	std::array<uint8_t, 16> input {};
	input.fill(0x5a);
	GuestMappingSnapshot snapshot;
	ASSERT_TRUE(CaptureGuestMappingSnapshot(view, input.size(), &snapshot));

	const uint64_t alias = MapSharedAligned(backing, 0, 0, page_size, Mode::ReadWrite, page_size);
	ASSERT_NE(alias, 0u);
	ASSERT_NE(alias, view);
	EXPECT_TRUE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));
	EXPECT_EQ(reinterpret_cast<const volatile uint8_t*>(alias)[0], 0x5au);
	ASSERT_TRUE(Free(alias));
	EXPECT_FALSE(MapSharedFixed(other, view, 0, page_size, Mode::ReadWrite));
	EXPECT_FALSE(AllocFixedReplacingOwnedReservation(view, page_size, Mode::ReadWrite));
	EXPECT_TRUE(CopyToGuestIfMappingMatches(snapshot, input.data(), input.size()));

	ASSERT_TRUE(Free(view));
	if (MapSharedFixed(backing, view, 0, page_size, Mode::ReadWrite))
	{
		std::array<uint8_t, 16> replacement {};
		replacement.fill(0xc3);
		EXPECT_FALSE(CopyToGuestIfMappingMatches(snapshot, replacement.data(), replacement.size()));
		EXPECT_EQ(reinterpret_cast<const volatile uint8_t*>(view)[0], 0x5au);
		EXPECT_TRUE(Free(view));
	}
	DestroySharedBacking(other);
	DestroySharedBacking(backing);
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

// A view gains backing pages only when it first touches them and loses them
// only through a discard, so the population summary changes exactly then.
TEST(CoreVirtualMemory, SharedBackingPopulationChangesOnlyWhenPagesAreAddedOrDiscarded)
{
#if !defined(__linux__)
	GTEST_SKIP() << "population is reported by the Linux memfd backing";
#else
	constexpr uint64_t kSize     = 0x10000;
	const uint64_t     page_size = GetPageSize();
	SharedBacking*     backing   = CreateSharedBacking(kSize);
	ASSERT_NE(backing, nullptr);
	const uint64_t view = MapSharedAligned(backing, 0, 0, kSize, Mode::ReadWrite, page_size);
	ASSERT_NE(view, 0u);

	SharedBackingPopulation empty;
	ASSERT_TRUE(QuerySharedBackingPopulation(backing, &empty));
	reinterpret_cast<volatile uint8_t*>(view)[0] = 1;
	SharedBackingPopulation touched;
	ASSERT_TRUE(QuerySharedBackingPopulation(backing, &touched));
	EXPECT_NE(touched, empty);
	EXPECT_EQ(touched.populated_bytes, empty.populated_bytes + page_size);

	reinterpret_cast<volatile uint8_t*>(view)[8] = 2;
	SharedBackingPopulation retouched;
	ASSERT_TRUE(QuerySharedBackingPopulation(backing, &retouched));
	EXPECT_EQ(retouched, touched);

	ASSERT_TRUE(Free(view));
	ASSERT_TRUE(DiscardSharedBackingRange(backing, 0, kSize));
	SharedBackingPopulation discarded;
	ASSERT_TRUE(QuerySharedBackingPopulation(backing, &discarded));
	EXPECT_NE(discarded, retouched);
	EXPECT_EQ(discarded.discards, retouched.discards + 1u);
	DestroySharedBacking(backing);
#endif
}

// One sweep over spans in any order marks exactly the spans whose backing
// holds no page: before, between and after populated pages, overlapping or not.
TEST(CoreVirtualMemory, UnpopulatedSharedBackingSpansAreFoundInOneSweep)
{
#if !defined(__linux__)
	GTEST_SKIP() << "population is reported by the Linux memfd backing";
#else
	const uint64_t page    = GetPageSize();
	const uint64_t size    = page * 8u;
	SharedBacking* backing = CreateSharedBacking(size);
	ASSERT_NE(backing, nullptr);
	const uint64_t view = MapSharedAligned(backing, 0, 0, size, Mode::ReadWrite, page);
	ASSERT_NE(view, 0u);
	reinterpret_cast<volatile uint8_t*>(view)[page * 2u] = 1;
	reinterpret_cast<volatile uint8_t*>(view)[page * 5u] = 1;

	SharedBackingSpan spans[] = {
	    {page * 6u, page * 2u}, {page * 3u, page * 2u}, {0, page * 2u}, {page, page * 2u}, {page * 5u, page}, {page * 4u, page},
	};
	FindUnpopulatedSharedBackingSpans(backing, spans, std::size(spans));
	EXPECT_TRUE(spans[0].unpopulated);
	EXPECT_TRUE(spans[1].unpopulated);
	EXPECT_TRUE(spans[2].unpopulated);
	EXPECT_FALSE(spans[3].unpopulated);
	EXPECT_FALSE(spans[4].unpopulated);
	EXPECT_TRUE(spans[5].unpopulated);
	ASSERT_TRUE(Free(view));
	DestroySharedBacking(backing);
#endif
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

TEST(CoreVirtualMemory, PosixIllegalInstructionReportCapturesSignalContext)
{
#if defined(_WIN32)
	GTEST_SKIP() << "POSIX signal-context coverage";
#else
	char report_path[128] = {};
	std::snprintf(report_path, sizeof(report_path), "/tmp/kyty-ill-context-%ld.json", static_cast<long>(::getpid()));
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
		(void)::raise(SIGILL);
		::_Exit(127);
	}

	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(WEXITSTATUS(status), 132);

	std::ifstream report(report_path);
	ASSERT_TRUE(report.good());
	const std::string json((std::istreambuf_iterator<char>(report)), std::istreambuf_iterator<char>());
	EXPECT_NE(json.find("\"exception_code\":\"0x0000000000000004\""), std::string::npos);
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
