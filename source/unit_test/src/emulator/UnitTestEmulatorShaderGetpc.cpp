#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdlib>

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
		}
	}
	const ShaderOperand invalid {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 4};
	EXPECT_FALSE(ShaderStorageBufferResourceIsBound(bind, invalid));
	const ShaderOperand negative {.type = ShaderOperandType::Sgpr, .register_id = -1, .size = 4};
	EXPECT_FALSE(ShaderStorageBufferResourceIsBound(bind, negative));
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
