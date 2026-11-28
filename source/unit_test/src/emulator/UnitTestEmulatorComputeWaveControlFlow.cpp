#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveNativeEquivalence.h"
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

TEST(EmulatorComputeWaveControlFlow, LanePrivateMasksLowerWithoutBallotsAndTrackVccExecRestore)
{
	// 0: s_mov_b64 vcc, exec
	// 1: v_cmpx_ne_u32 vcc, 0, v5
	// 2: v_mov_b32 v7, 2            (predicated before branch, live at store: must keep exec_lane_lo guard)
	// 3: s_cbranch_execz +1
	// 4: v_mov_b32 v7, 0            (active branch, live at store: exec_lane == 1 proven)
	// 5: s_mov_b64 exec, vcc        (restores full EXEC from vcc)
	// 6: v_mov_b32 v5, 1            (post-restore, live at store: exec_lane == 1 proven via vcc lattice)
	// 7: buffer_store_dword v7, v5, s[0:3], ...
	// 8: s_endpgm
	constexpr uint32_t words[] = {
	    0xbeea047eu, 0x7daa0a80u, 0x7e0e0282u, 0xbf880001u, 0x7e0e0280u,
	    0xbefe046au, 0x7e0a0281u, 0xe0e00004u, 0x80000507u, 0xbf810000u,
	};
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ASSERT_TRUE(ShaderTryParseBounded(words, sizeof(words), &code));
	ASSERT_EQ(code.GetInstructions().Size(), 9u);
	ASSERT_TRUE(ShaderAnalyzeComputeWaveNativeEquivalence(code).supported);

	ShaderComputeInputInfo input {};
	input.threads_num[0]     = 64u;
	input.threads_num[1]     = 1u;
	input.threads_num[2]     = 1u;
	input.lane_private_masks = true;
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 0;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadWrite;
	input.bind.storage_buffers.accesses[0]       = ShaderStorageAccess::Raw;
	ShaderCalcBindingIndices(&input.bind);
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	EXPECT_FALSE(source.ContainsStr("OpGroupNonUniformBallot"));
	EXPECT_FALSE(source.ContainsStr("OpGroupNonUniformAny"));
	EXPECT_FALSE(source.ContainsStr("%mask_exec_1_lane"));
	EXPECT_TRUE(source.ContainsStr("%exec_lo_u_2 = OpLoad %uint %exec_lane_lo"));
	EXPECT_TRUE(source.ContainsStr("%exec_lo_u_4 = OpCopyObject %uint %uint_1"));
	EXPECT_TRUE(source.ContainsStr("%exec_lo_u_6 = OpCopyObject %uint %uint_1"));
	EXPECT_FALSE(ShaderRecompileCS(code, &input).IsEmpty());
}

TEST(EmulatorComputeWaveControlFlow, LanePrivateMasksIfElseAndn2SaveexecRestoresActiveAfterJoin)
{
	// 0: v_cmp_ne_u32 vcc, 0, v5
	// 1: s_and_saveexec_b64 s[16:17], vcc
	// 2: s_xor_b64 s[16:17], exec, s[16:17]
	// 3: s_cbranch_execz +1
	// 4: v_mov_b32 v7, 0            (THEN: active, live at store)
	// 5: s_andn2_saveexec_b64 s[16:17], s[16:17]
	// 6: s_cbranch_execz +1
	// 7: v_mov_b32 v7, 1            (ELSE: active, live at store)
	// 8: s_or_b64 exec, exec, s[16:17]
	// 9: v_mov_b32 v5, 2            (post-join: active restored via CoversWithExec lattice)
	// 10: buffer_store_dword v7, v5, s[0:3], ...
	// 11: s_endpgm
	constexpr uint32_t words[] = {
	    0x7d8a0a80u, 0xbe90246au, 0x8990107eu, 0xbf880001u, 0x7e0e0280u, 0xbe902710u,
	    0xbf880001u, 0x7e0e0281u, 0x88fe107eu, 0x7e0a0282u, 0xe0e00004u, 0x80000507u, 0xbf810000u,
	};
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Config::SetNextGen(true);
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	ASSERT_TRUE(ShaderTryParseBounded(words, sizeof(words), &code));
	ASSERT_EQ(code.GetInstructions().Size(), 12u);
	ASSERT_TRUE(ShaderAnalyzeComputeWaveNativeEquivalence(code).supported);

	ShaderComputeInputInfo input {};
	input.threads_num[0]     = 64u;
	input.threads_num[1]     = 1u;
	input.threads_num[2]     = 1u;
	input.lane_private_masks = true;
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.start_register[0] = 0;
	input.bind.storage_buffers.usages[0]         = ShaderStorageUsage::ReadWrite;
	input.bind.storage_buffers.accesses[0]       = ShaderStorageAccess::Raw;
	ShaderCalcBindingIndices(&input.bind);
	const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
	EXPECT_FALSE(source.ContainsStr("OpGroupNonUniformBallot"));
	EXPECT_FALSE(source.ContainsStr("OpGroupNonUniformAny"));
	EXPECT_FALSE(source.ContainsStr("%mask_exec_1_lane"));
	EXPECT_TRUE(source.ContainsStr("%exec_lo_u_4 = OpCopyObject %uint %uint_1"));
	EXPECT_TRUE(source.ContainsStr("%exec_lo_u_7 = OpCopyObject %uint %uint_1"));
	EXPECT_TRUE(source.ContainsStr("%exec_lo_u_9 = OpCopyObject %uint %uint_1"));
	EXPECT_FALSE(ShaderRecompileCS(code, &input).IsEmpty());
}

UT_END();
