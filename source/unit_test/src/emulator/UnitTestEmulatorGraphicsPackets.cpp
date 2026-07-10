#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"

UT_BEGIN(EmulatorGraphicsPackets);

using namespace Libs::Graphics;

TEST(EmulatorGraphicsPackets, EncodesContiguousShRegisters)
{
	const ShaderRegister registers[] = {{0x20cu, 0x11111111u}, {0x20du, 0x22222222u}};
	uint32_t             command[4]  = {};

	ASSERT_EQ(Gen5::GraphicsGetShRegistersPacketSize(registers, 2), 4u);
	ASSERT_EQ(Gen5::GraphicsEncodeShRegisters(command, 4, registers, 2), 4u);
	EXPECT_EQ(command[0], KYTY_PM4(4, Pm4::IT_SET_SH_REG, 0));
	EXPECT_EQ(command[1], 0x20cu);
	EXPECT_EQ(command[2], 0x11111111u);
	EXPECT_EQ(command[3], 0x22222222u);
}

TEST(EmulatorGraphicsPackets, GroupsSortedShRegisters)
{
	const ShaderRegister registers[] = {{0x20fu, 0x33333333u}, {0x20cu, 0x11111111u}, {0x20du, 0x22222222u}};
	uint32_t             command[7]  = {};

	ASSERT_EQ(Gen5::GraphicsEncodeShRegisters(command, 7, registers, 3), 7u);
	EXPECT_EQ(command[0], KYTY_PM4(4, Pm4::IT_SET_SH_REG, 0));
	EXPECT_EQ(command[1], 0x20cu);
	EXPECT_EQ(command[2], 0x11111111u);
	EXPECT_EQ(command[3], 0x22222222u);
	EXPECT_EQ(command[4], KYTY_PM4(3, Pm4::IT_SET_SH_REG, 0));
	EXPECT_EQ(command[5], 0x20fu);
	EXPECT_EQ(command[6], 0x33333333u);
}

TEST(EmulatorGraphicsPackets, EncodesDispatch)
{
	uint32_t command[5] = {};

	ASSERT_EQ(Gen5::GraphicsEncodeDispatch(command, 5, 2, 3, 4, 0), 5u);
	EXPECT_EQ(command[0], KYTY_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0));
	EXPECT_EQ(command[1], 2u);
	EXPECT_EQ(command[2], 3u);
	EXPECT_EQ(command[3], 4u);
	EXPECT_EQ(command[4], 0x41u);
}

TEST(EmulatorGraphicsPackets, ParsesGen5LshlAddU32)
{
	const uint32_t shader[] = {0xd7460003u, 0x040a0300u, 0xbf810000u};

	Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	Config::SetNextGen(true);

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::VLshlAddU32);
	EXPECT_EQ(instruction.format, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2);
	EXPECT_EQ(instruction.dst.type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.dst.register_id, 3);
	EXPECT_EQ(instruction.src[0].register_id, 0);
	EXPECT_EQ(instruction.src[1].register_id, 1);
	EXPECT_EQ(instruction.src[2].register_id, 2);
}

TEST(EmulatorGraphicsPackets, RejectsInvalidPacketInputs)
{
	const ShaderRegister outside_sh_space[] = {{Pm4::SH_NUM, 1u}};
	uint32_t             command[5]         = {};

	EXPECT_EQ(Gen5::GraphicsGetShRegistersPacketSize(nullptr, 1), 0u);
	EXPECT_EQ(Gen5::GraphicsGetShRegistersPacketSize(outside_sh_space, 1), 0u);
	EXPECT_EQ(Gen5::GraphicsEncodeShRegisters(command, 1, outside_sh_space, 1), 0u);
	EXPECT_EQ(Gen5::GraphicsEncodeDispatch(command, 4, 1, 1, 1, 0), 0u);
}

TEST(EmulatorGraphicsPackets, SizesResourceRegistrationMemory)
{
	size_t small = 0;
	size_t large = 0;

	EXPECT_LT(Gen5Driver::GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(nullptr, 32, 4), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(&small, 32, 4), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(&large, 64, 8), 0);
	EXPECT_GT(small, 0u);
	EXPECT_GT(large, small);
	EXPECT_EQ(small % 64u, 0u);

	std::unique_ptr<uint8_t[]> memory(new uint8_t[small]);
	EXPECT_LT(Gen5Driver::GraphicsDriverInitResourceRegistration(nullptr, small, 4), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverInitResourceRegistration(memory.get(), 0, 4), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverInitResourceRegistration(memory.get(), small, 4), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverRegisterDefaultOwner(0), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterDefaultOwner(1), 0);
	uint32_t default_owner = 0;
	EXPECT_LT(Gen5Driver::GraphicsDriverGetDefaultOwner(nullptr), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverGetDefaultOwner(&default_owner), 0);
	EXPECT_GT(default_owner, 0u);
	uint32_t max_name = 0;
	EXPECT_LT(Gen5Driver::GraphicsDriverGetResourceRegistrationMaxNameLength(nullptr), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverGetResourceRegistrationMaxNameLength(&max_name), 0);
	EXPECT_GT(max_name, 0u);
	uint32_t owner = 0;
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterOwner(nullptr, "runtime"), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterOwner(&owner, nullptr), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverRegisterOwner(&owner, "runtime"), 0);
	EXPECT_GT(owner, 0u);
	uint8_t  resource_memory[64] = {};
	uint32_t resource            = 0;
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterResource(nullptr, owner, resource_memory, sizeof(resource_memory), "buffer", 1, 0), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverRegisterResource(&resource, owner, nullptr, sizeof(resource_memory), "buffer", 1, 0), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverRegisterResource(&resource, owner, resource_memory, sizeof(resource_memory), "buffer", 1, 0), 0);
	EXPECT_GT(resource, 0u);
}

UT_END();
