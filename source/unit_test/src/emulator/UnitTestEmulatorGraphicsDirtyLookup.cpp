#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GpuDirtyPageTracker.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

UT_BEGIN(EmulatorGraphicsDirtyLookup);

using Kyty::Core::VirtualMemory::CreateSharedBacking;
using Kyty::Core::VirtualMemory::DestroySharedBacking;
using Kyty::Core::VirtualMemory::Free;
using Kyty::Core::VirtualMemory::GetPageSize;
using Kyty::Core::VirtualMemory::MapSharedAligned;
using Kyty::Core::VirtualMemory::Mode;
using Kyty::Libs::Graphics::GpuDirtyPageTableIndex;
using Kyty::Libs::Graphics::GpuDirtyPageTracker;

namespace {

constexpr size_t    kPageTableMask       = (1u << 18u) - 1u;
constexpr size_t    kBlockTableMask      = (1u << 17u) - 1u;
constexpr size_t    kCollisionCandidates = 1u << 24u;
constexpr uintptr_t kSyntheticBase       = static_cast<uintptr_t>(0x10000000000ull);
constexpr size_t    kPagesPerBlock       = 64u;
constexpr size_t    kCollisionCount      = 8u;

uintptr_t BlockNumber(uintptr_t page, size_t page_size)
{
	return page / page_size / kPagesPerBlock;
}

size_t BlockTableIndex(uintptr_t block)
{
	return GpuDirtyPageTableIndex((block + 1u) << 12u, kBlockTableMask);
}

struct NativeMapping
{
	size_t                                    size    = 0;
	uintptr_t                                 address = 0;
	Kyty::Core::VirtualMemory::SharedBacking* backing = nullptr;

	explicit NativeMapping(size_t pages = 1u): size(GetPageSize() * pages), backing(size == 0u ? nullptr : CreateSharedBacking(size))
	{
		if (backing != nullptr)
		{
			address = MapSharedAligned(backing, 0, 0, size, Mode::ReadWrite, GetPageSize());
		}
	}

	~NativeMapping()
	{
		if (address != 0u)
		{
			Free(address);
		}
		if (backing != nullptr)
		{
			DestroySharedBacking(backing);
		}
	}
};

uintptr_t SyntheticPage(size_t page_size, size_t page_index)
{
	return kSyntheticBase + static_cast<uintptr_t>(page_size * page_index);
}

bool FindCollidingPages(uintptr_t target, size_t page_size, std::array<uintptr_t, kCollisionCount>& pages)
{
	if (page_size == 0u)
	{
		return false;
	}
	const size_t    target_index       = GpuDirtyPageTableIndex(target, kPageTableMask);
	const size_t    target_block_index = BlockTableIndex(BlockNumber(target, page_size));
	const uintptr_t search_start       = target + static_cast<uintptr_t>(page_size) * (1u << 20u);
	size_t          found              = 0;
	for (size_t candidate_index = 0; candidate_index < kCollisionCandidates && found < pages.size(); ++candidate_index)
	{
		const uintptr_t candidate = search_start + static_cast<uintptr_t>(page_size) * candidate_index;
		if (GpuDirtyPageTableIndex(candidate, kPageTableMask) != target_index)
		{
			continue;
		}
		const uintptr_t candidate_block = BlockNumber(candidate, page_size);
		const size_t    block_index     = BlockTableIndex(candidate_block);
		const size_t    block_distance  = (block_index - target_block_index) & kBlockTableMask;
		bool            unique          = block_distance > kCollisionCount;
		for (size_t i = 0; i < found; ++i)
		{
			unique = unique && BlockNumber(pages[i], page_size) != candidate_block &&
			         BlockTableIndex(BlockNumber(pages[i], page_size)) != block_index;
		}
		if (unique)
		{
			pages[found++] = candidate;
		}
	}
	return found == pages.size();
}

bool FindCollidingBlockPages(uintptr_t target, size_t page_size, const std::array<uintptr_t, kCollisionCount>& page_collisions,
                             std::array<uintptr_t, kCollisionCount>& pages)
{
	if (page_size == 0u || page_size > std::numeric_limits<uintptr_t>::max() / kPagesPerBlock)
	{
		return false;
	}
	const uintptr_t block_size   = static_cast<uintptr_t>(page_size) * kPagesPerBlock;
	const uintptr_t target_block = BlockNumber(target, page_size);
	const size_t    target_index = BlockTableIndex(target_block);
	const uintptr_t first_block  = target_block + (1u << 20u);
	size_t          found        = 0;
	for (size_t candidate_index = 0; candidate_index < kCollisionCandidates && found < pages.size(); ++candidate_index)
	{
		if (first_block > std::numeric_limits<uintptr_t>::max() - candidate_index)
		{
			return false;
		}
		const uintptr_t block = first_block + candidate_index;
		if (block > std::numeric_limits<uintptr_t>::max() / block_size || block == target_block)
		{
			return false;
		}
		bool used = false;
		for (const uintptr_t page: page_collisions)
		{
			used = used || page / block_size == block;
		}
		for (size_t i = 0; i < found; ++i)
		{
			used = used || pages[i] / block_size == block;
		}
		const uintptr_t candidate = block * block_size;
		const uintptr_t key       = block + 1u;
		if (!used && key <= (std::numeric_limits<uintptr_t>::max() >> 12u) &&
		    GpuDirtyPageTableIndex(key << 12u, kBlockTableMask) == target_index)
		{
			pages[found++] = candidate;
		}
	}
	return found == pages.size();
}

} // namespace

TEST(EmulatorGraphicsDirtyLookup, FindsAndMarksTheNinthCollidingPage)
{
	const size_t page_size = GetPageSize();
	ASSERT_TRUE(page_size != 0u);
	const uintptr_t                        target = SyntheticPage(page_size, 5u);
	std::array<uintptr_t, kCollisionCount> collisions {};
	ASSERT_TRUE(FindCollidingPages(target, page_size, collisions));

	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(target, page_size));
	for (const uintptr_t page: collisions)
	{
		ASSERT_TRUE(tracker.RegisterRange(page, page_size));
	}

	std::array<uint64_t, kCollisionCount + 1u>        snapshots {};
	const std::array<uintptr_t, kCollisionCount + 1u> pages = {target,        collisions[0], collisions[1], collisions[2], collisions[3],
	                                                           collisions[4], collisions[5], collisions[6], collisions[7]};
	for (size_t i = 0; i < pages.size(); ++i)
	{
		snapshots[i] = tracker.SnapshotGeneration(pages[i], page_size);
	}

	ASSERT_TRUE(tracker.NotifyWrite(pages.back(), page_size));
	EXPECT_TRUE(tracker.ChangedSince(pages.back(), page_size, snapshots.back()));
	for (size_t i = 0; i + 1u < pages.size(); ++i)
	{
		EXPECT_FALSE(tracker.ChangedSince(pages[i], page_size, snapshots[i]));
	}
}

TEST(EmulatorGraphicsDirtyLookup, ExactMissDoesNotAliasAnotherPageInItsPopulatedBlock)
{
	const size_t page_size = GetPageSize();
	ASSERT_TRUE(page_size != 0u);
	const uintptr_t miss       = SyntheticPage(page_size, 5u);
	const uintptr_t miss_index = GpuDirtyPageTableIndex(miss, kPageTableMask);
	uintptr_t       neighbor   = 0u;
	for (size_t offset = 1u; offset < kPagesPerBlock; ++offset)
	{
		const uintptr_t candidate = miss + static_cast<uintptr_t>(page_size) * offset;
		if ((candidate / page_size) / kPagesPerBlock == (miss / page_size) / kPagesPerBlock &&
		    GpuDirtyPageTableIndex(candidate, kPageTableMask) != miss_index)
		{
			neighbor = candidate;
			break;
		}
	}
	ASSERT_TRUE(neighbor != 0u);
	std::array<uintptr_t, kCollisionCount> collisions {};
	ASSERT_TRUE(FindCollidingPages(miss, page_size, collisions));

	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(neighbor, page_size));
	for (const uintptr_t page: collisions)
	{
		ASSERT_TRUE(tracker.RegisterRange(page, page_size));
	}
	const uint64_t                        neighbor_snapshot = tracker.SnapshotGeneration(neighbor, page_size);
	std::array<uint64_t, kCollisionCount> snapshots {};
	for (size_t i = 0; i < collisions.size(); ++i)
	{
		snapshots[i] = tracker.SnapshotGeneration(collisions[i], page_size);
	}

	EXPECT_FALSE(tracker.NotifyWrite(miss, page_size));
	EXPECT_FALSE(tracker.ChangedSince(neighbor, page_size, neighbor_snapshot));
	for (size_t i = 0; i < collisions.size(); ++i)
	{
		EXPECT_FALSE(tracker.ChangedSince(collisions[i], page_size, snapshots[i]));
	}
}

TEST(EmulatorGraphicsDirtyLookup, RemovalLeavesAProbeTombstoneAndReRegistrationWorks)
{
	const size_t page_size = GetPageSize();
	ASSERT_TRUE(page_size != 0u);
	const uintptr_t                        first = SyntheticPage(page_size, 5u);
	std::array<uintptr_t, kCollisionCount> collisions {};
	ASSERT_TRUE(FindCollidingPages(first, page_size, collisions));
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(first, page_size));
	for (const uintptr_t page: collisions)
	{
		ASSERT_TRUE(tracker.RegisterRange(page, page_size));
	}

	const uintptr_t tail          = collisions.back();
	const uint64_t  tail_snapshot = tracker.SnapshotGeneration(tail, page_size);
	ASSERT_TRUE(tracker.UnregisterRange(first, page_size));
	EXPECT_FALSE(tracker.NotifyWrite(first, page_size));
	ASSERT_TRUE(tracker.NotifyWrite(tail, page_size));
	EXPECT_TRUE(tracker.ChangedSince(tail, page_size, tail_snapshot));

	ASSERT_TRUE(tracker.RegisterRange(first, page_size));
	const uint64_t new_snapshot = tracker.SnapshotGeneration(first, page_size);
	EXPECT_FALSE(tracker.ChangedSince(first, page_size, new_snapshot));
	ASSERT_TRUE(tracker.NotifyWrite(first, page_size));
	EXPECT_TRUE(tracker.ChangedSince(first, page_size, new_snapshot));
}

TEST(EmulatorGraphicsDirtyLookup, RetiredCollidingPageCanBeReRegisteredAndWritten)
{
	NativeMapping mapping;
	ASSERT_TRUE(mapping.address != 0u);
	const size_t page_size = GetPageSize();
	ASSERT_TRUE(page_size != 0u);
	std::array<uintptr_t, kCollisionCount> collisions {};
	ASSERT_TRUE(FindCollidingPages(mapping.address, page_size, collisions));

	GpuDirtyPageTracker tracker;
	for (const uintptr_t page: collisions)
	{
		ASSERT_TRUE(tracker.RegisterRange(page, page_size));
	}
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, page_size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, page_size));
	ASSERT_TRUE(tracker.UnregisterRange(mapping.address, page_size));
	ASSERT_TRUE(tracker.HandleWriteFault(mapping.address));

	ASSERT_TRUE(tracker.RegisterRange(mapping.address, page_size));
	const uint64_t snapshot = tracker.SnapshotGeneration(mapping.address, page_size);
	ASSERT_TRUE(tracker.NotifyWrite(mapping.address, page_size));
	EXPECT_TRUE(tracker.ChangedSince(mapping.address, page_size, snapshot));
}

TEST(EmulatorGraphicsDirtyLookup, DeepBlockCollisionsDoNotHideTrackedPage)
{
	const size_t page_size = GetPageSize();
	ASSERT_TRUE(page_size != 0u);
	const uintptr_t                        target = SyntheticPage(page_size, 5u);
	std::array<uintptr_t, kCollisionCount> page_collisions {};
	std::array<uintptr_t, kCollisionCount> block_collisions {};
	ASSERT_TRUE(FindCollidingPages(target, page_size, page_collisions));
	ASSERT_TRUE(FindCollidingBlockPages(target, page_size, page_collisions, block_collisions));

	GpuDirtyPageTracker tracker;
	for (const uintptr_t page: page_collisions)
	{
		ASSERT_TRUE(tracker.RegisterRange(page, page_size));
	}
	for (const uintptr_t page: block_collisions)
	{
		ASSERT_TRUE(tracker.RegisterRange(page, page_size));
	}
	ASSERT_TRUE(tracker.RegisterRange(target, page_size));
	const uint64_t snapshot = tracker.SnapshotGeneration(target, page_size);
	ASSERT_TRUE(tracker.NotifyWrite(target, page_size));
	EXPECT_TRUE(tracker.ChangedSince(target, page_size, snapshot));

	const uint64_t after_target_write = tracker.SnapshotGeneration(target, page_size);
	EXPECT_FALSE(tracker.NotifyWrite(target + page_size, page_size));
	EXPECT_FALSE(tracker.ChangedSince(target, page_size, after_target_write));
}

TEST(EmulatorGraphicsDirtyLookup, FullBlockTableMissDoesNotAliasTrackedNeighbor)
{
	const size_t page_size = GetPageSize();
	ASSERT_TRUE(page_size != 0u);
	const uintptr_t target       = SyntheticPage(page_size, 5u);
	const uintptr_t neighbor     = target + page_size;
	const uintptr_t target_block = BlockNumber(target, page_size);
	ASSERT_EQ(BlockNumber(neighbor, page_size), target_block);

	std::array<uintptr_t, kCollisionCount> page_collisions {};
	std::array<uintptr_t, kCollisionCount> block_collisions {};
	ASSERT_TRUE(FindCollidingPages(target, page_size, page_collisions));
	ASSERT_TRUE(FindCollidingBlockPages(target, page_size, page_collisions, block_collisions));

	constexpr size_t     table_size = kBlockTableMask + 1u;
	std::vector<uint8_t> occupied(table_size, 0u);
	std::set<uintptr_t>  registered_blocks;
	GpuDirtyPageTracker  tracker;

	for (const uintptr_t page: page_collisions)
	{
		const uintptr_t block = BlockNumber(page, page_size);
		ASSERT_TRUE(registered_blocks.insert(block).second);
		const size_t slot = BlockTableIndex(block);
		ASSERT_EQ(occupied[slot], 0u);
		occupied[slot] = 1u;
		ASSERT_TRUE(tracker.RegisterRange(page, page_size));
	}

	const size_t target_home = BlockTableIndex(target_block);
	for (const uintptr_t page: block_collisions)
	{
		const uintptr_t block = BlockNumber(page, page_size);
		ASSERT_TRUE(registered_blocks.insert(block).second);
		ASSERT_EQ(BlockTableIndex(block), target_home);
		size_t slot = target_home;
		for (size_t probe = 0; probe < table_size && occupied[slot] != 0u; ++probe)
		{
			slot = (slot + 1u) & kBlockTableMask;
		}
		ASSERT_EQ(occupied[slot], 0u);
		occupied[slot] = 1u;
		ASSERT_TRUE(tracker.RegisterRange(page, page_size));
	}

	ASSERT_TRUE(registered_blocks.insert(target_block).second);
	size_t target_slot = target_home;
	for (size_t probe = 0; probe < kCollisionCount && occupied[target_slot] != 0u; ++probe)
	{
		target_slot = (target_slot + 1u) & kBlockTableMask;
	}
	ASSERT_EQ(occupied[target_slot], 0u);
	occupied[target_slot] = 1u;

	ASSERT_TRUE(page_size <= std::numeric_limits<uintptr_t>::max() / kPagesPerBlock);
	const uintptr_t block_size    = static_cast<uintptr_t>(page_size) * kPagesPerBlock;
	const size_t    filler_count  = table_size - registered_blocks.size();
	size_t          fillers_added = 0u;
	for (uintptr_t block = 1u; block <= kCollisionCandidates && fillers_added < filler_count; ++block)
	{
		if (block > std::numeric_limits<uintptr_t>::max() / block_size || block + 1u > (std::numeric_limits<uintptr_t>::max() >> 12u) ||
		    registered_blocks.find(block) != registered_blocks.end())
		{
			continue;
		}
		const size_t slot = BlockTableIndex(block);
		if (occupied[slot] != 0u)
		{
			continue;
		}
		ASSERT_TRUE(registered_blocks.insert(block).second);
		occupied[slot] = 1u;
		ASSERT_TRUE(tracker.RegisterRange(block * block_size, page_size));
		++fillers_added;
	}
	ASSERT_EQ(fillers_added, filler_count);
	ASSERT_EQ(registered_blocks.size(), table_size);
	ASSERT_TRUE(tracker.RegisterRange(neighbor, page_size));

	const uint64_t neighbor_snapshot = tracker.SnapshotGeneration(neighbor, page_size);
	EXPECT_FALSE(tracker.NotifyWrite(target, page_size));
	EXPECT_FALSE(tracker.ChangedSince(neighbor, page_size, neighbor_snapshot));
	ASSERT_TRUE(tracker.NotifyWrite(neighbor, page_size));
	EXPECT_TRUE(tracker.ChangedSince(neighbor, page_size, neighbor_snapshot));
}

TEST(EmulatorGraphicsDirtyLookup, IdentityBudgetRefusalPreservesRetiredFaultEvidence)
{
	NativeMapping mapping(2u);
	ASSERT_TRUE(mapping.address != 0u);
	const size_t page_size = GetPageSize();
	ASSERT_TRUE(page_size != 0u);
	GpuDirtyPageTracker tracker;
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, page_size));
	ASSERT_TRUE(tracker.Rearm(mapping.address, page_size));
	ASSERT_TRUE(tracker.UnregisterRange(mapping.address, page_size));

	// Retired protection still consumes an identity. Bound admission without
	// discarding the token needed by a delayed fault.
	constexpr size_t identity_budget = (kPageTableMask + 1u) / 2u;
	const uintptr_t  filler          = SyntheticPage(page_size, 0u);
	ASSERT_TRUE(tracker.RegisterRange(filler, (identity_budget - 1u) * page_size));
	const uint64_t snapshot = tracker.SnapshotGeneration(filler, page_size);
	const bool     admitted = tracker.RegisterRange(mapping.address, 2u * page_size);
	EXPECT_FALSE(admitted);
	if (admitted)
	{
		ASSERT_TRUE(tracker.UnregisterRange(mapping.address, 2u * page_size));
		return;
	}
	EXPECT_TRUE(tracker.HandleWriteFault(mapping.address));
	// Existing identities can be covered even at the admission limit.
	ASSERT_TRUE(tracker.RegisterRange(mapping.address, page_size));
	ASSERT_TRUE(tracker.UnregisterRange(mapping.address, page_size));
	EXPECT_FALSE(tracker.ChangedSince(filler, page_size, snapshot));
	ASSERT_TRUE(tracker.NotifyWrite(filler, page_size));
	EXPECT_TRUE(tracker.ChangedSince(filler, page_size, snapshot));
	ASSERT_TRUE(tracker.UnregisterRange(filler, (identity_budget - 1u) * page_size));
	ASSERT_TRUE(tracker.RegisterRange(SyntheticPage(page_size, identity_budget + 1u), page_size));
}

UT_END();
