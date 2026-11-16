#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderScalarLiveness.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "../../../emulator/src/Graphics/ShaderStorageAnalysis.h"
#include "Emulator/Log.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <vector>

UT_BEGIN(EmulatorShaderGetpc);

using namespace Libs::Graphics;

[[noreturn]] static void ParseSGetpcB64Probe()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	constexpr uint32_t kSop1Prefix = 0x17du << 23u;
	constexpr uint32_t kEndpgm    = 0xbf810000u;
	constexpr uint32_t kCodeEnd   = 0xbf9f0000u;
	const uint32_t     getpc_s8_s9 = kSop1Prefix | (8u << 16u) | (0x1fu << 8u);

	const std::array<uint32_t, 7> getpc_at_pc0 = {getpc_s8_s9, kEndpgm, kCodeEnd, kCodeEnd, kCodeEnd, kCodeEnd, kCodeEnd};
	ShaderCode code_at_pc0;
	code_at_pc0.SetType(ShaderType::Compute);
	ShaderParse(getpc_at_pc0.data(), static_cast<uint32_t>(sizeof(getpc_at_pc0)), &code_at_pc0);
	if (code_at_pc0.GetInstructions().Size() != 2u) { std::_Exit(2); }
	const auto& getpc0 = code_at_pc0.GetInstructions().At(0);
	if (getpc0.pc != 0u || getpc0.dst.type != ShaderOperandType::Sgpr || getpc0.dst.register_id != 8) { std::_Exit(3); }
	if (getpc0.dst.size != 2) { std::_Exit(4); }
	if (getpc0.src_num != 0) { std::_Exit(5); }
	if (getpc0.type != ShaderInstructionType::SGetpcB64) { std::_Exit(10); }
	if (getpc0.format != ShaderInstructionFormat::Sdst2) { std::_Exit(11); }
	if (getpc0.type == ShaderInstructionType::SBarrier || getpc0.format == ShaderInstructionFormat::Unknown)
	{
		std::_Exit(6);
	}
	const auto& endpgm0 = code_at_pc0.GetInstructions().At(1);
	if (endpgm0.pc != 4u || endpgm0.type != ShaderInstructionType::SEndpgm) { std::_Exit(7); }

	// 0xff in SOP1's unused ssrc0 must not consume the following ENDPGM word
	// as a literal; the code-end padding also keeps a bad read within this fixture.
	const std::array<uint32_t, 7> getpc_ignores_literal = {
	    getpc_s8_s9 | 0xffu, kEndpgm, kCodeEnd, kCodeEnd, kCodeEnd, kCodeEnd, kCodeEnd};
	ShaderCode code_ignoring_literal;
	code_ignoring_literal.SetType(ShaderType::Compute);
	ShaderParse(getpc_ignores_literal.data(), static_cast<uint32_t>(sizeof(getpc_ignores_literal)), &code_ignoring_literal);
	if (code_ignoring_literal.GetInstructions().Size() != 2u) { std::_Exit(8); }
	const auto& getpc_literal = code_ignoring_literal.GetInstructions().At(0);
	const auto& endpgm_literal = code_ignoring_literal.GetInstructions().At(1);
	if (getpc_literal.pc != 0u || getpc_literal.dst.size != 2 || getpc_literal.src_num != 0 || endpgm_literal.pc != 4u ||
	    endpgm_literal.type != ShaderInstructionType::SEndpgm)
	{
		std::_Exit(9);
	}

	std::_Exit(0);
}

TEST(EmulatorShaderGetpc, ParsesDestinationPairAndIgnoresUnusedSourceField)
{
	ASSERT_EXIT(ParseSGetpcB64Probe(), ::testing::ExitedWithCode(0), "");
}

[[noreturn]] static void ProgramBaseMetadataProbe()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderBindResources bind;
	bind.program_base_used = true;
	ShaderCalcBindingIndices(&bind);
	if (bind.program_base_offset_dw != 0u || bind.push_constant_size != 16u) { std::_Exit(12); }

	bind.direct_sgprs.sgprs_num = 32;
	for (int i = 0; i < bind.direct_sgprs.sgprs_num; ++i) { bind.direct_sgprs.start_register[i] = i; }
	ShaderCalcBindingIndices(&bind);
	if (bind.program_base_offset_dw != 32u || bind.push_constant_size != 144u || !bind.vsharp_uniform_buffer) { std::_Exit(13); }

	HW::ComputeShaderInfo cs_regs;
	cs_regs.cs_regs.data_addr = 0x100000u;
	cs_regs.cs_regs.chksum    = 0x1122334455667788ull;
	ShaderComputeInputInfo input;
	input.bind.program_base_used = true;
	input.bind.program_base      = 0x200000u;
	const auto original_id = ShaderGetIdCS(&cs_regs, &input);

	cs_regs.cs_regs.data_addr = 0x300000u;
	input.bind.program_base   = 0x400000u;
	const auto relocated_id = ShaderGetIdCS(&cs_regs, &input);
	if (original_id != relocated_id) { std::_Exit(14); }

	input.bind.program_base_used = false;
	const auto no_program_base_id = ShaderGetIdCS(&cs_regs, &input);
	if (original_id == no_program_base_id) { std::_Exit(15); }

	std::_Exit(0);
}

TEST(EmulatorShaderGetpc, ProgramBaseMetadataLayoutAndIdentity)
{
	ASSERT_EXIT(ProgramBaseMetadataProbe(), ::testing::ExitedWithCode(0), "");
}

[[noreturn]] static void LowerGetpcProbe()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	const uint32_t words[] = {0xbe881f00u, 0xbf810000u};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(words, &code);
	ShaderComputeInputInfo input {};
	input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
	input.bind.program_base_used = true;
	for (const int raw_sgprs: {0, 32})
	{
		input.bind.direct_sgprs.sgprs_num = raw_sgprs;
		for (int i = 0; i < raw_sgprs; ++i) { input.bind.direct_sgprs.start_register[i] = i; }
		ShaderCalcBindingIndices(&input.bind);
		const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		Vector<uint32_t> binary;
		String8 error;
		if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty()) { std::_Exit(16); }
	}
	std::_Exit(0);
}

TEST(EmulatorShaderGetpc, ValidatesRuntimeAddressLoadsInPushAndUniformLayouts)
{
	ASSERT_EXIT(LowerGetpcProbe(), ::testing::ExitedWithCode(0), "");
}

static String8 NggBufferLoadSource(int descriptor_register)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderInstruction getpc {};
	getpc.type   = ShaderInstructionType::SGetpcB64;
	getpc.format = ShaderInstructionFormat::Sdst2;
	getpc.dst    = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 2};
	ShaderInstruction load {};
	load.pc                  = 4;
	load.type                = ShaderInstructionType::BufferLoadDwordx4;
	load.format              = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
	load.dst                 = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 4};
	load.src[0]              = {.type = ShaderOperandType::Vgpr, .register_id = 5, .size = 1};
	load.src[1]              = {.type = ShaderOperandType::Sgpr, .register_id = descriptor_register, .size = 4};
	load.src[2].type         = ShaderOperandType::IntegerInlineConstant;
	load.src[2].constant.u   = 0;
	load.src_num             = 3;
	load.buffer_idxen        = true;
	load.buffer_flags        = 0; // Ordinary access: no LDS, TFE or unmodeled encoding bits.
	ShaderInstruction end {};
	end.pc     = 12;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	code.GetInstructions().Add(getpc);
	code.GetInstructions().Add(load);
	code.GetInstructions().Add(end);

	ShaderVertexInputInfo input {};
	input.gs_prolog                              = true;
	input.bind.program_base_used                 = true;
	input.bind.device_address_used               = true;
	input.bind.storage_buffers.buffers_num        = 1;
	input.bind.storage_buffers.start_register[0] = 0; // API zero is s[8:11], not s[0:3].
	input.bind.storage_buffers.usages[0]          = ShaderStorageUsage::ReadOnly;
	ShaderCalcBindingIndices(&input.bind);
	return SpirvGenerateSource(code, &input, nullptr, nullptr);
}

static bool ValidateNggBufferLoad(const String8& source, Vector<uint32_t>* binary, String8* error)
{
	class ValidationConfig final: public Config::ConfigSource
	{
	public:
		explicit ValidationConfig(bool enabled): m_enabled(enabled) {}
		bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
		int64_t GetInteger(const Core::String&) const override { return 0; }
		bool GetBool(const Core::String&) const override { return m_enabled; }
		Core::String GetString(const Core::String&) const override { return {}; }

	private:
		bool m_enabled;
	};
	const ValidationConfig restore(Config::ShaderValidationEnabled());
	Config::Load(ValidationConfig(true));
	const bool valid = ShaderToolchain::Run(source, binary, error);
	Config::Load(restore);
	return valid;
}

TEST(EmulatorShaderGetpc, NggInlineBufferDoesNotAliasApiRegisterZero)
{
	const auto source = NggBufferLoadSource(0);
	EXPECT_NE(source.FindIndex("%gbl_1_d0 = OpLoad %uint %s0"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%buf_addr_desc0_1_0"), Core::STRING8_INVALID_INDEX);
	Vector<uint32_t> binary;
	String8 error;
	EXPECT_TRUE(ValidateNggBufferLoad(source, &binary, &error)) << error.c_str();
	EXPECT_FALSE(binary.IsEmpty());
}

TEST(EmulatorShaderGetpc, NggBoundBufferUsesShiftedShaderRegister)
{
	const auto source = NggBufferLoadSource(8);
	EXPECT_EQ(source.FindIndex("%gbl_1_d0"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%buf_addr_desc0_1_0 = OpLoad %uint %s8"), Core::STRING8_INVALID_INDEX);
	Vector<uint32_t> binary;
	String8 error;
	EXPECT_TRUE(ValidateNggBufferLoad(source, &binary, &error)) << error.c_str();
	EXPECT_FALSE(binary.IsEmpty());
}

// A typed load through a V# built from s_getpc reads guest bytes; its base
// must never become a descriptor-array index. A V# that a mapped descriptor
// S_LOAD writes keeps the bound lowering.
static String8 GetpcTypedLoadSource(bool mapped_sload)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderInstruction getpc {};
	getpc.type   = ShaderInstructionType::SGetpcB64;
	getpc.format = ShaderInstructionFormat::Sdst2;
	getpc.dst    = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 2};
	ShaderInstruction load {};
	load.pc                   = 4;
	load.type                 = ShaderInstructionType::TBufferLoadFormatX;
	load.format               = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxenFloat1;
	load.dst                  = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	load.src[0]               = {.type = ShaderOperandType::Vgpr, .register_id = 5, .size = 1};
	load.src[1]               = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 4};
	load.src[2].type          = ShaderOperandType::IntegerInlineConstant;
	load.src[2].constant.u    = 0;
	load.src_num              = 3;
	load.buffer_idxen         = true;
	load.buffer_flags         = 0;
	load.mtbuf_format         = 22; // 32_FLOAT
	load.mtbuf_format_is_gen5 = true;
	load.mtbuf_components     = 1;
	ShaderInstruction end {};
	end.pc     = 12;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(getpc);
	code.GetInstructions().Add(load);
	code.GetInstructions().Add(end);

	ShaderComputeInputInfo input {};
	input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
	input.bind.program_base_used                 = true;
	input.bind.device_address_used               = true;
	input.bind.storage_buffers.buffers_num        = 1;
	input.bind.storage_buffers.start_register[0] = 8;
	input.bind.storage_buffers.usages[0]          = ShaderStorageUsage::ReadOnly;
	if (mapped_sload)
	{
		ShaderDynamicSLoadMapping record {};
		record.destination_register = 0;
		record.dword_count          = 4;
		record.last_consumer_pc     = load.pc;
		input.bind.dynamic_sloads.records.Add(record);
	}
	ShaderCalcBindingIndices(&input.bind);
	return SpirvGenerateSource(code, nullptr, nullptr, &input);
}

TEST(EmulatorShaderGetpc, RuntimeTypedBufferLoadReadsGuestAddress)
{
	const auto source = GetpcTypedLoadSource(false);
	EXPECT_NE(source.FindIndex("%gbl_1_d0 = OpLoad %uint %s0"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%gen5_tbuffer_load_1"), Core::STRING8_INVALID_INDEX);
	Vector<uint32_t> binary;
	String8 error;
	EXPECT_TRUE(ValidateNggBufferLoad(source, &binary, &error)) << error.c_str();
	EXPECT_FALSE(binary.IsEmpty());
}

TEST(EmulatorShaderGetpc, MappedTypedBufferLoadKeepsBoundLowering)
{
	const auto source = GetpcTypedLoadSource(true);
	EXPECT_EQ(source.FindIndex("%gbl_1_d0"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%gen5_tbuffer_load_1"), Core::STRING8_INVALID_INDEX);
}

// BUFFER_LOAD_FORMAT decodes the V#'s format and DST_SEL both through a V#
// built from s_getpc (guest addressed) and through a bound V#.
TEST(EmulatorShaderGetpc, FormatLoadsDecodeDescriptorFormatOnBothPaths)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	ShaderInstruction getpc {};
	getpc.type   = ShaderInstructionType::SGetpcB64;
	getpc.format = ShaderInstructionFormat::Sdst2;
	getpc.dst    = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 2};
	ShaderInstruction runtime {};
	runtime.pc                = 4;
	runtime.type              = ShaderInstructionType::BufferLoadFormatXyzw;
	runtime.format            = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
	runtime.dst               = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 4};
	runtime.src[0]            = {.type = ShaderOperandType::Vgpr, .register_id = 5, .size = 1};
	runtime.src[1]            = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 4};
	runtime.src[2].type       = ShaderOperandType::IntegerInlineConstant;
	runtime.src[2].constant.u = 0;
	runtime.src_num           = 3;
	runtime.buffer_idxen      = true;
	runtime.buffer_flags      = 0;
	ShaderInstruction bound   = runtime;
	bound.pc                  = 12;
	bound.type                = ShaderInstructionType::BufferLoadFormatXy;
	bound.format              = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
	bound.dst                 = {.type = ShaderOperandType::Vgpr, .register_id = 6, .size = 2};
	bound.src[1]              = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 4};
	ShaderInstruction end {};
	end.pc     = 20;
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(getpc);
	code.GetInstructions().Add(runtime);
	code.GetInstructions().Add(bound);
	code.GetInstructions().Add(end);

	ShaderComputeInputInfo input {};
	input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
	input.bind.program_base_used                 = true;
	input.bind.device_address_used               = true;
	input.bind.storage_buffers.buffers_num        = 1;
	input.bind.storage_buffers.start_register[0] = 8;
	input.bind.storage_buffers.usages[0]          = ShaderStorageUsage::ReadOnly;
	ShaderCalcBindingIndices(&input.bind);
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	EXPECT_NE(source.FindIndex("%gtl_1_value = OpFunctionCall %v4uint %gen5_buffer_format_decode"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("%gen5_mubuf_value_2_0 = OpFunctionCall %v4uint %gen5_buffer_format_decode"),
	          Core::STRING8_INVALID_INDEX);
	Vector<uint32_t> binary;
	String8 error;
	EXPECT_TRUE(ValidateNggBufferLoad(source, &binary, &error)) << error.c_str();
	EXPECT_FALSE(binary.IsEmpty());
}

// A V# read only by format loads binds in any decodable format; a format store keeps the 32-bit rule,
// since BUFFER_STORE_FORMAT encodes 32-bit elements only.
TEST(EmulatorShaderGetpc, FormatLoadOnlyStorageAcceptsEveryDecodableFormat)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	ShaderInstruction load {};
	load.type                 = ShaderInstructionType::BufferLoadFormatX;
	load.format               = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
	load.dst                  = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	load.src[0]               = {.type = ShaderOperandType::Vgpr, .register_id = 5, .size = 1};
	load.src[1]               = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 4};
	load.src[2].type          = ShaderOperandType::IntegerInlineConstant;
	load.src_num              = 3;
	load.buffer_idxen         = true;
	ShaderInstruction store   = load;
	store.pc                  = 8;
	store.type                = ShaderInstructionType::BufferStoreFormatX;
	ShaderInstruction end {};
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	const auto analyze = [&](bool with_store)
	{
		ShaderCode code;
		code.SetType(ShaderType::Compute);
		code.GetInstructions().Add(load);
		if (with_store) { code.GetInstructions().Add(store); }
		end.pc = with_store ? 16u : 8u;
		code.GetInstructions().Add(end);
		return AnalyzeShaderStorageUse(code, 8);
	};
	const auto loads = analyze(false);
	EXPECT_EQ(loads.access, ShaderStorageAccess::Typed);
	EXPECT_TRUE(loads.format_loads_only);
	const auto written = analyze(true);
	EXPECT_EQ(written.access, ShaderStorageAccess::Typed);
	EXPECT_FALSE(written.format_loads_only);

	ShaderBufferResource r16 {};
	r16.fields[1] = 2u << 16u;                    // stride 2
	r16.fields[2] = 64u;
	r16.fields[3] = (11u << 12u) | DstSel(4, 0, 0, 0); // R16_UINT
	EXPECT_TRUE(ShaderGen5FormatLoadDescriptorSupported(r16));
	EXPECT_FALSE(ShaderGen5StorageDescriptorSupported(r16, ShaderStorageAccess::Typed));
	r16.fields[3] = DstSel(4, 0, 0, 0); // INVALID
	EXPECT_FALSE(ShaderGen5FormatLoadDescriptorSupported(r16));
}

// The front loads an extended V# into s[0:3], parks it in v1 lanes 0..3 while
// s0 holds other data, restores it and reads it with one format load.
TEST(EmulatorShaderGetpc, ExtendedDescriptorFollowedThroughLaneSpillToFormatLoad)
{
	constexpr int kBase    = 8;
	constexpr int kPointer = 24; // API register of the EUD pointer, s[32:33] in the front
	const auto sgpr = [](int reg, int size = 1) { return ShaderOperand {.type = ShaderOperandType::Sgpr, .register_id = reg, .size = size}; };
	const auto vgpr = [](int reg, int size = 1) { return ShaderOperand {.type = ShaderOperandType::Vgpr, .register_id = reg, .size = size}; };
	const auto lane = [](int value)
	{
		ShaderOperand operand {.type = ShaderOperandType::IntegerInlineConstant, .size = 1};
		operand.constant.i = value;
		return operand;
	};
	const auto eud_load = [&](ShaderInstructionType type, int dst, int dwords, uint32_t byte_offset)
	{
		ShaderInstruction inst {};
		inst.type   = type;
		inst.dst    = sgpr(dst, dwords);
		inst.src[0] = sgpr(kPointer + kBase, 2);
		inst.src[1] = {.type = ShaderOperandType::LiteralConstant, .size = 1};
		inst.src[1].constant.u = byte_offset;
		inst.src_num           = 2;
		return inst;
	};
	const auto unary = [](ShaderInstructionType type, const ShaderOperand& dst, const ShaderOperand& src0, const ShaderOperand& src1)
	{
		ShaderInstruction inst {};
		inst.type    = type;
		inst.dst     = dst;
		inst.src[0]  = src0;
		inst.src[1]  = src1;
		inst.src_num = 2;
		return inst;
	};
	const auto format_access = [&](ShaderInstructionType type)
	{
		ShaderInstruction inst {};
		inst.type         = type;
		inst.dst          = vgpr(6, 2);
		inst.src[0]       = vgpr(0);
		inst.src[1]       = sgpr(0, 4);
		inst.src[2].type  = ShaderOperandType::IntegerInlineConstant;
		inst.src_num      = 3;
		inst.buffer_idxen = true;
		return inst;
	};
	const auto spill   = [&](int word) { return unary(ShaderInstructionType::VWritelaneB32, vgpr(1), sgpr(word), lane(word)); };
	const auto restore = [&](int word) { return unary(ShaderInstructionType::VReadlaneB32, sgpr(word), vgpr(1), lane(word)); };
	const auto program = [&]()
	{
		std::vector<ShaderInstruction> body {eud_load(ShaderInstructionType::SLoadDwordx4, 0, 4, 96)};
		for (int word = 0; word < 4; ++word) { body.push_back(spill(word)); }
		for (int word = 0; word < 4; ++word) { body.push_back(unary(ShaderInstructionType::SMovB32, sgpr(word), sgpr(40 + word), {})); }
		for (int word = 0; word < 4; ++word) { body.push_back(restore(word)); }
		body.push_back(format_access(ShaderInstructionType::BufferLoadFormatXy));
		body.push_back(unary(ShaderInstructionType::VMovB32, vgpr(1), lane(0), {}));
		body.push_back(eud_load(ShaderInstructionType::SLoadDwordx4, 0, 4, 0));
		return body;
	};
	ShaderBindResources bind {};
	bind.extended.used           = true;
	bind.extended.start_register = kPointer;
	const auto proves = [&](std::vector<ShaderInstruction> body)
	{
		ShaderCode code;
		code.SetType(ShaderType::Vertex);
		ShaderInstruction end {};
		end.type = ShaderInstructionType::SEndpgm;
		body.push_back(end);
		for (size_t index = 0; index < body.size(); ++index)
		{
			body[index].pc = static_cast<uint32_t>(index * 8u);
			code.GetInstructions().Add(body[index]);
		}
		return ShaderEudDescriptorReadOnlyByFormatLoads(code, bind, 24, kBase, *ShaderScalarFlowOf(code));
	};
	const auto edited = [&](size_t index, const ShaderInstruction& inst)
	{
		auto body = program();
		body.insert(body.begin() + static_cast<std::ptrdiff_t>(index), inst);
		return body;
	};

	EXPECT_TRUE(proves(program()));
	// Instructions 0 load, 1-4 spill, 5-8 clobber s[0:3], 9-12 restore, 13 format load.
	constexpr size_t kRestore = 9;
	constexpr size_t kConsumer = 13;
	// No load reaches the V# of the next table row.
	EXPECT_FALSE([&]()
	{
		ShaderCode code;
		for (const auto& inst: program()) { code.GetInstructions().Add(inst); }
		return ShaderEudDescriptorReadOnlyByFormatLoads(code, bind, 28, kBase, *ShaderScalarFlowOf(code));
	}());
	// Lane 2 refilled with other data before the readback.
	EXPECT_FALSE(proves(edited(kRestore, unary(ShaderInstructionType::VWritelaneB32, vgpr(1), sgpr(9), lane(2)))));
	// The spill VGPR is rewritten before the readback.
	EXPECT_FALSE(proves(edited(kRestore, unary(ShaderInstructionType::VMovB32, vgpr(1), lane(0), {}))));
	// The parked words are read as vector data.
	EXPECT_FALSE(proves(edited(kRestore, unary(ShaderInstructionType::VMovB32, vgpr(2), vgpr(1), {}))));
	// A branch while the V# is still live.
	ShaderInstruction branch {};
	branch.type    = ShaderInstructionType::SBranch;
	branch.src_num = 1;
	branch.src[0]  = lane(0);
	EXPECT_FALSE(proves(edited(kRestore, branch)));
	// The program ends with the V# still in s[0:3] and the lanes: no reader is left.
	auto ends_live = program();
	ends_live.resize(kConsumer + 1);
	EXPECT_TRUE(proves(ends_live));
	// Only three words come back to the quad the format load reads.
	auto partial = program();
	partial.erase(partial.begin() + static_cast<std::ptrdiff_t>(kRestore + 3));
	EXPECT_FALSE(proves(partial));
	// One word restored while another SGPR still holds it.
	EXPECT_FALSE(proves(edited(5, unary(ShaderInstructionType::VReadlaneB32, sgpr(7), vgpr(1), lane(0)))));
	// A store through the V#.
	auto stored       = program();
	stored[kConsumer] = format_access(ShaderInstructionType::BufferStoreFormatX);
	EXPECT_FALSE(proves(stored));
	// A raw load through the V#.
	auto raw       = program();
	raw[kConsumer] = format_access(ShaderInstructionType::BufferLoadDwordx2);
	EXPECT_FALSE(proves(raw));
	// A load that covers only part of the V#.
	EXPECT_FALSE(proves(edited(1, eud_load(ShaderInstructionType::SLoadDwordx8, 8, 8, 80))));
	// The pointer is copied, so other loads may reach the V#.
	EXPECT_FALSE(proves(edited(1, unary(ShaderInstructionType::SMovB64, sgpr(50, 2), sgpr(kPointer + kBase, 2), {}))));
	// A spilled V# whose table words no resource maps is read at runtime.
	bind.extended.read_at_runtime = true;
	EXPECT_FALSE(proves(program()));
}

TEST(EmulatorShaderGetpc, OnlyAttributeTableFormatLoadsAreVertexFetches)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	const auto sgpr     = [](int reg, int size) { return ShaderOperand {.type = ShaderOperandType::Sgpr, .register_id = reg, .size = size}; };
	const auto vgpr     = [](int reg, int size) { return ShaderOperand {.type = ShaderOperandType::Vgpr, .register_id = reg, .size = size}; };
	const auto constant = [](int value)
	{
		ShaderOperand op {};
		op.type       = ShaderOperandType::IntegerInlineConstant;
		op.constant.i = value;
		return op;
	};
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	const auto add = [&](ShaderInstructionType type, ShaderOperand dst, std::initializer_list<ShaderOperand> src)
	{
		ShaderInstruction inst {};
		inst.pc      = static_cast<uint32_t>(code.GetInstructions().Size()) * 8u;
		inst.type    = type;
		inst.dst     = dst;
		inst.src_num = static_cast<int>(src.size());
		int i        = 0;
		for (const auto& op: src)
		{
			inst.src[i++] = op;
		}
		inst.buffer_idxen = true;
		code.GetInstructions().Add(inst);
		return inst.pc;
	};
	// Attribute table at user data 22 (s30), buffer table at user data 20 (s28).
	add(ShaderInstructionType::SLoadDwordx2, sgpr(48, 2), {sgpr(30, 2), constant(0)});
	add(ShaderInstructionType::SLshlB32, sgpr(50, 1), {sgpr(48, 1), constant(4)});
	add(ShaderInstructionType::SLoadDwordx4, sgpr(20, 4), {sgpr(28, 2), sgpr(50, 1)});
	add(ShaderInstructionType::SBfeU32, sgpr(51, 1), {sgpr(48, 1), constant(14)});
	add(ShaderInstructionType::VCndmaskB32, vgpr(0, 1), {vgpr(8, 1), vgpr(5, 1), sgpr(2, 2)});
	const uint32_t fetch = add(ShaderInstructionType::BufferLoadFormatXyz, vgpr(14, 3), {vgpr(0, 1), sgpr(20, 4), sgpr(51, 1)});
	// A user-data V# indexed by the vertex id is a buffer load; it also overwrites the index.
	add(ShaderInstructionType::BufferLoadFormatXyzw, vgpr(0, 4), {vgpr(0, 1), sgpr(16, 4), constant(0)});
	add(ShaderInstructionType::BufferLoadFormatX, vgpr(4, 1), {vgpr(0, 1), sgpr(20, 4), sgpr(51, 1)});
	add(ShaderInstructionType::SEndpgm, {}, {});

	const auto scan = ShaderScanVertexStreamAccesses(code, 22, 20);
	ASSERT_EQ(scan.fetch_candidates.Size(), 1u);
	EXPECT_EQ(scan.fetch_candidates.At(0).first, fetch);
	EXPECT_EQ(scan.fetch_candidates.At(0).second, 0);
	EXPECT_TRUE(scan.unconditional_reader);
}

TEST(EmulatorShaderGetpc, AttributeScanDemandsOnlyTheBitsTheProgramCanObserve)
{
	const auto sgpr     = [](int reg, int size) { return ShaderOperand {.type = ShaderOperandType::Sgpr, .register_id = reg, .size = size}; };
	const auto constant = [](uint32_t value)
	{
		ShaderOperand op {};
		op.type       = ShaderOperandType::LiteralConstant;
		op.constant.u = value;
		return op;
	};
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	const auto add = [&](ShaderInstructionType type, ShaderOperand dst, std::initializer_list<ShaderOperand> src)
	{
		ShaderInstruction inst {};
		inst.pc      = static_cast<uint32_t>(code.GetInstructions().Size()) * 8u;
		inst.type    = type;
		inst.dst     = dst;
		inst.src_num = static_cast<int>(src.size());
		int i        = 0;
		for (const auto& op: src)
		{
			inst.src[i++] = op;
		}
		code.GetInstructions().Add(inst);
	};
	const auto scc = ShaderOperand {.type = ShaderOperandType::Scc};
	// Dwords 0-3 of the table at user data 22 (s30); dword 5 is a hole.
	add(ShaderInstructionType::SLoadDwordx4, sgpr(48, 4), {sgpr(30, 2), constant(0)});
	add(ShaderInstructionType::SBfeU32, sgpr(52, 1), {sgpr(48, 1), constant((12u << 16u) | 14u)});
	add(ShaderInstructionType::SLshlB32, sgpr(53, 1), {sgpr(49, 1), constant(4u)});
	add(ShaderInstructionType::SAndB32, sgpr(54, 1), {sgpr(53, 1), constant(0x1f0u)});
	add(ShaderInstructionType::SLshlB32, sgpr(55, 1), {sgpr(50, 1), constant(4u)});
	add(ShaderInstructionType::SCselectB32, sgpr(56, 1), {sgpr(55, 1), scc});
	add(ShaderInstructionType::SMovB32, sgpr(57, 1), {sgpr(51, 1)});
	add(ShaderInstructionType::VCndmaskB32, ShaderOperand {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1}, {sgpr(57, 1)});
	add(ShaderInstructionType::SEndpgm, {}, {});

	const auto scan = ShaderScanVertexStreamAccesses(code, 22, 20);
	EXPECT_EQ(scan.attrib_demanded_bits[0], 0x03ffc000u);
	EXPECT_EQ(scan.attrib_demanded_bits[1], 0x0000001fu);
	// SCC of the shift reaches a select, and it depends on every surviving bit.
	EXPECT_EQ(scan.attrib_demanded_bits[2], ~0u);
	EXPECT_EQ(scan.attrib_demanded_bits[3], ~0u);
	EXPECT_EQ(scan.attrib_demanded_bits[5], 0u);
}

TEST(EmulatorShaderGetpc, BufferNamespaceClassificationMatchesLoweringAndIgnoresStreamSentinels)
{
	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num = 2;
	bind.storage_buffers.start_register[0] = 0;
	bind.storage_buffers.start_register[1] = -1;
	for (const int base: {0, 8})
	{
		for (const int reg: {0, 7, 8, 12})
		{
			const ShaderOperand resource {.type = ShaderOperandType::Sgpr, .register_id = reg, .size = 4};
			const bool bound = reg == base;
			EXPECT_EQ(ShaderStorageBufferResourceIsBound(bind, resource, base), bound);
			ShaderInstruction load {};
			load.type = ShaderInstructionType::BufferLoadDwordx4;
			load.src_num = 2;
			load.src[1] = resource;
			ShaderCode code;
			code.GetInstructions().Add(load);
			EXPECT_EQ(ShaderHasUnboundBufferLoad(code, bind, base), !bound);
			ShaderInstruction scalar_load {};
			scalar_load.type    = ShaderInstructionType::SBufferLoadDword;
			scalar_load.dst     = {.type = ShaderOperandType::Sgpr, .register_id = 20, .size = 1};
			scalar_load.src_num = 2;
			scalar_load.src[0]  = resource;
			scalar_load.src[1]  = {.type = ShaderOperandType::LiteralConstant, .size = 1};
			EXPECT_EQ(ShaderScalarBufferUsesRuntimeDescriptor(bind, scalar_load, base), !bound);
		}
	}
	const ShaderOperand invalid {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 4};
	EXPECT_FALSE(ShaderStorageBufferResourceIsBound(bind, invalid));
	const ShaderOperand negative {.type = ShaderOperandType::Sgpr, .register_id = -1, .size = 4};
	EXPECT_FALSE(ShaderStorageBufferResourceIsBound(bind, negative));
}

// A bound V# register that S_GETPC redefines holds a guest base afterwards; the
// binding's descriptor slot index is valid only while it holds user data.
TEST(EmulatorShaderGetpc, RedefinedBoundDescriptorReadsRuntimeDescriptor)
{
	const ShaderOperand descriptor {.type = ShaderOperandType::Sgpr, .register_id = 12, .size = 4};
	const auto scalar_load = [&](uint32_t pc, int dst)
	{
		ShaderInstruction inst {};
		inst.pc      = pc;
		inst.type    = ShaderInstructionType::SBufferLoadDword;
		inst.dst     = {.type = ShaderOperandType::Sgpr, .register_id = dst, .size = 1};
		inst.src_num = 2;
		inst.src[0]  = descriptor;
		inst.src[1]  = {.type = ShaderOperandType::LiteralConstant, .size = 1};
		return inst;
	};
	ShaderInstruction getpc {};
	getpc.pc   = 8;
	getpc.type = ShaderInstructionType::SGetpcB64;
	getpc.dst  = {.type = ShaderOperandType::Sgpr, .register_id = 12, .size = 2};
	ShaderInstruction vector_load {};
	vector_load.pc      = 16;
	vector_load.type    = ShaderInstructionType::BufferLoadDword;
	vector_load.dst     = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	vector_load.src_num = 3;
	vector_load.src[0]  = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	vector_load.src[1]  = descriptor;
	ShaderInstruction end {};
	end.pc   = 24;
	end.type = ShaderInstructionType::SEndpgm;

	ShaderCode code;
	code.GetInstructions().Add(scalar_load(0, 20));
	code.GetInstructions().Add(getpc);
	code.GetInstructions().Add(scalar_load(12, 21));
	code.GetInstructions().Add(vector_load);
	code.GetInstructions().Add(end);

	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num       = 1;
	bind.storage_buffers.start_register[0] = 12;
	ShaderCollectRedefinedBufferDescriptors(code, &bind, 0, *ShaderScalarFlowOf(code));

	ASSERT_EQ(bind.redefined_descriptor_consumers.Size(), 2u);
	EXPECT_EQ(bind.redefined_descriptor_consumers.At(0), 12u);
	EXPECT_EQ(bind.redefined_descriptor_consumers.At(1), 16u);
	const auto& instructions = code.GetInstructions();
	EXPECT_FALSE(ShaderScalarBufferUsesRuntimeDescriptor(bind, instructions.At(0)));
	EXPECT_TRUE(ShaderScalarBufferUsesRuntimeDescriptor(bind, instructions.At(2)));
	EXPECT_TRUE(ShaderVectorBufferUsesRuntimeDescriptor(bind, instructions.At(3)));
}

// A stage without runtime V# lowering (the vertex front) keeps a reloaded
// consumer on the slot path. That is sound only when every path reloads the
// register; a path that keeps the bound row refuses the buffer instead.
static ShaderBindResources SlotOnlyBind(bool reload_on_one_branch)
{
	const ShaderOperand descriptor {.type = ShaderOperandType::Sgpr, .register_id = 12, .size = 4};
	ShaderInstruction   scalar_load {};
	scalar_load.type    = ShaderInstructionType::SBufferLoadDword;
	scalar_load.dst     = {.type = ShaderOperandType::Sgpr, .register_id = 20, .size = 1};
	scalar_load.src_num = 2;
	scalar_load.src[0]  = descriptor;
	scalar_load.src[1]  = {.type = ShaderOperandType::LiteralConstant, .size = 1};
	ShaderInstruction branch {};
	branch.type               = ShaderInstructionType::SCbranchScc0;
	branch.format             = ShaderInstructionFormat::Label;
	branch.src[0].type        = ShaderOperandType::LiteralConstant;
	branch.src[0].constant.i  = 4; // skips the reload
	branch.src_num            = 1;
	ShaderInstruction reload {};
	reload.type = ShaderInstructionType::SGetpcB64;
	reload.dst  = {.type = ShaderOperandType::Sgpr, .register_id = 12, .size = 2};
	ShaderInstruction vector_load {};
	vector_load.type    = ShaderInstructionType::BufferLoadDword;
	vector_load.dst     = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	vector_load.src_num = 3;
	vector_load.src[0]  = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	vector_load.src[1]  = descriptor;
	ShaderInstruction end {};
	end.type = ShaderInstructionType::SEndpgm;

	ShaderCode code;
	for (auto inst: reload_on_one_branch ? std::vector {scalar_load, branch, reload, vector_load, end}
	                                     : std::vector {scalar_load, reload, vector_load, end})
	{
		inst.pc = code.GetInstructions().Size() * 4u;
		code.GetInstructions().Add(inst);
	}
	if (reload_on_one_branch)
	{
		code.GetLabels().Add(ShaderLabel(12u, 4u));
		code.GetIndirectLabels().Add(ShaderLabel(8u, 4u));
	}
	ShaderBindResources bind {};
	bind.storage_buffers.buffers_num       = 1;
	bind.storage_buffers.start_register[0] = 12;
	ShaderCodeResourceMemo memo;
	ShaderCollectRedefinedBufferDescriptors(code, &bind, 0, *ShaderScalarFlowOf(code), &memo, false);
	EXPECT_TRUE(bind.descriptor_flow_proven);
	EXPECT_EQ(bind.redefined_descriptor_consumers.Size(), 0u);
	return bind;
}

TEST(EmulatorShaderGetpc, SlotOnlyConsumerNeedsTheRegisterReloadedOnEveryPath)
{
	const auto straight = SlotOnlyBind(false);
	ASSERT_EQ(straight.slot_only_consumers.Size(), 1u);
	EXPECT_EQ(straight.slot_only_consumers.At(0), 8u);
	EXPECT_FALSE(straight.storage_buffers.row_redefined[0]);

	const auto branched = SlotOnlyBind(true);
	EXPECT_EQ(branched.slot_only_consumers.Size(), 0u);
	EXPECT_TRUE(branched.storage_buffers.row_redefined[0]);
}

static String8 VertexScalarProbeSource(bool enabled, uint32_t loads)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderVertexInputInfo input {};
	input.gs_prolog = true;
	input.clip_probe.enabled = enabled;
	input.clip_probe_descriptor_set = 1;
	input.bind.storage_buffers.buffers_num = 1;
	input.bind.storage_buffers.start_register[0] = 0;
	input.bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadOnly;
	ShaderCalcBindingIndices(&input.bind);
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	for (uint32_t index = 0; index < loads; ++index)
	{
		ShaderInstruction load {};
		load.pc = index * 8u;
		load.type = index == 0u ? ShaderInstructionType::SBufferLoadDwordx16 : ShaderInstructionType::SBufferLoadDwordx4;
		load.format = index == 0u ? ShaderInstructionFormat::Sdst16SvSoffset : ShaderInstructionFormat::Sdst4SvSoffset;
		load.dst = {.type = ShaderOperandType::Sgpr, .register_id = index == 0u ? 32 : 8, .size = index == 0u ? 16 : 4};
		load.src[0] = {.type = ShaderOperandType::Sgpr, .register_id = 8, .size = 4};
		load.src[1].type = ShaderOperandType::IntegerInlineConstant;
		load.src[1].constant.i = 128;
		load.src_num = 2;
		code.GetInstructions().Add(load);
	}
	ShaderInstruction end {};
	end.pc = loads * 8u;
	end.type = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);
	return SpirvGenerateSource(code, &input, nullptr, nullptr);
}

TEST(EmulatorShaderGetpc, VertexScalarProbePreservesLoadAndSnapshotsBeforeOverlap)
{
	const auto source = VertexScalarProbeSource(true, 2u);
	const auto descriptor = source.FindIndex("%vs_sbuffer_desc_1_0 = OpLoad %uint %s8");
	const auto original = source.FindIndex("%t110_1 = OpFunctionCall %void %sbuffer_load_dword_4");
	const auto result = source.FindIndex("%vs_sbuffer_value_1_0 = OpLoad %uint %s8");
	EXPECT_NE(descriptor, Core::STRING8_INVALID_INDEX);
	EXPECT_NE(original, Core::STRING8_INVALID_INDEX);
	EXPECT_NE(result, Core::STRING8_INVALID_INDEX);
	EXPECT_LT(descriptor, original);
	EXPECT_LT(original, result);
	EXPECT_NE(source.FindIndex("%vs_sbuffer_value_0_15 = OpLoad %uint %s47"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(source.FindIndex("%vs_sbuffer_value_1_4"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(source.FindIndex("OpAtomicCompareExchange %uint %vs_sbuffer_claim_ptr_1"), Core::STRING8_INVALID_INDEX);
	Vector<uint32_t> binary;
	String8 error;
	EXPECT_TRUE(ValidateNggBufferLoad(source, &binary, &error)) << error.c_str();
}

TEST(EmulatorShaderGetpc, VertexScalarProbeIsDisabledNormallyAndCapsSites)
{
	const auto ordinary = VertexScalarProbeSource(false, 2u);
	EXPECT_EQ(ordinary.FindIndex("%vs_sbuffer_desc_"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(ordinary.FindIndex("%vertex_clip_probe"), Core::STRING8_INVALID_INDEX);
	const auto diagnostic = VertexScalarProbeSource(true, 9u);
	EXPECT_NE(diagnostic.FindIndex("%vs_sbuffer_desc_7_0"), Core::STRING8_INVALID_INDEX);
	EXPECT_EQ(diagnostic.FindIndex("%vs_sbuffer_desc_8_0"), Core::STRING8_INVALID_INDEX);
	EXPECT_NE(diagnostic.FindIndex("%t110_8 = OpFunctionCall %void %sbuffer_load_dword_4"), Core::STRING8_INVALID_INDEX);
	Vector<uint32_t> binary;
	String8 error;
	EXPECT_TRUE(ValidateNggBufferLoad(diagnostic, &binary, &error)) << error.c_str();
}

[[noreturn]] static void RejectGetpcProbe(int destination, bool metadata, bool graphics)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	const uint32_t words[] = {0xbe801f00u | (static_cast<uint32_t>(destination) << 16u), 0xbf810000u};
	ShaderCode code;
	code.SetType(graphics ? ShaderType::Vertex : ShaderType::Compute);
	ShaderParse(words, &code);
	ShaderComputeInputInfo input {};
	input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
	input.bind.program_base_used = metadata;
	ShaderCalcBindingIndices(&input.bind);
	ShaderVertexInputInfo vertex {};
	vertex.bind = input.bind;
	(void)SpirvGenerateSource(code, graphics ? &vertex : nullptr, nullptr, graphics ? nullptr : &input);
	std::_Exit(0);
}

TEST(EmulatorShaderGetpc, RejectsUnsupportedAddressAndDestinationRepresentations)
{
#if defined(_WIN32)
	constexpr int rejected_exit = 321;
#else
	constexpr int rejected_exit = 65;
#endif
	ASSERT_EXIT(RejectGetpcProbe(103, true, false), ::testing::ExitedWithCode(rejected_exit), "");
	ASSERT_EXIT(RejectGetpcProbe(8, false, false), ::testing::ExitedWithCode(rejected_exit), "");
	// Since 8d08fb38 a vertex program with a bound program base resolves GETPC
	// like compute; only a missing base or an invalid destination is rejected.
	ASSERT_EXIT(RejectGetpcProbe(8, true, true), ::testing::ExitedWithCode(0), "");
	ASSERT_EXIT(RejectGetpcProbe(8, false, true), ::testing::ExitedWithCode(rejected_exit), "");
}

UT_END();
