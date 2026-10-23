#include "Kyty/UnitTest.h"

#include "Emulator/GpuMemoryFault.h"

UT_BEGIN(EmulatorGpuMemoryFault);

namespace {

using Access = Emulator::GpuMemoryFault::AccessViolationKind;

struct Probe
{
	uint64_t last_address  = 0;
	Access   last_access   = Access::Unknown;
	int       access_calls = 0;
	int       install_calls = 0;

	static Probe* current;

	static bool Handle(uint64_t address, Access access)
	{
		current->last_address = address;
		current->last_access  = access;
		current->access_calls++;
		return address == 0x1234u && access == Access::Write;
	}

	static void Installed()
	{
		current->install_calls++;
	}
};

Probe* Probe::current = nullptr;

} // namespace

TEST(EmulatorGpuMemoryFault, RejectsIncompleteCallbacks)
{
	Emulator::GpuMemoryFault::Port port;
	Emulator::GpuMemoryFault::Callbacks callbacks {};
	callbacks.access_violation = Probe::Handle;
	EXPECT_FALSE(port.Install(callbacks));
	EXPECT_FALSE(port.HandleAccessViolation(0x1234u, Access::Write));
}

TEST(EmulatorGpuMemoryFault, DispatchesWithoutGraphicsHeaders)
{
	Emulator::GpuMemoryFault::Port port;
	Probe                              probe {};
	Probe::current = &probe;
	const Emulator::GpuMemoryFault::Callbacks callbacks {Probe::Handle, Probe::Installed};

	EXPECT_TRUE(port.Install(callbacks));
	EXPECT_TRUE(port.HandleAccessViolation(0x1234u, Access::Write));
	EXPECT_FALSE(port.HandleAccessViolation(0x5678u, Access::Write));
	port.NotifyFaultHandlerInstalled();

	EXPECT_EQ(probe.last_address, 0x5678u);
	EXPECT_EQ(probe.access_calls, 2);
	EXPECT_EQ(probe.install_calls, 1);
	EXPECT_FALSE(port.Install(callbacks));
}

// The decoded access kind reaches the adapter unchanged, including Unknown for
// hosts that cannot classify the access.
TEST(EmulatorGpuMemoryFault, ForwardsTheDecodedAccessKind)
{
	Emulator::GpuMemoryFault::Port port;
	Probe                              probe {};
	Probe::current = &probe;
	ASSERT_TRUE(port.Install({Probe::Handle, Probe::Installed}));

	for (const auto access: {Access::Unknown, Access::Read, Access::Write, Access::Execute})
	{
		EXPECT_EQ(port.HandleAccessViolation(0x1234u, access), access == Access::Write);
		EXPECT_EQ(probe.last_access, access);
	}
	EXPECT_EQ(probe.access_calls, 4);
}

UT_END();
