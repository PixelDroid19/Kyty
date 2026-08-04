#include "Kyty/UnitTest.h"

#include "Emulator/GpuMemoryFault.h"

UT_BEGIN(EmulatorGpuMemoryFault);

namespace {

struct Probe
{
	uint64_t last_address  = 0;
	int       access_calls = 0;
	int       install_calls = 0;

	static Probe* current;

	static bool Handle(uint64_t address)
	{
		current->last_address = address;
		current->access_calls++;
		return address == 0x1234u;
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
	EXPECT_FALSE(port.HandleAccessViolation(0x1234u));
}

TEST(EmulatorGpuMemoryFault, DispatchesWithoutGraphicsHeaders)
{
	Emulator::GpuMemoryFault::Port port;
	Probe                              probe {};
	Probe::current = &probe;
	const Emulator::GpuMemoryFault::Callbacks callbacks {Probe::Handle, Probe::Installed};

	EXPECT_TRUE(port.Install(callbacks));
	EXPECT_TRUE(port.HandleAccessViolation(0x1234u));
	EXPECT_FALSE(port.HandleAccessViolation(0x5678u));
	port.NotifyFaultHandlerInstalled();

	EXPECT_EQ(probe.last_address, 0x5678u);
	EXPECT_EQ(probe.access_calls, 2);
	EXPECT_EQ(probe.install_calls, 1);
	EXPECT_FALSE(port.Install(callbacks));
}

UT_END();
