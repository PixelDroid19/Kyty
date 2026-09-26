#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Log.h"

UT_BEGIN(EmulatorComputeWaveDispatchPacket);

using namespace Libs::Graphics;

namespace {

struct TestCommandBuffer
{
	uint32_t* bottom      = nullptr;
	uint32_t* top         = nullptr;
	uint32_t* cursor_up   = nullptr;
	uint32_t* cursor_down = nullptr;
	void*     callback    = nullptr;
	void*     user_data   = nullptr;
	uint32_t  reserved_dw = 0;
	uint32_t  pad         = 0;
};

} // namespace

TEST(EmulatorComputeWaveDispatchPacket, DirectPreservesWave32InitiatorBit)
{
	uint32_t command[5] = {};
	ASSERT_EQ(Gen5::GraphicsEncodeDispatch(command, 5, 2, 3, 4, 0x8000u), 5u);
	EXPECT_EQ(command[0], KYTY_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0u));
	EXPECT_EQ(command[4], 0x8041u);
}

TEST(EmulatorComputeWaveDispatchPacket, IndirectPreservesWave32InitiatorBit)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	uint32_t          storage[8] = {};
	TestCommandBuffer buffer {};
	buffer.bottom      = storage;
	buffer.top         = storage + 8;
	buffer.cursor_up   = storage;
	buffer.cursor_down = storage + 8;

	uint32_t* command = Gen5::GraphicsDcbDispatchIndirect(reinterpret_cast<Gen5::CommandBuffer*>(&buffer), 0u, 0x8000u);
	ASSERT_NE(command, nullptr);
	EXPECT_EQ(command[0], KYTY_PM4(3, Pm4::IT_DISPATCH_INDIRECT, 0u));
	EXPECT_EQ(command[2], 0x8041u);
}

UT_END();
