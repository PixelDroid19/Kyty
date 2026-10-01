#include "Emulator/Graphics/FragmentTransportAdmission.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::FragmentTransport {
namespace {

constexpr uint32_t kQuadLocalDppLimit = 0xffu;
constexpr uint32_t kGuestWaveWidth    = 64u;
constexpr uint32_t kHostSubgroupWidth = 32u;

bool RowDpp(const ShaderInstruction& instruction)
{
	return instruction.src_num > 0 && instruction.src[0].dpp && instruction.src[0].dpp_ctrl > kQuadLocalDppLimit;
}

bool Permute(const ShaderInstruction& instruction)
{
	return instruction.type == ShaderInstructionType::VPermlane16B32 || instruction.type == ShaderInstructionType::VPermlanex16B32;
}

bool SubgroupWidthsSupported(const ShaderComputeWaveVulkanState& state)
{
	return state.min_subgroup_size <= kHostSubgroupWidth && kHostSubgroupWidth <= state.max_subgroup_size &&
	       kHostSubgroupWidth * 2u == kGuestWaveWidth;
}

} // namespace

bool ProgramRequiresWaveTransport(const ShaderCode& code)
{
	if (code.GetType() != ShaderType::Pixel)
	{
		return false;
	}
	for (const auto& instruction: code.GetInstructions())
	{
		if (RowDpp(instruction) || Permute(instruction))
		{
			return true;
		}
	}
	return false;
}

const char* MissingHostCapability(const GraphicContext& context)
{
	const auto& wave = context.compute_wave_vulkan_state;
	if (!context.geometry_shader_supported)
	{
		return "geometryShader (interpolation geometry)";
	}
	if ((context.subgroup_stages & VK_SHADER_STAGE_FRAGMENT_BIT) == 0u ||
	    (context.subgroup_operations & VK_SUBGROUP_FEATURE_QUAD_BIT) == 0u)
	{
		return "fragment subgroup quad operations";
	}
	if (!context.guest_device_address_supported)
	{
		return "buffer device address with 64-bit shader integers";
	}
	if (!context.compute_derivative_group_linear_enabled)
	{
		return "computeDerivativeGroupLinear (enabled)";
	}
	if (!wave.size_control_feature_enabled || !wave.full_subgroups_feature_enabled)
	{
		return "subgroupSizeControl and computeFullSubgroups (enabled)";
	}
	if (!wave.compute_required_size_supported || !SubgroupWidthsSupported(wave))
	{
		return "required compute subgroup size 32";
	}
	return nullptr;
}

} // namespace Kyty::Libs::Graphics::FragmentTransport

#endif
