#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GdsRange.h"

#include <cstdint>
#include <limits>

UT_BEGIN(EmulatorGraphicsGdsRange);

using namespace Libs::Graphics;

TEST(EmulatorGraphicsGdsRange, AcceptsAlignedValidEndAndEmptyRanges)
{
	EXPECT_TRUE(GraphicsGdsDwordRangeValid(0, 0));
	EXPECT_TRUE(GraphicsGdsDwordRangeValid(0, kGraphicsGdsDwords));
	EXPECT_TRUE(GraphicsGdsDwordRangeValid(kGraphicsGdsDwords - 1, 1));
	EXPECT_TRUE(GraphicsGdsDwordRangeValid(kGraphicsGdsDwords, 0));
}

TEST(EmulatorGraphicsGdsRange, RejectsSpansPastTheGuestWindow)
{
	EXPECT_FALSE(GraphicsGdsDwordRangeValid(kGraphicsGdsDwords + 1, 0));
	EXPECT_FALSE(GraphicsGdsDwordRangeValid(kGraphicsGdsDwords - 1, 2));
	EXPECT_FALSE(GraphicsGdsDwordRangeValid(0, kGraphicsGdsDwords + 1));
}

TEST(EmulatorGraphicsGdsRange, RejectsOverflowingSpansWithoutWrapping)
{
	constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
	EXPECT_FALSE(GraphicsGdsDwordRangeValid(kMax, 1));
	EXPECT_FALSE(GraphicsGdsDwordRangeValid(1, kMax));
	EXPECT_FALSE(GraphicsGdsDwordRangeValid(kMax, kMax));
	// A count that would truncate to 1 in 32 bits must not pass.
	EXPECT_FALSE(GraphicsGdsDwordRangeValid(0, 0x100000001ull));
}

TEST(EmulatorGraphicsGdsRange, ByteRangesNeedDwordAlignedOffsetAndCount)
{
	EXPECT_TRUE(GraphicsGdsByteRangeValid(0, kGraphicsGdsDwords * 4));
	EXPECT_TRUE(GraphicsGdsByteRangeValid(kGraphicsGdsDwords * 4, 0));
	EXPECT_FALSE(GraphicsGdsByteRangeValid(2, 4));
	EXPECT_FALSE(GraphicsGdsByteRangeValid(0, 6));
	EXPECT_FALSE(GraphicsGdsByteRangeValid(0, kGraphicsGdsDwords * 4 + 4));
	EXPECT_FALSE(GraphicsGdsByteRangeValid(0, 0x100000004ull));
	EXPECT_FALSE(GraphicsGdsByteRangeValid(std::numeric_limits<uint64_t>::max() - 3, 4));
}

TEST(EmulatorGraphicsGdsRange, EopPackedWordIsCountHighHalfAndOffsetLowHalf)
{
	// {count[31:16], offset[15:0]} in dwords.
	EXPECT_TRUE(GraphicsGdsEopValueRangeValid(0x00000000u));
	EXPECT_TRUE(GraphicsGdsEopValueRangeValid(0x00003000u));
	EXPECT_TRUE(GraphicsGdsEopValueRangeValid(0x00010000u | 0x2fffu));
	EXPECT_TRUE(GraphicsGdsEopValueRangeValid(0x30000000u));
	EXPECT_FALSE(GraphicsGdsEopValueRangeValid(0x00020000u | 0x2fffu));
	EXPECT_FALSE(GraphicsGdsEopValueRangeValid(0xffffffffu));
}

TEST(EmulatorGraphicsGdsRange, OverlapNeedsANonEmptyIntersection)
{
	// Adjacent spans touch but do not overlap, so a same-buffer copy between them is valid.
	EXPECT_FALSE(GraphicsGdsDwordRangesOverlap(0, 4, 4));
	EXPECT_FALSE(GraphicsGdsDwordRangesOverlap(4, 0, 4));
	EXPECT_TRUE(GraphicsGdsDwordRangesOverlap(0, 3, 4));
	EXPECT_TRUE(GraphicsGdsDwordRangesOverlap(2, 2, 1));
	// An empty span never overlaps.
	EXPECT_FALSE(GraphicsGdsDwordRangesOverlap(5, 5, 0));
	EXPECT_FALSE(GraphicsGdsDwordRangesOverlap(0, kGraphicsGdsDwords, 0));
}

UT_END();
