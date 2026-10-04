#include "Kyty/UnitTest.h"

#include "Emulator/ControllerState.h"

#include <initializer_list>

UT_BEGIN(EmulatorControllerSources);

using namespace Libs::Controller;

namespace {

ControllerState Current(GameController& controller)
{
	ControllerState state;
	bool connected = false;
	int count = 0;
	controller.ReadState(&state, &connected, &count);
	return state;
}

int ReadHistory(GameController& controller, ControllerState* states)
{
	bool connected = false;
	int count = 0;
	return controller.ReadStates(states, 64, &connected, &count);
}

} // namespace

TEST(EmulatorControllerSources, SharedButtonBothReleaseOrdersPreserveTheOtherHoldAndHistory)
{
	for (const bool keyboard_releases_first: {false, true})
	{
		GameController controller;
		controller.Connect(CONTROLLER_KEYBOARD_ID);
		controller.Connect(10);
		ControllerState history[64];
		ReadHistory(controller, history);
		controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, true);
		controller.Button(10, PAD_BUTTON_CROSS, true);
		controller.Button(keyboard_releases_first ? CONTROLLER_KEYBOARD_ID : 10, PAD_BUTTON_CROSS, false);
		EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_CROSS);
		controller.Button(keyboard_releases_first ? 10 : CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, false);
		EXPECT_EQ(Current(controller).buttons, 0u);
		ASSERT_EQ(ReadHistory(controller, history), 4);
		EXPECT_EQ(history[0].buttons, PAD_BUTTON_CROSS);
		EXPECT_EQ(history[1].buttons, PAD_BUTTON_CROSS);
		EXPECT_EQ(history[2].buttons, PAD_BUTTON_CROSS);
		EXPECT_EQ(history[3].buttons, 0u);
	}
}

TEST(EmulatorControllerSources, DifferentButtonsRemainIndependent)
{
	GameController controller;
	controller.Connect(CONTROLLER_KEYBOARD_ID);
	controller.Connect(10);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, true);
	controller.Button(10, PAD_BUTTON_CIRCLE, true);
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_CROSS | PAD_BUTTON_CIRCLE);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, false);
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_CIRCLE);
	controller.Button(10, PAD_BUTTON_CIRCLE, false);
	EXPECT_EQ(Current(controller).buttons, 0u);
}

TEST(EmulatorControllerSources, DisconnectClearsOnlyThatSource)
{
	for (const bool disconnect_keyboard: {false, true})
	{
		GameController controller;
		controller.Connect(CONTROLLER_KEYBOARD_ID);
		controller.Connect(10);
		controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS | PAD_BUTTON_SQUARE, true);
		controller.Button(10, PAD_BUTTON_CROSS | PAD_BUTTON_CIRCLE, true);
		controller.Disconnect(disconnect_keyboard ? CONTROLLER_KEYBOARD_ID : 10);
		EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_CROSS | (disconnect_keyboard ? PAD_BUTTON_CIRCLE : PAD_BUTTON_SQUARE));
		controller.Button(disconnect_keyboard ? 10 : CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, false);
		EXPECT_EQ(Current(controller).buttons, disconnect_keyboard ? PAD_BUTTON_CIRCLE : PAD_BUTTON_SQUARE);
	}
}

TEST(EmulatorControllerSources, TriggerAxisAndDigitalButtonHaveSeparateOwnership)
{
	GameController controller;
	controller.Connect(CONTROLLER_KEYBOARD_ID);
	controller.Connect(10);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_L2, true);
	controller.Axis(10, Axis::TriggerLeft, 180);
	controller.Axis(10, Axis::TriggerLeft, 0);
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_L2);
	EXPECT_EQ(Current(controller).axes[static_cast<int>(Axis::TriggerLeft)], 0);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_L2, false);
	EXPECT_EQ(Current(controller).buttons, 0u);

	controller.Axis(10, Axis::TriggerRight, 100);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_R2, true);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_R2, false);
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_R2);
	EXPECT_EQ(Current(controller).axes[static_cast<int>(Axis::TriggerRight)], 100);
	controller.Axis(10, Axis::TriggerRight, 0);
	EXPECT_EQ(Current(controller).buttons, 0u);

	// An axis release also cannot erase a digital hold from the same source.
	controller.Button(10, PAD_BUTTON_L2, true);
	controller.Axis(10, Axis::TriggerLeft, 0);
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_L2);
	controller.Axis(10, Axis::TriggerLeft, 200);
	controller.Button(10, PAD_BUTTON_L2, false);
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_L2);
	controller.Axis(10, Axis::TriggerLeft, 0);
	EXPECT_EQ(Current(controller).buttons, 0u);
}

TEST(EmulatorControllerSources, ActiveControllerHandoffPreservesKeyboardAndRetiresOnlyOldPhysicalState)
{
	GameController controller;
	controller.Connect(CONTROLLER_KEYBOARD_ID);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, true);
	controller.Connect(10);
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_CROSS);
	controller.Connect(20);
	controller.Button(10, PAD_BUTTON_CIRCLE, true);
	controller.Button(20, PAD_BUTTON_SQUARE, true); // inactive source is not admitted
	controller.Axis(10, Axis::LeftX, 240);
	ControllerState history[64];
	ReadHistory(controller, history);
	controller.Disconnect(10);
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_CROSS);
	EXPECT_EQ(Current(controller).axes[static_cast<int>(Axis::LeftX)], 128);
	controller.Button(20, PAD_BUTTON_TRIANGLE, true);
	controller.Button(10, PAD_BUTTON_CROSS, false); // departed controller cannot clear the keyboard
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_CROSS | PAD_BUTTON_TRIANGLE);
	ASSERT_EQ(ReadHistory(controller, history), 2);
	EXPECT_EQ(history[0].buttons, PAD_BUTTON_CROSS);
	EXPECT_EQ(history[1].buttons, PAD_BUTTON_CROSS | PAD_BUTTON_TRIANGLE);
}

TEST(EmulatorControllerSources, SingleSourceKeepsAxisPriorityAndContinuousReports)
{
	GameController controller;
	controller.Connect(CONTROLLER_KEYBOARD_ID);
	controller.Axis(CONTROLLER_KEYBOARD_ID, Axis::LeftX, 0);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, true);
	EXPECT_EQ(Current(controller).axes[static_cast<int>(Axis::LeftX)], 0);
	controller.Connect(10);
	controller.Axis(10, Axis::LeftX, 230);
	controller.Axis(CONTROLLER_KEYBOARD_ID, Axis::LeftX, 10);
	EXPECT_EQ(Current(controller).axes[static_cast<int>(Axis::LeftX)], 230);
	EXPECT_EQ(Current(controller).buttons, PAD_BUTTON_CROSS);
	ControllerState history[64];
	ReadHistory(controller, history);
	ASSERT_EQ(ReadHistory(controller, history), 1);
	EXPECT_EQ(history[0].buttons, PAD_BUTTON_CROSS);
	controller.Disconnect(10);
	EXPECT_EQ(Current(controller).axes[static_cast<int>(Axis::LeftX)], 128);
	controller.Disconnect(CONTROLLER_KEYBOARD_ID);
	EXPECT_EQ(Current(controller).buttons, 0u);
}

TEST(EmulatorControllerSources, DisconnectRetainsEarlierEdgesAndPublishesOnlyTheDepartedSourcesRelease)
{
	GameController controller;
	controller.Connect(CONTROLLER_KEYBOARD_ID);
	controller.Connect(10);
	ControllerState history[64];
	ReadHistory(controller, history);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, true);
	controller.Button(10, PAD_BUTTON_CIRCLE, true);
	controller.Disconnect(10);
	controller.Button(CONTROLLER_KEYBOARD_ID, PAD_BUTTON_CROSS, false);
	ASSERT_EQ(ReadHistory(controller, history), 4);
	EXPECT_EQ(history[0].buttons, PAD_BUTTON_CROSS);
	EXPECT_EQ(history[1].buttons, PAD_BUTTON_CROSS | PAD_BUTTON_CIRCLE);
	EXPECT_EQ(history[2].buttons, PAD_BUTTON_CROSS);
	EXPECT_EQ(history[3].buttons, 0u);
}

UT_END();
