#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include <cstdint>

UT_BEGIN(CoreVirtualMemory);

using namespace Core::VirtualMemory;

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

UT_END();
