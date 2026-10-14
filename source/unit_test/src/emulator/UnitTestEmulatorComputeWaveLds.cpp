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

// Since 704f4ad3 every LDS access takes the ordered generic path and admission no
// longer proves the address. RDNA2 ISA 3.6.1 makes an out-of-range LDS write a
// discard and an out-of-range read zero. EmulatorShaderLdsBounds checks the
// production memory bodies, including inactive lanes and pointer evaluation.
TEST(EmulatorComputeWaveLds, AdmitsAnyLdsAddressWithoutAnAddressProof)
{
	struct Case { uint32_t move; uint32_t control; uint32_t registers; uint32_t dwords; };
	const Case cases[] = {
	    {0x7e020280u, 0xd8d80000u, 0x04000001u, 1}, // address zero, one dword read
	    {0x7e020300u, 0xd8340000u, 0x00000101u, 1}, // dynamic address v0
	    {0x7e020281u, 0xd8340000u, 0x00000101u, 1}, // unaligned address
	    {0x7e020284u, 0xd8340000u, 0x00000101u, 1}, // beyond the allocation
	    {0x7e020280u, 0xd8340004u, 0x00000101u, 1}, // offset beyond the allocation
	    {0x7e0202c1u, 0xd8340000u, 0x00000101u, 1}, // address+width wraps
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
			    std::_Exit(result.supported ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveLds, LegacyTuplePredicateStaysExactForTheBankedEmitter)
{
	struct Case { uint32_t control; uint32_t registers; ShaderInstructionType type; bool banked; };
	const Case cases[] = {
	    {0xd8d80000u, 0x04000001u, ShaderInstructionType::DsReadB32, true},   // ds_read_b32 v4, v1
	    {0xd8340000u, 0x00000101u, ShaderInstructionType::DsWriteB32, true},  // ds_write_b32 v1, v1
	    {0xd8360000u, 0x00000101u, ShaderInstructionType::DsWriteB32, false}, // GDS
	    {0xd8350000u, 0x00000101u, ShaderInstructionType::DsWriteB32, false}, // reserved control
	    {0xd8780000u, 0x00000101u, ShaderInstructionType::DsWriteB32, false}, // legacy write8 alias
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
			    std::_Exit(banked == test.banked && generic == test.banked &&
			               kind == (test.banked ? ShaderComputeWaveInstructionKind::BankedGenericLds
			                                   : ShaderComputeWaveInstructionKind::Unsupported) ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveLds, RejectsLostAtomicInputsAndWrongRegisterSpans)
{
	// INC/DEC's DATA0 wrap limit and the return destination of SUB_RTN are
	// dropped by legacy aliases; ordinary native atomics cannot repair them.
	for (const uint32_t opcode: {3u, 4u, 0x21u, 0x2u, 0x1eu, 0x38u})
	{
		const uint32_t words[] = {0xd8000000u | (opcode << 18u), 0x04000100u, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code)) { std::_Exit(2); }
			    std::_Exit(!ShaderComputeWaveGenericLdsSupported(code.GetInstructions().At(0)) &&
			               !ShaderAnalyzeComputeWaveCode(code, PairedInput(4)).supported ? 0 : 3);
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

// Branches and s_barrier are lowered by the block dispatcher, which resumes each
// guest wave at its own barrier phase, so neither needs a reachability proof.
TEST(EmulatorComputeWaveLds, AdmitsBarriersAndBranchPlaceholders)
{
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
			    std::_Exit(ShaderAnalyzeComputeWaveCode(code, input).supported ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

UT_END();
