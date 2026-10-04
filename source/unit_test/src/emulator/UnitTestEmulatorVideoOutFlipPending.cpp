#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/VideoOutFlipPending.h"

UT_BEGIN(EmulatorVideoOutFlipPending);

using namespace Libs::Graphics;

TEST(EmulatorVideoOutFlipPending, ADecodedGpuFlipIsPendingBeforeItReachesTheFlipQueue)
{
	VideoOutFlipPending pending;
	EXPECT_EQ(pending.FlipPendingNum(0), 0);

	pending.QueueGpuFlip();
	EXPECT_EQ(pending.GcQueueNum(), 1);
	EXPECT_EQ(pending.FlipPendingNum(0), 1);

	// The flip queue now holds the request: still one pending flip.
	EXPECT_TRUE(pending.TakeGpuFlip());
	EXPECT_EQ(pending.GcQueueNum(), 0);
	EXPECT_EQ(pending.FlipPendingNum(1), 1);
	EXPECT_EQ(pending.FlipPendingNum(0), 0);
}

TEST(EmulatorVideoOutFlipPending, AFlipQueueEntryWithoutADecodedGpuFlipIsRefused)
{
	VideoOutFlipPending pending;
	EXPECT_FALSE(pending.TakeGpuFlip());

	pending.QueueGpuFlip();
	pending.QueueGpuFlip();
	EXPECT_EQ(pending.FlipPendingNum(1), 3);
	EXPECT_TRUE(pending.TakeGpuFlip());
	EXPECT_TRUE(pending.TakeGpuFlip());
	EXPECT_FALSE(pending.TakeGpuFlip());
}

TEST(EmulatorVideoOutFlipPending, FlipsNameTheBlankIndexOrARegisteredBuffer)
{
	EXPECT_TRUE(VideoOutIsFlippableIndex(VIDEO_OUT_BUFFER_INDEX_BLANK, false));
	EXPECT_TRUE(VideoOutIsFlippableIndex(0, true));
	EXPECT_TRUE(VideoOutIsFlippableIndex(15, true));
	EXPECT_FALSE(VideoOutIsFlippableIndex(0, false));
	EXPECT_FALSE(VideoOutIsFlippableIndex(16, true));
	EXPECT_FALSE(VideoOutIsFlippableIndex(-2, true));
}

UT_END();
