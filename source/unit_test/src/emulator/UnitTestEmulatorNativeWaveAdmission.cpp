#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/FragmentTransportAdmission.h"
#include "Emulator/Graphics/GraphicsGeState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"

#include "spirv/unified1/spirv.hpp"

#include <cstring>
#include <initializer_list>
#include <vector>

UT_BEGIN(EmulatorNativeWaveAdmission);

using namespace Libs::Graphics;

namespace {

ShaderComputeWaveVulkanState Host()
{
	ShaderComputeWaveVulkanState state {};
	state.extension_advertised = state.extension_enabled = true;
	state.extension_revision = 2;
	state.size_control_feature_supported = state.size_control_feature_enabled = true;
	state.full_subgroups_feature_supported = state.full_subgroups_feature_enabled = true;
	state.vertex_required_size_supported = state.fragment_required_size_supported = state.compute_required_size_supported = true;
	state.compute_ballot_shuffle_supported = true;
	state.min_subgroup_size = 8;
	state.max_subgroup_size = 64;
	state.max_invocations = state.max_local_size[0] = state.max_local_size[1] = state.max_local_size[2] = 1024;
	state.max_subgroups = 32;
	state.max_shared_bytes = 65536;
	return state;
}

constexpr uint32_t kAllOps = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                             VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                             VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT | VK_SUBGROUP_FEATURE_CLUSTERED_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
constexpr uint32_t kStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

// Structural binary fixtures exercise the admission walk, not a replacement
// SPIR-V validator. Instructions deliberately occur after non-capabilities too.
std::vector<uint32_t> Module(std::initializer_list<uint32_t> capabilities = {})
{
	std::vector<uint32_t> words {spv::MagicNumber, 0x00010500u, 0, 64, 0};
	for (auto capability: capabilities) { words.insert(words.end(), {(2u << 16u) | spv::OpCapability, capability}); }
	words.insert(words.end(), {(4u << 16u) | spv::OpTypeInt, 1, 32, 0,
	                          (4u << 16u) | spv::OpConstant, 1, 2, 3});
	return words;
}

ShaderSubgroupModuleCheck Check(const std::vector<uint32_t>& words, VkShaderStageFlagBits stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                uint32_t operations = kAllOps, ShaderComputeWaveVulkanState state = Host(),
                                uint32_t stages = kStages, bool reconverges = true)
{
	return ShaderCheckSubgroupModule(words.data(), words.size(), stage, stages, operations, state, reconverges);
}

ShaderOperand Operand(ShaderOperandType type, int reg = 0, int size = 1)
{
	ShaderOperand operand {};
	operand.type = type;
	operand.register_id = reg;
	operand.size = size;
	return operand;
}

ShaderInstruction Unary(ShaderInstructionType type, ShaderOperand dst, ShaderOperand src, bool pair = false)
{
	ShaderInstruction inst {};
	inst.type = type;
	inst.format = pair ? ShaderInstructionFormat::Sdst2Ssrc02 : ShaderInstructionFormat::SVdstSVsrc0;
	inst.dst = dst;
	inst.src[0] = src;
	inst.src_num = 1;
	return inst;
}

ShaderCode Program(std::initializer_list<ShaderInstruction> instructions, ShaderType type = ShaderType::Pixel)
{
	ShaderCode code;
	code.SetType(type);
	for (auto inst: instructions)
	{
		inst.pc = code.GetInstructions().Size() * 4u;
		code.GetInstructions().Add(inst);
	}
	return code;
}

ShaderInstruction Compare()
{
	ShaderInstruction inst {};
	inst.type = ShaderInstructionType::VCmpEqU32;
	inst.format = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
	inst.dst = Operand(ShaderOperandType::Sgpr, 20, 2);
	inst.src[0] = Operand(ShaderOperandType::Vgpr, 0);
	inst.src[1] = Operand(ShaderOperandType::Vgpr, 1);
	inst.src_num = 2;
	return inst;
}

ShaderInstruction Select(ShaderOperand mask)
{
	ShaderInstruction inst {};
	inst.type = ShaderInstructionType::VCndmaskB32;
	inst.format = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
	inst.dst = Operand(ShaderOperandType::Vgpr, 10);
	inst.src[0] = Operand(ShaderOperandType::LiteralConstant, 0, 0);
	inst.src[1] = Operand(ShaderOperandType::Vgpr, 9);
	inst.src[2] = mask;
	inst.src_num = 3;
	return inst;
}

ShaderCode NeutralRegion()
{
	const auto exec = Operand(ShaderOperandType::ExecLo, 0, 2);
	const auto mask = Operand(ShaderOperandType::Sgpr, 36, 2);
	const auto saved = Operand(ShaderOperandType::VccLo, 0, 2);
	ShaderInstruction row {};
	row.type = ShaderInstructionType::VOrB32;
	row.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	row.dst = Operand(ShaderOperandType::Vgpr, 10);
	row.src[0] = row.dst;
	row.src[0].dpp = true;
	row.src[0].dpp_ctrl = 0x111u;
	row.src[0].dpp_row_mask = row.src[0].dpp_bank_mask = 15;
	row.src[1] = row.dst;
	row.src_num = 2;
	return Program({Unary(ShaderInstructionType::SMovB64, mask, exec, true),
	                Unary(ShaderInstructionType::SOrn2SaveexecB64, saved, mask, true), Select(mask), row,
	                Unary(ShaderInstructionType::SMovB64, exec, saved, true)});
}

} // namespace

TEST(EmulatorNativeWaveAdmission, EveryDeclaredCoreSubgroupFeatureIsCheckedInEveryStage)
{
	const uint32_t capabilities[] = {spv::CapabilityGroupNonUniform, spv::CapabilityGroupNonUniformVote,
	                                spv::CapabilityGroupNonUniformArithmetic, spv::CapabilityGroupNonUniformBallot,
	                                spv::CapabilityGroupNonUniformShuffle, spv::CapabilityGroupNonUniformShuffleRelative,
	                                spv::CapabilityGroupNonUniformClustered, spv::CapabilityGroupNonUniformQuad};
	const uint32_t features[] = {VK_SUBGROUP_FEATURE_BASIC_BIT, VK_SUBGROUP_FEATURE_VOTE_BIT, VK_SUBGROUP_FEATURE_ARITHMETIC_BIT,
	                            VK_SUBGROUP_FEATURE_BALLOT_BIT, VK_SUBGROUP_FEATURE_SHUFFLE_BIT,
	                            VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT, VK_SUBGROUP_FEATURE_CLUSTERED_BIT, VK_SUBGROUP_FEATURE_QUAD_BIT};
	auto state = Host();
	state.quad_operations_in_all_stages = true;
	for (auto stage: {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT})
	{
		for (size_t i = 0; i < 8; ++i)
		{
			const auto words = Module({capabilities[i]});
			EXPECT_TRUE(Check(words, stage, kAllOps, state).supported);
			EXPECT_FALSE(Check(words, stage, kAllOps & ~features[i], state).supported);
			EXPECT_FALSE(Check(words, stage, kAllOps, state, kStages & ~stage).supported);
		}
	}
}

TEST(EmulatorNativeWaveAdmission, EmittedOperationsCannotHideBehindIncompleteCapabilityDeclarations)
{
	const uint32_t ops[] = {spv::OpGroupNonUniformBallot, spv::OpGroupNonUniformBitwiseOr, spv::OpGroupNonUniformShuffle,
	                        spv::OpGroupNonUniformQuadBroadcast};
	const uint32_t features[] = {VK_SUBGROUP_FEATURE_BALLOT_BIT, VK_SUBGROUP_FEATURE_ARITHMETIC_BIT,
	                            VK_SUBGROUP_FEATURE_SHUFFLE_BIT, VK_SUBGROUP_FEATURE_QUAD_BIT};
	for (size_t i = 0; i < 4; ++i)
	{
		auto words = Module();
		words.insert(words.end(), {(6u << 16u) | ops[i], 1, 8, 2, 0, 2});
		EXPECT_TRUE(Check(words).supported);
		EXPECT_FALSE(Check(words, VK_SHADER_STAGE_FRAGMENT_BIT, kAllOps & ~features[i]).supported);
		EXPECT_FALSE(Check(words, VK_SHADER_STAGE_COMPUTE_BIT, kAllOps & ~features[i]).supported);
	}
}

TEST(EmulatorNativeWaveAdmission, DynamicBroadcastAndQuadStageFeaturesMustActuallyBeEnabled)
{
	auto words = Module({spv::CapabilityGroupNonUniformBallot});
	words.insert(words.end(), {(6u << 16u) | spv::OpGroupNonUniformBroadcast, 1, 8, 2, 9, 10});
	EXPECT_FALSE(Check(words).supported);
	auto state = Host();
	state.subgroup_broadcast_dynamic_id_enabled = true;
	EXPECT_TRUE(Check(words, VK_SHADER_STAGE_FRAGMENT_BIT, kAllOps, state).supported);
	words[1] = 0x00010300u;
	EXPECT_FALSE(Check(words, VK_SHADER_STAGE_FRAGMENT_BIT, kAllOps, state).supported);
	words.back() = 2; // A constant id is legal without dynamic-id support.
	EXPECT_TRUE(Check(words).supported);
	const auto quad = Module({spv::CapabilityGroupNonUniformQuad});
	EXPECT_FALSE(Check(quad, VK_SHADER_STAGE_VERTEX_BIT).supported);
	state.quad_operations_in_all_stages = true;
	EXPECT_TRUE(Check(quad, VK_SHADER_STAGE_VERTEX_BIT, kAllOps, state).supported);
}

TEST(EmulatorNativeWaveAdmission, ReconvergenceAndMalformedBinaryFailuresHaveWordProvenance)
{
	auto words = Module();
	const auto mode_word = words.size();
	words.insert(words.end(), {(3u << 16u) | spv::OpExecutionMode, 3, 6023});
	EXPECT_TRUE(Check(words).supported);
	const auto unavailable = Check(words, VK_SHADER_STAGE_FRAGMENT_BIT, kAllOps, Host(), kStages, false);
	EXPECT_FALSE(unavailable.supported);
	EXPECT_EQ(unavailable.word, mode_word);
	words.push_back(0);
	EXPECT_FALSE(Check(words).supported);
	words.back() = (0xffffu << 16u) | spv::OpCapability;
	EXPECT_FALSE(Check(words).supported);
	EXPECT_FALSE(ShaderCheckSubgroupModule(nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, kStages, kAllOps, Host(), true).supported);
}

TEST(EmulatorNativeWaveAdmission, Wave64On32RequiresAnExplicitStageSpecificProof)
{
	auto state = Host();
	state.max_subgroup_size = 32;
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 64, false, false, true).supported);
	const auto neutral = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 64, false, true, true);
	ASSERT_TRUE(neutral.supported);
	EXPECT_EQ(neutral.size, 32u);
	EXPECT_TRUE(neutral.require_size);
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_VERTEX_BIT, 32, 64, false, true, true).supported);
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 16, 64, false, false, true).supported);
	EXPECT_TRUE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 16, 64, true, false, true).supported);
	EXPECT_FALSE(ShaderSelectNativeSubgroup({}, VK_SHADER_STAGE_VERTEX_BIT, 128, 64, true, false, true).supported);
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_VERTEX_BIT, 32, 0, true, false, true).supported);
	state.fragment_required_size_supported = false;
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 64, false, true, true).supported);
}

TEST(EmulatorNativeWaveAdmission, ActualModuleVersionControlsExactDefaultWidth)
{
	for (auto stage: {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT})
	{
		auto state = Host();
		state.max_subgroup_size = 32;
		state.vertex_required_size_supported = state.fragment_required_size_supported = false;
		auto words = Module({spv::CapabilityGroupNonUniform});
		ASSERT_TRUE(Check(words, stage).supported);
		const auto legacy = ShaderSelectNativeSubgroup(state, stage, 32, 32, false, false, words[1] >= 0x00010600u);
		ASSERT_TRUE(legacy.supported);
		EXPECT_EQ(legacy.size, 32u);
		EXPECT_FALSE(legacy.require_size);

		words[1] = 0x00010600u;
		ASSERT_TRUE(Check(words, stage).supported);
		EXPECT_FALSE(ShaderSelectNativeSubgroup(state, stage, 32, 32, false, false, words[1] >= 0x00010600u).supported);
		state.vertex_required_size_supported = stage == VK_SHADER_STAGE_VERTEX_BIT;
		state.fragment_required_size_supported = stage == VK_SHADER_STAGE_FRAGMENT_BIT;
		const auto exact = ShaderSelectNativeSubgroup(state, stage, 32, 32, false, false, words[1] >= 0x00010600u);
		ASSERT_TRUE(exact.supported);
		EXPECT_EQ(exact.size, 32u);
		EXPECT_TRUE(exact.require_size); // Equal to the default still requires a node.
		state.size_control_feature_enabled = false;
		EXPECT_FALSE(ShaderSelectNativeSubgroup(state, stage, 32, 32, false, false, true).supported);
	}
}

TEST(EmulatorNativeWaveAdmission, LocalRangeProofCoversEveryWidthWithoutSizeControl)
{
	auto state = Host();
	state.min_subgroup_size = 4;
	state.max_subgroup_size = 32;
	state.size_control_feature_enabled = false;
	for (auto stage: {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT})
	{
		for (uint32_t guest: {32u, 64u})
		{
			const auto local = ShaderSelectNativeSubgroup(state, stage, 32, guest, true, false, true);
			ASSERT_TRUE(local.supported);
			EXPECT_EQ(local.size, 0u); // Every queried width, not an assertion of host32.
			EXPECT_FALSE(local.require_size);
		}
	}
	state.max_subgroup_size = 64;
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, true, false, true).supported);
	EXPECT_TRUE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 64, true, false, true).supported);
	state.size_control_feature_enabled = true;
	const auto constrained = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, true, false, true);
	ASSERT_TRUE(constrained.supported);
	EXPECT_EQ(constrained.size, 32u);
	EXPECT_TRUE(constrained.require_size);
}

TEST(EmulatorNativeWaveAdmission, NeutralProofPreservesNative64AndComposesLegalWidths)
{
	auto state = Host();
	state.min_subgroup_size = state.max_subgroup_size = 64;
	state.size_control_feature_enabled = false;
	for (bool varying: {false, true})
	{
		const auto native = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 64, 64, false, true, varying);
		ASSERT_TRUE(native.supported);
		EXPECT_EQ(native.size, 64u);
		EXPECT_FALSE(native.require_size);
	}
	state.min_subgroup_size = 32;
	for (uint32_t default_size: {32u, 64u})
	{
		// The only power-of-two widths are neutral32 and exact64.
		const auto composed = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, default_size, 64, false, true, true);
		ASSERT_TRUE(composed.supported);
		EXPECT_EQ(composed.size, 0u);
		EXPECT_FALSE(composed.require_size);
	}
	const auto legacy64 = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 64, 64, false, true, false);
	ASSERT_TRUE(legacy64.supported);
	EXPECT_EQ(legacy64.size, 64u);
	EXPECT_FALSE(legacy64.require_size);
	state.min_subgroup_size = 8;
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 64, false, true, true).supported);
	state.size_control_feature_enabled = true;
	const auto preferred = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 64, false, true, true);
	ASSERT_TRUE(preferred.supported);
	EXPECT_EQ(preferred.size, 64u); // Preserve exact guest width preference.
	EXPECT_TRUE(preferred.require_size);
}

TEST(EmulatorNativeWaveAdmission, QueriedSingletonNeedsNoEnabledSizeRequest)
{
	auto state = Host();
	state.min_subgroup_size = state.max_subgroup_size = 32;
	state.size_control_feature_enabled = false;
	state.fragment_required_size_supported = false;
	for (uint32_t guest: {32u, 64u})
	{
		const auto fixed = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, guest, false, guest == 64u, true);
		ASSERT_TRUE(fixed.supported);
		EXPECT_EQ(fixed.size, 32u);
		EXPECT_FALSE(fixed.require_size);
	}
}

TEST(EmulatorNativeWaveAdmission, NativeSelectionRejectsMalformedRangesAndDefaults)
{
	struct Limits
	{
		uint32_t minimum;
		uint32_t maximum;
		uint32_t default_size;
	};
	const Limits invalid[] = {{0, 0, 32}, {0, 32, 32}, {8, 0, 32}, {3, 32, 32}, {8, 48, 32},
	                          {64, 32, 32}, {8, 32, 0}, {8, 32, 24}, {8, 32, 4}, {8, 32, 64}};
	for (const auto& limits: invalid)
	{
		auto state = Host();
		state.min_subgroup_size = limits.minimum;
		state.max_subgroup_size = limits.maximum;
		for (bool varying: {false, true})
		{
			EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, limits.default_size, 32,
			                                      false, false, varying).supported);
			EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, limits.default_size, 64,
			                                      true, false, varying).supported);
		}
	}
}

TEST(EmulatorNativeWaveAdmission, NativeSelectionRejectsConflictingProofsAndUnsupportedStages)
{
	const auto state = Host();
	for (bool varying: {false, true})
	{
		EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 64, true, true, varying).supported);
		for (auto stage: {static_cast<VkShaderStageFlagBits>(0), VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
		                  static_cast<VkShaderStageFlagBits>(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)})
		{
			EXPECT_FALSE(ShaderSelectNativeSubgroup(state, stage, 32, 32, false, false, varying).supported);
		}
	}
}

TEST(EmulatorNativeWaveAdmission, RequiredSizeUsesQueriedStageAndEnabledFeatures)
{
	auto state = Host();
	EXPECT_TRUE(ShaderRequiredSubgroupSizeSupported(state, VK_SHADER_STAGE_VERTEX_BIT, 32));
	EXPECT_FALSE(ShaderRequiredSubgroupSizeSupported(state, VK_SHADER_STAGE_VERTEX_BIT, 48));
	EXPECT_FALSE(ShaderRequiredSubgroupSizeSupported(state, VK_SHADER_STAGE_GEOMETRY_BIT, 32));
	state.vertex_required_size_supported = false;
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_VERTEX_BIT, 64, 32, false, false, false).supported);
	EXPECT_TRUE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_VERTEX_BIT, 32, 32, false, false, false).supported);
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_VERTEX_BIT, 32, 32, false, false, true).supported);
	state.vertex_required_size_supported = true;
	state.size_control_feature_supported = false;
	EXPECT_FALSE(ShaderRequiredSubgroupSizeSupported(state, VK_SHADER_STAGE_VERTEX_BIT, 32));
	state.size_control_feature_supported = true;
	state.size_control_feature_enabled = false;
	EXPECT_FALSE(ShaderRequiredSubgroupSizeSupported(state, VK_SHADER_STAGE_VERTEX_BIT, 32));
}

TEST(EmulatorNativeWaveAdmission, NativeStageChainPreservesPriorNodesAndRejectsVaryingOrDuplicateSize)
{
	const auto state = Host();
	const auto selection = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_VERTEX_BIT, 32, 32, false, false, true);
	ASSERT_TRUE(selection.supported);
	ASSERT_TRUE(selection.require_size);
	VkBaseInStructure prior {};
	prior.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	VkPipelineShaderStageCreateInfo stage {};
	stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	stage.pNext = &prior;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
	ASSERT_TRUE(ShaderAttachNativeSubgroup(state, selection, &stage, &required));
	EXPECT_EQ(stage.pNext, &required);
	EXPECT_EQ(required.sType, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT);
	EXPECT_EQ(required.pNext, &prior);
	EXPECT_EQ(required.requiredSubgroupSize, 32u);
	EXPECT_EQ(stage.flags, 0u);
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT duplicate {};
	EXPECT_FALSE(ShaderAttachNativeSubgroup(state, selection, &stage, &duplicate));
	EXPECT_EQ(stage.pNext, &required);
	EXPECT_EQ(stage.flags, 0u);
	EXPECT_EQ(duplicate.pNext, nullptr);
	EXPECT_EQ(duplicate.requiredSubgroupSize, 0u);
	EXPECT_EQ(duplicate.sType, static_cast<VkStructureType>(0));
	stage.pNext = &prior;
	stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT;
	EXPECT_FALSE(ShaderAttachNativeSubgroup(state, selection, &stage, &duplicate));
	EXPECT_EQ(stage.pNext, &prior);
	EXPECT_EQ(stage.flags,
	          static_cast<VkPipelineShaderStageCreateFlags>(VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT));
	EXPECT_EQ(duplicate.pNext, nullptr);
	EXPECT_EQ(duplicate.requiredSubgroupSize, 0u);
	EXPECT_EQ(duplicate.sType, static_cast<VkStructureType>(0));
}

TEST(EmulatorNativeWaveAdmission, CoveredRangeAttachmentLeavesBothStructuresUntouched)
{
	const auto state = Host();
	const auto selection = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_VERTEX_BIT, 32, 64, true, false, true);
	ASSERT_TRUE(selection.supported);
	ASSERT_EQ(selection.size, 0u);
	ASSERT_FALSE(selection.require_size);
	VkBaseInStructure prior {};
	prior.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	VkPipelineShaderStageCreateInfo stage {};
	stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	stage.pNext = &prior;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
	required.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
	required.pNext = &prior;
	required.requiredSubgroupSize = 16;
	ASSERT_TRUE(ShaderAttachNativeSubgroup(state, selection, &stage, &required));
	EXPECT_EQ(stage.pNext, &prior);
	EXPECT_EQ(stage.flags, 0u);
	EXPECT_EQ(required.sType, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT);
	EXPECT_EQ(required.pNext, &prior);
	EXPECT_EQ(required.requiredSubgroupSize, 16u);
	const ShaderNativeSubgroupSelection zero_required {true, 0, true};
	EXPECT_FALSE(ShaderAttachNativeSubgroup(state, zero_required, &stage, &required));
	stage.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
	EXPECT_FALSE(ShaderAttachNativeSubgroup(state, selection, &stage, &required));
	EXPECT_EQ(stage.pNext, &prior);
	EXPECT_EQ(stage.flags, 0u);
	EXPECT_EQ(required.sType, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT);
	EXPECT_EQ(required.pNext, &prior);
	EXPECT_EQ(required.requiredSubgroupSize, 16u);
}

TEST(EmulatorNativeWaveAdmission, NativeAttachmentRechecksCapabilitiesWithoutMutatingOutputs)
{
	auto state = Host();
	const auto selection = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, false, false, true);
	ASSERT_TRUE(selection.supported);
	ASSERT_TRUE(selection.require_size);
	VkBaseInStructure prior {};
	prior.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	VkPipelineShaderStageCreateInfo stage {};
	stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stage.pNext = &prior;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
	required.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
	required.pNext = &prior;
	required.requiredSubgroupSize = 16;
	const auto expect_refused = [&]
	{
		EXPECT_FALSE(ShaderAttachNativeSubgroup(state, selection, &stage, &required));
		EXPECT_EQ(stage.pNext, &prior);
		EXPECT_EQ(stage.flags, 0u);
		EXPECT_EQ(required.sType, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT);
		EXPECT_EQ(required.pNext, &prior);
		EXPECT_EQ(required.requiredSubgroupSize, 16u);
	};
	state.size_control_feature_enabled = false;
	expect_refused();
	state = Host();
	state.size_control_feature_supported = false;
	expect_refused();
	state = Host();
	state.fragment_required_size_supported = false;
	expect_refused();
	state = Host();
	state.max_subgroup_size = 16;
	expect_refused();
	state = Host();
	state.min_subgroup_size = 3;
	expect_refused();
}

TEST(EmulatorNativeWaveAdmission, VertexWidthComesFromKnownLiveStageControl)
{
	GraphicsGeRawRegister stages;
	EXPECT_EQ(ShaderVertexGuestWaveSize(stages, true, true), 0u);
	stages.SetRaw(0x02002000u); // Known NGG passthrough, GS_W32_EN clear.
	EXPECT_EQ(ShaderVertexGuestWaveSize(stages, true, true), 64u);
	stages.SetRaw(0x02402000u);
	EXPECT_EQ(ShaderVertexGuestWaveSize(stages, true, true), 32u);
	EXPECT_EQ(ShaderVertexGuestWaveSize(stages, true, false), 0u);
	stages.SetRaw(1u << 23u); // Legacy VS stage, VS_W32_EN set.
	EXPECT_EQ(ShaderVertexGuestWaveSize(stages, true, false), 32u);
	stages.SetRaw(1u << 22u); // GS width bit must not select the VS width.
	EXPECT_EQ(ShaderVertexGuestWaveSize(stages, true, false), 64u);
	stages.SetDecoded();
	EXPECT_EQ(ShaderVertexGuestWaveSize(stages, true, false), 0u);
}

TEST(EmulatorNativeWaveAdmission, NumericMasksAndPackedSgprResultsRequireAdmission)
{
	const auto exec_copy = Unary(ShaderInstructionType::SMovB64, Operand(ShaderOperandType::Sgpr, 4, 2),
	                             Operand(ShaderOperandType::ExecLo, 0, 2), true);
	for (auto type: {ShaderType::Pixel, ShaderType::Vertex})
	{
		EXPECT_TRUE(ShaderUsesNativeWaveState(Program({exec_copy}, type)));
		EXPECT_TRUE(ShaderUsesNativeWaveState(Program({Compare()}, type)));
		auto carry = Compare();
		carry.type = ShaderInstructionType::VAddI32;
		carry.format = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
		carry.dst2 = carry.dst;
		carry.dst = Operand(ShaderOperandType::Vgpr, 2);
		EXPECT_TRUE(ShaderUsesNativeWaveState(Program({carry}, type)));
	}
	EXPECT_TRUE(FragmentTransport::ProgramRequiresWaveTransport(Program({exec_copy})));
	EXPECT_TRUE(FragmentTransport::ProgramRequiresWaveTransport(Program({Compare()})));
	EXPECT_FALSE(FragmentTransport::ProgramRequiresWaveTransport(Program({Compare()}, ShaderType::Vertex)));
}

TEST(EmulatorNativeWaveAdmission, PerLaneMaskConsumersHaveAWidthNeutralProofButNumericEscapesDoNot)
{
	const auto compare = Compare();
	const auto local = ShaderAnalyzeNativeWave(Program({compare, Select(compare.dst)}), 64);
	EXPECT_EQ(local.proof, ShaderNativeWaveProof::LaneLocal);
	EXPECT_EQ(local.refusal_reason, nullptr);
	auto host = Host();
	host.size_control_feature_enabled = false;
	const auto local_mapping = ShaderSelectNativeSubgroup(host, VK_SHADER_STAGE_FRAGMENT_BIT, 32, local.guest_wave_size,
	                                                    local.proof == ShaderNativeWaveProof::LaneLocal, false, true);
	ASSERT_TRUE(local_mapping.supported);
	EXPECT_EQ(local_mapping.size, 0u);
	EXPECT_FALSE(local_mapping.require_size);
	const auto escape = Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 12), Operand(ShaderOperandType::Sgpr, 20));
	const auto observed = ShaderAnalyzeNativeWave(Program({compare, escape}), 64);
	EXPECT_EQ(observed.proof, ShaderNativeWaveProof::ExactSubgroup);
	EXPECT_NE(observed.refusal_reason, nullptr);
	EXPECT_EQ(observed.refusal_pc, 4u);
	// A back edge can expose a packed mask before its textual producer.
	const auto backward = ShaderAnalyzeNativeWave(Program({escape, compare}), 64);
	EXPECT_NE(backward.refusal_reason, nullptr);
	EXPECT_EQ(backward.refusal_pc, 0u);
	const auto m0 = Unary(ShaderInstructionType::SMovB32, Operand(ShaderOperandType::M0), Operand(ShaderOperandType::ExecLo));
	EXPECT_NE(ShaderAnalyzeNativeWave(Program({m0}), 64).refusal_reason, nullptr);
}

TEST(EmulatorNativeWaveAdmission, NeutralRegionProofIsRetainedSeparatelyFromUnprovenPhysicalHelpers)
{
	const auto code = NeutralRegion();
	ASSERT_TRUE(ShaderAnalyzeFragmentNativeWaveTier(code).supported);
	const auto analysis = ShaderAnalyzeNativeWave(code, 64);
	EXPECT_EQ(analysis.guest_wave_size, 64u);
	EXPECT_EQ(analysis.proof, ShaderNativeWaveProof::FragmentNeutral32);
	ASSERT_NE(analysis.refusal_reason, nullptr);
	EXPECT_NE(std::strstr(analysis.refusal_reason, "source participation"), nullptr);
	EXPECT_EQ(analysis.refusal_pc, 12u);
	auto escaped = NeutralRegion();
	auto move = Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 12), Operand(ShaderOperandType::Sgpr, 36));
	move.pc = 20;
	escaped.GetInstructions().Add(move);
	// The old tier still accepts this: it says nothing about numeric masks.
	ASSERT_TRUE(ShaderAnalyzeFragmentNativeWaveTier(escaped).supported);
	const auto numeric = ShaderAnalyzeNativeWave(escaped, 64);
	EXPECT_EQ(numeric.proof, ShaderNativeWaveProof::ExactSubgroup);
	EXPECT_EQ(numeric.refusal_pc, 20u);
	EXPECT_NE(numeric.refusal_reason, nullptr);
}

TEST(EmulatorNativeWaveAdmission, CompleteQuadLocalDppDoesNotRequireWave64OnA32LaneHost)
{
	auto move = Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 2), Operand(ShaderOperandType::Vgpr, 1));
	move.src[0].dpp = true;
	move.src[0].dpp_ctrl = 0x1bu;
	move.src[0].dpp_row_mask = move.src[0].dpp_bank_mask = 15;
	const auto quad = ShaderAnalyzeNativeWave(Program({move}), 64);
	EXPECT_EQ(quad.proof, ShaderNativeWaveProof::QuadLocal);
	EXPECT_EQ(quad.refusal_reason, nullptr);
	EXPECT_EQ(quad.guest_wave_size, 64u);
	auto host = Host();
	host.size_control_feature_enabled = false;
	const auto quad_mapping = ShaderSelectNativeSubgroup(host, VK_SHADER_STAGE_FRAGMENT_BIT, 32, quad.guest_wave_size,
	                                                   quad.proof == ShaderNativeWaveProof::QuadLocal, false, true);
	ASSERT_TRUE(quad_mapping.supported);
	EXPECT_EQ(quad_mapping.size, 0u);
	EXPECT_FALSE(quad_mapping.require_size);
}

ShaderInstruction QuadDpp()
{
	// v_mov_b32_dpp v2, v1 quad_perm:[1,1,1,1] bound_ctrl:1
	auto move = Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 2), Operand(ShaderOperandType::Vgpr, 1));
	move.src[0].dpp            = true;
	move.src[0].dpp_ctrl       = 0x55u;
	move.src[0].dpp_bound_ctrl = true;
	move.src[0].dpp_row_mask = move.src[0].dpp_bank_mask = 15;
	return move;
}

ShaderInstruction FallThroughBranch(ShaderInstructionType type)
{
	ShaderInstruction branch {};
	branch.type               = type;
	branch.format             = ShaderInstructionFormat::Label;
	branch.src[0]             = Operand(ShaderOperandType::LiteralConstant, 0, 0);
	branch.src[0].constant.i  = 0;
	branch.src_num            = 1;
	return branch;
}

TEST(EmulatorNativeWaveAdmission, QuadDppAfterADirectBranchStillSeesItsWholeQuad)
{
	// Native pixel EXEC and VCC are packed words every invocation of the subgroup holds alike, helpers
	// included, and each scalar is computed alike from them: a direct branch moves whole quads.
	for (auto type: {ShaderInstructionType::SBranch, ShaderInstructionType::SCbranchScc0, ShaderInstructionType::SCbranchScc1})
	{
		const auto info = ShaderAnalyzeNativeWave(Program({FallThroughBranch(type), QuadDpp()}), 64);
		EXPECT_STREQ(info.refusal_reason, nullptr) << static_cast<int>(type);
		EXPECT_EQ(info.proof, ShaderNativeWaveProof::QuadLocal) << static_cast<int>(type);
	}
}

TEST(EmulatorNativeWaveAdmission, GuestComputeSensitivityComesFromGuestInstructions)
{
	auto ordinary = Unary(ShaderInstructionType::SMovB32, Operand(ShaderOperandType::Sgpr, 0),
	                      Operand(ShaderOperandType::Sgpr, 1));
	EXPECT_FALSE(ShaderUsesNativeWaveState(Program({ordinary}, ShaderType::Compute)));

	auto exec_read = Unary(ShaderInstructionType::SMovB64, Operand(ShaderOperandType::Sgpr, 4, 2),
	                       Operand(ShaderOperandType::ExecLo, 0, 2), true);
	EXPECT_TRUE(ShaderUsesNativeWaveState(Program({exec_read}, ShaderType::Compute)));

	ShaderInstruction mbcnt {};
	mbcnt.type = ShaderInstructionType::VMbcntLoU32B32;
	EXPECT_TRUE(ShaderUsesNativeWaveState(Program({mbcnt}, ShaderType::Compute)));

	ordinary.src[0].dpp = true;
	EXPECT_TRUE(ShaderUsesNativeWaveState(Program({ordinary}, ShaderType::Compute)));
}

TEST(EmulatorNativeWaveAdmission, NativeComputeWave32SelectionUsesDefaultOrEnabledSizeControl)
{
	// Width selection alone needs no size-control feature for a non-varying
	// native-32 default. Coordinate admission separately requires full subgroups.
	auto state = Host();
	state.max_subgroup_size = 32;
	state.size_control_feature_supported = state.size_control_feature_enabled = false;
	state.full_subgroups_feature_supported = state.full_subgroups_feature_enabled = false;
	const auto default32 = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_COMPUTE_BIT, 32, 32, false, false, false);
	ASSERT_TRUE(default32.supported);
	EXPECT_EQ(default32.size, 32u);
	EXPECT_FALSE(default32.require_size);

	// A default-64 device must refuse when it cannot enable the compute-stage
	// size request, even though its advertised range contains 32.
	state = Host();
	state.min_subgroup_size = 32;
	state.size_control_feature_supported = state.size_control_feature_enabled = false;
	state.full_subgroups_feature_supported = state.full_subgroups_feature_enabled = false;
	EXPECT_FALSE(ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_COMPUTE_BIT, 64, 32, false, false, false).supported);

	state = Host();
	state.full_subgroups_feature_supported = state.full_subgroups_feature_enabled = false;
	const auto requested32 = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_COMPUTE_BIT, 64, 32, false, false, false);
	ASSERT_TRUE(requested32.supported);
	EXPECT_EQ(requested32.size, 32u);
	EXPECT_TRUE(requested32.require_size);
	VkPipelineShaderStageCreateInfo stage {};
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
	ASSERT_TRUE(ShaderAttachNativeSubgroup(state, requested32, &stage, &required));
	EXPECT_EQ(required.requiredSubgroupSize, 32u);
	EXPECT_EQ(stage.flags, 0u);
}

TEST(EmulatorNativeWaveAdmission, QuadDppAfterAnExportOrAnIndirectJumpLacksItsQuad)
{
	// An export kills each invocation whose own EXEC bit is clear, and an indirect jump is not lowered
	// to uniform structured flow: either may leave a quad without the member the DPP reads.
	ShaderInstruction kill {};
	kill.type   = ShaderInstructionType::Exp;
	kill.format = ShaderInstructionFormat::NullVmDone;
	auto jump = Unary(ShaderInstructionType::SSetpcB64, ShaderOperand {}, Operand(ShaderOperandType::Sgpr, 8, 2));
	jump.format = ShaderInstructionFormat::Saddr;
	for (const auto& boundary: {kill, jump})
	{
		const auto info = ShaderAnalyzeNativeWave(Program({boundary, QuadDpp()}), 64);
		ASSERT_NE(info.refusal_reason, nullptr) << static_cast<int>(boundary.type);
		EXPECT_NE(std::strstr(info.refusal_reason, "quad participation"), nullptr) << info.refusal_reason;
		EXPECT_EQ(info.refusal_pc, 4u);
	}
}

TEST(EmulatorNativeWaveAdmission, ReadfirstNeedsAnInitialRepresentedTargetQuad)
{
	const auto read = Unary(ShaderInstructionType::VReadfirstlaneB32, Operand(ShaderOperandType::Sgpr, 4), Operand(ShaderOperandType::Vgpr, 1));
	const auto initial = ShaderAnalyzeNativeWave(Program({read}), 32);
	EXPECT_EQ(initial.proof, ShaderNativeWaveProof::ExactSubgroup);
	EXPECT_EQ(initial.refusal_reason, nullptr);
	const auto write = Unary(ShaderInstructionType::SMovB64, Operand(ShaderOperandType::ExecLo, 0, 2),
	                         Operand(ShaderOperandType::Sgpr, 8, 2), true);
	const auto scalar_target = ShaderAnalyzeNativeWave(Program({write, read}), 64);
	ASSERT_NE(scalar_target.refusal_reason, nullptr);
	EXPECT_NE(std::strstr(scalar_target.refusal_reason, "READFIRSTLANE target quad"), nullptr);
	EXPECT_EQ(scalar_target.refusal_pc, 4u);
}

TEST(EmulatorNativeWaveAdmission, MalformedOperandsAndMissingGuestWidthNeverEnterNeutralProof)
{
	auto malformed = Compare();
	malformed.src_num = 5;
	EXPECT_TRUE(ShaderUsesNativeWaveState(Program({malformed})));
	EXPECT_NE(ShaderAnalyzeNativeWave(Program({malformed}), 64).refusal_reason, nullptr);
	const auto unknown = ShaderAnalyzeNativeWave(Program({Compare()}, ShaderType::Vertex), 0);
	EXPECT_EQ(unknown.proof, ShaderNativeWaveProof::Unclassified);
	EXPECT_NE(unknown.refusal_reason, nullptr);
}

TEST(EmulatorNativeWaveAdmission, GuestWidthProofAndPreferredSizeArePartOfBothPipelineIdentities)
{
	if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
	const Kyty::GuestPlatform previous_platform = Config::GetGuestPlatform();
	Config::SetNextGen(true);
	HW::VertexShaderInfo vs {};
	HW::PixelShaderInfo ps {};
	ShaderVertexInputInfo vertex {};
	ShaderPixelInputInfo pixel {};
	vertex.native_wave = pixel.native_wave = {64, ShaderNativeWaveProof::ExactSubgroup, 0, nullptr};
	const auto vertex64 = ShaderGetIdVS(&vs, &vertex);
	const auto pixel64 = ShaderGetIdPS(&ps, &pixel);
	vertex.native_wave.guest_wave_size = pixel.native_wave.guest_wave_size = 32;
	EXPECT_NE(vertex64, ShaderGetIdVS(&vs, &vertex));
	EXPECT_NE(pixel64, ShaderGetIdPS(&ps, &pixel));
	vertex.native_wave.guest_wave_size = pixel.native_wave.guest_wave_size = 64;
	vertex.native_wave.proof = pixel.native_wave.proof = ShaderNativeWaveProof::LaneLocal;
	EXPECT_NE(vertex64, ShaderGetIdVS(&vs, &vertex));
	EXPECT_NE(pixel64, ShaderGetIdPS(&ps, &pixel));
	vertex.native_wave.proof = pixel.native_wave.proof = ShaderNativeWaveProof::ExactSubgroup;
	vertex.required_subgroup_size = pixel.required_subgroup_size = 64;
	EXPECT_NE(vertex64, ShaderGetIdVS(&vs, &vertex));
	EXPECT_NE(pixel64, ShaderGetIdPS(&ps, &pixel));
	HW::ComputeShaderInfo cs {};
	ShaderComputeInputInfo compute {};
	const auto compute_neutral = ShaderGetIdCS(&cs, &compute);
	compute.native_wave_sensitive = true;
	compute.required_subgroup_size = 32u;
	EXPECT_NE(compute_neutral, ShaderGetIdCS(&cs, &compute));
	Config::ResetGuestPlatform();
	if (previous_platform != Kyty::GuestPlatform::Unknown)
	{
		EXPECT_TRUE(Config::SetGuestPlatform(previous_platform));
	}
}

TEST(EmulatorNativeWaveAdmission, PairedGuestCeilRejectsUint64BoundaryWithoutMutatingStage)
{
	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(Host());
	ShaderComputeWaveLayout layout {};
	layout.strategy = ShaderComputeWaveStrategy::Paired64On32;
	layout.guest_wave_size = 64;
	layout.native_subgroup_size = 32;
	layout.banks = 2;
	layout.waves = 1;
	layout.guest_local[0] = UINT32_MAX;
	layout.guest_local[1] = 641;
	layout.guest_local[2] = 6700417; // Product is exactly UINT64_MAX.
	layout.physical_local[0] = 32;
	layout.physical_local[1] = layout.physical_local[2] = 1;
	VkPipelineShaderStageCreateInfo stage {};
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
	EXPECT_FALSE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required));
	EXPECT_EQ(stage.pNext, nullptr);
	EXPECT_EQ(stage.flags, 0u);
	layout.guest_local[0] = 65;
	layout.guest_local[1] = layout.guest_local[2] = 1;
	layout.waves = 2;
	layout.physical_local[0] = 64;
	EXPECT_TRUE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required));
}

TEST(EmulatorNativeWaveAdmission, NativeWave32RequiresFullSubgroupsForLogicalLaneMapping)
{
	auto state = Host();
	state.full_subgroups_feature_supported = state.full_subgroups_feature_enabled = false;
	auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(state);
	ShaderComputeWaveLayout layout {};
	layout.strategy = ShaderComputeWaveStrategy::Native;
	layout.guest_wave_size = 32;
	layout.native_subgroup_size = 32;
	layout.banks = 1;
	layout.guest_local[0] = layout.physical_local[0] = 64;
	layout.guest_local[1] = layout.physical_local[1] = 1;
	layout.guest_local[2] = layout.physical_local[2] = 1;
	VkPipelineShaderStageCreateInfo stage {};
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
	EXPECT_FALSE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required));
	EXPECT_EQ(stage.pNext, nullptr);
	EXPECT_EQ(stage.flags, 0u);

	state.full_subgroups_feature_supported = state.full_subgroups_feature_enabled = true;
	capabilities = ShaderComputeWaveVulkanBuildCapabilities(state);
	ASSERT_TRUE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required));
	EXPECT_EQ(required.requiredSubgroupSize, 32u);
	EXPECT_EQ(stage.flags, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT);
	EXPECT_EQ(stage.pNext, &required);

	// The queried maximum-compute-workgroup-subgroups limit is part of native
	// exact-width admission: 64 invocations cannot fit within one 32-lane slot.
	capabilities.max_subgroups = 1;
	stage = {};
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	required = {};
	EXPECT_FALSE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required));
	EXPECT_EQ(stage.pNext, nullptr);
	EXPECT_EQ(stage.flags, 0u);

	// Exact size alone cannot launch a full trailing wave or prove its index.
	capabilities.max_subgroups = 32;
	layout.guest_local[0] = layout.physical_local[0] = 48;
	stage = {};
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	required = {};
	EXPECT_FALSE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required));
	EXPECT_EQ(stage.pNext, nullptr);
	EXPECT_EQ(stage.flags, 0u);
}

TEST(EmulatorNativeWaveAdmission, NativeWave32Spirv16FullSubgroupsUseActualModuleVersion)
{
	auto state = Host();
	state.full_subgroups_feature_supported = state.full_subgroups_feature_enabled = false;
	const auto capabilities = ShaderComputeWaveVulkanBuildCapabilities(state);
	ShaderComputeWaveLayout layout {};
	layout.strategy = ShaderComputeWaveStrategy::Native;
	layout.guest_wave_size = layout.native_subgroup_size = 32;
	layout.banks = 1;
	layout.guest_local[0] = layout.physical_local[0] = 32;
	layout.guest_local[1] = layout.physical_local[1] = 2;
	layout.guest_local[2] = layout.physical_local[2] = 1;
	VkPipelineShaderStageCreateInfo stage {};
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
	EXPECT_FALSE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required, true, 0x00010500u));
	ASSERT_TRUE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, capabilities, &stage, &required, true, 0x00010600u));
	EXPECT_EQ(stage.pNext, &required);
	EXPECT_EQ(required.requiredSubgroupSize, 32u);
	EXPECT_EQ(stage.flags, 0u);

	// A singleton queried range fixes width even for SPIR-V 1.6 without the
	// size-control feature, so the full-subgroup guarantee needs no flags/node.
	state.min_subgroup_size = state.max_subgroup_size = 32;
	state.size_control_feature_supported = state.size_control_feature_enabled = false;
	const auto fixed_capabilities = ShaderComputeWaveVulkanBuildCapabilities(state);
	const auto fixed_selection = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_COMPUTE_BIT, 32u, 32u, false, false, true);
	ASSERT_TRUE(fixed_selection.supported);
	EXPECT_FALSE(fixed_selection.require_size);
	stage = {};
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	required = {};
	ASSERT_TRUE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, fixed_capabilities, &stage, &required,
	                                                           fixed_selection.require_size, 0x00010600u));
	EXPECT_EQ(stage.pNext, nullptr);
	EXPECT_EQ(stage.flags, 0u);

	// Total workgroup size being divisible by 32 does not prove full subgroups
	// when X itself is not a multiple of the effective subgroup size.
	layout.guest_local[0] = layout.physical_local[0] = 16;
	layout.guest_local[1] = layout.physical_local[1] = 4;
	EXPECT_FALSE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, fixed_capabilities, &stage, &required,
	                                                            false, 0x00010600u));
}

UT_END();
