#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include <array>
#include <cstdlib>
#include <initializer_list>

UT_BEGIN(EmulatorComputeWaveMasks);

using namespace Libs::Graphics;

static ShaderComputeInputInfo PairedInput()
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.thread_ids_num                        = 1;
	input.wave_layout                           = {ShaderComputeWaveStrategy::Paired64On32, {64, 1, 1}, {32, 1, 1}, 64, 32, 2, 1, 0};
	return input;
}

static void InitializeShaderConfig()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

static void ExpectExecCompareUnsupportedPc(std::initializer_list<uint32_t> words, uint32_t pc, const char* reason_part)
{
	ASSERT_EXIT(
	    {
		    InitializeShaderConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words.begin(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code))
		    {
			    std::_Exit(2);
		    }
		    const auto result = ShaderAnalyzeComputeWaveCode(code, PairedInput());
		    std::_Exit(!result.supported && result.unsupported_pc == pc && result.reason.ContainsStr(reason_part) ? 0 : 3);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// VOPC compare-and-update-exec words verified against the LLVM gfx1030
// assembler; 0x7daa0a80 is the exact word observed in the workload shader.
static constexpr uint32_t kCmpxNeW0    = 0x7daa0a80u; // v_cmpx_ne_u32 0, v5
static constexpr uint32_t kCmpxEqW0    = 0x7da40c0au; // v_cmpx_eq_u32 s10, v6
static constexpr uint32_t kCmpxLeW0    = 0x7da60702u; // v_cmpx_le_u32 v2, v3
static constexpr uint32_t kCmpxGeW0    = 0x7dac12ffu; // v_cmpx_ge_u32 0xff, v9 (+literal)
static constexpr uint32_t kCmpxSentinel = 0xc8000001u; // v_interp_p1_f32 v0, v1, attr0.x; pixel-only, outside the paired compute set
static constexpr uint32_t kCmpxEnd     = 0xbf810000u; // s_endpgm
static constexpr uint32_t kCmpxVop3W0  = 0xd4d1007eu; // v_cmpx_lt_u32 v7, s12 (VOP3 form)
static constexpr uint32_t kCmpxVop3W1  = 0x00001907u;
static constexpr uint32_t kCmpxSdwaW0  = 0x7daa0af9u; // v_cmpx_ne_u32_sdwa 0, v5
static constexpr uint32_t kCmpxSdwaCtl = 0x06860080u; // DWORD selects, no modifiers
static constexpr uint32_t kCmpxExecSrc = 0x7daa0a7eu; // v_cmpx_ne_u32 exec_lo, v5
static constexpr uint32_t kCmpxNeI32   = 0x7d2a0a80u; // v_cmpx_ne_i32 0, v5
static constexpr uint32_t kCmpxLtF32   = 0x7c220a80u; // v_cmpx_lt_f32 0, v5

TEST(EmulatorComputeWaveMasks, AdmitsPlainU32ExecCompares)
{
	// Every plain VOPC U32 exec-compare tuple; admission is proven by reaching
	// the trailing unsupported instruction at 0x8 (or 0xc behind a literal).
	ExpectExecCompareUnsupportedPc({kCmpxNeW0, kCmpxSentinel, kCmpxEnd}, 0x4u, "VInterpP1F32");
	ExpectExecCompareUnsupportedPc({kCmpxEqW0, kCmpxSentinel, kCmpxEnd}, 0x4u, "VInterpP1F32");
	ExpectExecCompareUnsupportedPc({kCmpxLeW0, kCmpxSentinel, kCmpxEnd}, 0x4u, "VInterpP1F32");
	ExpectExecCompareUnsupportedPc({kCmpxGeW0, 0x000000ffu, kCmpxSentinel, kCmpxEnd}, 0x8u, "VInterpP1F32");
}

TEST(EmulatorComputeWaveMasks, ExecCompareWritesExecWordsNotVcc)
{
	const uint32_t shader[] = {kCmpxNeW0, kCmpxEnd};
	ASSERT_EXIT(
	    {
		    InitializeShaderConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    const auto input  = PairedInput();
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    if (source.FindIndex("OpStore %exec_lo") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpStore %exec_hi") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpStore %vcc_lo") != Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpStore %vcc_hi") != Core::STRING8_INVALID_INDEX)
		    {
			    std::_Exit(3);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// Since 704f4ad3 any VOPC exec compare lowers per bank through the generic
// compare, so the SDWA form, the VOP3 encoding and other element types are
// admitted; only an EXEC source operand stays outside the set, because the
// compare would read the mask it is about to overwrite.
TEST(EmulatorComputeWaveMasks, ExecCompareLeavesOnlyAnExecSourceOperandRejected)
{
	ExpectExecCompareUnsupportedPc({kCmpxExecSrc, kCmpxSentinel, kCmpxEnd}, 0x0u, "exec compare");
	ExpectExecCompareUnsupportedPc({kCmpxSdwaW0, kCmpxSdwaCtl, kCmpxSentinel, kCmpxEnd}, 0x8u, "VInterpP1F32");
	ExpectExecCompareUnsupportedPc({kCmpxVop3W0, kCmpxVop3W1, kCmpxSentinel, kCmpxEnd}, 0x8u, "VInterpP1F32");
	ExpectExecCompareUnsupportedPc({kCmpxNeI32, kCmpxSentinel, kCmpxEnd}, 0x4u, "VInterpP1F32");
	ExpectExecCompareUnsupportedPc({kCmpxLtF32, kCmpxSentinel, kCmpxEnd}, 0x4u, "VInterpP1F32");
}

// 0x8bea106a is the workload word: s_orn2_b64 vcc, vcc, s[16:17].
static constexpr uint32_t kSOrn2W0 = 0x8bea106au;

TEST(EmulatorComputeWaveMasks, AdmitsScalarOrn2MaskPair)
{
	ExpectExecCompareUnsupportedPc({kSOrn2W0, kCmpxSentinel, kCmpxEnd}, 0x4u, "VInterpP1F32");

	// Same tuple through synthetic operands: any scalar mask pair source and
	// any scalar pair destination follow the existing scalar-mask contract.
	ShaderInstruction instruction;
	instruction.type              = ShaderInstructionType::SOrn2B64;
	instruction.format            = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
	instruction.src_num           = 2;
	instruction.dst.type          = ShaderOperandType::VccLo;
	instruction.dst.size          = 2;
	instruction.dst.register_id   = 0;
	instruction.src[0].type       = ShaderOperandType::VccLo;
	instruction.src[0].size       = 2;
	instruction.src[0].register_id = 0;
	instruction.src[1].type       = ShaderOperandType::Sgpr;
	instruction.src[1].size       = 2;
	instruction.src[1].register_id = 16;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::ScalarMask);

	auto modified          = instruction;
	modified.dst.type      = ShaderOperandType::Vgpr;
	modified.dst.register_id = 4;
	EXPECT_NE(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::ScalarMask);
	modified                = instruction;
	modified.src[1].size    = 1;
	EXPECT_NE(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::ScalarMask);
}

// v_cndmask_b32_sdwa v7, 0, 1, vcc; 0x86860680 is the workload control word
// (DWORD selects, S0/S1 constants, no modifiers).
static constexpr uint32_t kCndmaskSdwaW0  = 0x020f02f9u;
static constexpr uint32_t kCndmaskSdwaCtl = 0x86860680u;

TEST(EmulatorComputeWaveMasks, AdmitsIdentitySdwaCndmask)
{
	ExpectExecCompareUnsupportedPc({kCndmaskSdwaW0, kCndmaskSdwaCtl, kCmpxSentinel, kCmpxEnd}, 0x8u, "VInterpP1F32");

	// Any non-DWORD select or modifier changes the result and stays rejected.
	ExpectExecCompareUnsupportedPc({kCndmaskSdwaW0, 0x80860680u, kCmpxSentinel, kCmpxEnd}, 0x0u, "VCndmaskB32 SDWA");
	ExpectExecCompareUnsupportedPc({kCndmaskSdwaW0, 0x86800680u, kCmpxSentinel, kCmpxEnd}, 0x0u, "VCndmaskB32 SDWA");
	ExpectExecCompareUnsupportedPc({kCndmaskSdwaW0, 0x86860080u, kCmpxSentinel, kCmpxEnd}, 0x0u, "VCndmaskB32 SDWA");
	ExpectExecCompareUnsupportedPc({kCndmaskSdwaW0, 0x868e0680u, kCmpxSentinel, kCmpxEnd}, 0x0u, "VCndmaskB32 SDWA");
	ExpectExecCompareUnsupportedPc({kCndmaskSdwaW0, 0x86862680u, kCmpxSentinel, kCmpxEnd}, 0x0u, "VCndmaskB32 SDWA");
}

// Synthetic SOP2 op 0x1b: s_nor_b64 vcc, vcc, s[16:17].
static constexpr uint32_t kSNorW0 = 0x8dea106au;

TEST(EmulatorComputeWaveMasks, AdmitsScalarNorMaskPair)
{
	ExpectExecCompareUnsupportedPc({kSNorW0, kCmpxSentinel, kCmpxEnd}, 0x4u, "VInterpP1F32");

	ShaderInstruction instruction;
	instruction.type               = ShaderInstructionType::SNorB64;
	instruction.format             = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
	instruction.src_num            = 2;
	instruction.dst.type           = ShaderOperandType::Sgpr;
	instruction.dst.size           = 2;
	instruction.dst.register_id    = 8;
	instruction.src[0].type        = ShaderOperandType::ExecLo;
	instruction.src[0].size        = 2;
	instruction.src[0].register_id = 0;
	instruction.src[1].type        = ShaderOperandType::Sgpr;
	instruction.src[1].size        = 2;
	instruction.src[1].register_id = 16;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::ScalarMask);

	auto modified            = instruction;
	modified.dst.type        = ShaderOperandType::Vgpr;
	modified.dst.register_id = 4;
	EXPECT_NE(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::ScalarMask);
	modified             = instruction;
	modified.src[1].size = 1;
	EXPECT_NE(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::ScalarMask);
}

// 0xbefe087e is the workload word: s_not_b64 exec, exec.
static constexpr uint32_t kSNotW0 = 0xbefe087eu;

TEST(EmulatorComputeWaveMasks, AdmitsScalarNotMaskPair)
{
	ExpectExecCompareUnsupportedPc({kSNotW0, kCmpxSentinel, kCmpxEnd}, 0x4u, "VInterpP1F32");

	// Unary complement of a scalar mask pair: same pair tuple as SMovB64.
	ShaderInstruction instruction;
	instruction.type               = ShaderInstructionType::SNotB64;
	instruction.format             = ShaderInstructionFormat::Sdst2Ssrc02;
	instruction.src_num            = 1;
	instruction.dst.type           = ShaderOperandType::ExecLo;
	instruction.dst.size           = 2;
	instruction.dst.register_id    = 0;
	instruction.src[0].type        = ShaderOperandType::ExecLo;
	instruction.src[0].size        = 2;
	instruction.src[0].register_id = 0;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(instruction), ShaderComputeWaveInstructionKind::ScalarMask);

	auto modified          = instruction;
	modified.dst.type      = ShaderOperandType::Sgpr;
	modified.dst.register_id = 8;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::ScalarMask);
	modified                    = instruction;
	modified.src[0].type        = ShaderOperandType::IntegerInlineConstant;
	modified.src[0].size        = 2;
	modified.src[0].constant.i  = -1;
	EXPECT_EQ(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::ScalarMask);

	// One-word sources, two-source tuples, and non-pair destinations reject.
	modified             = instruction;
	modified.src_num     = 2;
	modified.src[1].type = ShaderOperandType::Sgpr;
	modified.src[1].size = 2;
	modified.src[1].register_id = 6;
	EXPECT_NE(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::ScalarMask);
	modified              = instruction;
	modified.src[0].size  = 1;
	EXPECT_NE(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::ScalarMask);
	modified            = instruction;
	modified.dst.type   = ShaderOperandType::Vgpr;
	modified.dst.register_id = 2;
	EXPECT_NE(ShaderClassifyComputeWaveInstruction(modified), ShaderComputeWaveInstructionKind::ScalarMask);
}

TEST(EmulatorComputeWaveMasks, GeneratesPhysicalShapeAndTwoArchitecturalMaskHalves)
{
	// Synthetic VOP3: V_CMP_EQ_U32 VCC, 63, V0. The initial X coordinate
	// makes precisely logical lane 63 true in a one-wave guest group.
	const uint32_t shader[] = {(0xd4u << 24u) | (0xc2u << 16u) | 106u, (128u + 63u) | (256u << 9u), 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitializeShaderConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    if (code.GetInstructions().Size() != 2u || code.GetInstructions().At(0).type != ShaderInstructionType::VCmpEqU32)
		    {
			    std::_Exit(2);
		    }
		    const auto input  = PairedInput();
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    if (source.FindIndex("OpExecutionMode %main LocalSize 32 1 1") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_32 %uint_1 %uint_1") ==
		            Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("%v0_low = OpVariable") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("%v0_high = OpVariable") == Core::STRING8_INVALID_INDEX ||
		        source.FindIndex("OpGroupNonUniformBallot") == Core::STRING8_INVALID_INDEX)
		    {
			    std::_Exit(3);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// V_NOT_B32 was outside the per-opcode allowlist; the generic vector lowering now
// emits it per bank, and the result must still be valid SPIR-V.
TEST(EmulatorComputeWaveMasks, LowersVNotB32ThroughTheGenericVectorPath)
{
	const uint32_t shader[] = {0x7e006f00u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    InitializeShaderConfig();
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    ShaderParse(shader, &code);
		    const auto input  = PairedInput();
		    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		    std::_Exit(source.IsEmpty() ? 3 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// A compare whose SGPR source overlaps its mask-pair destination reads every source
// before the mask is written. The generic compare evaluates both banks' predicates
// first and only then ballots and stores the pair, so the alias is lowered rather
// than rejected as it was when each bank wrote its own half.
TEST(EmulatorComputeWaveMasks, ReadsSourcesOfAnAliasedMaskPairBeforeWritingIt)
{
	const std::array<uint32_t, 3> programs[] = {
	    {(0xd4u << 24u) | (0xc2u << 16u) | 106u, 106u | (128u << 9u), 0xbf810000u},
	    {(0xd4u << 24u) | (0xc2u << 16u) | 4u, 4u | (128u << 9u), 0xbf810000u},
	    {(0xd4u << 24u) | (0xc2u << 16u) | 4u, 5u | (128u << 9u), 0xbf810000u},
	};
	int program_index = 0;
	for (const auto& program: programs)
	{
		SCOPED_TRACE(program_index++);
		EXPECT_EXIT(
		    {
			    InitializeShaderConfig();
			    ShaderCode code;
			    code.SetType(ShaderType::Compute);
			    ShaderParse(program.data(), &code);
			    const auto input  = PairedInput();
			    const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
			    const auto first  = source.FindIndex("%wave_gcmp_exec_0_low");
			    const auto last   = source.FindIndex("%wave_gcmp_active_0_high =");
			    if (first == Core::STRING8_INVALID_INDEX || last == Core::STRING8_INVALID_INDEX || first >= last)
			    {
				    std::_Exit(3);
			    }
			    // No store may separate the two banks' predicates from each other.
			    const auto predicates = source.Mid(first, last - first);
			    if (predicates.FindIndex("OpStore") != Core::STRING8_INVALID_INDEX)
			    {
				    std::_Exit(4);
			    }
			    Vector<uint32_t> binary;
			    Core::String8    error;
			    std::_Exit(ShaderToolchain::Run(source, &binary, &error) && !binary.IsEmpty() ? 0 : 5);
		    },
		    ::testing::ExitedWithCode(0), "");
	}
}

TEST(EmulatorComputeWaveMasks, RejectsInconsistentOrOverflowingGuestLayout)
{
#if defined(_WIN32)
	constexpr int rejected = 321;
#else
	constexpr int rejected = 65;
#endif
	for (uint32_t invalid = 0; invalid < 3u; ++invalid)
	{
		ASSERT_EXIT(
		    {
			    InitializeShaderConfig();
			    const uint32_t shader[] = {0xbf810000u};
			    ShaderCode     code;
			    code.SetType(ShaderType::Compute);
			    ShaderParse(shader, &code);
			    auto input = PairedInput();
			    if (invalid == 0u)
			    {
				    input.threads_num[0] = 128u;
			    } else if (invalid == 1u)
			    {
				    input.lds_dwords = 1u;
			    } else
			    {
				    for (uint32_t axis = 0; axis < 3u; ++axis)
				    {
					    input.wave_layout.guest_local[axis] = UINT32_MAX;
					    input.threads_num[axis]             = UINT32_MAX;
				    }
			    }
			    (void)SpirvGenerateSource(code, nullptr, nullptr, &input);
			    std::_Exit(0);
		    },
		    ::testing::ExitedWithCode(rejected), "");
	}
}

UT_END();
