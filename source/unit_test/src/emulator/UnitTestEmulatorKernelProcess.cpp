#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Config.h"
#include "Emulator/Log.h"
#include "Kyty/UnitTest.h"

UT_BEGIN(EmulatorKernelProcess);

using namespace Libs;

namespace {

void EnsureKernelProcessSubsystems()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
}

} // namespace

TEST(EmulatorKernelProcess, GuestProcessIdIsStable)
{
	const int first = Posix::getpid();
	EXPECT_GT(first, 0);
	EXPECT_EQ(Posix::getpid(), first);
}

TEST(EmulatorKernelProcess, GettimeofdayAdvancesWithinOneSecond)
{
	EnsureKernelProcessSubsystems();
	LibKernel::KernelTimeval before {};
	LibKernel::KernelTimeval after {};

	ASSERT_EQ(LibKernel::KernelGettimeofday(&before), 0);
	ASSERT_EQ(LibKernel::KernelUsleep(5'000), 0);
	ASSERT_EQ(LibKernel::KernelGettimeofday(&after), 0);

	const int64_t elapsed_us = (after.tv_sec - before.tv_sec) * 1'000'000 + (after.tv_usec - before.tv_usec);
	EXPECT_GE(elapsed_us, 1'000);
	EXPECT_LT(elapsed_us, 100'000);
}

UT_END();
