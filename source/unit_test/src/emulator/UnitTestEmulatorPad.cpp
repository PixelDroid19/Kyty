#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Controller.h"
#include "Emulator/Log.h"
#include "Emulator/Ports/ControllerInputPort.h"

#include <array>
#include <cmath>
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


constexpr size_t  kPadDataBytes = 0x78;
constexpr uint8_t kSentinel     = 0xaa;
constexpr int     kInvalidArg   = static_cast<int>(0x80920001u);
constexpr int     kInvalidHandle = static_cast<int>(0x80920003u);

uint32_t Load32(const uint8_t* record, size_t offset)
{
	uint32_t value = 0;
	std::memcpy(&value, record + offset, sizeof(value));
	return value;
}

uint16_t Load16(const uint8_t* record, size_t offset)
{
	uint16_t value = 0;
	std::memcpy(&value, record + offset, sizeof(value));
	return value;
}

float LoadFloat(const uint8_t* record, size_t offset)
{
	float value = 0.0f;
	std::memcpy(&value, record + offset, sizeof(value));
	return value;
}

// Padding, reserved and extension bytes of the 120-byte ScePadData record. The
// native layout reads `connected` (0x4C) as a 32-bit boolean, so its three
// trailing bytes must be zero as well.
void ExpectCleanPadRecord(const uint8_t* record)
{
	for (size_t offset: {0x0au, 0x0bu, 0x35u, 0x36u, 0x37u, 0x38u, 0x39u, 0x3au, 0x3bu, 0x41u, 0x42u, 0x43u, 0x49u, 0x4au, 0x4bu, 0x4du,
	                     0x4eu, 0x4fu, 0x69u, 0x6au, 0x6bu})
	{
		EXPECT_EQ(record[offset], 0u) << "offset 0x" << std::hex << offset;
	}
	for (size_t offset = 0x58; offset < 0x68; ++offset)
	{
		EXPECT_EQ(record[offset], 0u) << "extension offset 0x" << std::hex << offset;
	}
	for (size_t offset = 0x6c; offset < kPadDataBytes; ++offset)
	{
		EXPECT_EQ(record[offset], 0u) << "device data offset 0x" << std::hex << offset;
	}
	EXPECT_EQ(Load32(record, 0x4c), 1u);
	EXPECT_EQ(LoadFloat(record, 0x18), 1.0f); // identity orientation
	// Acceleration is in G: a motionless pad reads about 1, never the m/s^2 figure.
	EXPECT_FLOAT_EQ(LoadFloat(record, 0x24), 1.0f);
	EXPECT_EQ(LoadFloat(record, 0x1c), 0.0f);
	EXPECT_EQ(LoadFloat(record, 0x20), 0.0f);
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

TEST(EmulatorPad, ResetOrientationValidatesOpenHandle)
{
	EnsurePadSubsystems();

	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);
	EXPECT_EQ(PadResetOrientation(1), 0);
	EXPECT_EQ(PadResetOrientation(99), static_cast<int>(0x80920003u));
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

TEST(EmulatorPad, PadReadReturnsCurrentStateAfterHistoryIsDrained)
{
	EnsurePadSubsystems();

	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	// Consume every queued host transition first. A subsequent read still needs
	// to return the current report so a stationary controller remains pollable.
	ControllerAxis(CONTROLLER_KEYBOARD_ID, Axis::LeftX, 128);
	alignas(uint64_t) std::array<uint8_t, 64 * 256> history {};
	ASSERT_GE(PadRead(1, reinterpret_cast<PadData*>(history.data()), 64), 1);

	AgentPadClear();
	AgentPadSetAxis(Axis::LeftX, 200);

	alignas(uint64_t) std::array<uint8_t, 256> current {};
	ASSERT_EQ(PadRead(1, reinterpret_cast<PadData*>(current.data()), 1), 1);
	EXPECT_EQ(current[sizeof(uint32_t)], 200u);

	AgentPadClear();
}

TEST(EmulatorPad, PadReadStateWritesOneCompleteRecordAndNothingBeyondIt)
{
	EnsurePadSubsystems();

	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	alignas(uint64_t) std::array<uint8_t, 0x100> raw {};
	raw.fill(kSentinel);
	ASSERT_EQ(PadReadState(1, reinterpret_cast<PadData*>(raw.data())), 0);

	ExpectCleanPadRecord(raw.data());
	for (size_t offset = kPadDataBytes; offset < raw.size(); ++offset)
	{
		ASSERT_EQ(raw[offset], kSentinel) << "write past the record at 0x" << std::hex << offset;
	}
}

TEST(EmulatorPad, PadReadFillsEveryReturnedRecordAndNothingBeyondThem)
{
	EnsurePadSubsystems();

	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	alignas(uint64_t) std::array<uint8_t, 4 * kPadDataBytes> raw {};
	raw.fill(kSentinel);
	const int count = PadRead(1, reinterpret_cast<PadData*>(raw.data()), 3);
	ASSERT_GE(count, 1);
	ASSERT_LE(count, 3);
	for (int record = 0; record < count; ++record)
	{
		ExpectCleanPadRecord(raw.data() + static_cast<size_t>(record) * kPadDataBytes);
	}
	for (size_t offset = static_cast<size_t>(count) * kPadDataBytes; offset < raw.size(); ++offset)
	{
		ASSERT_EQ(raw[offset], kSentinel) << "write past the returned records at 0x" << std::hex << offset;
	}
}

TEST(EmulatorPad, ReadsRejectBadArgumentsAndHandlesInsteadOfContinuing)
{
	EnsurePadSubsystems();

	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	alignas(uint64_t) std::array<uint8_t, 65 * kPadDataBytes> raw {};
	auto*                                                     data = reinterpret_cast<PadData*>(raw.data());

	EXPECT_EQ(PadReadState(1, nullptr), kInvalidArg);
	EXPECT_EQ(PadReadState(99, data), kInvalidHandle);
	EXPECT_EQ(PadRead(1, nullptr, 1), kInvalidArg);
	EXPECT_EQ(PadRead(1, data, 0), kInvalidArg);
	EXPECT_EQ(PadRead(1, data, 65), kInvalidArg);
	EXPECT_EQ(PadRead(99, data, 1), kInvalidHandle);
	EXPECT_GE(PadRead(1, data, 64), 1); // the documented capacity is 1-64
}

TEST(EmulatorPad, ControllerInformationIsTheFullTwentyEightByteRecord)
{
	EnsurePadSubsystems();

	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	alignas(uint64_t) std::array<uint8_t, 0x40> info {};
	info.fill(kSentinel);
	ASSERT_EQ(PadGetControllerInformation(1, reinterpret_cast<PadControllerInformation*>(info.data())), 0);

	EXPECT_EQ(info[0x0c], 1u); // connected
	EXPECT_GE(info[0x0b], 1u); // connection generation
	EXPECT_EQ(Load32(info.data(), 0x10), 0u); // standard controller class
	for (size_t offset = 0x14; offset < 0x1c; ++offset)
	{
		EXPECT_EQ(info[offset], 0u) << "reserve offset 0x" << std::hex << offset;
	}
	for (size_t offset = 0x1c; offset < info.size(); ++offset)
	{
		ASSERT_EQ(info[offset], kSentinel) << "write past the structure at 0x" << std::hex << offset;
	}

	// The extended record embeds it: 0x1C of base followed by the extension fields, 0x40 in all.
	alignas(uint64_t) std::array<uint8_t, 0x50> ext {};
	ext.fill(kSentinel);
	ASSERT_EQ(PadGetExtControllerInformation(1, ext.data()), 0);
	EXPECT_EQ(ext[0x0c], 1u);
	EXPECT_EQ(ext[0x1d], 1u);
	for (size_t offset = 0x14; offset < 0x1c; ++offset)
	{
		EXPECT_EQ(ext[offset], 0u) << "base reserve offset 0x" << std::hex << offset;
	}
	for (size_t offset = 0x1f; offset < 0x40; ++offset)
	{
		EXPECT_EQ(ext[offset], 0u) << "extension reserve offset 0x" << std::hex << offset;
	}
	for (size_t offset = 0x40; offset < ext.size(); ++offset)
	{
		ASSERT_EQ(ext[offset], kSentinel) << "write past the extended structure at 0x" << std::hex << offset;
	}

	EXPECT_EQ(PadGetControllerInformation(99, reinterpret_cast<PadControllerInformation*>(info.data())), kInvalidHandle);
	EXPECT_EQ(PadGetControllerInformation(1, nullptr), kInvalidArg);
}

TEST(EmulatorPad, OutputAndSensorCallsValidateTheirHandleAndArguments)
{
	EnsurePadSubsystems();

	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	std::array<uint8_t, 4> vibration {};
	EXPECT_EQ(PadSetVibration(1, reinterpret_cast<const PadVibrationParam*>(vibration.data())), 0);
	EXPECT_EQ(PadSetVibration(1, nullptr), kInvalidArg);
	EXPECT_EQ(PadSetVibration(99, reinterpret_cast<const PadVibrationParam*>(vibration.data())), kInvalidHandle);
	EXPECT_EQ(PadResetLightBar(1), 0);
	EXPECT_EQ(PadResetLightBar(99), kInvalidHandle);
	EXPECT_EQ(PadSetLightBar(1, nullptr), kInvalidArg);
	EXPECT_EQ(PadSetLightBar(99, nullptr), kInvalidHandle);
	EXPECT_EQ(PadSetMotionSensorState(1, true), 0);
	EXPECT_EQ(PadSetMotionSensorState(99, true), kInvalidHandle);
}

TEST(EmulatorPad, ActiveGamepadMotionAndTouchReachTheRecord)
{
	EnsurePadSubsystems();

	constexpr int physical_id = 43;
	ControllerConnect(physical_id);
	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	ControllerMotion(physical_id, MotionSensor::Acceleration, 0.25f, 0.75f, -0.5f, 0);
	// 1 rad/s about Z for 0.1 s turns the pad by 0.1 rad: q = (0, 0, sin 0.05, cos 0.05).
	ControllerMotion(physical_id, MotionSensor::AngularVelocity, 0.0f, 0.0f, 1.0f, 1'000'000'000);
	ControllerMotion(physical_id, MotionSensor::AngularVelocity, 0.0f, 0.0f, 1.0f, 1'050'000'000);
	ControllerMotion(physical_id, MotionSensor::AngularVelocity, 0.0f, 0.0f, 1.0f, 1'100'000'000);
	// Samples of a gamepad that is not the active one are ignored.
	ControllerMotion(physical_id + 1, MotionSensor::Acceleration, 9.0f, 9.0f, 9.0f, 0);
	// The second finger alone reads as the first touch.
	ControllerTouch(physical_id, 1, true, 0.5f, 1.0f);

	alignas(uint64_t) std::array<uint8_t, kPadDataBytes> record {};
	ASSERT_EQ(PadReadState(1, reinterpret_cast<PadData*>(record.data())), 0);
	EXPECT_FLOAT_EQ(LoadFloat(record.data(), 0x1c), 0.25f);
	EXPECT_FLOAT_EQ(LoadFloat(record.data(), 0x20), 0.75f);
	EXPECT_FLOAT_EQ(LoadFloat(record.data(), 0x24), -0.5f);
	EXPECT_FLOAT_EQ(LoadFloat(record.data(), 0x30), 1.0f);
	EXPECT_NEAR(LoadFloat(record.data(), 0x14), std::sin(0.05f), 1e-3f);
	EXPECT_NEAR(LoadFloat(record.data(), 0x18), std::cos(0.05f), 1e-3f);
	EXPECT_EQ(record[0x34], 1u);
	EXPECT_EQ(Load16(record.data(), 0x3c), 959u);
	EXPECT_EQ(Load16(record.data(), 0x3e), 942u);
	const uint8_t first_id = record[0x40];
	EXPECT_GE(first_id, 1u);
	EXPECT_LE(first_id, 127u);

	ControllerTouch(physical_id, 0, true, 0.0f, 0.0f);
	alignas(uint64_t) std::array<uint8_t, 64 * kPadDataBytes> history {};
	const int read = PadRead(1, reinterpret_cast<PadData*>(history.data()), 64);
	ASSERT_GE(read, 1);
	const uint8_t* latest = history.data() + (read - 1) * kPadDataBytes;
	EXPECT_EQ(latest[0x34], 2u);
	EXPECT_EQ(Load16(latest, 0x3c), 0u);
	EXPECT_EQ(Load16(latest, 0x44), 959u);
	EXPECT_EQ(latest[0x48], first_id);
	EXPECT_NE(latest[0x40], first_id);

	EXPECT_EQ(PadResetOrientation(1), 0);
	ControllerTouch(physical_id, 0, false, 0.0f, 0.0f);
	ControllerTouch(physical_id, 1, false, 0.0f, 0.0f);
	ASSERT_EQ(PadReadState(1, reinterpret_cast<PadData*>(record.data())), 0);
	EXPECT_EQ(LoadFloat(record.data(), 0x14), 0.0f);
	EXPECT_EQ(LoadFloat(record.data(), 0x18), 1.0f);
	EXPECT_EQ(record[0x34], 0u);

	// Without the gamepad the record reads as a motionless pad again.
	ControllerDisconnect(physical_id);
	ASSERT_EQ(PadReadState(1, reinterpret_cast<PadData*>(record.data())), 0);
	ExpectCleanPadRecord(record.data());
}

struct FeedbackSeen
{
	int     vibrations = 0;
	uint8_t large      = 0;
	uint8_t small      = 0;
	int     light_bars = 0;
	uint8_t rgb[3]     = {};
};

FeedbackSeen g_feedback_seen;

TEST(EmulatorPad, VibrationAndLightBarReachTheFeedbackPort)
{
	EnsurePadSubsystems();

	ASSERT_EQ(PadInit(), 0);
	ASSERT_EQ(PadOpen(1, 0, 0, nullptr), 1);

	g_feedback_seen = {};
	::Kyty::Emulator::Ports::ControllerFeedbackPort::Install(
	    {[](uint8_t large, uint8_t small)
	     {
		     g_feedback_seen.vibrations++;
		     g_feedback_seen.large = large;
		     g_feedback_seen.small = small;
	     },
	     [](uint8_t red, uint8_t green, uint8_t blue)
	     {
		     g_feedback_seen.light_bars++;
		     g_feedback_seen.rgb[0] = red;
		     g_feedback_seen.rgb[1] = green;
		     g_feedback_seen.rgb[2] = blue;
	     }});

	const std::array<uint8_t, 2> vibration {200, 50};
	EXPECT_EQ(PadSetVibration(1, reinterpret_cast<const PadVibrationParam*>(vibration.data())), 0);
	EXPECT_EQ(g_feedback_seen.vibrations, 1);
	EXPECT_EQ(g_feedback_seen.large, 200u);
	EXPECT_EQ(g_feedback_seen.small, 50u);

	const std::array<uint8_t, 4> light_bar {10, 20, 30, 0};
	EXPECT_EQ(PadSetLightBar(1, reinterpret_cast<const PadLightBarParam*>(light_bar.data())), 0);
	EXPECT_EQ(g_feedback_seen.light_bars, 1);
	EXPECT_EQ(g_feedback_seen.rgb[0], 10u);
	EXPECT_EQ(g_feedback_seen.rgb[1], 20u);
	EXPECT_EQ(g_feedback_seen.rgb[2], 30u);

	// A reset restores the user 1 colour, blue.
	EXPECT_EQ(PadResetLightBar(1), 0);
	EXPECT_EQ(g_feedback_seen.light_bars, 2);
	EXPECT_EQ(g_feedback_seen.rgb[0], 0u);
	EXPECT_EQ(g_feedback_seen.rgb[1], 0u);
	EXPECT_EQ(g_feedback_seen.rgb[2], 255u);

	// A failed call asks for nothing.
	EXPECT_EQ(PadSetVibration(99, reinterpret_cast<const PadVibrationParam*>(vibration.data())), kInvalidHandle);
	EXPECT_EQ(g_feedback_seen.vibrations, 1);

	::Kyty::Emulator::Ports::ControllerFeedbackPort::Install({});
}

UT_END();
