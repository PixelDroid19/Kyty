#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

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

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

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

TEST(EmulatorGraphicsPackets, ParsesBufferStoreFormatXyzw)
{
	const uint32_t shader[] = {0xe01c2000u, 0x80010004u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& instruction = code.GetInstructions().At(0);
	EXPECT_EQ(instruction.type, ShaderInstructionType::BufferStoreFormatXyzw);
	EXPECT_EQ(instruction.format, ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen);
	EXPECT_EQ(instruction.dst.type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.dst.register_id, 0);
	EXPECT_EQ(instruction.dst.size, 4u);
	EXPECT_EQ(instruction.src[0].type, ShaderOperandType::Vgpr);
	EXPECT_EQ(instruction.src[0].register_id, 4);
	EXPECT_EQ(instruction.src[1].type, ShaderOperandType::Sgpr);
	EXPECT_EQ(instruction.src[1].register_id, 4);
	EXPECT_EQ(instruction.src[1].size, 4u);
	EXPECT_EQ(instruction.src[2].type, ShaderOperandType::IntegerInlineConstant);
	EXPECT_EQ(instruction.src[2].constant.u, 0u);
}

TEST(EmulatorGraphicsPackets, ClassifiesDirectBufferStoreAsReadWrite)
{
	const uint32_t shader[] = {0xe01c2000u, 0x80000004u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	EXPECT_EQ(ShaderGetDirectStorageUsage(code, 0), ShaderStorageUsage::ReadWrite);
	EXPECT_EQ(ShaderGetDirectStorageUsage(code, 4), ShaderStorageUsage::Unknown);
}

TEST(EmulatorGraphicsPackets, ReusesFourComponentBufferFunctionType)
{
	const uint32_t shader[] = {0xd7460003u, 0x040a0300u, 0xd7460003u, 0x040a0300u, 0xe01c2000u, 0x80000004u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ShaderComputeInputInfo input {};
	input.threads_num[0]                         = 1;
	input.threads_num[1]                         = 1;
	input.threads_num[2]                         = 1;
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 0;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadWrite;
	input.bind.push_constant_size                = 16;
	const auto source                            = SpirvGenerateSource(code, nullptr, nullptr, &input);

	EXPECT_NE(source.FindIndex("%function_buffer_load_store_float4 = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%function_buffer_load_float4 = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%function_buffer_store_float4 = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%function_tbuffer_load_store_format_xyzw = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%function_tbuffer_load_format_xyzw = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%function_tbuffer_store_format_xyzw = OpTypeFunction"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpSGreaterThanEqual %bool %tbuf_s_f_xyzw_11 %int_75"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpSLessThanEqual %bool %tbuf_s_f_xyzw_11 %int_77"), Core::STRING8_INVALID_INDEX);
}

TEST(EmulatorGraphicsPackets, ClassifiesGen5FourComponent32BitBufferFormats)
{
	EXPECT_FALSE(ShaderIsGen5FourComponent32BitBufferFormat(74));
	EXPECT_TRUE(ShaderIsGen5FourComponent32BitBufferFormat(75));
	EXPECT_TRUE(ShaderIsGen5FourComponent32BitBufferFormat(76));
	EXPECT_TRUE(ShaderIsGen5FourComponent32BitBufferFormat(77));
	EXPECT_FALSE(ShaderIsGen5FourComponent32BitBufferFormat(78));
}

TEST(EmulatorGraphicsPackets, AllowsRegionScalarsOnlyInsideSrtRange)
{
	ShaderUserData user_data {};
	user_data.srt_size_dw = 8;

	EXPECT_TRUE(ShaderCanBindDirectSgpr(&user_data, 4, HW::UserSgprType::Region));
	EXPECT_FALSE(ShaderCanBindDirectSgpr(&user_data, 8, HW::UserSgprType::Region));
	EXPECT_TRUE(ShaderCanBindDirectSgpr(&user_data, 8, HW::UserSgprType::Unknown));
}

TEST(EmulatorGraphicsPackets, AcceptsFullUserSgprWriteWindow)
{
	// Observed frontier: PS user-SGPR SET_SH_REG with reg_num == 16 at slot 0.
	// The previous check (reg_num >= 16) rejected a full-window write.
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(0, 16));
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(0, 1));
	EXPECT_TRUE(HW::UserSgprInfo::WriteRangeValid(15, 1));
	EXPECT_FALSE(HW::UserSgprInfo::WriteRangeValid(0, 0));
	EXPECT_FALSE(HW::UserSgprInfo::WriteRangeValid(0, 17));
	EXPECT_FALSE(HW::UserSgprInfo::WriteRangeValid(1, 16));
	EXPECT_FALSE(HW::UserSgprInfo::WriteRangeValid(16, 1));
}

TEST(EmulatorGraphicsPackets, ReportsType3Pm4PacketSizeInDwords)
{
	// WaitFlipDone header observed at IxYiarKlXxM frontier: 0xC0051018 → len 7.
	const uint32_t wait_flip = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE);
	EXPECT_EQ(wait_flip, 0xC0051018u);
	EXPECT_EQ(Gen5::GraphicsGetDataPacketSizeDw(&wait_flip), 7u);

	const uint32_t release_mem = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_RELEASE_MEM);
	EXPECT_EQ(release_mem, 0xC0051060u);
	EXPECT_EQ(Gen5::GraphicsGetDataPacketSizeDw(&release_mem), 7u);

	const uint32_t wait_mem64 = KYTY_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64);
	EXPECT_EQ(wait_mem64, 0xC0071058u);
	EXPECT_EQ(Gen5::GraphicsGetDataPacketSizeDw(&wait_mem64), 9u);

	EXPECT_EQ(Gen5::GraphicsGetDataPacketSizeDw(nullptr), 0u);
}

TEST(EmulatorGraphicsPackets, AllocatesCommandBufferDwords)
{
	// Layout matches Gen5::CommandBuffer in Graphics.cpp (non-virtual methods).
	// Observed NID LtTouSCZjHM: (CommandBuffer*, num_dw=10) → dword*.
	struct AlignasCommandBuffer
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

	uint32_t            storage[64] = {};
	AlignasCommandBuffer cb {};
	cb.bottom      = storage;
	cb.top         = storage + 64;
	cb.cursor_up   = storage;
	cb.cursor_down = storage + 64;

	EXPECT_EQ(Gen5::GraphicsCbAllocateDwords(nullptr, 10), nullptr);
	EXPECT_EQ(Gen5::GraphicsCbAllocateDwords(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 0), nullptr);

	uint32_t* first = Gen5::GraphicsCbAllocateDwords(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 10);
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(first, storage);
	EXPECT_EQ(cb.cursor_up, storage + 10);

	uint32_t* second = Gen5::GraphicsCbAllocateDwords(reinterpret_cast<Gen5::CommandBuffer*>(&cb), 4);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(second, storage + 10);
	EXPECT_EQ(cb.cursor_up, storage + 14);
}

// GFX linear surfaces pad rows to 256 bytes. For RGBA8 (format 56) that is a
// 64-texel pitch alignment — width alone is wrong for non-pow2 sizes.
TEST(EmulatorGraphicsPackets, AlignsGen5LinearTexturePitchTo256ByteRows)
{
	// 348-wide RGBA8 logo: 348*4 = 1392 -> next 256-byte boundary is 1536 bytes = 384 texels.
	EXPECT_EQ(ShaderGen5LinearTexturePitch(348, 56), 384u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(1, 56), 64u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(64, 56), 64u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(65, 56), 128u);
	EXPECT_EQ(ShaderGen5LinearTexturePitch(1280, 56), 1280u);
}

// Gen5 SMEM: SLoadDwordx2 s[4:5], s[0:1], 0; SLoadDword s6, s[0:1], 8; s_endpgm
// s[0:1] is the extended user-data pointer; offsets 0 and 8 map into push-constant vsharp.
TEST(EmulatorGraphicsPackets, MaterializesExtendedSLoadDwordAndX2)
{
	const uint32_t shader[] = {0xf4040100u, 0xfa000000u, 0xf4000180u, 0xfa000008u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::SLoadDwordx2);
	EXPECT_EQ(code.GetInstructions().At(1).type, ShaderInstructionType::SLoadDword);

	ShaderComputeInputInfo input {};
	input.threads_num[0]                         = 1;
	input.threads_num[1]                         = 1;
	input.threads_num[2]                         = 1;
	input.bind.extended.used                     = true;
	input.bind.extended.start_register           = 0;
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 16;
	input.bind.storage_buffers.extended[0]       = true;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::Constant;
	input.bind.push_constant_size                = 16;

	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);

	// Destinations must be real stores, not comment-only no-ops.
	EXPECT_NE(source.FindIndex("OpStore %s4"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s5"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s6"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpAccessChain %_ptr_PushConstant_uint %vsharp"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpLoad %uint"), Core::STRING8_INVALID_INDEX);
}

// Gen5 SMEM from fetch_attrib_reg (+gs_prolog shift): loads attribute-table dwords into SGPRs.
TEST(EmulatorGraphicsPackets, MaterializesFetchAttribSLoadDwordAndX2)
{
	// SLoadDwordx2 s[48:49], s[14:15], 0; SLoadDword s50, s[14:15], 8; s_endpgm
	const uint32_t shader[] = {0xf4040c07u, 0xfa000000u, 0xf4000c87u, 0xfa000008u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	ShaderParse(shader, &code);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::SLoadDwordx2);
	EXPECT_EQ(code.GetInstructions().At(0).src[0].register_id, 14);
	EXPECT_EQ(code.GetInstructions().At(1).type, ShaderInstructionType::SLoadDword);
	EXPECT_EQ(code.GetInstructions().At(1).dst.register_id, 50);

	ShaderVertexInputInfo input {};
	input.fetch_embedded        = true;
	input.gs_prolog             = true;
	input.fetch_attrib_reg      = 6; // physical s14 with +8 shift
	input.fetch_buffer_reg      = 4; // physical s12
	// Values >= 256 so SPIR-V emits hex constants (see Spirv::AddConstant).
	input.fetch_attrib_data[0]  = 0xa1b2c301u;
	input.fetch_attrib_data[1]  = 0xa1b2c302u;
	input.fetch_attrib_data[2]  = 0xa1b2c303u;
	input.fetch_attrib_data_num = 3;

	const auto source = SpirvGenerateSource(code, &input, nullptr, nullptr);

	EXPECT_NE(source.FindIndex("OpStore %s48"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s49"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpStore %s50"), Core::STRING8_INVALID_INDEX);
	// Sanitized table values must appear as SPIR-V uint constants used by the stores.
	EXPECT_NE(source.FindIndex("0xa1b2c301"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("0xa1b2c302"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("0xa1b2c303"), Core::STRING8_INVALID_INDEX);
}

// Gen5 SMEM: s_buffer_load_dwordx8 s[32:39], s[8:11], 0; s_nop; s_endpgm
// Encoding matches the SMRD opcode table (0x0B); recompiler already supports Sdst8SvSoffset.
TEST(EmulatorGraphicsPackets, ParsesGen5SBufferLoadDwordx8)
{
	// word0: enc=0x3d, op=0x0B, sdst=32, sbase=4 → s[8:11]; word1: null soffset, imm 0
	// s_nop (sopp) then s_endpgm so SEndpgm recompiler's index>=2 path is satisfied.
	const uint32_t shader[] = {0xf42c0804u, 0xfa000000u, 0xbf800000u, 0xbf810000u};

	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	ShaderParse(shader, &code);

	ASSERT_GE(code.GetInstructions().Size(), 2u);
	EXPECT_EQ(code.GetInstructions().At(0).type, ShaderInstructionType::SBufferLoadDwordx8);
	EXPECT_EQ(code.GetInstructions().At(0).format, ShaderInstructionFormat::Sdst8SvSoffset);
	EXPECT_EQ(code.GetInstructions().At(0).dst.register_id, 32);
	EXPECT_EQ(code.GetInstructions().At(0).dst.size, 8);
	EXPECT_EQ(code.GetInstructions().At(0).src[0].register_id, 8);
	EXPECT_EQ(code.GetInstructions().At(0).src[0].size, 4);

	ShaderVertexInputInfo input {};
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 8;
	input.bind.storage_buffers.extended[0]       = false;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::Constant;
	input.bind.storage_buffers.binding_index     = 0;
	input.bind.push_constant_size                = 16;
	input.bind.descriptor_set_slot               = 0;

	const auto source = SpirvGenerateSource(code, &input, nullptr, nullptr);

	// Helper and call site must exist; stores happen inside the helper via pointer params.
	EXPECT_NE(source.FindIndex("%sbuffer_load_dword_8"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpFunctionCall %void %sbuffer_load_dword_8"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%s32"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%s39"), Core::STRING8_INVALID_INDEX);
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
	EXPECT_LT(Gen5Driver::GraphicsDriverUnregisterResource(0), 0);
	EXPECT_EQ(Gen5Driver::GraphicsDriverUnregisterResource(resource), 0);
	EXPECT_LT(Gen5Driver::GraphicsDriverUnregisterResource(resource), 0);
}

TEST(EmulatorGraphicsPackets, MapsPixelInputsFromVertexOutputSuperset)
{
	ShaderSemantic outputs[2] {};
	outputs[0].semantic         = 15;
	outputs[0].hardware_mapping = 5;
	outputs[1].semantic         = 16;
	outputs[1].hardware_mapping = 7;

	ShaderSemantic inputs[1] {};
	inputs[0].semantic       = 15;
	inputs[0].is_flat_shaded = 1;

	ShaderRegister regs[32] {};
	ASSERT_TRUE(Gen5::GraphicsBuildInterpolantMapping(regs, outputs, 2, inputs, 1));
	EXPECT_EQ(regs[0].offset, Pm4::SPI_PS_INPUT_CNTL_0);
	EXPECT_EQ(regs[0].value, 5u | 0x400u);
	EXPECT_EQ(regs[1].offset, Pm4::SPI_PS_INPUT_CNTL_0 + 1);
	EXPECT_EQ(regs[1].value, 0u);

	inputs[0].semantic = 17;
	inputs[0].default_value = 0;
	ASSERT_TRUE(Gen5::GraphicsBuildInterpolantMapping(regs, outputs, 2, inputs, 1));
	EXPECT_EQ(regs[0].value, 0x20u);

	inputs[0].default_value = 3;
	ASSERT_TRUE(Gen5::GraphicsBuildInterpolantMapping(regs, outputs, 2, inputs, 1));
	EXPECT_EQ(regs[0].value, 0x320u);

	ASSERT_TRUE(Gen5::GraphicsBuildInterpolantMapping(regs, outputs, 2, nullptr, 0));
	EXPECT_EQ(regs[0].value, 5u);
	EXPECT_EQ(regs[1].value, 7u);
}

UT_END();
