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

TEST(EmulatorComputeWaveControlFlow, NativeExecNonzeroDecidesOnBothExecWords)
{
	// A native wave64 EXECNZ must not decide on the low EXEC word alone: the
	// mask adapter branches on the packed EXEC_LO | EXEC_HI pair.
	constexpr uint32_t words[] = {0xbf890002u, 0xbe880381u, 0xbf820001u, 0xbe880382u, 0xbf810000u};
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ASSERT_TRUE(ShaderTryParseBounded(words, sizeof(words), &code));
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	EXPECT_TRUE(source.ContainsStr("%branch_mask_hi_0 = OpLoad %uint %exec_hi"));
	EXPECT_TRUE(source.ContainsStr("OpBitwiseOr %uint %branch_mask_lo_0 %branch_mask_hi_0"));
	EXPECT_FALSE(source.ContainsStr("OpGroupNonUniformAny %bool %uint_3 %cc_lane_b_0"));
}

UT_END();
