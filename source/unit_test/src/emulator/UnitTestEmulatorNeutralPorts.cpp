#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Loader/GuestProgramName.h"
#include "Emulator/Log.h"
#include "Emulator/Ports/AudioPausePort.h"
#include "Emulator/Ports/ControllerInputPort.h"
#include "Emulator/Ports/SocketEventPort.h"
#include "Emulator/PresentationStats.h"
#include "Emulator/VideoFrameMemory.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

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
int    g_session_end_count = 0;

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

void TestSessionEnd()
{
	g_session_end_count++;
}

int g_pause_count = 0;
int g_last_pause = -1;

void TestSetHostPaused(bool paused)
{
	g_pause_count++;
	g_last_pause = paused ? 1 : 0;
}

int      g_register_count       = 0;
int      g_unregister_count     = 0;
int      g_notify_count         = 0;
uint64_t g_last_base            = 0;
uint64_t g_last_size            = 0;
uint64_t g_last_identity_base   = 0;
uint64_t g_last_identity_size   = 0;
uint32_t g_last_row_pitch_bytes = 0;

bool TestRegisterLinearFrame(uint64_t base, size_t size, uint32_t row_pitch_bytes,
	                         const Kyty::Core::VirtualMemory::GuestMappingIdentitySnapshot& identity)
{
	g_register_count++;
	g_last_base            = base;
	g_last_size            = size;
	g_last_row_pitch_bytes = row_pitch_bytes;
	g_last_identity_base   = identity.address;
	g_last_identity_size   = identity.size;
	return true;
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

TEST(EmulatorNeutralPorts, LogDirectionSupportsConcurrentReaders)
{
	ASSERT_EQ(Kyty::Log::GetDirection(), Kyty::Log::Direction::Silent);

	std::atomic_bool start {false};
	std::atomic_bool stop {false};
	std::atomic_bool invalid_direction {false};
	std::vector<std::thread> readers;
	for (uint32_t i = 0; i < 2u; ++i)
	{
		readers.emplace_back(
		    [&]
		    {
			    while (!start.load(std::memory_order_acquire))
			    {
				    std::this_thread::yield();
			    }
			    while (!stop.load(std::memory_order_acquire))
			    {
				    const auto direction = Kyty::Log::GetDirection();
				    if (direction != Kyty::Log::Direction::Silent && direction != Kyty::Log::Direction::Console)
				    {
					    invalid_direction.store(true, std::memory_order_release);
				    }
				    (void)Kyty::Log::IsColoredPrintf();
			    }
		    });
	}

	start.store(true, std::memory_order_release);
	for (uint32_t i = 0; i < 256u; ++i)
	{
		Kyty::Log::SetDirection(Kyty::Log::Direction::Console);
		Kyty::Log::SetDirection(Kyty::Log::Direction::Silent);
	}
	stop.store(true, std::memory_order_release);
	for (auto& reader: readers)
	{
		reader.join();
	}

	EXPECT_FALSE(invalid_direction.load(std::memory_order_acquire));
	EXPECT_EQ(Kyty::Log::GetDirection(), Kyty::Log::Direction::Silent);
}

TEST(EmulatorNeutralPorts, ControllerInputPortDeliversInstalledCallbacks)
{
	::Kyty::Emulator::Ports::ControllerInputPort::Install({});

	// No-op before install must be safe.
	::Kyty::Emulator::Ports::ControllerInputPort::Connect(1);
	::Kyty::Emulator::Ports::ControllerInputPort::Button(1, 0x00004000, true);
	::Kyty::Emulator::Ports::ControllerInputPort::SessionEnd();
	EXPECT_EQ(g_connect_count, 0);
	EXPECT_EQ(g_button_count, 0);
	EXPECT_EQ(g_session_end_count, 0);

	::Kyty::Emulator::Ports::ControllerInputPort::Install(
	    {TestConnect, TestDisconnect, TestButton, TestAxis, TestSessionEnd});

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

	::Kyty::Emulator::Ports::ControllerInputPort::SessionEnd();
	EXPECT_EQ(g_session_end_count, 1);

	// Reset to no-op dispatch after shutdown.
	::Kyty::Emulator::Ports::ControllerInputPort::Install({});
	::Kyty::Emulator::Ports::ControllerInputPort::Button(1, 1, true);
	EXPECT_EQ(g_button_count, 1);
}

// Before install the kernel must treat every descriptor as a file and fail
// socket calls closed; after install it reaches the network implementation.
TEST(EmulatorNeutralPorts, SocketEventPortFailsClosedBeforeInstall)
{
	using ::Kyty::Emulator::Ports::SocketEventPort;
	SocketEventPort::Install({});
	bool    ready = true;
	int64_t data  = 0;
	EXPECT_FALSE(SocketEventPort::IsSocket(5));
	EXPECT_EQ(SocketEventPort::Readiness(5, false, &ready, &data), SocketEventPort::NOT_INSTALLED);
	EXPECT_EQ(SocketEventPort::Watch(5, false, 1, 1, nullptr), SocketEventPort::NOT_INSTALLED);
	SocketEventPort::Unwatch(5, false, 1);

	SocketEventPort::Install({[](int id) { return id == 5; },
	                          [](int /*id*/, bool write, bool* is_ready, int64_t* bytes)
	                          {
		                          *is_ready = write;
		                          *bytes    = 64;
		                          return 0;
	                          },
	                          [](int id, bool /*write*/, uint64_t owner, uint64_t /*generation*/,
	                             ::Kyty::Emulator::Ports::SocketReadyNotify /*notify*/) { return static_cast<int>(owner) + id; },
	                          nullptr});
	EXPECT_TRUE(SocketEventPort::IsSocket(5));
	EXPECT_FALSE(SocketEventPort::IsSocket(6));
	EXPECT_EQ(SocketEventPort::Readiness(5, true, &ready, &data), 0);
	EXPECT_TRUE(ready);
	EXPECT_EQ(data, 64);
	EXPECT_EQ(SocketEventPort::Watch(5, false, 2, 1, nullptr), 7);
	SocketEventPort::Unwatch(5, false, 2);

	SocketEventPort::Install({});
	EXPECT_FALSE(SocketEventPort::IsSocket(5));
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

	struct GuestAllocation
	{
		uint64_t address = 0;
		~GuestAllocation()
		{
			if (address != 0)
			{
				(void)Kyty::Core::VirtualMemory::Free(address);
			}
		}
	} frame {Kyty::Core::VirtualMemory::Alloc(0, 4096, Kyty::Core::VirtualMemory::Mode::ReadWrite)};
	ASSERT_NE(frame.address, 0u);

	EXPECT_TRUE(Kyty::Emulator::VideoFrameMemory::RegisterLinearFrame(frame.address, 4096, 1024));
	EXPECT_EQ(g_register_count, 1);
	EXPECT_EQ(g_last_base, frame.address);
	EXPECT_EQ(g_last_size, 4096u);
	EXPECT_EQ(g_last_row_pitch_bytes, 1024u);
	EXPECT_EQ(g_last_identity_base, frame.address);
	EXPECT_EQ(g_last_identity_size, 4096u);
	EXPECT_FALSE(Kyty::Emulator::VideoFrameMemory::RegisterLinearFrame(0x1000, 4096, 1024));
	EXPECT_EQ(g_register_count, 1);

	Kyty::Core::VirtualMemory::GuestMappingIdentitySnapshot identity {};
	ASSERT_TRUE(Kyty::Core::VirtualMemory::CaptureGuestMappingIdentitySnapshot(frame.address, 4096, &identity));
	EXPECT_TRUE(Kyty::Emulator::VideoFrameMemory::RegisterLinearFrame(frame.address, 4096, 1024, identity));
	EXPECT_EQ(g_register_count, 2);

	identity.address++;
	EXPECT_FALSE(Kyty::Emulator::VideoFrameMemory::RegisterLinearFrame(frame.address, 4096, 1024, identity));
	EXPECT_EQ(g_register_count, 2);
	identity.address = frame.address;
	identity.size++;
	EXPECT_FALSE(Kyty::Emulator::VideoFrameMemory::RegisterLinearFrame(frame.address, 4096, 1024, identity));
	EXPECT_EQ(g_register_count, 2);

	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(0x2000, 512);
	EXPECT_EQ(g_notify_count, 1);
	EXPECT_EQ(g_last_base, 0x2000u);
	EXPECT_EQ(g_last_size, 512u);

	Kyty::Emulator::VideoFrameMemory::UnregisterFrame(frame.address);
	EXPECT_EQ(g_unregister_count, 1);
	EXPECT_EQ(g_last_base, frame.address);

	// Empty bundle restores no-op dispatch.
	EXPECT_TRUE(Kyty::Emulator::VideoFrameMemory::InstallCallbacks({}));
	EXPECT_TRUE(Kyty::Emulator::VideoFrameMemory::RegisterLinearFrame(frame.address, 4096, 1024));
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(1, 1);
	EXPECT_EQ(g_notify_count, 1);
	EXPECT_EQ(g_register_count, 2);
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
