#include "Emulator/Libs/Memalign.h"
#include "Kyty/UnitTest.h"

#include <cstddef>

UT_BEGIN(EmulatorLibcMemalign);

TEST(EmulatorLibcMemalign, AcceptsPowerOfTwoIncludingSubPointerAlign)
{
	using Kyty::Libs::LibC::MemalignAlignmentOk;

	// Reject zero and non-powers-of-two.
	EXPECT_FALSE(MemalignAlignmentOk(0));
	EXPECT_FALSE(MemalignAlignmentOk(3));
	EXPECT_FALSE(MemalignAlignmentOk(6));
	EXPECT_FALSE(MemalignAlignmentOk(12));
	EXPECT_FALSE(MemalignAlignmentOk(24));

	// Accept power-of-two alignments below alignof(void*) — required for
	// memalign(4, …) used by Gen5 titles for uint32 tables.
	EXPECT_TRUE(MemalignAlignmentOk(1));
	EXPECT_TRUE(MemalignAlignmentOk(2));
	EXPECT_TRUE(MemalignAlignmentOk(4));
	EXPECT_TRUE(MemalignAlignmentOk(8));
	EXPECT_TRUE(MemalignAlignmentOk(16));
	EXPECT_TRUE(MemalignAlignmentOk(4096));
}

UT_END();
