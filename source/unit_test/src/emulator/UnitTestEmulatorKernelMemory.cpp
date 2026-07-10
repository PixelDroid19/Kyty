#include "Emulator/Config.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"
#include "Kyty/UnitTest.h"

UT_BEGIN(EmulatorKernelMemory);

using namespace Libs;

TEST(EmulatorKernelMemory, CheckedReleaseReportsGuestErrors)
{
	Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	LibKernel::Memory::MemorySubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	int64_t address = 0;
	ASSERT_EQ(LibKernel::Memory::KernelAllocateDirectMemory(0x10000, 0x40000, 0x10000, 0x10000, 12, &address), OK);
	EXPECT_EQ(LibKernel::Memory::KernelCheckedReleaseDirectMemory(address, 0x10000), OK);
	EXPECT_EQ(LibKernel::Memory::KernelCheckedReleaseDirectMemory(address, 0x10000), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(LibKernel::Memory::KernelReleaseDirectMemory(address, 0x10000), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(LibKernel::Memory::KernelCheckedReleaseDirectMemory(address, 0), LibKernel::KERNEL_ERROR_EINVAL);
}

UT_END();
