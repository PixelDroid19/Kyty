#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GpuMemoryMaterializationCache.h"
#include "Emulator/Graphics/GpuMemoryRangeQueryCache.h"

#include <cstdint>
#include <limits>

UT_BEGIN(EmulatorGpuMemoryRangeQueryCache);

using namespace Libs::Graphics;

TEST(EmulatorGpuMemoryRangeQueryCache, ReusesExactQueryUntilObjectRangesMutate)
{
	GpuMemoryRangeQueryCache<int, 8> cache;

	const uint64_t address[] = {0x100000u};
	const uint64_t size[]    = {0x4000u};
	const auto     query     = GpuMemoryRangeQueryKey::Create(address, size, 1, false);

	int value = 0;
	EXPECT_FALSE(cache.Lookup(query, &value));

	cache.Store(query, 42);
	ASSERT_TRUE(cache.Lookup(query, &value));
	EXPECT_EQ(value, 42);

	const uint64_t other_size[] = {0x8000u};
	const auto     other_query  = GpuMemoryRangeQueryKey::Create(address, other_size, 1, false);
	EXPECT_FALSE(cache.Lookup(other_query, &value));

	cache.Invalidate();
	EXPECT_FALSE(cache.Lookup(query, &value));
}

TEST(EmulatorGpuMemoryRangeQueryCache, DistinguishesRangeOrderAndOnlyFirstPolicy)
{
	GpuMemoryRangeQueryCache<int, 8> cache;

	const uint64_t address[] = {0x200000u, 0x300000u};
	const uint64_t size[]    = {0x1000u, 0x2000u};
	const auto     query     = GpuMemoryRangeQueryKey::Create(address, size, 2, false);
	cache.Store(query, 7);

	int value = 0;
	ASSERT_TRUE(cache.Lookup(query, &value));
	EXPECT_EQ(value, 7);

	const uint64_t reverse_address[] = {address[1], address[0]};
	const uint64_t reverse_size[]    = {size[1], size[0]};
	EXPECT_FALSE(cache.Lookup(GpuMemoryRangeQueryKey::Create(reverse_address, reverse_size, 2, false), &value));
	EXPECT_FALSE(cache.Lookup(GpuMemoryRangeQueryKey::Create(address, size, 2, true), &value));
}

TEST(EmulatorGpuMemoryRangeQueryCache, NeverCachesInvalidRangeKeys)
{
	GpuMemoryRangeQueryCache<int, 8> cache;

	const uint64_t address[] = {0x1000u};
	const uint64_t size[]    = {0x100u};
	const uint64_t zero[]    = {0u};
	const uint64_t overflow_address[] = {std::numeric_limits<uint64_t>::max() - 0x7fu};
	const uint64_t overflow_size[]    = {0x100u};
	const auto invalid_count = GpuMemoryRangeQueryKey::Create(address, size, 0, false);
	const auto invalid_zero  = GpuMemoryRangeQueryKey::Create(address, zero, 1, false);
	const auto invalid_overflow = GpuMemoryRangeQueryKey::Create(overflow_address, overflow_size, 1, false);

	int value = 0;
	cache.Store(invalid_count, 9);
	cache.Store(invalid_zero, 10);
	cache.Store(invalid_overflow, 11);
	EXPECT_FALSE(cache.Lookup(invalid_count, &value));
	EXPECT_FALSE(cache.Lookup(invalid_zero, &value));
	EXPECT_FALSE(cache.Lookup(invalid_overflow, &value));
}

TEST(EmulatorGpuMemoryRangeQueryCache, RangeInvalidationKeepsDisjointQueries)
{
	GpuMemoryRangeQueryCache<int, 8> cache;

	const uint64_t first_address[]  = {0x100000u};
	const uint64_t second_address[] = {0x400000u};
	const uint64_t size[]           = {0x1000u};
	const auto     first            = GpuMemoryRangeQueryKey::Create(first_address, size, 1, false);
	const auto     second           = GpuMemoryRangeQueryKey::Create(second_address, size, 1, false);
	cache.Store(first, 1);
	cache.Store(second, 2);

	cache.InvalidateRange(0x100800u, 0x100u);

	int value = 0;
	EXPECT_FALSE(cache.Lookup(first, &value));
	ASSERT_TRUE(cache.Lookup(second, &value));
	EXPECT_EQ(value, 2);
}

TEST(EmulatorGpuMemoryRangeQueryCache, RangeInvalidationChecksEveryQueryRange)
{
	GpuMemoryRangeQueryCache<int, 8> cache;

	const uint64_t address[] = {0x100000u, 0x800000u};
	const uint64_t size[]    = {0x1000u, 0x2000u};
	const auto     query     = GpuMemoryRangeQueryKey::Create(address, size, 2, false);
	cache.Store(query, 3);

	cache.InvalidateRange(0x801000u, 0x100u);

	int value = 0;
	EXPECT_FALSE(cache.Lookup(query, &value));
}

TEST(EmulatorGpuMemoryRangeQueryCache, AdjacentRangeDoesNotInvalidate)
{
	GpuMemoryRangeQueryCache<int, 8> cache;

	const uint64_t address[] = {0x100000u};
	const uint64_t size[]    = {0x1000u};
	const auto     query     = GpuMemoryRangeQueryKey::Create(address, size, 1, false);
	cache.Store(query, 4);

	cache.InvalidateRange(0x101000u, 0x100u);

	int value = 0;
	ASSERT_TRUE(cache.Lookup(query, &value));
	EXPECT_EQ(value, 4);
}

TEST(EmulatorGpuMemoryRangeQueryCache, MaterializationCacheReusesOnlyTheExactGpuUse)
{
	GpuMemoryMaterializationCache<int, 8> cache;

	const uint64_t address[] = {0x100000u};
	const uint64_t size[]    = {0x4000u};
	const uint64_t params[]  = {11u, 22u, 33u};
	const auto key = GpuMemoryMaterializationKey::Create(7u, 2u, 19u, address, size, 1, 4u, params, 3, true, false);

	int value = 0;
	EXPECT_FALSE(cache.Lookup(key, &value));

	cache.Store(key, 42);
	ASSERT_TRUE(cache.Lookup(key, &value));
	EXPECT_EQ(value, 42);

	const auto next_guest_submit =
	    GpuMemoryMaterializationKey::Create(8u, 2u, 19u, address, size, 1, 4u, params, 3, true, false);
	const auto next_host_submit =
	    GpuMemoryMaterializationKey::Create(7u, 2u, 20u, address, size, 1, 4u, params, 3, true, false);
	const auto write_use =
	    GpuMemoryMaterializationKey::Create(7u, 2u, 19u, address, size, 1, 4u, params, 3, true, true);
	EXPECT_FALSE(cache.Lookup(next_guest_submit, &value));
	EXPECT_FALSE(cache.Lookup(next_host_submit, &value));
	EXPECT_FALSE(cache.Lookup(write_use, &value));
}

TEST(EmulatorGpuMemoryRangeQueryCache, MaterializationCacheDistinguishesParametersAndInvalidates)
{
	GpuMemoryMaterializationCache<int, 8> cache;

	const uint64_t address[] = {0x200000u};
	const uint64_t size[]    = {0x1000u};
	const uint64_t params[]  = {1u, 2u};
	const uint64_t changed[] = {1u, 3u};
	const auto key = GpuMemoryMaterializationKey::Create(1u, 1u, 1u, address, size, 1, 5u, params, 2, false, true);
	const auto other = GpuMemoryMaterializationKey::Create(1u, 1u, 1u, address, size, 1, 5u, changed, 2, false, true);

	cache.Store(key, 9);

	int value = 0;
	ASSERT_TRUE(cache.Lookup(key, &value));
	EXPECT_EQ(value, 9);
	EXPECT_FALSE(cache.Lookup(other, &value));

	cache.Invalidate();
	EXPECT_FALSE(cache.Lookup(key, &value));
}

TEST(EmulatorGpuMemoryRangeQueryCache, MaterializationRangeInvalidationKeepsDisjointGpuUses)
{
	GpuMemoryMaterializationCache<int, 8> cache;

	const uint64_t first_address[]  = {0x200000u};
	const uint64_t second_address[] = {0x800000u};
	const uint64_t size[]           = {0x1000u};
	const uint64_t params[]         = {1u};
	const auto first = GpuMemoryMaterializationKey::Create(1u, 1u, 1u, first_address, size, 1, 5u, params, 1, true, true);
	const auto second =
	    GpuMemoryMaterializationKey::Create(1u, 1u, 1u, second_address, size, 1, 5u, params, 1, true, true);
	cache.Store(first, 3);
	cache.Store(second, 7);

	cache.InvalidateRange(0x200800u, 0x100u);

	int value = 0;
	EXPECT_FALSE(cache.Lookup(first, &value));
	ASSERT_TRUE(cache.Lookup(second, &value));
	EXPECT_EQ(value, 7);
}

TEST(EmulatorGpuMemoryRangeQueryCache, MaterializationRangeInvalidationChecksEveryRangeAndKeepsAdjacent)
{
	GpuMemoryMaterializationCache<int, 8> cache;

	const uint64_t address[] = {0x100000u, 0x800000u};
	const uint64_t size[]    = {0x1000u, 0x2000u};
	const uint64_t params[]  = {1u};
	const auto key = GpuMemoryMaterializationKey::Create(1u, 1u, 1u, address, size, 2, 5u, params, 1, true, true);
	cache.Store(key, 9);

	cache.InvalidateRange(0x102000u, 0x100u);
	int value = 0;
	ASSERT_TRUE(cache.Lookup(key, &value));
	EXPECT_EQ(value, 9);

	cache.InvalidateRange(0x801000u, 0x100u);
	EXPECT_FALSE(cache.Lookup(key, &value));
}

TEST(EmulatorGpuMemoryRangeQueryCache, MaterializationCacheRejectsIncompleteKeys)
{
	GpuMemoryMaterializationCache<int, 8> cache;

	const uint64_t address[] = {0x300000u};
	const uint64_t size[]    = {0x1000u};
	const uint64_t params[]  = {1u};
	const auto no_host_submission =
	    GpuMemoryMaterializationKey::Create(1u, 1u, 0u, address, size, 1, 2u, params, 1, false, true);
	const auto no_ranges = GpuMemoryMaterializationKey::Create(1u, 1u, 1u, address, size, 0, 2u, params, 1, false, true);

	int value = 0;
	cache.Store(no_host_submission, 1);
	cache.Store(no_ranges, 2);
	EXPECT_FALSE(cache.Lookup(no_host_submission, &value));
	EXPECT_FALSE(cache.Lookup(no_ranges, &value));
}

UT_END();
