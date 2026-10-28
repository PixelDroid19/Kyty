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

static ShaderComputeWaveRequest SplitRequest()
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

// Vulkan 1.4 device that enables the promoted core features without
// advertising the extension name.
static ShaderComputeWaveVulkanState CoreOnlyState()
{
	auto state                 = SupportedState();
	state.extension_advertised = false;
	state.extension_revision   = 0;
	state.extension_enabled    = false;
	return state;
}

static ShaderComputeWaveLayoutStatus SplitStatus(const ShaderComputeWaveVulkanState& state)
{
	ShaderComputeWaveLayout layout {};
	return ShaderBuildSplitComputeWaveLayout(SplitRequest(), ShaderComputeWaveVulkanBuildCapabilities(state), &layout);
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
	EXPECT_EQ(ShaderBuildSplitComputeWaveLayout(SplitRequest(), capabilities, &layout),
	          ShaderComputeWaveLayoutStatus::MissingHostCapability);
}

TEST(EmulatorComputeWaveVulkan, EnablesSplitLayoutWhenSizeControlFeaturesAreEnabled)
{
	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(SupportedState());
	ShaderComputeWaveLayout layout {};
	EXPECT_EQ(ShaderBuildSplitComputeWaveLayout(SplitRequest(), capabilities, &layout),
	          ShaderComputeWaveLayoutStatus::Supported);
}

TEST(EmulatorComputeWaveVulkan, CoreFeaturesAdmitSplitLayoutWithoutExtensionDiagnostics)
{
	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(CoreOnlyState());
	EXPECT_TRUE(capabilities.size_control_enabled);
	EXPECT_TRUE(capabilities.full_subgroups_enabled);
	EXPECT_TRUE(capabilities.compute_required_size_supported);
	EXPECT_EQ(SplitStatus(CoreOnlyState()), ShaderComputeWaveLayoutStatus::Supported);

	// Under the Vulkan 1.4 floor an older advertised revision is only a diagnostic.
	auto revision_one                 = CoreOnlyState();
	revision_one.extension_advertised = true;
	revision_one.extension_revision   = 1;
	EXPECT_EQ(SplitStatus(revision_one), ShaderComputeWaveLayoutStatus::Supported);
}

TEST(EmulatorComputeWaveVulkan, ExtensionDiagnosticsWithoutQueriedFeaturesStayRefused)
{
	auto state                             = SupportedState();
	state.size_control_feature_supported   = false;
	state.full_subgroups_feature_supported = false;
	EXPECT_EQ(SplitStatus(state), ShaderComputeWaveLayoutStatus::MissingHostCapability);

	state.size_control_feature_enabled   = false;
	state.full_subgroups_feature_enabled = false;
	EXPECT_EQ(SplitStatus(state), ShaderComputeWaveLayoutStatus::MissingHostCapability);
}

TEST(EmulatorComputeWaveVulkan, SplitLayoutRequiresEveryCoreCapability)
{
	auto disabled                         = CoreOnlyState();
	disabled.size_control_feature_enabled = false;
	EXPECT_EQ(SplitStatus(disabled), ShaderComputeWaveLayoutStatus::MissingHostCapability);

	auto no_full_subgroups                             = CoreOnlyState();
	no_full_subgroups.full_subgroups_feature_supported = false;
	no_full_subgroups.full_subgroups_feature_enabled   = false;
	EXPECT_EQ(SplitStatus(no_full_subgroups), ShaderComputeWaveLayoutStatus::MissingHostCapability);

	auto no_compute_size                            = CoreOnlyState();
	no_compute_size.compute_required_size_supported = false;
	EXPECT_EQ(SplitStatus(no_compute_size), ShaderComputeWaveLayoutStatus::MissingHostCapability);

	auto no_ballot_shuffle                             = CoreOnlyState();
	no_ballot_shuffle.compute_ballot_shuffle_supported = false;
	EXPECT_EQ(SplitStatus(no_ballot_shuffle), ShaderComputeWaveLayoutStatus::MissingHostCapability);
}

TEST(EmulatorComputeWaveVulkan, SplitLayoutRequiresItsExactPartWidthInQueriedRange)
{
	// The split runs 16-lane parts when the host range holds 16, 8-lane parts
	// when it also holds 8 and the guest allocates more than 64 VGPRs, else
	// 32-lane parts. Columns: min, max, guest VGPRs, part width.
	const uint32_t ranges[][4] = {{8, 32, 64, 16}, {8, 32, 68, 8}, {16, 32, 68, 16}, {16, 16, 16, 16}, {32, 64, 68, 32}};
	for (const auto& range: ranges)
	{
		auto state              = CoreOnlyState();
		state.min_subgroup_size = range[0];
		state.max_subgroup_size = range[1];
		const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(state);
		auto       request      = SplitRequest();
		request.vgprs           = range[2];
		ShaderComputeWaveLayout layout {};
		ASSERT_EQ(ShaderBuildSplitComputeWaveLayout(request, capabilities, &layout), ShaderComputeWaveLayoutStatus::Supported);
		EXPECT_EQ(layout.native_subgroup_size, range[3]);
		VkPipelineShaderStageCreateInfo stage {};
		stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
		ASSERT_TRUE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required));
		EXPECT_EQ(required.requiredSubgroupSize, range[3]);
		EXPECT_NE(stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT, 0u);
	}

	// A 64-only host cannot run the split layout; native guest64 stays selectable.
	auto native64              = CoreOnlyState();
	native64.min_subgroup_size = native64.max_subgroup_size = 64;
	EXPECT_EQ(SplitStatus(native64), ShaderComputeWaveLayoutStatus::MissingHostCapability);
	const auto selection = ShaderSelectNativeSubgroup(native64, VK_SHADER_STAGE_COMPUTE_BIT, 64, 64, false, false, false);
	EXPECT_TRUE(selection.supported);
	EXPECT_EQ(selection.size, 64u);
}

TEST(EmulatorComputeWaveVulkan, SplitLayoutRefusesUnqueriedOrReversedRange)
{
	auto zero              = CoreOnlyState();
	zero.min_subgroup_size = 0;
	EXPECT_FALSE(ShaderComputeWaveVulkanBuildCapabilities(zero).size_control_enabled);
	EXPECT_EQ(SplitStatus(zero), ShaderComputeWaveLayoutStatus::MissingHostCapability);

	auto reversed              = CoreOnlyState();
	reversed.min_subgroup_size = 64;
	reversed.max_subgroup_size = 32;
	EXPECT_EQ(SplitStatus(reversed), ShaderComputeWaveLayoutStatus::MissingHostCapability);
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
	auto       request      = SplitRequest();
	ShaderComputeWaveLayout layout {};
	ASSERT_EQ(ShaderBuildSplitComputeWaveLayout(request, capabilities, &layout), ShaderComputeWaveLayoutStatus::Supported);

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
	EXPECT_EQ(required.requiredSubgroupSize, 16u);
	EXPECT_NE(stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT, 0u);
	EXPECT_EQ(stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT, 0u);
}

TEST(EmulatorComputeWaveVulkan, DoesNotMutateStageWhenFeatureAdmissionFails)
{
	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(SupportedState());
	auto       request      = SplitRequest();
	ShaderComputeWaveLayout layout {};
	ASSERT_EQ(ShaderBuildSplitComputeWaveLayout(request, capabilities, &layout), ShaderComputeWaveLayoutStatus::Supported);
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

TEST(EmulatorComputeWaveVulkan, RequiresFragmentStageSupportForWave32LaneExchange)
{
	auto state                                 = SupportedState();
	state.fragment_required_size_supported     = true;
	constexpr uint32_t kLaneExchangeOps = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
	                                      VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
	                                      VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;

	EXPECT_TRUE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                            32u, state, true));

	// Every gate must hold on its own.
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_COMPUTE_BIT, kLaneExchangeOps, true,
	                                                             32u, state, true));
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps,
	                                                             false, 32u, state, true));
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(
	    VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps & ~VK_SUBGROUP_FEATURE_SHUFFLE_BIT, true, 32u, state, true));
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(
	    VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps & ~VK_SUBGROUP_FEATURE_QUAD_BIT, true, 32u, state, true));

	// Only a pre-1.6, non-varying stage can use the default on this range.
	auto no_required_size = state;
	no_required_size.fragment_required_size_supported = false;
	no_required_size.size_control_feature_enabled     = false;
	EXPECT_TRUE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                            32u, no_required_size, false));
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                             32u, no_required_size, true));
	no_required_size.max_subgroup_size = 64;
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                             64u, no_required_size, false));

	// With a default of 64 only an attachable required size proves the map.
	state.max_subgroup_size = 64;
	EXPECT_TRUE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                            64u, state, true));
}

TEST(EmulatorComputeWaveVulkan, FragmentLaneExchangeAcceptsOnlyGuaranteed32)
{
	auto state = SupportedState();
	state.size_control_feature_enabled = false;
	state.min_subgroup_size = state.max_subgroup_size = 32;
	constexpr uint32_t kLaneExchangeOps = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
	                                      VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
	                                      VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
	EXPECT_TRUE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                            32u, state, true));
	state.min_subgroup_size = 16;
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                             32u, state, true));
	state.min_subgroup_size = 0;
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                             32u, state, false));
	state = SupportedState();
	state.fragment_required_size_supported = true;
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                             64u, state, true)); // Default outside queried range.
	state.max_subgroup_size = 48;
	EXPECT_FALSE(ShaderWave32FragmentNativeLaneExchangeSupported(VK_SHADER_STAGE_FRAGMENT_BIT, kLaneExchangeOps, true,
	                                                             32u, state, true));
}

TEST(EmulatorComputeWaveVulkan, NativeRequiredSizeAcceptsEnabledCoreSupportWithoutExtensionName)
{
	auto state = SupportedState();
	state.extension_advertised = state.extension_enabled = false;
	state.extension_revision = 0;
	state.fragment_required_size_supported = true;
	const auto selection = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, false, false, true);
	ASSERT_TRUE(selection.supported);
	EXPECT_EQ(selection.size, 32u);
	EXPECT_TRUE(selection.require_size);
	state.size_control_feature_enabled = false;
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, false, false, true).supported);
}

TEST(EmulatorComputeWaveVulkan, FragmentRequiredSizeFollowsHostMinMax)
{
	auto state                             = SupportedState();
	state.fragment_required_size_supported = true;

	EXPECT_TRUE(ShaderFragmentRequiredSubgroupSizeSupported(state, 32u));
	EXPECT_TRUE(ShaderFragmentRequiredSubgroupSizeSupported(state, 16u));
	EXPECT_FALSE(ShaderFragmentRequiredSubgroupSizeSupported(state, 0u));
	EXPECT_FALSE(ShaderFragmentRequiredSubgroupSizeSupported(state, 64u));
	EXPECT_FALSE(ShaderFragmentRequiredSubgroupSizeSupported(state, 4u));
	state.min_subgroup_size = 3;
	EXPECT_FALSE(ShaderFragmentRequiredSubgroupSizeSupported(state, 32u));
	state.min_subgroup_size = 8;
	state.max_subgroup_size = 48;
	EXPECT_FALSE(ShaderFragmentRequiredSubgroupSizeSupported(state, 32u));
}

UT_END();
