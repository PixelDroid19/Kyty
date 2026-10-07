#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvTemplates.h"
#include "ShaderStorageAnalysis.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Objects/VulkanImageFormat.h"
#include "Emulator/Graphics/ShaderStorageImage.h"
#include "Emulator/Log.h"

#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static void ValidateImageSampleLzAddresses(const ShaderInstruction& inst, uint32_t coordinate_num)
{
	EXIT_IF(coordinate_num < 2 || coordinate_num > 3);
	if (inst.mimg_address_num == 0)
	{
		return;
	}

	// NSA encodes every address lane supplied by the instruction. Only the
	// number of operands required by the materialized view participates in the
	// sample; later encoded lanes are not padding and must remain ignored.
	if (inst.mimg_address_num < static_cast<int>(coordinate_num)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.mimg_address_num < static_cast<int>(coordinate_num) condition ignored (continuing)\n"); }
}

struct ImageSampleLzPlan
{
	ShaderGen5SampledTextureShape shape;
	uint32_t                      coordinate_num;
	bool                          cube_coordinates;
};

int ResolveStorageTextureArrayIndex(const ShaderCode& code, uint32_t instruction_index,
                                   const ShaderBindResources& bind, int user_data_register_base)
{
	const int descriptor_index = ShaderFindImageStorageTextureDescriptor(code, instruction_index, bind, user_data_register_base);
	if (descriptor_index < 0) { return -1; }
	int storage_index = 0;
	for (int index = 0; index < descriptor_index; ++index)
	{
		storage_index += bind.textures2D.desc[index].usage == ShaderTextureUsage::ReadWrite ? 1 : 0;
	}
	return storage_index < bind.textures2D.textures2d_storage_num ? storage_index : -1;
}

// The shader component each memory channel of a storage store receives:
// identity, unless the emitter applies the descriptor's channel selection.
static void StorageStoreComponentOrder(const ShaderCode& code, uint32_t instruction_index, const ShaderBindResources& bind,
                                       int user_data_register_base, uint32_t order[4])
{
	const int      descriptor_index = ShaderFindImageStorageTextureDescriptor(code, instruction_index, bind, user_data_register_base);
	const uint32_t swizzle          = bind.textures2D.desc[descriptor_index].texture.DstSelXYZW();
	for (uint32_t channel = 0; channel < 4; channel++)
	{
		order[channel] = channel;
	}
	if (!ShaderStorageImageSwizzleInShader(swizzle))
	{
		return;
	}
	for (uint32_t component = 0; component < 4; component++)
	{
		order[GetDstSel(swizzle, component) - 4u] = component;
	}
}

static ImageSampleLzPlan PlanImageSampleLz(const ShaderInstruction& inst, const ShaderBindResources& bind, int user_data_register_base)
{
	const int descriptor_index = ShaderFindImageSampledTextureDescriptor(inst, bind, user_data_register_base);
	if (descriptor_index >= 0)
	{
		const auto& descriptor = bind.textures2D.desc[descriptor_index];
		const auto  shape      = ShaderResolvedSampledTextureShape(descriptor);
		return {shape, shape == ShaderGen5SampledTextureShape::TwoDimensional ? 2u : 3u, inst.mimg_dimension == 3u};
	}

	EXIT("image_sample_lz has no sampled descriptor: srsrc=s%d pc=0x%08" PRIx32 "\n", inst.src[1].register_id, inst.pc);
	return {};
}

bool UsesArrayed2dImages(const ShaderBindResources* bind, ShaderTextureUsage usage)
{
	if (bind == nullptr || bind->textures2D.textures_num == 0)
	{
		return false;
	}

	bool has_arrayed = false;
	bool has_flat    = false;
	for (int i = 0; i < bind->textures2D.textures_num; ++i)
	{
		const auto& descriptor = bind->textures2D.desc[i];
		if (descriptor.usage != usage)
		{
			continue;
		}

		switch (ShaderResolvedSampledTextureShape(descriptor))
		{
			case ShaderGen5SampledTextureShape::TwoDimensional: has_flat = true; break;
			case ShaderGen5SampledTextureShape::TwoDimensionalArray: has_arrayed = true; break;
			case ShaderGen5SampledTextureShape::ThreeDimensional: break;
		}
	}

	// Sampled and storage images occupy distinct SPIR-V descriptor arrays. The
	// sampled path has independent 2D and 2D-array bindings; storage images are
	// still one typed array and must reject a mixed view shape until it receives
	// the same split representation.
	if (has_arrayed && has_flat)
	{
		if (usage == ShaderTextureUsage::ReadWrite) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: usage == ShaderTextureUsage::ReadWrite condition ignored (continuing)\n"); }
		return false;
	}
	return has_arrayed;
}

bool UsesVolumeStorageImages(const ShaderBindResources* bind)
{
	if (bind == nullptr) { return false; }
	for (int i = 0; i < bind->textures2D.textures_num; ++i)
	{
		const auto& descriptor = bind->textures2D.desc[i];
		if (descriptor.usage == ShaderTextureUsage::ReadWrite &&
		    ShaderResolvedSampledTextureShape(descriptor) == ShaderGen5SampledTextureShape::ThreeDimensional) { return true; }
	}
	return false;
}

static bool UsesThreeDimensionalImages(const ShaderBindResources* bind)
{
	return bind != nullptr && bind->textures2D.textures3d_sampled_num > 0;
}

bool SupportsArrayed2dImageInstruction(const ShaderInstruction& inst)
{
	if ((inst.type == ShaderInstructionType::ImageGetResinfo && inst.format == ShaderInstructionFormat::VdataVaddrStDmask) ||
	    (inst.type == ShaderInstructionType::ImageGather4 && inst.format == ShaderInstructionFormat::Vdata4Vaddr3StSsMimgDmask) ||
	    (inst.type == ShaderInstructionType::ImageLoad && inst.format == ShaderInstructionFormat::VdataVaddr3StDmask))
	{
		return true;
	}
	if ((inst.type == ShaderInstructionType::ImageLoad || inst.type == ShaderInstructionType::ImageStore) &&
	    (inst.format == ShaderInstructionFormat::VdataVaddr3StDmask ||
	     inst.format == ShaderInstructionFormat::VdataVaddr4StDmask ||
	     inst.format == ShaderInstructionFormat::Vdata4Vaddr3StDmaskF))
	{
		return true;
	}
	// Bias and PCF sample lower through the shared typed sample emitter, which
	// already classifies the bound descriptor as flat/array/volume.
	if (inst.type == ShaderInstructionType::ImageSampleB || inst.type == ShaderInstructionType::ImageSampleDrefLz)
	{
		return true;
	}
	if (inst.type == ShaderInstructionType::ImageSampleL && (inst.mimg_dimension == 3u || inst.mimg_dimension == 5u))
	{
		return true;
	}
	return inst.type == ShaderInstructionType::ImageSample &&
	       (inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1 ||
	        inst.format == ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3 ||
	        inst.format == ShaderInstructionFormat::Vdata2Vaddr3StSsDmaskC ||
	        inst.format == ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7 ||
	        inst.format == ShaderInstructionFormat::Vdata3Vaddr3StSsDmaskD ||
	        inst.format == ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF);
}

static String8 EmitImageSampleCoordinateLoads(uint32_t index, const SpirvValue& x, const SpirvValue& y, bool cube_coordinates)
{
	const auto index_string = String8::FromPrintf("%u", index);
	if (!cube_coordinates)
	{
		return String8(R"(
%image_sample_x_<index> = OpLoad %float %<x>
%image_sample_y_<index> = OpLoad %float %<y>
)")
		    .ReplaceStr("<index>", index_string)
		    .ReplaceStr("<x>", x.value)
		    .ReplaceStr("<y>", y.value);
	}

	// Cube MIMG coordinates use a [1, 2] face-local window. A 2D-array Vulkan
	// view expects normalized [0, 1] coordinates, while the third component is
	// already the face index. Shift only the two normalized components.
	return String8(R"(
%image_sample_x_raw_<index> = OpLoad %float %<x>
%image_sample_y_raw_<index> = OpLoad %float %<y>
%image_sample_x_<index> = OpFSub %float %image_sample_x_raw_<index> %float_1_000000
%image_sample_y_<index> = OpFSub %float %image_sample_y_raw_<index> %float_1_000000
)")
	    .ReplaceStr("<index>", index_string)
	    .ReplaceStr("<x>", x.value)
	    .ReplaceStr("<y>", y.value);
}

static bool PixelInput0ProbeSelectsFirstSampleB(const Spirv* spirv, uint32_t instruction_index)
{
	if (spirv == nullptr || !spirv->UsesPixelInput0Probe())
	{
		return false;
	}
	const auto& code = spirv->GetCode();
	if (instruction_index >= code.GetInstructions().Size() ||
	    code.GetInstructions().At(instruction_index).type != ShaderInstructionType::ImageSampleB)
	{
		return false;
	}
	for (uint32_t index = 0; index < instruction_index; ++index)
	{
		if (code.GetInstructions().At(index).type == ShaderInstructionType::ImageSampleB)
		{
			return false;
		}
	}
	return true;
}

static bool PixelSampleProbeSelectsInstruction(const Spirv* spirv, uint32_t instruction_index)
{
	if (spirv == nullptr || !spirv->UsesPixelSampleProbe())
	{
		return false;
	}
	const auto* input = spirv->GetPsInputInfo();
	const auto& code  = spirv->GetCode();
	if (input == nullptr || !ShaderPixelSampleProbeMatchesInstruction(code, input->input0_probe))
	{
		return false;
	}
	return instruction_index == input->input0_probe.sample_ordinal;
}

bool Spirv::EmitPixelRgbaProbe(String8* dst_source, uint32_t index, const String8& value_id, const char* symbol_prefix,
	                           bool include_frag_coord) const
{
	if (dst_source == nullptr || value_id.IsEmpty() || symbol_prefix == nullptr || symbol_prefix[0] == '\0')
	{
		return false;
	}
	const auto scope     = GetConstantUint(1u);
	const auto semantics = GetConstantUint(SPIRV_DEVICE_MEMORY_ACQ_REL);
	const auto one       = GetConstantUint(1u);
	const auto zero_uint = GetConstantUint(0u);
	const auto sign_mask = GetConstantUint(0x80000000u);
	if (scope == "unknown_uint_constant" || semantics == "unknown_uint_constant" || one == "unknown_uint_constant" ||
	    zero_uint == "unknown_uint_constant" || sign_mask == "unknown_uint_constant")
	{
		return false;
	}

	static const char* text = R"(
%pixel_sample_probe_count_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_37
%pixel_sample_probe_count_prior_<index> = OpAtomicIAdd %uint %pixel_sample_probe_count_ptr_<index> %<scope> %<semantics> %<one>
%pixel_sample_probe_r_<index> = OpCompositeExtract %float <value_id> 0
%pixel_sample_probe_g_<index> = OpCompositeExtract %float <value_id> 1
%pixel_sample_probe_b_<index> = OpCompositeExtract %float <value_id> 2
%pixel_sample_probe_a_<index> = OpCompositeExtract %float <value_id> 3
%pixel_sample_probe_r_nan_<index> = OpIsNan %bool %pixel_sample_probe_r_<index>
%pixel_sample_probe_g_nan_<index> = OpIsNan %bool %pixel_sample_probe_g_<index>
%pixel_sample_probe_b_nan_<index> = OpIsNan %bool %pixel_sample_probe_b_<index>
%pixel_sample_probe_a_nan_<index> = OpIsNan %bool %pixel_sample_probe_a_<index>
%pixel_sample_probe_r_inf_<index> = OpIsInf %bool %pixel_sample_probe_r_<index>
%pixel_sample_probe_g_inf_<index> = OpIsInf %bool %pixel_sample_probe_g_<index>
%pixel_sample_probe_b_inf_<index> = OpIsInf %bool %pixel_sample_probe_b_<index>
%pixel_sample_probe_a_inf_<index> = OpIsInf %bool %pixel_sample_probe_a_<index>
%pixel_sample_probe_nan_rg_<index> = OpLogicalOr %bool %pixel_sample_probe_r_nan_<index> %pixel_sample_probe_g_nan_<index>
%pixel_sample_probe_nan_ba_<index> = OpLogicalOr %bool %pixel_sample_probe_b_nan_<index> %pixel_sample_probe_a_nan_<index>
%pixel_sample_probe_nan_<index> = OpLogicalOr %bool %pixel_sample_probe_nan_rg_<index> %pixel_sample_probe_nan_ba_<index>
%pixel_sample_probe_inf_rg_<index> = OpLogicalOr %bool %pixel_sample_probe_r_inf_<index> %pixel_sample_probe_g_inf_<index>
%pixel_sample_probe_inf_ba_<index> = OpLogicalOr %bool %pixel_sample_probe_b_inf_<index> %pixel_sample_probe_a_inf_<index>
%pixel_sample_probe_inf_<index> = OpLogicalOr %bool %pixel_sample_probe_inf_rg_<index> %pixel_sample_probe_inf_ba_<index>
%pixel_sample_probe_invalid_<index> = OpLogicalOr %bool %pixel_sample_probe_nan_<index> %pixel_sample_probe_inf_<index>
               OpSelectionMerge %pixel_sample_probe_merge_<index> None
               OpBranchConditional %pixel_sample_probe_invalid_<index> %pixel_sample_probe_invalid_block_<index> %pixel_sample_probe_finite_block_<index>
%pixel_sample_probe_invalid_block_<index> = OpLabel
%pixel_sample_probe_nonfinite_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_38
%pixel_sample_probe_nonfinite_prior_<index> = OpAtomicIAdd %uint %pixel_sample_probe_nonfinite_ptr_<index> %<scope> %<semantics> %<one>
               OpBranch %pixel_sample_probe_merge_<index>
%pixel_sample_probe_finite_block_<index> = OpLabel
%pixel_sample_probe_r_bits_<index> = OpBitcast %uint %pixel_sample_probe_r_<index>
%pixel_sample_probe_r_sign_<index> = OpBitwiseAnd %uint %pixel_sample_probe_r_bits_<index> %<sign_mask>
%pixel_sample_probe_r_negative_<index> = OpINotEqual %bool %pixel_sample_probe_r_sign_<index> %<zero_uint>
%pixel_sample_probe_r_inverted_<index> = OpNot %uint %pixel_sample_probe_r_bits_<index>
%pixel_sample_probe_r_positive_<index> = OpBitwiseXor %uint %pixel_sample_probe_r_bits_<index> %<sign_mask>
%pixel_sample_probe_r_ordered_<index> = OpSelect %uint %pixel_sample_probe_r_negative_<index> %pixel_sample_probe_r_inverted_<index> %pixel_sample_probe_r_positive_<index>
%pixel_sample_probe_min_r_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_39
%pixel_sample_probe_min_r_prior_<index> = OpAtomicUMin %uint %pixel_sample_probe_min_r_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_r_ordered_<index>
%pixel_sample_probe_max_r_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_40
%pixel_sample_probe_max_r_prior_<index> = OpAtomicUMax %uint %pixel_sample_probe_max_r_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_r_ordered_<index>
%pixel_sample_probe_g_bits_<index> = OpBitcast %uint %pixel_sample_probe_g_<index>
%pixel_sample_probe_g_sign_<index> = OpBitwiseAnd %uint %pixel_sample_probe_g_bits_<index> %<sign_mask>
%pixel_sample_probe_g_negative_<index> = OpINotEqual %bool %pixel_sample_probe_g_sign_<index> %<zero_uint>
%pixel_sample_probe_g_inverted_<index> = OpNot %uint %pixel_sample_probe_g_bits_<index>
%pixel_sample_probe_g_positive_<index> = OpBitwiseXor %uint %pixel_sample_probe_g_bits_<index> %<sign_mask>
%pixel_sample_probe_g_ordered_<index> = OpSelect %uint %pixel_sample_probe_g_negative_<index> %pixel_sample_probe_g_inverted_<index> %pixel_sample_probe_g_positive_<index>
%pixel_sample_probe_min_g_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_41
%pixel_sample_probe_min_g_prior_<index> = OpAtomicUMin %uint %pixel_sample_probe_min_g_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_g_ordered_<index>
%pixel_sample_probe_max_g_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_42
%pixel_sample_probe_max_g_prior_<index> = OpAtomicUMax %uint %pixel_sample_probe_max_g_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_g_ordered_<index>
%pixel_sample_probe_b_bits_<index> = OpBitcast %uint %pixel_sample_probe_b_<index>
%pixel_sample_probe_b_sign_<index> = OpBitwiseAnd %uint %pixel_sample_probe_b_bits_<index> %<sign_mask>
%pixel_sample_probe_b_negative_<index> = OpINotEqual %bool %pixel_sample_probe_b_sign_<index> %<zero_uint>
%pixel_sample_probe_b_inverted_<index> = OpNot %uint %pixel_sample_probe_b_bits_<index>
%pixel_sample_probe_b_positive_<index> = OpBitwiseXor %uint %pixel_sample_probe_b_bits_<index> %<sign_mask>
%pixel_sample_probe_b_ordered_<index> = OpSelect %uint %pixel_sample_probe_b_negative_<index> %pixel_sample_probe_b_inverted_<index> %pixel_sample_probe_b_positive_<index>
%pixel_sample_probe_min_b_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_43
%pixel_sample_probe_min_b_prior_<index> = OpAtomicUMin %uint %pixel_sample_probe_min_b_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_b_ordered_<index>
%pixel_sample_probe_max_b_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_44
%pixel_sample_probe_max_b_prior_<index> = OpAtomicUMax %uint %pixel_sample_probe_max_b_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_b_ordered_<index>
%pixel_sample_probe_a_bits_<index> = OpBitcast %uint %pixel_sample_probe_a_<index>
%pixel_sample_probe_a_sign_<index> = OpBitwiseAnd %uint %pixel_sample_probe_a_bits_<index> %<sign_mask>
%pixel_sample_probe_a_negative_<index> = OpINotEqual %bool %pixel_sample_probe_a_sign_<index> %<zero_uint>
%pixel_sample_probe_a_inverted_<index> = OpNot %uint %pixel_sample_probe_a_bits_<index>
%pixel_sample_probe_a_positive_<index> = OpBitwiseXor %uint %pixel_sample_probe_a_bits_<index> %<sign_mask>
%pixel_sample_probe_a_ordered_<index> = OpSelect %uint %pixel_sample_probe_a_negative_<index> %pixel_sample_probe_a_inverted_<index> %pixel_sample_probe_a_positive_<index>
%pixel_sample_probe_min_a_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_45
%pixel_sample_probe_min_a_prior_<index> = OpAtomicUMin %uint %pixel_sample_probe_min_a_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_a_ordered_<index>
%pixel_sample_probe_max_a_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_46
%pixel_sample_probe_max_a_prior_<index> = OpAtomicUMax %uint %pixel_sample_probe_max_a_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_a_ordered_<index>
               OpBranch %pixel_sample_probe_merge_<index>
%pixel_sample_probe_merge_<index> = OpLabel
)";
	const auto index_string = String8::FromPrintf("%u", index);
	auto       record       = String8(text)
	                        .ReplaceStr("pixel_sample_probe", symbol_prefix)
	                        .ReplaceStr("<value_id>", value_id)
	                        .ReplaceStr("<index>", index_string)
	                        .ReplaceStr("<scope>", scope)
	                        .ReplaceStr("<semantics>", semantics)
	                        .ReplaceStr("<one>", one)
	                        .ReplaceStr("<zero_uint>", zero_uint)
	                        .ReplaceStr("<sign_mask>", sign_mask);
	if (include_frag_coord)
	{
		static const char* coverage_text = R"(
%pixel_sample_probe_frag_coord_<index> = OpLoad %v4float %gl_FragCoord
%pixel_sample_probe_frag_x_<index> = OpCompositeExtract %float %pixel_sample_probe_frag_coord_<index> 0
%pixel_sample_probe_frag_y_<index> = OpCompositeExtract %float %pixel_sample_probe_frag_coord_<index> 1
%pixel_sample_probe_frag_x_bits_<index> = OpBitcast %uint %pixel_sample_probe_frag_x_<index>
%pixel_sample_probe_frag_x_sign_<index> = OpBitwiseAnd %uint %pixel_sample_probe_frag_x_bits_<index> %<sign_mask>
%pixel_sample_probe_frag_x_negative_<index> = OpINotEqual %bool %pixel_sample_probe_frag_x_sign_<index> %<zero_uint>
%pixel_sample_probe_frag_x_inverted_<index> = OpNot %uint %pixel_sample_probe_frag_x_bits_<index>
%pixel_sample_probe_frag_x_positive_<index> = OpBitwiseXor %uint %pixel_sample_probe_frag_x_bits_<index> %<sign_mask>
%pixel_sample_probe_frag_x_ordered_<index> = OpSelect %uint %pixel_sample_probe_frag_x_negative_<index> %pixel_sample_probe_frag_x_inverted_<index> %pixel_sample_probe_frag_x_positive_<index>
%pixel_sample_probe_min_x_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_47
%pixel_sample_probe_min_x_prior_<index> = OpAtomicUMin %uint %pixel_sample_probe_min_x_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_frag_x_ordered_<index>
%pixel_sample_probe_max_x_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_48
%pixel_sample_probe_max_x_prior_<index> = OpAtomicUMax %uint %pixel_sample_probe_max_x_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_frag_x_ordered_<index>
%pixel_sample_probe_frag_y_bits_<index> = OpBitcast %uint %pixel_sample_probe_frag_y_<index>
%pixel_sample_probe_frag_y_sign_<index> = OpBitwiseAnd %uint %pixel_sample_probe_frag_y_bits_<index> %<sign_mask>
%pixel_sample_probe_frag_y_negative_<index> = OpINotEqual %bool %pixel_sample_probe_frag_y_sign_<index> %<zero_uint>
%pixel_sample_probe_frag_y_inverted_<index> = OpNot %uint %pixel_sample_probe_frag_y_bits_<index>
%pixel_sample_probe_frag_y_positive_<index> = OpBitwiseXor %uint %pixel_sample_probe_frag_y_bits_<index> %<sign_mask>
%pixel_sample_probe_frag_y_ordered_<index> = OpSelect %uint %pixel_sample_probe_frag_y_negative_<index> %pixel_sample_probe_frag_y_inverted_<index> %pixel_sample_probe_frag_y_positive_<index>
%pixel_sample_probe_min_y_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_49
%pixel_sample_probe_min_y_prior_<index> = OpAtomicUMin %uint %pixel_sample_probe_min_y_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_frag_y_ordered_<index>
%pixel_sample_probe_max_y_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_50
%pixel_sample_probe_max_y_prior_<index> = OpAtomicUMax %uint %pixel_sample_probe_max_y_ptr_<index> %<scope> %<semantics> %pixel_sample_probe_frag_y_ordered_<index>
)";
		record += String8(coverage_text)
		              .ReplaceStr("pixel_sample_probe", symbol_prefix)
		              .ReplaceStr("<index>", index_string)
		              .ReplaceStr("<scope>", scope)
		              .ReplaceStr("<semantics>", semantics)
		              .ReplaceStr("<zero_uint>", zero_uint)
		              .ReplaceStr("<sign_mask>", sign_mask);
	}
	if (!UsesSparsePixelSampleProbe())
	{
		*dst_source = record;
		return true;
	}

	const auto subgroup_scope = GetConstantUint(3u);
	if (subgroup_scope == "unknown_uint_constant")
	{
		return false;
	}
	static const char* sparse_text = R"(
%<prefix>_elect_<index> = OpGroupNonUniformElect %bool %<subgroup_scope>
               OpSelectionMerge %<prefix>_elect_merge_<index> None
               OpBranchConditional %<prefix>_elect_<index> %<prefix>_elect_record_<index> %<prefix>_elect_merge_<index>
%<prefix>_elect_record_<index> = OpLabel
<record>
               OpBranch %<prefix>_elect_merge_<index>
%<prefix>_elect_merge_<index> = OpLabel
)";
	*dst_source = String8(sparse_text)
	                  .ReplaceStr("<prefix>", symbol_prefix)
	                  .ReplaceStr("<index>", index_string)
	                  .ReplaceStr("<subgroup_scope>", subgroup_scope)
	                  .ReplaceStr("<record>", record);
	return true;
}

static bool EmitTypedImageSample(String8* dst_source, uint32_t index, const ShaderInstruction& inst, const Spirv* spirv,
                                            const SpirvValue& x, const SpirvValue& y, const SpirvValue& array_layer,
                                            const SpirvValue& texture, const SpirvValue& sampler,
                                            const SpirvValue* destinations, uint32_t destination_num,
                                            const uint32_t* components = nullptr, const SpirvValue* bias = nullptr)
{
	if (dst_source == nullptr || spirv == nullptr || destinations == nullptr || destination_num == 0 || destination_num > 4)
	{
		return false;
	}

	const auto* bind = spirv->GetBindInfo();
	if (bind == nullptr)
	{
		return false;
	}
	const auto* vs_info = spirv->GetVsInputInfo();
	const int   user_data_register_base = (vs_info != nullptr && vs_info->gs_prolog ? 8 : 0);
	const int   descriptor_index = ShaderFindImageSampledTextureDescriptor(inst, *bind, user_data_register_base);
	if (descriptor_index < 0)
	{
		return false;
	}
	const auto& descriptor       = bind->textures2D.desc[descriptor_index];
	const auto  shape            = ShaderResolvedSampledTextureShape(descriptor);
	const bool  cube_coordinates = inst.mimg_dimension == 3u;
	const int   sampled_num =
	    (shape == ShaderGen5SampledTextureShape::TwoDimensional
	         ? bind->textures2D.textures2d_sampled_num
	         : (shape == ShaderGen5SampledTextureShape::TwoDimensionalArray ? bind->textures2D.textures2d_array_sampled_num
	                                                                        : bind->textures2D.textures3d_sampled_num));
	if (sampled_num <= 0)
	{
		return false;
	}
	const auto* ps_info = spirv->GetPsInputInfo();
	const bool  query_lod_tap = bias != nullptr && ps_info != nullptr &&
	                           FragmentTapQueryLodSelection(spirv->GetCode(), ps_info->fragment_tap, index);
	const bool input0_probe = PixelInput0ProbeSelectsFirstSampleB(spirv, index);
	const bool sample_probe = PixelSampleProbeSelectsInstruction(spirv, index);

	const auto index_string = String8::FromPrintf("%u", index);
	static const char* flat_text = R"(
%image_sample_descriptor_raw_<index> = OpLoad %uint %<texture>
%image_sample_descriptor_<index> = OpBitwiseAnd %uint %image_sample_descriptor_raw_<index> %uint_0x1fffffff
%image_sample_sampler_index_<index> = OpLoad %uint %<sampler>
%image_sample_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_sample_sampler_index_<index>
%image_sample_sampler_<index> = OpLoad %Sampler %image_sample_sampler_ptr_<index>
%image_sample_image_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %image_sample_descriptor_<index>
%image_sample_image_<index> = OpLoad %ImageS %image_sample_image_ptr_<index>
%image_sampled_image_<index> = OpSampledImage %SampledImage %image_sample_image_<index> %image_sample_sampler_<index>
%image_sample_coord_<index> = OpCompositeConstruct %v2float %image_sample_x_<index> %image_sample_y_<index>
%image_sample_value_<index> = OpImageSampleImplicitLod %v4float %image_sampled_image_<index> %image_sample_coord_<index><bias_operand>
)";
	static const char* array_text = R"(
%image_sample_descriptor_raw_<index> = OpLoad %uint %<texture>
%image_sample_descriptor_<index> = OpBitwiseAnd %uint %image_sample_descriptor_raw_<index> %uint_0x1fffffff
%image_sample_sampler_index_<index> = OpLoad %uint %<sampler>
%image_sample_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_sample_sampler_index_<index>
%image_sample_sampler_<index> = OpLoad %Sampler %image_sample_sampler_ptr_<index>
%image_sample_layer_<index> = OpLoad %float %<array_layer>
%image_sample_image_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageSA %textures2DA_S %image_sample_descriptor_<index>
%image_sample_image_<index> = OpLoad %ImageSA %image_sample_image_ptr_<index>
%image_sampled_image_<index> = OpSampledImage %SampledImageA %image_sample_image_<index> %image_sample_sampler_<index>
%image_sample_coord_<index> = OpCompositeConstruct %v3float %image_sample_x_<index> %image_sample_y_<index> %image_sample_layer_<index>
%image_sample_value_<index> = <array_sample_op>
)";
	static const char* volume_text = R"(
%image_sample_descriptor_raw_<index> = OpLoad %uint %<texture>
%image_sample_descriptor_<index> = OpBitwiseAnd %uint %image_sample_descriptor_raw_<index> %uint_0x1fffffff
%image_sample_sampler_index_<index> = OpLoad %uint %<sampler>
%image_sample_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_sample_sampler_index_<index>
%image_sample_sampler_<index> = OpLoad %Sampler %image_sample_sampler_ptr_<index>
%image_sample_layer_<index> = OpLoad %float %<array_layer>
%image_sample_image_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageS3D %textures3D_S %image_sample_descriptor_<index>
%image_sample_image_<index> = OpLoad %ImageS3D %image_sample_image_ptr_<index>
%image_sampled_image_<index> = OpSampledImage %SampledImage3D %image_sample_image_<index> %image_sample_sampler_<index>
%image_sample_coord_<index> = OpCompositeConstruct %v3float %image_sample_x_<index> %image_sample_y_<index> %image_sample_layer_<index>
%image_sample_value_<index> = OpImageSampleImplicitLod %v4float %image_sampled_image_<index> %image_sample_coord_<index><bias_operand>
)";

	const String8 bias_operand = (bias != nullptr)
	                                  ? String8::FromPrintf(" Bias %%image_sample_bias_%s", index_string.c_str())
	                                  : String8();

	// SPIR-V requires the bias value to be defined before the sample consumes it.
	String8 bias_source;
	if (bias != nullptr)
	{
		bias_source = String8(R"(
%image_sample_bias_<index> = OpLoad %float %<bias>
)")
		                  .ReplaceStr("<index>", index_string)
		                  .ReplaceStr("<bias>", bias->value);
	}

	String8 input0_probe_source;
	if (input0_probe)
	{
		const auto scope     = spirv->GetConstantUint(1u);
		const auto semantics = spirv->GetConstantUint(SPIRV_DEVICE_MEMORY_ACQ_REL);
		const auto one       = spirv->GetConstantUint(1u);
		const auto zero_uint = spirv->GetConstantUint(0u);
		const auto sign_mask = spirv->GetConstantUint(0x80000000u);
		if (scope == "unknown_uint_constant" || semantics == "unknown_uint_constant" || one == "unknown_uint_constant" ||
		    zero_uint == "unknown_uint_constant" || sign_mask == "unknown_uint_constant")
		{
			return false;
		}
		static const char* input0_probe_text = R"(
%pixel_input0_probe_count_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_16
%pixel_input0_probe_count_prior_<index> = OpAtomicIAdd %uint %pixel_input0_probe_count_ptr_<index> %<scope> %<semantics> %<one>
%pixel_input0_probe_x_nan_<index> = OpIsNan %bool %image_sample_x_<index>
%pixel_input0_probe_y_nan_<index> = OpIsNan %bool %image_sample_y_<index>
%pixel_input0_probe_x_inf_<index> = OpIsInf %bool %image_sample_x_<index>
%pixel_input0_probe_y_inf_<index> = OpIsInf %bool %image_sample_y_<index>
%pixel_input0_probe_nan_<index> = OpLogicalOr %bool %pixel_input0_probe_x_nan_<index> %pixel_input0_probe_y_nan_<index>
%pixel_input0_probe_inf_<index> = OpLogicalOr %bool %pixel_input0_probe_x_inf_<index> %pixel_input0_probe_y_inf_<index>
%pixel_input0_probe_invalid_<index> = OpLogicalOr %bool %pixel_input0_probe_nan_<index> %pixel_input0_probe_inf_<index>
               OpSelectionMerge %pixel_input0_probe_merge_<index> None
               OpBranchConditional %pixel_input0_probe_invalid_<index> %pixel_input0_probe_invalid_block_<index> %pixel_input0_probe_finite_block_<index>
%pixel_input0_probe_invalid_block_<index> = OpLabel
%pixel_input0_probe_nonfinite_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_17
%pixel_input0_probe_nonfinite_prior_<index> = OpAtomicIAdd %uint %pixel_input0_probe_nonfinite_ptr_<index> %<scope> %<semantics> %<one>
               OpBranch %pixel_input0_probe_merge_<index>
%pixel_input0_probe_finite_block_<index> = OpLabel
%pixel_input0_probe_x_bits_<index> = OpBitcast %uint %image_sample_x_<index>
%pixel_input0_probe_x_sign_<index> = OpBitwiseAnd %uint %pixel_input0_probe_x_bits_<index> %<sign_mask>
%pixel_input0_probe_x_negative_<index> = OpINotEqual %bool %pixel_input0_probe_x_sign_<index> %<zero_uint>
%pixel_input0_probe_x_inverted_<index> = OpNot %uint %pixel_input0_probe_x_bits_<index>
%pixel_input0_probe_x_positive_<index> = OpBitwiseXor %uint %pixel_input0_probe_x_bits_<index> %<sign_mask>
%pixel_input0_probe_x_ordered_<index> = OpSelect %uint %pixel_input0_probe_x_negative_<index> %pixel_input0_probe_x_inverted_<index> %pixel_input0_probe_x_positive_<index>
%pixel_input0_probe_y_bits_<index> = OpBitcast %uint %image_sample_y_<index>
%pixel_input0_probe_y_sign_<index> = OpBitwiseAnd %uint %pixel_input0_probe_y_bits_<index> %<sign_mask>
%pixel_input0_probe_y_negative_<index> = OpINotEqual %bool %pixel_input0_probe_y_sign_<index> %<zero_uint>
%pixel_input0_probe_y_inverted_<index> = OpNot %uint %pixel_input0_probe_y_bits_<index>
%pixel_input0_probe_y_positive_<index> = OpBitwiseXor %uint %pixel_input0_probe_y_bits_<index> %<sign_mask>
%pixel_input0_probe_y_ordered_<index> = OpSelect %uint %pixel_input0_probe_y_negative_<index> %pixel_input0_probe_y_inverted_<index> %pixel_input0_probe_y_positive_<index>
%pixel_input0_probe_min_x_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_18
%pixel_input0_probe_min_x_prior_<index> = OpAtomicUMin %uint %pixel_input0_probe_min_x_ptr_<index> %<scope> %<semantics> %pixel_input0_probe_x_ordered_<index>
%pixel_input0_probe_max_x_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_19
%pixel_input0_probe_max_x_prior_<index> = OpAtomicUMax %uint %pixel_input0_probe_max_x_ptr_<index> %<scope> %<semantics> %pixel_input0_probe_x_ordered_<index>
%pixel_input0_probe_min_y_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_20
%pixel_input0_probe_min_y_prior_<index> = OpAtomicUMin %uint %pixel_input0_probe_min_y_ptr_<index> %<scope> %<semantics> %pixel_input0_probe_y_ordered_<index>
%pixel_input0_probe_max_y_ptr_<index> = OpAccessChain %_ptr_StorageBuffer_uint %vertex_clip_probe %int_21
%pixel_input0_probe_max_y_prior_<index> = OpAtomicUMax %uint %pixel_input0_probe_max_y_ptr_<index> %<scope> %<semantics> %pixel_input0_probe_y_ordered_<index>
               OpBranch %pixel_input0_probe_merge_<index>
%pixel_input0_probe_merge_<index> = OpLabel
)";
		input0_probe_source = String8(input0_probe_text)
		                          .ReplaceStr("<index>", index_string)
		                          .ReplaceStr("<scope>", scope)
		                          .ReplaceStr("<semantics>", semantics)
		                          .ReplaceStr("<one>", one)
		                          .ReplaceStr("<zero_uint>", zero_uint)
		                          .ReplaceStr("<sign_mask>", sign_mask);
	}

	const char* sample_text = (shape == ShaderGen5SampledTextureShape::TwoDimensional ? flat_text :
	                           (shape == ShaderGen5SampledTextureShape::TwoDimensionalArray ? array_text : volume_text));
	// Cube faces are 2D-array layers. Implicit LOD uses ddx/ddy of that
	// layer, which jumps at face seams and on clipped sky-filling
	// triangles, so the sample collapses to an empty high mip. Pin lod 0.
	const bool cube_array_implicit =
	    cube_coordinates && shape == ShaderGen5SampledTextureShape::TwoDimensionalArray && bias == nullptr;
	const char* array_sample_op =
	    (cube_array_implicit || inst.type == ShaderInstructionType::ImageSampleLz)
	        ? "OpImageSampleExplicitLod %v4float %image_sampled_image_<index> %image_sample_coord_<index> Lod %float_0_000000"
	        : "OpImageSampleImplicitLod %v4float %image_sampled_image_<index> %image_sample_coord_<index><bias_operand>";
	String8 source = bias_source + EmitImageSampleCoordinateLoads(index, x, y, cube_coordinates) + input0_probe_source + String8(sample_text)
	                     .ReplaceStr("<array_sample_op>", array_sample_op)
	                     .ReplaceStr("<index>", index_string)
	                     .ReplaceStr("<texture>", texture.value)
	                     .ReplaceStr("<sampler>", sampler.value)
	                     .ReplaceStr("<x>", x.value)
	                     .ReplaceStr("<y>", y.value)
	                     .ReplaceStr("<array_layer>", array_layer.value)
	                     .ReplaceStr("<bias_operand>", bias_operand);
	if (sample_probe)
	{
		String8 sample_probe_source;
		if (!spirv->EmitPixelRgbaProbe(&sample_probe_source, index,
		                              String8::FromPrintf("%%image_sample_value_%u", index), "pixel_sample_probe"))
		{
			return false;
		}
		source += sample_probe_source;
	}
	for (uint32_t component = 0; component < destination_num; component++)
	{
		const uint32_t source_component = (components != nullptr ? components[component] : component);
		const auto component_string = String8::FromPrintf("%u", source_component);
		source += String8(R"(
%image_sample_component_<index>_<component> = OpCompositeExtract %float %image_sample_value_<index> <component>
OpStore %<destination> %image_sample_component_<index>_<component>
)")
		              .ReplaceStr("<index>", index_string)
		              .ReplaceStr("<component>", component_string)
		              .ReplaceStr("<destination>", destinations[component].value);
	}
	if (query_lod_tap)
	{
		const bool array_shape = shape == ShaderGen5SampledTextureShape::TwoDimensionalArray;
		if (array_shape)
		{
			source += String8(R"(
%image_sample_lod_coord_<index> = OpCompositeConstruct %v2float %image_sample_x_<index> %image_sample_y_<index>
)")
			              .ReplaceStr("<index>", index_string);
		}
		const String8 query_coord = array_shape ? String8("%image_sample_lod_coord_<index>") :
		                                               String8("%image_sample_coord_<index>");
		source += String8(R"(
%image_sample_lod_query_<index> = OpImageQueryLod %v2float %image_sampled_image_<index> <query_coord>
%image_sample_lod_relative_<index> = OpCompositeExtract %float %image_sample_lod_query_<index> 1
%image_sample_lod_biased_<index> = OpFAdd %float %image_sample_lod_relative_<index> %image_sample_bias_<index>
%image_sample_lod_levels_i_<index> = OpImageQueryLevels %int %image_sample_image_<index>
%image_sample_lod_levels_f_<index> = OpConvertSToF %float %image_sample_lod_levels_i_<index>
%image_sample_lod_last_f_<index> = OpFSub %float %image_sample_lod_levels_f_<index> %float_1_000000
%image_sample_lod_after_missing_f_<index> = OpFAdd %float %image_sample_lod_levels_f_<index> %float_1_000000
%image_sample_lod_reaches_last_<index> = OpFOrdGreaterThanEqual %bool %image_sample_lod_biased_<index> %image_sample_lod_last_f_<index>
%image_sample_lod_reaches_missing_0_<index> = OpFOrdGreaterThanEqual %bool %image_sample_lod_biased_<index> %image_sample_lod_levels_f_<index>
%image_sample_lod_reaches_missing_1_<index> = OpFOrdGreaterThanEqual %bool %image_sample_lod_biased_<index> %image_sample_lod_after_missing_f_<index>
%image_sample_lod_reaches_last_vis_<index> = OpSelect %float %image_sample_lod_reaches_last_<index> %float_1_000000 %float_0_000000
%image_sample_lod_reaches_missing_0_vis_<index> = OpSelect %float %image_sample_lod_reaches_missing_0_<index> %float_1_000000 %float_0_000000
%image_sample_lod_reaches_missing_1_vis_<index> = OpSelect %float %image_sample_lod_reaches_missing_1_<index> %float_1_000000 %float_0_000000
OpStore %fs_tap_0 %image_sample_lod_reaches_last_vis_<index>
OpStore %fs_tap_1 %image_sample_lod_reaches_missing_0_vis_<index>
OpStore %fs_tap_2 %image_sample_lod_reaches_missing_1_vis_<index>
OpStore %fs_tap_3 %float_1_000000
)")
		              .ReplaceStr("<index>", index_string)
		              .ReplaceStr("<query_coord>", query_coord.ReplaceStr("<index>", index_string));
	}
	*dst_source += source;
	return true;
}

struct SampledImageNumericProfile
{
	bool has_floating_point = false;
	bool has_unsigned       = false;
};

static SampledImageNumericProfile GetSampledImageNumericProfile(const ShaderBindResources* bind)
{
	SampledImageNumericProfile profile {};
	if (bind == nullptr || bind->textures2D.textures_num == 0 || !Config::IsNextGen())
	{
		return profile;
	}

	for (int i = 0; i < bind->textures2D.textures_num; ++i)
	{
		if (bind->textures2D.desc[i].usage != ShaderTextureUsage::ReadOnly)
		{
			continue;
		}
		const auto current_type = VulkanGen5ImageNumericType(bind->textures2D.desc[i].texture.Format());
		if (current_type == GuestImageNumericType::Unsupported)
		{
			EXIT("unsupported Gen5 image numeric type: format=%u\\n", bind->textures2D.desc[i].texture.Format());
		}
		if (current_type == GuestImageNumericType::SignedInteger)
		{
			EXIT("signed Gen5 image types require a signed SPIR-V descriptor path: format=%u\\n",
			     bind->textures2D.desc[i].texture.Format());
		}
		if (current_type == GuestImageNumericType::UnsignedInteger)
		{
			profile.has_unsigned = true;
		} else
		{
			profile.has_floating_point = true;
		}
	}
	return profile;
}

bool UsesUnsignedIntegerImages(const ShaderBindResources* bind)
{
	const auto profile = GetSampledImageNumericProfile(bind);
	return profile.has_unsigned && !profile.has_floating_point;
}

bool UsesMixedSampledImageNumericTypes(const ShaderBindResources* bind)
{
	const auto profile = GetSampledImageNumericProfile(bind);
	return profile.has_unsigned && profile.has_floating_point;
}

ShaderStorageImagePlan ShaderPlanStorageImages(const ShaderCode& code, const ShaderBindResources* bind)
{
	ShaderStorageImagePlan plan {};
	if (bind == nullptr) { return plan; }
	const auto& textures = bind->textures2D;
	plan.has_storage = textures.textures2d_storage_num > 0;
	if (!Config::IsNextGen())
	{
		plan.formatless = plan.has_storage;
		return plan;
	}
	auto reject = [&plan](const char* reason, int descriptor = -1, uint16_t format = 0u, uint32_t pc = 0u)
	{
		plan.supported = false;
		plan.reason = reason;
		plan.descriptor_index = descriptor;
		plan.guest_format = format;
		plan.instruction_pc = pc;
		return plan;
	};
	if (textures.textures_num < 0 || textures.textures_num > ShaderTextureResources::RES_MAX ||
	    textures.textures2d_storage_num < 0 || textures.textures2d_storage_num > textures.textures_num)
	{
		return reject("invalid writable descriptor count");
	}
	const bool atomic_alias = code.HasAnyOf({ShaderInstructionType::ImageAtomicAdd});
	int writable_count = 0;
	int first_float = -1;
	int first_uint = -1;
	uint16_t uint_format = 0;
	bool has_flat = false;
	bool has_array = false;
	bool has_volume = false;
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& descriptor = textures.desc[index];
		if (descriptor.usage != ShaderTextureUsage::ReadWrite) { continue; }
		++writable_count;
		const auto format = descriptor.texture.Format();
		const auto numeric = VulkanGen5ImageNumericType(format);
		if (numeric == GuestImageNumericType::SignedInteger)
		{
			return reject("signed writable images require a signed descriptor bank", index, format);
		}
		if (numeric == GuestImageNumericType::Unsupported || !VulkanSupportsGen5ImageFormat(GuestImageUsage::Storage, format))
		{
			return reject("unsupported writable image format", index, format);
		}
		const auto shape = ShaderResolvedSampledTextureShape(descriptor);
		has_volume |= shape == ShaderGen5SampledTextureShape::ThreeDimensional;
		has_array |= shape == ShaderGen5SampledTextureShape::TwoDimensionalArray;
		has_flat |= shape == ShaderGen5SampledTextureShape::TwoDimensional;
		if (static_cast<int>(has_flat) + static_cast<int>(has_array) + static_cast<int>(has_volume) > 1)
		{
			return reject("mixed 2D, 2D-array and 3D writable images require separate shape banks", index, format);
		}
		if (numeric == GuestImageNumericType::FloatingPoint)
		{
			if (first_float < 0) { first_float = index; }
			continue;
		}
		if (atomic_alias && (format != 20u || descriptor.texture.Type() != 9u))
		{
			return reject("atomic uint alias requires 2D R32_UINT writable descriptors", index, format);
		}
		if (first_uint >= 0 && uint_format != format)
		{
			return reject("mixed writable uint formats require separate typed descriptor banks", index, format);
		}
		first_uint = index;
		uint_format = format;
	}
	if (writable_count != textures.textures2d_storage_num)
	{
		return reject("writable descriptor count differs from storage binding count");
	}
	if (first_float >= 0 && first_uint >= 0 && !atomic_alias)
	{
		return reject("mixed float and uint writable images require the R32 atomic alias or separate typed banks",
		              first_uint, uint_format);
	}
	plan.three_dimensional = has_volume;
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		if (inst.type == ShaderInstructionType::ImageAtomicAdd && (has_volume || !ShaderImageAtomicResourceSupported(code, index, *bind)))
		{
			return reject("image atomic requires a proven 2D R32_UINT writable descriptor", -1, 0u, inst.pc);
		}
		if (inst.type == ShaderInstructionType::ImageStoreMip && (has_array || has_volume))
		{
			return reject("packed-mip store requires a non-array 2D writable image", -1, 0u, inst.pc);
		}
	}
	plan.unsigned_primary = first_uint >= 0 && first_float < 0;
	plan.formatless = first_float >= 0;
	if (plan.unsigned_primary)
	{
		// SPIR-V image-format/VkFormat compatibility is exact, not just numeric
		// class compatibility. No integer domain is represented as generic R32ui.
		// StorageImageExtendedFormats is a Vulkan-supported SPIR-V capability;
		// shaderStorageImageExtendedFormats only guarantees format availability.
		// The resource path still queries support for the actual format/usage.
		switch (uint_format)
		{
			case 5u: plan.image_format = "R8ui"; plan.extended_formats = true; break;
			case 11u: plan.image_format = "R16ui"; plan.extended_formats = true; break;
			case 20u: plan.image_format = "R32ui"; break;
			case 60u: plan.image_format = "Rgba8ui"; break;
			case 62u: plan.image_format = "Rg32ui"; plan.extended_formats = true; break;
			case 75u: plan.image_format = "Rgba32ui"; break;
			default: return reject("writable uint format has no exact SPIR-V declaration", first_uint, uint_format);
		}
	}
	return plan;
}

static ShaderStorageImagePlan RequireStorageImagePlan(const ShaderCode& code, const ShaderBindResources* bind)
{
	const auto plan = ShaderPlanStorageImages(code, bind);
	if (!plan.supported)
	{
		EXIT("unsupported storage image declaration: descriptor=%d format=%u pc=0x%08" PRIx32 " reason=%s\n",
		     plan.descriptor_index, plan.guest_format, plan.instruction_pc, plan.reason);
	}
	return plan;
}

bool UsesUnsignedIntegerStorageImages(const ShaderCode& code, const ShaderBindResources* bind)
{
	return RequireStorageImagePlan(code, bind).unsigned_primary;
}

bool UsesFormatlessStorageImages(const ShaderCode& code, const ShaderBindResources* bind)
{
	// WriteHeader calls this before emitting any image declaration, including
	// typed-only modules. Unsupported mixes terminate before invalid SPIR-V.
	return RequireStorageImagePlan(code, bind).formatless;
}

const char* GetStorageImageFormat(const ShaderCode& code, const ShaderBindResources* bind)
{
	return RequireStorageImagePlan(code, bind).image_format;
}

bool UsesExtendedStorageImageFormats(const ShaderCode& code, const ShaderBindResources* bind)
{
	return RequireStorageImagePlan(code, bind).extended_formats;
}

static bool ResolveStorageImageStoreNumericType(const ShaderCode& code, uint32_t index, const ShaderBindResources& bind,
                                                int user_data_register_base, bool* uint_alias, bool* uint_values)
{
	*uint_alias = false;
	*uint_values = false;
	if (Config::IsNextGen())
	{
		// WriteHeader has admitted the complete domain. Resolve this particular
		// writable descriptor for both store forms, preserving descriptor-flow
		// proofs and avoiding any dependency on sampled numeric classification.
		const int descriptor = ShaderFindImageStorageTextureDescriptor(code, index, bind, user_data_register_base);
		if (descriptor < 0) { return false; }
		*uint_values = VulkanGen5ImageNumericType(bind.textures2D.desc[descriptor].texture.Format()) ==
		               GuestImageNumericType::UnsignedInteger;
		*uint_alias = *uint_values && code.HasAnyOf({ShaderInstructionType::ImageAtomicAdd});
	}
	return true;
}

bool IsImageInstruction(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::ImageGetResinfo:
		case ShaderInstructionType::ImageGather4:
		case ShaderInstructionType::ImageLoad:
		case ShaderInstructionType::ImageSample:
		case ShaderInstructionType::ImageSampleL:
		case ShaderInstructionType::ImageSampleLz:
		case ShaderInstructionType::ImageSampleLzO:
		case ShaderInstructionType::ImageSampleB:
		case ShaderInstructionType::ImageSampleDrefLz:
		case ShaderInstructionType::ImageStore:
		case ShaderInstructionType::ImageAtomicAdd:
		case ShaderInstructionType::ImageStoreMip: return true;
		default: return false;
	}
}

bool IsSampledImageInstruction(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::ImageGetResinfo:
		case ShaderInstructionType::ImageGather4:
		case ShaderInstructionType::ImageLoad:
		case ShaderInstructionType::ImageSample:
		case ShaderInstructionType::ImageSampleL:
		case ShaderInstructionType::ImageSampleLz:
		case ShaderInstructionType::ImageSampleLzO:
		case ShaderInstructionType::ImageSampleB:
		case ShaderInstructionType::ImageSampleDrefLz: return true;
		default: return false;
	}
}

bool IsStorageImageInstruction(const ShaderInstruction& inst)
{
	return inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip ||
	       inst.type == ShaderInstructionType::ImageAtomicAdd;
}

String8 GuardImageDestinationStores(const String8& source, const ShaderInstruction& inst, uint32_t index)
{
	if (source.IsEmpty() || !IsImageInstruction(inst) || inst.dst.type != ShaderOperandType::Vgpr)
	{
		return source;
	}

	uint32_t destination_count = inst.dst.size > 0 ? static_cast<uint32_t>(inst.dst.size) : 1u;
	if (inst.mimg_dmask != 0)
	{
		destination_count = 0;
		for (uint32_t mask = inst.mimg_dmask; mask != 0; mask >>= 1u)
		{
			destination_count += mask & 1u;
		}
	}

	std::set<std::string> destinations;
	for (uint32_t offset = 0; offset < destination_count; ++offset)
	{
		destinations.emplace("v" + std::to_string(inst.dst.register_id + static_cast<int>(offset)));
	}

	const std::string input = source.c_str();
	std::string       output;
	output.reserve(input.size() + 256u);
	bool     guarded = false;
	uint32_t store_number = 0;
	std::string line;
	for (size_t begin = 0; begin < input.size();)
	{
		const size_t end = input.find('\n', begin);
		line             = input.substr(begin, end == std::string::npos ? std::string::npos : end - begin);

		bool replaced = false;
		for (const auto& destination: destinations)
		{
			const std::string needle = "OpStore %" + destination + " ";
			const size_t      store  = line.find(needle);
			if (store == std::string::npos)
			{
				continue;
			}

			const std::string value = line.substr(store + needle.size());
			const std::string old_id = "image_exec_old_" + std::to_string(index) + "_" + std::to_string(store_number);
			const std::string new_id = "image_exec_value_" + std::to_string(index) + "_" + std::to_string(store_number);
			const std::string indentation = line.substr(0, store);
			output += indentation + "%" + old_id + " = OpLoad %float %" + destination + "\n";
			output += indentation + "%" + new_id + " = OpSelect %float %image_exec_active_" + std::to_string(index) + " " + value +
			          " %" + old_id + "\n";
			output += indentation + "OpStore %" + destination + " %" + new_id + "\n";
			guarded = true;
			replaced = true;
			++store_number;
			break;
		}

		if (!replaced)
		{
			output += line;
			output += '\n';
		}
		if (end == std::string::npos)
		{
			break;
		}
		begin = end + 1;
	}

	if (!guarded)
	{
		return source;
	}

	const std::string suffix = std::to_string(index);
	const std::string prefix =
	    "        %image_exec_value_" + suffix + " = OpLoad %uint %exec_lo\n" +
	    "        %image_exec_active_" + suffix + " = OpINotEqual %bool %image_exec_value_" + suffix + " %uint_0\n";
	return String8((prefix + output).c_str());
}


KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsDmask1)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float || src0_value0.type != SpirvType::Float || src1_value0.type != SpirvType::Uint ||
		    src2_value0.type != SpirvType::Uint)
		{
			return false;
		}
		if (bind_info->textures2D.textures2d_array_sampled_num > 0)
		{
			const SpirvValue destinations[] = {dst_value0};
			return EmitTypedImageSample(dst_source, index, inst, spirv, src0_value0, src0_value1, src0_value2,
			                                      src1_value0, src2_value0, destinations, 1);
		}

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_raw_<index> = OpLoad %uint %<src1_value0>
         %t24_<index> = OpBitwiseAnd %uint %t24_raw_<index> %uint_0x1fffffff
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsDmask2)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr &&
	    (bind_info->textures2D.textures2d_sampled_num > 0 || bind_info->textures2D.textures2d_array_sampled_num > 0) &&
	    bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float || src0_value0.type != SpirvType::Float || src1_value0.type != SpirvType::Uint ||
		    src2_value0.type != SpirvType::Uint)
		{
			return false;
		}
		// dmask 0x2 → sample and keep G (component 1).
		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	if (bind_info != nullptr)
	{
		KYTY_LOG_DEBUG(
		             "ImageSample binding unavailable: sampled=%d textures=%d samplers=%d texture_sgpr=%d sampler_sgpr=%d\\n",
		             bind_info->textures2D.textures2d_sampled_num, bind_info->textures2D.textures_num,
		             bind_info->samplers.samplers_num, inst.src[1].register_id, inst.src[2].register_id);
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsDmask4)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// dmask 0x4 → sample and keep B (component 2).
		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsDmask8)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmask3)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr &&
	    (bind_info->textures2D.textures2d_sampled_num > 0 || bind_info->textures2D.textures2d_array_sampled_num > 0) &&
	    bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (dst_value1.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value1.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (bind_info->textures2D.textures2d_array_sampled_num > 0)
		{
			const SpirvValue destinations[] = {dst_value0, dst_value1};
			return EmitTypedImageSample(dst_source, index, inst, spirv, src0_value0, src0_value1, src0_value2,
			                                      src1_value0, src2_value0, destinations, 2);
		}

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmask5)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmask9)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmaskA)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// dmask 0xa -> G+A, stored compactly into vdata[0:1].
		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

// dmask 0xc -> B+A, stored compactly into vdata[0:1].
KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmaskC)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr &&
	    (bind_info->textures2D.textures2d_sampled_num > 0 || bind_info->textures2D.textures2d_array_sampled_num > 0) &&
	    bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (bind_info->textures2D.textures2d_array_sampled_num > 0)
		{
			const uint32_t components[]  = {2, 3};
			const SpirvValue destinations[] = {dst_value0, dst_value1};
			return EmitTypedImageSample(dst_source, index, inst, spirv, src0_value0, src0_value1, src0_value2,
			                                      src1_value0, src2_value0, destinations, 2, components);
		}

		// dmask 0xc -> B+A: sample and keep components 2 and 3.
		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata3Vaddr3StSsDmask7)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr &&
	    (bind_info->textures2D.textures2d_sampled_num > 0 || bind_info->textures2D.textures2d_array_sampled_num > 0) &&
	    bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (bind_info->textures2D.textures2d_array_sampled_num > 0)
		{
			const SpirvValue destinations[] = {dst_value0, dst_value1, dst_value2};
			return EmitTypedImageSample(dst_source, index, inst, spirv, src0_value0, src0_value1, src0_value2,
			                                      src1_value0, src2_value0, destinations, 3);
		}

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

// dmask 0xb: R+G+A → store sample components 0,1,3 into three VGPRs.
KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata3Vaddr3StSsDmaskB)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

// dmask 0xd -> R+B+A, stored compactly into vdata[0:2].
KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata3Vaddr3StSsDmaskD)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr &&
	    (bind_info->textures2D.textures2d_sampled_num > 0 || bind_info->textures2D.textures2d_array_sampled_num > 0) &&
	    bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (bind_info->textures2D.textures2d_array_sampled_num > 0)
		{
			const uint32_t components[]  = {0, 2, 3};
			const SpirvValue destinations[] = {dst_value0, dst_value1, dst_value2};
			return EmitTypedImageSample(dst_source, index, inst, spirv, src0_value0, src0_value1, src0_value2,
			                                      src1_value0, src2_value0, destinations, 3, components);
		}

		// dmask 0xd -> R+B+A: sample and keep components 0, 2 and 3.
		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

// IMAGE_SAMPLE_LZ samples level 0 once; DMASK enables components in R, G, B, A order and they return
// packed into consecutive VGPRs.
static bool RecompileImageSampleLzComponents(KYTY_RECOMPILER_ARGS)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr || bind_info->samplers.samplers_num <= 0)
	{
		return false;
	}

	const auto* vs_info = spirv->GetVsInputInfo();
	const int   user_data_register_base = (vs_info != nullptr && vs_info->gs_prolog ? 8 : 0);
	const auto  plan = PlanImageSampleLz(inst, *bind_info, user_data_register_base);
	const auto shape = plan.shape;
	const int sampled_num =
	    (shape == ShaderGen5SampledTextureShape::TwoDimensional ? bind_info->textures2D.textures2d_sampled_num :
	                                                               (shape == ShaderGen5SampledTextureShape::TwoDimensionalArray ?
	                                                                    bind_info->textures2D.textures2d_array_sampled_num :
	                                                                    bind_info->textures2D.textures3d_sampled_num));
	if (sampled_num <= 0)
	{
		EXIT("image_sample_lz descriptor array unavailable: flat=%d array=%d volume=%d pc=0x%08" PRIx32 "\n",
		     bind_info->textures2D.textures2d_sampled_num,
		     bind_info->textures2D.textures2d_array_sampled_num, bind_info->textures2D.textures3d_sampled_num, inst.pc);
	}

	const uint32_t coordinate_num = plan.coordinate_num;
	ValidateImageSampleLzAddresses(inst, coordinate_num);
	uint32_t components[4] = {};
	int      num           = 0;
	for (uint32_t component = 0; component < 4u; ++component)
	{
		if ((inst.mimg_dmask & (1u << component)) != 0u) { components[num++] = component; }
	}
	if (num == 0 || num != inst.dst.size) { return false; }
	SpirvValue destinations[4];
	for (int i = 0; i < num; ++i)
	{
		destinations[i] = operand_variable_to_str(inst.dst, i);
		if (destinations[i].type != SpirvType::Float) { return false; }
	}
	const auto x       = mimg_address_to_str(inst, 0);
	const auto y       = mimg_address_to_str(inst, 1);
	const auto texture = operand_variable_to_str(inst.src[1], 0);
	const auto sampler = operand_variable_to_str(inst.src[2], 0);
	SpirvValue z {};
	if (coordinate_num == 3u)
	{
		z = mimg_address_to_str(inst, 2);
	}
	if (x.type != SpirvType::Float || y.type != SpirvType::Float || (coordinate_num == 3u && z.type != SpirvType::Float) ||
	    texture.type != SpirvType::Uint || sampler.type != SpirvType::Uint)
	{
		return false;
	}

	static const char* flat_text = R"(
%image_sample_lz_scalar_texture_<index> = OpLoad %uint %<texture>
%image_sample_lz_scalar_descriptor_<index> = OpBitwiseAnd %uint %image_sample_lz_scalar_texture_<index> %uint_0x1fffffff
%image_sample_lz_scalar_texture_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %image_sample_lz_scalar_descriptor_<index>
%image_sample_lz_scalar_image_<index> = OpLoad %ImageS %image_sample_lz_scalar_texture_ptr_<index>
%image_sample_lz_scalar_sampler_<index> = OpLoad %uint %<sampler>
%image_sample_lz_scalar_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_sample_lz_scalar_sampler_<index>
%image_sample_lz_scalar_sampler_value_<index> = OpLoad %Sampler %image_sample_lz_scalar_sampler_ptr_<index>
%image_sample_lz_scalar_sampled_<index> = OpSampledImage %SampledImage %image_sample_lz_scalar_image_<index> %image_sample_lz_scalar_sampler_value_<index>
%image_sample_lz_scalar_coordinate_<index> = OpCompositeConstruct %v2float %image_sample_x_<index> %image_sample_y_<index>
%image_sample_lz_scalar_value_<index> = OpImageSampleExplicitLod %v4float %image_sample_lz_scalar_sampled_<index> %image_sample_lz_scalar_coordinate_<index> Lod %float_0_000000
)";
	static const char* array_text = R"(
%image_sample_lz_scalar_texture_<index> = OpLoad %uint %<texture>
%image_sample_lz_scalar_descriptor_<index> = OpBitwiseAnd %uint %image_sample_lz_scalar_texture_<index> %uint_0x1fffffff
%image_sample_lz_scalar_sampler_<index> = OpLoad %uint %<sampler>
%image_sample_lz_scalar_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_sample_lz_scalar_sampler_<index>
%image_sample_lz_scalar_sampler_value_<index> = OpLoad %Sampler %image_sample_lz_scalar_sampler_ptr_<index>
%image_sample_lz_scalar_z_<index> = OpLoad %float %<z>
%image_sample_lz_scalar_texture_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageSA %textures2DA_S %image_sample_lz_scalar_descriptor_<index>
%image_sample_lz_scalar_image_<index> = OpLoad %ImageSA %image_sample_lz_scalar_texture_ptr_<index>
%image_sample_lz_scalar_sampled_<index> = OpSampledImage %SampledImageA %image_sample_lz_scalar_image_<index> %image_sample_lz_scalar_sampler_value_<index>
%image_sample_lz_scalar_coordinate_<index> = OpCompositeConstruct %v3float %image_sample_x_<index> %image_sample_y_<index> %image_sample_lz_scalar_z_<index>
%image_sample_lz_scalar_value_<index> = OpImageSampleExplicitLod %v4float %image_sample_lz_scalar_sampled_<index> %image_sample_lz_scalar_coordinate_<index> Lod %float_0_000000
)";
	static const char* volume_text = R"(
%image_sample_lz_scalar_texture_<index> = OpLoad %uint %<texture>
%image_sample_lz_scalar_descriptor_<index> = OpBitwiseAnd %uint %image_sample_lz_scalar_texture_<index> %uint_0x1fffffff
%image_sample_lz_scalar_sampler_<index> = OpLoad %uint %<sampler>
%image_sample_lz_scalar_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_sample_lz_scalar_sampler_<index>
%image_sample_lz_scalar_sampler_value_<index> = OpLoad %Sampler %image_sample_lz_scalar_sampler_ptr_<index>
%image_sample_lz_scalar_z_<index> = OpLoad %float %<z>
%image_sample_lz_scalar_texture_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageS3D %textures3D_S %image_sample_lz_scalar_descriptor_<index>
%image_sample_lz_scalar_image_<index> = OpLoad %ImageS3D %image_sample_lz_scalar_texture_ptr_<index>
%image_sample_lz_scalar_sampled_<index> = OpSampledImage %SampledImage3D %image_sample_lz_scalar_image_<index> %image_sample_lz_scalar_sampler_value_<index>
%image_sample_lz_scalar_coordinate_<index> = OpCompositeConstruct %v3float %image_sample_x_<index> %image_sample_y_<index> %image_sample_lz_scalar_z_<index>
%image_sample_lz_scalar_value_<index> = OpImageSampleExplicitLod %v4float %image_sample_lz_scalar_sampled_<index> %image_sample_lz_scalar_coordinate_<index> Lod %float_0_000000
)";
	const char* text = (shape == ShaderGen5SampledTextureShape::TwoDimensional ? flat_text :
	                    (shape == ShaderGen5SampledTextureShape::TwoDimensionalArray ? array_text : volume_text));
	String8 stores;
	for (int i = 0; i < num; ++i)
	{
		stores += String8::FromPrintf("%%image_sample_lz_component_%d_<index> = OpCompositeExtract %%float "
		                              "%%image_sample_lz_scalar_value_<index> %u\nOpStore %%%s %%image_sample_lz_component_%d_<index>\n",
		                              i, components[i], destinations[i].value.c_str(), i);
	}
	*dst_source += EmitImageSampleCoordinateLoads(index, x, y, plan.cube_coordinates) + (String8(text) + stores)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<texture>", texture.value)
	                   .ReplaceStr("<sampler>", sampler.value)
	                   .ReplaceStr("<x>", x.value)
	                   .ReplaceStr("<y>", y.value)
	                   .ReplaceStr("<z>", z.value);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLz_VdataVaddr3StSsMimgDmask)
{
	return RecompileImageSampleLzComponents(index, code, dst_source, spirv, param, scc_check);
}

// dmask 0xb -> R+G+A, stored compactly into vdata[0:2].
static bool RecompileImageSampleLzO(KYTY_RECOMPILER_ARGS)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	uint32_t destination_count = 0;
	for (uint32_t component = 0; component < 4; component++)
	{
		destination_count += (inst.mimg_dmask >> component) & 1u;
	}
	if (destination_count == 0 || inst.dst.size != static_cast<int>(destination_count))
	{
		return false;
	}

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>

         %t39_<index> = OpLoad %float %<src0_value1>
         %t40_<index> = OpLoad %float %<src0_value2>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>

         %90_<index> = OpLoad %float %<src0_value0>
         %91_<index> = OpBitcast %int %90_<index>
         %98_<index> = OpBitFieldSExtract %int %91_<index> %int_0 %int_6
        %101_<index> = OpBitFieldSExtract %int %91_<index> %int_8 %int_6
        %102_<index> = OpCompositeConstruct %v2int %98_<index> %101_<index>

         %130_<index> = OpConvertSToF %v2float %102_<index>
         %138_<index> = OpImage %ImageS %t38_<index>
        %139_<index> = OpImageQuerySizeLod %v2int %138_<index> %int_0
        %140_<index> = OpConvertSToF %v2float %139_<index>
        %141_<index> = OpFDiv %v2float %130_<index> %140_<index>
         %142_<index> = OpFAdd %v2float %t42_<index> %141_<index>

         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %142_<index> Lod %float_0_000000
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value);
		uint32_t destination = 0;
		for (uint32_t component = 0; component < 4; component++)
		{
			if ((inst.mimg_dmask & (1u << component)) == 0)
			{
				continue;
			}
			const auto dst_value = operand_variable_to_str(inst.dst, static_cast<int>(destination++));
			*dst_source += String8::FromPrintf(
			    "%%image_sample_lzo_%u_%u = OpCompositeExtract %%float %%t43_%u %u\nOpStore %%%s %%image_sample_lzo_%u_%u\n",
			    index, component, index, component, dst_value.value.c_str(), index, component);
		}

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLzO_Vdata1Vaddr4StSsDmask1)
{
	return RecompileImageSampleLzO(index, code, dst_source, spirv, param, scc_check);
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLzO_Vdata3Vaddr4StSsDmask7)
{
	return RecompileImageSampleLzO(index, code, dst_source, spirv, param, scc_check);
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLzO_VdataVaddr4StSsMimgDmask)
{
	return RecompileImageSampleLzO(index, code, dst_source, spirv, param, scc_check);
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata4Vaddr3StSsDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr &&
	    (bind_info->textures2D.textures2d_sampled_num > 0 || bind_info->textures2D.textures2d_array_sampled_num > 0) &&
	    bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (bind_info->textures2D.textures2d_array_sampled_num > 0)
		{
			const SpirvValue destinations[] = {dst_value0, dst_value1, dst_value2, dst_value3};
			return EmitTypedImageSample(dst_source, index, inst, spirv, src0_value0, src0_value1, src0_value2,
			                                      src1_value0, src2_value0, destinations, 4);
		}

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		if (PixelSampleProbeSelectsInstruction(spirv, index))
		{
			String8 sample_probe_source;
			if (!spirv->EmitPixelRgbaProbe(&sample_probe_source, index, String8::FromPrintf("%%t43_%u", index),
			                              "pixel_sample_probe"))
			{
				return false;
			}
			*dst_source += sample_probe_source;
		}

		return true;
	}

	if (bind_info != nullptr)
	{
		KYTY_LOG_DEBUG(
		             "ImageSample RGBA binding unavailable: sampled=%d textures=%d samplers=%d texture_sgpr=%d sampler_sgpr=%d\\n",
		             bind_info->textures2D.textures2d_sampled_num, bind_info->textures2D.textures_num,
		             bind_info->samplers.samplers_num, inst.src[1].register_id, inst.src[2].register_id);
	}

	return false;
}

// image_sample_b: RDNA address is {bias}{coords}. One multi-format emitter
// keeps compact dmask stores and routes through the typed sample path
// (descriptor shape selects flat / array / volume).
// IMAGE_SAMPLE with an enabled-component set that has no dedicated tuple.
KYTY_RECOMPILER_FUNC(Recompile_ImageSample_VdataVaddr3StSsMimgDmask)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// 1 = 2D (x,y); 5 = 2D array (x,y,slice).
	if ((inst.mimg_dimension != 1 && inst.mimg_dimension != 5) || bind_info == nullptr || bind_info->samplers.samplers_num <= 0)
	{
		return false;
	}

	uint32_t components[4] = {};
	int      num           = 0;
	for (uint32_t component = 0; component < 4u; ++component)
	{
		if ((inst.mimg_dmask & (1u << component)) != 0u) { components[num++] = component; }
	}
	if (num == 0 || num != inst.dst.size) { return false; }

	SpirvValue dst_value[4];
	for (int i = 0; i < num; i++)
	{
		dst_value[i] = operand_variable_to_str(inst.dst, i);
		if (dst_value[i].type != SpirvType::Float) { return false; }
	}
	const auto x       = mimg_address_to_str(inst, 0);
	const auto y       = mimg_address_to_str(inst, 1);
	const auto layer   = mimg_address_to_str(inst, 2);
	const auto texture = operand_variable_to_str(inst.src[1], 0);
	const auto sampler = operand_variable_to_str(inst.src[2], 0);
	if (x.type != SpirvType::Float || y.type != SpirvType::Float || texture.type != SpirvType::Uint || sampler.type != SpirvType::Uint)
	{
		return false;
	}
	return EmitTypedImageSample(dst_source, index, inst, spirv, x, y, layer, texture, sampler, dst_value, static_cast<uint32_t>(num),
	                            components);
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleB_VdataVaddrStSsMimgDmask)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// 1 = 2D (bias,x,y); 3 = cube (bias,x,y,face); 5 = 2D array (bias,x,y,slice).
	// Cube faces materialize as array layers (resource type 11).
	if (inst.mimg_dimension != 1 && inst.mimg_dimension != 3 && inst.mimg_dimension != 5)
	{
		return false;
	}

	if (bind_info == nullptr || bind_info->samplers.samplers_num <= 0 ||
	    (bind_info->textures2D.textures2d_sampled_num <= 0 && bind_info->textures2D.textures2d_array_sampled_num <= 0 &&
	     bind_info->textures2D.textures3d_sampled_num <= 0))
	{
		return false;
	}

	// DMASK enables components in R, G, B, A order; the results pack into consecutive VGPRs.
	uint32_t components[4] = {};
	int      num           = 0;
	for (uint32_t component = 0; component < 4u; ++component)
	{
		if ((inst.mimg_dmask & (1u << component)) != 0u) { components[num++] = component; }
	}
	if (num == 0 || num != inst.dst.size) { return false; }

	SpirvValue dst_value[4];
	for (int i = 0; i < num; i++)
	{
		dst_value[i] = operand_variable_to_str(inst.dst, i);
	}
	// Address: [0]=bias, [1]=x, [2]=y, [3]=layer/face when present.
	const auto src0_bias   = mimg_address_to_str(inst, 0);
	const auto src0_x      = mimg_address_to_str(inst, 1);
	const auto src0_y      = mimg_address_to_str(inst, 2);
	const auto src0_layer  = (inst.mimg_dimension == 3 || inst.mimg_dimension == 5) ? mimg_address_to_str(inst, 3) : src0_y;
	auto       src1_value0 = operand_variable_to_str(inst.src[1], 0);
	auto       src2_value0 = operand_variable_to_str(inst.src[2], 0);

	if (dst_value[0].type != SpirvType::Float)
	{
		return false;
	}
	if (src0_bias.type != SpirvType::Float || src0_x.type != SpirvType::Float || src0_y.type != SpirvType::Float)
	{
		return false;
	}
	if (src1_value0.type != SpirvType::Uint || src2_value0.type != SpirvType::Uint)
	{
		return false;
	}
	return EmitTypedImageSample(dst_source, index, inst, spirv, src0_x, src0_y, src0_layer, src1_value0, src2_value0,
	                                       dst_value, static_cast<uint32_t>(num), components, &src0_bias);
}

// IMAGE_SAMPLE_C_LZ samples LOD 0 and applies S# DEPTH_COMPARE_FUNC(ref, texel).
// Pure DepthReference bindings use Vulkan Dref so linear filtering performs
// compare-before-filter PCF. Mixed/color bindings retain the explicit sample
// and ALU comparison. Never hardcode LEQUAL: S# encodes 0..7.
static String8 EmitImageSampleCLzCompare(uint32_t index, uint8_t compare_func)
{
	const auto index_string = String8::FromPrintf("%u", index);
	const char* compare_op  = nullptr;
	switch (compare_func)
	{
		case 0: return String8("%image_dref_result_<index> = OpFMul %float %float_0_000000 %float_1_000000\n")
		            .ReplaceStr("<index>", index_string);
		case 1: compare_op = "OpFOrdLessThan"; break;
		case 2: compare_op = "OpFOrdEqual"; break;
		case 3: compare_op = "OpFOrdLessThanEqual"; break;
		case 4: compare_op = "OpFOrdGreaterThan"; break;
		case 5: compare_op = "OpFOrdNotEqual"; break;
		case 6: compare_op = "OpFOrdGreaterThanEqual"; break;
		case 7: return String8("%image_dref_result_<index> = OpFAdd %float %float_1_000000 %float_0_000000\n")
		            .ReplaceStr("<index>", index_string);
		default: return {};
	}
	return String8(R"(
%image_dref_passes_<index> = <compare_op> %bool %image_dref_reference_<index> %image_dref_texel_<index>
%image_dref_result_<index> = OpSelect %float %image_dref_passes_<index> %float_1_000000 %float_0_000000
)")
	    .ReplaceStr("<index>", index_string)
	    .ReplaceStr("<compare_op>", compare_op);
}

static bool SupportsArrayComparisonSampler(const ShaderSamplerResource& sampler)
{
	return !sampler.ForceUnormCoords() && sampler.MinLod() == 0u && sampler.FilterMode() == 0u &&
	       sampler.XyMagFilter() <= 1u && sampler.XyMinFilter() <= 1u;
}

static String8 EmitArrayComparisonFilter(uint32_t index, uint8_t compare_func)
{
	const auto index_string = String8::FromPrintf("%u", index);
	String8 text;
	// Gather order is (i0,j1), (i1,j1), (i1,j0), (i0,j0). Compare each
	// wrapped/border-replaced texel before interpolating the four results.
	for (uint32_t tap = 0; tap < 4u; tap++)
	{
		const auto suffix = String8::FromPrintf("_%u", tap);
		text += String8::FromPrintf("%%image_dref_texel_%u_%u = OpCompositeExtract %%float %%image_dref_gather_%u %u\n",
		                            index, tap, index, tap);
		text += EmitImageSampleCLzCompare(index, compare_func)
		            .ReplaceStr("%image_dref_texel_" + index_string, "%image_dref_texel_" + index_string + suffix)
		            .ReplaceStr("%image_dref_passes_" + index_string, "%image_dref_passes_" + index_string + suffix)
		            .ReplaceStr("%image_dref_result_" + index_string, "%image_dref_result_" + index_string + suffix);
	}
	text += String8(R"(
%image_dref_extent_<index> = OpImageQuerySizeLod %v3int %image_dref_image_<index> %int_0
%image_dref_width_<index> = OpCompositeExtract %int %image_dref_extent_<index> 0
%image_dref_height_<index> = OpCompositeExtract %int %image_dref_extent_<index> 1
%image_dref_width_float_<index> = OpConvertSToF %float %image_dref_width_<index>
%image_dref_height_float_<index> = OpConvertSToF %float %image_dref_height_<index>
%image_dref_u_<index> = OpFMul %float %image_dref_x_<index> %image_dref_width_float_<index>
%image_dref_v_<index> = OpFMul %float %image_dref_y_<index> %image_dref_height_float_<index>
%image_dref_center_u_<index> = OpFSub %float %image_dref_u_<index> %float_0_500000
%image_dref_center_v_<index> = OpFSub %float %image_dref_v_<index> %float_0_500000
%image_dref_weight_u_<index> = OpExtInst %float %GLSL_std_450 Fract %image_dref_center_u_<index>
%image_dref_weight_v_<index> = OpExtInst %float %GLSL_std_450 Fract %image_dref_center_v_<index>
%image_dref_lower_<index> = OpExtInst %float %GLSL_std_450 FMix %image_dref_result_<index>_3 %image_dref_result_<index>_2 %image_dref_weight_u_<index>
%image_dref_upper_<index> = OpExtInst %float %GLSL_std_450 FMix %image_dref_result_<index>_0 %image_dref_result_<index>_1 %image_dref_weight_u_<index>
%image_dref_result_<index> = OpExtInst %float %GLSL_std_450 FMix %image_dref_lower_<index> %image_dref_upper_<index> %image_dref_weight_v_<index>
)").ReplaceStr("<index>", index_string);
	return text;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleDrefLz_Vdata1Vaddr3StSsDmask1)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr || inst.mimg_dmask != 0x1 || inst.dst.size != 1)
	{
		return false;
	}

	const auto* vs_info = spirv->GetVsInputInfo();
	const int   user_data_register_base = (vs_info != nullptr && vs_info->gs_prolog ? 8 : 0);
	const int   texture_index = ShaderFindImageSampledTextureDescriptor(inst, *bind_info, user_data_register_base);
	const int   sampler_index = ShaderFindImageSamplerDescriptor(inst, *bind_info, user_data_register_base);
	if (texture_index < 0 || sampler_index < 0)
	{
		return false;
	}

	const auto& descriptor = bind_info->textures2D.desc[texture_index];
	const auto shape   = ShaderResolvedSampledTextureShape(descriptor);
	const bool flat    = shape == ShaderGen5SampledTextureShape::TwoDimensional;
	const bool arrayed = shape == ShaderGen5SampledTextureShape::TwoDimensionalArray;
	const bool depth_view = flat && descriptor.sample_operation == State::ImageSampleOperation::DepthReference;
	const bool comparison_sampled =
	    depth_view && ShaderSamplerDepthComparisonEligible(bind_info->textures2D, bind_info->samplers, sampler_index);
	const auto& sampler = bind_info->samplers.samplers[sampler_index];
	if (arrayed && !SupportsArrayComparisonSampler(sampler))
	{
		return false;
	}
	const bool manual_linear = arrayed && sampler.XyMagFilter() == 1u;
	if ((!flat && !arrayed) || (flat && inst.mimg_dimension != 1u) ||
	    (arrayed && inst.mimg_dimension != 3u && inst.mimg_dimension != 5u))
	{
		return false;
	}
	if ((depth_view && bind_info->textures2D.textures2d_sampled_depth_num <= 0) ||
	    (flat && !depth_view && bind_info->textures2D.textures2d_sampled_num <= 0) ||
	    (arrayed && bind_info->textures2D.textures2d_array_sampled_num <= 0))
	{
		return false;
	}

	const uint32_t address_num = flat ? 3u : 4u;
	if (inst.src[0].size != static_cast<int>(address_num) ||
	    (inst.mimg_address_num != 0 && inst.mimg_address_num < static_cast<int>(address_num)))
	{
		return false;
	}

	const auto dst_value     = operand_variable_to_str(inst.dst);
	const auto dref_value    = mimg_address_to_str(inst, 0);
	const auto x_value       = mimg_address_to_str(inst, 1);
	const auto y_value       = mimg_address_to_str(inst, 2);
	const auto layer_value   = arrayed ? mimg_address_to_str(inst, 3) : y_value;
	const auto texture_value = operand_variable_to_str(inst.src[1], 0);
	const auto sampler_value = operand_variable_to_str(inst.src[2], 0);
	if (dst_value.type != SpirvType::Float || dref_value.type != SpirvType::Float || x_value.type != SpirvType::Float ||
	    y_value.type != SpirvType::Float || (arrayed && layer_value.type != SpirvType::Float) ||
	    texture_value.type != SpirvType::Uint || sampler_value.type != SpirvType::Uint)
	{
		return false;
	}

	const uint8_t compare_func = sampler.DepthCompareFunc();
	const String8 compare_text = EmitImageSampleCLzCompare(index, compare_func);
	if (compare_text.IsEmpty())
	{
		return false;
	}

	static const char* flat_text = R"(
%image_dref_texture_raw_<index> = OpLoad %uint %<texture>
%image_dref_texture_<index> = OpBitwiseAnd %uint %image_dref_texture_raw_<index> %uint_0x1fffffff
%image_dref_image_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %image_dref_texture_<index>
%image_dref_image_<index> = OpLoad %ImageS %image_dref_image_ptr_<index>
%image_dref_sampler_index_<index> = OpLoad %uint %<sampler>
%image_dref_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_dref_sampler_index_<index>
%image_dref_sampler_<index> = OpLoad %Sampler %image_dref_sampler_ptr_<index>
%image_dref_sampled_<index> = OpSampledImage %SampledImage %image_dref_image_<index> %image_dref_sampler_<index>
%image_dref_reference_<index> = OpLoad %float %<dref>
%image_dref_x_<index> = OpLoad %float %<x>
%image_dref_y_<index> = OpLoad %float %<y>
%image_dref_coordinate_<index> = OpCompositeConstruct %v2float %image_dref_x_<index> %image_dref_y_<index>
%image_dref_sample_<index> = OpImageSampleExplicitLod %v4float %image_dref_sampled_<index> %image_dref_coordinate_<index> Lod %float_0_000000
%image_dref_texel_<index> = OpCompositeExtract %float %image_dref_sample_<index> 0
)";
	static const char* flat_depth_text = R"(
%image_dref_texture_raw_<index> = OpLoad %uint %<texture>
%image_dref_texture_<index> = OpBitwiseAnd %uint %image_dref_texture_raw_<index> %uint_0x1fffffff
%image_dref_image_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageSD %textures2D_SD %image_dref_texture_<index>
%image_dref_image_<index> = OpLoad %ImageSD %image_dref_image_ptr_<index>
%image_dref_sampler_index_<index> = OpLoad %uint %<sampler>
%image_dref_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_dref_sampler_index_<index>
%image_dref_sampler_<index> = OpLoad %Sampler %image_dref_sampler_ptr_<index>
%image_dref_sampled_<index> = OpSampledImage %SampledImageD %image_dref_image_<index> %image_dref_sampler_<index>
%image_dref_reference_<index> = OpLoad %float %<dref>
%image_dref_x_<index> = OpLoad %float %<x>
%image_dref_y_<index> = OpLoad %float %<y>
%image_dref_coordinate_<index> = OpCompositeConstruct %v2float %image_dref_x_<index> %image_dref_y_<index>
%image_dref_sample_<index> = OpImageSampleExplicitLod %v4float %image_dref_sampled_<index> %image_dref_coordinate_<index> Lod %float_0_000000
%image_dref_texel_<index> = OpCompositeExtract %float %image_dref_sample_<index> 0
)";
	static const char* flat_depth_comparison_text = R"(
%image_dref_texture_raw_<index> = OpLoad %uint %<texture>
%image_dref_texture_<index> = OpBitwiseAnd %uint %image_dref_texture_raw_<index> %uint_0x1fffffff
%image_dref_image_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageSD %textures2D_SD %image_dref_texture_<index>
%image_dref_image_<index> = OpLoad %ImageSD %image_dref_image_ptr_<index>
%image_dref_sampler_index_<index> = OpLoad %uint %<sampler>
%image_dref_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_dref_sampler_index_<index>
%image_dref_sampler_<index> = OpLoad %Sampler %image_dref_sampler_ptr_<index>
%image_dref_sampled_<index> = OpSampledImage %SampledImageD %image_dref_image_<index> %image_dref_sampler_<index>
%image_dref_reference_<index> = OpLoad %float %<dref>
%image_dref_x_<index> = OpLoad %float %<x>
%image_dref_y_<index> = OpLoad %float %<y>
%image_dref_coordinate_<index> = OpCompositeConstruct %v2float %image_dref_x_<index> %image_dref_y_<index>
%image_dref_result_<index> = OpImageSampleDrefExplicitLod %float %image_dref_sampled_<index> %image_dref_coordinate_<index> %image_dref_reference_<index> Lod %float_0_000000
)";
	static const char* array_text = R"(
%image_dref_texture_raw_<index> = OpLoad %uint %<texture>
%image_dref_texture_<index> = OpBitwiseAnd %uint %image_dref_texture_raw_<index> %uint_0x1fffffff
%image_dref_image_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageSA %textures2DA_S %image_dref_texture_<index>
%image_dref_image_<index> = OpLoad %ImageSA %image_dref_image_ptr_<index>
%image_dref_sampler_index_<index> = OpLoad %uint %<sampler>
%image_dref_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_dref_sampler_index_<index>
%image_dref_sampler_<index> = OpLoad %Sampler %image_dref_sampler_ptr_<index>
%image_dref_sampled_<index> = OpSampledImage %SampledImageA %image_dref_image_<index> %image_dref_sampler_<index>
%image_dref_reference_<index> = OpLoad %float %<dref>
%image_dref_x_<index> = OpLoad %float %<x>
%image_dref_y_<index> = OpLoad %float %<y>
%image_dref_layer_<index> = OpLoad %float %<layer>
%image_dref_coordinate_<index> = OpCompositeConstruct %v3float %image_dref_x_<index> %image_dref_y_<index> %image_dref_layer_<index>
<array_sample>
)";
	static const char* array_nearest_text = R"(
%image_dref_sample_<index> = OpImageSampleExplicitLod %v4float %image_dref_sampled_<index> %image_dref_coordinate_<index> Lod %float_0_000000
%image_dref_texel_<index> = OpCompositeExtract %float %image_dref_sample_<index> 0
)";
	static const char* array_linear_text = R"(
%image_dref_gather_<index> = OpImageGather %v4float %image_dref_sampled_<index> %image_dref_coordinate_<index> %int_0
)";

	const char* sample_text = comparison_sampled ? flat_depth_comparison_text : (depth_view ? flat_depth_text : (flat ? flat_text : array_text));
	*dst_source += String8(sample_text)
	                   .ReplaceStr("<array_sample>", manual_linear ? array_linear_text : array_nearest_text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<texture>", texture_value.value)
	                   .ReplaceStr("<sampler>", sampler_value.value)
	                   .ReplaceStr("<dref>", dref_value.value)
	                   .ReplaceStr("<x>", x_value.value)
	                   .ReplaceStr("<y>", y_value.value)
	                   .ReplaceStr("<layer>", layer_value.value);
	if (manual_linear)
	{
		*dst_source += EmitArrayComparisonFilter(index, compare_func);
	} else if (!comparison_sampled)
	{
		*dst_source += compare_text;
	}
	*dst_source += String8("OpStore %<destination> %image_dref_result_<index>\n")
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<destination>", dst_value.value);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLz_Vdata4Vaddr3StSsDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info == nullptr || bind_info->samplers.samplers_num <= 0)
	{
		return false;
	}

	const auto* vs_info = spirv->GetVsInputInfo();
	const int   user_data_register_base = (vs_info != nullptr && vs_info->gs_prolog ? 8 : 0);
	const auto  plan                    = PlanImageSampleLz(inst, *bind_info, user_data_register_base);

	if (plan.shape == ShaderGen5SampledTextureShape::TwoDimensionalArray &&
	    bind_info->textures2D.textures2d_array_sampled_num > 0)
	{
		const int descriptor = ShaderFindImageSampledTextureDescriptor(inst, *bind_info, user_data_register_base);
		if (Config::IsNextGen() &&
		    VulkanGen5ImageNumericType(bind_info->textures2D.desc[descriptor].texture.Format()) != GuestImageNumericType::FloatingPoint)
		{
			return false;
		}
		ValidateImageSampleLzAddresses(inst, plan.coordinate_num);
		const SpirvValue destinations[] = {operand_variable_to_str(inst.dst, 0), operand_variable_to_str(inst.dst, 1),
		                                    operand_variable_to_str(inst.dst, 2), operand_variable_to_str(inst.dst, 3)};
		return EmitTypedImageSample(dst_source, index, inst, spirv, mimg_address_to_str(inst, 0), mimg_address_to_str(inst, 1),
		                            mimg_address_to_str(inst, 2), operand_variable_to_str(inst.src[1], 0),
		                            operand_variable_to_str(inst.src[2], 0), destinations, 4);
	}

	if (plan.shape == ShaderGen5SampledTextureShape::TwoDimensional && bind_info->textures2D.textures2d_sampled_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		if (PixelSampleProbeSelectsInstruction(spirv, index))
		{
			String8 sample_probe_source;
			if (!spirv->EmitPixelRgbaProbe(&sample_probe_source, index, String8::FromPrintf("%%t43_%u", index),
			                              "pixel_sample_probe"))
			{
				return false;
			}
			*dst_source += sample_probe_source;
		}
		return true;
	}

	if (plan.shape == ShaderGen5SampledTextureShape::ThreeDimensional && bind_info->textures2D.textures3d_sampled_num > 0)
	{
		ValidateImageSampleLzAddresses(inst, 3u);

		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		if (dst_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value0.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src0_value2.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value2.type != SpirvType::Float condition ignored (continuing)\n"); }
		if (src1_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }
		if (src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

		static const char* volume_text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS3D %textures3D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS3D %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage3D %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t41_<index> = OpLoad %float %<src0_value2>
         %t42_<index> = OpCompositeConstruct %v3float %t39_<index> %t40_<index> %t41_<index>
         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		*dst_source += String8(volume_text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

static bool RecompileCubeImageSampleL(uint32_t destination_num, KYTY_RECOMPILER_ARGS)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.mimg_dimension != 3u || destination_num == 0 || destination_num > 4)
	{
		return false;
	}

	const auto* bind = spirv->GetBindInfo();
	if (bind == nullptr || bind->textures2D.textures2d_array_sampled_num <= 0 || bind->samplers.samplers_num <= 0)
	{
		return false;
	}

	const auto x       = mimg_address_to_str(inst, 0);
	const auto y       = mimg_address_to_str(inst, 1);
	const auto layer   = mimg_address_to_str(inst, 2);
	const auto lod     = mimg_address_to_str(inst, 3);
	const auto texture = operand_variable_to_str(inst.src[1], 0);
	const auto sampler = operand_variable_to_str(inst.src[2], 0);
	if (x.type != SpirvType::Float || y.type != SpirvType::Float || layer.type != SpirvType::Float ||
	    lod.type != SpirvType::Float || texture.type != SpirvType::Uint || sampler.type != SpirvType::Uint)
	{
		return false;
	}

	SpirvValue destinations[4];
	for (uint32_t component = 0; component < destination_num; ++component)
	{
		destinations[component] = operand_variable_to_str(inst.dst, static_cast<int>(component));
		if (destinations[component].type != SpirvType::Float)
		{
			return false;
		}
	}

	const auto index_string = String8::FromPrintf("%u", index);
	String8    source = String8(R"(
%image_sample_l_descriptor_raw_<index> = OpLoad %uint %<texture>
%image_sample_l_descriptor_<index> = OpBitwiseAnd %uint %image_sample_l_descriptor_raw_<index> %uint_0x1fffffff
%image_sample_l_image_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageSA %textures2DA_S %image_sample_l_descriptor_<index>
%image_sample_l_image_<index> = OpLoad %ImageSA %image_sample_l_image_ptr_<index>
%image_sample_l_sampler_index_<index> = OpLoad %uint %<sampler>
%image_sample_l_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %image_sample_l_sampler_index_<index>
%image_sample_l_sampler_<index> = OpLoad %Sampler %image_sample_l_sampler_ptr_<index>
%image_sample_l_sampled_<index> = OpSampledImage %SampledImageA %image_sample_l_image_<index> %image_sample_l_sampler_<index>
%image_sample_l_x_raw_<index> = OpLoad %float %<x>
%image_sample_l_y_raw_<index> = OpLoad %float %<y>
%image_sample_l_layer_<index> = OpLoad %float %<layer>
%image_sample_l_lod_<index> = OpLoad %float %<lod>
%image_sample_l_x_<index> = OpFSub %float %image_sample_l_x_raw_<index> %float_1_000000
%image_sample_l_y_<index> = OpFSub %float %image_sample_l_y_raw_<index> %float_1_000000
%image_sample_l_coordinate_<index> = OpCompositeConstruct %v3float %image_sample_l_x_<index> %image_sample_l_y_<index> %image_sample_l_layer_<index>
%image_sample_l_value_<index> = OpImageSampleExplicitLod %v4float %image_sample_l_sampled_<index> %image_sample_l_coordinate_<index> Lod %image_sample_l_lod_<index>
)")
	                         .ReplaceStr("<index>", index_string)
	                         .ReplaceStr("<texture>", texture.value)
	                         .ReplaceStr("<sampler>", sampler.value)
	                         .ReplaceStr("<x>", x.value)
	                         .ReplaceStr("<y>", y.value)
	                         .ReplaceStr("<layer>", layer.value)
	                         .ReplaceStr("<lod>", lod.value);

	uint32_t destination = 0;
	for (uint32_t component = 0; component < 4u; ++component)
	{
		if ((inst.mimg_dmask & (1u << component)) == 0u)
		{
			continue;
		}
		if (destination >= destination_num)
		{
			return false;
		}
		source += String8(R"(
%image_sample_l_component_<index>_<component> = OpCompositeExtract %float %image_sample_l_value_<index> <component>
OpStore %<destination> %image_sample_l_component_<index>_<component>
)")
		              .ReplaceStr("<index>", index_string)
		              .ReplaceStr("<component>", String8::FromPrintf("%u", component))
		              .ReplaceStr("<destination>", destinations[destination].value);
		++destination;
	}
	if (destination != destination_num)
	{
		return false;
	}

	*dst_source += source;
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleL_Vdata4Vaddr3StSsDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (inst.mimg_dimension == 3u)
	{
		return RecompileCubeImageSampleL(4, index, code, dst_source, spirv, param, scc_check);
	}

	if (bind_info == nullptr || bind_info->textures2D.textures2d_sampled_num <= 0 || bind_info->samplers.samplers_num <= 0)
	{
		return false;
	}

	const auto dst_value0  = operand_variable_to_str(inst.dst, 0);
	const auto dst_value1  = operand_variable_to_str(inst.dst, 1);
	const auto dst_value2  = operand_variable_to_str(inst.dst, 2);
	const auto dst_value3  = operand_variable_to_str(inst.dst, 3);
	const auto src0_value0 = mimg_address_to_str(inst, 0);
	const auto src0_value1 = mimg_address_to_str(inst, 1);
	const auto src0_value2 = mimg_address_to_str(inst, 2);
	const auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
	const auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

	if (dst_value0.type != SpirvType::Float || dst_value1.type != SpirvType::Float ||
	                     dst_value2.type != SpirvType::Float || dst_value3.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float || dst_value1.type != SpirvType::Float || condition ignored (continuing)\n"); }
	if (src0_value0.type != SpirvType::Float || src0_value1.type != SpirvType::Float ||
	                     src0_value2.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float || src0_value1.type != SpirvType::Float || condition ignored (continuing)\n"); }
	if (src1_value0.type != SpirvType::Uint || src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint || src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

	if (bind_info->textures2D.textures3d_sampled_num > 0)
	{
		static const char* tagged_text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t25_<index> = OpBitwiseAnd %uint %t24_<index> %uint_0x80000000
         %t26_<index> = OpINotEqual %bool %t25_<index> %uint_0
         %t27_<index> = OpBitwiseAnd %uint %t24_<index> %uint_0x7fffffff
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t41_<index> = OpLoad %float %<src0_value2>
               OpSelectionMerge %image_sample_l_merge_<index> None
               OpBranchConditional %t26_<index> %image_sample_l_3d_<index> %image_sample_l_2d_<index>
%image_sample_l_2d_<index> = OpLabel
         %t42_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t27_<index>
         %t43_<index> = OpLoad %ImageS %t42_<index>
         %t44_<index> = OpSampledImage %SampledImage %t43_<index> %t36_<index>
         %t45_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t46_<index> = OpImageSampleExplicitLod %v4float %t44_<index> %t45_<index> Lod %t41_<index>
               OpBranch %image_sample_l_merge_<index>
%image_sample_l_3d_<index> = OpLabel
         %t47_<index> = OpAccessChain %_ptr_UniformConstant_ImageS3D %textures3D_S %t27_<index>
         %t48_<index> = OpLoad %ImageS3D %t47_<index>
         %t49_<index> = OpSampledImage %SampledImage3D %t48_<index> %t36_<index>
         %t50_<index> = OpCompositeConstruct %v3float %t39_<index> %t40_<index> %t41_<index>
         %t51_<index> = OpImageSampleExplicitLod %v4float %t49_<index> %t50_<index> Lod %float_0_000000
               OpBranch %image_sample_l_merge_<index>
%image_sample_l_merge_<index> = OpLabel
         %t43_result_<index> = OpPhi %v4float %t46_<index> %image_sample_l_2d_<index> %t51_<index> %image_sample_l_3d_<index>
               OpStore %temp_v4float %t43_result_<index>
         %t52_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t53_<index> = OpLoad %float %t52_<index>
               OpStore %<dst_value0> %t53_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
         %t56_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t57_<index> = OpLoad %float %t56_<index>
               OpStore %<dst_value2> %t57_<index>
         %t58_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t59_<index> = OpLoad %float %t58_<index>
               OpStore %<dst_value3> %t59_<index>
)";
		*dst_source += String8(tagged_text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);
		return true;
	}

	static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t41_<index> = OpLoad %float %<src0_value2>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %t41_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0_value0>", src0_value0.value)
	                   .ReplaceStr("<src0_value1>", src0_value1.value)
	                   .ReplaceStr("<src0_value2>", src0_value2.value)
	                   .ReplaceStr("<src1_value0>", src1_value0.value)
	                   .ReplaceStr("<src2_value0>", src2_value0.value)
	                   .ReplaceStr("<dst_value0>", dst_value0.value)
	                   .ReplaceStr("<dst_value1>", dst_value1.value)
	                   .ReplaceStr("<dst_value2>", dst_value2.value)
	                   .ReplaceStr("<dst_value3>", dst_value3.value);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleL_Vdata3Vaddr3StSsDmask7)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (inst.mimg_dimension == 3u)
	{
		return RecompileCubeImageSampleL(3, index, code, dst_source, spirv, param, scc_check);
	}
	if (bind_info == nullptr || bind_info->textures2D.textures2d_sampled_num <= 0 || bind_info->samplers.samplers_num <= 0)
	{
		return false;
	}

	const auto dst_value0  = operand_variable_to_str(inst.dst, 0);
	const auto dst_value1  = operand_variable_to_str(inst.dst, 1);
	const auto dst_value2  = operand_variable_to_str(inst.dst, 2);
	const auto src0_value0 = mimg_address_to_str(inst, 0);
	const auto src0_value1 = mimg_address_to_str(inst, 1);
	const auto src0_value2 = mimg_address_to_str(inst, 2);
	const auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
	const auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

	if (dst_value0.type != SpirvType::Float || dst_value1.type != SpirvType::Float ||
	                     dst_value2.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float || dst_value1.type != SpirvType::Float || condition ignored (continuing)\n"); }
	if (src0_value0.type != SpirvType::Float || src0_value1.type != SpirvType::Float ||
	                     src0_value2.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float || src0_value1.type != SpirvType::Float || condition ignored (continuing)\n"); }
	if (src1_value0.type != SpirvType::Uint || src2_value0.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src1_value0.type != SpirvType::Uint || src2_value0.type != SpirvType::Uint condition ignored (continuing)\n"); }

	static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t25_<index> = OpBitwiseAnd %uint %t24_<index> %uint_0x1fffffff
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t25_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t41_<index> = OpLoad %float %<src0_value2>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %t41_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0_value0>", src0_value0.value)
	                   .ReplaceStr("<src0_value1>", src0_value1.value)
	                   .ReplaceStr("<src0_value2>", src0_value2.value)
	                   .ReplaceStr("<src1_value0>", src1_value0.value)
	                   .ReplaceStr("<src2_value0>", src2_value0.value)
	                   .ReplaceStr("<dst_value0>", dst_value0.value)
	                   .ReplaceStr("<dst_value1>", dst_value1.value)
	                   .ReplaceStr("<dst_value2>", dst_value2.value);

	return true;
}

enum class SampledImageShape
{
	Flat2d,
	Array2d,
	ThreeDimensional
};

struct SampledImageTypeInfo
{
	const char* suffix;
	const char* pointer_type;
	const char* variable;
	const char* image_type;
	const char* sampled_image_type;
	const char* size_type;
	const char* float_coordinate_type;
	bool        has_third_dimension;
};

static SampledImageTypeInfo GetSampledImageTypeInfo(SampledImageShape shape, bool uint_images = false, bool shared_uint_type = false)
{
	// Descriptor banks retain their own variables and indices. In a uint-only
	// sampled domain WriteTypes gives both banks the primary S element type.
	switch (shape)
	{
		case SampledImageShape::Flat2d:
			return uint_images ? SampledImageTypeInfo {"flat_uint", "_ptr_UniformConstant_ImageU", "textures2D_U",
			                                         shared_uint_type ? "ImageS" : "ImageU", shared_uint_type ? "SampledImage" : "SampledImageU",
			                                         "v2int", "v2float", false}
			                   : SampledImageTypeInfo {"flat", "_ptr_UniformConstant_ImageS", "textures2D_S", "ImageS", "SampledImage", "v2int", "v2float", false};
		case SampledImageShape::Array2d:
			return uint_images ? SampledImageTypeInfo {"array_uint", "_ptr_UniformConstant_ImageUA", "textures2DA_U",
			                                         shared_uint_type ? "ImageSA" : "ImageUA", shared_uint_type ? "SampledImageA" : "SampledImageUA",
			                                         "v3int", "v3float", true}
			                   : SampledImageTypeInfo {"array", "_ptr_UniformConstant_ImageSA", "textures2DA_S", "ImageSA", "SampledImageA", "v3int", "v3float", true};
		case SampledImageShape::ThreeDimensional:
			return uint_images ? SampledImageTypeInfo {"3d_uint", "_ptr_UniformConstant_ImageU3D", "textures3D_U",
			                                         shared_uint_type ? "ImageS3D" : "ImageU3D", shared_uint_type ? "SampledImage3D" : "SampledImageU3D",
			                                         "v3int", "v3float", true}
			                   : SampledImageTypeInfo {"3d", "_ptr_UniformConstant_ImageS3D", "textures3D_S", "ImageS3D", "SampledImage3D", "v3int", "v3float", true};
	}
	EXIT("unknown sampled image shape\n");
	return {};
}

// IMAGE_SAMPLE_L with an enabled-component set that has no dedicated tuple.
// The address is the DIM's coordinates followed by the LOD; the T# shape
// selects the descriptor bank and a cube samples its faces as array layers.
KYTY_RECOMPILER_FUNC(Recompile_ImageSampleL_VdataVaddrStSsMimgDmask)
{
	const auto& inst = code.GetInstructions().At(index);
	if (inst.mimg_dimension == 3u)
	{
		return RecompileCubeImageSampleL(static_cast<uint32_t>(inst.dst.size), index, code, dst_source, spirv, param, scc_check);
	}

	const auto* bind = spirv->GetBindInfo();
	if (bind == nullptr || bind->samplers.samplers_num <= 0)
	{
		return false;
	}
	const auto* vs_info                 = spirv->GetVsInputInfo();
	const int   user_data_register_base = (vs_info != nullptr && vs_info->gs_prolog ? 8 : 0);
	const int   descriptor_index        = ShaderFindImageSampledTextureDescriptor(inst, *bind, user_data_register_base);
	if (descriptor_index < 0)
	{
		return false;
	}
	const auto descriptor_shape = ShaderResolvedSampledTextureShape(bind->textures2D.desc[descriptor_index]);

	// 1 = 2D (x, y); 2 = 3D (x, y, z); 5 = 2D array (x, y, slice).
	SampledImageShape shape       = SampledImageShape::Flat2d;
	int               sampled_num = 0;
	if (inst.mimg_dimension == 1u && descriptor_shape == ShaderGen5SampledTextureShape::TwoDimensional)
	{
		sampled_num = bind->textures2D.textures2d_sampled_num;
	} else if (inst.mimg_dimension == 2u && descriptor_shape == ShaderGen5SampledTextureShape::ThreeDimensional)
	{
		shape       = SampledImageShape::ThreeDimensional;
		sampled_num = bind->textures2D.textures3d_sampled_num;
	} else if (inst.mimg_dimension == 5u && descriptor_shape == ShaderGen5SampledTextureShape::TwoDimensionalArray)
	{
		shape       = SampledImageShape::Array2d;
		sampled_num = bind->textures2D.textures2d_array_sampled_num;
	}
	if (sampled_num <= 0)
	{
		return false;
	}
	const auto type_info      = GetSampledImageTypeInfo(shape);
	const int  coordinate_num = type_info.has_third_dimension ? 3 : 2;

	const auto texture = operand_variable_to_str(inst.src[1], 0);
	const auto sampler = operand_variable_to_str(inst.src[2], 0);
	const auto lod     = mimg_address_to_str(inst, coordinate_num);
	if (texture.type != SpirvType::Uint || sampler.type != SpirvType::Uint || lod.type != SpirvType::Float)
	{
		return false;
	}

	const auto index_string = String8::FromPrintf("%u", index);
	String8    coordinate_ids;
	String8    source;
	for (int coordinate = 0; coordinate < coordinate_num; ++coordinate)
	{
		const auto value = mimg_address_to_str(inst, coordinate);
		if (value.type != SpirvType::Float)
		{
			return false;
		}
		source += String8::FromPrintf("%%sample_l_coordinate_%s_%d = OpLoad %%float %%%s\n", index_string.c_str(), coordinate,
		                              value.value.c_str());
		coordinate_ids += String8::FromPrintf(" %%sample_l_coordinate_%s_%d", index_string.c_str(), coordinate);
	}
	source += String8(R"(
%sample_l_descriptor_raw_<index> = OpLoad %uint %<texture>
%sample_l_descriptor_<index> = OpBitwiseAnd %uint %sample_l_descriptor_raw_<index> %uint_0x1fffffff
%sample_l_image_ptr_<index> = OpAccessChain %<pointer_type> %<variable> %sample_l_descriptor_<index>
%sample_l_image_<index> = OpLoad %<image_type> %sample_l_image_ptr_<index>
%sample_l_sampler_index_<index> = OpLoad %uint %<sampler>
%sample_l_sampler_ptr_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %sample_l_sampler_index_<index>
%sample_l_sampler_<index> = OpLoad %Sampler %sample_l_sampler_ptr_<index>
%sample_l_sampled_<index> = OpSampledImage %<sampled_image_type> %sample_l_image_<index> %sample_l_sampler_<index>
%sample_l_lod_<index> = OpLoad %float %<lod>
%sample_l_coordinate_<index> = OpCompositeConstruct %<coordinate_type><coordinates>
%sample_l_value_<index> = OpImageSampleExplicitLod %v4float %sample_l_sampled_<index> %sample_l_coordinate_<index> Lod %sample_l_lod_<index>
)")
	              .ReplaceStr("<index>", index_string)
	              .ReplaceStr("<texture>", texture.value)
	              .ReplaceStr("<sampler>", sampler.value)
	              .ReplaceStr("<lod>", lod.value)
	              .ReplaceStr("<pointer_type>", type_info.pointer_type)
	              .ReplaceStr("<variable>", type_info.variable)
	              .ReplaceStr("<image_type>", type_info.image_type)
	              .ReplaceStr("<sampled_image_type>", type_info.sampled_image_type)
	              .ReplaceStr("<coordinate_type>", type_info.float_coordinate_type)
	              .ReplaceStr("<coordinates>", coordinate_ids);

	// DMASK enables components in R, G, B, A order; the results pack into consecutive VGPRs.
	int destination = 0;
	for (uint32_t component = 0; component < 4u; ++component)
	{
		if ((inst.mimg_dmask & (1u << component)) == 0u)
		{
			continue;
		}
		if (destination >= inst.dst.size)
		{
			return false;
		}
		const auto dst = operand_variable_to_str(inst.dst, destination++);
		if (dst.type != SpirvType::Float)
		{
			return false;
		}
		source += String8(R"(
%sample_l_component_<index>_<component> = OpCompositeExtract %float %sample_l_value_<index> <component>
OpStore %<dst> %sample_l_component_<index>_<component>
)")
		              .ReplaceStr("<index>", index_string)
		              .ReplaceStr("<component>", String8::FromPrintf("%u", component))
		              .ReplaceStr("<dst>", dst.value);
	}
	if (destination != inst.dst.size)
	{
		return false;
	}

	*dst_source += source;
	return true;
}

static String8 EmitImageResinfoQuery(uint32_t index, SampledImageShape shape, const String8& descriptor_index,
	                                 const String8& lod, bool uint_images = false, bool shared_uint_type = false)
{
	const auto type_info = GetSampledImageTypeInfo(shape, uint_images, shared_uint_type);
	const auto prefix = String8::FromPrintf("image_resinfo_%s_%u", type_info.suffix, index);
	const auto third_definition =
	    (type_info.has_third_dimension ?
	         String8(R"(
%<prefix>_third_i = OpCompositeExtract %int %<prefix>_size 2
%<prefix>_third = OpBitcast %uint %<prefix>_third_i
)") :
	         String8(""));
	const auto third_value = type_info.has_third_dimension ? String8("<prefix>_third") : String8("uint_1");

	static const char* text = R"(
%<prefix>_ptr = OpAccessChain %<pointer_type> %<variable> <descriptor_index>
%<prefix>_image = OpLoad %<image_type> %<prefix>_ptr
%<prefix>_size = OpImageQuerySizeLod %<size_type> %<prefix>_image <lod>
%<prefix>_width_i = OpCompositeExtract %int %<prefix>_size 0
%<prefix>_width = OpBitcast %uint %<prefix>_width_i
%<prefix>_height_i = OpCompositeExtract %int %<prefix>_size 1
%<prefix>_height = OpBitcast %uint %<prefix>_height_i
<third_definition>%<prefix>_levels_i = OpImageQueryLevels %int %<prefix>_image
%<prefix>_levels = OpBitcast %uint %<prefix>_levels_i
%<prefix>_result = OpCompositeConstruct %v4uint %<prefix>_width %<prefix>_height %<third_value> %<prefix>_levels
)";

	return String8(text)
	    .ReplaceStr("<prefix>", prefix)
	    .ReplaceStr("<pointer_type>", type_info.pointer_type)
	    .ReplaceStr("<variable>", type_info.variable)
	    .ReplaceStr("<image_type>", type_info.image_type)
	    .ReplaceStr("<size_type>", type_info.size_type)
	    .ReplaceStr("<descriptor_index>", descriptor_index)
	    .ReplaceStr("<lod>", lod)
	    .ReplaceStr("<third_definition>", third_definition.ReplaceStr("<prefix>", prefix))
	    .ReplaceStr("<third_value>", third_value.ReplaceStr("<prefix>", prefix));
}

static String8 ImageResinfoResultName(uint32_t index, SampledImageShape shape, bool uint_images = false)
{
	return String8::FromPrintf("%%image_resinfo_%s_%u_result", GetSampledImageTypeInfo(shape, uint_images).suffix, index);
}

static String8 ImageLoadResultName(uint32_t index, SampledImageShape shape, bool uint_images = false)
{
	return String8::FromPrintf("%%image_load_%s_%u_result", GetSampledImageTypeInfo(shape, uint_images).suffix, index);
}

static String8 EmitImageLoadFetch(uint32_t index, SampledImageShape shape, const String8& descriptor_index, const String8& x,
	                              const String8& y, const String8& z, bool uint_images, const String8& lod, bool shared_uint_type = false)
{
	const auto type_info = GetSampledImageTypeInfo(shape, uint_images, shared_uint_type);
	const auto prefix    = String8::FromPrintf("image_load_%s_%u", type_info.suffix, index);
	const auto coordinate_type = type_info.has_third_dimension ? "v3uint" : "v2uint";
	const auto coordinate_tail = type_info.has_third_dimension ? String8(" ") + z : String8("");
	const auto image_vector = uint_images ? "v4uint" : "v4float";

	static const char* text = R"(
%<prefix>_ptr = OpAccessChain %<pointer_type> %<variable> <descriptor_index>
%<prefix>_image = OpLoad %<image_type> %<prefix>_ptr
%<prefix>_coordinate = OpCompositeConstruct %<coordinate_type> <x> <y><coordinate_tail>
%<prefix>_result = OpImageFetch %<image_vector> %<prefix>_image %<prefix>_coordinate<lod_operand>
)";

	return String8(text)
	    .ReplaceStr("<prefix>", prefix)
	    .ReplaceStr("<pointer_type>", type_info.pointer_type)
	    .ReplaceStr("<variable>", type_info.variable)
	    .ReplaceStr("<image_type>", type_info.image_type)
	    .ReplaceStr("<descriptor_index>", descriptor_index)
	    .ReplaceStr("<coordinate_type>", coordinate_type)
	    .ReplaceStr("<x>", x)
	    .ReplaceStr("<y>", y)
	    .ReplaceStr("<coordinate_tail>", coordinate_tail)
	    .ReplaceStr("<lod_operand>", lod.IsEmpty() ? String8("") : String8(" Lod ") + lod)
	    .ReplaceStr("<image_vector>", image_vector);
}

struct ImageLoadFetch
{
	String8 source;
	String8 result;
	String8 exit_label;
};

static ImageLoadFetch BuildImageLoadFetch(uint32_t index, SampledImageShape shape, const String8& descriptor_index,
                                         const String8& x, const String8& y, const String8& z, bool uint_images,
                                         bool mixed_numeric_types, const String8& lod)
{
	const auto prefix = String8::FromPrintf("image_load_%s_%u", GetSampledImageTypeInfo(shape).suffix, index);
	if (!mixed_numeric_types)
	{
		return {EmitImageLoadFetch(index, shape, descriptor_index, x, y, z, uint_images, lod, uint_images),
		        ImageLoadResultName(index, shape, uint_images), "%" + prefix};
	}

	static const char* select = R"(
OpSelectionMerge %<prefix>_numeric_merge None
OpBranchConditional %image_load_is_uint_<index> %<prefix>_uint_branch %<prefix>_float_branch
%<prefix>_float_branch = OpLabel
<float_fetch>OpBranch %<prefix>_numeric_merge
%<prefix>_uint_branch = OpLabel
<uint_fetch>%<prefix>_uint_bits = OpBitcast %v4float <uint_result>
OpBranch %<prefix>_numeric_merge
%<prefix>_numeric_merge = OpLabel
%<prefix>_numeric_result = OpPhi %v4float <float_result> %<prefix>_float_branch %<prefix>_uint_bits %<prefix>_uint_branch
)";
	const auto source = String8(select).ReplaceStr("<prefix>", prefix)
	                                  .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                                  .ReplaceStr("<float_fetch>", EmitImageLoadFetch(index, shape, descriptor_index, x, y, z, false, lod))
	                                  .ReplaceStr("<uint_fetch>", EmitImageLoadFetch(index, shape, descriptor_index, x, y, z, true, lod))
	                                  .ReplaceStr("<float_result>", ImageLoadResultName(index, shape, false))
	                                  .ReplaceStr("<uint_result>", ImageLoadResultName(index, shape, true));
	// The shape selection's phi must name the numeric merge as predecessor.
	return {source, "%" + prefix + "_numeric_result", "%" + prefix + "_numeric_merge"};
}

static uint32_t ImageGatherComponent(uint8_t dmask)
{
	if (dmask == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dmask == 0 condition ignored (continuing)\n"); }
	for (uint32_t component = 0; component < 4; component++)
	{
		if ((dmask & (1u << component)) != 0)
		{
			return component;
		}
	}
	EXIT("image gather component selection failed\n");
	return 0;
}

static String8 ImageGatherResultName(uint32_t index, SampledImageShape shape)
{
	return String8::FromPrintf("%%image_gather_%s_%u_result", GetSampledImageTypeInfo(shape).suffix, index);
}

static String8 EmitImageGather(uint32_t index, SampledImageShape shape, const String8& descriptor_index,
	                           const String8& sampler_index, const String8& x, const String8& y, const String8& z,
	                           uint32_t component, bool uint_images)
{
	if (shape == SampledImageShape::ThreeDimensional) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: shape == SampledImageShape::ThreeDimensional condition ignored (continuing)\n"); }
	// This emitter's uint_images is the homogeneous-domain predicate, so its
	// uint bank reuses the primary type. Keep the existing result/phi names.
	const auto type_info       = GetSampledImageTypeInfo(shape, uint_images, uint_images);
	const auto prefix          = String8::FromPrintf("image_gather_%s_%u", GetSampledImageTypeInfo(shape).suffix, index);
	const auto coordinate_tail = type_info.has_third_dimension ? String8(" ") + z : String8("");
	const auto image_vector    = uint_images ? "v4uint" : "v4float";
	const auto component_id    = String8::FromPrintf("%%uint_%u", component);

	static const char* text = R"(
%<prefix>_ptr = OpAccessChain %<pointer_type> %<variable> <descriptor_index>
%<prefix>_image = OpLoad %<image_type> %<prefix>_ptr
%<prefix>_sampler_ptr = OpAccessChain %_ptr_UniformConstant_Sampler %samplers <sampler_index>
%<prefix>_sampler = OpLoad %Sampler %<prefix>_sampler_ptr
%<prefix>_sampled = OpSampledImage %<sampled_image_type> %<prefix>_image %<prefix>_sampler
%<prefix>_coordinate = OpCompositeConstruct %<float_coordinate_type> <x> <y><coordinate_tail>
%<prefix>_result = OpImageGather %<image_vector> %<prefix>_sampled %<prefix>_coordinate <component>
)";

	return String8(text)
	    .ReplaceStr("<prefix>", prefix)
	    .ReplaceStr("<pointer_type>", type_info.pointer_type)
	    .ReplaceStr("<variable>", type_info.variable)
	    .ReplaceStr("<image_type>", type_info.image_type)
	    .ReplaceStr("<sampled_image_type>", type_info.sampled_image_type)
	    .ReplaceStr("<float_coordinate_type>", type_info.float_coordinate_type)
	    .ReplaceStr("<descriptor_index>", descriptor_index)
	    .ReplaceStr("<sampler_index>", sampler_index)
	    .ReplaceStr("<x>", x)
	    .ReplaceStr("<y>", y)
	    .ReplaceStr("<coordinate_tail>", coordinate_tail)
	    .ReplaceStr("<image_vector>", image_vector)
	    .ReplaceStr("<component>", component_id);
}

KYTY_RECOMPILER_FUNC(Recompile_ImageGather4_Vdata4Vaddr3StSsMimgDmask)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr)
	{
		return false;
	}

	const bool has_flat  = bind_info->textures2D.textures2d_sampled_num > 0;
	const bool has_array = bind_info->textures2D.textures2d_array_sampled_num > 0;
	const bool has_3d    = bind_info->textures2D.textures3d_sampled_num > 0;
	if ((!has_flat && !has_array) || bind_info->samplers.samplers_num <= 0)
	{
		return false;
	}
	if (has_3d) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: has_3d condition ignored (continuing)\n"); }
	if (inst.mimg_dmask == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.mimg_dmask == 0 condition ignored (continuing)\n"); }
	if (inst.dst.size != 4) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.dst.size != 4 condition ignored (continuing)\n"); }

	const auto x          = mimg_address_to_str(inst, 0);
	const auto y          = mimg_address_to_str(inst, 1);
	const auto z          = mimg_address_to_str(inst, 2);
	const auto descriptor = operand_variable_to_str(inst.src[1], 0);
	const auto sampler    = operand_variable_to_str(inst.src[2], 0);
	if (x.type != SpirvType::Float || y.type != SpirvType::Float || z.type != SpirvType::Float ||
	                     descriptor.type != SpirvType::Uint || sampler.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: x.type != SpirvType::Float || y.type != SpirvType::Float || z.type != SpirvType: condition ignored (continuing)\n"); }

	const auto index_string = String8::FromPrintf("%u", index);
	static const char* setup = R"(
%image_gather_descriptor_raw_<index> = OpLoad %uint %<descriptor>
%image_gather_descriptor_<index> = OpBitwiseAnd %uint %image_gather_descriptor_raw_<index> %uint_0x1fffffff
%image_gather_sampler_<index> = OpLoad %uint %<sampler>
%image_gather_x_<index> = OpLoad %float %<x>
%image_gather_y_<index> = OpLoad %float %<y>
%image_gather_z_<index> = OpLoad %float %<z>
)";
	*dst_source += String8(setup)
	                   .ReplaceStr("<index>", index_string)
	                   .ReplaceStr("<descriptor>", descriptor.value)
	                   .ReplaceStr("<sampler>", sampler.value)
	                   .ReplaceStr("<x>", x.value)
	                   .ReplaceStr("<y>", y.value)
	                   .ReplaceStr("<z>", z.value);

	const auto descriptor_index = String8::FromPrintf("%%image_gather_descriptor_%u", index);
	const auto sampler_index    = String8::FromPrintf("%%image_gather_sampler_%u", index);
	const auto x_value          = String8::FromPrintf("%%image_gather_x_%u", index);
	const auto y_value          = String8::FromPrintf("%%image_gather_y_%u", index);
	const auto z_value          = String8::FromPrintf("%%image_gather_z_%u", index);
	const auto component        = ImageGatherComponent(inst.mimg_dmask);
	const bool uint_images      = UsesUnsignedIntegerImages(bind_info);
	const auto result_type      = uint_images ? "v4uint" : "v4float";
	String8     result;
	if (has_flat && !has_array)
	{
		*dst_source += EmitImageGather(index, SampledImageShape::Flat2d, descriptor_index, sampler_index, x_value, y_value,
		                              z_value, component, uint_images);
		result = ImageGatherResultName(index, SampledImageShape::Flat2d);
	} else if (!has_flat && has_array)
	{
		*dst_source += EmitImageGather(index, SampledImageShape::Array2d, descriptor_index, sampler_index, x_value, y_value,
		                              z_value, component, uint_images);
		result = ImageGatherResultName(index, SampledImageShape::Array2d);
	} else
	{
		static const char* select = R"(
%image_gather_array_tag_<index> = OpBitwiseAnd %uint %image_gather_descriptor_raw_<index> %uint_0x40000000
%image_gather_is_array_<index> = OpINotEqual %bool %image_gather_array_tag_<index> %uint_0
OpSelectionMerge %image_gather_merge_<index> None
OpBranchConditional %image_gather_is_array_<index> %image_gather_array_<index> %image_gather_flat_<index>
%image_gather_flat_<index> = OpLabel
<flat_gather>OpBranch %image_gather_merge_<index>
%image_gather_array_<index> = OpLabel
<array_gather>OpBranch %image_gather_merge_<index>
%image_gather_merge_<index> = OpLabel
%image_gather_result_<index> = OpPhi %<result_type> %image_gather_flat_<index>_result %image_gather_flat_<index> %image_gather_array_<index>_result %image_gather_array_<index>
)";
		*dst_source += String8(select)
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<result_type>", result_type)
		                   .ReplaceStr("<flat_gather>", EmitImageGather(index, SampledImageShape::Flat2d, descriptor_index, sampler_index,
		                                                                  x_value, y_value, z_value, component, uint_images))
		                   .ReplaceStr("<array_gather>", EmitImageGather(index, SampledImageShape::Array2d, descriptor_index, sampler_index,
		                                                                   x_value, y_value, z_value, component, uint_images));
		result = String8::FromPrintf("%%image_gather_result_%u", index);
	}

	const auto image_scalar    = uint_images ? "uint" : "float";
	const auto scalar_to_float = uint_images ? "OpBitcast" : "OpCopyObject";
	for (uint32_t component_index = 0; component_index < 4; component_index++)
	{
		const auto dst = operand_variable_to_str(inst.dst, static_cast<int>(component_index));
		if (dst.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst.type != SpirvType::Float condition ignored (continuing)\n"); }
		*dst_source += String8(R"(
%image_gather_component_<index>_<component> = OpCompositeExtract %<image_scalar> <result> <component>
%image_gather_component_f_<index>_<component> = <scalar_to_float> %float %image_gather_component_<index>_<component>
OpStore %<destination> %image_gather_component_f_<index>_<component>
)")
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<component>", String8::FromPrintf("%u", component_index))
		                   .ReplaceStr("<image_scalar>", image_scalar)
		                   .ReplaceStr("<scalar_to_float>", scalar_to_float)
		                   .ReplaceStr("<result>", result)
		                   .ReplaceStr("<destination>", dst.value);
	}
	return true;
}

static bool ImageResinfoHasZeroLod(const ShaderCode& code, uint32_t index)
{
	const auto& instructions = code.GetInstructions();
	const auto& query = instructions.At(index);
	const auto& lod = query.mimg_address_num > 0 ? query.mimg_address[0] : query.src[0];
	if (lod.type != ShaderOperandType::Vgpr || lod.size != 1 || lod.dpp) { return false; }
	for (const auto& label: code.GetLabels())
	{
		if (!label.IsDisabled() && label.GetDst() <= query.pc) { return false; }
	}
	for (const auto& label: code.GetIndirectLabels())
	{
		if (!label.IsDisabled() && label.GetDst() <= query.pc) { return false; }
	}
	const auto overlaps = [&lod](const ShaderOperand& operand)
	{
		return operand.type == ShaderOperandType::Vgpr && operand.register_id <= lod.register_id &&
		       operand.size > lod.register_id - operand.register_id;
	};
	const auto writes_exec = [](const ShaderOperand& operand)
	{
		return operand.type == ShaderOperandType::ExecLo || operand.type == ShaderOperandType::ExecHi ||
		       operand.type == ShaderOperandType::ExecZ;
	};
	bool zero = false;
	for (uint32_t current = 0; current < index; ++current)
	{
		const auto& inst = instructions.At(current);
		const auto name = ShaderInstructionTypeName(inst.type);
		if (ShaderInstructionHasStaticBranchTarget(inst.type) || inst.type == ShaderInstructionType::SSetpcB64 ||
		    inst.type == ShaderInstructionType::SSwappcB64 || inst.type == ShaderInstructionType::SEndpgm ||
		    inst.type == ShaderInstructionType::Unknown || writes_exec(inst.dst) || writes_exec(inst.dst2) ||
		    ShaderInstructionTypeStartsWith(inst.type, "VCmpx") || name.find("Saveexec") != std::string_view::npos) { return false; }
		if (overlaps(inst.dst))
		{
			zero = inst.type == ShaderInstructionType::VMovB32 && inst.dst.size == 1 && inst.src_num == 1 &&
			       operand_is_constant(inst.src[0]) && inst.src[0].constant.u == 0u && !inst.src[0].dpp &&
			       !inst.src[0].absolute && !inst.src[0].negate && inst.src[0].swizzle == 6u &&
			       !inst.dst.clamp && inst.dst.multiplier == 1.0f && !inst.vop_sdwa;
		}
		if (overlaps(inst.dst2)) { zero = false; }
	}
	return zero;
}

static bool EmitStorageImageResinfo(const ShaderCode& code, uint32_t index, int descriptor_index, int storage_index,
                                    Spirv* spirv, String8* dst_source)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto& bind = *spirv->GetBindInfo();
	const auto& texture = bind.textures2D.desc[descriptor_index].texture;
	// A storage view has one mip. Only a proven level-zero spatial query of
	// a single-mip 2D resource has the same dimensions as that view here.
	if (!Config::IsNextGen() || texture.Type() != 9u || texture.BaseLevel() != 0u || texture.LastLevel() != 0u ||
	    texture.MaxMip() != 0u || inst.mimg_dimension != 1u || inst.mimg_dmask == 0u || (inst.mimg_dmask & ~3u) != 0u ||
	    !ImageResinfoHasZeroLod(code, index) || code.HasAnyOf({ShaderInstructionType::ImageAtomicAdd})) { return false; }
	for (int resource = 0; resource < bind.textures2D.textures_num; ++resource)
	{
		const auto& candidate = bind.textures2D.desc[resource];
		if (candidate.usage == ShaderTextureUsage::ReadWrite && candidate.texture.Type() != 9u) { return false; }
	}
	const int components = static_cast<int>((inst.mimg_dmask & 1u) + ((inst.mimg_dmask >> 1u) & 1u));
	const auto storage_constant = spirv->GetConstantUint(static_cast<uint32_t>(storage_index));
	if (inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size != components || storage_constant.StartsWith("unknown_"))
	{
		return false;
	}
	const auto index_string = String8::FromPrintf("%u", index);
	*dst_source += String8(R"(
%storage_resinfo_ptr_<index> = OpAccessChain %_ptr_UniformConstant_ImageL %textures2D_L %<storage_index>
%storage_resinfo_image_<index> = OpLoad %ImageL %storage_resinfo_ptr_<index>
%storage_resinfo_size_<index> = OpImageQuerySize %v2uint %storage_resinfo_image_<index>
)").ReplaceStr("<index>", index_string).ReplaceStr("<storage_index>", storage_constant);
	int destination = 0;
	for (uint32_t component = 0; component < 2u; ++component)
	{
		if ((inst.mimg_dmask & (1u << component)) == 0u) { continue; }
		const auto dst = operand_variable_to_str(inst.dst, destination++);
		*dst_source += String8(R"(
%storage_resinfo_component_<index>_<component> = OpCompositeExtract %uint %storage_resinfo_size_<index> <component>
%storage_resinfo_float_<index>_<component> = OpBitcast %float %storage_resinfo_component_<index>_<component>
OpStore %<destination> %storage_resinfo_float_<index>_<component>
)").ReplaceStr("<index>", index_string).ReplaceStr("<component>", String8::FromPrintf("%u", component))
		   .ReplaceStr("<destination>", dst.value);
	}
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageGetResinfo_VdataVaddrStDmask)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr)
	{
		return false;
	}
	const auto* vs_info = spirv->GetVsInputInfo();
	const int user_data_register_base = vs_info != nullptr && vs_info->gs_prolog ? 8 : 0;
	const int sampled_descriptor = ShaderFindImageSampledTextureDescriptor(inst, *bind_info, user_data_register_base);
	const int storage_descriptor = sampled_descriptor < 0 && bind_info->textures2D.textures2d_storage_num > 0
	                                   ? ShaderFindImageStorageTextureDescriptor(code, index, *bind_info, user_data_register_base) : -1;
	if (storage_descriptor >= 0)
	{
		const int storage_index = ResolveStorageTextureArrayIndex(code, index, *bind_info, user_data_register_base);
		return storage_index >= 0 && EmitStorageImageResinfo(code, index, storage_descriptor, storage_index, spirv, dst_source);
	}

	const bool has_flat  = bind_info->textures2D.textures2d_sampled_num > 0;
	const bool has_array = bind_info->textures2D.textures2d_array_sampled_num > 0;
	const bool has_3d    = bind_info->textures2D.textures3d_sampled_num > 0;
	const bool uint_images = UsesUnsignedIntegerImages(bind_info);
	const bool mixed_numeric_types = UsesMixedSampledImageNumericTypes(bind_info);
	if (!has_flat && !has_array && !has_3d)
	{
		return false;
	}

	const auto lod        = mimg_address_to_str(inst, 0);
	const auto descriptor = operand_variable_to_str(inst.src[1], 0);
	if (lod.type != SpirvType::Float || descriptor.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: lod.type != SpirvType::Float || descriptor.type != SpirvType::Uint condition ignored (continuing)\n"); }
	if (inst.mimg_dmask == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: inst.mimg_dmask == 0 condition ignored (continuing)\n"); }

	int destination_count = 0;
	for (uint32_t component = 0; component < 4; component++)
	{
		destination_count += static_cast<int>((inst.mimg_dmask >> component) & 1u);
	}
	if (destination_count != inst.dst.size) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: destination_count != inst.dst.size condition ignored (continuing)\n"); }

	const auto index_string = String8::FromPrintf("%u", index);
	static const char* setup = R"(
%image_resinfo_descriptor_raw_<index> = OpLoad %uint %<descriptor>
%image_resinfo_descriptor_<index> = OpBitwiseAnd %uint %image_resinfo_descriptor_raw_<index> %uint_0x1fffffff
%image_resinfo_uint_tag_<index> = OpBitwiseAnd %uint %image_resinfo_descriptor_raw_<index> %uint_0x20000000
%image_resinfo_is_uint_<index> = OpINotEqual %bool %image_resinfo_uint_tag_<index> %uint_0
%image_resinfo_lod_f_<index> = OpLoad %float %<lod>
%image_resinfo_lod_<index> = OpBitcast %int %image_resinfo_lod_f_<index>
)";
	*dst_source += String8(setup).ReplaceStr("<index>", index_string).ReplaceStr("<descriptor>", descriptor.value).ReplaceStr("<lod>", lod.value);

	const auto descriptor_index = String8::FromPrintf("%%image_resinfo_descriptor_%u", index);
	const auto lod_value        = String8::FromPrintf("%%image_resinfo_lod_%u", index);
	String8     result;
	if (has_flat && !has_array && !has_3d)
	{
		if (mixed_numeric_types)
		{
			static const char* select = R"(
OpSelectionMerge %image_resinfo_numeric_merge_<index> None
OpBranchConditional %image_resinfo_is_uint_<index> %image_resinfo_uint_<index> %image_resinfo_float_<index>
%image_resinfo_float_<index> = OpLabel
<float_query>OpBranch %image_resinfo_numeric_merge_<index>
%image_resinfo_uint_<index> = OpLabel
<uint_query>OpBranch %image_resinfo_numeric_merge_<index>
%image_resinfo_numeric_merge_<index> = OpLabel
%image_resinfo_numeric_result_<index> = OpPhi %v4uint <float_result> %image_resinfo_float_<index> <uint_result> %image_resinfo_uint_<index>
)";
			*dst_source += String8(select)
			                   .ReplaceStr("<index>", index_string)
			                   .ReplaceStr("<float_query>", EmitImageResinfoQuery(index, SampledImageShape::Flat2d, descriptor_index, lod_value, false))
			                   .ReplaceStr("<uint_query>", EmitImageResinfoQuery(index, SampledImageShape::Flat2d, descriptor_index, lod_value, true))
			                   .ReplaceStr("<float_result>", ImageResinfoResultName(index, SampledImageShape::Flat2d, false))
			                   .ReplaceStr("<uint_result>", ImageResinfoResultName(index, SampledImageShape::Flat2d, true));
			result = String8::FromPrintf("%%image_resinfo_numeric_result_%u", index);
		} else
		{
			*dst_source += EmitImageResinfoQuery(index, SampledImageShape::Flat2d, descriptor_index, lod_value, uint_images, uint_images);
			result = ImageResinfoResultName(index, SampledImageShape::Flat2d, uint_images);
		}
	} else if (!has_flat && has_array && !has_3d)
	{
		if (mixed_numeric_types) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: mixed_numeric_types condition ignored (continuing)\n"); }
		*dst_source += EmitImageResinfoQuery(index, SampledImageShape::Array2d, descriptor_index, lod_value, uint_images, uint_images);
		result = ImageResinfoResultName(index, SampledImageShape::Array2d, uint_images);
	} else if (!has_flat && !has_array && has_3d)
	{
		if (mixed_numeric_types) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: mixed_numeric_types condition ignored (continuing)\n"); }
		*dst_source += EmitImageResinfoQuery(index, SampledImageShape::ThreeDimensional, descriptor_index, lod_value, uint_images, uint_images);
		result = ImageResinfoResultName(index, SampledImageShape::ThreeDimensional, uint_images);
	} else if (has_flat && has_array && !has_3d)
	{
		static const char* select = R"(
%image_resinfo_array_tag_<index> = OpBitwiseAnd %uint %image_resinfo_descriptor_raw_<index> %uint_0x40000000
%image_resinfo_is_array_<index> = OpINotEqual %bool %image_resinfo_array_tag_<index> %uint_0
OpSelectionMerge %image_resinfo_merge_<index> None
OpBranchConditional %image_resinfo_is_array_<index> %image_resinfo_array_<index> %image_resinfo_flat_<index>
%image_resinfo_flat_<index> = OpLabel
<flat_query>OpBranch %image_resinfo_merge_<index>
%image_resinfo_array_<index> = OpLabel
<array_query>OpBranch %image_resinfo_merge_<index>
%image_resinfo_merge_<index> = OpLabel
%image_resinfo_result_<index> = OpPhi %v4uint <flat_result> %image_resinfo_flat_<index> <array_result> %image_resinfo_array_<index>
)";
		*dst_source += String8(select)
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<flat_result>", ImageResinfoResultName(index, SampledImageShape::Flat2d, uint_images))
		                   .ReplaceStr("<array_result>", ImageResinfoResultName(index, SampledImageShape::Array2d, uint_images))
		                   .ReplaceStr("<flat_query>", EmitImageResinfoQuery(index, SampledImageShape::Flat2d, descriptor_index, lod_value, uint_images, uint_images))
		                   .ReplaceStr("<array_query>", EmitImageResinfoQuery(index, SampledImageShape::Array2d, descriptor_index, lod_value, uint_images, uint_images));
		result = String8::FromPrintf("%%image_resinfo_result_%u", index);
	} else if (has_flat && !has_array && has_3d)
	{
		static const char* select = R"(
%image_resinfo_3d_tag_<index> = OpBitwiseAnd %uint %image_resinfo_descriptor_raw_<index> %uint_0x80000000
%image_resinfo_is_3d_<index> = OpINotEqual %bool %image_resinfo_3d_tag_<index> %uint_0
OpSelectionMerge %image_resinfo_merge_<index> None
OpBranchConditional %image_resinfo_is_3d_<index> %image_resinfo_3d_<index> %image_resinfo_flat_<index>
%image_resinfo_flat_<index> = OpLabel
<flat_query>OpBranch %image_resinfo_merge_<index>
%image_resinfo_3d_<index> = OpLabel
<volume_query>OpBranch %image_resinfo_merge_<index>
%image_resinfo_merge_<index> = OpLabel
%image_resinfo_result_<index> = OpPhi %v4uint <flat_result> %image_resinfo_flat_<index> <volume_result> %image_resinfo_3d_<index>
)";
		*dst_source += String8(select)
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<flat_result>", ImageResinfoResultName(index, SampledImageShape::Flat2d, uint_images))
		                   .ReplaceStr("<volume_result>", ImageResinfoResultName(index, SampledImageShape::ThreeDimensional, uint_images))
		                   .ReplaceStr("<flat_query>", EmitImageResinfoQuery(index, SampledImageShape::Flat2d, descriptor_index, lod_value, uint_images, uint_images))
		                   .ReplaceStr("<volume_query>", EmitImageResinfoQuery(index, SampledImageShape::ThreeDimensional, descriptor_index, lod_value, uint_images, uint_images));
		result = String8::FromPrintf("%%image_resinfo_result_%u", index);
	} else if (!has_flat && has_array && has_3d)
	{
		static const char* select = R"(
%image_resinfo_3d_tag_<index> = OpBitwiseAnd %uint %image_resinfo_descriptor_raw_<index> %uint_0x80000000
%image_resinfo_is_3d_<index> = OpINotEqual %bool %image_resinfo_3d_tag_<index> %uint_0
OpSelectionMerge %image_resinfo_merge_<index> None
OpBranchConditional %image_resinfo_is_3d_<index> %image_resinfo_3d_<index> %image_resinfo_array_<index>
%image_resinfo_array_<index> = OpLabel
<array_query>OpBranch %image_resinfo_merge_<index>
%image_resinfo_3d_<index> = OpLabel
<volume_query>OpBranch %image_resinfo_merge_<index>
%image_resinfo_merge_<index> = OpLabel
%image_resinfo_result_<index> = OpPhi %v4uint <array_result> %image_resinfo_array_<index> <volume_result> %image_resinfo_3d_<index>
)";
		*dst_source += String8(select)
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<array_result>", ImageResinfoResultName(index, SampledImageShape::Array2d, uint_images))
		                   .ReplaceStr("<volume_result>", ImageResinfoResultName(index, SampledImageShape::ThreeDimensional, uint_images))
		                   .ReplaceStr("<array_query>", EmitImageResinfoQuery(index, SampledImageShape::Array2d, descriptor_index, lod_value, uint_images, uint_images))
		                   .ReplaceStr("<volume_query>", EmitImageResinfoQuery(index, SampledImageShape::ThreeDimensional, descriptor_index, lod_value, uint_images, uint_images));
		result = String8::FromPrintf("%%image_resinfo_result_%u", index);
	} else
	{
		static const char* select = R"(
%image_resinfo_3d_tag_<index> = OpBitwiseAnd %uint %image_resinfo_descriptor_raw_<index> %uint_0x80000000
%image_resinfo_is_3d_<index> = OpINotEqual %bool %image_resinfo_3d_tag_<index> %uint_0
OpSelectionMerge %image_resinfo_merge_<index> None
OpBranchConditional %image_resinfo_is_3d_<index> %image_resinfo_3d_<index> %image_resinfo_2d_<index>
%image_resinfo_2d_<index> = OpLabel
%image_resinfo_array_tag_<index> = OpBitwiseAnd %uint %image_resinfo_descriptor_raw_<index> %uint_0x40000000
%image_resinfo_is_array_<index> = OpINotEqual %bool %image_resinfo_array_tag_<index> %uint_0
OpSelectionMerge %image_resinfo_2d_merge_<index> None
OpBranchConditional %image_resinfo_is_array_<index> %image_resinfo_array_<index> %image_resinfo_flat_<index>
%image_resinfo_flat_<index> = OpLabel
<flat_query>OpBranch %image_resinfo_2d_merge_<index>
%image_resinfo_array_<index> = OpLabel
<array_query>OpBranch %image_resinfo_2d_merge_<index>
%image_resinfo_2d_merge_<index> = OpLabel
%image_resinfo_2d_result_<index> = OpPhi %v4uint <flat_result> %image_resinfo_flat_<index> <array_result> %image_resinfo_array_<index>
OpBranch %image_resinfo_merge_<index>
%image_resinfo_3d_<index> = OpLabel
<volume_query>OpBranch %image_resinfo_merge_<index>
%image_resinfo_merge_<index> = OpLabel
%image_resinfo_result_<index> = OpPhi %v4uint %image_resinfo_2d_result_<index> %image_resinfo_2d_merge_<index> <volume_result> %image_resinfo_3d_<index>
)";
		*dst_source += String8(select)
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<flat_result>", ImageResinfoResultName(index, SampledImageShape::Flat2d, uint_images))
		                   .ReplaceStr("<array_result>", ImageResinfoResultName(index, SampledImageShape::Array2d, uint_images))
		                   .ReplaceStr("<volume_result>", ImageResinfoResultName(index, SampledImageShape::ThreeDimensional, uint_images))
		                   .ReplaceStr("<flat_query>", EmitImageResinfoQuery(index, SampledImageShape::Flat2d, descriptor_index, lod_value, uint_images, uint_images))
		                   .ReplaceStr("<array_query>", EmitImageResinfoQuery(index, SampledImageShape::Array2d, descriptor_index, lod_value, uint_images, uint_images))
		                   .ReplaceStr("<volume_query>", EmitImageResinfoQuery(index, SampledImageShape::ThreeDimensional, descriptor_index, lod_value, uint_images, uint_images));
		result = String8::FromPrintf("%%image_resinfo_result_%u", index);
	}

	int destination = 0;
	for (uint32_t component = 0; component < 4; component++)
	{
		if ((inst.mimg_dmask & (1u << component)) == 0)
		{
			continue;
		}
		const auto dst = operand_variable_to_str(inst.dst, destination++);
		if (dst.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst.type != SpirvType::Float condition ignored (continuing)\n"); }
		*dst_source += String8(R"(
%image_resinfo_component_<index>_<component> = OpCompositeExtract %uint <result> <component>
%image_resinfo_component_f_<index>_<component> = OpBitcast %float %image_resinfo_component_<index>_<component>
OpStore %<destination> %image_resinfo_component_f_<index>_<component>
)")
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<component>", String8::FromPrintf("%u", component))
		                   .ReplaceStr("<result>", result)
		                   .ReplaceStr("<destination>", dst.value);
	}
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageLoad_VdataVaddr3StDmask)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	const uint8_t dmask   = inst.format == ShaderInstructionFormat::Vdata4Vaddr3StDmaskF ? 0xfu : inst.mimg_dmask;
	if (bind_info == nullptr)
	{
		return false;
	}

	const bool has_flat  = bind_info->textures2D.textures2d_sampled_num > 0;
	const bool has_array = bind_info->textures2D.textures2d_array_sampled_num > 0;
	const bool has_3d    = bind_info->textures2D.textures3d_sampled_num > 0;
	const bool mixed_numeric_types = UsesMixedSampledImageNumericTypes(bind_info);
	if (!has_flat && !has_array && !has_3d)
	{
		return false;
	}

	const auto x          = mimg_address_to_str(inst, 0);
	const auto y          = mimg_address_to_str(inst, 1);
	const auto z          = mimg_address_to_str(inst, 2);
	const auto descriptor = operand_variable_to_str(inst.src[1], 0);
	if (x.type != SpirvType::Float || y.type != SpirvType::Float || z.type != SpirvType::Float ||
	                     descriptor.type != SpirvType::Uint) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: x.type != SpirvType::Float || y.type != SpirvType::Float || z.type != SpirvType: condition ignored (continuing)\n"); }
	if (dmask == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dmask == 0 condition ignored (continuing)\n"); }

	int destination_count = 0;
	for (uint32_t component = 0; component < 4; component++)
	{
		destination_count += static_cast<int>((dmask >> component) & 1u);
	}
	if (destination_count != inst.dst.size) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: destination_count != inst.dst.size condition ignored (continuing)\n"); }

	const auto index_string = String8::FromPrintf("%u", index);
	static const char* setup = R"(
%image_load_descriptor_raw_<index> = OpLoad %uint %<descriptor>
%image_load_descriptor_<index> = OpBitwiseAnd %uint %image_load_descriptor_raw_<index> %uint_0x1fffffff
%image_load_uint_tag_<index> = OpBitwiseAnd %uint %image_load_descriptor_raw_<index> %uint_0x20000000
%image_load_is_uint_<index> = OpINotEqual %bool %image_load_uint_tag_<index> %uint_0
%image_load_x_f_<index> = OpLoad %float %<x>
%image_load_x_<index> = OpBitcast %uint %image_load_x_f_<index>
%image_load_y_f_<index> = OpLoad %float %<y>
%image_load_y_<index> = OpBitcast %uint %image_load_y_f_<index>
%image_load_z_f_<index> = OpLoad %float %<z>
%image_load_z_<index> = OpBitcast %uint %image_load_z_f_<index>
)";
	*dst_source += String8(setup)
	                   .ReplaceStr("<index>", index_string)
	                   .ReplaceStr("<descriptor>", descriptor.value)
	                   .ReplaceStr("<x>", x.value)
	                   .ReplaceStr("<y>", y.value)
	                   .ReplaceStr("<z>", z.value);
	String8 lod_value;
	if (inst.mimg_explicit_lod)
	{
		const auto lod = mimg_address_to_str(inst, inst.mimg_dimension == 1u ? 2 : 3);
		if (lod.type != SpirvType::Float)
		{
			return false;
		}
		*dst_source += String8(R"(
%image_load_lod_f_<index> = OpLoad %float %<lod>
%image_load_lod_<index> = OpBitcast %int %image_load_lod_f_<index>
)")
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<lod>", lod.value);
		lod_value = String8::FromPrintf("%%image_load_lod_%u", index);
	}

	const bool uint_images = UsesUnsignedIntegerImages(bind_info);
	const auto descriptor_index = String8::FromPrintf("%%image_load_descriptor_%u", index);
	const auto x_value          = String8::FromPrintf("%%image_load_x_%u", index);
	const auto y_value          = String8::FromPrintf("%%image_load_y_%u", index);
	const auto z_value          = String8::FromPrintf("%%image_load_z_%u", index);
	const auto result_type      = uint_images ? "v4uint" : "v4float";
	const auto flat_fetch = BuildImageLoadFetch(index, SampledImageShape::Flat2d, descriptor_index, x_value, y_value, z_value,
	                                            uint_images, mixed_numeric_types, lod_value);
	const auto array_fetch = BuildImageLoadFetch(index, SampledImageShape::Array2d, descriptor_index, x_value, y_value, z_value,
	                                             uint_images, mixed_numeric_types, lod_value);
	const auto volume_fetch = BuildImageLoadFetch(index, SampledImageShape::ThreeDimensional, descriptor_index, x_value, y_value, z_value,
	                                              uint_images, mixed_numeric_types, lod_value);
	String8 result;
	if (has_flat && !has_array && !has_3d)
	{
		*dst_source += flat_fetch.source;
		result = flat_fetch.result;
	} else if (!has_flat && has_array && !has_3d)
	{
		*dst_source += array_fetch.source;
		result = array_fetch.result;
	} else if (!has_flat && !has_array && has_3d)
	{
		*dst_source += volume_fetch.source;
		result = volume_fetch.result;
	} else if (has_flat && has_array && !has_3d)
	{
		static const char* select = R"(
%image_load_array_tag_<index> = OpBitwiseAnd %uint %image_load_descriptor_raw_<index> %uint_0x40000000
%image_load_is_array_<index> = OpINotEqual %bool %image_load_array_tag_<index> %uint_0
OpSelectionMerge %image_load_merge_<index> None
OpBranchConditional %image_load_is_array_<index> %image_load_array_<index> %image_load_flat_<index>
%image_load_flat_<index> = OpLabel
<flat_fetch>OpBranch %image_load_merge_<index>
%image_load_array_<index> = OpLabel
<array_fetch>OpBranch %image_load_merge_<index>
%image_load_merge_<index> = OpLabel
%image_load_result_<index> = OpPhi %<result_type> <flat_result> <flat_exit> <array_result> <array_exit>
)";
		*dst_source += String8(select)
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<result_type>", result_type)
		                   .ReplaceStr("<flat_result>", flat_fetch.result).ReplaceStr("<flat_exit>", flat_fetch.exit_label)
		                   .ReplaceStr("<array_result>", array_fetch.result).ReplaceStr("<array_exit>", array_fetch.exit_label)
		                   .ReplaceStr("<volume_result>", volume_fetch.result).ReplaceStr("<volume_exit>", volume_fetch.exit_label)
		                   .ReplaceStr("<flat_fetch>", flat_fetch.source)
		                   .ReplaceStr("<array_fetch>", array_fetch.source);
		result = String8::FromPrintf("%%image_load_result_%u", index);
	} else if (has_flat && !has_array && has_3d)
	{
		static const char* select = R"(
%image_load_3d_tag_<index> = OpBitwiseAnd %uint %image_load_descriptor_raw_<index> %uint_0x80000000
%image_load_is_3d_<index> = OpINotEqual %bool %image_load_3d_tag_<index> %uint_0
OpSelectionMerge %image_load_merge_<index> None
OpBranchConditional %image_load_is_3d_<index> %image_load_3d_<index> %image_load_flat_<index>
%image_load_flat_<index> = OpLabel
<flat_fetch>OpBranch %image_load_merge_<index>
%image_load_3d_<index> = OpLabel
<volume_fetch>OpBranch %image_load_merge_<index>
%image_load_merge_<index> = OpLabel
%image_load_result_<index> = OpPhi %<result_type> <flat_result> <flat_exit> <volume_result> <volume_exit>
)";
		*dst_source += String8(select)
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<result_type>", result_type)
		                   .ReplaceStr("<flat_result>", flat_fetch.result).ReplaceStr("<flat_exit>", flat_fetch.exit_label)
		                   .ReplaceStr("<array_result>", array_fetch.result).ReplaceStr("<array_exit>", array_fetch.exit_label)
		                   .ReplaceStr("<volume_result>", volume_fetch.result).ReplaceStr("<volume_exit>", volume_fetch.exit_label)
		                   .ReplaceStr("<flat_fetch>", flat_fetch.source)
		                   .ReplaceStr("<volume_fetch>", volume_fetch.source);
		result = String8::FromPrintf("%%image_load_result_%u", index);
	} else if (!has_flat && has_array && has_3d)
	{
		static const char* select = R"(
%image_load_3d_tag_<index> = OpBitwiseAnd %uint %image_load_descriptor_raw_<index> %uint_0x80000000
%image_load_is_3d_<index> = OpINotEqual %bool %image_load_3d_tag_<index> %uint_0
OpSelectionMerge %image_load_merge_<index> None
OpBranchConditional %image_load_is_3d_<index> %image_load_3d_<index> %image_load_array_<index>
%image_load_array_<index> = OpLabel
<array_fetch>OpBranch %image_load_merge_<index>
%image_load_3d_<index> = OpLabel
<volume_fetch>OpBranch %image_load_merge_<index>
%image_load_merge_<index> = OpLabel
%image_load_result_<index> = OpPhi %<result_type> <array_result> <array_exit> <volume_result> <volume_exit>
)";
		*dst_source += String8(select)
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<result_type>", result_type)
		                   .ReplaceStr("<flat_result>", flat_fetch.result).ReplaceStr("<flat_exit>", flat_fetch.exit_label)
		                   .ReplaceStr("<array_result>", array_fetch.result).ReplaceStr("<array_exit>", array_fetch.exit_label)
		                   .ReplaceStr("<volume_result>", volume_fetch.result).ReplaceStr("<volume_exit>", volume_fetch.exit_label)
		                   .ReplaceStr("<array_fetch>", array_fetch.source)
		                   .ReplaceStr("<volume_fetch>", volume_fetch.source);
		result = String8::FromPrintf("%%image_load_result_%u", index);
	} else
	{
		static const char* select = R"(
%image_load_3d_tag_<index> = OpBitwiseAnd %uint %image_load_descriptor_raw_<index> %uint_0x80000000
%image_load_is_3d_<index> = OpINotEqual %bool %image_load_3d_tag_<index> %uint_0
OpSelectionMerge %image_load_merge_<index> None
OpBranchConditional %image_load_is_3d_<index> %image_load_3d_<index> %image_load_2d_<index>
%image_load_2d_<index> = OpLabel
%image_load_array_tag_<index> = OpBitwiseAnd %uint %image_load_descriptor_raw_<index> %uint_0x40000000
%image_load_is_array_<index> = OpINotEqual %bool %image_load_array_tag_<index> %uint_0
OpSelectionMerge %image_load_2d_merge_<index> None
OpBranchConditional %image_load_is_array_<index> %image_load_array_<index> %image_load_flat_<index>
%image_load_flat_<index> = OpLabel
<flat_fetch>OpBranch %image_load_2d_merge_<index>
%image_load_array_<index> = OpLabel
<array_fetch>OpBranch %image_load_2d_merge_<index>
%image_load_2d_merge_<index> = OpLabel
%image_load_2d_result_<index> = OpPhi %<result_type> <flat_result> <flat_exit> <array_result> <array_exit>
OpBranch %image_load_merge_<index>
%image_load_3d_<index> = OpLabel
<volume_fetch>OpBranch %image_load_merge_<index>
%image_load_merge_<index> = OpLabel
%image_load_result_<index> = OpPhi %<result_type> %image_load_2d_result_<index> %image_load_2d_merge_<index> <volume_result> <volume_exit>
)";
		*dst_source += String8(select)
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<result_type>", result_type)
		                   .ReplaceStr("<flat_result>", flat_fetch.result).ReplaceStr("<flat_exit>", flat_fetch.exit_label)
		                   .ReplaceStr("<array_result>", array_fetch.result).ReplaceStr("<array_exit>", array_fetch.exit_label)
		                   .ReplaceStr("<volume_result>", volume_fetch.result).ReplaceStr("<volume_exit>", volume_fetch.exit_label)
		                   .ReplaceStr("<flat_fetch>", flat_fetch.source)
		                   .ReplaceStr("<array_fetch>", array_fetch.source)
		                   .ReplaceStr("<volume_fetch>", volume_fetch.source);
		result = String8::FromPrintf("%%image_load_result_%u", index);
	}

	const auto image_scalar    = uint_images ? "uint" : "float";
	const auto scalar_to_float = uint_images ? "OpBitcast" : "OpCopyObject";
	int        destination     = 0;
	for (uint32_t component = 0; component < 4; component++)
	{
		if ((dmask & (1u << component)) == 0)
		{
			continue;
		}
		const auto dst = operand_variable_to_str(inst.dst, destination++);
		if (dst.type != SpirvType::Float) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst.type != SpirvType::Float condition ignored (continuing)\n"); }
		*dst_source += String8(R"(
%image_load_component_<index>_<component> = OpCompositeExtract %<image_scalar> <result> <component>
%image_load_component_f_<index>_<component> = <scalar_to_float> %float %image_load_component_<index>_<component>
OpStore %<destination> %image_load_component_f_<index>_<component>
)")
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<component>", String8::FromPrintf("%u", component))
		                   .ReplaceStr("<image_scalar>", image_scalar)
		                   .ReplaceStr("<scalar_to_float>", scalar_to_float)
		                   .ReplaceStr("<result>", result)
		                   .ReplaceStr("<destination>", dst.value);
	}
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageStore_VdataVaddr3StDmask)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	const uint8_t dmask   = inst.format == ShaderInstructionFormat::Vdata4Vaddr3StDmaskF ? 0xfu : inst.mimg_dmask;

	if (bind_info != nullptr && bind_info->textures2D.textures2d_storage_num > 0)
	{
		const auto* vs_info                 = spirv->GetVsInputInfo();
		const int   user_data_register_base = (vs_info != nullptr && vs_info->gs_prolog ? 8 : 0);
		const int   storage_index           = ResolveStorageTextureArrayIndex(code, index, *bind_info, user_data_register_base);
		if (storage_index < 0)
		{
			return false;
		}
		const auto storage_index_constant = spirv->GetConstantUint(static_cast<uint32_t>(storage_index));
		if (storage_index_constant.StartsWith("unknown_"))
		{
			return false;
		}

		bool uint_alias = false;
		bool uint_images = false;
		if (!ResolveStorageImageStoreNumericType(code, index, *bind_info, user_data_register_base, &uint_alias, &uint_images))
		{
			return false;
		}
		const bool arrayed           = !uint_alias && UsesArrayed2dImages(bind_info, ShaderTextureUsage::ReadWrite);
		// The storage bank is one shape (ShaderPlanStorageImages): 2D, 2D array or
		// 3D. Sampled 3D textures do not change the storage image's dimensionality.
		const bool three_dimensional = UsesVolumeStorageImages(bind_info);
		// DIM states how many coordinates the instruction supplies; the T# type
		// selects the addressing. A 2D-array store supplies (x, y, slice), which a
		// 3D resource addresses as (x, y, z). A plain 2D store supplies no z.
		if (three_dimensional && inst.mimg_dimension != 2u && inst.mimg_dimension != 5u)
		{
			return false;
		}
		const auto src0_value0       = mimg_address_to_str(inst, 0);
		const auto src0_value1       = mimg_address_to_str(inst, 1);
		const auto zero_component    = uint_images ? spirv->GetConstantUint(0u) : spirv->GetConstantFloat(0.0f);

		if (src0_value0.type != SpirvType::Float || src0_value1.type != SpirvType::Float || zero_component.StartsWith("unknown_"))
		{
			return false;
		}

		String8 component_loads;
		String8 values[4];
		String8 coordinate_bounds;
		int     source_component = 0;
		for (uint32_t component = 0; component < 4; component++)
		{
			if ((dmask & (1u << component)) == 0)
			{
				values[component] = String8::FromPrintf(" %%%s", zero_component.c_str());
				continue;
			}

			const auto source = operand_variable_to_str(inst.dst, source_component++);
			if (source.type != SpirvType::Float)
			{
				return false;
			}
			component_loads += String8::FromPrintf("         %%image_store_value_%u_%u = OpLoad %%float %%%s\n"
			                                      "         %%image_store_component_%u_%u = %s %%%s %%image_store_value_%u_%u\n",
			                                      index, component, source.value.c_str(), index, component,
			                                      uint_images ? "OpBitcast" : "OpCopyObject", uint_images ? "uint" : "float", index,
			                                      component);
			values[component] = String8::FromPrintf(" %%image_store_component_%u_%u", index, component);
		}
		uint32_t order[4];
		StorageStoreComponentOrder(code, index, *bind_info, user_data_register_base, order);
		String8 component_values;
		for (uint32_t channel = 0; channel < 4; channel++)
		{
			component_values += values[order[channel]];
		}
		const uint32_t coordinate_count = (arrayed || three_dimensional) ? 3u : 2u;
		for (uint32_t component = 0; component < coordinate_count; component++)
		{
			coordinate_bounds += String8::FromPrintf("         %%image_store_coordinate_%u_%u = OpCompositeExtract %%uint %%t73_%u %u\n"
			                                          "         %%image_store_extent_%u_%u = OpCompositeExtract %%uint %%image_store_extent_u_%u %u\n"
			                                          "         %%image_store_component_in_bounds_%u_%u = OpULessThan %%bool %%image_store_coordinate_%u_%u %%image_store_extent_%u_%u\n",
			                                          index, component, index, component, index, component, index, component, index,
			                                          component, index, component, index, component);
		}
		for (uint32_t component = 1; component < coordinate_count; component++)
		{
			const auto previous = (component == 1u ? String8::FromPrintf("%%image_store_component_in_bounds_%u_0", index) :
			                                           String8::FromPrintf("%%image_store_in_bounds_%u_%u", index, component - 1u));
			coordinate_bounds += String8::FromPrintf("         %%image_store_in_bounds_%u_%u = OpLogicalAnd %%bool %s "
			                                          "%%image_store_component_in_bounds_%u_%u\n",
			                                          index, component, previous.c_str(), index, component);
		}

		static const char* text                  = R"(
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageL %textures2D_L %<storage_index>
         %t27_<index> = OpLoad %ImageL %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
<array_coordinate_load>         %t73_<index> = OpCompositeConstruct %<coordinate_type> %t69_<index> %t71_<index><array_coordinate_value>
<component_loads>         %t88_<index> = OpCompositeConstruct %<image_vector><component_values>
         %image_store_extent_i_<index> = OpImageQuerySize %<extent_type> %t27_<index>
         %image_store_extent_u_<index> = OpBitcast %<coordinate_type> %image_store_extent_i_<index>
<coordinate_bounds>         %image_store_exec_<index> = OpLoad %uint %exec_lo
         %image_store_active_<index> = OpINotEqual %bool %image_store_exec_<index> %uint_0
         %image_store_enabled_<index> = OpLogicalAnd %bool %image_store_active_<index> %image_store_in_bounds_<index>_<last_coordinate>
               OpSelectionMerge %image_store_merge_<index> None
               OpBranchConditional %image_store_enabled_<index> %image_store_write_<index> %image_store_merge_<index>
         %image_store_write_<index> = OpLabel
               OpImageWrite %t27_<index> %t73_<index> %t88_<index>
               OpBranch %image_store_merge_<index>
         %image_store_merge_<index> = OpLabel
)";
		const auto    src0_value2    = mimg_address_to_str(inst, 2);
		const String8 index_string   = String8::FromPrintf("%u", index);
		const bool    array_window_2d = arrayed && inst.mimg_dimension == 1u;
		const bool    load_third_coordinate = (arrayed || three_dimensional) && !array_window_2d;
		const String8 array_coordinate_load =
		    (load_third_coordinate ? String8("         %t72_<index> = OpLoad %float %<src0_value2>\n         %t721_<index> = OpBitcast %uint %t72_<index>\n")
		                                 .ReplaceStr("<index>", index_string)
		                                 .ReplaceStr("<src0_value2>", src0_value2.value) :
		                             String8(""));
		const String8 array_coordinate_value =
		    ((arrayed || three_dimensional) ? (array_window_2d ? String8(" %uint_0") : String8::FromPrintf(" %%t721_%u", index)) :
		                                       String8(""));
		*dst_source += String8(text)
		                   .ReplaceStr("ImageL", uint_alias ? "ImageLU" : "ImageL")
		                   .ReplaceStr("textures2D_L", uint_alias ? "textures2D_LU" : "textures2D_L")
		                   .ReplaceStr("<array_coordinate_load>", array_coordinate_load)
		                   .ReplaceStr("<coordinate_type>", (arrayed || three_dimensional) ? "v3uint" : "v2uint")
		                   .ReplaceStr("<extent_type>", (arrayed || three_dimensional) ? "v3int" : "v2int")
		                   .ReplaceStr("<array_coordinate_value>", array_coordinate_value)
		                   .ReplaceStr("<image_vector>", uint_images ? "v4uint" : "v4float")
		                   .ReplaceStr("<component_loads>", component_loads)
		                   .ReplaceStr("<component_values>", component_values)
		                   .ReplaceStr("<coordinate_bounds>", coordinate_bounds)
		                   .ReplaceStr("<last_coordinate>", String8::FromPrintf("%u", coordinate_count - 1u))
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<storage_index>", storage_index_constant)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageStoreMip_Vdata4Vaddr4StDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_storage_num > 0)
	{
		const auto* vs_info                 = spirv->GetVsInputInfo();
		const int   user_data_register_base = (vs_info != nullptr && vs_info->gs_prolog ? 8 : 0);
		const int   storage_index           = ResolveStorageTextureArrayIndex(code, index, *bind_info, user_data_register_base);
		if (storage_index < 0)
		{
			return false;
		}
		const auto storage_index_constant = spirv->GetConstantUint(static_cast<uint32_t>(storage_index));
		if (storage_index_constant.StartsWith("unknown_"))
		{
			return false;
		}
		bool uint_alias = false;
		bool uint_images = false;
		if (!ResolveStorageImageStoreNumericType(code, index, *bind_info, user_data_register_base, &uint_alias, &uint_images))
		{
			return false;
		}

		auto dst_value0 = operand_variable_to_str(inst.dst, 0);
		auto dst_value1 = operand_variable_to_str(inst.dst, 1);
		auto dst_value2 = operand_variable_to_str(inst.dst, 2);
		auto dst_value3 = operand_variable_to_str(inst.dst, 3);

		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);

		auto src1_value2 = operand_variable_to_str(inst.src[1], 2);

		if (dst_value0.type != SpirvType::Float)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: dst_value0.type != SpirvType::Float condition ignored (continuing)\n");
		}
		if (src0_value0.type != SpirvType::Float)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: src0_value0.type != SpirvType::Float condition ignored (continuing)\n");
		}

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED
		// TODO() convert SRGB -> LINEAR if SRGB format was replaced with UNORM

		uint32_t order[4];
		StorageStoreComponentOrder(code, index, *bind_info, user_data_register_base, order);
		String8 channel_values;
		for (uint32_t channel = 0; channel < 4; channel++)
		{
			channel_values += String8::FromPrintf(" %%t%u_<index>", 84u + order[channel]);
		}

		static const char* text = R"(
		 %t25_<index> = OpLoad %uint %<src1_value2>
		%t143_<index> = OpShiftRightLogical %uint %t25_<index> %uint_0
        %t145_<index> = OpBitwiseAnd %uint %t143_<index> %uint_0x00003fff
        %t146_<index> = OpIAdd %uint %t145_<index> %uint_1
        %t149_<index> = OpShiftRightLogical %uint %t25_<index> %uint_14
        %t150_<index> = OpBitwiseAnd %uint %t149_<index> %uint_0x00003fff
        %t151_<index> = OpIAdd %uint %t150_<index> %uint_1
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageL %textures2D_L %<storage_index>
         %t27_<index> = OpLoad %ImageL %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
         %t701_<index> = OpLoad %float %<src0_value2>
         %t711_<index> = OpBitcast %uint %t701_<index>
         %t160_<index> = OpFunctionCall %v2uint %mipmap %t711_<index> %t146_<index> %t151_<index>
         %t73_<index> = OpCompositeConstruct %v2uint %t69_<index> %t71_<index>
         %t84_<index> = OpLoad %float %<dst_value0>
         %t85_<index> = OpLoad %float %<dst_value1>
         %t86_<index> = OpLoad %float %<dst_value2>
         %t87_<index> = OpLoad %float %<dst_value3>
         %t172_<index> = OpIAdd %v2uint %t160_<index> %t73_<index>
         %image_store_mip_float_<index> = OpCompositeConstruct %v4float<channel_values>
         %t88_<index> = <texel_conversion> %<image_vector> %image_store_mip_float_<index>
                OpImageWrite %t27_<index> %t172_<index> %t88_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("ImageL", uint_alias ? "ImageLU" : "ImageL")
		                   .ReplaceStr("textures2D_L", uint_alias ? "textures2D_LU" : "textures2D_L")
		                   .ReplaceStr("<channel_values>", channel_values)
		                   .ReplaceStr("<texel_conversion>", uint_images ? "OpBitcast" : "OpCopyObject")
		                   .ReplaceStr("<image_vector>", uint_images ? "v4uint" : "v4float")
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<storage_index>", storage_index_constant)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value2>", src1_value2.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

/* XXX: Andn2, Or, Nor, Cselect */

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
