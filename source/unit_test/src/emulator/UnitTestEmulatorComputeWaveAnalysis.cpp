#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Log.h"

#include <cstdlib>

UT_BEGIN(EmulatorComputeWaveAnalysis);

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

static ShaderInstruction ScalarBfeI64(uint32_t pc, int destination, int source, int control)
{
	ShaderInstruction instruction {};
	instruction.pc      = pc;
	instruction.type    = ShaderInstructionType::SBfeI64;
	instruction.format  = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
	instruction.dst     = {.type = ShaderOperandType::Sgpr, .register_id = destination, .size = 2};
	instruction.src[0]  = {.type = ShaderOperandType::Sgpr, .register_id = source, .size = 2};
	instruction.src[1]  = {.type = ShaderOperandType::Sgpr, .register_id = control, .size = 1};
	instruction.src_num = 2;
	return instruction;
}

TEST(EmulatorComputeWaveAnalysis, ClassifiesOnlyDefinedInstructionPrefetchModesAsHints)
{
	// Modes 1-3 are the defined hints. Any other mode has no effect on state, so
	// the generic scalar lowering admits it without calling it a prefetch hint.
	for (const uint32_t encoded: {0xbfa00000u, 0xbfa00001u, 0xbfa00002u, 0xbfa00003u, 0xbfa00004u})
	{
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    uint32_t words[2];
			    words[0] = encoded;
			    words[1] = 0xbf810000u;
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().Size() != 2u ||
			        code.GetInstructions().At(0).type != ShaderInstructionType::SInstPrefetch)
			    {
				    std::_Exit(2);
			    }
			    const bool defined_mode = encoded >= 0xbfa00001u && encoded <= 0xbfa00003u;
			    const bool hint = ShaderClassifyComputeWaveInstruction(code.GetInstructions().At(0)) == ShaderComputeWaveInstructionKind::ScalarHint;
			    std::_Exit(hint == defined_mode ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveAnalysis, DoesNotPromoteSoppHintAliasesToInstructionPrefetchHints)
{
	// s_nop 3 and the SOPP alias 0xbf96 decode through the same placeholder type;
	// they are state-free, so they are admitted but never classified as a hint.
	for (const uint32_t encoded: {0xbf800003u, 0xbf960003u})
	{
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    uint32_t words[2];
			    words[0] = encoded;
			    words[1] = 0xbf810000u;
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().At(0).type != ShaderInstructionType::SInstPrefetch)
			    {
				    std::_Exit(2);
			    }
			    const bool hint = ShaderClassifyComputeWaveInstruction(code.GetInstructions().At(0)) == ShaderComputeWaveInstructionKind::ScalarHint;
			    std::_Exit(!hint ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveAnalysis, ClassifiesUntakenDebugBranchesButNotWaveStateAsHints)
{
	// s_cbranch_cdbg* (0x17..0x1a) are never taken without a debugger. Wave-state
	// controls such as s_setkill are refused by the decoder instead.
	for (const uint32_t encoded: {0xbf970024u, 0xbf980001u, 0xbf990001u, 0xbf9a0001u})
	{
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    uint32_t words[2];
			    words[0] = encoded;
			    words[1] = 0xbf810000u;
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().At(0).type != ShaderInstructionType::SBarrier)
			    {
				    std::_Exit(2);
			    }
			    const bool hint = ShaderClassifyComputeWaveInstruction(code.GetInstructions().At(0)) == ShaderComputeWaveInstructionKind::ScalarHint;
			    std::_Exit(hint ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveAnalysis, PreservesSmemCacheControlBitsForAdmission)
{
	for (const uint32_t flags: {0u, 1u, 2u})
	{
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    uint32_t words[3];
			    words[0] = 0xf4080406u | ((flags & 1u) << 16u) | ((flags & 2u) << 13u);
			    words[1] = 0xfa000050u;
			    words[2] = 0xbf810000u;
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().Size() != 2u ||
			        code.GetInstructions().At(0).type != ShaderInstructionType::SLoadDwordx4)
			    {
				    std::_Exit(2);
			    }
			    std::_Exit(code.GetInstructions().At(0).smem_flags == flags ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveAnalysis, Vop2AndVopcPreserveEncodingControlsWithoutLookahead)
{
	const uint32_t instructions[] = {0x06000100u, 0x7d840100u};
	for (const auto instruction: instructions)
	{
		const uint32_t words[] = {instruction, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code))
			    {
				    std::_Exit(2);
			    }
			    const auto& source = code.GetInstructions().At(0).src[0];
			    std::_Exit(!source.dpp && source.dpp_ctrl == 0u && source.dpp_bank_mask == 0u && source.dpp_row_mask == 0u ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
		const uint32_t sdwa_words[] = {(instruction & ~0x1ffu) | 249u, 0x06060000u, 0xbf810000u};
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(sdwa_words, sizeof(sdwa_words), &code))
			    {
				    std::_Exit(2);
			    }
			    std::_Exit(code.GetInstructions().At(0).vop_sdwa ? 0 : 3);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

// Malformed pairs must fail before generic fallback; other representable source
// forms remain outside the specialized scalar-copy contract.
TEST(EmulatorComputeWaveAnalysis, ScalarMaskCopyClaimsOnlyAlignedBoundedPairs)
{
	ShaderInstruction instruction;
	instruction.type               = ShaderInstructionType::SMovB64;
	instruction.format             = ShaderInstructionFormat::Sdst2Ssrc02;
	instruction.src_num            = 1;
	instruction.dst.type           = ShaderOperandType::ExecLo;
	instruction.dst.size           = 2;
	instruction.src[0].type        = ShaderOperandType::Sgpr;
	instruction.src[0].register_id = 102;
	instruction.src[0].size        = 2;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::ScalarCopy);
	instruction.src[0].register_id = 103;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::Unsupported);
	instruction.src[0].register_id = 5;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::Unsupported);
	instruction.src[0].register_id = 0;
	for (const auto type: {ShaderOperandType::VccHi, ShaderOperandType::ExecHi, ShaderOperandType::M0, ShaderOperandType::Vgpr,
	                       ShaderOperandType::LiteralConstant, ShaderOperandType::FloatInlineConstant})
	{
		instruction.src[0].type = type;
		EXPECT_NE(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::ScalarCopy);
	}
	instruction.src[0].type     = ShaderOperandType::VccLo;
	instruction.dst.type        = ShaderOperandType::Sgpr;
	instruction.dst.register_id = 5;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::Unsupported);
	instruction.dst.register_id = 102;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::ScalarCopy);
	instruction.dst.type        = ShaderOperandType::VccLo;
	instruction.dst.register_id = 0;
	instruction.src[0].type     = ShaderOperandType::IntegerInlineConstant;
	for (const int value: {-17, -16, 0, 64, 65})
	{
		instruction.src[0].constant.i = value;
		const bool claimed = ShaderClassifyComputeWaveInstruction(instruction) == ShaderComputeWaveInstructionKind::ScalarCopy;
		EXPECT_EQ(claimed, value >= -16 && value <= 64) << value;
	}
}

TEST(EmulatorComputeWaveAnalysis, ScalarBfeI64RequiresExactPairs)
{
	auto instruction = ScalarBfeI64(0u, 20, 22, 24);
	ASSERT_TRUE(ShaderInstructionLoweringPreconditions(instruction));
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::ScalarGeneric);

	auto invalid            = instruction;
	invalid.dst.type        = ShaderOperandType::ExecLo;
	invalid.dst.register_id = 0;
	EXPECT_TRUE(ShaderInstructionLoweringPreconditions(invalid));
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(invalid), ShaderComputeWaveInstructionKind::Unsupported);
	invalid             = instruction;
	invalid.src[0].size = 1;
	EXPECT_TRUE(ShaderInstructionLoweringPreconditions(invalid));
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(invalid), ShaderComputeWaveInstructionKind::Unsupported);
	invalid             = instruction;
	invalid.src[1].type = ShaderOperandType::Vgpr;
	EXPECT_TRUE(ShaderInstructionLoweringPreconditions(invalid));
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(invalid), ShaderComputeWaveInstructionKind::Unsupported);
}

TEST(EmulatorComputeWaveAnalysis, GenericFallbackRequiresValidOperandSpans)
{
	ShaderInstruction instruction {};
	instruction.type = ShaderInstructionType::VAddF32;
	instruction.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	instruction.src_num = 2;
	instruction.dst = {.type = ShaderOperandType::Vgpr, .register_id = 255, .size = 1};
	instruction.src[0] = {.type = ShaderOperandType::Vgpr, .register_id = 254, .size = 1};
	instruction.src[1] = {.type = ShaderOperandType::Sgpr, .register_id = 103, .size = 1};
	ASSERT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::BankedGeneric);
	for (int corrupt = 0; corrupt < 6; ++corrupt)
	{
		auto invalid = instruction;
		switch (corrupt)
		{
			case 0: invalid.dst.register_id = 256; break;
			case 1: invalid.dst.size = 2; break; // crosses v255
			case 2: invalid.src[0].register_id = -1; break;
			case 3: invalid.src[1] = {.type = ShaderOperandType::Sgpr, .register_id = 105, .size = 2}; break; // crosses s105
			case 4: invalid.src_num = 1; break; // live source outside the tuple
			case 5: invalid.src_num = 5; break;
		}
		EXPECT_FALSE(ShaderComputeWaveGenericVectorSupported(invalid));
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(invalid), ShaderComputeWaveInstructionKind::Unsupported);
	}
	ShaderInstruction copy {};
	copy.type = ShaderInstructionType::SMovB64;
	copy.format = ShaderInstructionFormat::Sdst2Ssrc02;
	copy.src_num = 1;
	copy.dst = {.type = ShaderOperandType::Sgpr, .register_id = 2, .size = 2};
	copy.src[0] = {.type = ShaderOperandType::Sgpr, .register_id = 4, .size = 2};
	EXPECT_TRUE(ShaderInstructionLoweringPreconditions(copy));
	copy.src[0].register_id = 5;
	EXPECT_FALSE(ShaderComputeWaveGenericScalarSupported(copy));
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(copy), ShaderComputeWaveInstructionKind::Unsupported);
}

TEST(EmulatorComputeWaveAnalysis, IntegerControlsCannotEscapeToGenericLowering)
{
	ShaderInstruction instruction {};
	instruction.type = ShaderInstructionType::VAdd3U32;
	instruction.format = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
	instruction.src_num = 3;
	instruction.dst = {.type = ShaderOperandType::Vgpr, .register_id = 0, .size = 1};
	for (int source = 0; source < 3; ++source)
	{
		instruction.src[source] = {.type = ShaderOperandType::Vgpr, .register_id = source + 1, .size = 1};
	}
	EXPECT_TRUE(ShaderInstructionLoweringPreconditions(instruction));
	for (int corrupt = 0; corrupt < 5; ++corrupt)
	{
		auto invalid = instruction;
		switch (corrupt)
		{
			case 0: invalid.dst.clamp = true; break;
			case 1: invalid.vop3_op_sel = 1; break;
			case 2: invalid.vop3_omod = 1; break;
			case 3: invalid.src[1].size = 2; break;
			case 4: invalid.src[2].negate = true; break;
		}
		EXPECT_FALSE(ShaderComputeWaveGenericVectorSupported(invalid));
		EXPECT_EQ(ShaderClassifyComputeWaveInstruction(invalid), ShaderComputeWaveInstructionKind::Unsupported);
	}
}

UT_END();
