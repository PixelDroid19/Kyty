#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdlib>
#include <vector>

UT_BEGIN(EmulatorComputeWaveControlFlow);

using namespace Libs::Graphics;

TEST(EmulatorComputeWaveControlFlow, ExecNonzeroRetainsForwardTargetAndFallthrough)
{
	constexpr uint32_t words[] = {0xbf890001u, 0xbf810000u, 0xbf810000u};
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ASSERT_TRUE(ShaderTryParseBounded(words, sizeof(words), &code));
	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	const auto& branch = code.GetInstructions().At(0);
	EXPECT_EQ(branch.type, ShaderInstructionType::SCbranchExecnz);
	EXPECT_EQ(branch.format, ShaderInstructionFormat::Label);
	EXPECT_EQ(branch.src_num, 1);
	EXPECT_EQ(branch.src[0].constant.i, 4);
	ASSERT_EQ(code.GetLabels().Size(), 1u);
	EXPECT_EQ(code.GetLabels().At(0).GetSrc(), 0u);
	EXPECT_EQ(code.GetLabels().At(0).GetDst(), 8u);
	ASSERT_EQ(code.GetIndirectLabels().Size(), 1u);
	EXPECT_EQ(code.GetIndirectLabels().At(0).GetDst(), 4u);
}

TEST(EmulatorComputeWaveControlFlow, ExecNonzeroSignExtendsBackwardDisplacement)
{
	constexpr uint32_t words[] = {0xbf800000u, 0xbf89fffeu, 0xbf810000u};
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ASSERT_TRUE(ShaderTryParseBounded(words, sizeof(words), &code));
	ASSERT_EQ(code.GetInstructions().Size(), 3u);
	EXPECT_EQ(code.GetInstructions().At(1).type, ShaderInstructionType::SCbranchExecnz);
	EXPECT_EQ(code.GetInstructions().At(1).src[0].constant.i, -8);
	ASSERT_EQ(code.GetLabels().Size(), 1u);
	EXPECT_EQ(code.GetLabels().At(0).GetDst(), 0u);
	ASSERT_EQ(code.GetIndirectLabels().Size(), 1u);
	EXPECT_EQ(code.GetIndirectLabels().At(0).GetDst(), 8u);
}

TEST(EmulatorComputeWaveControlFlow, NativeExecNonzeroFailsClosedUntilWaveWidthIsRepresented)
{
	// Native mode has no proof that EXEC_HI can be ignored. This previously
	// decoded as an unsupported placeholder, so the new parser must not turn
	// it into an unguarded low-word-only branch.
	constexpr uint32_t words[] = {0xbf890002u, 0xbe880381u, 0xbf820001u, 0xbe880382u, 0xbf810000u};
	ASSERT_EXIT(
	    {
		    if (!Config::IsInitialized())
		    {
			    Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    }
		    Config::SetNextGen(true);
		    Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		    ShaderCode code;
		    code.SetType(ShaderType::Compute);
		    if (!ShaderTryParseBounded(words, sizeof(words), &code))
		    {
			    std::_Exit(2);
		    }
		    ShaderComputeInputInfo input {};
		    input.threads_num[0] = 64u;
		    input.threads_num[1] = input.threads_num[2] = 1u;
		    (void)SpirvGenerateSource(code, nullptr, nullptr, &input);
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(65), "");
}

// Since 704f4ad3 guest control flow runs as a block dispatcher, exact for any CFG
// of uniform branches, so the barrier-in-arm, back-edge and second-conditional
// variants are admitted along with the reconverged diamond itself.
TEST(EmulatorComputeWaveControlFlow, AdmitsTheDiamondAndItsBlockDispatchedVariants)
{
	// Two guest waves: the comparison selects logical lane 63 in the first
	// wave, while the second wave has an empty EXEC mask.
	constexpr std::array<uint32_t, 12> diamond {0x7e020280u, 0xbf8a0000u, 0xd4c2006au, 256u | (191u << 9u), 0xbefe046au, 0xbf880003u,
	                                            0xbefe04c1u, 0x7e0202aau, 0xbf820002u, 0xbefe04c1u,         0x7e020287u, 0xbf810000u};
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 128;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.thread_ids_num                        = 1;
	input.wave_layout                           = {ShaderComputeWaveStrategy::Paired64On32, {128, 1, 1}, {64, 1, 1}, 64, 32, 2, 2, 0};
	const struct Case
	{
		int      altered_word;
		uint32_t replacement;
	} cases[] = {{-1, 0},
	             {7, 0xbf8a0000u},    // barrier within one divergent arm
	             {8, 0xbf82fffbu},    // backward edge
	             {10, 0xbf880000u}};  // second conditional
	for (const auto& test: cases)
	{
		auto words = diamond;
		if (test.altered_word >= 0)
		{
			words[static_cast<size_t>(test.altered_word)] = test.replacement;
		}
		ShaderCode code;
		code.SetType(ShaderType::Compute);
		ASSERT_TRUE(ShaderTryParseBounded(words.data(), words.size() * sizeof(uint32_t), &code));
		SCOPED_TRACE(test.altered_word);
		EXPECT_TRUE(ShaderAnalyzeComputeWaveCode(code, input).supported);
	}

	// A shared LDS access after reconvergence takes the ordered generic LDS path.
	std::vector<uint32_t> lds_suffix(diamond.begin(), diamond.end());
	lds_suffix.insert(lds_suffix.end() - 1, {0xd8340000u, 0x00000101u});
	input.wave_layout.lds_dwords = 1u;
	ShaderCode with_lds;
	with_lds.SetType(ShaderType::Compute);
	ASSERT_TRUE(ShaderTryParseBounded(lds_suffix.data(), lds_suffix.size() * sizeof(uint32_t), &with_lds));
	EXPECT_TRUE(ShaderAnalyzeComputeWaveCode(with_lds, input).supported);
}

UT_END();
