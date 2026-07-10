#include "Emulator/Kernel/Pthread.h"
#include "Kyty/UnitTest.h"

UT_BEGIN(EmulatorKernelProcess);

using namespace Libs;

TEST(EmulatorKernelProcess, GuestProcessIdIsStable)
{
	const int first = Posix::getpid();
	EXPECT_GT(first, 0);
	EXPECT_EQ(Posix::getpid(), first);
}

UT_END();
