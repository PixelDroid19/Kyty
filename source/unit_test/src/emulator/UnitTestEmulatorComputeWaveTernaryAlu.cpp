#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>

UT_BEGIN(EmulatorComputeWaveTernaryAlu);

using namespace Libs::Graphics;

// v_add3_u32 v43, vcc_lo, vcc_hi, v0, checked with the LLVM gfx1030 disassembler.
static constexpr uint32_t kAdd3Word0 = 0xd76d002bu;
static constexpr uint32_t kAdd3Word1 = 0x0400d66au;
static constexpr uint32_t kEnd       = 0xbf810000u;

static void InitializeConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

static ShaderComputeInputInfo PairedInput()
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num = 1;
	input.wave_layout    = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0};
	return input;
}

TEST(EmulatorComputeWaveTernaryAlu, AdmitsPlainAdd3WithScalarWordAndVectorSources)
{
	// vcc_lo/vcc_hi/v0, s20/vcc_hi/v0, and s20 plus a VOP3 literal plus v0.
	struct Case
	{
		uint32_t word1;
		uint32_t literal;
	};
	for (const auto& test: {Case {kAdd3Word1, 0u}, Case {0x0400d614u, 0u}, Case {0x0401fe14u, 0x12345678u}})
	{
		const uint32_t words[] = {kAdd3Word0, test.word1, test.literal != 0u ? test.literal : kEnd, kEnd};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    const uint32_t bytes = test.literal != 0u ? sizeof(words) : sizeof(words) - sizeof(uint32_t);
			    if (!ShaderTryParseBounded(words, bytes, &code) || code.GetInstructions().Size() != 2u ||
			        code.GetInstructions().At(0).type != ShaderInstructionType::VAdd3U32)
			    {
				    std::_Exit(2);
			    }
			    const bool banked = ShaderClassifyComputeWaveInstruction(code.GetInstructions().At(0)) ==
			                        ShaderComputeWaveInstructionKind::BankedAlu;
			    std::_Exit(banked && ShaderAnalyzeComputeWaveCode(code, PairedInput()).supported ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

// The banked ternary path lowers only the plain tuple. Encoding modifiers leave
// it for the generic per-lane lowering; EXEC as a data source has no lowering at all.
TEST(EmulatorComputeWaveTernaryAlu, BankedPathClaimsNoModifiedTuple)
{
	struct Case
	{
		uint32_t word0;
		uint32_t word1;
	};
	const Case cases[] = {
	    {kAdd3Word0 | (1u << 15u), kAdd3Word1}, // clamp
	    {kAdd3Word0 | (1u << 11u), kAdd3Word1}, // op_sel
	    {kAdd3Word0 | (1u << 8u), kAdd3Word1},  // abs src0
	    {kAdd3Word0, kAdd3Word1 | (1u << 31u)}, // neg src2
	    {kAdd3Word0, kAdd3Word1 | (1u << 27u)}, // omod
	    {kAdd3Word0, 0x0400d67cu},              // m0 as data
	};
	for (const auto& test: cases)
	{
		const uint32_t words[] = {test.word0, test.word1, kEnd};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().At(0).type != ShaderInstructionType::VAdd3U32)
			    {
				    std::_Exit(2);
			    }
			    const auto kind = ShaderClassifyComputeWaveInstruction(code.GetInstructions().At(0));
			    std::_Exit(kind != ShaderComputeWaveInstructionKind::BankedAlu ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveTernaryAlu, RejectsExecAsADataSource)
{
	const uint32_t words[] = {kAdd3Word0, 0x0400d67eu, kEnd}; // exec_lo as data
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().At(0).type != ShaderInstructionType::VAdd3U32)
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, PairedInput());
		    std::_Exit(!result.supported && result.unsupported_pc == 0u ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

UT_END();
