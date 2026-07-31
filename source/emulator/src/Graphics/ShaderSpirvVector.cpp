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
			EXIT_NOT_IMPLEMENTED(info == nullptr || !info->ps_pixel_kill_enable);
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
	const String8      export_value =
	    String8::FromPrintf("%%t11_<index> = OpCompositeConstruct %%v4float %%%s_<index> %%%s_<index> %%%s_<index> %%%s_<index>",
	                        source_names[component0], source_names[component1], source_names[component2], source_names[component3]);

	*dst_source += String8(text)
	                   .ReplaceStr("<export_value>", export_value)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<load_src0>", load_src0)
	                   .ReplaceStr("<load_src1>", load_src1)
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
                               const String8& dst0, const String8& dst1, const String8& index_str)
{
	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <predicate> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          %texec_<index> = OpLoad %uint %exec_lo
          %tmasked_<index> = OpBitwiseAnd %uint %t3_<index> %texec_<index>
          OpStore %<dst0> %tmasked_<index>
          OpStore %<dst1> %uint_0
          OpStore %exec_lo %tmasked_<index>
          OpStore %exec_hi %uint_0
          <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst0)
	                   .ReplaceStr("<dst1>", dst1)
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
	append_cmpx_result(dst_source, load0, load1, param[0], dst_value0.value, dst_value1.value, index_str);

	return true;
}

// VOPC compare-and-update-exec helper for unsigned 32-bit predicates.
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_XXX_U32_SmaskVsrc0Vsrc1)
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
	append_cmpx_result(dst_source, load0, load1, param[0], dst_value0.value, dst_value1.value, index_str);

	return true;
}

// Ordered and unordered float predicates share the compare-and-update-exec path.
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1)
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
	append_cmpx_result(dst_source, load0, load1, param[0], dst_value0.value, dst_value1.value, index_str);

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

KYTY_RECOMPILER_FUNC(Recompile_VMbcntHiU32B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	// if (inst.src[0].type == ShaderOperandType::ExecHi)
	//{
	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
	    <load0>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t1_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
	)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
	//}

	//	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_VMbcntLoU32B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	// if (inst.src[0].type == ShaderOperandType::ExecLo)
	//{
	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
	    <load0>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t1_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
	)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
	//}

	// return false;
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
