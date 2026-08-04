#include "Kyty/UnitTest.h"

#include "Emulator/GuestRuntimePort.h"

#include <cstdint>

UT_BEGIN(EmulatorKernelGuestRuntime);

namespace {

const void* TestFindProgramByAddr(uint64_t vaddr)
{
	return vaddr == 0x1234 ? reinterpret_cast<const void*>(static_cast<uintptr_t>(0x5678)) : nullptr;
}

uint64_t KYTY_SYSV_ABI TestInvoke(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
	return target + arg0 + arg1 + arg2;
}

uint64_t KYTY_SYSV_ABI TestInvokeOnStack(uint64_t target, uint64_t arg0, uint64_t arg1, uint64_t arg2, void* stack_top)
{
	return target + arg0 + arg1 + arg2 + reinterpret_cast<uintptr_t>(stack_top);
}

} // namespace

TEST(EmulatorKernelGuestRuntime, InstalledProviderForwardsOpaqueLookupAndCalls)
{
	Kyty::Emulator::GuestRuntimePort::Install({TestFindProgramByAddr, TestInvoke, nullptr, TestInvokeOnStack});

	EXPECT_EQ(Kyty::Emulator::GuestRuntimePort::FindProgramByAddr(0x1234), reinterpret_cast<const void*>(static_cast<uintptr_t>(0x5678)));
	EXPECT_EQ(Kyty::Emulator::GuestRuntimePort::FindProgramByAddr(0), nullptr);
	EXPECT_EQ(Kyty::Emulator::GuestRuntimePort::Invoke(1, 2, 3, 4), 10u);
	EXPECT_EQ(Kyty::Emulator::GuestRuntimePort::InvokeOnStack(1, 2, 3, 4, reinterpret_cast<void*>(static_cast<uintptr_t>(5))), 15u);

	Kyty::Emulator::GuestRuntimePort::Install({});
	EXPECT_EQ(Kyty::Emulator::GuestRuntimePort::FindProgramByAddr(0x1234), nullptr);
	EXPECT_EQ(Kyty::Emulator::GuestRuntimePort::Invoke(1, 2, 3, 4), 0u);
	EXPECT_EQ(Kyty::Emulator::GuestRuntimePort::InvokeOnStack(1, 2, 3, 4, reinterpret_cast<void*>(static_cast<uintptr_t>(5))), 0u);
}

UT_END();
