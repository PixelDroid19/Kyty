#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveLds.h"
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

TEST(EmulatorComputeWaveLds, ConditionalExchangeNeverLowersToAddition)
{
	// These opcode tables name conditional exchanges, not adds. Refuse them
	// until their target-generation semantics and return widths are implemented.
	for (const bool next_gen: {false, true})
	{
		for (const uint32_t opcode: {0x7eu, 0xfdu})
		{
			const uint32_t words[] = {0xd8000000u | (opcode << 18u) | 0x0112u, 0x04000302u, 0xbf810000u};
			EXPECT_DEATH(
			    {
				    InitializeConfig();
				    Config::SetNextGen(next_gen);
				    ShaderCode code;
				    code.SetType(ShaderType::Compute);
				    (void)ShaderTryParseBounded(words, sizeof(words), &code);
			    },
			    "");
		}
	}
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

TEST(EmulatorComputeWaveLds, LegacyTuplePredicateStaysExactForTheBankedEmitter)
{
	using Kind = ShaderComputeWaveInstructionKind;
	struct Case { uint32_t control; uint32_t registers; ShaderInstructionType type; bool banked; Kind kind; };
	const Case cases[] = {
	    {0xd8d80000u, 0x04000001u, ShaderInstructionType::DsReadB32, true, Kind::BankedGenericLds},   // ds_read_b32 v4, v1
	    {0xd8340000u, 0x00000101u, ShaderInstructionType::DsWriteB32, true, Kind::BankedGenericLds},  // ds_write_b32 v1, v1
	    {0xd8360000u, 0x00000101u, ShaderInstructionType::DsWriteB32, false, Kind::BankedGdsWrite},   // GDS store, never LDS
	    {0xd8350000u, 0x00000101u, ShaderInstructionType::DsWriteB32, false, Kind::Unsupported},      // reserved control
	};
	for (const auto& test: cases)
	{
		const uint32_t words[] = {test.control, test.registers, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
			    const auto& instruction = code.GetInstructions().At(0);
			    const bool  banked      = ShaderComputeWaveLdsInstructionSupported(instruction);
			    const bool generic = ShaderComputeWaveGenericLdsSupported(instruction);
			    const auto kind = ShaderClassifyComputeWaveInstruction(instruction);
			    std::_Exit(banked == test.banked && generic == test.banked && kind == test.kind ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveLds, RejectsWrongRegisterSpans)
{
	// Decoding refuses the lossy legacy aliases (UnitTestEmulatorShaderLdsBounds);
	// a read2st64 with a data register set is not a native tuple either.
	{
		const uint32_t words[] = {0xd8000000u | (0x38u << 18u), 0x04000100u, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
			    std::_Exit(!ShaderComputeWaveGenericLdsSupported(code.GetInstructions().At(0)) ? 0 : 3);
		    }, ::testing::ExitedWithCode(0), "");
	}
	const uint32_t words[] = {0xd8dc0000u, 0xff000000u, 0xbf810000u}; // read2 v[255:256]
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
		    std::_Exit(!ShaderComputeWaveGenericLdsSupported(code.GetInstructions().At(0)) ? 0 : 3);
	    }, ::testing::ExitedWithCode(0), "");
}

UT_END();
