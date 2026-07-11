#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Controller.h"
#include "Emulator/Log.h"

#include <array>
#include <cstring>

UT_BEGIN(EmulatorPad);

using namespace Libs::Controller;

namespace {

void EnsurePadSubsystems()
{
	static bool once = false;
	if (once)
	{
		return;
	}
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ControllerSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	once = true;
}

} // namespace

TEST(EmulatorPad, GetHandleAndDualsenseNoops)
{
	EnsurePadSubsystems();

	EXPECT_EQ(PadInit(), 0);
	EXPECT_EQ(PadOpen(1, 0, 0, nullptr), 1);
	EXPECT_EQ(PadGetHandle(1, 0, 0), 1);
	// Not opened for other user/index.
	EXPECT_EQ(PadGetHandle(-1, 0, 0), static_cast<int>(0x80920008u));
	EXPECT_EQ(PadGetHandle(1, 0, 1), static_cast<int>(0x80920008u));

	// DualSense extras: accept primary handle, reject invalid.
	EXPECT_EQ(PadSetVibrationMode(1, 0), 0);
	EXPECT_EQ(PadSetTriggerEffect(1, nullptr), 0);
	EXPECT_EQ(PadSetVibrationMode(99, 0), static_cast<int>(0x80920003u));
	EXPECT_EQ(PadSetTriggerEffect(99, nullptr), static_cast<int>(0x80920003u));
}

TEST(EmulatorPad, KeyboardButtonsRemainVisibleWithPhysicalController)
{
	EnsurePadSubsystems();

	constexpr int physical_id = 42;
	ControllerConnect(physical_id);
	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	ControllerButton(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, true);

	alignas(uint64_t) std::array<uint8_t, 256> raw_data {};
	ASSERT_EQ(PadReadState(1, reinterpret_cast<PadData*>(raw_data.data())), 0);

	uint32_t buttons = 0;
	std::memcpy(&buttons, raw_data.data(), sizeof(buttons));
	EXPECT_NE(buttons & PAD_BUTTON_CROSS, 0u);

	ControllerButton(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, false);
	ControllerDisconnect(physical_id);
}

UT_END();
