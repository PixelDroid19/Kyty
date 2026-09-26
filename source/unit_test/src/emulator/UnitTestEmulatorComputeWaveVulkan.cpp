#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/ShaderComputeWaveLayout.h"
#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"

#include <cstddef>

UT_BEGIN(EmulatorComputeWaveVulkan);

using namespace Libs::Graphics;

static ShaderComputeWaveVulkanState SupportedState()
{
	ShaderComputeWaveVulkanState state {};
	state.extension_advertised                = true;
	state.extension_revision                  = 2;
	state.extension_enabled                   = true;
	state.size_control_feature_supported      = true;
	state.full_subgroups_feature_supported    = true;
	state.size_control_feature_enabled        = true;
	state.full_subgroups_feature_enabled      = true;
	state.compute_required_size_supported     = true;
	state.compute_ballot_shuffle_supported    = true;
	state.min_subgroup_size                   = 8;
	state.max_subgroup_size                   = 32;
	state.max_local_size[0]                   = 1024;
	state.max_local_size[1]                   = 1024;
	state.max_local_size[2]                   = 64;
	state.max_group_count[0]                  = 65535;
	state.max_group_count[1]                  = 65535;
	state.max_group_count[2]                  = 65535;
	state.max_invocations                     = 1024;
	state.max_subgroups                       = 128;
	state.max_shared_bytes                    = 49152;
	return state;
}

static ShaderComputeWaveRequest PairedRequest()
{
	ShaderComputeWaveRequest request {};
	request.local[0] = 64;
	request.local[1] = 1;
	request.local[2] = 1;
	request.groups[0] = 1;
	request.groups[1] = 1;
	request.groups[2] = 1;
	request.dispatch_mode = 0x41;
	request.lane_order = ShaderGuestLaneOrder::LinearXFirst;
	return request;
}

TEST(EmulatorComputeWaveVulkan, RequiresEnabledFeaturesInsteadOfExtensionAdvertisement)
{
	auto state = SupportedState();
	state.size_control_feature_enabled   = false;
	state.full_subgroups_feature_enabled = false;

	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(state);
	EXPECT_EQ(capabilities.size_control_enabled, false);
	EXPECT_EQ(capabilities.full_subgroups_enabled, false);
	ShaderComputeWaveLayout layout {};
	EXPECT_EQ(ShaderBuildPairedComputeWaveLayout(PairedRequest(), capabilities, &layout),
	          ShaderComputeWaveLayoutStatus::MissingHostCapability);
}

TEST(EmulatorComputeWaveVulkan, EnablesPairedLayoutOnlyWhenRevisionTwoFeaturesAreEnabled)
{
	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(SupportedState());
	ShaderComputeWaveLayout layout {};
	EXPECT_EQ(ShaderBuildPairedComputeWaveLayout(PairedRequest(), capabilities, &layout),
	          ShaderComputeWaveLayoutStatus::Supported);
}

TEST(EmulatorComputeWaveVulkan, ConservativelyRejectsRevisionOneFeatureQueries)
{
	auto state = SupportedState();
	state.extension_revision = 1;

	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(state);
	EXPECT_EQ(capabilities.size_control_enabled, false);
	EXPECT_EQ(capabilities.full_subgroups_enabled, false);
	ShaderComputeWaveLayout layout {};
	EXPECT_EQ(ShaderBuildPairedComputeWaveLayout(PairedRequest(), capabilities, &layout),
	          ShaderComputeWaveLayoutStatus::MissingHostCapability);
}

TEST(EmulatorComputeWaveVulkan, BoundsProbeOutputWordCountBeforeResourceCreation)
{
	EXPECT_TRUE(ShaderComputeWaveVulkanProbeOutputWordCountValid(4096));
	EXPECT_FALSE(ShaderComputeWaveVulkanProbeOutputWordCountValid(0));
	EXPECT_FALSE(ShaderComputeWaveVulkanProbeOutputWordCountValid(4097));
}

TEST(EmulatorComputeWaveVulkan, ChainsRequiredSizeStateWithoutDroppingExistingStageState)
{
	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(SupportedState());
	auto       request      = PairedRequest();
	ShaderComputeWaveLayout layout {};
	ASSERT_EQ(ShaderBuildPairedComputeWaveLayout(request, capabilities, &layout), ShaderComputeWaveLayoutStatus::Supported);

	VkPipelineTessellationStateCreateInfo prior_state {};
	prior_state.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
	VkPipelineShaderStageCreateInfo stage {};
	stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.pNext  = &prior_state;
	stage.flags  = VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};

	ASSERT_TRUE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required));
	EXPECT_EQ(stage.pNext, &required);
	EXPECT_EQ(required.pNext, &prior_state);
	EXPECT_EQ(required.sType, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT);
	EXPECT_EQ(required.requiredSubgroupSize, 32u);
	EXPECT_NE(stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT, 0u);
	EXPECT_EQ(stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT, 0u);
}

TEST(EmulatorComputeWaveVulkan, DoesNotMutateStageWhenFeatureAdmissionFails)
{
	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(SupportedState());
	auto       request      = PairedRequest();
	ShaderComputeWaveLayout layout {};
	ASSERT_EQ(ShaderBuildPairedComputeWaveLayout(request, capabilities, &layout), ShaderComputeWaveLayoutStatus::Supported);
	VkPipelineShaderStageCreateInfo stage {};
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.flags = 0x100u;
	const auto before = stage;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
	auto unavailable = capabilities;
	unavailable.full_subgroups_enabled = false;

	EXPECT_FALSE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, unavailable, &stage, &required));
	EXPECT_EQ(stage.pNext, before.pNext);
	EXPECT_EQ(stage.flags, before.flags);
}

UT_END();
