#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/FragmentTransportAdmission.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Shader.h"

#include <cstring>
#include <vector>

UT_BEGIN(EmulatorFragmentTransportAdmission);

using namespace Libs::Graphics;
using namespace Libs::Graphics::FragmentTransport;

static ShaderInstruction Dpp(uint16_t control)
{
	ShaderInstruction instruction {};
	instruction.type            = ShaderInstructionType::VOrB32;
	instruction.format          = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	instruction.dst.type        = ShaderOperandType::Vgpr;
	instruction.dst.size        = 1;
	instruction.src[0]          = instruction.dst;
	instruction.src[0].dpp      = true;
	instruction.src[0].dpp_ctrl = control;
	instruction.src[1]          = instruction.dst;
	instruction.src_num         = 2;
	return instruction;
}

static ShaderInstruction Typed(ShaderInstructionType type)
{
	ShaderInstruction instruction {};
	instruction.type    = type;
	instruction.src_num = 3;
	return instruction;
}

static bool Required(ShaderType type, const std::vector<ShaderInstruction>& program)
{
	ShaderCode code;
	code.SetType(type);
	for (const auto& instruction: program)
	{
		code.GetInstructions().Add(instruction);
	}
	return ProgramRequiresWaveTransport(code);
}

static void Supported(GraphicContext* context_out)
{
	auto& context                                                     = *context_out;
	context.geometry_shader_supported                                 = true;
	context.subgroup_stages                                           = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	context.subgroup_operations                                       = VK_SUBGROUP_FEATURE_QUAD_BIT;
	context.guest_device_address_supported                            = true;
	context.compute_derivative_group_linear_enabled                   = true;
	context.compute_wave_vulkan_state.size_control_feature_enabled    = true;
	context.compute_wave_vulkan_state.full_subgroups_feature_enabled  = true;
	context.compute_wave_vulkan_state.compute_required_size_supported = true;
	context.compute_wave_vulkan_state.min_subgroup_size               = 8;
	context.compute_wave_vulkan_state.max_subgroup_size               = 32;
}

TEST(EmulatorFragmentTransportAdmission, WaveSensitiveProgramsRequireAdmissionIncludingQuadLocalOnes)
{
	EXPECT_FALSE(Required(ShaderType::Pixel, {}));
	// Entering admission does not select transport: the whole-program proof
	// can retain native quad-local execution after checking mask use and helpers.
	EXPECT_TRUE(Required(ShaderType::Pixel, {Dpp(0xb1)}));
	EXPECT_TRUE(Required(ShaderType::Pixel, {Dpp(0x00)}));
	EXPECT_TRUE(Required(ShaderType::Pixel, {Dpp(0x101)}));
	EXPECT_TRUE(Required(ShaderType::Pixel, {Dpp(0x111)}));
	EXPECT_TRUE(Required(ShaderType::Pixel, {Dpp(0x140)}));
	EXPECT_TRUE(Required(ShaderType::Pixel, {Dpp(0xb1), Typed(ShaderInstructionType::VPermlanex16B32)}));
	EXPECT_TRUE(Required(ShaderType::Pixel, {Typed(ShaderInstructionType::VPermlane16B32)}));
	EXPECT_FALSE(Required(ShaderType::Compute, {Dpp(0x111), Typed(ShaderInstructionType::VPermlanex16B32)}));
	EXPECT_TRUE(Required(ShaderType::Pixel, {Typed(ShaderInstructionType::VReadlaneB32)}));
}

TEST(EmulatorFragmentTransportAdmission, DppWithoutSourcesIsNotARowRead)
{
	auto instruction    = Dpp(0x111);
	instruction.src_num = 0;
	EXPECT_FALSE(Required(ShaderType::Pixel, {instruction}));
}

TEST(EmulatorFragmentTransportAdmission, NamesTheFirstMissingHostCapability)
{
	GraphicContext baseline;
	Supported(&baseline);
	EXPECT_TRUE(MissingHostCapability(baseline) == nullptr);
	for (int field = 0; field < 9; ++field)
	{
		GraphicContext context;
		Supported(&context);
		switch (field)
		{
			case 0: context.geometry_shader_supported = false; break;
			case 1: context.subgroup_stages = VK_SHADER_STAGE_COMPUTE_BIT; break;
			case 2: context.subgroup_operations = 0; break;
			case 3: context.guest_device_address_supported = false; break;
			case 4: context.compute_derivative_group_linear_enabled = false; break;
			case 5: context.compute_wave_vulkan_state.size_control_feature_enabled = false; break;
			case 6: context.compute_wave_vulkan_state.full_subgroups_feature_enabled = false; break;
			case 7: context.compute_wave_vulkan_state.compute_required_size_supported = false; break;
			default: context.compute_wave_vulkan_state.max_subgroup_size = 16; break;
		}
		const char* missing = MissingHostCapability(context);
		EXPECT_TRUE(missing != nullptr && std::strlen(missing) > 0);
	}
	GraphicContext wide;
	Supported(&wide);
	wide.compute_wave_vulkan_state.min_subgroup_size = 64;
	wide.compute_wave_vulkan_state.max_subgroup_size = 64;
	EXPECT_TRUE(MissingHostCapability(wide) != nullptr);
}

TEST(EmulatorFragmentTransportAdmission, AdvertisedButNotEnabledDerivativesAreMissing)
{
	GraphicContext context;
	Supported(&context);
	context.compute_derivative_group_linear_supported = true;
	context.compute_derivative_group_linear_enabled   = false;
	EXPECT_TRUE(MissingHostCapability(context) != nullptr);
}

UT_END();
