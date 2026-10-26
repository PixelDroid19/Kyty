#include "Kyty/UnitTest.h"

#include "Emulator/Host/HostInput.h"
#include "Emulator/Host/InputMapper.h"
#include "Emulator/Host/InputProfile.h"

#include <string>

UT_BEGIN(EmulatorInputProfile);

using namespace Emulator::Host;

namespace {

// A complete, valid profile.  Each rejection case changes one field of it.
const char* const VALID_PROFILE = R"({
  "schema": "kyty_input_profile_v1",
  "name": "Test",
  "keyboard": {
    "cross": ["Space"], "circle": [], "square": [], "triangle": [], "up": [], "down": [], "left": [], "right": [],
    "l1": [], "r1": [], "l2": [], "r2": ["Mouse Left"], "l3": [], "r3": [], "options": [], "touchpad": [],
    "left-stick-left": ["A"], "left-stick-right": ["D"], "left-stick-up": ["W"], "left-stick-down": ["S"],
    "right-stick-left": [], "right-stick-right": ["L"], "right-stick-up": [], "right-stick-down": []
  },
  "mouse": { "look": true, "sensitivity": 1.0, "invert_y": true },
  "gamepad": {
    "device": "",
    "buttons": {
      "cross": ["a"], "circle": [], "square": [], "triangle": [], "up": [], "down": [], "left": [], "right": [],
      "l1": [], "r1": [], "l2": [], "r2": [], "l3": [], "r3": [], "options": [], "touchpad": ["back", "touchpad"]
    },
    "left_stick": { "deadzone": 0.25, "invert_x": false, "invert_y": false },
    "right_stick": { "deadzone": 0.0, "invert_x": true, "invert_y": false },
    "trigger_deadzone": 0.5
  }
})";

std::string Replaced(const std::string& from, const std::string& to)
{
	std::string text = VALID_PROFILE;
	text.replace(text.find(from), from.size(), to);
	return text;
}

int Key(const char* name)
{
	return HostInput::ScanCodeFromName(name);
}

} // namespace

TEST(EmulatorInputProfile, RejectsEachInvalidField)
{
	InputProfile profile;
	ASSERT_TRUE(ParseInputProfile(VALID_PROFILE, &profile).empty());

	const std::pair<const char*, const char*> cases[] = {
	    {R"("kyty_input_profile_v1")", R"("kyty_input_profile_v2")"},
	    {R"("name": "Test")", R"("name": "")"},
	    {R"("circle": [], "square")", R"("circle": [], "shield": [], "square")"},
	    {R"("cross": ["Space"])", R"("cross": ["Escape"])"},
	    {R"("cross": ["Space"])", R"("cross": ["F3"])"},
	    {R"("cross": ["Space"])", R"("cross": ["NotAKey"])"},
	    {R"("touchpad": ["back", "touchpad"])", R"("touchpad": ["back"], "left-stick-up": ["dpup"])"},
	    {R"("cross": ["a"])", R"("cross": ["notabutton"])"},
	    {R"("deadzone": 0.25)", R"("deadzone": 1.5)"},
	    {R"("sensitivity": 1.0)", R"("sensitivity": 0.0)"},
	    {R"("invert_y": true)", R"("invert_y": 1)"},
	    {R"("trigger_deadzone": 0.5)", R"("trigger": 0.5)"},
	    {R"("name": "Test",)", ""},
	};
	for (const auto& [from, to]: cases)
	{
		InputProfile rejected;
		EXPECT_FALSE(ParseInputProfile(Replaced(from, to), &rejected).empty()) << to;
	}
}

TEST(EmulatorInputProfile, DefaultProfileDrivesTheKeyboardPad)
{
	const InputProfile  profile = DefaultInputProfile();
	KeyboardMouseMapper keyboard(&profile);

	keyboard.SetInput(Key("W"), true);
	EXPECT_EQ(keyboard.LeftStick().y, 0);
	keyboard.SetInput(Key("A"), true);
	keyboard.SetInput(Key("D"), true);
	EXPECT_EQ(keyboard.LeftStick().x, 128);
	keyboard.SetInput(Key("J"), true);
	EXPECT_EQ(keyboard.RightStick().x, 0);

	const auto cross = keyboard.SetInput(Key("Space"), true);
	EXPECT_EQ(cross.before & ControlBit(PadControl::Cross), 0u);
	EXPECT_NE(cross.after & ControlBit(PadControl::Cross), 0u);
	// Two keys on one control: releasing one keeps the control held.
	keyboard.SetInput(Key("Z"), true);
	EXPECT_NE(keyboard.SetInput(Key("Space"), false).after & ControlBit(PadControl::Cross), 0u);

	// Mouse buttons count only while the mouse is captured.
	EXPECT_EQ(keyboard.SetInput(MOUSE_BUTTON_LEFT, true).after & ControlBit(PadControl::R2), 0u);
	keyboard.SetMouseCaptured(true);
	EXPECT_NE(keyboard.SetInput(MOUSE_BUTTON_LEFT, true).after & ControlBit(PadControl::R2), 0u);
	EXPECT_EQ(keyboard.SetMouseCaptured(false).after & ControlBit(PadControl::R2), 0u);

	const auto released = keyboard.ReleaseAll();
	EXPECT_NE(released.before, 0u);
	EXPECT_EQ(released.after, 0u);
	EXPECT_EQ(keyboard.LeftStick().x, 128);
	EXPECT_EQ(keyboard.LeftStick().y, 128);
}

TEST(EmulatorInputProfile, MouseLookDeflectsTheRightStickOutsideTheGuestDeadzone)
{
	InputProfile profile;
	ASSERT_TRUE(ParseInputProfile(VALID_PROFILE, &profile).empty());
	KeyboardMouseMapper keyboard(&profile);

	keyboard.AddMouseMotion(400, 0);
	keyboard.Tick(10.0);
	EXPECT_EQ(keyboard.RightStick().x, 128) << "motion before capture is ignored";

	keyboard.SetMouseCaptured(true);
	keyboard.Tick(11.0);
	keyboard.AddMouseMotion(20, 5);
	keyboard.Tick(11.5);
	// 40 px/s is a slow turn, yet it must clear the deadzone the pad reports to the guest (about 31/127).
	EXPECT_GT(keyboard.RightStick().x, 128 + 31);
	EXPECT_LT(keyboard.RightStick().x, 200);
	// invert_y is set: downward motion looks up.
	EXPECT_LT(keyboard.RightStick().y, 128 - 31);

	keyboard.AddMouseMotion(-5000, 0);
	keyboard.Tick(12.0);
	EXPECT_LE(keyboard.RightStick().x, 1);

	// A held direction key overrides mouse look on its axis.
	keyboard.SetInput(Key("L"), true);
	EXPECT_EQ(keyboard.RightStick().x, 255);
	keyboard.SetInput(Key("L"), false);

	keyboard.Tick(12.5);
	EXPECT_EQ(keyboard.RightStick().x, 128);
	EXPECT_EQ(keyboard.RightStick().y, 128);
}

TEST(EmulatorInputProfile, GamepadDeadzonesAndRemappedButtons)
{
	InputProfile profile;
	ASSERT_TRUE(ParseInputProfile(VALID_PROFILE, &profile).empty());
	GamepadMapper gamepad(&profile);

	// Left stick, radial deadzone 0.25: a 0.2 tilt rests, full travel still reaches the edge.
	gamepad.SetAxis(7, ControllerAxis::LeftX, 6553);
	EXPECT_EQ(gamepad.LeftStick(7).x, 128);
	gamepad.SetAxis(7, ControllerAxis::LeftX, 32767);
	EXPECT_EQ(gamepad.LeftStick(7).x, 255);
	gamepad.SetAxis(7, ControllerAxis::LeftX, -32768);
	EXPECT_EQ(gamepad.LeftStick(7).x, 0);
	// Just past the deadzone the output starts near rest instead of jumping.
	gamepad.SetAxis(7, ControllerAxis::LeftX, 9830);
	EXPECT_GT(gamepad.LeftStick(7).x, 128);
	EXPECT_LT(gamepad.LeftStick(7).x, 150);

	// Right stick has invert_x.
	gamepad.SetAxis(7, ControllerAxis::RightX, 32767);
	EXPECT_EQ(gamepad.RightStick(7).x, 0);

	// Trigger deadzone 0.5: half travel reads 0, full travel 255.
	gamepad.SetAxis(7, ControllerAxis::TriggerLeft, 16000);
	EXPECT_EQ(gamepad.Trigger(7, ControllerAxis::TriggerLeft), 0);
	gamepad.SetAxis(7, ControllerAxis::TriggerLeft, 32767);
	EXPECT_EQ(gamepad.Trigger(7, ControllerAxis::TriggerLeft), 255);

	// back and touchpad both press touchpad; it releases only when neither is held.
	EXPECT_NE(gamepad.SetButton(7, ControllerButton::Back, true).after & ControlBit(PadControl::Touchpad), 0u);
	gamepad.SetButton(7, ControllerButton::Touchpad, true);
	EXPECT_NE(gamepad.SetButton(7, ControllerButton::Back, false).after & ControlBit(PadControl::Touchpad), 0u);
	EXPECT_EQ(gamepad.SetButton(7, ControllerButton::Touchpad, false).after & ControlBit(PadControl::Touchpad), 0u);
	// An unbound button presses nothing.
	EXPECT_EQ(gamepad.SetButton(7, ControllerButton::Guide, true).after, 0u);
}

TEST(EmulatorInputProfile, ControllerGuidIgnoresTheNameCrc)
{
	EXPECT_TRUE(ControllerGuidMatches("030012344c050000e60c000011810000", "03009abc4C050000E60C000011810000"));
	EXPECT_FALSE(ControllerGuidMatches("030012344c050000e60c000011810000", "050012344c050000e60c000011810000"));
	EXPECT_FALSE(ControllerGuidMatches("030012344c050000e60c000011810000", "030012344c050000ce0d000011810000"));
}

UT_END();
