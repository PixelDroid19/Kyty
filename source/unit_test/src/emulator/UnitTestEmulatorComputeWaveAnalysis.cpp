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

static ShaderComputeInputInfo PairedInput()
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num                        = 1;
	input.wave_layout                           = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0};
	return input;
}

static ShaderInstruction MappedEudLoad()
{
	ShaderInstruction load {};
	load.pc                 = 4u;
	load.type               = ShaderInstructionType::SLoadDwordx4;
	load.format             = ShaderInstructionFormat::Sdst4SbaseSoffset;
	load.dst                = {.type = ShaderOperandType::Sgpr, .register_id = 16, .size = 4};
	load.src[0]             = {.type = ShaderOperandType::Sgpr, .register_id = 12, .size = 2};
	load.src[1].type        = ShaderOperandType::IntegerInlineConstant;
	load.src[1].constant.u  = 0x50u;
	load.src_num            = 2;
	load.smem_flags         = 0u;
	return load;
}

static ShaderComputeInputInfo Type5EudInput()
{
	auto input = PairedInput();
	input.bind.extended.used              = true;
	input.bind.extended.slot              = 5;
	input.bind.extended.start_register    = 12;
	input.bind.extended.eud_user_sgpr_num = 14;
	input.bind.extended.eud_size_dw       = 24;
	input.bind.extended.eud_offset_base   = 32;
	input.bind.extended.data.fields[0]    = 1u;
	return input;
}

static ShaderComputeInputInfo MappedEudInput()
{
	auto input = Type5EudInput();
	input.bind.storage_buffers.buffers_num    = 1;
	input.bind.storage_buffers.dynamic_sload[0] = true;
	input.bind.storage_buffers.sources[0]    = ShaderStorageBindingSource::DynamicScalarLoad;
	ShaderDynamicSLoadMapping mapping {};
	mapping.kind                 = ShaderDynamicSLoadResourceKind::StorageBuffer;
	mapping.resource_index       = 0;
	mapping.destination_register = 16;
	mapping.instruction_pc       = 4u;
	mapping.offset_dw            = 20;
	mapping.dword_count          = 4;
	mapping.last_consumer_pc     = 0x14u;
	input.bind.dynamic_sloads.records.Add(mapping);
	return input;
}

static ShaderInstruction EudBasePairClobber(uint32_t pc)
{
	ShaderInstruction instruction {};
	instruction.pc           = pc;
	instruction.type         = ShaderInstructionType::SMovB64;
	instruction.format       = ShaderInstructionFormat::Sdst2Ssrc02;
	instruction.dst          = {.type = ShaderOperandType::Sgpr, .register_id = 12, .size = 2};
	instruction.src[0].type  = ShaderOperandType::IntegerInlineConstant;
	instruction.src[0].size  = 2;
	instruction.src_num      = 1;
	return instruction;
}

static ShaderInstruction EudBaseHighHalfClobber(uint32_t pc)
{
	ShaderInstruction instruction {};
	instruction.pc           = pc;
	instruction.type         = ShaderInstructionType::SMovB32;
	instruction.format       = ShaderInstructionFormat::SVdstSVsrc0;
	instruction.dst          = {.type = ShaderOperandType::Sgpr, .register_id = 13, .size = 1};
	instruction.src[0].type  = ShaderOperandType::IntegerInlineConstant;
	instruction.src_num      = 1;
	return instruction;
}

static ShaderInstruction EudBasePairRead(uint32_t pc)
{
	ShaderInstruction instruction {};
	instruction.pc           = pc;
	instruction.type         = ShaderInstructionType::SMovB64;
	instruction.format       = ShaderInstructionFormat::Sdst2Ssrc02;
	instruction.dst          = {.type = ShaderOperandType::Sgpr, .register_id = 20, .size = 2};
	instruction.src[0]       = {.type = ShaderOperandType::Sgpr, .register_id = 12, .size = 2};
	instruction.src_num      = 1;
	return instruction;
}

static ShaderInstruction EudBasePairBufferLoad(uint32_t pc)
{
	ShaderInstruction instruction {};
	instruction.pc           = pc;
	instruction.type         = ShaderInstructionType::SBufferLoadDwordx2;
	instruction.format       = ShaderInstructionFormat::Sdst2SvSoffset;
	instruction.dst          = {.type = ShaderOperandType::Sgpr, .register_id = 12, .size = 2};
	instruction.src[0]       = {.type = ShaderOperandType::Sgpr, .register_id = 0, .size = 4};
	instruction.src[1].type  = ShaderOperandType::IntegerInlineConstant;
	instruction.src_num      = 2;
	return instruction;
}

static ShaderInstruction EudTestBranch(uint32_t pc, uint32_t target, bool conditional = false)
{
	ShaderInstruction instruction {};
	instruction.pc                 = pc;
	instruction.type               = conditional ? ShaderInstructionType::SCbranchScc0 : ShaderInstructionType::SBranch;
	instruction.format             = ShaderInstructionFormat::Label;
	instruction.src[0].type        = ShaderOperandType::LiteralConstant;
	instruction.src[0].constant.i  = static_cast<int32_t>(target) - static_cast<int32_t>(pc) - 4;
	instruction.src_num            = 1;
	return instruction;
}

static ShaderInstruction EudTestEnd(uint32_t pc)
{
	ShaderInstruction instruction {};
	instruction.pc     = pc;
	instruction.type   = ShaderInstructionType::SEndpgm;
	instruction.format = ShaderInstructionFormat::Empty;
	return instruction;
}

TEST(EmulatorComputeWaveAnalysis, AdmitsScalarSelectorAndCrossHalfRead)
{
	const uint32_t words[] = {0xbe84039fu, 0xd7600005u, 256u | (4u << 9u), 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code) || code.GetInstructions().Size() != 3u)
		    {
			    std::_Exit(2);
		    }
		    const auto analysis = ShaderAnalyzeComputeWaveCode(code, PairedInput());
		    std::_Exit(analysis.supported ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveAnalysis, PreservesEncodedOpSelAndLowersThe32BitCompare)
{
	// op_sel is decoded faithfully; a 32-bit compare ignores it, so the generic
	// compare lowering admits the tuple instead of rejecting it at its pc.
	const uint32_t words[] = {0xbe84039fu, 0xd4c2086au, 256u | (128u << 9u), 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    if (code.GetInstructions().At(1).vop3_op_sel != 1u)
		    {
			    std::_Exit(3);
		    }
		    std::_Exit(ShaderAnalyzeComputeWaveCode(code, PairedInput()).supported ? 0 : 4);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveAnalysis, AdmitsABranchToTheNextInstruction)
{
	// Guest branches run through the block dispatcher, so the scalar setup and
	// the branch are both admitted and the first unsupported pc is never a branch.
	const uint32_t words[] = {0xbe84039fu, 0xbf820000u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    std::_Exit(ShaderAnalyzeComputeWaveCode(code, PairedInput()).supported ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveAnalysis, NamesUnknownInstructionAtItsOriginalPc)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderInstruction instruction {};
	instruction.pc   = 12u;
	instruction.type = ShaderInstructionType::VInterpP1F32;
	code.GetInstructions().Add(instruction);

	const auto analysis = ShaderAnalyzeComputeWaveCode(code, PairedInput());
	EXPECT_FALSE(analysis.supported);
	EXPECT_EQ(analysis.unsupported_pc, 12u);
	EXPECT_TRUE(analysis.reason.ContainsStr("VInterpP1F32"));
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
			    std::_Exit(hint == defined_mode && ShaderAnalyzeComputeWaveCode(code, PairedInput()).supported ? 0 : 3);
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
			    std::_Exit(!hint && ShaderAnalyzeComputeWaveCode(code, PairedInput()).supported ? 0 : 3);
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

TEST(EmulatorComputeWaveAnalysis, ScalarLoadAdvancesToNextUnsupportedInstructionWithOrWithoutAMapping)
{
	// An unmapped S_LOAD is no longer the first unsupported instruction: the
	// generic scalar lowering reads through the computed pointer.
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(MappedEudLoad());
	ShaderInstruction next {};
	next.pc   = 12u;
	next.type = ShaderInstructionType::VInterpP1F32;
	code.GetInstructions().Add(next);

	EXPECT_EQ(ShaderAnalyzeComputeWaveCode(code, PairedInput()).unsupported_pc, 12u);
	EXPECT_EQ(ShaderAnalyzeComputeWaveCode(code, MappedEudInput()).unsupported_pc, 12u);
}

TEST(EmulatorComputeWaveAnalysis, RejectsSgpr64MoveThatClobbersEudBaseBeforeLoad)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(EudBasePairClobber(0u));
	code.GetInstructions().Add(MappedEudLoad());
	ShaderInstruction end {};
	end.pc   = 12u;
	end.type = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);
	const auto result = ShaderAnalyzeComputeWaveCode(code, MappedEudInput());
	EXPECT_FALSE(result.supported);
	EXPECT_EQ(result.unsupported_pc, 4u);
	EXPECT_TRUE(result.reason.ContainsStr("extended pointer base"));
}

TEST(EmulatorComputeWaveAnalysis, HighHalfClobberOfEudBaseOnlyMattersToALaterMappedLoad)
{
	ShaderInstruction end {};
	end.type   = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;

	// A write to the base after its last use changes nothing the lowering reads.
	ShaderCode unused_after;
	unused_after.SetType(ShaderType::Compute);
	unused_after.GetInstructions().Add(MappedEudLoad());
	unused_after.GetInstructions().Add(EudBaseHighHalfClobber(12u));
	end.pc = 16u;
	unused_after.GetInstructions().Add(end);
	EXPECT_TRUE(ShaderAnalyzeComputeWaveCode(unused_after, MappedEudInput()).supported);

	// A second mapped load through the clobbered base no longer sees the original pointer.
	ShaderCode reused;
	reused.SetType(ShaderType::Compute);
	reused.GetInstructions().Add(MappedEudLoad());
	reused.GetInstructions().Add(EudBaseHighHalfClobber(12u));
	auto second = MappedEudLoad();
	second.pc   = 16u;
	reused.GetInstructions().Add(second);
	end.pc = 24u;
	reused.GetInstructions().Add(end);
	const auto result = ShaderAnalyzeComputeWaveCode(reused, MappedEudInput());
	EXPECT_FALSE(result.supported);
	EXPECT_EQ(result.unsupported_pc, 16u);
	EXPECT_TRUE(result.reason.ContainsStr("extended pointer base"));
}

TEST(EmulatorComputeWaveAnalysis, AdmitsEudBasePairReuseAfterMappedLoad)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(MappedEudLoad());
	code.GetInstructions().Add(EudBasePairBufferLoad(0x18u));
	code.GetInstructions().Add(EudBasePairRead(0x20u));
	code.GetInstructions().Add(EudTestEnd(0x24u));
	const auto result = ShaderAnalyzeComputeWaveCode(code, MappedEudInput());
	EXPECT_TRUE(result.supported) << result.reason.c_str();
}

TEST(EmulatorComputeWaveAnalysis, RejectsEudBasePairReuseAcrossBackEdge)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(MappedEudLoad());
	code.GetInstructions().Add(EudBasePairBufferLoad(0x18u));
	const auto branch = EudTestBranch(0x20u, 4u);
	code.GetInstructions().Add(branch);
	code.GetLabels().Add(ShaderLabel(branch));
	code.GetInstructions().Add(EudTestEnd(0x24u));
	const auto result = ShaderAnalyzeComputeWaveCode(code, MappedEudInput());
	EXPECT_FALSE(result.supported);
	EXPECT_EQ(result.unsupported_pc, 4u);
	EXPECT_TRUE(result.reason.ContainsStr("extended pointer base"));
}

TEST(EmulatorComputeWaveAnalysis, RejectsEudBaseReadWhenBranchSkipsPairWrite)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(MappedEudLoad());
	const auto branch = EudTestBranch(0x18u, 0x24u, true);
	code.GetInstructions().Add(branch);
	code.GetLabels().Add(ShaderLabel(branch));
	code.GetInstructions().Add(EudBasePairBufferLoad(0x1cu));
	code.GetInstructions().Add(EudBasePairRead(0x24u));
	code.GetInstructions().Add(EudTestEnd(0x28u));
	const auto result = ShaderAnalyzeComputeWaveCode(code, MappedEudInput());
	EXPECT_FALSE(result.supported);
	EXPECT_EQ(result.unsupported_pc, 0x24u);
	EXPECT_TRUE(result.reason.ContainsStr("extended pointer base"));
}

TEST(EmulatorComputeWaveAnalysis, RejectsOrdinaryReadOfEudBasePair)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(EudBasePairRead(0u));
	code.GetInstructions().Add(MappedEudLoad());
	ShaderInstruction end {};
	end.pc   = 12u;
	end.type = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);
	const auto result = ShaderAnalyzeComputeWaveCode(code, MappedEudInput());
	EXPECT_FALSE(result.supported);
	EXPECT_EQ(result.unsupported_pc, 0u);
	EXPECT_TRUE(result.reason.ContainsStr("extended pointer base"));
}

TEST(EmulatorComputeWaveAnalysis, RejectsExtendedBaseReadWithoutMappedLoad)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.GetInstructions().Add(EudBasePairRead(0u));
	ShaderInstruction end {};
	end.pc   = 4u;
	end.type = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);
	const auto result = ShaderAnalyzeComputeWaveCode(code, Type5EudInput());
	EXPECT_FALSE(result.supported);
	EXPECT_EQ(result.unsupported_pc, 0u);
	EXPECT_TRUE(result.reason.ContainsStr("extended pointer base"));
}

TEST(EmulatorComputeWaveAnalysis, RejectsExtendedBasePairOutsideSgprRange)
{
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderInstruction end {};
	end.pc   = 0u;
	end.type = ShaderInstructionType::SEndpgm;
	end.format = ShaderInstructionFormat::Empty;
	code.GetInstructions().Add(end);
	auto input = Type5EudInput();
	input.bind.extended.start_register = 106; // past SGPR105
	const auto result                  = ShaderAnalyzeComputeWaveCode(code, input);
	EXPECT_FALSE(result.supported);
	EXPECT_EQ(result.unsupported_pc, 0u);
	EXPECT_TRUE(result.reason.ContainsStr("extended pointer base"));
}

TEST(EmulatorComputeWaveAnalysis, PlainMoveDoesNotInheritNextInstructionsDppControls)
{
	const uint32_t words[] = {0x7e000204u, 0x7e000300u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto& source = code.GetInstructions().At(0).src[0];
		    if (source.dpp || source.dpp_ctrl != 0 || source.dpp_row_mask != 0 || source.dpp_bank_mask != 0)
		    {
			    std::_Exit(3);
		    }
		    std::_Exit(ShaderAnalyzeComputeWaveCode(code, PairedInput()).supported ? 0 : 4);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(EmulatorComputeWaveAnalysis, RejectsErasedSdwaAndScalarDestinationOmod)
{
	// Partial-byte SDWA destination and VOP3 scalar-destination OMOD must not
	// become ordinary moves/lane reads merely because the parser replaces dst.
	const uint32_t cases[][3] = {{0x7e0002f9u, 0x00060000u, 0xbf810000u}, {0xd7600005u, 256u | (4u << 9u) | (1u << 27u), 0xbf810000u}};
	for (const auto& words: cases)
	{
		EXPECT_EXIT(
		    {
			    InitializeConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    if (!ShaderTryParseBounded(words, sizeof(words), &code))
			    {
				    std::_Exit(2);
			    }
			    const auto analysis = ShaderAnalyzeComputeWaveCode(code, PairedInput());
			    std::_Exit(!analysis.supported && analysis.unsupported_pc == 0u ? 0 : 3);
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

TEST(EmulatorComputeWaveAnalysis, AdmitsOnlyTheRealSoppBarrierTuple)
{
	// SOPP opcode 0x0a is a barrier; opcode 0x0b is still a parser placeholder.
	constexpr uint32_t real_words[] = {0xbf8a0000u, 0xbf810000u};
	constexpr uint32_t placeholder_words[] = {0xbf8b0000u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitializeConfig();
		    ShaderCode real;
		    real.SetType(ShaderType::Compute);
		    ShaderCode placeholder;
		    placeholder.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(real_words, sizeof(real_words), &real) ||
		        !ShaderTryParseBounded(placeholder_words, sizeof(placeholder_words), &placeholder))
		    {
			    std::_Exit(2);
		    }
		    const auto real_result = ShaderAnalyzeComputeWaveCode(real, PairedInput());
		    const auto placeholder_result = ShaderAnalyzeComputeWaveCode(placeholder, PairedInput());
		    std::_Exit(real_result.supported && !placeholder_result.supported && placeholder_result.unsupported_pc == 0u ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
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
