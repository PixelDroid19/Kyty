#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
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

UT_END();
