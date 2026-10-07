#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include <cstdlib>

UT_BEGIN(EmulatorShaderSopkDecode);

using namespace Libs::Graphics;

// Encodings follow the RDNA2 ISA (SOPK: 0xB in bits 31:28, opcode in 27:23, SDST
// in 22:16, SIMM16 in 15:0; SOPC: 0x17E in bits 31:23, opcode in 22:16). The
// shader decoders used to turn several of these into an SBarrier and continue,
// which silently dropped real ALU work or invented behavior for undefined
// opcodes.

namespace {

#if defined(_WIN32)
constexpr int kRejectedExit = 321;
#else
constexpr int kRejectedExit = 65;
#endif

constexpr uint32_t kSEndpgm = 0xbf810000u;

constexpr uint32_t Sopk(uint32_t opcode, uint32_t sdst, uint32_t simm16)
{
	return (0xBu << 28u) | (opcode << 23u) | (sdst << 16u) | (simm16 & 0xffffu);
}

constexpr uint32_t Sopc(uint32_t opcode, uint32_t ssrc0, uint32_t ssrc1)
{
	return 0xBF000000u | (opcode << 16u) | (ssrc1 << 8u) | ssrc0;
}

void InitializeDecodeTest(bool next_gen)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(next_gen);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

ShaderCode Parse(const uint32_t* words)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(words, &code);
	return code;
}

String8 Generate(const ShaderCode& code)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 1;
	input.threads_num[1] = 1;
	input.threads_num[2] = 1;
	return SpirvGenerateSource(code, nullptr, nullptr, &input);
}

bool Has(const String8& source, const char* text)
{
	return source.FindIndex(text) != Core::STRING8_INVALID_INDEX;
}

[[noreturn]] void ParseAndExit(uint32_t first)
{
	InitializeDecodeTest(true);
	const uint32_t words[] = {first, kSEndpgm};
	ShaderCode     code    = Parse(words);
	std::_Exit(code.GetInstructions().IsEmpty() ? 3 : 0);
}

// gtest has no "any abnormal termination" predicate; a lwe fetch stops the process
// through the not-implemented handler whose exit style differs by platform.
struct NotCleanExit
{
	bool operator()(int status) const { return !::testing::ExitedWithCode(0)(status); }
};

} // namespace

TEST(EmulatorShaderSopkDecode, AddkDecodesAsSignedAddWithOverflowScc)
{
	// s_addk_i32 s5, -16: D += signext(SIMM16), SCC = signed overflow, i.e.
	// s_add_i32 s5, s5, -16 (ISA section 12.2, opcode 15).
	InitializeDecodeTest(true);
	const uint32_t words[] = {Sopk(15, 5, 0xfff0u), kSEndpgm};
	const auto     code    = Parse(words);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& inst = code.GetInstructions().At(0);
	EXPECT_EQ(inst.type, ShaderInstructionType::SAddI32);
	EXPECT_EQ(inst.format, ShaderInstructionFormat::SVdstSVsrc0SVsrc1);
	EXPECT_EQ(inst.dst.type, ShaderOperandType::Sgpr);
	EXPECT_EQ(inst.dst.register_id, 5);
	ASSERT_EQ(inst.src_num, 2);
	EXPECT_EQ(inst.src[0].type, ShaderOperandType::Sgpr);
	EXPECT_EQ(inst.src[0].register_id, 5);
	EXPECT_EQ(inst.src[1].type, ShaderOperandType::IntegerInlineConstant);
	EXPECT_EQ(inst.src[1].constant.i, -16);

	const auto source = Generate(code);
	EXPECT_TRUE(Has(source, "OpIAdd %int"));
	EXPECT_TRUE(Has(source, "OpStore %s5"));
	EXPECT_TRUE(Has(source, "OpStore %scc"));
}

TEST(EmulatorShaderSopkDecode, CmovkDecodesAsConditionalMoveKeepingOldValue)
{
	// s_cmovk_i32 s7, 0x123: if (SCC) D = signext(SIMM16) (ISA opcode 2).
	InitializeDecodeTest(true);
	const uint32_t words[] = {Sopk(2, 7, 0x0123u), kSEndpgm};
	const auto     code    = Parse(words);

	ASSERT_EQ(code.GetInstructions().Size(), 2u);
	const auto& inst = code.GetInstructions().At(0);
	EXPECT_EQ(inst.type, ShaderInstructionType::SCmovB32);
	EXPECT_EQ(inst.format, ShaderInstructionFormat::SVdstSVsrc0SVsrc1);
	EXPECT_EQ(inst.dst.register_id, 7);
	ASSERT_EQ(inst.src_num, 2);
	EXPECT_EQ(inst.src[0].type, ShaderOperandType::IntegerInlineConstant);
	EXPECT_EQ(inst.src[0].constant.i, 0x123);
	EXPECT_EQ(inst.src[1].type, ShaderOperandType::Sgpr);
	EXPECT_EQ(inst.src[1].register_id, 7);

	const auto source = Generate(code);
	EXPECT_TRUE(Has(source, "OpLoad %uint %scc"));
	EXPECT_TRUE(Has(source, "OpSelect %uint"));
	EXPECT_TRUE(Has(source, "OpStore %s7"));
}

TEST(EmulatorShaderSopkDecode, ControlsStillDecodeDefinedOpcodes)
{
	ASSERT_EXIT(ParseAndExit(Sopk(16, 5, 3u)), ::testing::ExitedWithCode(0), ""); // s_mulk_i32
	ASSERT_EXIT(ParseAndExit(Sopc(0x12, 128u, 128u)), ::testing::ExitedWithCode(0), ""); // s_cmp_eq_u64
}

TEST(EmulatorShaderSopkDecode, Rdna2RejectsSopkOpcodesItDoesNotDefine)
{
	// The RDNA2 SOPK table skips opcodes 17 and 20.
	ASSERT_EXIT(ParseAndExit(Sopk(17, 5, 0u)), ::testing::ExitedWithCode(kRejectedExit), "");
	ASSERT_EXIT(ParseAndExit(Sopk(20, 5, 0u)), ::testing::ExitedWithCode(kRejectedExit), "");
}

TEST(EmulatorShaderSopkDecode, Rdna2RejectsHardwareRegisterAccess)
{
	// s_getreg_b32 (18), s_setreg_b32 (19), s_setreg_imm32_b32 (21) reach MODE, STATUS
	// and HW_ID, which the translator does not model; a barrier stand-in would
	// silently change rounding or denormal behavior.
	ASSERT_EXIT(ParseAndExit(Sopk(18, 5, 0u)), ::testing::ExitedWithCode(kRejectedExit), "");
	ASSERT_EXIT(ParseAndExit(Sopk(19, 5, 0u)), ::testing::ExitedWithCode(kRejectedExit), "");
	ASSERT_EXIT(ParseAndExit(Sopk(21, 5, 0u)), ::testing::ExitedWithCode(kRejectedExit), "");
}

TEST(EmulatorShaderSopkDecode, Rdna2RejectsSopcOpcodeSixteen)
{
	// The RDNA2 SOPC table skips from opcode 15 to 18 and the ISA has no VSKIP.
	ASSERT_EXIT(ParseAndExit(Sopc(0x10, 128u, 128u)), ::testing::ExitedWithCode(kRejectedExit), "");
}

namespace {

[[noreturn]] void ParseImageFetch(bool lod_warning_enable)
{
	InitializeDecodeTest(true);
	const uint32_t word0   = (0x3cu << 26u) | (0x27u << 18u) | (0xfu << 8u) | (lod_warning_enable ? (1u << 17u) : 0u);
	const uint32_t words[] = {word0, 0u, kSEndpgm};
	ShaderCode     code    = Parse(words);
	std::_Exit(code.GetInstructions().Size() == 2u ? 0 : 3);
}

} // namespace

TEST(EmulatorShaderSopkDecode, ImageFetchesRejectLodWarningEnable)
{
	// LWE makes a fetch return LOD_CLAMPED, a sticky MODE status bit (ISA 3.5). The
	// parser rejects it, which is why image emitters never lower that result.
	ASSERT_EXIT(ParseImageFetch(false), ::testing::ExitedWithCode(0), "");
	ASSERT_EXIT(ParseImageFetch(true), NotCleanExit(), "");
}

UT_END();
