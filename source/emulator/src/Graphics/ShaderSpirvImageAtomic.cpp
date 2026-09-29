#include "ShaderSpirvInternal.h"
#include "ShaderSpirvEmitters.h"

#include "Emulator/Config.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_RECOMPILER_FUNC(Recompile_ImageAtomicAdd)
{
	const auto& instruction = code.GetInstructions().At(index);
	const auto* bind = spirv->GetBindInfo();
	if (!Config::IsNextGen() || code.GetType() != ShaderType::Compute || !ShaderImageAtomicAddSupported(instruction) ||
	    bind == nullptr || !ShaderImageAtomicResourceSupported(code, index, *bind)) { return false; }
	const int descriptor = ResolveStorageTextureArrayIndex(code, index, *bind, 0);
	if (descriptor < 0) { return false; }
	const auto binding = spirv->GetConstantUint(static_cast<uint32_t>(descriptor));
	const auto x = operand_variable_to_str(instruction.src[0], 0);
	const auto y = operand_variable_to_str(instruction.src[0], 1);
	const auto value = operand_variable_to_str(instruction.src[2]);
	if (binding.StartsWith("unknown_") || x.type != SpirvType::Float || y.type != SpirvType::Float || value.type != SpirvType::Float)
	{
		return false;
	}
	String8 store;
	if (instruction.mimg_return_old_value)
	{
		store = "%image_atomic_old_f_<index> = OpBitcast %float %image_atomic_old_<index>\n"
		        "OpStore %<value> %image_atomic_old_f_<index>\n";
	}
	// Keep both the atomic side effect and its optional return inside EXEC and
	// coordinate guards. The banked path preserves this branch around atomics.
	static const char* text = R"(
%image_atomic_resource_<index> = OpAccessChain %_ptr_UniformConstant_ImageLU %textures2D_LU %<binding>
%image_atomic_image_<index> = OpLoad %ImageLU %image_atomic_resource_<index>
%image_atomic_x_f_<index> = OpLoad %float %<x>
%image_atomic_y_f_<index> = OpLoad %float %<y>
%image_atomic_x_<index> = OpBitcast %uint %image_atomic_x_f_<index>
%image_atomic_y_<index> = OpBitcast %uint %image_atomic_y_f_<index>
%image_atomic_coord_<index> = OpCompositeConstruct %v2uint %image_atomic_x_<index> %image_atomic_y_<index>
%image_atomic_extent_<index> = OpImageQuerySize %v2uint %image_atomic_image_<index>
%image_atomic_width_<index> = OpCompositeExtract %uint %image_atomic_extent_<index> 0
%image_atomic_height_<index> = OpCompositeExtract %uint %image_atomic_extent_<index> 1
%image_atomic_x_valid_<index> = OpULessThan %bool %image_atomic_x_<index> %image_atomic_width_<index>
%image_atomic_y_valid_<index> = OpULessThan %bool %image_atomic_y_<index> %image_atomic_height_<index>
%image_atomic_in_bounds_<index> = OpLogicalAnd %bool %image_atomic_x_valid_<index> %image_atomic_y_valid_<index>
%image_atomic_exec_<index> = OpLoad %uint %exec_lo
%image_atomic_active_<index> = OpINotEqual %bool %image_atomic_exec_<index> %uint_0
%image_atomic_enabled_<index> = OpLogicalAnd %bool %image_atomic_active_<index> %image_atomic_in_bounds_<index>
OpSelectionMerge %image_atomic_merge_<index> None
OpBranchConditional %image_atomic_enabled_<index> %image_atomic_update_<index> %image_atomic_merge_<index>
%image_atomic_update_<index> = OpLabel
%image_atomic_value_f_<index> = OpLoad %float %<value>
%image_atomic_value_<index> = OpBitcast %uint %image_atomic_value_f_<index>
%image_atomic_texel_<index> = OpImageTexelPointer %_ptr_Image_uint %image_atomic_resource_<index> %image_atomic_coord_<index> %uint_0
%image_atomic_old_<index> = OpAtomicIAdd %uint %image_atomic_texel_<index> %uint_1 %uint_0 %image_atomic_value_<index>
<store>OpBranch %image_atomic_merge_<index>
%image_atomic_merge_<index> = OpLabel
)";
	*dst_source += String8(text).ReplaceStr("<store>", store)
	                          .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                          .ReplaceStr("<binding>", binding)
	                          .ReplaceStr("<x>", x.value)
	                          .ReplaceStr("<y>", y.value)
	                          .ReplaceStr("<value>", value.value);
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
