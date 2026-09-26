#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>

UT_BEGIN(EmulatorComputeWaveLds);

using namespace Libs::Graphics;

static void InitializeConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

static ShaderComputeInputInfo PairedInput(uint32_t dwords, uint32_t lanes = 64)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = lanes;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.thread_ids_num = 1;
	input.lds_dwords = dwords;
	input.wave_layout = {ShaderComputeWaveStrategy::Paired64On32, {lanes, 1, 1}, {lanes / 2, 1, 1}, 64, 32, 2, lanes / 64, dwords};
	return input;
}

TEST(EmulatorComputeWaveLds, ParsesAtomicReturnDestinationAndFullOffset)
{
	// Independently checked against AMD RDNA2 machine-readable ENC_DS and
	// LLVM gfx1030: opcode32 at bit18, VDST4/ADDR2/DATA3, byte offset0x112.
	const uint32_t words[] = {0xd8800112u, 0x04000302u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto& instruction = code.GetInstructions().At(0);
		    const bool  valid       = instruction.type == ShaderInstructionType::DsAddRtnU32 &&
		                              instruction.format == ShaderInstructionFormat::VdstVaddrVdataOffset &&
		                              instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.register_id == 4 &&
		                              instruction.dst.size == 1 && instruction.src_num == 2 && instruction.src[0].register_id == 2 &&
		                              instruction.src[1].register_id == 3 && instruction.ds_offset == 0x112u;
		    std::_Exit(valid ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveLds, PreservesControlsBeforeLegacyAliasLowering)
{
	// A write2/GDS must remain distinguishable from the legacy write32 alias.
	const uint32_t words[] = {0xd83a0004u, 0x00040302u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto& instruction = code.GetInstructions().At(0);
		    std::_Exit(instruction.ds_encoding_control == words[0] && instruction.ds_encoding_registers == words[1] ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveLds, RejectsUnprovenOrInvalidAccessesBeforeLowering)
{
	struct Case { uint32_t move; uint32_t control; uint32_t registers; uint32_t dwords; bool accepted; };
	const Case cases[] = {
	    {0x7e020280u, 0xd8d80000u, 0x04000001u, 1, true},  // address zero, one dword read
	    {0x7e020280u, 0xd8360000u, 0x00000101u, 1, false}, // GDS
	    {0x7e020280u, 0xd8350000u, 0x00000101u, 1, false}, // reserved control
	    {0x7e020280u, 0xd8780000u, 0x00000101u, 1, false}, // legacy write8 alias
	    {0x7e020280u, 0xd8340000u, 0x00000101u, 0, false}, // no LDS allocation
	    {0x7e020300u, 0xd8340000u, 0x00000101u, 1, false}, // dynamic address v0
	    {0x7e020281u, 0xd8340000u, 0x00000101u, 1, false}, // unaligned address
	    {0x7e020284u, 0xd8340000u, 0x00000101u, 1, false}, // beyond allocation
	    {0x7e020280u, 0xd8340004u, 0x00000101u, 1, false}, // offset beyond allocation
	    {0x7e0202c1u, 0xd8340000u, 0x00000101u, 1, false}, // address+width must not wrap
	};
	for (const auto& test: cases)
	{
		const uint32_t words[] = {test.move, test.control, test.registers, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
			    const auto result = ShaderAnalyzeComputeWaveCode(code, PairedInput(test.dwords));
			    std::_Exit(result.supported == test.accepted && (result.supported || result.unsupported_pc == 4u) ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveLds, RejectsBarrierBypassAndUnprovenBarrierControls)
{
	// Branches/placeholders are not a proof that all invocations reach a
	// barrier. Nonzero barrier immediates are outside this exact subset.
	for (const uint32_t control: {0xbf820000u, 0xbf890000u, 0xbf8a0001u})
	{
		const uint32_t words[] = {control, 0xbf8a0000u, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
			    ShaderComputeInputInfo input {};
			    input.wave_layout.strategy = ShaderComputeWaveStrategy::Paired64On32;
			    const auto result = ShaderAnalyzeComputeWaveCode(code, input);
			    std::_Exit(!result.supported && result.unsupported_pc == 0u ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveLds, ExecSelfCopyDoesNotFabricateFullLaneInitialization)
{
	for (const uint32_t restore: {0xbefe047eu, 0xbefe04c1u})
	{
		const uint32_t words[] = {0xbefe0480u, restore, 0x7e020280u, 0xd8d80000u, 0x04000001u, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
			    const auto result = ShaderAnalyzeComputeWaveCode(code, PairedInput(1));
			    const bool full_exec = restore == 0xbefe04c1u;
			    std::_Exit(result.supported == full_exec && (full_exec || result.unsupported_pc == 12u) ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

UT_END();
