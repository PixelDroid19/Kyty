#include "Emulator/Graphics/FragmentTransportLayout.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kOutputWords = FragmentTransport::OUTPUT_LANE_WORDS;

const char* BankName(ShaderWaveBank bank)
{
	return bank == ShaderWaveBank::Low ? "low" : "high";
}

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
	return String8::FromPrintf("OpDecorate %%fragment_words ArrayStride 4\n"
	                           "OpMemberDecorate %%fragment_block 0 Offset 0\nOpDecorate %%fragment_block Block\n"
	                           "OpDecorate %%fragment_input NonWritable\n"
	                           "OpDecorate %%fragment_input DescriptorSet %u\nOpDecorate %%fragment_input Binding %u\n"
	                           "OpDecorate %%fragment_output DescriptorSet %u\nOpDecorate %%fragment_output Binding %u\n",
	                           m_fragment_compute_info->descriptor_set, m_fragment_compute_info->input_binding,
	                           m_fragment_compute_info->descriptor_set, m_fragment_compute_info->output_binding);
}

String8 Spirv::FragmentTransportTypes() const
{
	return UsesFragmentCompute() ? String8("%fragment_words = OpTypeRuntimeArray %uint\n"
	                                       "%fragment_block = OpTypeStruct %fragment_words\n"
	                                       "%fragment_block_ptr = OpTypePointer StorageBuffer %fragment_block\n"
	                                       "%fragment_word_ptr = OpTypePointer StorageBuffer %uint\n")
	                             : String8();
}

String8 Spirv::FragmentTransportVariables() const
{
	return UsesFragmentCompute() ? String8("%fragment_input = OpVariable %fragment_block_ptr StorageBuffer\n"
	                                       "%fragment_output = OpVariable %fragment_block_ptr StorageBuffer\n")
	                             : String8();
}

String8 Spirv::FragmentLocalVariables() const
{
	String8 source;
	if (!UsesFragmentCompute())
	{
		return source;
	}
	for (const char* bank: {"low", "high"})
	{
		source += String8::FromPrintf("%%fragment_alive_%s = OpVariable %%_ptr_Function_uint Function\n", bank);
		source += String8::FromPrintf("%%fragment_mask_%s = OpVariable %%_ptr_Function_uint Function\n", bank);
		for (uint32_t component = 0; component < 32u; ++component)
		{
			source += String8::FromPrintf("%%fragment_color_%s_%u = OpVariable %%_ptr_Function_float Function\n", bank, component);
		}
	}
	return source;
}

void Spirv::FindFragmentConstants()
{
	if (!UsesFragmentCompute())
	{
		return;
	}
	const uint32_t input_words  = 1u + m_fragment_compute_info->initial_vgpr_count + m_ps_input_info->input_num * 16u;
	const uint32_t header_words = m_fragment_compute_info->HeaderWords();
	for (uint32_t value = 0; value <= input_words; ++value)
	{
		AddConstantUint(value);
	}
	AddConstantUint(input_words * 64u + header_words);
	AddConstantUint(kOutputWords * 64u);
	AddConstantUint(kOutputWords);
	AddConstantUint(kOutputWords - 1u);
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

String8 Spirv::FragmentProlog() const
{
	const uint32_t stride       = 1u + m_fragment_compute_info->initial_vgpr_count + m_ps_input_info->input_num * 16u;
	const uint32_t header_words = m_fragment_compute_info->HeaderWords();
	const uint32_t wave_words   = stride * 64u + header_words;
	String8 source = String8::FromPrintf("%%wave_lane_id = OpLoad %%uint %%gl_SubgroupInvocationID\n"
	                                     "%%wave_subgroup_id = OpLoad %%uint %%gl_SubgroupID\n"
	                                     "%%wave_logical_low = OpCopyObject %%uint %%wave_lane_id\n"
	                                     "%%wave_logical_high = OpIAdd %%uint %%wave_lane_id %%uint_32\n"
	                                     "%%fragment_group_vector = OpLoad %%v3uint %%gl_WorkGroupID\n"
	                                     "%%fragment_group = OpCompositeExtract %%uint %%fragment_group_vector 0\n"
	                                     "%%fragment_group_y = OpCompositeExtract %%uint %%fragment_group_vector 1\n"
	                                     "%%fragment_group_z = OpCompositeExtract %%uint %%fragment_group_vector 2\n"
	                                     "%%fragment_group_yz = OpBitwiseOr %%uint %%fragment_group_y %%fragment_group_z\n"
	                                     "%%fragment_group_1d = OpIEqual %%bool %%fragment_group_yz %%uint_0\n"
	                                     "%%fragment_input_length = OpArrayLength %%uint %%fragment_input 0\n"
	                                     "%%fragment_output_length = OpArrayLength %%uint %%fragment_output 0\n"
	                                     "%%fragment_input_waves = OpUDiv %%uint %%fragment_input_length %%%s\n"
	                                     "%%fragment_output_waves = OpUDiv %%uint %%fragment_output_length %%%s\n"
	                                     "%%fragment_input_fits = OpULessThan %%bool %%fragment_group %%fragment_input_waves\n"
	                                     "%%fragment_output_fits = OpULessThan %%bool %%fragment_group %%fragment_output_waves\n"
	                                     "%%fragment_buffers_fit = OpLogicalAnd %%bool %%fragment_input_fits %%fragment_output_fits\n"
	                                     "%%fragment_dispatch_fits = OpLogicalAnd %%bool %%fragment_buffers_fit %%fragment_group_1d\n"
	                                     "OpSelectionMerge %%fragment_begin None\n"
	                                     "OpBranchConditional %%fragment_dispatch_fits %%fragment_begin %%fragment_abort\n"
	                                     "%%fragment_abort = OpLabel\nOpReturn\n%%fragment_begin = OpLabel\n"
	                                     "%%fragment_wave_base = OpIMul %%uint %%fragment_group %%uint_64\n"
	                                     "%%fragment_input_wave_base = OpIMul %%uint %%fragment_group %%%s\n"
	                                     "%%fragment_lane_base = OpIAdd %%uint %%fragment_input_wave_base %%%s\n",
	                                     GetConstantUint(wave_words).c_str(), GetConstantUint(kOutputWords * 64u).c_str(),
	                                     GetConstantUint(wave_words).c_str(), GetConstantUint(header_words).c_str());
	if (header_words != 0u)
	{
		source += ReadWord(*this, "fragment_input_wave_base", 0u, "fragment_parameters");
		source += String8::FromPrintf("OpStore %%s%u %%fragment_parameters\n", m_fragment_compute_info->user_sgpr_count);
	}
	if (m_fragment_compute_info->parameter_state == ShaderFragmentParameterState::Virtualized)
	{
		// All sixteen quads own the same captured parameter block. Zero selects
		// its normalized base; admission proves no raw state word is observable.
		source += String8::FromPrintf("OpStore %%s%u %%uint_0\n", m_fragment_compute_info->user_sgpr_count);
	}
	for (const char* bank: {"low", "high"})
	{
		const auto tag = String8("fragment_") + bank;
		source += String8::FromPrintf("%%%s_lane = OpIAdd %%uint %%fragment_wave_base %%wave_logical_%s\n"
		                              "%%%s_offset = OpIMul %%uint %%wave_logical_%s %%%s\n"
		                              "%%%s_base = OpIAdd %%uint %%fragment_lane_base %%%s_offset\n",
		                              tag.c_str(), bank, tag.c_str(), bank, GetConstantUint(stride).c_str(), tag.c_str(), tag.c_str());
		source += ReadWord(*this, tag + "_base", 0u, tag + "_flags");
		source += Flag(*this, tag, 1u, "allocated");
		source += Flag(*this, tag, 2u, "exec");
		source += Flag(*this, tag, 4u, "covered_raw");
		source += String8::FromPrintf("%%%s_initial_exec = OpLogicalAnd %%bool %%%s_allocated %%%s_exec\n"
		                              "%%%s_covered = OpLogicalAnd %%bool %%%s_allocated %%%s_covered_raw\n"
		                              "OpStore %%fragment_mask_%s %%uint_0\nOpStore %%fragment_alive_%s %%uint_0\n",
		                              tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), tag.c_str(), bank, bank);
		for (uint32_t reg = 0; reg < m_fragment_compute_info->initial_vgpr_count; ++reg)
		{
			const auto value = tag + String8::FromPrintf("_v%u", reg);
			source += ReadWord(*this, tag + "_base", reg + 1u, value);
			source += String8::FromPrintf("%%%s_float = OpBitcast %%float %%%s\nOpStore %%v%u_%s %%%s_float\n", value.c_str(),
			                              value.c_str(), reg, bank, value.c_str());
		}
		for (uint32_t component = 0; component < 32u; ++component)
		{
			source += String8::FromPrintf("OpStore %%fragment_color_%s_%u %%float_0_000000\n", bank, component);
		}
	}
	EXIT_IF(!EmitComputeWaveBallot("fragment_low_allocated", "fragment_high_allocated", "wave_valid_lo", "wave_valid_hi", &source));
	EXIT_IF(!EmitComputeWaveBallot("fragment_low_initial_exec", "fragment_high_initial_exec", "fragment_initial_exec_lo",
	                               "fragment_initial_exec_hi", &source));
	return source;
}

String8 Spirv::FragmentEpilog() const
{
	String8 source;
	if (!UsesFragmentCompute())
	{
		return source;
	}
	for (const char* bank: {"low", "high"})
	{
		const auto tag = String8("fragment_out_") + bank;
		source += String8::FromPrintf("%%%s_base = OpIMul %%uint %%fragment_%s_lane %%%s\n", tag.c_str(), bank,
		                              GetConstantUint(kOutputWords).c_str());
		for (uint32_t word = 0; word < kOutputWords; ++word)
		{
			const auto id = tag + String8::FromPrintf("_%u", word);
			source += String8::FromPrintf("%%%s_index = OpIAdd %%uint %%%s_base %%%s\n"
			                              "%%%s_ptr = OpAccessChain %%fragment_word_ptr %%fragment_output %%int_0 %%%s_index\n",
			                              id.c_str(), tag.c_str(), GetConstantUint(word).c_str(), id.c_str(), id.c_str());
			if (word < 2u)
			{
				source += String8::FromPrintf("%%%s = OpLoad %%uint %%fragment_%s_%s\n", id.c_str(), word == 0u ? "alive" : "mask", bank);
			} else
			{
				source += String8::FromPrintf("%%%s_float = OpLoad %%float %%fragment_color_%s_%u\n"
				                              "%%%s = OpBitcast %%uint %%%s_float\n",
				                              id.c_str(), bank, word - 2u, id.c_str(), id.c_str());
			}
			source += String8::FromPrintf("OpStore %%%s_ptr %%%s\n", id.c_str(), id.c_str());
		}
	}
	return source;
}

bool Spirv::EmitFragmentInterpolation(const ShaderInstruction& instruction, ShaderWaveBank bank, const String8& tag, String8* output) const
{
	const uint32_t attribute = instruction.src[1].constant.u;
	const uint32_t parameter = instruction.type == ShaderInstructionType::VInterpMovF32 ? 4u + instruction.src[0].constant.u * 4u : 0u;
	const uint32_t offset = 1u + m_fragment_compute_info->initial_vgpr_count + attribute * 16u + parameter + instruction.src[2].constant.u;
	*output += ReadWord(*this, String8("fragment_") + BankName(bank) + "_base", offset, tag + "_raw");
	const auto destination = GetComputeWaveRegister(instruction.dst, bank, 0);
	*output += String8::FromPrintf("%%%s_value = OpBitcast %%float %%%s_raw\n"
	                               "%%%s_old = OpLoad %%float %%%s\n"
	                               "%%%s_result = OpSelect %%float %%%s_exec %%%s_value %%%s_old\nOpStore %%%s %%%s_result\n",
	                               tag.c_str(), tag.c_str(), tag.c_str(), destination.value.c_str(), tag.c_str(), tag.c_str(), tag.c_str(),
	                               tag.c_str(), destination.value.c_str(), tag.c_str());
	return true;
}

bool Spirv::EmitFragmentExport(const ShaderInstruction& instruction, uint32_t index, ShaderWaveBank bank, const String8& tag,
                               String8* output) const
{
	*output +=
	    String8::FromPrintf("%%%s_write = OpLogicalAnd %%bool %%%s_exec %%fragment_%s_covered\n", tag.c_str(), tag.c_str(), BankName(bank));
	if ((instruction.exp_control & 1u) != 0u)
	{
		*output += String8::FromPrintf("%%%s_alive = OpSelect %%uint %%%s_write %%uint_1 %%uint_0\n"
		                               "OpStore %%fragment_alive_%s %%%s_alive\n",
		                               tag.c_str(), tag.c_str(), BankName(bank), tag.c_str());
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
			*output += String8::FromPrintf("%%%s_raw = OpLoad %%uint %%v%d_packed_half_%s\n", id.c_str(), instruction.src[pair].register_id,
			                               BankName(bank));
		} else if (!EmitComputeWaveOperandUint(instruction.src[pair], bank, id + "_raw", output))
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
		*output +=
		    String8::FromPrintf("%%%s_value = OpCompositeExtract %%float %%%s_pair%u %u\n"
		                        "%%%s_old = OpLoad %%float %%fragment_color_%s_%u\n"
		                        "%%%s = OpSelect %%float %%%s_write %%%s_value %%%s_old\n"
		                        "OpStore %%fragment_color_%s_%u %%%s\n",
		                        id.c_str(), tag.c_str(), selected / 2u, selected % 2u, id.c_str(), BankName(bank), target * 4u + component,
		                        id.c_str(), tag.c_str(), id.c_str(), id.c_str(), BankName(bank), target * 4u + component, id.c_str());
	}
	*output += String8::FromPrintf("%%%s_mask_old = OpLoad %%uint %%fragment_mask_%s\n"
	                               "%%%s_mask_bits = OpSelect %%uint %%%s_write %%%s %%uint_0\n"
	                               "%%%s_mask_new = OpBitwiseOr %%uint %%%s_mask_old %%%s_mask_bits\n"
	                               "OpStore %%fragment_mask_%s %%%s_mask_new\n",
	                               tag.c_str(), BankName(bank), tag.c_str(), tag.c_str(), GetConstantUint(mask).c_str(), tag.c_str(),
	                               tag.c_str(), tag.c_str(), BankName(bank), tag.c_str());
	return true;
}

bool Spirv::EmitFragmentInstruction(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (!UsesFragmentCompute() || output == nullptr)
	{
		return false;
	}
	const auto kind = ShaderClassifyFragmentWaveInstruction(instruction, *m_ps_input_info);
	if (kind == ShaderComputeWaveInstructionKind::PixelInterpolation && instruction.type == ShaderInstructionType::VInterpP1F32)
	{
		return ShaderFragmentInterpolationPairSupported(m_code, index, *m_ps_input_info);
	}
	if (kind != ShaderComputeWaveInstructionKind::PixelInterpolation && kind != ShaderComputeWaveInstructionKind::PixelExport)
	{
		return false;
	}
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto tag = String8::FromPrintf("fragment_instruction_%u_%s", index, BankName(bank));
		if (!EmitComputeWaveMaskBit(exec, bank, tag + "_exec", output))
		{
			return false;
		}
		const bool emitted = kind == ShaderComputeWaveInstructionKind::PixelInterpolation
		                         ? EmitFragmentInterpolation(instruction, bank, tag, output)
		                         : EmitFragmentExport(instruction, index, bank, tag, output);
		if (!emitted)
		{
			return false;
		}
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
