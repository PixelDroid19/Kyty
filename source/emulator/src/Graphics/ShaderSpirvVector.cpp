#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvTemplates.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"

#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static uint32_t null_mrt_target(ShaderInstructionFormat::Format format)
{
	switch (format)
	{
		case ShaderInstructionFormat::Mrt0OffOffComprVmDone: return 0;
		case ShaderInstructionFormat::Mrt1OffOffComprVmDone: return 1;
		case ShaderInstructionFormat::Mrt2OffOffComprVmDone: return 2;
		case ShaderInstructionFormat::Mrt3OffOffComprVmDone: return 3;
		printf("WARNING: not a null MRT done format (continuing)\n");
	}
	return 0;
}

// Null MRT done exports are no-ops unless they form the captured discard tail:
// SPI_SHADER_COL_FORMAT ZERO: the hardware drops every export to that target,
// so the recompiled shader must not store a color for it. The matching
// CB_SHADER_MASK nibble already clears the Vulkan color write mask.
constexpr uint8_t kColorExportModeZero = 0;

// exec=0; EXP MRTn null/done; endpgm. The target number does not change the
// discard semantics.
KYTY_RECOMPILER_FUNC(Recompile_Exp_MrtNullDone)
{
	const auto& inst = code.GetInstructions().At(index);
	EXIT_NOT_IMPLEMENTED(!ShaderIsNullMrtDoneFormat(inst.format));
	EXIT_NOT_IMPLEMENTED(inst.src_num > 0);

	if (index > 0 && index + 1 < code.GetInstructions().Size())
	{
		const auto& prev_inst = code.GetInstructions().At(index - 1);
		if (code.ReadBlock(prev_inst.pc).is_discard)
		{
			const auto* info   = spirv->GetPsInputInfo();
			const auto  target = null_mrt_target(inst.format);
			// The null export is the guest's inactive-execution tail. Its
			// presence is authoritative for this shader; DB_SHADER_CONTROL's
			// KILL_ENABLE bit is a pipeline scheduling hint and is not a reason
			// to drop the guest's execution-mask semantics.
			EXIT_NOT_IMPLEMENTED(info == nullptr);
			EXIT_NOT_IMPLEMENTED(info->target_output_mode[target] != 4);
			*dst_source += "        OpKill\n";
			return true;
		}
	}

	// MRT0 null/done has only been evidenced as a discard tail. Keep unsupported
	// standalone MRT0 behavior strict; MRT1-3 remain captured no-op terminators.
	if (inst.format == ShaderInstructionFormat::Mrt0OffOffComprVmDone)
	{
		return false;
	}
	return true;
}

// Compressed half2 MRT export → Location <mrt>. param[0] is the SPIR-V output
// variable name (outColor, outColor1, …).
KYTY_RECOMPILER_FUNC(Recompile_Exp_Mrt_Compr_Vsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto* info = spirv->GetPsInputInfo();
	EXIT_NOT_IMPLEMENTED(info == nullptr);
	EXIT_NOT_IMPLEMENTED(param[0] == nullptr);

	// MRT index from trailing digit of outColor / outColorN.
	int mrt = 0;
	if (param[0][0] != '\0' && strcmp(param[0], "outColor") != 0)
	{
		const char* p = param[0];
		while (*p != '\0' && (*p < '0' || *p > '9'))
		{
			p++;
		}
		EXIT_NOT_IMPLEMENTED(*p == '\0');
		mrt = *p - '0';
	}
	EXIT_NOT_IMPLEMENTED(mrt < 0 || mrt > 7);
	if (info->target_output_mode[mrt] == kColorExportModeZero)
	{
		return true;
	}
	EXIT_NOT_IMPLEMENTED(info->target_output_mode[mrt] != 4);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED((inst.exp_enable_mask & 0xcu) != 0 && !operand_is_variable(inst.src[1]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);

	// TODO() check VSKIP
	// TODO() check EXEC

	const auto index_str = String8::FromPrintf("%u", index);
	String8    load_src0;
	String8    load_src1;
	if (spirv->CanLoadPackedHalfForExport(index, inst.src[0]))
	{
		load_src0 = String8("%t2_<index> = OpLoad %uint %<src0_packed>")
		                .ReplaceStr("<index>", index_str)
		                .ReplaceStr("<src0_packed>", packed_half_shadow_to_str(inst.src[0]));
	} else
	{
		load_src0 =
		    (String8("%t1_<index> = OpLoad %float %<src0>\n") + String8(' ', 9) + String8("%t2_<index> = OpBitcast %uint %t1_<index>"))
		        .ReplaceStr("<index>", index_str)
		        .ReplaceStr("<src0>", src0_value.value);
	}
	if (spirv->CanLoadPackedHalfForExport(index, inst.src[1]))
	{
		load_src1 = String8("%t7_<index> = OpLoad %uint %<src1_packed>")
		                .ReplaceStr("<index>", index_str)
		                .ReplaceStr("<src1_packed>", packed_half_shadow_to_str(inst.src[1]));
	} else
	{
		load_src1 =
		    (String8("%t6_<index> = OpLoad %float %<src1>\n") + String8(' ', 9) + String8("%t7_<index> = OpBitcast %uint %t6_<index>"))
		        .ReplaceStr("<index>", index_str)
		        .ReplaceStr("<src1>", src1_value.value);
	}

	static const char* text           = R"(
         %exp_exec_u_<index> = OpLoad %uint %exec_lo
         %exp_exec_b_<index> = OpINotEqual %bool %exp_exec_u_<index> %uint_0
               OpSelectionMerge %exp_merge_<index> None
               OpBranchConditional %exp_exec_b_<index> %exp_store_<index> %exp_kill_<index>
         %exp_kill_<index> = OpLabel
               OpKill
         %exp_store_<index> = OpLabel
		 %exp_old_<index> = OpLoad %v4float %<mrt>
		 %exp_old0_<index> = OpCompositeExtract %float %exp_old_<index> 0
		 %exp_old1_<index> = OpCompositeExtract %float %exp_old_<index> 1
		 %exp_old2_<index> = OpCompositeExtract %float %exp_old_<index> 2
		 %exp_old3_<index> = OpCompositeExtract %float %exp_old_<index> 3
         <load_src0>
         %t3_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t2_<index>
         %t4_<index> = OpCompositeExtract %float %t3_<index> 0
         %t5_<index> = OpCompositeExtract %float %t3_<index> 1
         <load_src1>
		 %t8_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t7_<index>
         %t9_<index> = OpCompositeExtract %float %t8_<index> 0
         %t10_<index> = OpCompositeExtract %float %t8_<index> 1
		 <tap_load>
		 <export_value>
		       OpStore %<mrt> %t11_<index>
               OpBranch %exp_merge_<index>
         %exp_merge_<index> = OpLabel
)";
	const uint32_t     component0     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 0);
	const uint32_t     component1     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 1);
	const uint32_t     component2     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 2);
	const uint32_t     component3     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 3);
	const char*        enabled_names[] = {"t4", "t5", "t9", "t10"};
	const char*        disabled_names[] = {"exp_old0", "exp_old1", "exp_old2", "exp_old3"};
	const char*        source_names[4] {};
	for (uint32_t component = 0; component < 4; component++)
	{
		source_names[component] = (inst.exp_enable_mask & (1u << component)) != 0 ? enabled_names[component] : disabled_names[component];
	}
	uint32_t tap_pc = 0;
	int      tap_register = 0;
	const bool fragment_tap = FragmentTapSelection(code, &tap_pc, &tap_register);
	const String8 tap_load = fragment_tap
	                             ? String8("%fs_tap_out_0_<index> = OpLoad %float %fs_tap_0\n"
	                                       "         %fs_tap_out_1_<index> = OpLoad %float %fs_tap_1\n"
	                                       "         %fs_tap_out_2_<index> = OpLoad %float %fs_tap_2\n"
	                                       "         %fs_tap_out_3_<index> = OpLoad %float %fs_tap_3")
	                                   .ReplaceStr("<index>", index_str)
	                             : String8();
	const String8 export_value =
	    fragment_tap
	        ? String8("%t11_<index> = OpCompositeConstruct %v4float %fs_tap_out_0_<index> %fs_tap_out_1_<index> "
	                  "%fs_tap_out_2_<index> %fs_tap_out_3_<index>")
	              .ReplaceStr("<index>", index_str)
	        : String8::FromPrintf("%%t11_<index> = OpCompositeConstruct %%v4float %%%s_<index> %%%s_<index> %%%s_<index> %%%s_<index>",
	                              source_names[component0], source_names[component1], source_names[component2], source_names[component3]);

	*dst_source += String8(text)
	                   .ReplaceStr("<export_value>", export_value)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<load_src0>", load_src0)
	                   .ReplaceStr("<load_src1>", load_src1)
	                   .ReplaceStr("<tap_load>", tap_load)
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<mrt>", param[0]);

	return true;
}

// Full float32 MRT export → Location <mrt>.
KYTY_RECOMPILER_FUNC(Recompile_Exp_Mrt_Full_Vsrc0Vsrc1Vsrc2Vsrc3)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto* info = spirv->GetPsInputInfo();
	EXIT_NOT_IMPLEMENTED(info == nullptr);
	EXIT_NOT_IMPLEMENTED(param[0] == nullptr);

	int mrt = 0;
	if (strcmp(param[0], "outColor") != 0)
	{
		const char* p = param[0];
		while (*p != '\0' && (*p < '0' || *p > '9'))
		{
			p++;
		}
		EXIT_NOT_IMPLEMENTED(*p == '\0');
		mrt = *p - '0';
	}
	EXIT_NOT_IMPLEMENTED(mrt < 0 || mrt > 7);
	if (info->target_output_mode[mrt] == kColorExportModeZero)
	{
		return true;
	}
	EXIT_NOT_IMPLEMENTED(info->target_output_mode[mrt] != 9);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[3]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);
	auto src2_value = operand_variable_to_str(inst.src[2]);
	auto src3_value = operand_variable_to_str(inst.src[3]);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text           = R"(
         %exp_exec_u_<index> = OpLoad %uint %exec_lo
         %exp_exec_b_<index> = OpINotEqual %bool %exp_exec_u_<index> %uint_0
               OpSelectionMerge %exp_merge_<index> None
               OpBranchConditional %exp_exec_b_<index> %exp_store_<index> %exp_kill_<index>
         %exp_kill_<index> = OpLabel
               OpKill
         %exp_store_<index> = OpLabel
         %t0_<index> = OpLoad %float %<src0>
		 %t1_<index> = OpLoad %float %<src1>
		 %t2_<index> = OpLoad %float %<src2>
		 %t3_<index> = OpLoad %float %<src3>
		 <export_value>
		       OpStore %<mrt> %t11_<index>
               OpBranch %exp_merge_<index>
         %exp_merge_<index> = OpLabel
)";
	const uint32_t     component0     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 0);
	const uint32_t     component1     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 1);
	const uint32_t     component2     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 2);
	const uint32_t     component3     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 3);
	const char*        source_names[] = {"t0", "t1", "t2", "t3"};
	const String8      export_value =
	    String8::FromPrintf("%%t11_<index> = OpCompositeConstruct %%v4float %%%s_<index> %%%s_<index> %%%s_<index> %%%s_<index>",
	                        source_names[component0], source_names[component1], source_names[component2], source_names[component3]);

	*dst_source += String8(text)
	                   .ReplaceStr("<export_value>", export_value)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<src2>", src2_value.value)
	                   .ReplaceStr("<src3>", src3_value.value)
	                   .ReplaceStr("<mrt>", param[0]);

	return true;
}

/* XXX: 0, 1, 2, 3, 4 */
KYTY_RECOMPILER_FUNC(Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[3]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);
	auto src2_value = operand_variable_to_str(inst.src[2]);
	auto src3_value = operand_variable_to_str(inst.src[3]);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
         %t0_<index> = OpLoad %float %<src0>
         %t1_<index> = OpLoad %float %<src1>
         %t2_<index> = OpLoad %float %<src2>
         %t3_<index> = OpLoad %float %<src3>
         %t4_<index> = OpCompositeConstruct %v4float %t0_<index> %t1_<index> %t2_<index> %t3_<index>
               OpStore %<param> %t4_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<src2>", src2_value.value)
	                   .ReplaceStr("<src3>", src3_value.value)
	                   .ReplaceStr("<param>", param[0]);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_Exp_Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[3]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);
	auto src2_value = operand_variable_to_str(inst.src[2]);
	auto src3_value = operand_variable_to_str(inst.src[3]);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
         %t0_<index> = OpLoad %float %<src0>
         %t1_<index> = OpLoad %float %<src1>
         %t2_<index> = OpLoad %float %<src2>
         %t3_<index> = OpLoad %float %<src3>
         %t4_<index> = OpCompositeConstruct %v4float %t0_<index> %t1_<index> %t2_<index> %t3_<index>
         %t5_<index> = OpAccessChain %_ptr_Output_v4float %outPerVertex %int_per_vertex_0
               OpStore %t5_<index> %t4_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<src2>", src2_value.value)
	                   .ReplaceStr("<src3>", src3_value.value);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_Exp_PrimVsrc0OffOffOffDone)
{
	const auto& inst    = code.GetInstructions().At(index);
	const auto* vs_info = spirv->GetVsInputInfo();

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));

	return (vs_info != nullptr && vs_info->gs_prolog);
}


KYTY_RECOMPILER_FUNC(Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Ne, Gt, Ge, F, Le, T */
KYTY_RECOMPILER_FUNC(Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Le, Ge, F, Gt, Lt, T */
KYTY_RECOMPILER_FUNC(Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

static void append_cmpx_result(String8* dst_source, const String8& load0, const String8& load1, const String8& predicate,
                               const String8& index_str)
{
	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <predicate> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          %texec_<index> = OpLoad %uint %exec_lo
          %tmasked_<index> = OpBitwiseAnd %uint %t3_<index> %texec_<index>
          OpStore %exec_lo %tmasked_<index>
          OpStore %exec_hi %uint_0
          <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<predicate>", predicate)
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<index>", index_str);
}

/* XXX: Eq, Ne */
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1)
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

	// TODO() check VSKIP
	append_cmpx_result(dst_source, load0, load1, param[0], index_str);

	return true;
}

// VOPC compare-and-update-exec helper for unsigned 32-bit predicates.
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_XXX_U32_SmaskVsrc0Vsrc1)
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

	// TODO() check VSKIP
	append_cmpx_result(dst_source, load0, load1, param[0], index_str);

	return true;
}

// Ordered and unordered float predicates share the compare-and-update-exec path.
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	append_cmpx_result(dst_source, load0, load1, param[0], index_str);

	return true;
}

/* v_cmpx_f_f32: comparison is always false, so the execution mask is cleared. */
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_F_F32_SmaskVsrc0Vsrc1)
{
	static const char* text = R"(
          OpStore %exec_lo %uint_0
          OpStore %exec_hi %uint_0
          <execz>
)";
	String8 index_str = String8::FromPrintf("%u", index);
	*dst_source += String8(text).ReplaceStr("<execz>", EXECZ).ReplaceStr("<index>", index_str);
	return true;
}

/* v_cmpx_tru_f32: comparison is always true, so the execution mask is kept. */
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_Tru_F32_SmaskVsrc0Vsrc1)
{
	static const char* text = R"(
          <execz>
)";
	String8 index_str = String8::FromPrintf("%u", index);
	*dst_source += String8(text).ReplaceStr("<execz>", EXECZ).ReplaceStr("<index>", index_str);
	return true;
}

/* v_cmpx_o_f32: ordered — true when neither operand is NaN. */
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_O_F32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          %onan0_<index> = OpIsNan %bool %t0_<index>
          %onan1_<index> = OpIsNan %bool %t1_<index>
          %ounord_<index> = OpLogicalOr %bool %onan0_<index> %onan1_<index>
          %oord_<index> = OpLogicalNot %bool %ounord_<index>
          %osel_<index> = OpSelect %uint %oord_<index> %uint_1 %uint_0
          %oexec_<index> = OpLoad %uint %exec_lo
          %omasked_<index> = OpBitwiseAnd %uint %osel_<index> %oexec_<index>
          OpStore %exec_lo %omasked_<index>
          OpStore %exec_hi %uint_0
          <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cmpx_u_f32: unordered — true when either operand is NaN. */
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_U_F32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          %unan0_<index> = OpIsNan %bool %t0_<index>
          %unan1_<index> = OpIsNan %bool %t1_<index>
          %uunord_<index> = OpLogicalOr %bool %unan0_<index> %unan1_<index>
          %usel_<index> = OpSelect %uint %uunord_<index> %uint_1 %uint_0
          %uexec_<index> = OpLoad %uint %exec_lo
          %umasked_<index> = OpBitwiseAnd %uint %usel_<index> %uexec_<index>
          OpStore %exec_lo %umasked_<index>
          OpStore %exec_hi %uint_0
          <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VCndmaskB32_VdstVsrc0Vsrc1Smask2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));

	auto src_bool_value0 = operand_variable_to_str(inst.src[2], 0);
	auto src_bool_value1 = operand_variable_to_str(inst.src[2], 1);

	EXIT_NOT_IMPLEMENTED(src_bool_value0.type != SpirvType::Uint);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
    <load0>
    <load1>
    %t22_<index> = OpLoad %uint %<src0>
    %t23_<index> = OpLoad %uint %<src1> ; unused
    %tb_<index> = OpBitwiseAnd %uint %t22_<index> %uint_1
    %t2_<index> = OpINotEqual %bool %tb_<index> %uint_0
    %t3_<index> = OpSelect %float %t2_<index> %t1_<index> %t0_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t3_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<src0>", src_bool_value0.value)
	                   .ReplaceStr("<src1>", src_bool_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VCvtPkrtzF16F32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check DX10_CLAMP

	static const char* text = R"(
    <load0>
    <load1>
    ; Convert each source float to binary16 bits with round-toward-zero.
	    ; The shader ISA writes the packed value, not a float rounded by GLSL.
	    %tpk0_bits_<index> = OpBitcast %uint %t0_<index>
	    %tpk0_shr_sign_<index> = OpShiftRightLogical %uint %tpk0_bits_<index> %uint_16
	    %tpk0_sign_<index> = OpBitwiseAnd %uint %tpk0_shr_sign_<index> %uint_0x00008000
    %tpk0_exp_shift_<index> = OpShiftRightLogical %uint %tpk0_bits_<index> %uint_23
	    %tpk0_exp_<index> = OpBitwiseAnd %uint %tpk0_exp_shift_<index> %uint_255
    %tpk0_mant_<index> = OpBitwiseAnd %uint %tpk0_bits_<index> %uint_0x007fffff
    %tpk0_half_exp_<index> = OpISub %uint %tpk0_exp_<index> %uint_112
    %tpk0_normal_exp_<index> = OpShiftLeftLogical %uint %tpk0_half_exp_<index> %uint_10
    %tpk0_normal_mant_<index> = OpShiftRightLogical %uint %tpk0_mant_<index> %uint_13
    %tpk0_normal_payload_<index> = OpBitwiseOr %uint %tpk0_normal_exp_<index> %tpk0_normal_mant_<index>
    %tpk0_normal_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %tpk0_normal_payload_<index>
    %tpk0_mant_hidden_<index> = OpBitwiseOr %uint %tpk0_mant_<index> %uint_0x00800000
    %tpk0_sub_raw_shift_<index> = OpISub %uint %uint_126 %tpk0_exp_<index>
    %tpk0_exp_lt_103_<index> = OpULessThan %bool %tpk0_exp_<index> %uint_103
    %tpk0_exp_gt_112_<index> = OpUGreaterThan %bool %tpk0_exp_<index> %uint_112
    %tpk0_sub_shift_low_<index> = OpSelect %uint %tpk0_exp_lt_103_<index> %uint_31 %tpk0_sub_raw_shift_<index>
    %tpk0_sub_shift_<index> = OpSelect %uint %tpk0_exp_gt_112_<index> %uint_14 %tpk0_sub_shift_low_<index>
    %tpk0_sub_mant_<index> = OpShiftRightLogical %uint %tpk0_mant_hidden_<index> %tpk0_sub_shift_<index>
    %tpk0_subnormal_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %tpk0_sub_mant_<index>
	    %tpk0_mant_shift_<index> = OpShiftRightLogical %uint %tpk0_mant_<index> %uint_13
	    %tpk0_nan_payload_<index> = OpBitwiseOr %uint %tpk0_mant_shift_<index> %uint_0x00000200
    %tpk0_nan_exp_<index> = OpBitwiseOr %uint %uint_0x00007c00 %tpk0_nan_payload_<index>
    %tpk0_nan_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %tpk0_nan_exp_<index>
    %tpk0_inf_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %uint_0x00007c00
    %tpk0_max_finite_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %uint_0x00007bff
    %tpk0_mant_zero_<index> = OpIEqual %bool %tpk0_mant_<index> %uint_0
    %tpk0_special_<index> = OpSelect %uint %tpk0_mant_zero_<index> %tpk0_inf_<index> %tpk0_nan_<index>
    %tpk0_exp_le_112_<index> = OpULessThanEqual %bool %tpk0_exp_<index> %uint_112
    %tpk0_exp_ge_143_<index> = OpUGreaterThanEqual %bool %tpk0_exp_<index> %uint_143
    %tpk0_exp_eq_255_<index> = OpIEqual %bool %tpk0_exp_<index> %uint_255
    %tpk0_finite0_<index> = OpSelect %uint %tpk0_exp_le_112_<index> %tpk0_subnormal_<index> %tpk0_normal_<index>
    %tpk0_finite1_<index> = OpSelect %uint %tpk0_exp_lt_103_<index> %tpk0_sign_<index> %tpk0_finite0_<index>
    %tpk0_finite2_<index> = OpSelect %uint %tpk0_exp_ge_143_<index> %tpk0_max_finite_<index> %tpk0_finite1_<index>
	    %tpk0_exp_select_<index> = OpSelect %uint %tpk0_exp_eq_255_<index> %tpk0_special_<index> %tpk0_finite2_<index>
	    %tpk0_result_<index> = OpBitwiseAnd %uint %tpk0_exp_select_<index> %uint_0x0000ffff

	    %tpk1_bits_<index> = OpBitcast %uint %t1_<index>
	    %tpk1_shr_sign_<index> = OpShiftRightLogical %uint %tpk1_bits_<index> %uint_16
	    %tpk1_sign_<index> = OpBitwiseAnd %uint %tpk1_shr_sign_<index> %uint_0x00008000
    %tpk1_exp_shift_<index> = OpShiftRightLogical %uint %tpk1_bits_<index> %uint_23
	    %tpk1_exp_<index> = OpBitwiseAnd %uint %tpk1_exp_shift_<index> %uint_255
    %tpk1_mant_<index> = OpBitwiseAnd %uint %tpk1_bits_<index> %uint_0x007fffff
    %tpk1_half_exp_<index> = OpISub %uint %tpk1_exp_<index> %uint_112
    %tpk1_normal_exp_<index> = OpShiftLeftLogical %uint %tpk1_half_exp_<index> %uint_10
    %tpk1_normal_mant_<index> = OpShiftRightLogical %uint %tpk1_mant_<index> %uint_13
    %tpk1_normal_payload_<index> = OpBitwiseOr %uint %tpk1_normal_exp_<index> %tpk1_normal_mant_<index>
    %tpk1_normal_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %tpk1_normal_payload_<index>
    %tpk1_mant_hidden_<index> = OpBitwiseOr %uint %tpk1_mant_<index> %uint_0x00800000
    %tpk1_sub_raw_shift_<index> = OpISub %uint %uint_126 %tpk1_exp_<index>
    %tpk1_exp_lt_103_<index> = OpULessThan %bool %tpk1_exp_<index> %uint_103
    %tpk1_exp_gt_112_<index> = OpUGreaterThan %bool %tpk1_exp_<index> %uint_112
    %tpk1_sub_shift_low_<index> = OpSelect %uint %tpk1_exp_lt_103_<index> %uint_31 %tpk1_sub_raw_shift_<index>
    %tpk1_sub_shift_<index> = OpSelect %uint %tpk1_exp_gt_112_<index> %uint_14 %tpk1_sub_shift_low_<index>
    %tpk1_sub_mant_<index> = OpShiftRightLogical %uint %tpk1_mant_hidden_<index> %tpk1_sub_shift_<index>
    %tpk1_subnormal_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %tpk1_sub_mant_<index>
	    %tpk1_mant_shift_<index> = OpShiftRightLogical %uint %tpk1_mant_<index> %uint_13
	    %tpk1_nan_payload_<index> = OpBitwiseOr %uint %tpk1_mant_shift_<index> %uint_0x00000200
    %tpk1_nan_exp_<index> = OpBitwiseOr %uint %uint_0x00007c00 %tpk1_nan_payload_<index>
    %tpk1_nan_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %tpk1_nan_exp_<index>
    %tpk1_inf_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %uint_0x00007c00
    %tpk1_max_finite_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %uint_0x00007bff
    %tpk1_mant_zero_<index> = OpIEqual %bool %tpk1_mant_<index> %uint_0
    %tpk1_special_<index> = OpSelect %uint %tpk1_mant_zero_<index> %tpk1_inf_<index> %tpk1_nan_<index>
    %tpk1_exp_le_112_<index> = OpULessThanEqual %bool %tpk1_exp_<index> %uint_112
    %tpk1_exp_ge_143_<index> = OpUGreaterThanEqual %bool %tpk1_exp_<index> %uint_143
    %tpk1_exp_eq_255_<index> = OpIEqual %bool %tpk1_exp_<index> %uint_255
    %tpk1_finite0_<index> = OpSelect %uint %tpk1_exp_le_112_<index> %tpk1_subnormal_<index> %tpk1_normal_<index>
    %tpk1_finite1_<index> = OpSelect %uint %tpk1_exp_lt_103_<index> %tpk1_sign_<index> %tpk1_finite0_<index>
    %tpk1_finite2_<index> = OpSelect %uint %tpk1_exp_ge_143_<index> %tpk1_max_finite_<index> %tpk1_finite1_<index>
	    %tpk1_exp_select_<index> = OpSelect %uint %tpk1_exp_eq_255_<index> %tpk1_special_<index> %tpk1_finite2_<index>
	    %tpk1_result_<index> = OpBitwiseAnd %uint %tpk1_exp_select_<index> %uint_0x0000ffff

	    %tpk1_shifted_<index> = OpShiftLeftLogical %uint %tpk1_result_<index> %uint_16
	    %tpk_result_<index> = OpBitwiseOr %uint %tpk0_result_<index> %tpk1_shifted_<index>
    %t4_<index> = OpBitcast %float %tpk_result_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t4_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
        %tdst_packed_<index> = OpLoad %uint %<dst_packed>
        %tpacked_val_<index> = OpSelect %uint %exec_lo_b_<index> %tpk_result_<index> %tdst_packed_<index>
               OpStore %<dst_packed> %tpacked_val_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<dst_packed>", packed_half_shadow_to_str(inst.dst))
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("uint_103", spirv->GetConstantUint(103))
	                   .ReplaceStr("uint_112", spirv->GetConstantUint(112))
	                   .ReplaceStr("uint_126", spirv->GetConstantUint(126))
	                   .ReplaceStr("uint_143", spirv->GetConstantUint(143))
	                   .ReplaceStr("uint_255", spirv->GetConstantUint(255));

	return true;
}

/* Generalized f16 ALU: operands arrive packed as uint32 (low 16 bits carry the
 * binary16). Unpack to f32, apply the operation from param[0], repack and
 * store with EXEC predication. No Float16 capability required. */
KYTY_RECOMPILER_FUNC(Recompile_VF16_XXX_VdstVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

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
        %hf0v_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t0_<index>
        %hf1v_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t1_<index>
        %hf0_<index> = OpCompositeExtract %float %hf0v_<index> 0
        %hf1_<index> = OpCompositeExtract %float %hf1v_<index> 0
        <param0>
        %hpackv_<index> = OpExtInst %v2float %GLSL_std_450 PackHalf2x16 %t_<index> %t_<index>
        %hpack_<index> = OpCompositeExtract %uint %hpackv_<index> 0
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %hpack_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* Generalized packed-f16 compare: unpack both operands, compare as f32
 * (param[0]), and write the lane mask to the VCC pair. */
KYTY_RECOMPILER_FUNC(Recompile_VCmpF16_XXX_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

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
          %hc0v_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t0_<index>
          %hc1v_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t1_<index>
          %hc0_<index> = OpCompositeExtract %float %hc0v_<index> 0
          %hc1_<index> = OpCompositeExtract %float %hc1v_<index> 0
          %t2_<index> = <param> %bool %hc0_<index> %hc1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* Generalized f64 compare: operands are register pairs. Recombine each pair
 * into a double, compare (param[0]) and write the VCC lane mask. */
KYTY_RECOMPILER_FUNC(Recompile_VCmpF64_XXX_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0lo;
	String8 load0hi;
	String8 load1lo;
	String8 load1hi;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	if (!operand_load_uint(spirv, inst.src[0], "t0_lo_<index>", index_str, &load0lo, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t0_hi_<index>", index_str, &load0hi, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_lo_<index>", index_str, &load1lo, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_hi_<index>", index_str, &load1hi, 1))
	{
		return false;
	}

	static const char* text = R"(
          <load0lo>
          <load0hi>
          <load1lo>
          <load1hi>
          %d0v_<index> = OpCompositeConstruct %v2uint %t0_lo_<index> %t0_hi_<index>
          %d0u_<index> = OpBitcast %ulong %d0v_<index>
          %d0_<index> = OpBitcast %double %d0u_<index>
          %d1v_<index> = OpCompositeConstruct %v2uint %t1_lo_<index> %t1_hi_<index>
          %d1u_<index> = OpBitcast %ulong %d1v_<index>
          %d1_<index> = OpBitcast %double %d1u_<index>
          %t2_<index> = <param> %bool %d0_<index> %d1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0lo>", load0lo)
	                   .ReplaceStr("<load0hi>", load0hi)
	                   .ReplaceStr("<load1lo>", load1lo)
	                   .ReplaceStr("<load1hi>", load1hi)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_f32_f64: recombine the source register pair into a double, convert to
 * float, store to the single f32 destination. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtF32F64_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0lo;
	String8 load0hi;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_lo_<index>", index_str, &load0lo, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t0_hi_<index>", index_str, &load0hi, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0lo>
    <load0hi>
        %d0v_<index> = OpCompositeConstruct %v2uint %t0_lo_<index> %t0_hi_<index>
        %d0u_<index> = OpBitcast %ulong %d0v_<index>
        %d0_<index> = OpBitcast %double %d0u_<index>
        %t_<index> = OpConvertFToF %float %d0_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0lo>", load0lo)
	                   .ReplaceStr("<load0hi>", load0hi)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_f64_f32: convert the f32 source to double and store across the
 * destination register pair. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtF64F32_SVdst2SVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);
	EXIT_NOT_IMPLEMENTED(inst.dst.size != 2);

	auto dst_lo = operand_variable_to_str(inst.dst, 0);
	auto dst_hi = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_lo.type != SpirvType::Float || dst_hi.type != SpirvType::Float);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
        %d0_<index> = OpConvertFToF %double %t0_<index>
        %du_<index> = OpBitcast %ulong %d0_<index>
        %dv_<index> = OpBitcast %v2uint %du_<index>
        %lo_<index> = OpCompositeExtract %uint %dv_<index> 0
        %hi_<index> = OpCompositeExtract %uint %dv_<index> 1
        %lo_f_<index> = OpBitcast %float %lo_<index>
        %hi_f_<index> = OpBitcast %float %hi_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_lo_<index> = OpLoad %float %<dst_lo>
        %tdst_hi_<index> = OpLoad %float %<dst_hi>
        %tval_lo_<index> = OpSelect %float %exec_lo_b_<index> %lo_f_<index> %tdst_lo_<index>
        %tval_hi_<index> = OpSelect %float %exec_lo_b_<index> %hi_f_<index> %tdst_hi_<index>
               OpStore %<dst_lo> %tval_lo_<index>
               OpStore %<dst_hi> %tval_hi_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<dst_lo>", dst_lo.value)
	                   .ReplaceStr("<dst_hi>", dst_hi.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_i32_f64: double source pair to signed int. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtI32F64_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0lo;
	String8 load0hi;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_lo_<index>", index_str, &load0lo, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t0_hi_<index>", index_str, &load0hi, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0lo>
    <load0hi>
        %d0v_<index> = OpCompositeConstruct %v2uint %t0_lo_<index> %t0_hi_<index>
        %d0u_<index> = OpBitcast %ulong %d0v_<index>
        %d0_<index> = OpBitcast %double %d0u_<index>
        %i_<index> = OpConvertFToS %int %d0_<index>
        %t_<index> = OpBitcast %float %i_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0lo>", load0lo)
	                   .ReplaceStr("<load0hi>", load0hi)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_f64_i32: signed int source to double destination pair. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtF64I32_SVdst2SVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);
	EXIT_NOT_IMPLEMENTED(inst.dst.size != 2);

	auto dst_lo = operand_variable_to_str(inst.dst, 0);
	auto dst_hi = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_lo.type != SpirvType::Float || dst_hi.type != SpirvType::Float);

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
        %d0_<index> = OpConvertSToF %double %t0_<index>
        %du_<index> = OpBitcast %ulong %d0_<index>
        %dv_<index> = OpBitcast %v2uint %du_<index>
        %lo_<index> = OpCompositeExtract %uint %dv_<index> 0
        %hi_<index> = OpCompositeExtract %uint %dv_<index> 1
        %lo_f_<index> = OpBitcast %float %lo_<index>
        %hi_f_<index> = OpBitcast %float %hi_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_lo_<index> = OpLoad %float %<dst_lo>
        %tdst_hi_<index> = OpLoad %float %<dst_hi>
        %tval_lo_<index> = OpSelect %float %exec_lo_b_<index> %lo_f_<index> %tdst_lo_<index>
        %tval_hi_<index> = OpSelect %float %exec_lo_b_<index> %hi_f_<index> %tdst_hi_<index>
               OpStore %<dst_lo> %tval_lo_<index>
               OpStore %<dst_hi> %tval_hi_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<dst_lo>", dst_lo.value)
	                   .ReplaceStr("<dst_hi>", dst_hi.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_f16_f32: unpack the f16 source (low 16 bits of a uint32) to f32. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtF16F32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
        %hv_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t0_<index>
        %t_<index> = OpCompositeExtract %float %hv_<index> 0
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* Generalized f16 unary math (trunc/ceil/floor/rndne/rcp/sqrt): unpack to
 * f32, apply param[0], repack and store with EXEC predication. */
KYTY_RECOMPILER_FUNC(Recompile_VF16_Unary_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
        %hv_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t0_<index>
        %h0_<index> = OpCompositeExtract %float %hv_<index> 0
        <param0>
        %hpackv_<index> = OpExtInst %v2float %GLSL_std_450 PackHalf2x16 %t_<index> %t_<index>
        %hpack_<index> = OpCompositeExtract %uint %hpackv_<index> 0
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %hpack_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_f16_i16: sign-extend the i16 source (low 16 bits), convert to f32 and
 * pack as f16 into the destination. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtF16I16_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
        %i16_<index> = OpBitcast %int %t0_<index>
        %i16s_<index> = OpShiftLeftLogical %int %i16_<index> %int_16
        %i16x_<index> = OpShiftRightArithmetic %int %i16s_<index> %int_16
        %f_<index> = OpConvertSToF %float %i16x_<index>
        %hpackv_<index> = OpExtInst %v2float %GLSL_std_450 PackHalf2x16 %f_<index> %f_<index>
        %hpack_<index> = OpCompositeExtract %uint %hpackv_<index> 0
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %hpack_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_f16_u16: zero-extend the u16 source, convert to f32, pack as f16. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtF16U16_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
        %u16_<index> = OpBitwiseAnd %uint %t0_<index> %uint_0xffff
        %f_<index> = OpConvertUToF %float %u16_<index>
        %hpackv_<index> = OpExtInst %v2float %GLSL_std_450 PackHalf2x16 %f_<index> %f_<index>
        %hpack_<index> = OpCompositeExtract %uint %hpackv_<index> 0
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %hpack_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_i16_f16: unpack the f16 source and store the signed 16-bit truncation
 * in the low bits of the destination. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtI16F16_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
        %hv_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t0_<index>
        %h0_<index> = OpCompositeExtract %float %hv_<index> 0
        %i_<index> = OpConvertFToS %int %h0_<index>
        %u_<index> = OpBitcast %uint %i_<index>
        %masked_<index> = OpBitwiseAnd %uint %u_<index> %uint_0xffff
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %masked_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_u16_f16: unpack the f16 source and store the unsigned 16-bit
 * truncation in the low bits of the destination. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtU16F16_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
        %hv_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t0_<index>
        %h0_<index> = OpCompositeExtract %float %hv_<index> 0
        %u_<index> = OpConvertFToU %uint %h0_<index>
        %masked_<index> = OpBitwiseAnd %uint %u_<index> %uint_0xffff
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %masked_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_pk_u16_u32: truncate both u32 sources to u16 and pack into the
 * destination (src0 low, src1 high). */
KYTY_RECOMPILER_FUNC(Recompile_VCvtPkU16U32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

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
        %lo_<index> = OpBitwiseAnd %uint %t0_<index> %uint_0xffff
        %hi_<index> = OpBitwiseAnd %uint %t1_<index> %uint_0xffff
        %t_<index> = OpBitFieldInsert %uint %lo_<index> %hi_<index> %uint_16 %uint_16
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_pk_i16_i32: truncate both i32 sources to i16 and pack. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtPkI16I32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

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
        %lo_<index> = OpBitwiseAnd %uint %t0_<index> %uint_0xffff
        %hi_<index> = OpBitwiseAnd %uint %t1_<index> %uint_0xffff
        %t_<index> = OpBitFieldInsert %uint %lo_<index> %hi_<index> %uint_16 %uint_16
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_pknorm_u16_f32: clamp both f32 sources to [0,1], scale to u16 and
 * pack. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtPknormU16F32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const auto zero = spirv->GetConstantFloat(0.0f);
	const auto one  = spirv->GetConstantFloat(1.0f);
	if (zero == "unknown_float_constant" || one == "unknown_float_constant")
	{
		return false;
	}

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <load1>
        %c0_<index> = OpExtInst %float %GLSL_std_450 FClamp %t0_<index> <zero> <one>
        %c1_<index> = OpExtInst %float %GLSL_std_450 FClamp %t1_<index> <zero> <one>
        %u0_<index> = OpConvertFToU %uint %c0_<index>
        %u1_<index> = OpConvertFToU %uint %c1_<index>
        %lo_<index> = OpBitwiseAnd %uint %u0_<index> %uint_0xffff
        %hi_<index> = OpBitwiseAnd %uint %u1_<index> %uint_0xffff
        %t_<index> = OpBitFieldInsert %uint %lo_<index> %hi_<index> %uint_16 %uint_16
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<zero>", zero)
	                   .ReplaceStr("<one>", one)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_pknorm_i16_f32: clamp both f32 sources to [-1,1], scale to i16 and
 * pack. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtPknormI16F32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const auto neg_one = spirv->GetConstantFloat(-1.0f);
	const auto one     = spirv->GetConstantFloat(1.0f);
	if (neg_one == "unknown_float_constant" || one == "unknown_float_constant")
	{
		return false;
	}

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <load1>
        %c0_<index> = OpExtInst %float %GLSL_std_450 FClamp %t0_<index> <neg_one> <one>
        %c1_<index> = OpExtInst %float %GLSL_std_450 FClamp %t1_<index> <neg_one> <one>
        %i0_<index> = OpConvertFToS %int %c0_<index>
        %i1_<index> = OpConvertFToS %int %c1_<index>
        %u0_<index> = OpBitcast %uint %i0_<index>
        %u1_<index> = OpBitcast %uint %i1_<index>
        %lo_<index> = OpBitwiseAnd %uint %u0_<index> %uint_0xffff
        %hi_<index> = OpBitwiseAnd %uint %u1_<index> %uint_0xffff
        %t_<index> = OpBitFieldInsert %uint %lo_<index> %hi_<index> %uint_16 %uint_16
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<neg_one>", neg_one)
	                   .ReplaceStr("<one>", one)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_u32_f64: double source pair to unsigned int. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtU32F64_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0lo;
	String8 load0hi;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_lo_<index>", index_str, &load0lo, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t0_hi_<index>", index_str, &load0hi, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0lo>
    <load0hi>
        %d0v_<index> = OpCompositeConstruct %v2uint %t0_lo_<index> %t0_hi_<index>
        %d0u_<index> = OpBitcast %ulong %d0v_<index>
        %d0_<index> = OpBitcast %double %d0u_<index>
        %i_<index> = OpConvertFToU %uint %d0_<index>
        %t_<index> = OpBitcast %float %i_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0lo>", load0lo)
	                   .ReplaceStr("<load0hi>", load0hi)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_cvt_f64_u32: unsigned int source to double destination pair. */
KYTY_RECOMPILER_FUNC(Recompile_VCvtF64U32_SVdst2SVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);
	EXIT_NOT_IMPLEMENTED(inst.dst.size != 2);

	auto dst_lo = operand_variable_to_str(inst.dst, 0);
	auto dst_hi = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_lo.type != SpirvType::Float || dst_hi.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
        %d0_<index> = OpConvertUToF %double %t0_<index>
        %du_<index> = OpBitcast %ulong %d0_<index>
        %dv_<index> = OpBitcast %v2uint %du_<index>
        %lo_<index> = OpCompositeExtract %uint %dv_<index> 0
        %hi_<index> = OpCompositeExtract %uint %dv_<index> 1
        %lo_f_<index> = OpBitcast %float %lo_<index>
        %hi_f_<index> = OpBitcast %float %hi_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_lo_<index> = OpLoad %float %<dst_lo>
        %tdst_hi_<index> = OpLoad %float %<dst_hi>
        %tval_lo_<index> = OpSelect %float %exec_lo_b_<index> %lo_f_<index> %tdst_lo_<index>
        %tval_hi_<index> = OpSelect %float %exec_lo_b_<index> %hi_f_<index> %tdst_hi_<index>
               OpStore %<dst_lo> %tval_lo_<index>
               OpStore %<dst_hi> %tval_hi_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<dst_lo>", dst_lo.value)
	                   .ReplaceStr("<dst_hi>", dst_hi.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* Generalized f64 unary math (trunc/ceil/floor/rndne/rcp/rsq/sqrt/fract):
 * recombine the source pair into a double, apply param[0], split the result
 * back into the destination pair. */
KYTY_RECOMPILER_FUNC(Recompile_VF64_Unary_SVdst2SVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0lo;
	String8 load0hi;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);
	EXIT_NOT_IMPLEMENTED(inst.dst.size != 2);

	auto dst_lo = operand_variable_to_str(inst.dst, 0);
	auto dst_hi = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_lo.type != SpirvType::Float || dst_hi.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_lo_<index>", index_str, &load0lo, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t0_hi_<index>", index_str, &load0hi, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0lo>
    <load0hi>
        %d0v_<index> = OpCompositeConstruct %v2uint %t0_lo_<index> %t0_hi_<index>
        %d0u_<index> = OpBitcast %ulong %d0v_<index>
        %d0_<index> = OpBitcast %double %d0u_<index>
        <param0>
        %du_<index> = OpBitcast %ulong %t_<index>
        %dv_<index> = OpBitcast %v2uint %du_<index>
        %lo_<index> = OpCompositeExtract %uint %dv_<index> 0
        %hi_<index> = OpCompositeExtract %uint %dv_<index> 1
        %lo_f_<index> = OpBitcast %float %lo_<index>
        %hi_f_<index> = OpBitcast %float %hi_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_lo_<index> = OpLoad %float %<dst_lo>
        %tdst_hi_<index> = OpLoad %float %<dst_hi>
        %tval_lo_<index> = OpSelect %float %exec_lo_b_<index> %lo_f_<index> %tdst_lo_<index>
        %tval_hi_<index> = OpSelect %float %exec_lo_b_<index> %hi_f_<index> %tdst_hi_<index>
               OpStore %<dst_lo> %tval_lo_<index>
               OpStore %<dst_hi> %tval_hi_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0lo>", load0lo)
	                   .ReplaceStr("<load0hi>", load0hi)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<dst_lo>", dst_lo.value)
	                   .ReplaceStr("<dst_hi>", dst_hi.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* v_fract_f64: frac(x) = x - floor(x). */
KYTY_RECOMPILER_FUNC(Recompile_VFractF64_SVdst2SVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0lo;
	String8 load0hi;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);
	EXIT_NOT_IMPLEMENTED(inst.dst.size != 2);

	auto dst_lo = operand_variable_to_str(inst.dst, 0);
	auto dst_hi = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_lo.type != SpirvType::Float || dst_hi.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_lo_<index>", index_str, &load0lo, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t0_hi_<index>", index_str, &load0hi, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0lo>
    <load0hi>
        %d0v_<index> = OpCompositeConstruct %v2uint %t0_lo_<index> %t0_hi_<index>
        %d0u_<index> = OpBitcast %ulong %d0v_<index>
        %d0_<index> = OpBitcast %double %d0u_<index>
        %dfl_<index> = OpExtInst %double %GLSL_std_450 Floor %d0_<index>
        %t_<index> = OpFSub %double %d0_<index> %dfl_<index>
        %du_<index> = OpBitcast %ulong %t_<index>
        %dv_<index> = OpBitcast %v2uint %du_<index>
        %lo_<index> = OpCompositeExtract %uint %dv_<index> 0
        %hi_<index> = OpCompositeExtract %uint %dv_<index> 1
        %lo_f_<index> = OpBitcast %float %lo_<index>
        %hi_f_<index> = OpBitcast %float %hi_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_lo_<index> = OpLoad %float %<dst_lo>
        %tdst_hi_<index> = OpLoad %float %<dst_hi>
        %tval_lo_<index> = OpSelect %float %exec_lo_b_<index> %lo_f_<index> %tdst_lo_<index>
        %tval_hi_<index> = OpSelect %float %exec_lo_b_<index> %hi_f_<index> %tdst_hi_<index>
               OpStore %<dst_lo> %tval_lo_<index>
               OpStore %<dst_hi> %tval_hi_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0lo>", load0lo)
	                   .ReplaceStr("<load0hi>", load0hi)
	                   .ReplaceStr("<dst_lo>", dst_lo.value)
	                   .ReplaceStr("<dst_hi>", dst_hi.value)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* Generalized packed-i16 compare: sign-extend both operands from the low 16
 * bits, compare as i32 (param[0]) and write the VCC lane mask. */
KYTY_RECOMPILER_FUNC(Recompile_VCmpI16_XXX_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

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
          %i0s_<index> = OpShiftLeftLogical %uint %t0_<index> %uint_16
          %i0x_<index> = OpShiftRightArithmetic %uint %i0s_<index> %uint_16
          %i1s_<index> = OpShiftLeftLogical %uint %t1_<index> %uint_16
          %i1x_<index> = OpShiftRightArithmetic %uint %i1s_<index> %uint_16
          %t2_<index> = <param> %bool %i0x_<index> %i1x_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* Generalized packed-u16 compare: zero-extend both operands from the low 16
 * bits, compare as u32 (param[0]) and write the VCC lane mask. */
KYTY_RECOMPILER_FUNC(Recompile_VCmpU16_XXX_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

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
          %i0x_<index> = OpBitwiseAnd %uint %t0_<index> %uint_0xffff
          %i1x_<index> = OpBitwiseAnd %uint %t1_<index> %uint_0xffff
          %t2_<index> = <param> %bool %i0x_<index> %i1x_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);
	return true;
}

/* Generalized packed-i16/u16 compare-to-exec: extend both operands (signed
 * when param[1]=="s", zero otherwise), compare (param[0]) and update the EXEC
 * mask like v_cmpx. */
KYTY_RECOMPILER_FUNC(Recompile_VCmpx16_XXX_SmaskVsrc0Vsrc1)
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
          <extend0>
          <extend1>
          %t2_<index> = <predicate> %bool %i0x_<index> %i1x_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          %texec_<index> = OpLoad %uint %exec_lo
          %tmasked_<index> = OpBitwiseAnd %uint %t3_<index> %texec_<index>
          OpStore %exec_lo %tmasked_<index>
          OpStore %exec_hi %uint_0
          <execz>
)";
	const bool is_signed = (param[1] != nullptr && strcmp(param[1], "s") == 0);
	String8 extend;
	if (is_signed)
	{
		extend = R"(
          %i0s_<index> = OpShiftLeftLogical %uint %t0_<index> %uint_16
          %i0x_<index> = OpShiftRightArithmetic %uint %i0s_<index> %uint_16
          %i1s_<index> = OpShiftLeftLogical %uint %t1_<index> %uint_16
          %i1x_<index> = OpShiftRightArithmetic %uint %i1s_<index> %uint_16
)";
	} else
	{
		extend = R"(
          %i0x_<index> = OpBitwiseAnd %uint %t0_<index> %uint_0xffff
          %i1x_<index> = OpBitwiseAnd %uint %t1_<index> %uint_0xffff
)";
	}
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<extend0>", extend)
	                   .ReplaceStr("<extend1>", "")
	                   .ReplaceStr("<predicate>", param[0])
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VInterpP1F32_VdstVsrcAttrChan)
{
	return true;
}

static bool RecompilePixelInterpolatorLoad(Spirv* spirv, const ShaderPixelInputInfo* ps_info, uint32_t input, uint32_t component,
                                           const String8& index_str, const String8& dst, String8* dst_source)
{
	EXIT_IF(spirv == nullptr);
	EXIT_IF(ps_info == nullptr);
	EXIT_IF(dst_source == nullptr);
	EXIT_NOT_IMPLEMENTED(input >= ps_info->input_num);
	EXIT_NOT_IMPLEMENTED(component >= 4u);

	ShaderPixelInterpolator interpolator {};
	EXIT_NOT_IMPLEMENTED(!ShaderDecodePixelInterpolator(ps_info->interpolator_settings[input], &interpolator));

	if (interpolator.source == ShaderPixelInterpolatorSource::Default)
	{
		const auto value = spirv->GetConstantFloat(ShaderPixelInterpolatorDefaultComponent(interpolator, component));
		EXIT_IF(value == "unknown_float_constant");

		static const char* default_text = R"(
                       OpStore %<dst> %<value>
)";
		*dst_source += String8(default_text).ReplaceStr("<dst>", dst).ReplaceStr("<value>", value);
		return true;
	}

	const uint32_t attr = ShaderPixelCanonicalInterpolator(*ps_info, input);
	String8 load0 = String8::FromPrintf("%%t0_<index> = OpAccessChain %%_ptr_Input_float %%attr%u %%uint_%u", attr, component);

	static const char* parameter_text = R"(
         <load0>
         %t1_<index> = OpLoad %float %t0_<index>
                       OpStore %<dst> %t1_<index>
)";
	*dst_source += String8(parameter_text)
	                   .ReplaceStr("<dst>", dst)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<index>", index_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VInterpP2F32_VdstVsrcAttrChan)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

	auto dst_value = operand_variable_to_str(inst.dst);

	const auto* ps_info = spirv->GetPsInputInfo();
	EXIT_IF(ps_info == nullptr);
	return RecompilePixelInterpolatorLoad(spirv, ps_info, inst.src[1].constant.u, inst.src[2].constant.u, index_str, dst_value.value,
	                                      dst_source);
}

KYTY_RECOMPILER_FUNC(Recompile_VInterpMovF32_VdstVsrcAttrChan)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

	EXIT_NOT_IMPLEMENTED(inst.src[0].constant.u != 2);

	auto dst_value = operand_variable_to_str(inst.dst);

	const auto* ps_info = spirv->GetPsInputInfo();
	EXIT_IF(ps_info == nullptr);
	return RecompilePixelInterpolatorLoad(spirv, ps_info, inst.src[1].constant.u, inst.src[2].constant.u, index_str, dst_value.value,
	                                      dst_source);
}

/* XXX: Mad, Madak, Madmk, Max3, Min3, Med3, Fma */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load2;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	// EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	// EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check SP_ROUND
	// TODO() check DX10_CLAMP
	// TODO() check IEEE

	static const char* text = R"(
              <load0>
              <load1>
              <load2>
              <param0>
              <param1>
              <param2>
              <param3>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VDot2cF32F16_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	for (int source = 0; source < 3; ++source)
	{
		EXIT_NOT_IMPLEMENTED(inst.src[source].negate || inst.src[source].absolute || inst.src[source].swizzle != 6u);
	}

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	String8       load1;
	String8       load2;
	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1) ||
	    !operand_load_float(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <load2>
        %u0_<index> = OpBitcast %uint %t0_<index>
        %u1_<index> = OpBitcast %uint %t1_<index>
        %h0_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %u0_<index>
        %h1_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %u1_<index>
        %h00_<index> = OpCompositeExtract %float %h0_<index> 0
        %h01_<index> = OpCompositeExtract %float %h0_<index> 1
        %h10_<index> = OpCompositeExtract %float %h1_<index> 0
        %h11_<index> = OpCompositeExtract %float %h1_<index> 1
        %p0_<index> = OpFMul %float %h00_<index> %h10_<index>
        %p1_<index> = OpFMul %float %h01_<index> %h11_<index>
        %sum_<index> = OpFAdd %float %p0_<index> %p1_<index>
        %t_<index> = OpFAdd %float %sum_<index> %t2_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VDot4cI32I8_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);
	for (int source = 0; source < 3; ++source)
	{
		EXIT_NOT_IMPLEMENTED(inst.src[source].negate || inst.src[source].absolute || inst.src[source].swizzle != 6u);
	}

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	String8       load1;
	String8       load2;
	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1) ||
	    !operand_load_int(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <load2>
        %i00_<index> = OpBitFieldSExtract %int %t0_<index> %int_0 %int_8
        %i01_<index> = OpBitFieldSExtract %int %t0_<index> %int_8 %int_8
        %i02_<index> = OpBitFieldSExtract %int %t0_<index> %int_16 %int_8
        %i03_<index> = OpBitFieldSExtract %int %t0_<index> %int_24 %int_8
        %i10_<index> = OpBitFieldSExtract %int %t1_<index> %int_0 %int_8
        %i11_<index> = OpBitFieldSExtract %int %t1_<index> %int_8 %int_8
        %i12_<index> = OpBitFieldSExtract %int %t1_<index> %int_16 %int_8
        %i13_<index> = OpBitFieldSExtract %int %t1_<index> %int_24 %int_8
        %p0_<index> = OpIMul %int %i00_<index> %i10_<index>
        %p1_<index> = OpIMul %int %i01_<index> %i11_<index>
        %p2_<index> = OpIMul %int %i02_<index> %i12_<index>
        %p3_<index> = OpIMul %int %i03_<index> %i13_<index>
        %s0_<index> = OpIAdd %int %p0_<index> %p1_<index>
        %s1_<index> = OpIAdd %int %p2_<index> %p3_<index>
        %sum_<index> = OpIAdd %int %s0_<index> %s1_<index>
        %t_<index> = OpIAdd %int %sum_<index> %t2_<index>
        %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %tf_<index>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

// v_cubetc_f32 calculates the T coordinate for cube-map lookup. The major
// axis selection and signs follow the GCN cube-coordinate contract; it is not
// interchangeable with a plain component move.
KYTY_RECOMPILER_FUNC(Recompile_VCubetcF32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	String8       load1;
	String8       load2;
	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1) ||
	    !operand_load_float(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <load2>
        %abs0_<index> = OpExtInst %float %GLSL_std_450 FAbs %t0_<index>
        %abs1_<index> = OpExtInst %float %GLSL_std_450 FAbs %t1_<index>
        %abs2_<index> = OpExtInst %float %GLSL_std_450 FAbs %t2_<index>
      %maxxy_<index> = OpExtInst %float %GLSL_std_450 FMax %abs0_<index> %abs1_<index>
       %zmax_<index> = OpFOrdGreaterThanEqual %bool %abs2_<index> %maxxy_<index>
        %yge_<index> = OpFOrdGreaterThanEqual %bool %abs1_<index> %abs0_<index>
       %notz_<index> = OpLogicalNot %bool %zmax_<index>
       %ymax_<index> = OpLogicalAnd %bool %notz_<index> %yge_<index>
       %yneg_<index> = OpFOrdLessThan %bool %t1_<index> %float_0_000000
       %neg2_<index> = OpFNegate %float %t2_<index>
        %yt_<index> = OpSelect %float %yneg_<index> %neg2_<index> %t2_<index>
       %neg1_<index> = OpFNegate %float %t1_<index>
         %t_<index> = OpSelect %float %ymax_<index> %yt_<index> %neg1_<index>
  %exec_lo_u_<index> = OpLoad %uint %exec_lo
  %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
    %dst_old_<index> = OpLoad %float %<dst>
        %dst_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %dst_old_<index>
               OpStore %<dst> %dst_<index>
)";
	*dst_source += String8(text).ReplaceStr("<load0>", load0).ReplaceStr("<load1>", load1).ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);
	return true;
}

// v_cubesc_f32 calculates the cube-map S coordinate from the dominant axis.
KYTY_RECOMPILER_FUNC(Recompile_VCubescF32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const String8 index_str = String8::FromPrintf("%u", index);
	String8 load0;
	String8 load1;
	String8 load2;
	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1) ||
	    !operand_load_float(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <load2>
        %abs0_<index> = OpExtInst %float %GLSL_std_450 FAbs %t0_<index>
        %abs1_<index> = OpExtInst %float %GLSL_std_450 FAbs %t1_<index>
        %abs2_<index> = OpExtInst %float %GLSL_std_450 FAbs %t2_<index>
      %maxxy_<index> = OpExtInst %float %GLSL_std_450 FMax %abs0_<index> %abs1_<index>
       %zmax_<index> = OpFOrdGreaterThanEqual %bool %abs2_<index> %maxxy_<index>
        %yge_<index> = OpFOrdGreaterThanEqual %bool %abs1_<index> %abs0_<index>
       %notz_<index> = OpLogicalNot %bool %zmax_<index>
       %ymax_<index> = OpLogicalAnd %bool %notz_<index> %yge_<index>
       %zneg_<index> = OpFOrdLessThan %bool %t2_<index> %float_0_000000
       %xneg_<index> = OpFOrdLessThan %bool %t0_<index> %float_0_000000
       %neg0_<index> = OpFNegate %float %t0_<index>
       %neg2_<index> = OpFNegate %float %t2_<index>
      %zcase_<index> = OpSelect %float %zneg_<index> %neg0_<index> %t0_<index>
      %xcase_<index> = OpSelect %float %xneg_<index> %t2_<index> %neg2_<index>
       %nonz_<index> = OpSelect %float %ymax_<index> %t0_<index> %xcase_<index>
         %t_<index> = OpSelect %float %zmax_<index> %zcase_<index> %nonz_<index>
  %exec_lo_u_<index> = OpLoad %uint %exec_lo
  %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
    %dst_old_<index> = OpLoad %float %<dst>
        %dst_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %dst_old_<index>
               OpStore %<dst> %dst_<index>
)";
	*dst_source += String8(text).ReplaceStr("<load0>", load0).ReplaceStr("<load1>", load1).ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);
	return true;
}

// v_cubeid_f32 selects the cube face (±X, ±Y, ±Z) using the same dominant
// axis ordering as the coordinate transforms.
KYTY_RECOMPILER_FUNC(Recompile_VCubeIdF32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
	const String8 index_str = String8::FromPrintf("%u", index);
	String8 load0;
	String8 load1;
	String8 load2;
	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1) ||
	    !operand_load_float(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}
	static const char* text = R"(
              <load0>
              <load1>
              <load2>
        %abs0_<index> = OpExtInst %float %GLSL_std_450 FAbs %t0_<index>
        %abs1_<index> = OpExtInst %float %GLSL_std_450 FAbs %t1_<index>
        %abs2_<index> = OpExtInst %float %GLSL_std_450 FAbs %t2_<index>
      %maxxy_<index> = OpExtInst %float %GLSL_std_450 FMax %abs0_<index> %abs1_<index>
       %zmax_<index> = OpFOrdGreaterThanEqual %bool %abs2_<index> %maxxy_<index>
        %yge_<index> = OpFOrdGreaterThanEqual %bool %abs1_<index> %abs0_<index>
       %zneg_<index> = OpFOrdLessThan %bool %t2_<index> %float_0_000000
       %yneg_<index> = OpFOrdLessThan %bool %t1_<index> %float_0_000000
       %xneg_<index> = OpFOrdLessThan %bool %t0_<index> %float_0_000000
      %zcase_<index> = OpSelect %float %zneg_<index> %float_5_000000 %float_4_000000
      %ycase_<index> = OpSelect %float %yneg_<index> %float_3_000000 %float_2_000000
      %xcase_<index> = OpSelect %float %xneg_<index> %float_1_000000 %float_0_000000
     %xycase_<index> = OpSelect %float %yge_<index> %ycase_<index> %xcase_<index>
         %t_<index> = OpSelect %float %zmax_<index> %zcase_<index> %xycase_<index>
  %exec_lo_u_<index> = OpLoad %uint %exec_lo
  %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
    %dst_old_<index> = OpLoad %float %<dst>
        %dst_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %dst_old_<index>
               OpStore %<dst> %dst_<index>
)";
	*dst_source += String8(text).ReplaceStr("<load0>", load0).ReplaceStr("<load1>", load1).ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);
	return true;
}

// v_cubema_f32 returns the cube-map major-axis scale: 2 * max(abs(x),
// abs(y), abs(z)). It shares the same three-source VOP3 form as v_cubetc.
KYTY_RECOMPILER_FUNC(Recompile_VCubeMaF32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	String8       load1;
	String8       load2;
	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1) ||
	    !operand_load_float(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <load2>
        %abs0_<index> = OpExtInst %float %GLSL_std_450 FAbs %t0_<index>
        %abs1_<index> = OpExtInst %float %GLSL_std_450 FAbs %t1_<index>
        %abs2_<index> = OpExtInst %float %GLSL_std_450 FAbs %t2_<index>
      %maxxy_<index> = OpExtInst %float %GLSL_std_450 FMax %abs0_<index> %abs1_<index>
       %maxxyz_<index> = OpExtInst %float %GLSL_std_450 FMax %abs2_<index> %maxxy_<index>
         %t_<index> = OpFMul %float %float_2_000000 %maxxyz_<index>
  %exec_lo_u_<index> = OpLoad %uint %exec_lo
  %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
    %dst_old_<index> = OpLoad %float %<dst>
        %dst_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %dst_old_<index>
               OpStore %<dst> %dst_<index>
)";
	*dst_source += String8(text).ReplaceStr("<load0>", load0).ReplaceStr("<load1>", load1).ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);
	return true;
}

static bool RecompileFragmentMbcnt(const ShaderInstruction& inst, uint32_t index, bool low_half, Spirv* spirv, String8* dst_source)
{
	EXIT_IF(spirv == nullptr || dst_source == nullptr);
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp || inst.dst.multiplier != 1.0f);

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const String8 index_str = String8::FromPrintf("%u", index);
	String8      mask_load;
	String8      accumulator_load;
	if (!operand_load_uint(spirv, inst.src[0], "mbcnt_mask_<index>", index_str, &mask_load) ||
	    !operand_load_uint(spirv, inst.src[1], "mbcnt_acc_<index>", index_str, &accumulator_load))
	{
		return false;
	}

	static const char* text = R"(
        <mask_load>
        <accumulator_load>
        %mbcnt_lane_<index> = OpLoad %uint %gl_SubgroupInvocationID
        %mbcnt_half_<index> = <half_test>
        %mbcnt_lane_bit_<index> = <lane_bit>
        %mbcnt_mask_bit_<index> = OpShiftLeftLogical %uint %uint_1 %mbcnt_lane_bit_<index>
        %mbcnt_masked_<index> = OpBitwiseAnd %uint %mbcnt_mask_<index> %mbcnt_mask_bit_<index>
        %mbcnt_source_active_<index> = OpINotEqual %bool %mbcnt_masked_<index> %uint_0
        %mbcnt_selected_<index> = OpSelect %uint %mbcnt_source_active_<index> %uint_1 %uint_0
        %mbcnt_prefix_<index> = OpGroupNonUniformIAdd %uint %uint_3 ExclusiveScan %mbcnt_selected_<index>
        %mbcnt_result_<index> = OpIAdd %uint %mbcnt_acc_<index> %mbcnt_prefix_<index>
        %mbcnt_result_float_<index> = OpBitcast %float %mbcnt_result_<index>
        %mbcnt_exec_lane_lt32_<index> = OpULessThan %bool %mbcnt_lane_<index> %uint_32
        %mbcnt_exec_lane_bit_<index> = OpBitwiseAnd %uint %mbcnt_lane_<index> %uint_31
        %mbcnt_exec_word_lo_<index> = OpLoad %uint %exec_lo
        %mbcnt_exec_word_hi_<index> = OpLoad %uint %exec_hi
        %mbcnt_exec_word_<index> = OpSelect %uint %mbcnt_exec_lane_lt32_<index> %mbcnt_exec_word_lo_<index> %mbcnt_exec_word_hi_<index>
        %mbcnt_exec_mask_<index> = OpShiftLeftLogical %uint %uint_1 %mbcnt_exec_lane_bit_<index>
        %mbcnt_exec_masked_<index> = OpBitwiseAnd %uint %mbcnt_exec_word_<index> %mbcnt_exec_mask_<index>
        %mbcnt_exec_active_<index> = OpINotEqual %bool %mbcnt_exec_masked_<index> %uint_0
        %mbcnt_old_<index> = OpLoad %float %<dst>
        %mbcnt_value_<index> = OpSelect %float %mbcnt_exec_active_<index> %mbcnt_result_float_<index> %mbcnt_old_<index>
               OpStore %<dst> %mbcnt_value_<index>
    )";

	String8 half_test;
	String8 lane_bit;
	if (low_half)
	{
		half_test = String8("OpULessThan %bool %mbcnt_lane_<index> %uint_32");
		lane_bit  = String8("OpBitwiseAnd %uint %mbcnt_lane_<index> %uint_31");
	} else
	{
		half_test = String8("OpUGreaterThanEqual %bool %mbcnt_lane_<index> %uint_32");
		lane_bit  = String8("OpBitwiseAnd %uint %mbcnt_lane_offset_<index> %uint_31");
	}

	String8 source = String8(text)
	                    .ReplaceStr("<mask_load>", mask_load)
	                    .ReplaceStr("<accumulator_load>", accumulator_load)
	                    .ReplaceStr("<half_test>", half_test)
	                    .ReplaceStr("<lane_bit>", lane_bit)
	                    .ReplaceStr("<dst>", dst_value.value)
	                    .ReplaceStr("<index>", index_str);
	if (!low_half)
	{
		const String8 offset = String8("        %mbcnt_lane_offset_<index> = OpISub %uint %mbcnt_lane_<index> %uint_32\n")
		                           .ReplaceStr("<index>", index_str);
		source = offset + source;
	}

	*dst_source += source;
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VMbcntHiU32B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	if (code.GetType() != ShaderType::Pixel)
	{
		return false;
	}
	return RecompileFragmentMbcnt(inst, index, false, spirv, dst_source);
}

KYTY_RECOMPILER_FUNC(Recompile_VMbcntLoU32B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	if (code.GetType() != ShaderType::Pixel)
	{
		return false;
	}
	return RecompileFragmentMbcnt(inst, index, true, spirv, dst_source);
}

/* XXX: Bfrev, Not */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_B32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
              <load0>
              <param0>
              %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VNop)
{
	return true;
}

// v_readfirstlane_b32: SGPR := VGPR[first guest-EXEC-active lane].
// Guest EXEC is a software mask, so SPIR-V "active" alone is insufficient;
// ballot the per-lane EXEC bit and broadcast from its lowest set lane.
KYTY_RECOMPILER_FUNC(Recompile_VReadfirstlaneB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.src_num < 1);

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);

	String8 index_str = String8::FromPrintf("%u", index);
	String8 load0;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
        <load0>
        %rfl_lane_<index> = OpLoad %uint %gl_SubgroupInvocationID
        %rfl_exec_lo_<index> = OpLoad %uint %exec_lo
        %rfl_exec_hi_<index> = OpLoad %uint %exec_hi
        %rfl_lane_lt32_<index> = OpULessThan %bool %rfl_lane_<index> %uint_32
        %rfl_lane_mod_<index> = OpBitwiseAnd %uint %rfl_lane_<index> %uint_31
        %rfl_exec_word_<index> = OpSelect %uint %rfl_lane_lt32_<index> %rfl_exec_lo_<index> %rfl_exec_hi_<index>
        %rfl_bit_<index> = OpShiftLeftLogical %uint %uint_1 %rfl_lane_mod_<index>
        %rfl_masked_<index> = OpBitwiseAnd %uint %rfl_exec_word_<index> %rfl_bit_<index>
        %rfl_active_<index> = OpINotEqual %bool %rfl_masked_<index> %uint_0
        %rfl_ballot_<index> = OpGroupNonUniformBallot %v4uint %uint_3 %rfl_active_<index>
        %rfl_any_<index> = OpGroupNonUniformBallotBitCount %uint %uint_3 Reduce %rfl_ballot_<index>
        %rfl_empty_<index> = OpIEqual %bool %rfl_any_<index> %uint_0
        %rfl_first_<index> = OpGroupNonUniformBallotFindLSB %uint %uint_3 %rfl_ballot_<index>
        %rfl_lane_sel_<index> = OpSelect %uint %rfl_empty_<index> %uint_0 %rfl_first_<index>
        %rfl_value_<index> = OpGroupNonUniformBroadcast %uint %uint_3 %t0_<index> %rfl_lane_sel_<index>
        %rfl_result_<index> = OpSelect %uint %rfl_empty_<index> %uint_0 %rfl_value_<index>
               OpStore %<dst> %rfl_result_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

// v_readlane_b32 ignores EXEC and reads the requested physical wave lane.
KYTY_RECOMPILER_FUNC(Recompile_VReadlaneB32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);

	int spill_register = 0;
	int spill_lane     = 0;
	if (IsStaticScalarSpillRead(inst, &spill_register, &spill_lane) &&
	    HasLiveScalarSpill(code, index, spill_register, spill_lane))
	{
		const String8 slot       = ScalarSpillSlotName(spill_register, spill_lane);
		const String8 index_str  = String8::FromPrintf("%u", index);
		static const char* text = R"(
        %readlane_spill_<index> = OpLoad %float %<slot>
        %readlane_value_<index> = OpBitcast %uint %readlane_spill_<index>
               OpStore %<dst> %readlane_value_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<dst>", dst_value.value)
		                   .ReplaceStr("<slot>", slot)
		                   .ReplaceStr("<index>", index_str);
		return true;
	}
	if (IsStaticScalarSpillRead(inst, &spill_register, &spill_lane) &&
	    HasInvalidatedScalarSpill(code, index, spill_register, spill_lane))
	{
		return false;
	}

	String8 index_str = String8::FromPrintf("%u", index);
	String8 load0;
	String8 load1;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
        <load0>
        <load1>
        %readlane_<index> = OpGroupNonUniformShuffle %uint %uint_3 %t0_<index> %t1_<index>
               OpStore %<dst> %readlane_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

// v_writelane_b32 updates only the invocation whose physical wave lane matches the selector.
KYTY_RECOMPILER_FUNC(Recompile_VWritelaneB32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 index_str = String8::FromPrintf("%u", index);
	String8 load0;
	String8 load1;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	int spill_register = 0;
	int spill_lane     = 0;
	if (IsStaticScalarSpillWrite(inst, &spill_register, &spill_lane) &&
	    HasFutureScalarSpillRead(code, index, spill_register, spill_lane))
	{
		const String8 slot = ScalarSpillSlotName(spill_register, spill_lane);
		static const char* text = R"(
        <load0>
        %writelane_spill_<index> = OpBitcast %float %t0_<index>
               OpStore %<slot> %writelane_spill_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<load0>", load0)
		                   .ReplaceStr("<slot>", slot)
		                   .ReplaceStr("<index>", index_str);
		return true;
	}

	static const char* text = R"(
        <load0>
        <load1>
        %writelane_id_<index> = OpLoad %uint %gl_SubgroupInvocationID
        %writelane_match_<index> = OpIEqual %bool %writelane_id_<index> %t1_<index>
        %writelane_value_<index> = OpBitcast %float %t0_<index>
        %writelane_old_<index> = OpLoad %float %<dst>
        %writelane_result_<index> = OpSelect %float %writelane_match_<index> %writelane_value_<index> %writelane_old_<index>
               OpStore %<dst> %writelane_result_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VMovB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!inst.src[0].dpp)
	{
		if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
		{
			return false;
		}
	} else
	{
		// DPP quad-perm controls (0x000-0x0ff) select a lane within each
		// four-lane quad. Full masks make every selected lane valid; bound_ctrl
		// does not alter quad-perm routing. Other DPP modes require distinct
		// row/bank semantics and must not silently become same-lane reads.
		EXIT_NOT_IMPLEMENTED(inst.src[0].dpp_ctrl > 0xffu || inst.src[0].dpp_row_mask != 0xfu ||
		                     inst.src[0].dpp_bank_mask != 0xfu || inst.src[0].dpp_fetch_inactive);

		if (!operand_load_float(spirv, inst.src[0], "dpp_src_<index>", index_str, &load0))
		{
			return false;
		}

		const auto ctrl      = spirv->GetConstantUint(inst.src[0].dpp_ctrl);
		const auto quad_mask = spirv->GetConstantUint(0xfffffffcu);
		const auto uint_1    = spirv->GetConstantUint(1u);
		const auto uint_3    = spirv->GetConstantUint(3u);

		static const char* dpp_quad_permute = R"(
          %dpp_src_u_<index> = OpBitcast %uint %dpp_src_<index>
          %dpp_lane_<index> = OpLoad %uint %gl_SubgroupInvocationID
     %dpp_quad_base_<index> = OpBitwiseAnd %uint %dpp_lane_<index> %<quad_mask>
     %dpp_quad_lane_<index> = OpBitwiseAnd %uint %dpp_lane_<index> %<uint_3>
    %dpp_quad_shift_<index> = OpShiftLeftLogical %uint %dpp_quad_lane_<index> %<uint_1>
 %dpp_quad_select_bits_<index> = OpShiftRightLogical %uint %<ctrl> %dpp_quad_shift_<index>
   %dpp_quad_select_<index> = OpBitwiseAnd %uint %dpp_quad_select_bits_<index> %<uint_3>
       %dpp_target_<index> = OpBitwiseOr %uint %dpp_quad_base_<index> %dpp_quad_select_<index>
      %dpp_value_u_<index> = OpGroupNonUniformShuffle %uint %uint_3 %dpp_src_u_<index> %dpp_target_<index>
             %t0_<index> = OpBitcast %float %dpp_value_u_<index>
)";

		load0 += String8(dpp_quad_permute)
		             .ReplaceStr("<index>", index_str)
		             .ReplaceStr("<ctrl>", ctrl)
		             .ReplaceStr("<quad_mask>", quad_mask)
		             .ReplaceStr("<uint_1>", uint_1)
		             .ReplaceStr("<uint_3>", uint_3);
	}

	// TODO() check VSKIP

	static const char* text = R"(
    <load0>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t0_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Mac, Max, Min, Mul, Sub, Subrev, Add */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load_dst;
	String8 param0 = param[0];

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (param0.ContainsStr("tdst_<index>") && !operand_load_float(spirv, inst.dst, "tdst_<index>", index_str, &load_dst))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check SP_DENORM
	// TODO() check SP_ROUND
	// TODO() check DX10_CLAMP
	// TODO() check IEEE

	static const char* text = R"(
              <load0>
              <load1>
              <load_dst>
              <param>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load_dst>", load_dst)
	                   .ReplaceStr("<param>", param0)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Rcp, Rsq, Sqrt, Ceil, Floor, Fract, Rndne, Trunc, Exp, Log, Cos, Sin */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_F32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	// EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	// EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check DX10_CLAMP
	// TODO() check IEEE

	static const char* text = R"(
    <load0>
    <param0>
    <param1>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: And, Or, Bcnt, Bfm, Lshr, Lshl, Lshlrev, Lshrrev, MinU32, MulU32U24, MulLoU32, MulHiU32 */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              <param2>
              %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Ashr, Ashrrev, MulLo */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_I32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              %tf_<index> = OpBitcast %float %t_<index>
              OpStore %<dst> %tf_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

// Three-input signed integer ALU operations. VGPR storage is raw bits, so the
// signed result is converted back to float storage only after the operation.
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_I32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load2;

	const String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1) ||
	    !operand_load_int(spirv, inst.src[2], "t2_<index>", index_str, &load2))
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
         %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: U32 */
KYTY_RECOMPILER_FUNC(Recompile_VCvt_XXX_F32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC
	// TODO() check SP_DENORM_IN

	static const char* text = R"(
    <load0>
    <param0>
    <param1>
    <param2>
    %t_<index> = OpBitcast %float %t2_<index>
    OpStore %<dst> %t_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: U32, I32, UbyteX, F16 */
KYTY_RECOMPILER_FUNC(Recompile_VCvtF32_XXX_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check SP_ROUND

	static const char* text = R"(
    <load0>
    <param0>
    <param1>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VCvtOffF32I4_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <param0>
    <param1>
    %ti_<index> = OpBitcast %int %t0_<index>
    %tn_<index> = OpBitFieldSExtract %int %ti_<index> %int_0 %int_4
    %tf_<index> = OpConvertSToF %float %tn_<index>
    %t_<index> = OpFMul %float %tf_<index> %float_0_062500
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Sad, Bfe, MadU32U24 */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load2;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() Sad: use only lower 16 bits of Vaccum

	static const char* text = R"(
               <load0>
               <load1>
               <load2>
               <param0>
               <param1>
               <param2>
               <param3>
         %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Add, Sub, Subrev */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst2));

	auto dst_value   = operand_variable_to_str(inst.dst);
	auto dst2_value0 = operand_variable_to_str(inst.dst2, 0);
	auto dst2_value1 = operand_variable_to_str(inst.dst2, 1);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst2));

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(dst2_value0.type != SpirvType::Uint);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
              <load0>
              <load1>
        <param>
        %t208_<index> = OpCompositeExtract %uint %t_<index> 1
        %t209_<index> = OpCompositeExtract %uint %t_<index> 0
        %t210_<index> = OpBitcast %float %t209_<index>
               OpStore %<dst> %t210_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %t213_<index> = OpSelect %uint %exec_lo_b_<index> %t208_<index> %uint_0
               OpStore %<dst2_0> %t213_<index>
               OpStore %<dst2_1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<dst2_0>", dst2_value0.value)
	                   .ReplaceStr("<dst2_1>", dst2_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1Ssrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load2;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst2));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp || inst.dst.multiplier != 1.0f);

	auto dst_value   = operand_variable_to_str(inst.dst);
	auto dst2_value0 = operand_variable_to_str(inst.dst2, 0);
	auto dst2_value1 = operand_variable_to_str(inst.dst2, 1);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst2));
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(dst2_value0.type != SpirvType::Uint);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[2], "t2_<index>", index_str, &load2, 0))
	{
		return false;
	}

	// V_ADD_CO_CI_U32 uses the VOP3B scalar source pair as a per-lane carry-in.
	// The shared addc helper returns the modular sum and carry-out as uvec2.
	static const char* text = R"(
              <load0>
              <load1>
              <load2>
        %t_<index> = OpFunctionCall %v2uint %addc %t0_<index> %t1_<index> %t2_<index>
        %t208_<index> = OpCompositeExtract %uint %t_<index> 1
        %t209_<index> = OpCompositeExtract %uint %t_<index> 0
        %t210_<index> = OpBitcast %float %t209_<index>
               OpStore %<dst> %t210_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %t213_<index> = OpSelect %uint %exec_lo_b_<index> %t208_<index> %uint_0
               OpStore %<dst2_0> %t213_<index>
               OpStore %<dst2_1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<dst2_0>", dst2_value0.value)
	                   .ReplaceStr("<dst2_1>", dst2_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VMadU64U32_Vdst2Sdst2Vsrc0Vsrc1Vsrc2Pair)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst) || !operand_is_variable(inst.dst2));
	EXIT_NOT_IMPLEMENTED(inst.dst.size != 2 || inst.dst2.size != 2 || inst.src[2].size != 2);
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp || inst.dst.multiplier != 1.0f);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst2));

	const String8 index_str = String8::FromPrintf("%u", index);
	const auto    dst_lo    = operand_variable_to_str(inst.dst, 0);
	const auto    dst_hi    = operand_variable_to_str(inst.dst, 1);
	const auto    carry_lo  = operand_variable_to_str(inst.dst2, 0);
	const auto    carry_hi  = operand_variable_to_str(inst.dst2, 1);

	EXIT_NOT_IMPLEMENTED(dst_lo.type != SpirvType::Float || dst_hi.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(carry_lo.type != SpirvType::Uint || carry_hi.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2_lo;
	String8 load2_hi;
	if (!operand_load_uint(spirv, inst.src[0], "tmad0_<index>", index_str, &load0) ||
	    !operand_load_uint(spirv, inst.src[1], "tmad1_<index>", index_str, &load1) ||
	    !operand_load_uint(spirv, inst.src[2], "tadd_lo_<index>", index_str, &load2_lo, 0) ||
	    !operand_load_uint(spirv, inst.src[2], "tadd_hi_<index>", index_str, &load2_hi, 1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          <load2_lo>
          <load2_hi>
          %tmul_<index> = OpUMulExtended %ResTypeU %tmad0_<index> %tmad1_<index>
          %tmul_lo_<index> = OpCompositeExtract %uint %tmul_<index> 0
          %tmul_hi_<index> = OpCompositeExtract %uint %tmul_<index> 1
          %tsum_lo_pair_<index> = OpIAddCarry %ResTypeU %tmul_lo_<index> %tadd_lo_<index>
          %tsum_lo_<index> = OpCompositeExtract %uint %tsum_lo_pair_<index> 0
          %tcarry_lo_<index> = OpCompositeExtract %uint %tsum_lo_pair_<index> 1
          %tsum_hi_base_pair_<index> = OpIAddCarry %ResTypeU %tmul_hi_<index> %tadd_hi_<index>
          %tsum_hi_base_<index> = OpCompositeExtract %uint %tsum_hi_base_pair_<index> 0
          %tcarry_hi_base_<index> = OpCompositeExtract %uint %tsum_hi_base_pair_<index> 1
          %tsum_hi_pair_<index> = OpIAddCarry %ResTypeU %tsum_hi_base_<index> %tcarry_lo_<index>
          %tsum_hi_<index> = OpCompositeExtract %uint %tsum_hi_pair_<index> 0
          %tcarry_hi_extra_<index> = OpCompositeExtract %uint %tsum_hi_pair_<index> 1
          %tcarry_out_<index> = OpBitwiseOr %uint %tcarry_hi_base_<index> %tcarry_hi_extra_<index>
          %tresult_lo_<index> = OpBitcast %float %tsum_lo_<index>
          %tresult_hi_<index> = OpBitcast %float %tsum_hi_<index>
          %texec_mad_<index> = OpLoad %uint %exec_lo
          %tactive_mad_<index> = OpINotEqual %bool %texec_mad_<index> %uint_0
          %tdst_old_lo_<index> = OpLoad %float %<dst_lo>
          %tdst_old_hi_<index> = OpLoad %float %<dst_hi>
          %tdst_active_lo_<index> = OpSelect %float %tactive_mad_<index> %tresult_lo_<index> %tdst_old_lo_<index>
          %tdst_active_hi_<index> = OpSelect %float %tactive_mad_<index> %tresult_hi_<index> %tdst_old_hi_<index>
          OpStore %<dst_lo> %tdst_active_lo_<index>
          OpStore %<dst_hi> %tdst_active_hi_<index>
          %tcarry_active_<index> = OpSelect %uint %tactive_mad_<index> %tcarry_out_<index> %uint_0
          OpStore %<carry_lo> %tcarry_active_<index>
          OpStore %<carry_hi> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2_lo>", load2_lo)
	                   .ReplaceStr("<load2_hi>", load2_hi)
	                   .ReplaceStr("<dst_lo>", dst_lo.value)
	                   .ReplaceStr("<dst_hi>", dst_hi.value)
	                   .ReplaceStr("<carry_lo>", carry_lo.value)
	                   .ReplaceStr("<carry_hi>", carry_hi.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_Fetch)
{
	const auto& inst = code.GetInstructions().At(index);

	const auto* input_info = spirv->GetVsInputInfo();

	EXIT_NOT_IMPLEMENTED(input_info == nullptr || !input_info->fetch_embedded);

	if (input_info != nullptr && input_info->fetch_embedded && inst.dst.type == ShaderOperandType::Vgpr &&
	    inst.src[2].type == ShaderOperandType::IntegerInlineConstant)
	{
		int attrib_id = inst.src[2].constant.i;

		EXIT_IF(attrib_id < 0 || attrib_id >= input_info->resources_num || attrib_id >= ShaderVertexInputInfo::RES_MAX);

		const auto& r = input_info->resources_dst[attrib_id];

		if (inst.dst.size < 1 || inst.dst.size > 4 || r.registers_num < inst.dst.size || r.registers_num > 4)
		{
			printf("WARNING: invalid embedded fetch width in shader (continuing)\n");
		}

		if (r.registers_num > inst.dst.size)
		{
			const String8 index_str = String8::FromPrintf("%d_%u", attrib_id, index);
			const String8 vector_type =
			    String8::FromPrintf("v%dfloat", r.registers_num);
			const String8 value_id = String8::FromPrintf("tfetch_prefix_%s", index_str.c_str());

			*dst_source += String8::FromPrintf("%%%s = OpLoad %%%s %%attr%d\n", value_id.c_str(), vector_type.c_str(), attrib_id);
			for (int component = 0; component < inst.dst.size; component++)
			{
				const String8 component_id =
				    String8::FromPrintf("tfetch_prefix_component_%s_%d", index_str.c_str(), component);
				*dst_source += String8::FromPrintf("%%%s = OpCompositeExtract %%float %%%s %d\n"
				                                  "OpStore %%v%d %%%s\n",
				                                  component_id.c_str(), value_id.c_str(), component,
				                                  inst.dst.register_id + component, component_id.c_str());
			}
			return true;
		}

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
				printf("WARNING: invalid registers_num %d in shader (continuing)\n", r.registers_num);
				break;
		}

		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%d_%u", attrib_id, index))
		                   .ReplaceStr("<p0>", String8::FromPrintf("v%d", inst.dst.register_id + 0))
		                   .ReplaceStr("<p1>", String8::FromPrintf("v%d", inst.dst.register_id + 1))
		                   .ReplaceStr("<p2>", String8::FromPrintf("v%d", inst.dst.register_id + 2))
		                   .ReplaceStr("<p3>", String8::FromPrintf("v%d", inst.dst.register_id + 3))
		                   .ReplaceStr("<attr>", String8::FromPrintf("attr%d", attrib_id));

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_Inject_Debug)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	bool injected = false;
	int  str_id   = 0;
	for (const auto& c: code.GetDebugPrintfs())
	{
		if (c.pc == inst.pc)
		{
			Core::StringList8 loads;
			Core::StringList8 params;
			int               arg_id = 0;
			EXIT_IF(c.args.Size() != c.types.Size());
			for (const auto& a: c.args)
			{
				auto    type = c.types.At(arg_id);
				String8 load;
				bool    ok        = false;
				String8 result_id = String8::FromPrintf("t_%d_<index>", arg_id);
				switch (type)
				{
					case ShaderDebugPrintf::Type::Uint: ok = operand_load_uint(spirv, a, result_id, index_str, &load); break;
					case ShaderDebugPrintf::Type::Int: ok = operand_load_int(spirv, a, result_id, index_str, &load); break;
					case ShaderDebugPrintf::Type::Float: ok = operand_load_float(spirv, a, result_id, index_str, &load); break;
				}
				EXIT_NOT_IMPLEMENTED(!ok);
				loads.Add(load);
				params.Add("%" + result_id);
				arg_id++;
			}

			static const char* text = R"(
                <loads>
     %tt_<index> = OpExtInst %void %NonSemantic_DebugPrintf 1 %printf_str_<str_id> <params>
		)";
			*dst_source += String8(text)
			                   .ReplaceStr("<loads>", loads.Concat("\n"))
			                   .ReplaceStr("<str_id>", String8::FromPrintf("%d", str_id))
			                   .ReplaceStr("<params>", params.Concat(" "))
			                   .ReplaceStr("<index>", index_str);
			injected = true;
		}
		str_id++;
	}

	return injected;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
