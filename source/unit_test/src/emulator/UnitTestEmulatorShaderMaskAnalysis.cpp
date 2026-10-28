#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderScalarLiveness.h"
#include "Emulator/Log.h"

#include "../../../emulator/src/Graphics/ShaderMaskAnalysis.h"

UT_BEGIN(EmulatorShaderMaskAnalysis);

using namespace Libs::Graphics;

namespace {

constexpr uint32_t kCmpLtU32S4 = (0xd4u << 24u) | (0xc1u << 16u) | 4u;
constexpr uint32_t kCmpSources = 256u | ((256u + 1u) << 9u);
constexpr uint32_t kSubrevCoCi = (0xd4u << 24u) | (0x12au << 16u) | (8u << 8u) | 6u;
constexpr uint32_t kSubrevSources(uint32_t mask_register)
{
	return (129u << 0u) | ((256u + 18u) << 9u) | (mask_register << 18u);
}
constexpr uint32_t kEndpgm = 0xbf810000u;

ShaderCode ParseShader(const uint32_t* words)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ShaderParse(words, &code);
	return code;
}

} // namespace

TEST(EmulatorShaderMaskAnalysis, AcceptsFullPairVcmpProducer)
{
	const uint32_t shader[] = {kCmpLtU32S4, kCmpSources, kSubrevCoCi, kSubrevSources(4u), kEndpgm};
	const auto     code      = ParseShader(shader);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_TRUE(ShaderReverseBorrowMaskHasProvenance(code, 1u));
}

TEST(EmulatorShaderMaskAnalysis, Wave32CompareDefinesOnlyTheLowMaskSgpr)
{
	constexpr uint32_t v_cmp_lt_u32_s13 = (0xd4u << 24u) | (0xc1u << 16u) | 13u;
	constexpr uint32_t s_mov_b32_s20_s14 = (0x17du << 23u) | (20u << 16u) | (0x03u << 8u) | 14u;
	const uint32_t     shader[]          = {v_cmp_lt_u32_s13, kCmpSources, s_mov_b32_s20_s14, kEndpgm};
	const auto         code              = ParseShader(shader);
	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	ASSERT_EQ(code.GetInstructions().At(0).dst.size, 2);

	const auto wave32 = ShaderScalarFlowOf(code, 32u);
	const auto wave64 = ShaderScalarFlowOf(code, 64u);

	EXPECT_TRUE(wave32->holding_entry_value[1].test(14));
	EXPECT_FALSE(wave32->holding_entry_value[1].test(13));
	EXPECT_EQ(wave32->entry_sources[2][20], 14);
	EXPECT_TRUE(wave32->live_at_entry.test(14));
	EXPECT_FALSE(wave64->holding_entry_value[1].test(14));
	EXPECT_FALSE(wave64->live_at_entry.test(14));
}

TEST(EmulatorShaderMaskAnalysis, RejectsRawScalarPairMove)
{
	constexpr uint32_t s_mov_b64_s20_s2 = (0x17du << 23u) | (20u << 16u) | (0x04u << 8u) | 2u;
	const uint32_t     shader[]         = {s_mov_b64_s20_s2, kSubrevCoCi, kSubrevSources(20u), kEndpgm};
	const auto         code             = ParseShader(shader);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_FALSE(ShaderReverseBorrowMaskHasProvenance(code, 1u));
}

TEST(EmulatorShaderMaskAnalysis, RejectsPartialHighWordOverwrite)
{
	constexpr uint32_t s_mov_b32_s5_s2 = (0x17du << 23u) | (5u << 16u) | (0x03u << 8u) | 2u;
	const uint32_t     shader[]       = {kCmpLtU32S4, kCmpSources, s_mov_b32_s5_s2, kSubrevCoCi, kSubrevSources(4u), kEndpgm};
	const auto         code           = ParseShader(shader);

	ASSERT_EQ(code.GetInstructions().Size(), 4u);
	EXPECT_FALSE(ShaderReverseBorrowMaskHasProvenance(code, 2u));
}

TEST(EmulatorShaderMaskAnalysis, RejectsStaticBranchTargetThatBypassesCompare)
{
	// The branch at pc 0 can enter the reverse-borrow at pc 12 without running
	// the compare at pc 4. The destination label must invalidate the proof.
	constexpr uint32_t s_branch_to_consumer = 0xbf820002u;
	const uint32_t     shader[]             = {s_branch_to_consumer, kCmpLtU32S4, kCmpSources, kSubrevCoCi,
	                                           kSubrevSources(4u), kEndpgm};
	const auto         code                 = ParseShader(shader);

	ASSERT_EQ(code.GetInstructions().Size(), 4u);
	EXPECT_FALSE(ShaderReverseBorrowMaskHasProvenance(code, 2u));
}

TEST(EmulatorShaderMaskAnalysis, RejectsBranchBetweenCompareAndConsumer)
{
	constexpr uint32_t s_branch = 0xbf820000u;
	const uint32_t     shader[] = {kCmpLtU32S4, kCmpSources, s_branch, kSubrevCoCi, kSubrevSources(4u), kEndpgm};
	auto               code     = ParseShader(shader);

	ASSERT_EQ(code.GetInstructions().Size(), 4u);
	// Exercise the branch-boundary check independently of the branch's label.
	code.GetLabels().Clear();
	EXPECT_FALSE(ShaderReverseBorrowMaskHasProvenance(code, 2u));
}

TEST(EmulatorShaderMaskAnalysis, RejectsIndirectLabelThatCanBypassCompare)
{
	const uint32_t shader[] = {kCmpLtU32S4, kCmpSources, kSubrevCoCi, kSubrevSources(4u), kEndpgm};
	auto               code = ParseShader(shader);

	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	const auto consumer_pc = code.GetInstructions().At(1u).pc;
	code.GetIndirectLabels().Add(ShaderLabel(consumer_pc, code.GetInstructions().At(0u).pc));
	EXPECT_FALSE(ShaderReverseBorrowMaskHasProvenance(code, 1u));
}

namespace {

constexpr uint32_t kCndmaskWord0       = (0xd4u << 24u) | (0x101u << 16u) | 5u;
constexpr uint32_t kCndmaskSources(uint32_t mask_register)
{
	return 256u | ((256u + 1u) << 9u) | (mask_register << 18u);
}
constexpr uint32_t kSCmpLgU64S4Zero    = (0x17eu << 23u) | (0x13u << 16u) | (128u << 8u) | 4u;
constexpr uint32_t kSAndB64S6S4Exec    = (2u << 30u) | (0x0fu << 23u) | (6u << 16u) | (126u << 8u) | 4u;
constexpr uint32_t kSAndSaveexecS6S4   = (0x17du << 23u) | (6u << 16u) | (0x24u << 8u) | 4u;
constexpr uint32_t kSCbranchScc1Next   = (0x17fu << 23u) | (5u << 16u);
constexpr uint32_t kSCbranchExeczNext  = (0x17fu << 23u) | (8u << 16u);
constexpr uint32_t kVCmpxLtU32         = (0x3eu << 25u) | (0xd1u << 17u) | (1u << 9u) | 256u;
constexpr uint32_t kVReadfirstlaneS8V0 = (0x3fu << 25u) | (8u << 17u) | (2u << 9u) | 256u;

} // namespace

TEST(EmulatorShaderMaskAnalysis, LaneBitReaderKeepsCompareLocal)
{
	const uint32_t shader[] = {kCmpLtU32S4, kCmpSources, kCndmaskWord0, kCndmaskSources(4u), kEndpgm};
	const auto     local    = ShaderLaneLocalMaskResults(ParseShader(shader));

	ASSERT_EQ(local.size(), 3u);
	EXPECT_TRUE(local[0]);
	EXPECT_FALSE(local[1]);
}

TEST(EmulatorShaderMaskAnalysis, WaveWideScalarReaderNeedsWholeCompare)
{
	const uint32_t shader[] = {kCmpLtU32S4, kCmpSources, kSCmpLgU64S4Zero, kCndmaskWord0, kCndmaskSources(4u), kEndpgm};
	const auto     local    = ShaderLaneLocalMaskResults(ParseShader(shader));

	ASSERT_EQ(local.size(), 4u);
	EXPECT_FALSE(local[0]);
}

TEST(EmulatorShaderMaskAnalysis, PointwiseAlgebraKeepsCompareLocalUntilSccIsRead)
{
	const uint32_t local_shader[] = {kCmpLtU32S4, kCmpSources, kSAndB64S6S4Exec, kCndmaskWord0, kCndmaskSources(6u), kEndpgm};
	const uint32_t scc_shader[]   = {kCmpLtU32S4,     kCmpSources,       kSAndB64S6S4Exec, kSCbranchScc1Next,
	                                 kCndmaskWord0, kCndmaskSources(6u), kEndpgm};

	// s_cmp_eq_u32 s0, s1 redefines SCC before the branch reads it.
	constexpr uint32_t s_cmp_eq_u32_s0_s1 = (0x17eu << 23u) | (0x06u << 16u) | (1u << 8u) | 0u;
	const uint32_t     redefined_scc[]    = {kCmpLtU32S4,       kCmpSources,     kSAndB64S6S4Exec,    s_cmp_eq_u32_s0_s1,
	                                         kSCbranchScc1Next, kCndmaskWord0, kCndmaskSources(6u), kEndpgm};

	EXPECT_TRUE(ShaderLaneLocalMaskResults(ParseShader(local_shader))[0]);
	EXPECT_FALSE(ShaderLaneLocalMaskResults(ParseShader(scc_shader))[0]);
	EXPECT_TRUE(ShaderLaneLocalMaskResults(ParseShader(redefined_scc))[0]);
}

TEST(EmulatorShaderMaskAnalysis, ExecBranchAfterSaveexecNeedsWholeCompare)
{
	const uint32_t shader[]    = {kCmpLtU32S4, kCmpSources, kSAndSaveexecS6S4, kSCbranchExeczNext, kEndpgm};
	const uint32_t no_branch[] = {kCmpLtU32S4, kCmpSources, kSAndSaveexecS6S4, kEndpgm};
	const auto     local       = ShaderLaneLocalMaskResults(ParseShader(shader));

	ASSERT_EQ(local.size(), 4u);
	EXPECT_FALSE(local[0]);
	EXPECT_TRUE(ShaderLaneLocalMaskResults(ParseShader(no_branch))[0]);
}

TEST(EmulatorShaderMaskAnalysis, ReadfirstlaneNeedsWholeCompareExec)
{
	constexpr uint32_t v_mov_b32_v2_v0 = (0x3fu << 25u) | (2u << 17u) | (1u << 9u) | 256u;
	const uint32_t     shader[]        = {kVCmpxLtU32, kVReadfirstlaneS8V0, kEndpgm};
	const uint32_t     lane_reader[]   = {kVCmpxLtU32, v_mov_b32_v2_v0, kEndpgm};
	const auto         local           = ShaderLaneLocalMaskResults(ParseShader(shader));

	ASSERT_EQ(local.size(), 3u);
	EXPECT_FALSE(local[0]);
	EXPECT_TRUE(ShaderLaneLocalMaskResults(ParseShader(lane_reader))[0]);
}

TEST(EmulatorShaderMaskAnalysis, LoopBackEdgeReaderNeedsWholeCompare)
{
	// s_cmp_lg_u64 s[4:5], 0 at pc 0; v_cmp at pc 4; s_branch back to pc 0 at pc 12.
	constexpr uint32_t s_branch_to_start = (0x17fu << 23u) | (2u << 16u) | 0xfffcu;
	const uint32_t     shader[]          = {kSCmpLgU64S4Zero, kCmpLtU32S4, kCmpSources, s_branch_to_start, kEndpgm};
	const auto         local             = ShaderLaneLocalMaskResults(ParseShader(shader));

	ASSERT_EQ(local.size(), 4u);
	EXPECT_FALSE(local[1]);

	// The same loop with a lane-bit reader at pc 0 (8 bytes); the branch at pc 16.
	constexpr uint32_t s_branch_over_cndmask = (0x17fu << 23u) | (2u << 16u) | 0xfffbu;
	const uint32_t     lane_loop[] = {kCndmaskWord0, kCndmaskSources(4u), kCmpLtU32S4, kCmpSources, s_branch_over_cndmask, kEndpgm};
	EXPECT_TRUE(ShaderLaneLocalMaskResults(ParseShader(lane_loop))[1]);
}

UT_END();
