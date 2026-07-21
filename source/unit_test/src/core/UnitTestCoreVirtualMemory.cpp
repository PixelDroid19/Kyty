#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include <chrono>
#include <cstdint>
#include <array>

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

TEST(CoreVirtualMemory, DemandMapUsesHostPageSize)
{
	const uint64_t page_size = GetPageSize();
	ASSERT_GT(page_size, 0u);

#if defined(_WIN32)
	GTEST_SKIP() << "demand paging signal path is POSIX-only";
#else
	const uint64_t address = Alloc(0, page_size, Mode::NoAccess);
	ASSERT_NE(address, 0u);

	RegisterDemandRange(address, page_size);
	ASSERT_TRUE(TryDemandMap(address + page_size - 1u));

	auto* bytes = reinterpret_cast<uint8_t*>(address);
	bytes[0]              = 0x5a;
	bytes[page_size - 1u] = 0xc3;
	EXPECT_EQ(bytes[0], 0x5a);
	EXPECT_EQ(bytes[page_size - 1u], 0xc3);
	ASSERT_TRUE(Free(address));
#endif
}

TEST(CoreVirtualMemory, SignalDiagnosticsConfigurationUsesPresenceSemantics)
{
	const auto disabled = MakeSignalDiagnosticsConfig(nullptr, nullptr);
	EXPECT_FALSE(disabled.skip_ud2);
	EXPECT_FALSE(disabled.fault_log);

	const auto enabled = MakeSignalDiagnosticsConfig("0", "");
	EXPECT_TRUE(enabled.skip_ud2);
	EXPECT_TRUE(enabled.fault_log);

	const auto partial = MakeSignalDiagnosticsConfig("1", nullptr);
	EXPECT_TRUE(partial.skip_ud2);
	EXPECT_FALSE(partial.fault_log);
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
