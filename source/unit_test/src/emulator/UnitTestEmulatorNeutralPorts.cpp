#include "Kyty/UnitTest.h"

#include "Emulator/Loader/GuestProgramName.h"
#include "Emulator/Ports/AudioPausePort.h"
#include "Emulator/Ports/ControllerInputPort.h"
#include "Emulator/PresentationStats.h"
#include "Emulator/VideoFrameMemory.h"

#include <cstdint>

UT_BEGIN(EmulatorNeutralPorts);

namespace {

int    g_connect_count = 0;
int    g_disconnect_count = 0;
int    g_button_count = 0;
int    g_axis_count = 0;
int    g_last_button_id = -1;
uint32_t g_last_button = 0;
bool   g_last_button_down = false;
int    g_last_axis_id = -1;
::Kyty::Emulator::Ports::Axis g_last_axis = ::Kyty::Emulator::Ports::Axis::LeftX;
int    g_last_axis_value = 0;

void TestConnect(int id)
{
	g_connect_count++;
}

void TestDisconnect(int id)
{
	g_disconnect_count++;
}

void TestButton(int id, uint32_t button, bool down)
{
	g_button_count++;
	g_last_button_id = id;
	g_last_button    = button;
	g_last_button_down = down;
}

void TestAxis(int id, ::Kyty::Emulator::Ports::Axis axis, int value)
{
	g_axis_count++;
	g_last_axis_id    = id;
	g_last_axis       = axis;
	g_last_axis_value = value;
}

int g_pause_count = 0;
int g_last_pause = -1;

void TestSetHostPaused(bool paused)
{
	g_pause_count++;
	g_last_pause = paused ? 1 : 0;
}

int    g_register_count = 0;
int    g_unregister_count = 0;
int    g_notify_count = 0;
uint64_t g_last_base = 0;
uint64_t g_last_size = 0;

void TestRegisterLinearFrame(uint64_t base, size_t size, uint32_t row_pitch_bytes)
{
	g_register_count++;
	g_last_base = base;
	g_last_size = size;
}

void TestUnregisterFrame(uint64_t base)
{
	g_unregister_count++;
	g_last_base = base;
}

void TestNotifyHostWrite(uint64_t base, uint64_t size)
{
	g_notify_count++;
	g_last_base = base;
	g_last_size = size;
}

int g_install_count = 0;
int g_query_count = 0;

bool TestPresentationQuery(void* /*context*/, Kyty::Emulator::PresentationStats::Snapshot* out)
{
	g_query_count++;
	if (out != nullptr)
	{
		out->frame   = 42;
		out->present = 7;
		out->fps     = 60.0;
	}
	return true;
}

} // namespace

TEST(EmulatorNeutralPorts, ControllerInputPortDeliversInstalledCallbacks)
{
	::Kyty::Emulator::Ports::ControllerInputPort::Install({});

	// No-op before install must be safe.
	::Kyty::Emulator::Ports::ControllerInputPort::Connect(1);
	::Kyty::Emulator::Ports::ControllerInputPort::Button(1, 0x00004000, true);
	EXPECT_EQ(g_connect_count, 0);
	EXPECT_EQ(g_button_count, 0);

	::Kyty::Emulator::Ports::ControllerInputPort::Install(
	    {TestConnect, TestDisconnect, TestButton, TestAxis});

	::Kyty::Emulator::Ports::ControllerInputPort::Connect(3);
	::Kyty::Emulator::Ports::ControllerInputPort::Disconnect(3);
	::Kyty::Emulator::Ports::ControllerInputPort::Button(3, 0x00004000, true);
	::Kyty::Emulator::Ports::ControllerInputPort::Axis(3, ::Kyty::Emulator::Ports::Axis::LeftY, 255);

	EXPECT_EQ(g_connect_count, 1);
	EXPECT_EQ(g_disconnect_count, 1);
	EXPECT_EQ(g_button_count, 1);
	EXPECT_EQ(g_last_button_id, 3);
	EXPECT_EQ(g_last_button, 0x00004000u);
	EXPECT_TRUE(g_last_button_down);
	EXPECT_EQ(g_axis_count, 1);
	EXPECT_EQ(g_last_axis_id, 3);
	EXPECT_EQ(g_last_axis, ::Kyty::Emulator::Ports::Axis::LeftY);
	EXPECT_EQ(g_last_axis_value, 255);

	// Reset to no-op dispatch after shutdown.
	::Kyty::Emulator::Ports::ControllerInputPort::Install({});
	::Kyty::Emulator::Ports::ControllerInputPort::Button(1, 1, true);
	EXPECT_EQ(g_button_count, 1);
}

TEST(EmulatorNeutralPorts, AudioPausePortDeliversInstalledCallback)
{
	::Kyty::Emulator::Ports::AudioPausePort::Install(nullptr);
	::Kyty::Emulator::Ports::AudioPausePort::SetHostPaused(true);
	EXPECT_EQ(g_pause_count, 0);

	::Kyty::Emulator::Ports::AudioPausePort::Install(TestSetHostPaused);
	::Kyty::Emulator::Ports::AudioPausePort::SetHostPaused(true);
	::Kyty::Emulator::Ports::AudioPausePort::SetHostPaused(false);
	EXPECT_EQ(g_pause_count, 2);
	EXPECT_EQ(g_last_pause, 0);

	::Kyty::Emulator::Ports::AudioPausePort::Install(nullptr);
	::Kyty::Emulator::Ports::AudioPausePort::SetHostPaused(true);
	EXPECT_EQ(g_pause_count, 2);
}

TEST(EmulatorNeutralPorts, VideoFrameMemoryInstallsCompleteBundlesOnly)
{
	// Partial bundles are rejected and leave the current bundle intact.
	Kyty::Emulator::VideoFrameMemory::Callbacks partial {};
	partial.register_linear_frame = TestRegisterLinearFrame;
	EXPECT_FALSE(Kyty::Emulator::VideoFrameMemory::InstallCallbacks(partial));

	Kyty::Emulator::VideoFrameMemory::Callbacks complete {};
	complete.register_linear_frame = TestRegisterLinearFrame;
	complete.unregister_frame      = TestUnregisterFrame;
	complete.notify_host_write     = TestNotifyHostWrite;
	EXPECT_TRUE(Kyty::Emulator::VideoFrameMemory::InstallCallbacks(complete));

	Kyty::Emulator::VideoFrameMemory::RegisterLinearFrame(0x1000, 4096, 1024);
	EXPECT_EQ(g_register_count, 1);
	EXPECT_EQ(g_last_base, 0x1000u);
	EXPECT_EQ(g_last_size, 4096u);

	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(0x2000, 512);
	EXPECT_EQ(g_notify_count, 1);
	EXPECT_EQ(g_last_base, 0x2000u);
	EXPECT_EQ(g_last_size, 512u);

	Kyty::Emulator::VideoFrameMemory::UnregisterFrame(0x1000);
	EXPECT_EQ(g_unregister_count, 1);
	EXPECT_EQ(g_last_base, 0x1000u);

	// Empty bundle restores no-op dispatch.
	EXPECT_TRUE(Kyty::Emulator::VideoFrameMemory::InstallCallbacks({}));
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(1, 1);
	EXPECT_EQ(g_notify_count, 1);
}

TEST(EmulatorNeutralPorts, PresentationStatsQueriesFailClosedBeforeInstall)
{
	auto& port = Kyty::Emulator::PresentationStats::GetPort();
	Kyty::Emulator::PresentationStats::Snapshot snapshot {};

	EXPECT_FALSE(port.Query(&snapshot));

	Kyty::Emulator::PresentationStats::Callbacks callbacks {nullptr, TestPresentationQuery};
	EXPECT_TRUE(port.Install(callbacks));
	ASSERT_TRUE(port.Query(&snapshot));
	EXPECT_EQ(snapshot.frame, 42);
	EXPECT_EQ(snapshot.present, 7u);

	// The bundle is installed once and remains for the process lifetime; an
	// empty install is rejected and leaves the previous bundle active.
	EXPECT_FALSE(port.Install({}));
	ASSERT_TRUE(port.Query(&snapshot));
	EXPECT_EQ(snapshot.frame, 42);
}

TEST(EmulatorNeutralPorts, GuestProgramNamePublishesLoaderOwnedStorage)
{
	// Storage is pre-initialized to the empty name; the pointer is address-stable.
	ASSERT_NE(::Kyty::Loader::g_progname, nullptr);
	EXPECT_EQ(::Kyty::Core::String::FromUtf8(::Kyty::Loader::g_progname), U"");

	::Kyty::Loader::SetGuestProgramName(U"test-program");
	EXPECT_NE(::Kyty::Loader::g_progname, nullptr);
	EXPECT_EQ(::Kyty::Core::String::FromUtf8(::Kyty::Loader::g_progname), U"test-program");

	// Storage address is stable across updates (guest export contract).
	const char* first = ::Kyty::Loader::g_progname;
	::Kyty::Loader::SetGuestProgramName(U"second-name");
	EXPECT_EQ(::Kyty::Loader::g_progname, first);
	EXPECT_EQ(::Kyty::Core::String::FromUtf8(::Kyty::Loader::g_progname), U"second-name");
}

UT_END();
