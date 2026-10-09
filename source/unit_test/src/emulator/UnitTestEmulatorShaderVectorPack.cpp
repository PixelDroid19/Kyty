#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderArithmeticTestInterpreter.h"

#include <cstdio>
#include <cstdlib>

UT_BEGIN(EmulatorShaderVectorPack);

using namespace Libs::Graphics;

#if defined(_WIN32)
constexpr int kRejectedExit = 321;
#else
constexpr int kRejectedExit = 65;
#endif

namespace {

class ValidationConfig final: public Config::ConfigSource
{
public:
	bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
	int64_t GetInteger(const Core::String&) const override { return 0; }
	bool GetBool(const Core::String&) const override { return true; }
	Core::String GetString(const Core::String&) const override { return {}; }
};

void InitializeNextGenShaderTest()
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ValidationConfig validation;
	Config::Load(validation);
}

ShaderOperand Vgpr(int register_id, int size = 1)
{
	ShaderOperand operand {};
	operand.type        = ShaderOperandType::Vgpr;
	operand.register_id = register_id;
	operand.size        = size;
	return operand;
}

ShaderOperand Immediate(ShaderOperandType type, uint32_t value)
{
	ShaderOperand operand {};
	operand.type       = type;
	operand.constant.u = value;
	return operand;
}

ShaderInstruction MakeIntegerPack(ShaderInstructionType type, uint32_t pc, int dst, const ShaderOperand& src0,
                                  const ShaderOperand& src1)
{
	ShaderInstruction pack {};
	pack.pc            = pc;
	pack.type          = type;
	pack.format        = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	pack.dst           = Vgpr(dst);
	pack.src[0]        = src0;
	pack.src[1]        = src1;
	pack.src_num       = 2;
	return pack;
}

ShaderInstruction MakeEnd(uint32_t pc)
{
	ShaderInstruction end {};
	end.pc            = pc;
	end.type          = ShaderInstructionType::SEndpgm;
	end.format        = ShaderInstructionFormat::Empty;
	end.sopp_opcode   = 1u;
	end.raw_word      = 0xbf810000u;
	end.src[0]        = Immediate(ShaderOperandType::LiteralConstant, 0u);
	return end;
}

ShaderComputeInputInfo SingleLaneComputeInput()
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
	input.wave_layout   = {ShaderComputeWaveStrategy::Native, {1, 1, 1}, {1, 1, 1}, 32, 32, 1, 1, 0};
	return input;
}

Core::String8 GeneratePackSource(const ShaderInstruction& pack)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(pack);
	code.GetInstructions().Add(MakeEnd(8u));
	const auto input = SingleLaneComputeInput();
	return SpirvGenerateSource(code, nullptr, nullptr, &input);
}

enum class ModifiedPackCase
{
	SourceModifier,
	WideSource,
	Swizzle,
	Dpp,
	OpSel,
};

void RunModifiedPackCase(ShaderInstructionType type, ModifiedPackCase test_case)
{
	InitializeNextGenShaderTest();
	ShaderInstruction pack = MakeIntegerPack(type, 0u, 7, Vgpr(5), Vgpr(6));
	switch (test_case)
	{
		case ModifiedPackCase::SourceModifier: pack.src[0].negate = true; break;
		case ModifiedPackCase::WideSource: pack.src[0].size = 2; break;
		case ModifiedPackCase::Swizzle: pack.src[0].swizzle = 5u; break;
		case ModifiedPackCase::Dpp:
			pack.src[0].dpp           = true;
			pack.src[0].dpp_ctrl      = 0x1bu;
			pack.src[0].dpp_row_mask  = 0x0fu;
			pack.src[0].dpp_bank_mask = 0x0fu;
			break;
		case ModifiedPackCase::OpSel: pack.vop3_op_sel = 1u; break;
	}
	(void)GeneratePackSource(pack);
	std::_Exit(0);
}

} // namespace

TEST(EmulatorShaderVectorPack, DecodesAndLowersGen5Unorm16Pair)
{
	// Synthetic VOP3A fields from the RDNA2 ISA: two VGPR float sources,
	// one VGPR packed destination, and no modifiers. The first source aliases
	// the destination to exercise read-before-write behavior.
	constexpr uint32_t word0 = (0x35u << 26u) | (0x369u << 16u) | 5u;
	constexpr uint32_t word1 = ((256u + 6u) << 9u) | (256u + 5u);
	constexpr uint32_t words[] = {word0, word1, 0xbf810000u};

	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    class ValidationConfig final: public Config::ConfigSource
		    {
		    public:
			    bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t GetInteger(const Core::String&) const override { return 0; }
			    bool GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation;
		    Config::Load(validation);
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().Size() != 2u) { std::_Exit(2); }
		    const auto& pack = code.GetInstructions().At(0);
		    if (pack.type != ShaderInstructionType::VCvtPknormU16F32 ||
		        pack.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || pack.src_num != 2u ||
		        pack.dst.type != ShaderOperandType::Vgpr || pack.dst.register_id != 5 ||
		        pack.src[0].type != ShaderOperandType::Vgpr || pack.src[0].register_id != 5 ||
		        pack.src[1].type != ShaderOperandType::Vgpr || pack.src[1].register_id != 6 ||
		        pack.vop3_op_sel != 0u || pack.vop3_omod != 0u || pack.dst.clamp)
		    {
			    std::_Exit(3);
		    }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    const auto first_load = source.FindIndex("%t0_0 = OpLoad %float %v5");
		    const auto second_load = source.FindIndex("%t1_0 = OpLoad %float %v6");
		    const auto pack_op = source.FindIndex("OpExtInst %uint %GLSL_std_450 PackUnorm2x16");
		    const auto bitcast = source.FindIndex("OpBitcast %float %packed_bits_0");
		    const auto store = source.FindIndex("OpStore %v5 %tval_0");
		    if (first_load == Core::STRING8_INVALID_INDEX || second_load == Core::STRING8_INVALID_INDEX ||
		        pack_op == Core::STRING8_INVALID_INDEX || bitcast == Core::STRING8_INVALID_INDEX ||
		        store == Core::STRING8_INVALID_INDEX || first_load >= pack_op || second_load >= pack_op || pack_op >= store)
		    {
			    std::_Exit(4);
		    }
		    Vector<uint32_t> binary;
		    Core::String8 error;
		    std::_Exit(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty() ? 0 : 5);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderVectorPack, DecodesAndLowersGen5Snorm16Pair)
{
	// A separate synthetic VOP3A encoding exercises the signed normalized
	// conversion with a tied VGPR source and an inline zero, without modifiers.
	constexpr uint32_t word0 = (0x35u << 26u) | (0x368u << 16u) | 7u;
	constexpr uint32_t word1 = (128u << 9u) | (256u + 7u);
	constexpr uint32_t words[] = {word0, word1, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    class ValidationConfig final: public Config::ConfigSource
		    {
		    public:
			    bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
			    int64_t GetInteger(const Core::String&) const override { return 0; }
			    bool GetBool(const Core::String&) const override { return true; }
			    Core::String GetString(const Core::String&) const override { return {}; }
		    } validation;
		    Config::Load(validation);
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().Size() != 2u) { std::_Exit(2); }
		    const auto& pack = code.GetInstructions().At(0);
		    if (pack.type != ShaderInstructionType::VCvtPknormI16F32 ||
		        pack.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || pack.src_num != 2u ||
		        pack.dst.type != ShaderOperandType::Vgpr || pack.dst.register_id != 7 ||
		        pack.src[0].type != ShaderOperandType::Vgpr || pack.src[0].register_id != 7 ||
		        pack.src[1].type != ShaderOperandType::IntegerInlineConstant || pack.src[1].constant.i != 0 ||
		        pack.vop3_op_sel != 0u || pack.vop3_omod != 0u || pack.dst.clamp)
		    {
			    std::_Exit(3);
		    }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    const auto pair = source.FindIndex("OpCompositeConstruct %v2float %t0_0 %t1_0");
		    const auto pack_op = source.FindIndex("OpExtInst %uint %GLSL_std_450 PackSnorm2x16");
		    const auto bitcast = source.FindIndex("OpBitcast %float %packed_bits_0");
		    const auto store = source.FindIndex("OpStore %v7 %tval_0");
		    if (pair == Core::STRING8_INVALID_INDEX || pack_op == Core::STRING8_INVALID_INDEX ||
		        bitcast == Core::STRING8_INVALID_INDEX || store == Core::STRING8_INVALID_INDEX ||
		        pair >= pack_op || pack_op >= bitcast || bitcast >= store)
		    {
			    std::_Exit(4);
		    }
		    Vector<uint32_t> binary;
		    Core::String8 error;
		    std::_Exit(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty() ? 0 : 5);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderVectorPack, DecodesGen5UnsignedU32PackOpcode)
{
	// Synthetic Gen5 VOP3A with two VGPR sources and one VGPR destination.
	// Keep this parser-only so an unsupported opcode is distinct from lowering
	// or SPIR-V validation failures in the emitter test below.
	constexpr uint32_t word0 = (0x35u << 26u) | (0x36au << 16u) | 7u;
	constexpr uint32_t word1 = ((256u + 6u) << 9u) | (256u + 5u);
	constexpr uint32_t words[] = {word0, word1, 0xbf810000u};

	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().Size() != 2u) { std::_Exit(2); }
		    const auto& pack = code.GetInstructions().At(0);
		    if (pack.type != ShaderInstructionType::VCvtPkU16U32 ||
		        pack.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || pack.src_num != 2u ||
		        pack.dst.type != ShaderOperandType::Vgpr || pack.dst.register_id != 7 ||
		        pack.src[0].type != ShaderOperandType::Vgpr || pack.src[0].register_id != 5 ||
		        pack.src[1].type != ShaderOperandType::Vgpr || pack.src[1].register_id != 6 ||
		        pack.vop3_op_sel != 0u || pack.vop3_omod != 0u || pack.dst.clamp)
		    {
			    std::_Exit(3);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderVectorPack, SaturatesGen5IntegerPairsToSixteenBits)
{
	ASSERT_EXIT(
	    ([] {
		    InitializeNextGenShaderTest();
		    struct Pair { ShaderInstructionType type; uint32_t src0; uint32_t src1; uint32_t expected; const char* label; };
		    // u32_to_u16 and i32_to_i16 clamp to the destination range; values
		    // already in range pass through unchanged.
		    const Pair pairs[] = {
		        {ShaderInstructionType::VCvtPkU16U32, 0x1234u, 0x5678u, 0x56781234u, "unsigned in range"},
		        {ShaderInstructionType::VCvtPkU16U32, 0xf0000000u, 0x1cu, 0x001cffffu, "unsigned low lane clamps"},
		        {ShaderInstructionType::VCvtPkU16U32, 0xffffu, 0x10000u, 0xffffffffu, "unsigned high lane clamps"},
		        {ShaderInstructionType::VCvtPkU16U32, 0u, UINT32_MAX, 0xffff0000u, "unsigned maximum source"},
		        {ShaderInstructionType::VCvtPkI16I32, 0x1234u, UINT32_MAX, 0xffff1234u, "signed in range"},
		        {ShaderInstructionType::VCvtPkI16I32, 0xf0000000u, 0x1cu, 0x001c8000u, "signed low lane clamps"},
		        {ShaderInstructionType::VCvtPkI16I32, 0x8000u, 0xffff7fffu, 0x80007fffu, "signed one past each bound"},
		        {ShaderInstructionType::VCvtPkI16I32, 0x80000000u, 0x7fffffffu, 0x7fff8000u, "signed extreme sources"},
		    };
		    for (const auto& pair: pairs)
		    {
			    const auto source = GeneratePackSource(MakeIntegerPack(pair.type, 0u, 7, Vgpr(5), Vgpr(6)));
			    if (source.IsEmpty()) { std::_Exit(2); }

			    Vector<uint32_t> binary;
			    Core::String8 error;
			    if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
			    {
				    std::fprintf(stderr, "%s\n", error.c_str());
				    std::_Exit(3);
			    }

			    const ShaderArithmeticTest::Program program(source.c_str());
			    ShaderArithmeticTest::Equal(
			        program.Run({{"%v5", pair.src0}, {"%v6", pair.src1}, {"%v7", 0xa5a5a5a5u}, {"%exec_lo", 1u},
			                     {"%exec_hi", 0u}, {"%exec_lane_lo", 1u}, {"%exec_lane_hi", 0u}, {"%scc", 0u}}, "%v7"),
			        pair.expected, pair.label);
		    }
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderVectorPack, SaturatesGen5UnsignedImmediatePair)
{
	ASSERT_EXIT(
	    ([] {
		    InitializeNextGenShaderTest();
		    // A VOP3 instruction has one shared literal extension, so the high
		    // lane uses an inline constant.
		    const auto source = GeneratePackSource(MakeIntegerPack(ShaderInstructionType::VCvtPkU16U32, 0u, 7,
		                                                           Immediate(ShaderOperandType::LiteralConstant, 0x10000u),
		                                                           Immediate(ShaderOperandType::IntegerInlineConstant, 42u)));
		    if (source.IsEmpty()) { std::_Exit(2); }
		    Vector<uint32_t> binary;
		    Core::String8 error;
		    if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty()) { std::_Exit(3); }
		    const ShaderArithmeticTest::Program program(source.c_str());
		    ShaderArithmeticTest::Equal(
		        program.Run({{"%v7", 0xa5a5a5a5u}, {"%exec_lo", 1u}, {"%exec_hi", 0u}, {"%exec_lane_lo", 1u},
		                     {"%exec_lane_hi", 0u}, {"%scc", 0u}}, "%v7"),
		        0x002affffu, "literal low lane clamps");
		    std::_Exit(0);
	    }()),
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorShaderVectorPack, RejectsModifiedIntegerPackOperands)
{
	const ShaderInstructionType types[] = {ShaderInstructionType::VCvtPkU16U32, ShaderInstructionType::VCvtPkI16I32};
	const ModifiedPackCase cases[] = {ModifiedPackCase::SourceModifier, ModifiedPackCase::WideSource,
	                                  ModifiedPackCase::Swizzle, ModifiedPackCase::Dpp, ModifiedPackCase::OpSel};
	for (const auto type: types)
	{
		for (const auto test_case: cases)
		{
			SCOPED_TRACE(static_cast<int>(test_case));
			ASSERT_EXIT(RunModifiedPackCase(type, test_case), ::testing::ExitedWithCode(kRejectedExit), "");
		}
	}
}

UT_END();
