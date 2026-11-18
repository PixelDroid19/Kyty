#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvTemplates.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"

#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_RECOMPILER_FUNC(Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);
	const bool discard_result = inst.type == ShaderInstructionType::SCmovB64 && inst.dst.type == ShaderOperandType::Null;

	if (!operand_is_variable(inst.dst) && !discard_result)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n");
	}

	auto dst_value0 = (discard_result ? SpirvValue {} : operand_variable_to_str(inst.dst, 0));
	auto dst_value1 = (discard_result ? SpirvValue {} : operand_variable_to_str(inst.dst, 1));

	if (!discard_result && dst_value0.type != SpirvType::Uint)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n");
	}

	String8 load0;
	String8 load1;
	String8 load2;
	String8 load3;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t3_<index>", index_str, &load3, 1))
	{
		return false;
	}

	String8 params = String8(param[0]) + "\n" + param[1] + "\n" + (param[2] == nullptr ? "" : param[2]) + "\n" +
	                 (param[3] == nullptr ? "" : param[3]) + "\n";
	if (spirv->IsLaneMaskGather(index))
	{
		// Every lane holds its own part's bits of the combined mask; one
		// gather of each lane's own bit completes the wave mask.
		params = params.ReplaceStr("%tb_<index> =", "%tb_part_<index> =").ReplaceStr("%td_<index> =", "%td_part_<index> =") + R"(
    %tg_lane_<index> = OpLoad %uint %gl_SubgroupInvocationID
    %tg_upper_<index> = OpUGreaterThanEqual %bool %tg_lane_<index> %uint_32
    %tg_word_<index> = OpSelect %uint %tg_upper_<index> %td_part_<index> %tb_part_<index>
    %tg_shift_<index> = OpBitwiseAnd %uint %tg_lane_<index> %uint_31
    %tg_bits_<index> = OpShiftRightLogical %uint %tg_word_<index> %tg_shift_<index>
    %tg_bit_<index> = OpBitwiseAnd %uint %tg_bits_<index> %uint_1
    %tg_own_<index> = OpINotEqual %bool %tg_bit_<index> %uint_0
    <gather>
    %tb_<index> = OpCompositeExtract %uint %tg_ballot_<index> 0
    %td_<index> = OpCompositeExtract %uint %tg_ballot_<index> 1
)";
		params = params.ReplaceStr("<gather>", spirv->NativeMaskBallot("tg_own_<index>", "tg_ballot_<index>"));
	}

	static const char* text = R"(
    <load0>
    <load1>
    <load2>
    <load3>
    <params>
    <store0>
    <store1>
    <execz>
    <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<load3>", load3)
	                   .ReplaceStr("<params>", params)
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<store0>", discard_result ? "" : String8("OpStore %<dst0> %tb_<index>").ReplaceStr("<dst0>", dst_value0.value))
	                   .ReplaceStr("<store1>", discard_result ? "" : String8("OpStore %<dst1> %td_<index>").ReplaceStr("<dst1>", dst_value1.value))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_S_Lshl_B64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	if (dst_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

	String8 load0;
	String8 load1;
	String8 load2;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
     <load0>
     <load1>
     <load2>
     <param0>
     <param1>
     <param2>
     <param3>
%t22_<index> = OpBitwiseAnd %uint %t2_<index> %uint_63
     OpStore %temp_uint_2 %t0_<index>
     OpStore %temp_uint_3 %t1_<index>
     OpStore %temp_uint_4 %t22_<index>
%t_<index> = OpFunctionCall %void %shift_left %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
%r0_<index> = OpLoad %uint %temp_uint_0
%r1_<index> = OpLoad %uint %temp_uint_1
     OpStore %<dst0> %r0_<index>
     OpStore %<dst1> %r1_<index>
     <execz>
     <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_S_Lshr_B64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	if (dst_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

	String8 load0;
	String8 load1;
	String8 load2;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
     <load0>
     <load1>
     <load2>
     <param0>
     <param1>
     <param2>
     <param3>
%t22_<index> = OpBitwiseAnd %uint %t2_<index> %uint_63
     OpStore %temp_uint_2 %t0_<index>
     OpStore %temp_uint_3 %t1_<index>
     OpStore %temp_uint_4 %t22_<index>
%t_<index> = OpFunctionCall %void %shift_right %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
%r0_<index> = OpLoad %uint %temp_uint_0
%r1_<index> = OpLoad %uint %temp_uint_1
     OpStore %<dst0> %r0_<index>
     OpStore %<dst1> %r1_<index>
     <execz>
     <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SAshrI64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.dst.size != 2 || inst.src[0].size != 2 || inst.src[1].size > 1)
	{
		return false;
	}

	const bool discard_result = inst.dst.type == ShaderOperandType::Null;
	if (!discard_result && !operand_is_variable(inst.dst))
	{
		return false;
	}
	const auto dst_value0 = discard_result ? SpirvValue {} : operand_variable_to_str(inst.dst, 0);
	const auto dst_value1 = discard_result ? SpirvValue {} : operand_variable_to_str(inst.dst, 1);
	if (!discard_result && (dst_value0.type != SpirvType::Uint || dst_value1.type != SpirvType::Uint))
	{
		return false;
	}

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	String8       load1;
	String8       load2;
	if (!operand_load_uint(spirv, inst.src[0], "ashr_source_lo_<index>", index_str, &load0, 0) ||
	    !operand_load_i64_word(spirv, inst.src[0], "ashr_source_hi_<index>", index_str, &load1, 1) ||
	    !operand_load_uint(spirv, inst.src[1], "ashr_shift_source_<index>", index_str, &load2))
	{
		return false;
	}

	// The two-word value is snapshotted before either destination word is written.
	// The branches keep every SPIR-V shift count below 32; the zero-count arm
	// also avoids relying on a shift by 32 in an unselected OpSelect operand.
	static const char* text = R"(
<load0>
<load1>
<load2>
%ashr_count_<index> = OpBitwiseAnd %uint %ashr_shift_source_<index> %uint_63
%ashr_small_<index> = OpULessThan %bool %ashr_count_<index> %uint_32
OpSelectionMerge %ashr_merge_<index> None
OpBranchConditional %ashr_small_<index> %ashr_small_block_<index> %ashr_large_block_<index>
%ashr_small_block_<index> = OpLabel
%ashr_small_right_<index> = OpShiftRightLogical %uint %ashr_source_lo_<index> %ashr_count_<index>
%ashr_small_cross_count_raw_<index> = OpISub %uint %uint_32 %ashr_count_<index>
%ashr_small_cross_count_<index> = OpBitwiseAnd %uint %ashr_small_cross_count_raw_<index> %uint_31
%ashr_small_cross_<index> = OpShiftLeftLogical %uint %ashr_source_hi_<index> %ashr_small_cross_count_<index>
%ashr_small_join_<index> = OpBitwiseOr %uint %ashr_small_right_<index> %ashr_small_cross_<index>
%ashr_small_is_zero_<index> = OpIEqual %bool %ashr_count_<index> %uint_0
%ashr_small_low_<index> = OpSelect %uint %ashr_small_is_zero_<index> %ashr_source_lo_<index> %ashr_small_join_<index>
%ashr_small_high_<index> = OpShiftRightArithmetic %uint %ashr_source_hi_<index> %ashr_count_<index>
OpStore %temp_uint_0 %ashr_small_low_<index>
OpStore %temp_uint_1 %ashr_small_high_<index>
OpBranch %ashr_merge_<index>
%ashr_large_block_<index> = OpLabel
%ashr_large_count_<index> = OpISub %uint %ashr_count_<index> %uint_32
%ashr_large_low_<index> = OpShiftRightArithmetic %uint %ashr_source_hi_<index> %ashr_large_count_<index>
%ashr_large_high_<index> = OpShiftRightArithmetic %uint %ashr_source_hi_<index> %uint_31
OpStore %temp_uint_0 %ashr_large_low_<index>
OpStore %temp_uint_1 %ashr_large_high_<index>
OpBranch %ashr_merge_<index>
%ashr_merge_<index> = OpLabel
%ashr_result_lo_<index> = OpLoad %uint %temp_uint_0
%ashr_result_hi_<index> = OpLoad %uint %temp_uint_1
%ashr_low_nonzero_<index> = OpINotEqual %bool %ashr_result_lo_<index> %uint_0
%ashr_high_nonzero_<index> = OpINotEqual %bool %ashr_result_hi_<index> %uint_0
%ashr_result_nonzero_<index> = OpLogicalOr %bool %ashr_low_nonzero_<index> %ashr_high_nonzero_<index>
%ashr_scc_<index> = OpSelect %uint %ashr_result_nonzero_<index> %uint_1 %uint_0
<store0>
<store1>
<execz>
OpStore %scc %ashr_scc_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<store0>", discard_result ? "" : String8("OpStore %<dst0> %ashr_result_lo_<index>").ReplaceStr("<dst0>", dst_value0.value))
	                   .ReplaceStr("<store1>", discard_result ? "" : String8("OpStore %<dst1> %ashr_result_hi_<index>").ReplaceStr("<dst1>", dst_value1.value))
	                   .ReplaceStr("<execz>", !discard_result && operand_is_exec(inst.dst) ? EXECZ : "")
	                   .ReplaceStr("<index>", index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_S_Bfe_U64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	if (dst_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

	String8 load0;
	String8 load1;
	String8 load2;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
     <load0>
     <load1>
     <load2>
     <param0>
     <param1>
     <param2>
     <param3>
 %to_<index> = OpBitFieldUExtract %uint %t2_<index> %uint_0  %uint_6
 %ts_<index> = OpBitFieldUExtract %uint %t2_<index> %uint_16 %uint_7
%tn0_<index> = OpISub %uint %uint_64 %to_<index>
%ts2_<index> = OpExtInst %uint %GLSL_std_450 UMin %ts_<index> %tn0_<index>
%tn1_<index> = OpISub %uint %uint_64 %ts2_<index>
%tn2_<index> = OpISub %uint %tn1_<index> %to_<index>
     OpStore %temp_uint_2 %t0_<index>
     OpStore %temp_uint_3 %t1_<index>
     OpStore %temp_uint_4 %tn2_<index>
%tf1_<index> = OpFunctionCall %void %shift_left %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
     OpStore %temp_uint_4 %tn1_<index>
%tf2_<index> = OpFunctionCall %void %shift_right %temp_uint_2 %temp_uint_3 %temp_uint_0 %temp_uint_1 %temp_uint_4
 %r0_<index> = OpLoad %uint %temp_uint_2
 %r1_<index> = OpLoad %uint %temp_uint_3
     OpStore %<dst0> %r0_<index>
     OpStore %<dst1> %r1_<index>
     <execz>
     <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_S_Bfe_I64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.type != ShaderInstructionType::SBfeI64 || inst.format != ShaderInstructionFormat::Sdst2Ssrc02Ssrc1 || inst.src_num != 2 ||
	    inst.dst.size != 2 || inst.src[0].size != 2 || inst.src[1].size > 1)
	{
		return false;
	}

	const bool discard_result = inst.dst.type == ShaderOperandType::Null;
	if (!discard_result && !operand_is_variable(inst.dst))
	{
		return false;
	}
	const auto dst_value0 = discard_result ? SpirvValue {} : operand_variable_to_str(inst.dst, 0);
	const auto dst_value1 = discard_result ? SpirvValue {} : operand_variable_to_str(inst.dst, 1);
	if (!discard_result && (dst_value0.type != SpirvType::Uint || dst_value1.type != SpirvType::Uint))
	{
		return false;
	}

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	String8       load1;
	String8       load2;
	if (!operand_load_uint(spirv, inst.src[0], "bfei64_source_lo_<index>", index_str, &load0, 0) ||
	    !operand_load_i64_word(spirv, inst.src[0], "bfei64_source_hi_<index>", index_str, &load1, 1) ||
	    !operand_load_uint(spirv, inst.src[1], "bfei64_control_<index>", index_str, &load2))
	{
		return false;
	}

	// Shift the source pair right by the field offset, then sign-extract its low
	// and high portions. Clamp the field to the bits remaining in the source.
	// All variable shift counts and signed extract widths stay within 0..31/32.
	static const char* text = R"(
<load0>
<load1>
<load2>
%bfei64_offset_<index> = OpBitFieldUExtract %uint %bfei64_control_<index> %uint_0 %uint_6
%bfei64_width_<index> = OpBitFieldUExtract %uint %bfei64_control_<index> %uint_16 %uint_7
%bfei64_remaining_<index> = OpISub %uint %uint_64 %bfei64_offset_<index>
%bfei64_width_within_<index> = OpULessThan %bool %bfei64_width_<index> %bfei64_remaining_<index>
%bfei64_count_<index> = OpSelect %uint %bfei64_width_within_<index> %bfei64_width_<index> %bfei64_remaining_<index>
%bfei64_small_offset_<index> = OpULessThan %bool %bfei64_offset_<index> %uint_32
%bfei64_offset_count_<index> = OpBitwiseAnd %uint %bfei64_offset_<index> %uint_31
%bfei64_cross_count_raw_<index> = OpISub %uint %uint_32 %bfei64_offset_<index>
%bfei64_cross_count_<index> = OpBitwiseAnd %uint %bfei64_cross_count_raw_<index> %uint_31
%bfei64_cross_shift_<index> = OpShiftLeftLogical %uint %bfei64_source_hi_<index> %bfei64_cross_count_<index>
%bfei64_offset_is_zero_<index> = OpIEqual %bool %bfei64_offset_<index> %uint_0
%bfei64_cross_<index> = OpSelect %uint %bfei64_offset_is_zero_<index> %uint_0 %bfei64_cross_shift_<index>
%bfei64_small_low_shift_<index> = OpShiftRightLogical %uint %bfei64_source_lo_<index> %bfei64_offset_count_<index>
%bfei64_small_low_<index> = OpBitwiseOr %uint %bfei64_small_low_shift_<index> %bfei64_cross_<index>
%bfei64_small_high_<index> = OpShiftRightLogical %uint %bfei64_source_hi_<index> %bfei64_offset_count_<index>
%bfei64_large_offset_raw_<index> = OpISub %uint %bfei64_offset_<index> %uint_32
%bfei64_large_offset_<index> = OpBitwiseAnd %uint %bfei64_large_offset_raw_<index> %uint_31
%bfei64_large_low_<index> = OpShiftRightLogical %uint %bfei64_source_hi_<index> %bfei64_large_offset_<index>
%bfei64_shifted_low_<index> = OpSelect %uint %bfei64_small_offset_<index> %bfei64_small_low_<index> %bfei64_large_low_<index>
%bfei64_shifted_high_<index> = OpSelect %uint %bfei64_small_offset_<index> %bfei64_small_high_<index> %uint_0

%bfei64_low_count_is_small_<index> = OpULessThan %bool %bfei64_count_<index> %uint_32
%bfei64_low_count_<index> = OpSelect %uint %bfei64_low_count_is_small_<index> %bfei64_count_<index> %uint_32
%bfei64_high_count_<index> = OpISub %uint %bfei64_count_<index> %bfei64_low_count_<index>
%bfei64_shifted_low_signed_<index> = OpBitcast %int %bfei64_shifted_low_<index>
%bfei64_shifted_high_signed_<index> = OpBitcast %int %bfei64_shifted_high_<index>
%bfei64_result_low_signed_<index> = OpBitFieldSExtract %int %bfei64_shifted_low_signed_<index> %uint_0 %bfei64_low_count_<index>
%bfei64_result_low_<index> = OpBitcast %uint %bfei64_result_low_signed_<index>
%bfei64_result_high_part_<index> = OpBitFieldSExtract %int %bfei64_shifted_high_signed_<index> %uint_0 %bfei64_high_count_<index>
%bfei64_result_high_from_low_<index> = OpBitFieldSExtract %int %bfei64_result_low_signed_<index> %uint_31 %uint_1
%bfei64_count_exceeds_word_<index> = OpUGreaterThan %bool %bfei64_count_<index> %uint_32
%bfei64_result_high_signed_<index> = OpSelect %int %bfei64_count_exceeds_word_<index> %bfei64_result_high_part_<index> %bfei64_result_high_from_low_<index>
%bfei64_result_high_<index> = OpBitcast %uint %bfei64_result_high_signed_<index>
%bfei64_low_nonzero_<index> = OpINotEqual %bool %bfei64_result_low_<index> %uint_0
%bfei64_high_nonzero_<index> = OpINotEqual %bool %bfei64_result_high_<index> %uint_0
%bfei64_result_nonzero_<index> = OpLogicalOr %bool %bfei64_low_nonzero_<index> %bfei64_high_nonzero_<index>
%bfei64_scc_<index> = OpSelect %uint %bfei64_result_nonzero_<index> %uint_1 %uint_0
<store0>
<store1>
<execz>
OpStore %scc %bfei64_scc_<index>
)";
	*dst_source +=
	    String8(text)
	        .ReplaceStr("<load0>", load0)
	        .ReplaceStr("<load1>", load1)
	        .ReplaceStr("<load2>", load2)
	        .ReplaceStr("<store0>",
	                    discard_result ? "" : String8("OpStore %<dst0> %bfei64_result_low_<index>").ReplaceStr("<dst0>", dst_value0.value))
	        .ReplaceStr("<store1>",
	                    discard_result ? "" : String8("OpStore %<dst1> %bfei64_result_high_<index>").ReplaceStr("<dst1>", dst_value1.value))
	        .ReplaceStr("<execz>", !discard_result && operand_is_exec(inst.dst) ? EXECZ : "")
	        .ReplaceStr("<index>", index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_S_Bfm_B64_Sdst2Ssrc0Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	if (!operand_is_variable(inst.dst) || inst.dst.size != 2)
	{
		return false;
	}
	const auto dst_lo = operand_variable_to_str(inst.dst, 0);
	const auto dst_hi = operand_variable_to_str(inst.dst, 1);
	if (dst_lo.type != SpirvType::Uint || dst_hi.type != SpirvType::Uint)
	{
		return false;
	}

	const String8 index_str = String8::FromPrintf("%u", index);
	String8 width_load;
	String8 offset_load;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &width_load) ||
	    !operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &offset_load))
	{
		return false;
	}

	// S_BFM_B64 builds width low bits and shifts the resulting 64-bit mask
	// by offset. The ISA masks both source words to six bits and leaves SCC
	// unchanged. Split the mask into dwords for SPIR-V without Int64.
	static const char* text = R"(
<width_load>
<offset_load>
<param0>
<param1>
<param2>
<param3>
%bfm_count_<index> = OpBitwiseAnd %uint %t0_<index> %uint_63
%bfm_offset_<index> = OpBitwiseAnd %uint %t1_<index> %uint_63
%bfm_low_count_<index> = OpExtInst %uint %GLSL_std_450 UMin %bfm_count_<index> %uint_32
%bfm_high_count_<index> = OpISub %uint %bfm_count_<index> %bfm_low_count_<index>
%bfm_initial_lo_<index> = OpBitFieldInsert %uint %uint_0 %uint_0xffffffff %uint_0 %bfm_low_count_<index>
%bfm_initial_hi_<index> = OpBitFieldInsert %uint %uint_0 %uint_0xffffffff %uint_0 %bfm_high_count_<index>
OpStore %temp_uint_2 %bfm_initial_lo_<index>
OpStore %temp_uint_3 %bfm_initial_hi_<index>
OpStore %temp_uint_4 %bfm_offset_<index>
%bfm_shift_<index> = OpFunctionCall %void %shift_left %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
%bfm_result_lo_<index> = OpLoad %uint %temp_uint_0
%bfm_result_hi_<index> = OpLoad %uint %temp_uint_1
OpStore %<dst_lo> %bfm_result_lo_<index>
OpStore %<dst_hi> %bfm_result_hi_<index>
<execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<width_load>", width_load)
	                   .ReplaceStr("<offset_load>", offset_load)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1] == nullptr ? "" : param[1])
	                   .ReplaceStr("<param2>", param[2] == nullptr ? "" : param[2])
	                   .ReplaceStr("<param3>", param[3] == nullptr ? "" : param[3])
	                   .ReplaceStr("<dst_lo>", dst_lo.value)
	                   .ReplaceStr("<dst_hi>", dst_hi.value)
	                   .ReplaceStr("<execz>", operand_is_exec(inst.dst) ? EXECZ : "")
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* XXX: And, Lshl, Lshr, CSelect, Or */
KYTY_RECOMPILER_FUNC(Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);
	const bool discard_result = inst.type == ShaderInstructionType::SCmovB32 && inst.dst.type == ShaderOperandType::Null;

	if (!operand_is_variable(inst.dst) && !discard_result)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n");
	}

	auto dst_value = (discard_result ? SpirvValue {} : operand_variable_to_str(inst.dst));

	if (!discard_result && dst_value.type != SpirvType::Uint)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Uint condition ignored (continuing)\n");
	}
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              <param2>
              <store>
              <execz>
              <scc>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<store>", discard_result ? "" : String8("OpStore %<dst> %t_<index>").ReplaceStr("<dst>", dst_value.value))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Add, Mul, Sub */
KYTY_RECOMPILER_FUNC(Recompile_S_XXX_I32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value = operand_variable_to_str(inst.dst);

	if (dst_value.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Uint condition ignored (continuing)\n"); }
	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <param>
              %tu_<index> = OpBitcast %uint %t_<index>
              OpStore %<dst> %tu_<index>
              <scc>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Add, Addc, Bfe, Lshl4Add, MulHi */
KYTY_RECOMPILER_FUNC(Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value = operand_variable_to_str(inst.dst);

	if (dst_value.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Uint condition ignored (continuing)\n"); }
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              <param2>
              <param3>
              OpStore %<dst> %t_<index>
              <scc>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SSubbU32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.src_num != 2)
	{
		return false;
	}

	const bool discard_result = inst.dst.type == ShaderOperandType::Null;
	if (!discard_result && (!operand_is_variable(inst.dst) || inst.dst.size != 1))
	{
		return false;
	}
	const auto dst_value = discard_result ? SpirvValue {} : operand_variable_to_str(inst.dst);
	if (!discard_result && dst_value.type != SpirvType::Uint)
	{
		return false;
	}

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	String8       load1;
	if (!operand_load_uint(spirv, inst.src[0], "subb_source0_<index>", index_str, &load0) ||
	    !operand_load_uint(spirv, inst.src[1], "subb_source1_<index>", index_str, &load1))
	{
		return false;
	}

	// Snapshot SCC before the destination store, then use comparisons that do
	// not overflow when source1 is UINT32_MAX and the incoming borrow is one.
	static const char* text = R"(
<load0>
<load1>
%subb_scc_in_<index> = OpLoad %uint %scc
%subb_scc_nonzero_<index> = OpINotEqual %bool %subb_scc_in_<index> %uint_0
%subb_scc_bit_<index> = OpSelect %uint %subb_scc_nonzero_<index> %uint_1 %uint_0
%subb_result0_<index> = OpISub %uint %subb_source0_<index> %subb_source1_<index>
%subb_result_<index> = OpISub %uint %subb_result0_<index> %subb_scc_bit_<index>
%subb_source_gt_<index> = OpUGreaterThan %bool %subb_source1_<index> %subb_source0_<index>
%subb_sources_equal_<index> = OpIEqual %bool %subb_source1_<index> %subb_source0_<index>
%subb_equal_borrow_<index> = OpLogicalAnd %bool %subb_sources_equal_<index> %subb_scc_nonzero_<index>
%subb_borrow_<index> = OpLogicalOr %bool %subb_source_gt_<index> %subb_equal_borrow_<index>
%subb_scc_out_<index> = OpSelect %uint %subb_borrow_<index> %uint_1 %uint_0
<store>
<execz>
OpStore %scc %subb_scc_out_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<store>", discard_result ? "" : String8("OpStore %<dst> %subb_result_<index>").ReplaceStr("<dst>", dst_value.value))
	                   .ReplaceStr("<execz>", !discard_result && operand_is_exec(inst.dst) ? EXECZ : "")
	                   .ReplaceStr("<index>", index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SSaveexecB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);
	if (!operand_is_variable(inst.dst) || inst.dst.size != 1 || inst.src_num != 1)
	{
		return false;
	}
	const auto destination = operand_variable_to_str(inst.dst);
	if (destination.type != SpirvType::Uint) { return false; }
	const auto index_string = String8::FromPrintf("%u", index);
	String8 load;
	if (!operand_load_uint(spirv, inst.src[0], "saveexec_source_<index>", index_string, &load))
	{
		return false;
	}
	// Read both operands before saving EXEC, including when source and destination alias.
	// The B32 form preserves EXEC_HI and derives SCC from the new EXEC_LO alone.
	*dst_source += String8(R"(
<load>
%saveexec_old_<index> = OpLoad %uint %exec_lo
<operation>
OpStore %<destination> %saveexec_old_<index>
OpStore %exec_lo %saveexec_new_<index>
<execz>
%saveexec_nonzero_<index> = OpINotEqual %bool %saveexec_new_<index> %uint_0
%saveexec_scc_<index> = OpSelect %uint %saveexec_nonzero_<index> %uint_1 %uint_0
OpStore %scc %saveexec_scc_<index>
)")
	                   .ReplaceStr("<load>", load)
	                   .ReplaceStr("<operation>", param[0])
	                   .ReplaceStr("<destination>", destination.value)
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<index>", index_string);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SSaveexecB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	if (dst_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

	if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }

	String8 load0;
	String8 load1;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	String8 exec_update;
	switch (inst.type)
	{
		case ShaderInstructionType::SAndSaveexecB64:
			exec_update = R"(
        %t194_<index> = OpBitwiseAnd %uint %t0_<index> %t190_<index>
               OpStore %exec_lo %t194_<index>
        %t197_<index> = OpBitwiseAnd %uint %t1_<index> %t191_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		case ShaderInstructionType::SAndn1SaveexecB64:
			exec_update = R"(
        %t193_<index> = OpNot %uint %t0_<index>
        %t194_<index> = OpBitwiseAnd %uint %t193_<index> %t190_<index>
               OpStore %exec_lo %t194_<index>
        %t196_<index> = OpNot %uint %t1_<index>
        %t197_<index> = OpBitwiseAnd %uint %t196_<index> %t191_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		case ShaderInstructionType::SOrSaveexecB64:
			exec_update = R"(
        %t194_<index> = OpBitwiseOr %uint %t0_<index> %t190_<index>
               OpStore %exec_lo %t194_<index>
        %t197_<index> = OpBitwiseOr %uint %t1_<index> %t191_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		case ShaderInstructionType::SXorSaveexecB64:
			exec_update = R"(
        %t194_<index> = OpBitwiseXor %uint %t0_<index> %t190_<index>
               OpStore %exec_lo %t194_<index>
        %t197_<index> = OpBitwiseXor %uint %t1_<index> %t191_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		case ShaderInstructionType::SAndn2SaveexecB64:
			exec_update = R"(
        %t193_<index> = OpNot %uint %t190_<index>
        %t194_<index> = OpBitwiseAnd %uint %t0_<index> %t193_<index>
               OpStore %exec_lo %t194_<index>
        %t196_<index> = OpNot %uint %t191_<index>
        %t197_<index> = OpBitwiseAnd %uint %t1_<index> %t196_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		case ShaderInstructionType::SOrn2SaveexecB64:
			exec_update = R"(
        %t193_<index> = OpNot %uint %t190_<index>
        %t194_<index> = OpBitwiseOr %uint %t0_<index> %t193_<index>
               OpStore %exec_lo %t194_<index>
        %t196_<index> = OpNot %uint %t191_<index>
        %t197_<index> = OpBitwiseOr %uint %t1_<index> %t196_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		case ShaderInstructionType::SNandSaveexecB64:
			exec_update = R"(
        %t192_<index> = OpBitwiseAnd %uint %t0_<index> %t190_<index>
        %t194_<index> = OpNot %uint %t192_<index>
               OpStore %exec_lo %t194_<index>
        %t195_<index> = OpBitwiseAnd %uint %t1_<index> %t191_<index>
        %t197_<index> = OpNot %uint %t195_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		case ShaderInstructionType::SNorSaveexecB64:
			exec_update = R"(
        %t192_<index> = OpBitwiseOr %uint %t0_<index> %t190_<index>
        %t194_<index> = OpNot %uint %t192_<index>
               OpStore %exec_lo %t194_<index>
        %t195_<index> = OpBitwiseOr %uint %t1_<index> %t191_<index>
        %t197_<index> = OpNot %uint %t195_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		case ShaderInstructionType::SXnorSaveexecB64:
			exec_update = R"(
        %t192_<index> = OpBitwiseXor %uint %t0_<index> %t190_<index>
        %t194_<index> = OpNot %uint %t192_<index>
               OpStore %exec_lo %t194_<index>
        %t195_<index> = OpBitwiseXor %uint %t1_<index> %t191_<index>
        %t197_<index> = OpNot %uint %t195_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		default: return false;
	}

	static const char* text = R"(
        <load0>
        <load1>
        %t190_<index> = OpLoad %uint %exec_lo
               OpStore %<dst0> %t190_<index>
        %t191_<index> = OpLoad %uint %exec_hi
               OpStore %<dst1> %t191_<index>
        <exec_update>
        <execz>
        <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<exec_update>", exec_update)
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Ge, Gt, Lg, Lt, Le */
KYTY_RECOMPILER_FUNC(Recompile_SCmp_XXX_I32_Ssrc0Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %scc %t3_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Le, Lg, Gt, Lt */
KYTY_RECOMPILER_FUNC(Recompile_SCmp_XXX_U32_Ssrc0Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %scc %t3_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}


KYTY_RECOMPILER_FUNC(Recompile_SMulkI32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value = operand_variable_to_str(inst.dst);

	if (dst_value.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Uint condition ignored (continuing)\n"); }
	if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }

	String8 load0;

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	String8 load_dst;

	if (!operand_load_int(spirv, inst.dst, "tdst_<index>", index_str, &load_dst))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <load_dst>
%t_<index> = OpIMul %int %tdst_<index> %t0_<index>
%tu_<index> = OpBitcast %uint %t_<index>
    OpStore %<dst> %tu_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load_dst>", load_dst)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SMovB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value = operand_variable_to_str(inst.dst);

	if (dst_value.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Uint condition ignored (continuing)\n"); }
	if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }

	String8 load0;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    OpStore %<dst> %t0_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SMovB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	if (dst_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

	// EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;
	String8 load1;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <load1>
    OpStore %<dst0> %t0_<index>
    OpStore %<dst1> %t1_<index>
    <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SAbsI32)
{
	const auto& inst = code.GetInstructions().At(index);
	const bool discard = inst.dst.type == ShaderOperandType::Null;
	if (!discard && !operand_is_variable(inst.dst))
	{
		return false;
	}
	const auto dst = discard ? SpirvValue {} : operand_variable_to_str(inst.dst);
	if (!discard && dst.type != SpirvType::Uint)
	{
		return false;
	}
	const auto index_str = String8::FromPrintf("%u", index);
	String8 load;
	if (!operand_load_int(spirv, inst.src[0], "abs_source_<index>", index_str, &load))
	{
		return false;
	}
	// Unsigned subtraction preserves the ISA's INT_MIN result without signed
	// overflow. SCC observes the result even when the destination is null.
	static const char* text = R"(
<load>
%abs_negative_<index> = OpSLessThan %bool %abs_source_<index> %int_0
%abs_bits_<index> = OpBitcast %uint %abs_source_<index>
%abs_negated_<index> = OpISub %uint %uint_0 %abs_bits_<index>
%abs_result_<index> = OpSelect %uint %abs_negative_<index> %abs_negated_<index> %abs_bits_<index>
<store>
<execz>
%abs_nonzero_<index> = OpINotEqual %bool %abs_result_<index> %uint_0
%abs_scc_<index> = OpSelect %uint %abs_nonzero_<index> %uint_1 %uint_0
OpStore %scc %abs_scc_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load>", load)
	                   .ReplaceStr("<store>", discard ? "" : String8("OpStore %<dst> %abs_result_<index>").ReplaceStr("<dst>", dst.value))
	                   .ReplaceStr("<execz>", operand_is_exec(inst.dst) ? EXECZ : "")
	                   .ReplaceStr("<index>", index_str);
	return true;
}

static const char* ScalarUnaryB32Operation(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SNotB32: return "OpNot";
		case ShaderInstructionType::SBrevB32: return "OpBitReverse";
		default: EXIT("unknown scalar unary bit operation\n"); return "";
	}
}

KYTY_RECOMPILER_FUNC(Recompile_SUnaryB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value = operand_variable_to_str(inst.dst);

	if (dst_value.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value.type != SpirvType::Uint condition ignored (continuing)\n"); }
	if (operand_is_exec(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: operand_is_exec(inst.dst) condition ignored (continuing)\n"); }

	String8 load0;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
%t_<index> = <operation> %uint %t0_<index>
    OpStore %<dst> %t_<index>
    <scc>
)";
	// Insert <scc> before <dst> so SCC_NZ_1 %<dst> resolves.
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<operation>", ScalarUnaryB32Operation(inst.type))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

// s_not_b64: dst[63:0] = ~src0[63:0]; SCC = (dst != 0).
KYTY_RECOMPILER_FUNC(Recompile_SNotB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	if (dst_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

	String8 load0;
	String8 load1;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <load1>
    %tb_<index> = OpNot %uint %t0_<index>
    %td_<index> = OpNot %uint %t1_<index>
    OpStore %<dst0> %tb_<index>
    OpStore %<dst1> %td_<index>
    <execz>
    <scc>
)";
	// Insert <scc> before <dst0>/<dst1> so SCC_NZ_2 placeholders resolve.
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SSwappcB64_Sdst2Ssrc02)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetVsInputInfo();

	if (input_info != nullptr)
	{
		if (!input_info->fetch_external) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !input_info->fetch_external condition ignored (continuing)\n"); }
		if (input_info->fetch_shader_reg != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: input_info->fetch_shader_reg != 0 condition ignored (continuing)\n"); }
	}

	if (input_info != nullptr && input_info->fetch_external && inst.dst.type == ShaderOperandType::Sgpr && inst.dst.register_id == 0 &&
	    inst.src[0].type == ShaderOperandType::Sgpr && inst.src[0].register_id == 0 && index == 1)
	{
		for (int i = 0; i < input_info->resources_num; i++)
		{
			const auto& r = input_info->resources_dst[i];

			String8 text;

			switch (r.registers_num)
			{
				case 1:
					text = R"(
				         %t1_<index> = OpLoad %float %<attr>
				                       OpStore %temp_float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_ %<p0> %temp_float
				)";
					break;
				case 2:
					text = R"(
				         %t1_<index> = OpLoad %v2float %<attr>
				                       OpStore %temp_v2float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_vf2_ %<p0> %<p1> %temp_v2float
				)";
					break;
				case 3:
					text = R"(
				         %t1_<index> = OpLoad %v3float %<attr>
				                       OpStore %temp_v3float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_f1_vf3_ %<p0> %<p1> %<p2> %temp_v3float
				)";
					break;
				case 4:
					text = R"(
				         %t1_<index> = OpLoad %v4float %<attr>
				                       OpStore %temp_v4float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_f1_f1_vf4_ %<p0> %<p1> %<p2> %<p3> %temp_v4float
				)";
					break;
				default:
					KYTY_LOG_DEBUG("WARNING: invalid registers_num %d in shader (continuing)\n", r.registers_num);
					break;
			}

			*dst_source += String8(text)
			                   .ReplaceStr("<index>", String8::FromPrintf("%d_%u", i, index))
			                   .ReplaceStr("<p0>", String8::FromPrintf("v%d", r.register_start + 0))
			                   .ReplaceStr("<p1>", String8::FromPrintf("v%d", r.register_start + 1))
			                   .ReplaceStr("<p2>", String8::FromPrintf("v%d", r.register_start + 2))
			                   .ReplaceStr("<p3>", String8::FromPrintf("v%d", r.register_start + 3))
			                   .ReplaceStr("<attr>", String8::FromPrintf("attr%d", i));
		}
		return true;
	}

	return false;
}

// WQM is a numeric operation on a packed architectural word, independent of
static bool emit_wqm_word(const Spirv* spirv, const String8& src_id, const String8& dst_id, const String8& tag, String8* out)
{
	const auto c1   = spirv->GetConstantUint(1u);
	const auto c2   = spirv->GetConstantUint(2u);
	const auto m_aa = spirv->GetConstantUint(0xaaaaaaaau);
	const auto m_55 = spirv->GetConstantUint(0x55555555u);
	const auto m_cc = spirv->GetConstantUint(0xccccccccu);
	const auto m_33 = spirv->GetConstantUint(0x33333333u);
	if (c1 == "unknown_uint_constant" || c2 == "unknown_uint_constant" || m_aa == "unknown_uint_constant" ||
	    m_55 == "unknown_uint_constant" || m_cc == "unknown_uint_constant" || m_33 == "unknown_uint_constant")
	{
		return false;
	}
	// Bit-parallel quad smear: expand every non-zero 4-bit nibble to 0xF.
	*out += String8(R"(
        %<t>_a1 = OpBitwiseAnd %uint <src> %<maa>
        %<t>_a2 = OpShiftRightLogical %uint %<t>_a1 %<c1>
        %<t>_a3 = OpBitwiseAnd %uint <src> %<m55>
        %<t>_a4 = OpShiftLeftLogical %uint %<t>_a3 %<c1>
        %<t>_a5 = OpBitwiseOr %uint <src> %<t>_a2
        %<t>_p  = OpBitwiseOr %uint %<t>_a5 %<t>_a4
        %<t>_b1 = OpBitwiseAnd %uint %<t>_p %<mcc>
        %<t>_b2 = OpShiftRightLogical %uint %<t>_b1 %<c2>
        %<t>_b3 = OpBitwiseAnd %uint %<t>_p %<m33>
        %<t>_b4 = OpShiftLeftLogical %uint %<t>_b3 %<c2>
        %<t>_b5 = OpBitwiseOr %uint %<t>_p %<t>_b2
        <dst> = OpBitwiseOr %uint %<t>_b5 %<t>_b4
)")
	            .ReplaceStr("<t>", tag)
	            .ReplaceStr("<src>", src_id)
	            .ReplaceStr("<dst>", dst_id)
	            .ReplaceStr("<c1>", c1)
	            .ReplaceStr("<c2>", c2)
	            .ReplaceStr("<maa>", m_aa)
	            .ReplaceStr("<m55>", m_55)
	            .ReplaceStr("<mcc>", m_cc)
	            .ReplaceStr("<m33>", m_33);
	return true;
}

// the register that holds it. The native adapter refreshes the EXEC lane view
// after a mask destination write, just as it does for any other scalar ALU.
KYTY_RECOMPILER_FUNC(Recompile_SWqmB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	const auto dst_value = operand_variable_to_str(inst.dst);
	if (dst_value.type != SpirvType::Uint)
	{
		return false;
	}

	String8 index_str = String8::FromPrintf("%u", index);
	String8 load0;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	String8 wqm_body;
	if (!emit_wqm_word(spirv, "%t0_" + index_str, "%wqm_r7_" + index_str, "wqm32_" + index_str, &wqm_body))
	{
		return false;
	}

	*dst_source += load0.ReplaceStr("<index>", index_str) + wqm_body;
	*dst_source += String8("OpStore %<dst> %wqm_r7_<index>\n<scc>\n<execz>\n")
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<execz>", operand_is_exec(inst.dst) ? EXECZ : "")
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SWqmB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_is_variable(inst.dst)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !operand_is_variable(inst.dst) condition ignored (continuing)\n"); }

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	if (dst_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

	String8 load0;
	String8 load1;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	String8 wqm_body;
	if (!emit_wqm_word(spirv, "%t0_" + index_str, "%t235_" + index_str, "wqm64l_" + index_str, &wqm_body) ||
	    !emit_wqm_word(spirv, "%t1_" + index_str, "%t2351_" + index_str, "wqm64h_" + index_str, &wqm_body))
	{
		return false;
	}

	*dst_source += load0.ReplaceStr("<index>", index_str) + "\n" + load1.ReplaceStr("<index>", index_str) + wqm_body;
	*dst_source += String8(R"(
               OpStore %<dst0> %t235_<index>
               OpStore %<dst1> %t2351_<index>
        <execz>
        <scc>
)")
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

// ISA 12.5.1: save wave, stall wave gen, halt waves, ordered PS done and GS alloc req signal the hardware and leave
// shader state alone. GS output and GS done carry the geometry stream, and any other type is not listed, so those
// stop the translation instead of being dropped.
KYTY_RECOMPILER_FUNC(Recompile_SSendmsg)
{
	const auto&    inst    = code.GetInstructions().At(index);
	const uint32_t message = inst.src[0].constant.u & 0xfu;
	return message >= 4u && message <= 9u && message != 8u;
}

/* SInstPrefetch, SWaitcnt */
KYTY_RECOMPILER_FUNC(Recompile_Skip)
{
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
