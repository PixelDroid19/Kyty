#include "Kyty/UnitTest.h"

#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Loader/GuestCall.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

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

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static uint64_t g_guest_stack_low = 0;

static uint64_t KYTY_SYSV_ABI HostStackRangeHoldsFrame(uint64_t /*arg0*/, uint64_t /*arg1*/, uint64_t /*arg2*/)
{
	const auto range = Kyty::Core::Thread::GetStackRange();
	const auto frame = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
	g_guest_stack_low = range.low;
	return range.low <= frame && frame < range.high ? 1 : 0;
}

// Windows-native callees probe and validate RSP against the host's stack record. MSVC's __chkstk
// writes a zero into each page between the recorded limit and the new RSP, so a limit above the
// stack's bottom corrupts guest frames once a call nests deeper than its top page.
TEST(EmulatorKernelGuestRuntime, HostStackRecordCoversGuestStackDuringCall)
{
	constexpr uint64_t kStackSize = 64u * 1024u;
	const uint64_t     stack      = Kyty::Core::VirtualMemory::Alloc(0, kStackSize, Kyty::Core::VirtualMemory::Mode::ReadWrite);
	ASSERT_NE(stack, 0u);
	const auto host_stack = Kyty::Core::Thread::GetStackRange();

	EXPECT_EQ(Kyty::Loader::GuestCall::InvokeOnStack(reinterpret_cast<uint64_t>(&HostStackRangeHoldsFrame), 0, 0, 0,
	                                                 reinterpret_cast<void*>(stack + kStackSize)),
	          1u);
	EXPECT_LE(g_guest_stack_low, stack);

	const auto restored = Kyty::Core::Thread::GetStackRange();
	EXPECT_EQ(restored.low, host_stack.low);
	EXPECT_EQ(restored.high, host_stack.high);
	EXPECT_TRUE(Kyty::Core::VirtualMemory::Free(stack));
}
#endif

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
