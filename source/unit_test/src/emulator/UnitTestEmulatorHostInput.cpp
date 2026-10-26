#include "Kyty/UnitTest.h"

#include "Emulator/Host/HostInput.h"

#include <SDL3/SDL.h>

#include <memory>

UT_BEGIN(EmulatorHostInput);

using namespace Emulator::Host;

namespace {

struct VirtualPadFeedback
{
	Uint16 low_frequency  = 0;
	Uint16 high_frequency = 0;
	Uint8  rgb[3]         = {};
};

// Polls until an event of `type` arrives, dropping the others. Bounded, so a
// missing event fails the test instead of hanging it.
bool PollFor(HostInput* input, InputEventType type, InputEvent* out)
{
	for (int attempt = 0; attempt < 1000; attempt++)
	{
		if (input->Poll() == 0)
		{
			SDL_Delay(1);
			continue;
		}
		if (input->GetEvent()->type == type)
		{
			*out = *input->GetEvent();
			return true;
		}
	}
	return false;
}

} // namespace

// An SDL virtual gamepad stands in for a physical one with motion sensors, a
// touchpad, motors and a light bar.
TEST(EmulatorHostInput, VirtualGamepadMotionTouchAndFeedback)
{
	ASSERT_TRUE(SDL_Init(SDL_INIT_GAMEPAD)) << SDL_GetError();
	std::unique_ptr<HostInput> input(HostInput::Create());

	VirtualPadFeedback                  feedback;
	const SDL_VirtualJoystickTouchpadDesc touchpad {2, {}};
	const SDL_VirtualJoystickSensorDesc   sensors[2] = {{SDL_SENSOR_ACCEL, 250.0f}, {SDL_SENSOR_GYRO, 250.0f}};
	SDL_VirtualJoystickDesc               desc;
	SDL_INIT_INTERFACE(&desc);
	desc.type       = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.naxes      = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons   = SDL_GAMEPAD_BUTTON_COUNT;
	desc.ntouchpads = 1;
	desc.touchpads  = &touchpad;
	desc.nsensors   = 2;
	desc.sensors    = sensors;
	desc.name       = "Kyty virtual gamepad";
	desc.userdata   = &feedback;
	desc.Rumble     = [](void* userdata, Uint16 low_frequency, Uint16 high_frequency)
	{
		auto* seen           = static_cast<VirtualPadFeedback*>(userdata);
		seen->low_frequency  = low_frequency;
		seen->high_frequency = high_frequency;
		return true;
	};
	desc.SetLED = [](void* userdata, Uint8 red, Uint8 green, Uint8 blue)
	{
		auto* seen   = static_cast<VirtualPadFeedback*>(userdata);
		seen->rgb[0] = red;
		seen->rgb[1] = green;
		seen->rgb[2] = blue;
		return true;
	};
	const SDL_JoystickID device = SDL_AttachVirtualJoystick(&desc);
	ASSERT_NE(device, 0u) << SDL_GetError();

	int instance = 0;
	ASSERT_TRUE(input->OpenController(static_cast<int>(device), &instance)) << input->LastError();
	SDL_Joystick* joystick = SDL_GetJoystickFromID(device);
	ASSERT_NE(joystick, nullptr);

	// Opening enables the sensors, and acceleration arrives in G.
	InputEvent  event;
	const float acceleration[3] = {0.0f, SDL_STANDARD_GRAVITY, -0.5f * SDL_STANDARD_GRAVITY};
	ASSERT_TRUE(SDL_SendJoystickVirtualSensorData(joystick, SDL_SENSOR_ACCEL, 7, acceleration, 3));
	ASSERT_TRUE(PollFor(input.get(), InputEventType::ControllerSensor, &event));
	EXPECT_EQ(event.controller.id, instance);
	EXPECT_EQ(event.controller.sensor, ControllerSensor::Acceleration);
	EXPECT_FLOAT_EQ(event.controller.sensor_data[0], 0.0f);
	EXPECT_FLOAT_EQ(event.controller.sensor_data[1], 1.0f);
	EXPECT_FLOAT_EQ(event.controller.sensor_data[2], -0.5f);
	EXPECT_EQ(event.controller.sensor_timestamp_ns, 7u);

	const float angular_velocity[3] = {0.5f, -0.25f, 2.0f};
	ASSERT_TRUE(SDL_SendJoystickVirtualSensorData(joystick, SDL_SENSOR_GYRO, 9, angular_velocity, 3));
	ASSERT_TRUE(PollFor(input.get(), InputEventType::ControllerSensor, &event));
	EXPECT_EQ(event.controller.sensor, ControllerSensor::AngularVelocity);
	EXPECT_FLOAT_EQ(event.controller.sensor_data[0], 0.5f);
	EXPECT_FLOAT_EQ(event.controller.sensor_data[1], -0.25f);
	EXPECT_FLOAT_EQ(event.controller.sensor_data[2], 2.0f);
	EXPECT_EQ(event.controller.sensor_timestamp_ns, 9u);

	ASSERT_TRUE(SDL_SetJoystickVirtualTouchpad(joystick, 0, 1, true, 0.25f, 0.75f, 1.0f));
	ASSERT_TRUE(PollFor(input.get(), InputEventType::ControllerTouchpad, &event));
	EXPECT_EQ(event.controller.id, instance);
	EXPECT_TRUE(event.controller.touchpad);
	EXPECT_TRUE(event.controller.touch_down);
	EXPECT_EQ(event.controller.touch_finger, 1);
	EXPECT_FLOAT_EQ(event.controller.touch_x, 0.25f);
	EXPECT_FLOAT_EQ(event.controller.touch_y, 0.75f);

	ASSERT_TRUE(SDL_SetJoystickVirtualTouchpad(joystick, 0, 1, false, 0.25f, 0.75f, 0.0f));
	ASSERT_TRUE(PollFor(input.get(), InputEventType::ControllerTouchpad, &event));
	EXPECT_FALSE(event.controller.touch_down);
	EXPECT_EQ(event.controller.touch_finger, 1);

	HostInput::RumbleController(instance, 0x1234, 0xabcd);
	EXPECT_EQ(feedback.low_frequency, 0x1234u);
	EXPECT_EQ(feedback.high_frequency, 0xabcdu);
	HostInput::SetControllerLed(instance, 1, 2, 3);
	EXPECT_EQ(feedback.rgb[0], 1u);
	EXPECT_EQ(feedback.rgb[1], 2u);
	EXPECT_EQ(feedback.rgb[2], 3u);

	input->CloseController(instance);
	EXPECT_TRUE(SDL_DetachVirtualJoystick(device));
	SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

UT_END();
