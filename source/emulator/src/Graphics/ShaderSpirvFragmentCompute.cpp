#include "Emulator/Graphics/FragmentTransportLayout.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kOutputWords = FragmentTransport::OUTPUT_LANE_WORDS;

String8 ReadWord(const Spirv& spirv, const String8& base, uint32_t offset, const String8& tag)
{
	return String8::FromPrintf("%%%s_index = OpIAdd %%uint %%%s %%%s\n"
	                           "%%%s_ptr = OpAccessChain %%fragment_word_ptr %%fragment_input %%int_0 %%%s_index\n"
	                           "%%%s = OpLoad %%uint %%%s_ptr\n",
	                           tag.c_str(), base.c_str(), spirv.GetConstantUint(offset).c_str(), tag.c_str(), tag.c_str(), tag.c_str(),
	                           tag.c_str());
}

String8 Flag(const Spirv& spirv, const String8& tag, uint32_t bit, const char* result)
{
	return String8::FromPrintf("%%%s_%s_word = OpBitwiseAnd %%uint %%%s_flags %%%s\n"
	                           "%%%s_%s = OpINotEqual %%bool %%%s_%s_word %%uint_0\n",
	                           tag.c_str(), result, tag.c_str(), spirv.GetConstantUint(bit).c_str(), tag.c_str(), result, tag.c_str(),
	                           result);
}

uint32_t Target(const ShaderInstruction& instruction)
{
	switch (instruction.format)
	{
		case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone: return 0u;
		case ShaderInstructionFormat::Mrt1Vsrc0Vsrc1ComprVm: return 1u;
		case ShaderInstructionFormat::Mrt2Vsrc0Vsrc1ComprVm: return 2u;
		case ShaderInstructionFormat::Mrt3Vsrc0Vsrc1ComprVm: return 3u;
		case ShaderInstructionFormat::Mrt4Vsrc0Vsrc1ComprVm: return 4u;
		case ShaderInstructionFormat::Mrt5Vsrc0Vsrc1ComprVm: return 5u;
		case ShaderInstructionFormat::Mrt6Vsrc0Vsrc1ComprVm: return 6u;
		case ShaderInstructionFormat::Mrt7Vsrc0Vsrc1ComprVm: return 7u;
		default: return 8u;
	}
}

} // namespace

String8 Spirv::FragmentTransportAnnotations() const
{
	if (!UsesFragmentCompute())
	{
		return {};
	}
	String8 control;
	if (m_fragment_compute_info->control_binding != UINT32_MAX)
	{
		control = String8::FromPrintf("OpMemberDecorate %%fragment_control_block 0 Offset 0\n"
		                              "OpDecorate %%fragment_control_block Block\n"
		                              "OpDecorate %%fragment_control DescriptorSet %u\n"
		                              "OpDecorate %%fragment_control Binding %u\n",
		                              m_fragment_compute_info->descriptor_set, m_fragment_compute_info->control_binding);
	}
	return String8::FromPrintf("OpDecorate %%fragment_words ArrayStride 4\n"
	                           "OpMemberDecorate %%fragment_block 0 Offset 0\nOpDecorate %%fragment_block Block\n"
	                           "OpDecorate %%fragment_input NonWritable\n"
	                           "OpDecorate %%fragment_input DescriptorSet %u\nOpDecorate %%fragment_input Binding %u\n"
	                           "OpDecorate %%fragment_output DescriptorSet %u\nOpDecorate %%fragment_output Binding %u\n"
	                           "%s",
	                           m_fragment_compute_info->descriptor_set, m_fragment_compute_info->input_binding,
	                           m_fragment_compute_info->descriptor_set, m_fragment_compute_info->output_binding,
	                           control.c_str());
}

String8 Spirv::FragmentTransportTypes() const
{
	if (!UsesFragmentCompute())
	{
		return {};
	}
	String8 types("%fragment_words = OpTypeRuntimeArray %uint\n"
	              "%fragment_block = OpTypeStruct %fragment_words\n"
	              "%fragment_block_ptr = OpTypePointer StorageBuffer %fragment_block\n"
	              "%fragment_word_ptr = OpTypePointer StorageBuffer %uint\n");
	if (m_fragment_compute_info->control_binding != UINT32_MAX)
	{
		types += "%fragment_control_block = OpTypeStruct %fragment_words\n"
		         "%fragment_control_ptr = OpTypePointer StorageBuffer %fragment_control_block\n";
	}
	return types;
}

String8 Spirv::FragmentTransportVariables() const
{
	if (!UsesFragmentCompute())
	{
		return {};
	}
	String8 variables("%fragment_input = OpVariable %fragment_block_ptr StorageBuffer\n"
	                  "%fragment_output = OpVariable %fragment_block_ptr StorageBuffer\n");
	if (m_fragment_compute_info->control_binding != UINT32_MAX)
	{
		variables += "%fragment_control = OpVariable %fragment_control_ptr StorageBuffer\n";
	}
	return variables;
}

String8 Spirv::FragmentLocalVariables() const
{
	if (!UsesFragmentCompute())
	{
		return {};
	}
	String8 source = "%fragment_alive = OpVariable %_ptr_Function_uint Function\n"
	                 "%fragment_mask = OpVariable %_ptr_Function_uint Function\n";
	for (uint32_t component = 0; component < 32u; ++component)
	{
		source += String8::FromPrintf("%%fragment_color_%u = OpVariable %%_ptr_Function_float Function\n", component);
	}
	return source;
}

void Spirv::FindFragmentConstants()
{
	if (!UsesFragmentCompute())
	{
		return;
	}
	const uint32_t input_words  = 1u + m_fragment_compute_info->initial_vgpr_count + FragmentLanePacking().words;
	const uint32_t header_words = m_fragment_compute_info->HeaderWords();
	for (uint32_t value = 0; value <= input_words || value <= kOutputWords; ++value)
	{
		AddConstantUint(value);
	}
	for (uint32_t value: {1u, 2u, 4u, 7u, 8u, 15u, 16u, 34u, 64u, 33u, 0xfffffff0u, header_words, input_words * 64u + header_words,
	                      kOutputWords * 64u})
	{
		AddConstantUint(value);
	}
	for (uint32_t bit = 0; bit < 32u; ++bit)
	{
		AddConstantUint(1u << bit);
	}
	for (uint32_t target = 0; target < 8u; ++target)
	{
		for (uint32_t mask = 0; mask < 16u; ++mask)
		{
			AddConstantUint(mask << (target * 4u));
		}
	}
}

const FragmentTransport::LanePacking& Spirv::FragmentLanePacking() const
{
	if (!m_fragment_lane_packing_valid)
	{
		EXIT_IF(m_ps_input_info == nullptr);
		m_fragment_lane_packing       = FragmentTransport::BuildLanePacking(m_code, *m_ps_input_info);
		m_fragment_lane_packing_valid = true;
	}
	return m_fragment_lane_packing;
}

String8 Spirv::FragmentProlog() const
{
	if (!UsesFragmentCompute())
	{
		return {};
	}
	const uint32_t stride       = 1u + m_fragment_compute_info->initial_vgpr_count + FragmentLanePacking().words;
	const uint32_t header_words = m_fragment_compute_info->HeaderWords();
	const uint32_t wave_words   = stride * 64u + header_words;
	String8        active_check;
	if (m_fragment_compute_info->control_binding != UINT32_MAX)
	{
		// Pack publishes the number of active waves; dispatch groups past it
		// hold no lane records and must not consume capacity.
		active_check = "%fragment_control_count_ptr = OpAccessChain %fragment_word_ptr %fragment_control %int_0 %uint_2\n"
		               "%fragment_control_count = OpLoad %uint %fragment_control_count_ptr\n"
		               "%fragment_control_active = OpULessThan %bool %fragment_group %fragment_control_count\n"
		               "%fragment_input_bounded = OpLogicalAnd %bool %fragment_input_fits %fragment_control_active\n";
	} else
	{
		active_check = "%fragment_input_bounded = OpCopyObject %bool %fragment_input_fits\n";
	}
	String8        source       = String8::FromPrintf(
        "%%fragment_input_length = OpArrayLength %%uint %%fragment_input 0\n"
        "%%fragment_output_length = OpArrayLength %%uint %%fragment_output 0\n"
        "%%fragment_input_waves = OpUDiv %%uint %%fragment_input_length %%%s\n"
        "%%fragment_output_waves = OpUDiv %%uint %%fragment_output_length %%%s\n"
        "%%fragment_group_vector = OpLoad %%v3uint %%gl_WorkGroupID\n"
        "%%fragment_group = OpCompositeExtract %%uint %%fragment_group_vector 0\n"
        "%%fragment_group_y = OpCompositeExtract %%uint %%fragment_group_vector 1\n"
        "%%fragment_group_z = OpCompositeExtract %%uint %%fragment_group_vector 2\n"
        "%%fragment_group_yz = OpBitwiseOr %%uint %%fragment_group_y %%fragment_group_z\n"
        "%%fragment_group_1d = OpIEqual %%bool %%fragment_group_yz %%uint_0\n"
        "%%fragment_input_fits = OpULessThan %%bool %%fragment_group %%fragment_input_waves\n"
        "%%fragment_output_fits = OpULessThan %%bool %%fragment_group %%fragment_output_waves\n"
        "%s"
        "%%fragment_buffers_fit = OpLogicalAnd %%bool %%fragment_input_bounded %%fragment_output_fits\n"
        "%%fragment_dispatch_fits = OpLogicalAnd %%bool %%fragment_buffers_fit %%fragment_group_1d\n"
        "OpSelectionMerge %%fragment_begin None\n"
        "OpBranchConditional %%fragment_dispatch_fits %%fragment_begin %%fragment_abort\n"
        "%%fragment_abort = OpLabel\nOpReturn\n%%fragment_begin = OpLabel\n"
        "%%fragment_wave_base = OpIMul %%uint %%fragment_group %%uint_64\n"
        "%%fragment_input_wave_base = OpIMul %%uint %%fragment_group %%%s\n"
        "%%fragment_lane = OpLoad %%uint %%gl_SubgroupInvocationID\n"
        "%%fragment_lane_offset = OpIMul %%uint %%fragment_lane %%%s\n"
        "%%fragment_lane_header = OpIAdd %%uint %%fragment_input_wave_base %%%s\n"
        "%%fragment_lane_base = OpIAdd %%uint %%fragment_lane_header %%fragment_lane_offset\n",
        GetConstantUint(wave_words).c_str(), GetConstantUint(kOutputWords * 64u).c_str(), active_check.c_str(),
        GetConstantUint(wave_words).c_str(), GetConstantUint(stride).c_str(), GetConstantUint(header_words).c_str());
	if (header_words != 0u)
	{
		source += ReadWord(*this, "fragment_input_wave_base", 0u, "fragment_parameters");
		source += String8::FromPrintf("OpStore %%s%u %%fragment_parameters\n", m_fragment_compute_info->user_sgpr_count);
	}
	if (m_fragment_compute_info->parameter_state == ShaderFragmentParameterState::Virtualized)
	{
		source += String8::FromPrintf("OpStore %%s%u %%uint_0\n", m_fragment_compute_info->user_sgpr_count);
	}
	source += ReadWord(*this, "fragment_lane_base", 0u, "fragment_flags");
	source += Flag(*this, "fragment", 1u, "allocated");
	source += Flag(*this, "fragment", 2u, "exec");
	source += Flag(*this, "fragment", 4u, "covered");
	source += String8::FromPrintf(
	    "%%fragment_initial_exec = OpSelect %%uint %%fragment_exec %%uint_1 %%uint_0\n"
	    "%%fragment_initial_ballot = OpFunctionCall %%v4uint %%split_ballot %%fragment_exec\n"
	    "%%fragment_initial_lo = OpCompositeExtract %%uint %%fragment_initial_ballot 0\n"
	    "%%fragment_initial_hi = OpCompositeExtract %%uint %%fragment_initial_ballot 1\n"
	    "OpStore %%exec_lo %%fragment_initial_lo\nOpStore %%exec_hi %%fragment_initial_hi\n"
	    "OpStore %%exec_lane_lo %%fragment_initial_exec\nOpStore %%exec_lane_hi %%uint_0\n"
	    "%%fragment_exec_z = OpSelect %%uint %%fragment_exec %%uint_0 %%uint_1\nOpStore %%execz %%fragment_exec_z\n");
	for (uint32_t reg = 0; reg < m_fragment_compute_info->initial_vgpr_count; ++reg)
	{
		const auto value = String8::FromPrintf("fragment_v%u", reg);
		source += ReadWord(*this, "fragment_lane_base", reg + 1u, value);
		source += String8::FromPrintf("%%%s_float = OpBitcast %%float %%%s\nOpStore %%v%u %%%s_float\n", value.c_str(), value.c_str(), reg,
		                              value.c_str());
	}
	source += "OpStore %fragment_alive %uint_0\nOpStore %fragment_mask %uint_0\n";
	for (uint32_t component = 0; component < 32u; ++component)
	{
		source += String8::FromPrintf("OpStore %%fragment_color_%u %%float_0_000000\n", component);
	}
	return source;
}

String8 Spirv::FragmentEpilog(uint32_t index) const
{
	if (!UsesFragmentCompute())
	{
		return {};
	}
	const auto tag = String8::FromPrintf("fragment_out_%u", index);
	String8    source = String8::FromPrintf("%%%s_lane = OpIAdd %%uint %%fragment_wave_base %%fragment_lane\n"
	                                        "%%%s_base = OpIMul %%uint %%%s_lane %%uint_34\n",
	                                        tag.c_str(), tag.c_str(), tag.c_str());
	for (uint32_t word = 0; word < kOutputWords; ++word)
	{
		const auto id = String8::FromPrintf("%s_%u", tag.c_str(), word);
		source += String8::FromPrintf("%%%s_index = OpIAdd %%uint %%%s_base %%%s\n"
		                              "%%%s_ptr = OpAccessChain %%fragment_word_ptr %%fragment_output %%int_0 %%%s_index\n",
		                              id.c_str(), tag.c_str(), GetConstantUint(word).c_str(), id.c_str(), id.c_str());
		if (word < 2u)
		{
			source += String8::FromPrintf("%%%s = OpLoad %%uint %%fragment_%s\n", id.c_str(), word == 0u ? "alive" : "mask");
		} else
		{
			source += String8::FromPrintf("%%%s_float = OpLoad %%float %%fragment_color_%u\n"
			                              "%%%s = OpBitcast %%uint %%%s_float\n",
			                              id.c_str(), word - 2u, id.c_str(), id.c_str());
		}
		source += String8::FromPrintf("OpStore %%%s_ptr %%%s\n", id.c_str(), id.c_str());
	}
	return source;
}

bool Spirv::EmitFragmentInterpolation(const ShaderInstruction& instruction, const String8& exec, const String8& tag, String8* output) const
{
	if (output == nullptr ||
	    (instruction.type != ShaderInstructionType::VInterpMovF32 && instruction.type != ShaderInstructionType::VInterpP1F32 &&
	     instruction.type != ShaderInstructionType::VInterpP2F32))
	{
		return false;
	}
	const uint32_t attribute = instruction.src[1].constant.u;
	if (attribute >= FragmentLanePacking().attributes.size())
	{
		return false;
	}
	const bool     move       = instruction.type == ShaderInstructionType::VInterpMovF32;
	const bool     per_vertex = move && m_ps_input_info != nullptr && m_ps_input_info->custom_interpolation.Enabled() &&
	                            (m_ps_input_info->custom_interpolation.per_vertex_inputs & (1u << attribute)) != 0u;
	const auto&    lane      = FragmentLanePacking().attributes[attribute];
	const uint32_t word_base = per_vertex ? lane.raw_offset : lane.parameter_offset;
	if (word_base == UINT32_MAX)
	{
		return false;
	}
	const uint32_t parameter = per_vertex ? instruction.src[0].constant.u * 4u : 0u;
	const uint32_t offset    = 1u + m_fragment_compute_info->initial_vgpr_count + word_base + parameter + instruction.src[2].constant.u;
	*output += ReadWord(*this, "fragment_lane_base", offset, tag + "_raw");
	const auto destination = String8::FromPrintf("v%d", instruction.dst.register_id);
	*output += String8::FromPrintf("%%%s_value = OpBitcast %%float %%%s_raw\n"
	                               "%%%s_old = OpLoad %%float %%%s\n"
	                               "%%%s_result = OpSelect %%float %%%s %%%s_value %%%s_old\nOpStore %%%s %%%s_result\n",
	                               tag.c_str(), tag.c_str(), tag.c_str(), destination.c_str(), tag.c_str(), exec.c_str(), tag.c_str(),
	                               tag.c_str(), destination.c_str(), tag.c_str());
	return true;
}

bool Spirv::EmitFragmentExport(const ShaderInstruction& instruction, uint32_t index, const String8& exec, const String8& tag,
                               String8* output)
{
	*output += String8::FromPrintf("%%%s_write = OpLogicalAnd %%bool %%%s %%fragment_covered\n", tag.c_str(), exec.c_str());
	if ((instruction.exp_control & 1u) != 0u)
	{
		*output += String8::FromPrintf("%%%s_alive = OpSelect %%uint %%%s_write %%uint_1 %%uint_0\nOpStore %%fragment_alive %%%s_alive\n",
		                               tag.c_str(), tag.c_str(), tag.c_str());
	}
	if (ShaderIsNullMrtDoneFormat(instruction.format))
	{
		return true;
	}
	const uint32_t target = Target(instruction);
	if (target >= 8u)
	{
		return false;
	}
	if (m_ps_input_info->target_output_mode[target] == 0u)
	{
		return true;
	}
	for (uint32_t pair = 0; pair < 2u; ++pair)
	{
		const auto id = tag + String8::FromPrintf("_pair%u", pair);
		if (CanLoadPackedHalfForExport(static_cast<int>(index), instruction.src[pair]))
		{
			*output += String8::FromPrintf("%%%s_raw = OpLoad %%uint %%v%d_packed_half\n", id.c_str(), instruction.src[pair].register_id);
		} else if (!operand_load_uint(this, instruction.src[pair], id + "_raw", String8(), output))
		{
			return false;
		}
		*output += String8::FromPrintf("%%%s = OpExtInst %%v2float %%GLSL_std_450 UnpackHalf2x16 %%%s_raw\n", id.c_str(), id.c_str());
	}
	uint32_t mask = 0u;
	for (uint32_t component = 0; component < 4u; ++component)
	{
		const uint32_t selected = ShaderColorExportSourceComponent(m_ps_input_info->target_output_order[target], component);
		if ((instruction.exp_enable_mask & (1u << selected)) == 0u)
		{
			continue;
		}
		mask |= 1u << (target * 4u + component);
		const auto id = tag + String8::FromPrintf("_component%u", component);
		*output += String8::FromPrintf("%%%s_value = OpCompositeExtract %%float %%%s_pair%u %u\n"
		                              "%%%s_old = OpLoad %%float %%fragment_color_%u\n"
		                              "%%%s = OpSelect %%float %%%s_write %%%s_value %%%s_old\n"
		                              "OpStore %%fragment_color_%u %%%s\n",
		                              id.c_str(), tag.c_str(), selected / 2u, selected % 2u, id.c_str(), target * 4u + component, id.c_str(),
		                              tag.c_str(), id.c_str(), id.c_str(), target * 4u + component, id.c_str());
	}
	*output += String8::FromPrintf("%%%s_mask_old = OpLoad %%uint %%fragment_mask\n"
	                               "%%%s_mask_bits = OpSelect %%uint %%%s_write %%%s %%uint_0\n"
	                               "%%%s_mask_new = OpBitwiseOr %%uint %%%s_mask_old %%%s_mask_bits\n"
	                               "OpStore %%fragment_mask %%%s_mask_new\n",
	                               tag.c_str(), tag.c_str(), tag.c_str(), GetConstantUint(mask).c_str(), tag.c_str(), tag.c_str(), tag.c_str(),
	                               tag.c_str());
	return true;
}

bool Spirv::EmitFragmentInstruction(const ShaderInstruction& instruction, uint32_t index, String8* output)
{
	if (!UsesFragmentCompute() || output == nullptr)
	{
		return false;
	}
	const bool interpolation = instruction.type == ShaderInstructionType::VInterpMovF32 ||
	                           instruction.type == ShaderInstructionType::VInterpP1F32 ||
	                           instruction.type == ShaderInstructionType::VInterpP2F32;
	const bool export_inst = instruction.type == ShaderInstructionType::Exp;
	if (!interpolation && !export_inst)
	{
		return false;
	}
	if (interpolation && instruction.type == ShaderInstructionType::VInterpP1F32 &&
	    !ShaderFragmentInterpolationPairSupported(m_code, index, *m_ps_input_info))
	{
		return false;
	}
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	const auto tag = String8::FromPrintf("fragment_instruction_%u", index);
	if (!EmitNativeMaskBit(exec, tag + "_exec", output))
	{
		return false;
	}
	// The accessor uses the packed alias for explicit EXEC operands; this pass
	// reads the architectural pair the prolog initialized.
	*output = output->ReplaceStr("%packed_exec_", "%exec_");
	*output += String8::FromPrintf("%%%s_exec_pred = OpINotEqual %%bool %%%s_exec %%uint_0\n", tag.c_str(), tag.c_str());
	const auto predicate = tag + "_exec_pred";
	return interpolation ? EmitFragmentInterpolation(instruction, predicate, tag, output)
	                     : EmitFragmentExport(instruction, index, predicate, tag, output);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
