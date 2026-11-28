#include "Kyty/UnitTest.h"

#include "../../../emulator/src/Graphics/ShaderSpirvSelectionCut.h"

#include <string>

UT_BEGIN(EmulatorShaderSpirvSelectionCut);

using namespace Kyty::Libs::Graphics;

namespace {

bool Contains(const String8& text, const char* part)
{
	return text.FindIndex(part) != Kyty::Core::STRING8_INVALID_INDEX;
}

std::string MakeSyntheticMain(uint32_t scalar_count, uint32_t body_blocks, bool with_loop, bool include_uninit_and_mentioned,
                              bool use_phi_at_merge)
{
	std::string s;
	s += "%main = OpFunction %void None %fn_ty\n";
	s += "%entry = OpLabel\n";
	for (uint32_t i = 0; i < scalar_count; ++i)
	{
		const char* ptr_ty = (i % 3u == 0u) ? "%_ptr_Function_uint" : ((i % 3u == 1u) ? "%_ptr_Function_int" : "%_ptr_Function_float");
		s += "%v_" + std::to_string(i) + " = OpVariable " + ptr_ty + " Function\n";
	}
	if (include_uninit_and_mentioned)
	{
		s += "%v_uninit = OpVariable %_ptr_Function_uint Function\n";
		s += "%v_ment = OpVariable %_ptr_Function_uint Function\n";
	}

	// Initialize scalars using both OpStore and OpFunctionCall (pointer arg counts as def).
	for (uint32_t i = 0; i < scalar_count; ++i)
	{
		if (i % 5u == 1u)
		{
			s += "%call_" + std::to_string(i) + " = OpFunctionCall %uint %helper %v_" + std::to_string(i) + "\n";
		} else
		{
			const char* val = (i % 3u == 0u) ? "%u0" : ((i % 3u == 1u) ? "%i0" : "%f0");
			s += "OpStore %v_" + std::to_string(i) + " " + val + "\n";
		}
	}
	if (include_uninit_and_mentioned)
	{
		s += "OpStore %v_ment %u0\n";
		s += "OpBranch %pre_branch\n";
		s += "%pre_branch = OpLabel\n";
		s += "OpSelectionMerge %sel_pre_end None\n";
		s += "OpBranchConditional %cond0 %pre_then %sel_pre_end\n";
		s += "%pre_then = OpLabel\n";
		// Only initialized along one branch: NOT must_init at selection header.
		s += "OpStore %v_uninit %u0\n";
		s += "OpBranch %sel_pre_end\n";
		s += "%sel_pre_end = OpLabel\n";
	}

	s += "OpSelectionMerge %sel_merge None\n";
	s += "OpBranchConditional %cond1 %body_0 %sel_merge\n";

	for (uint32_t b = 0; b < body_blocks; ++b)
	{
		s += "%body_" + std::to_string(b) + " = OpLabel\n";
		if (with_loop && b == 1u)
		{
			s += "OpLoopMerge %loop_merge %loop_cont None\n";
			s += "OpBranchConditional %cond2 %loop_cont %loop_merge\n";
			s += "%loop_cont = OpLabel\n";
			s += "OpBranch %body_1\n";
			s += "%loop_merge = OpLabel\n";
		}
		if (include_uninit_and_mentioned && b == 2u)
		{
			s += "%ment_ld = OpLoad %uint %v_ment\n";
		}
		if (b + 1u < body_blocks)
		{
			s += "OpBranch %body_" + std::to_string(b + 1u) + "\n";
		} else
		{
			s += "OpBranch %sel_merge\n";
		}
	}

	s += "%sel_merge = OpLabel\n";
	if (use_phi_at_merge)
	{
		s += "%m_phi = OpPhi %uint %u0 %entry %u1 %body_" + std::to_string(body_blocks - 1u) + "\n";
	}
	for (uint32_t i = 0; i < scalar_count; ++i)
	{
		const char* ty = (i % 3u == 0u) ? "%uint" : ((i % 3u == 1u) ? "%int" : "%float");
		s += "%use_" + std::to_string(i) + " = OpLoad " + ty + " %v_" + std::to_string(i) + "\n";
	}
	if (include_uninit_and_mentioned)
	{
		s += "%use_uninit = OpLoad %uint %v_uninit\n";
		s += "%use_ment = OpLoad %uint %v_ment\n";
	}
	s += "OpReturn\nOpFunctionEnd\n";
	return s;
}

} // namespace

TEST(EmulatorShaderSpirvSelectionCut, CutsMustInitLiveOutScalarsAcrossLoopSelection)
{
	const std::string body = MakeSyntheticMain(62u, 110u, true, true, true);
	String8           out_constants;
	String8           out_main;

	SpirvSelectionCutConfig cfg {};
	cfg.device_address_block  = 4u;
	cfg.vsharp_uniform_buffer = false;

	ASSERT_TRUE(SpirvCutSelectionLiveRanges(String8(body.c_str()), cfg, &out_constants, &out_main));
	EXPECT_TRUE(Contains(out_constants, "%lrc_blk = OpConstant %int 4\n"));
	EXPECT_TRUE(Contains(out_main, "%lrc_zp = OpAccessChain %_ptr_PushConstant_uint %vsharp %lrc_i0 %lrc_blk %lrc_i0\n"));
	EXPECT_TRUE(Contains(out_main, "%lrc_z = OpBitwiseAnd %uint %lrc_tl %lrc_u15\n"));

	// Pre-cut appears before OpSelectionMerge %sel_merge; post-cut appears after OpPhi inside %sel_merge.
	EXPECT_TRUE(Contains(out_main, "%lrc_pre_61_a = OpLoad %int %v_61\n"
	                               "%lrc_pre_61_u = OpBitcast %uint %lrc_pre_61_a\n"
	                               "%lrc_pre_61_b = OpIAdd %uint %lrc_pre_61_u %lrc_z\n"
	                               "%lrc_pre_61_r = OpBitcast %int %lrc_pre_61_b\n"
	                               "OpStore %v_61 %lrc_pre_61_r\n"
	                               "OpSelectionMerge %sel_merge None"));
	EXPECT_TRUE(Contains(out_main, "%m_phi = OpPhi %uint %u0 %entry %u1 %body_109\n"
	                               "%lrc_post_0_a = OpLoad %uint %v_0"));

	// Bitcast cut emitted for non-uint scalars (including %v_1 defined via OpFunctionCall).
	EXPECT_TRUE(Contains(out_main, "%lrc_pre_1_a = OpLoad %int %v_1\n"
	                               "%lrc_pre_1_u = OpBitcast %uint %lrc_pre_1_a\n"
	                               "%lrc_pre_1_b = OpIAdd %uint %lrc_pre_1_u %lrc_z\n"
	                               "%lrc_pre_1_r = OpBitcast %int %lrc_pre_1_b\n"
	                               "OpStore %v_1 %lrc_pre_1_r\n"));
	EXPECT_TRUE(Contains(out_main, "%lrc_pre_2_a = OpLoad %float %v_2\n"
	                               "%lrc_pre_2_u = OpBitcast %uint %lrc_pre_2_a\n"));

	// Uninitialized-on-all-paths and inside-region mentioned variables must never be parked.
	EXPECT_FALSE(Contains(out_main, "%lrc_pre_62"));
	EXPECT_FALSE(Contains(out_main, "OpLoad %uint %v_uninit\n%lrc_"));
	EXPECT_FALSE(Contains(out_main, "OpLoad %uint %v_ment\n%lrc_"));

	// Uniform-buffer %vsharp mode selects %_ptr_Uniform_uint.
	cfg.device_address_block  = 7u;
	cfg.vsharp_uniform_buffer = true;
	ASSERT_TRUE(SpirvCutSelectionLiveRanges(String8(body.c_str()), cfg, &out_constants, &out_main));
	EXPECT_TRUE(Contains(out_constants, "%lrc_blk = OpConstant %int 7\n"));
	EXPECT_TRUE(Contains(out_main, "%lrc_zp = OpAccessChain %_ptr_Uniform_uint %vsharp %lrc_i0 %lrc_blk %lrc_i0\n"));
}

TEST(EmulatorShaderSpirvSelectionCut, RejectsSmallOrLoopFreeSelections)
{
	SpirvSelectionCutConfig cfg {};
	cfg.device_address_block = 2u;
	String8 out_constants;
	String8 out_main;

	const std::string too_few_blocks = MakeSyntheticMain(62u, 40u, true, false, false);
	EXPECT_FALSE(SpirvCutSelectionLiveRanges(String8(too_few_blocks.c_str()), cfg, &out_constants, &out_main));

	const std::string no_loop = MakeSyntheticMain(62u, 110u, false, false, false);
	EXPECT_FALSE(SpirvCutSelectionLiveRanges(String8(no_loop.c_str()), cfg, &out_constants, &out_main));

	const std::string too_few_vars = MakeSyntheticMain(30u, 110u, true, false, false);
	EXPECT_FALSE(SpirvCutSelectionLiveRanges(String8(too_few_vars.c_str()), cfg, &out_constants, &out_main));
}

UT_END();
