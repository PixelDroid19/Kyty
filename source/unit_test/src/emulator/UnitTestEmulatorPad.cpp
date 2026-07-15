#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Controller.h"
#include "Emulator/Graphics/KeyboardInput.h"
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

// Pure keyboard seam: A/D/W/S press and release map to left stick; opposites
// neutralize to 128; reset returns neutral. Key codes match SDL letter values.
TEST(EmulatorPad, KeyboardMovementKeyPressReleaseMapsLeftStick)
{
	using namespace Kyty::Libs::Graphics;

	KeyboardLeftStickState state {};

	auto press_a = ApplyKeyboardLeftStickKey(state, 'a', true);
	EXPECT_TRUE(press_a.handled);
	EXPECT_TRUE(press_a.changed);
	EXPECT_EQ(press_a.axes.x, 0);
	EXPECT_EQ(press_a.axes.y, 128);

	auto release_a = ApplyKeyboardLeftStickKey(state, 'a', false);
	EXPECT_TRUE(release_a.handled);
	EXPECT_TRUE(release_a.changed);
	EXPECT_EQ(release_a.axes.x, 128);
	EXPECT_EQ(release_a.axes.y, 128);

	auto press_d = ApplyKeyboardLeftStickKey(state, 'd', true);
	EXPECT_TRUE(press_d.handled);
	EXPECT_TRUE(press_d.changed);
	EXPECT_EQ(press_d.axes.x, 255);

	auto press_a_with_d = ApplyKeyboardLeftStickKey(state, 'a', true);
	EXPECT_TRUE(press_a_with_d.handled);
	EXPECT_TRUE(press_a_with_d.changed);
	EXPECT_EQ(press_a_with_d.axes.x, 128);

	EXPECT_TRUE(ApplyKeyboardLeftStickKey(state, 'a', false).handled);
	EXPECT_TRUE(ApplyKeyboardLeftStickKey(state, 'd', false).handled);

	auto press_w = ApplyKeyboardLeftStickKey(state, 'w', true);
	EXPECT_TRUE(press_w.handled);
	EXPECT_EQ(press_w.axes.y, 0);

	auto press_s = ApplyKeyboardLeftStickKey(state, 's', true);
	EXPECT_TRUE(press_s.handled);
	EXPECT_EQ(press_s.axes.y, 128);

	EXPECT_TRUE(ApplyKeyboardLeftStickKey(state, 'w', false).handled);
	auto only_s = ApplyKeyboardLeftStickKey(state, 's', true);
	EXPECT_TRUE(only_s.handled);
	EXPECT_EQ(only_s.axes.y, 255);
	EXPECT_TRUE(ApplyKeyboardLeftStickKey(state, 's', false).handled);

	// Unknown key is not handled.
	auto ignored = ApplyKeyboardLeftStickKey(state, 'z', true);
	EXPECT_FALSE(ignored.handled);
	EXPECT_FALSE(ignored.changed);

	// Hold left then reset.
	ASSERT_TRUE(ApplyKeyboardLeftStickKey(state, 'a', true).handled);
	auto reset = ResetKeyboardLeftStick(state);
	EXPECT_TRUE(reset.changed);
	EXPECT_EQ(reset.axes.x, 128);
	EXPECT_EQ(reset.axes.y, 128);
	EXPECT_FALSE(state.left);
	EXPECT_FALSE(state.right);
	EXPECT_FALSE(state.up);
	EXPECT_FALSE(state.down);
}

TEST(EmulatorPad, KeyboardAxesAreReturnedByPadReadState)
{
	EnsurePadSubsystems();

	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	ControllerAxis(CONTROLLER_KEYBOARD_ID, Axis::LeftX, 0);
	ControllerAxis(CONTROLLER_KEYBOARD_ID, Axis::LeftY, 255);

	alignas(uint64_t) std::array<uint8_t, 256> raw_data {};
	ASSERT_EQ(PadReadState(1, reinterpret_cast<PadData*>(raw_data.data())), 0);

	// PadData starts with buttons followed by the two left-stick bytes.
	EXPECT_EQ(raw_data[sizeof(uint32_t)], 0u);
	EXPECT_EQ(raw_data[sizeof(uint32_t) + 1], 255u);

	ControllerAxis(CONTROLLER_KEYBOARD_ID, Axis::LeftX, 128);
	ControllerAxis(CONTROLLER_KEYBOARD_ID, Axis::LeftY, 128);
}

UT_END();
