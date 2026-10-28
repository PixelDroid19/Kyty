#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"
#include "Emulator/Graphics/FragmentTransportAdmission.h"
#include "Emulator/Graphics/GraphicsGeState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"
#include "Emulator/Graphics/ShaderFragmentLaneReduction.h"
#include "Emulator/Graphics/ShaderFragmentMaskFlow.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "NggPassthroughFixture.h"
#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"

#include "spirv/unified1/spirv.hpp"

#include <array>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

UT_BEGIN(EmulatorNativeWaveAdmission);

using namespace Libs::Graphics;

namespace {

class ScopedShaderValidation final: public Config::ConfigSource
{
public:
	ScopedShaderValidation(): m_previous(Config::ShaderValidationEnabled()) { Config::Load(*this); }
	~ScopedShaderValidation() override
	{
		m_enabled = m_previous;
		Config::Load(*this);
	}
	bool Has(const Core::String& key) const override { return key == U"ShaderValidationEnabled"; }
	int64_t GetInteger(const Core::String&) const override { return 0; }
	bool GetBool(const Core::String&) const override { return m_enabled; }
	Core::String GetString(const Core::String&) const override { return {}; }

private:
	bool m_previous;
	bool m_enabled = true;
};

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

ShaderOperand Constant(uint32_t value)
{
	auto operand       = Operand(ShaderOperandType::IntegerInlineConstant, 0, 0);
	operand.constant.u = value;
	return operand;
}

ShaderInstruction Vector(ShaderInstructionType type, int dst, ShaderOperand a, ShaderOperand b)
{
	ShaderInstruction inst {};
	inst.type = type;
	inst.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	inst.dst = Operand(ShaderOperandType::Vgpr, dst);
	inst.src[0] = a;
	inst.src[1] = b;
	inst.src_num = 2;
	return inst;
}

// v10 |= v10 shifted right by `shift` lanes inside each row.
ShaderInstruction RowShiftOr(uint32_t shift, uint8_t row_mask = 15)
{
	auto inst = Vector(ShaderInstructionType::VOrB32, 10, Operand(ShaderOperandType::Vgpr, 10), Operand(ShaderOperandType::Vgpr, 10));
	inst.src[0].dpp = true;
	inst.src[0].dpp_ctrl = static_cast<uint16_t>(0x110u + shift);
	inst.src[0].dpp_row_mask = row_mask;
	inst.src[0].dpp_bank_mask = 15;
	return inst;
}

ShaderInstruction Readlane(int dst, uint32_t lane)
{
	auto inst = Vector(ShaderInstructionType::VReadlaneB32, 0, Operand(ShaderOperandType::Vgpr, 10), Constant(lane));
	inst.dst = Operand(ShaderOperandType::Sgpr, dst);
	return inst;
}

ShaderInstruction Endpgm()
{
	ShaderInstruction inst {};
	inst.type = ShaderInstructionType::SEndpgm;
	inst.format = ShaderInstructionFormat::Empty;
	return inst;
}

// The whole-wave OR of a seed `cond ? v9 : 0` (cond = the entry EXEC): row-shift ORs fold each row onto its
// last lane, PERMLANEX16 brings the other row of each half, and lanes 31 and 63 are read after the restore.
// `between` sits between the restore and the reads, `tail` follows the reads.
ShaderCode Reduction(std::initializer_list<ShaderInstruction> tail, uint8_t row_mask = 15, std::initializer_list<ShaderInstruction> between = {})
{
	const auto exec  = Operand(ShaderOperandType::ExecLo, 0, 2);
	const auto mask  = Operand(ShaderOperandType::Sgpr, 36, 2);
	const auto saved = Operand(ShaderOperandType::VccLo, 0, 2);
	ShaderInstruction permute {};
	permute.type = ShaderInstructionType::VPermlanex16B32;
	permute.format = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
	permute.dst = Operand(ShaderOperandType::Vgpr, 9);
	permute.src[0] = Operand(ShaderOperandType::Vgpr, 10);
	permute.src[1] = permute.src[2] = Constant(0xffffffffu);
	permute.src_num = 3;
	std::vector<ShaderInstruction> body {Unary(ShaderInstructionType::SMovB64, mask, exec, true),
	                                     Unary(ShaderInstructionType::SOrn2SaveexecB64, saved, mask, true),
	                                     Select(mask),
	                                     RowShiftOr(1, row_mask),
	                                     RowShiftOr(2),
	                                     RowShiftOr(4),
	                                     RowShiftOr(8),
	                                     permute,
	                                     Vector(ShaderInstructionType::VOrB32, 10, Operand(ShaderOperandType::Vgpr, 10),
	                                            Operand(ShaderOperandType::Vgpr, 9)),
	                                     Unary(ShaderInstructionType::SMovB64, exec, saved, true)};
	body.insert(body.end(), between.begin(), between.end());
	body.push_back(Readlane(0, 31));
	body.push_back(Readlane(1, 63));
	body.insert(body.end(), tail.begin(), tail.end());
	body.push_back(Endpgm());
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	for (auto inst: body)
	{
		inst.pc = code.GetInstructions().Size() * 4u;
		code.GetInstructions().Add(inst);
	}
	return code;
}

// The Wave32 form of the reduction, as a guest program encodes it: B32 EXEC save, ORN2_SAVEEXEC and restore on
// single SGPRs, the select condition encoded as a pair, row-shift ORs folding each row onto its last lane, and
// lanes 15 and `high_lane` read after an unrelated VALU instruction.
ShaderCode Wave32Reduction(std::initializer_list<ShaderInstruction> tail, uint32_t high_lane = 31)
{
	const auto exec  = Operand(ShaderOperandType::ExecLo, 0, 1);
	const auto mask  = Operand(ShaderOperandType::Sgpr, 36, 1);
	const auto saved = Operand(ShaderOperandType::VccLo, 0, 1);
	std::vector<ShaderInstruction> body {Unary(ShaderInstructionType::SMovB32, mask, exec),
	                                     Unary(ShaderInstructionType::SOrn2SaveexecB32, saved, mask),
	                                     Select(Operand(ShaderOperandType::Sgpr, 36, 2)),
	                                     RowShiftOr(1),
	                                     RowShiftOr(2),
	                                     RowShiftOr(4),
	                                     RowShiftOr(8),
	                                     Unary(ShaderInstructionType::SMovB32, exec, saved),
	                                     Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 12), Constant(1)),
	                                     Readlane(0, 15),
	                                     Readlane(1, high_lane)};
	body.insert(body.end(), tail.begin(), tail.end());
	body.push_back(Endpgm());
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	for (auto inst: body)
	{
		inst.pc = code.GetInstructions().Size() * 4u;
		code.GetInstructions().Add(inst);
	}
	return code;
}

// Overwrites of the network registers under the restored EXEC.
ShaderInstruction Clear(int vgpr)
{
	return Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, vgpr), Constant(0));
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
	layout.guest_local[0] = 32;
	layout.guest_local[1] = 2;
	layout.guest_local[2] = 1;
	layout.physical_local[0] = 64;
	layout.physical_local[1] = layout.physical_local[2] = 1;
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

	// Full subgroups need a host X that is a multiple of the subgroup size, so
	// the guest shape is never launched as is when its X is not.
	layout.guest_local[0] = layout.physical_local[0] = 16;
	layout.guest_local[1] = layout.physical_local[1] = 4;
	EXPECT_FALSE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, fixed_capabilities, &stage, &required,
	                                                            false, 0x00010600u));
	layout.physical_local[0] = 64;
	layout.physical_local[1] = 1;
	EXPECT_TRUE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, fixed_capabilities, &stage, &required,
	                                                           false, 0x00010600u));

	// A group smaller than one wave pads to one subgroup of inactive lanes.
	layout.guest_local[0] = layout.guest_local[1] = 1;
	layout.physical_local[0] = 1;
	EXPECT_FALSE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, fixed_capabilities, &stage, &required,
	                                                            false, 0x00010600u));
	layout.physical_local[0] = 32;
	EXPECT_TRUE(ShaderComputeWaveVulkanAttachRequiredSubgroupSize(layout, fixed_capabilities, &stage, &required,
	                                                           false, 0x00010600u));
}


TEST(EmulatorNativeWaveAdmission, WholeWaveOrReductionRunsAsAPartialWaveOfAnyWidth)
{
	const auto code = Reduction({Clear(9), Clear(10)});
	const auto info = ShaderAnalyzeNativeWave(code, 64);
	EXPECT_EQ(info.proof, ShaderNativeWaveProof::FragmentPartialWave);
	EXPECT_EQ(info.refusal_reason, nullptr);
	const auto reductions = ShaderFindFragmentLaneReductions(code, ShaderAnalyzeFragmentMaskFlow(code, 64u));
	ASSERT_EQ(reductions.reductions.size(), 1u);
	EXPECT_FALSE(reductions.IsStep(2));
	EXPECT_TRUE(reductions.IsStep(3));
	EXPECT_TRUE(reductions.IsStep(8));
	EXPECT_FALSE(reductions.IsStep(9));
	uint64_t lanes = 0;
	ASSERT_NE(reductions.ReadAt(10, &lanes), nullptr);
	EXPECT_EQ(lanes, 0x00000000ffffffffull);
	ASSERT_NE(reductions.ReadAt(11, &lanes), nullptr);
	EXPECT_EQ(lanes, 0xffffffff00000000ull);
	EXPECT_EQ(reductions.reductions[0].seed, 10);
}

TEST(EmulatorNativeWaveAdmission, PartialWaveRefusesObservedNetworkValues)
{
	const auto row_refusal = [](const ShaderCode& code)
	{
		const auto info = ShaderAnalyzeNativeWave(code, 64);
		EXPECT_NE(info.proof, ShaderNativeWaveProof::FragmentPartialWave);
		ASSERT_NE(info.refusal_reason, nullptr);
		EXPECT_NE(std::strstr(info.refusal_reason, "row DPP"), nullptr) << info.refusal_reason;
		EXPECT_EQ(info.refusal_pc, 12u);
	};
	// An intermediate lane of the network is read before it is overwritten.
	row_refusal(Reduction({Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 11), Operand(ShaderOperandType::Vgpr, 9)),
	                       Clear(9), Clear(10)}));
	// An export reads a network register.
	ShaderInstruction export_color {};
	export_color.type = ShaderInstructionType::Exp;
	export_color.format = ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone;
	for (int source = 0; source < 4; ++source) { export_color.src[source] = Operand(ShaderOperandType::Vgpr, 9 + source); }
	export_color.src_num = 4;
	row_refusal(Reduction({export_color}));
	// A row-masked step is not an OR of the whole row.
	row_refusal(Reduction({Clear(9), Clear(10)}, 3));
}

TEST(EmulatorNativeWaveAdmission, ExactGuestWidthOnlySupersedesParticipationRefusals)
{
	const char* participation[] = {
	    "fragment wave append/consume lacks a participating-lane proof",
	    "fragment mask packing after an export or indirect jump lacks a quad participation proof",
	    "READFIRSTLANE target quad is not proven to retain a participating real member",
	    "READLANE physical source may be unavailable or in an all-helper quad",
	    "fragment permutation source participation is unproven",
	    "fragment DPP after an export or indirect jump lacks a quad participation proof",
	    "fragment row DPP source participation is unproven (neutral values do not prove helper recovery)",
	};
	for (const auto* reason: participation) { EXPECT_TRUE(ShaderNativeWaveParticipationRefusal(reason)) << reason; }
	EXPECT_FALSE(ShaderNativeWaveParticipationRefusal(nullptr));
	EXPECT_FALSE(ShaderNativeWaveParticipationRefusal("fragment numeric mask observation lacks an all-helper/unavailable-quad proof"));
	EXPECT_FALSE(ShaderNativeWaveParticipationRefusal("guest wave width is not established by stage control"));

	// AMD-like: the device pins a fragment subgroup to the guest's 64 lanes.
	auto state = Host();
	state.min_subgroup_size = 32;
	state.max_subgroup_size = 64;
	auto mapping = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 64, false, false, true);
	ASSERT_TRUE(mapping.supported);
	EXPECT_EQ(mapping.size, 64u);
	EXPECT_TRUE(mapping.require_size);
	// Intel-like: fragment sizes stop at 32, so the same program needs transport.
	state.max_subgroup_size = 32;
	state.fragment_required_size_supported = false;
	mapping = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 64, false, false, true);
	EXPECT_FALSE(mapping.supported);
}

TEST(EmulatorNativeWaveAdmission, NeutralRegionParticipationRefusalIsWidthOnlyButNumericEscapeIsNot)
{
	const auto region = ShaderAnalyzeNativeWave(NeutralRegion(), 64);
	ASSERT_NE(region.refusal_reason, nullptr);
	EXPECT_TRUE(ShaderNativeWaveParticipationRefusal(region.refusal_reason)) << region.refusal_reason;

	auto escaped = NeutralRegion();
	auto move = Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 12), Operand(ShaderOperandType::Sgpr, 36));
	move.pc = 20;
	escaped.GetInstructions().Add(move);
	const auto numeric = ShaderAnalyzeNativeWave(escaped, 64);
	ASSERT_NE(numeric.refusal_reason, nullptr);
	EXPECT_FALSE(ShaderNativeWaveParticipationRefusal(numeric.refusal_reason)) << numeric.refusal_reason;
}

TEST(EmulatorNativeWaveAdmission, ReductionReadsMayFollowInstructionsThatNameNoNetworkRegister)
{
	const auto unrelated = Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 12), Constant(1));
	const auto code      = Reduction({Clear(9), Clear(10)}, 15, {unrelated});
	EXPECT_EQ(ShaderAnalyzeNativeWave(code, 64).proof, ShaderNativeWaveProof::FragmentPartialWave);
	const auto reductions = ShaderFindFragmentLaneReductions(code, ShaderAnalyzeFragmentMaskFlow(code, 64u));
	ASSERT_EQ(reductions.reductions.size(), 1u);
	uint64_t lanes = 0;
	EXPECT_EQ(reductions.ReadAt(10, &lanes), nullptr);
	ASSERT_NE(reductions.ReadAt(11, &lanes), nullptr);
	EXPECT_EQ(lanes, 0x00000000ffffffffull);
	// An instruction that reads a network register before the reads observes a step result.
	const auto observes = Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 12), Operand(ShaderOperandType::Vgpr, 10));
	EXPECT_NE(ShaderAnalyzeNativeWave(Reduction({Clear(9), Clear(10)}, 15, {observes}), 64).proof, ShaderNativeWaveProof::FragmentPartialWave);
}

TEST(EmulatorNativeWaveAdmission, Wave32OrReductionRunsAsAPartialWave)
{
	const auto code = Wave32Reduction({Clear(10)});
	const auto info = ShaderAnalyzeNativeWave(code, 32);
	EXPECT_EQ(info.proof, ShaderNativeWaveProof::FragmentPartialWave);
	EXPECT_EQ(info.refusal_reason, nullptr);
	const auto reductions = ShaderFindFragmentLaneReductions(code, ShaderAnalyzeFragmentMaskFlow(code, 32u));
	ASSERT_EQ(reductions.reductions.size(), 1u);
	EXPECT_TRUE(reductions.IsStep(3));
	EXPECT_FALSE(reductions.IsStep(7));
	uint64_t lanes = 0;
	ASSERT_NE(reductions.ReadAt(9, &lanes), nullptr);
	EXPECT_EQ(lanes, 0x000000000000ffffull);
	ASSERT_NE(reductions.ReadAt(10, &lanes), nullptr);
	EXPECT_EQ(lanes, 0x00000000ffff0000ull);
	// In a Wave64 program the B32 forms move only the low half of EXEC: no region, no reduction.
	EXPECT_NE(ShaderAnalyzeNativeWave(code, 64).proof, ShaderNativeWaveProof::FragmentPartialWave);
	// A lane select past the Wave32 width is not a static lane of the wave.
	EXPECT_NE(ShaderAnalyzeNativeWave(Wave32Reduction({Clear(10)}, 32), 32).proof, ShaderNativeWaveProof::FragmentPartialWave);
}

TEST(EmulatorNativeWaveAdmission, Wave32OrReductionMayHoldScalarLoadsAmongItsSteps)
{
	const auto load = [](ShaderOperand dst)
	{
		ShaderInstruction inst {};
		inst.type    = ShaderInstructionType::SBufferLoadDword;
		inst.format  = ShaderInstructionFormat::SdstSvSoffset;
		inst.dst     = dst;
		inst.src[0]  = Operand(ShaderOperandType::Sgpr, 12, 4);
		inst.src[1]  = Constant(0);
		inst.src_num = 2;
		return inst;
	};
	// One load between the entry and the seed, one between the first two steps.
	const auto with_loads = [](const ShaderInstruction& before_seed, const ShaderInstruction& among_steps)
	{
		const auto plain = Wave32Reduction({Clear(10)});
		ShaderCode code;
		code.SetType(ShaderType::Pixel);
		const auto add = [&code](ShaderInstruction inst)
		{
			inst.pc = code.GetInstructions().Size() * 4u;
			code.GetInstructions().Add(inst);
		};
		for (uint32_t index = 0; index < plain.GetInstructions().Size(); ++index)
		{
			if (index == 2u) { add(before_seed); }
			if (index == 4u) { add(among_steps); }
			add(plain.GetInstructions().At(index));
		}
		return code;
	};
	const auto code = with_loads(load(Operand(ShaderOperandType::VccHi)), load(Operand(ShaderOperandType::Sgpr, 16)));
	EXPECT_EQ(ShaderAnalyzeNativeWave(code, 32).proof, ShaderNativeWaveProof::FragmentPartialWave);
	const auto reductions = ShaderFindFragmentLaneReductions(code, ShaderAnalyzeFragmentMaskFlow(code, 32u));
	ASSERT_EQ(reductions.reductions.size(), 1u);
	EXPECT_EQ(reductions.reductions[0].seed_index, 3u);
	EXPECT_FALSE(reductions.IsStep(2));
	EXPECT_TRUE(reductions.IsStep(4));
	EXPECT_FALSE(reductions.IsStep(5)); // the load runs as written
	EXPECT_TRUE(reductions.IsStep(6));
	uint64_t lanes = 0;
	ASSERT_NE(reductions.ReadAt(11, &lanes), nullptr);
	EXPECT_EQ(lanes, 0x000000000000ffffull);
	// A load that writes the seed condition, the saved mask or EXEC breaks the region.
	const auto refused = [&](const ShaderCode& program)
	{ EXPECT_TRUE(ShaderFindFragmentLaneReductions(program, ShaderAnalyzeFragmentMaskFlow(program, 32u)).reductions.empty()); };
	const auto other = load(Operand(ShaderOperandType::Sgpr, 16));
	refused(with_loads(load(Operand(ShaderOperandType::Sgpr, 36)), other));
	refused(with_loads(load(Operand(ShaderOperandType::VccLo)), other));
	refused(with_loads(other, load(Operand(ShaderOperandType::VccLo))));
	refused(with_loads(other, load(Operand(ShaderOperandType::ExecLo))));
}

// The guest form of the entry: the launch mask saved before WQM, the condition taken as EXEC & launch, an
// ORN2_SAVEEXEC of EXEC itself, and M0 saved for the region's interpolation-free span.
TEST(EmulatorNativeWaveAdmission, PartialWaveEntryMaySaveExecItselfAndCopyM0)
{
	const auto exec   = Operand(ShaderOperandType::ExecLo, 0, 2);
	const auto launch = Operand(ShaderOperandType::Sgpr, 30, 2);
	const auto mask   = Operand(ShaderOperandType::Sgpr, 36, 2);
	ShaderInstruction condition {};
	condition.type    = ShaderInstructionType::SAndB64;
	condition.format  = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
	condition.dst     = mask;
	condition.src[0]  = exec;
	condition.src[1]  = launch;
	condition.src_num = 2;
	const auto plain = Reduction({Clear(9), Clear(10)});
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	const auto add = [&code](ShaderInstruction inst)
	{
		inst.pc = code.GetInstructions().Size() * 4u;
		code.GetInstructions().Add(inst);
	};
	add(Unary(ShaderInstructionType::SMovB64, launch, exec, true));
	add(Unary(ShaderInstructionType::SWqmB64, exec, exec, true));
	add(condition);
	add(Unary(ShaderInstructionType::SOrn2SaveexecB64, Operand(ShaderOperandType::VccLo, 0, 2), exec, true));
	add(Unary(ShaderInstructionType::SMovB32, Operand(ShaderOperandType::M0), Operand(ShaderOperandType::Sgpr, 32)));
	for (uint32_t index = 2; index < plain.GetInstructions().Size(); ++index) { add(plain.GetInstructions().At(index)); }
	EXPECT_EQ(ShaderAnalyzeNativeWave(code, 64).proof, ShaderNativeWaveProof::FragmentPartialWave);
	const auto reductions = ShaderFindFragmentLaneReductions(code, ShaderAnalyzeFragmentMaskFlow(code, 64u));
	ASSERT_EQ(reductions.reductions.size(), 1u);
	EXPECT_EQ(reductions.reductions[0].seed_index, 5u);
	EXPECT_FALSE(reductions.IsStep(4)); // the M0 copy runs as written
	EXPECT_TRUE(reductions.IsStep(6));
}

// The first write after the reads ends a network register only when it replaces every lane EXEC holds.
TEST(EmulatorNativeWaveAdmission, PartialWaveNetworkEndsOnlyAtWholeDestinationWrites)
{
	const auto exec    = Operand(ShaderOperandType::ExecLo, 0, 2);
	const auto widen   = Unary(ShaderInstructionType::SWqmB64, exec, exec, true);
	const auto partial = [](const ShaderCode& code)
	{
		const auto info = ShaderAnalyzeNativeWave(code, 64);
		EXPECT_NE(info.proof, ShaderNativeWaveProof::FragmentPartialWave);
		ASSERT_NE(info.refusal_reason, nullptr);
		EXPECT_NE(std::strstr(info.refusal_reason, "row DPP"), nullptr) << info.refusal_reason;
		EXPECT_EQ(info.refusal_pc, 12u);
	};
	// SDWA with DST_SEL DWORD writes the whole register; WORD_0 with UNUSED_PRESERVE keeps its high half.
	const auto sdwa = [](uint32_t dst_sel, uint32_t dst_unused)
	{
		auto inst = Vector(ShaderInstructionType::VMulF32, 9, Operand(ShaderOperandType::Vgpr, 11), Operand(ShaderOperandType::Vgpr, 12));
		inst.vop_sdwa      = true;
		inst.vop_sdwa_ctrl = (6u << 24u) | (6u << 16u) | (dst_unused << 11u) | (dst_sel << 8u);
		inst.src[0].swizzle = inst.src[1].swizzle = 6;
		return inst;
	};
	EXPECT_EQ(ShaderAnalyzeNativeWave(Reduction({sdwa(6, 0), Clear(10)}), 64).proof, ShaderNativeWaveProof::FragmentPartialWave);
	partial(Reduction({sdwa(4, 2), Clear(10)}));
	// A quad permutation always has its source lane, so it writes every lane the row and bank masks enable.
	const auto quad = [](bool bound_ctrl, uint8_t row_mask)
	{
		auto inst = Vector(ShaderInstructionType::VSubF32, 9, Operand(ShaderOperandType::Vgpr, 11), Operand(ShaderOperandType::Vgpr, 11));
		inst.src[0].dpp            = true;
		inst.src[0].dpp_ctrl       = 0x55u;
		inst.src[0].dpp_bound_ctrl = bound_ctrl;
		inst.src[0].dpp_row_mask   = row_mask;
		inst.src[0].dpp_bank_mask  = 15;
		return inst;
	};
	EXPECT_EQ(ShaderAnalyzeNativeWave(Reduction({widen, quad(true, 15), Clear(10)}), 64).proof, ShaderNativeWaveProof::FragmentPartialWave);
	EXPECT_EQ(ShaderAnalyzeNativeWave(Reduction({widen, quad(false, 15), Clear(10)}), 64).proof, ShaderNativeWaveProof::FragmentPartialWave);
	partial(Reduction({widen, quad(true, 3), Clear(10)}));
}

TEST(EmulatorNativeWaveAdmission, PartialWaveKeepsStaleLanesOutsideTheRestoredExec)
{
	const auto exec  = Operand(ShaderOperandType::ExecLo, 0, 2);
	const auto read  = Unary(ShaderInstructionType::VMovB32, Operand(ShaderOperandType::Vgpr, 11), Operand(ShaderOperandType::Vgpr, 10));
	const auto widen = Unary(ShaderInstructionType::SWqmB64, exec, exec, true);
	// The overwrite under the restored EXEC leaves lanes outside it stale; reading inside it is exact.
	EXPECT_EQ(ShaderAnalyzeNativeWave(Reduction({Clear(9), Clear(10), read}), 64).proof, ShaderNativeWaveProof::FragmentPartialWave);
	// Widening EXEC lets a stale lane compute with the network value.
	const auto widened = ShaderAnalyzeNativeWave(Reduction({Clear(9), Clear(10), widen, read}), 64);
	EXPECT_NE(widened.proof, ShaderNativeWaveProof::FragmentPartialWave);
	EXPECT_NE(widened.refusal_reason, nullptr);
	// After WQM EXEC holds every launched lane, so an overwrite ends the register in all of them.
	EXPECT_EQ(ShaderAnalyzeNativeWave(Reduction({widen, Clear(9), Clear(10), read}), 64).proof, ShaderNativeWaveProof::FragmentPartialWave);
	// The same obligation in a Wave32 program follows its B32 EXEC writes.
	const auto exec32  = Operand(ShaderOperandType::ExecLo, 0, 1);
	const auto widen32 = Unary(ShaderInstructionType::SWqmB32, exec32, exec32);
	EXPECT_EQ(ShaderAnalyzeNativeWave(Wave32Reduction({Clear(10), read}), 32).proof, ShaderNativeWaveProof::FragmentPartialWave);
	EXPECT_NE(ShaderAnalyzeNativeWave(Wave32Reduction({Clear(10), widen32, read}), 32).proof, ShaderNativeWaveProof::FragmentPartialWave);
}

TEST(EmulatorNativeWaveAdmission, PartialWaveReductionEmitsOneSubgroupOrPerRead)
{
	ASSERT_EXIT(([] {
		NggFixture::Initialize();
		ScopedShaderValidation validation;
		if (!Config::ShaderValidationEnabled()) { std::_Exit(4); }
		// Wave64: six network steps; Wave32: four.
		const std::array<std::pair<ShaderCode, uint32_t>, 2> programs {{{Reduction({Clear(9), Clear(10)}), 64u}, {Wave32Reduction({Clear(10)}), 32u}}};
		for (const auto& [code, wave]: programs)
		{
			ShaderPixelInputInfo input {};
			input.native_wave = ShaderAnalyzeNativeWave(code, wave);
			if (input.native_wave.proof != ShaderNativeWaveProof::FragmentPartialWave) { std::_Exit(1); }
			const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
			const auto count  = [&source](const char* text)
			{
				size_t found = 0;
				for (auto at = source.FindIndex(text); at != Core::STRING8_INVALID_INDEX; at = source.FindIndex(text, at + 1)) { found++; }
				return found;
			};
			// The network steps are not run; each of the two reads is one subgroup OR of its lanes' seeds.
			if (count("; reduction step:") != (wave == 64u ? 6u : 4u) ||
			    count("= OpGroupNonUniformBitwiseOr %uint %uint_3 Reduce %lr_part_") != 2u)
			{
				std::_Exit(2);
			}
			Kyty::Vector<uint32_t> binary;
			String8                error;
			if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
			{
				std::fprintf(stderr, "%s\n", error.c_str());
				std::_Exit(3);
			}
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

// A pixel append allocation: each valid pixel takes counter + MBCNT(launch EXEC) as its slot.
//   v_mbcnt_hi_u32_b32 v7, exec_hi, 0 / s_mov_b32 m0, s20 / ds_append v6 gds / v_mbcnt_lo_u32_b32 v0, <mask>, v7
static ShaderCode AppendAllocation(bool branch_first, uint32_t low_mask = 126u)
{
	std::vector<uint32_t> words;
	if (branch_first) { words.push_back(0xbf820000u); } // s_branch to the next instruction
	for (uint32_t word: {0xd7660007u, 0x0001007fu, 0xbefc0314u, 0xd8f80000u | (1u << 17u), 0x06000000u, 0xd7650000u,
	                     low_mask | (263u << 9u), 0xbf810000u})
	{
		words.push_back(word);
	}
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	EXPECT_TRUE(ShaderTryParseBounded(words.data(), static_cast<uint32_t>(words.size() * sizeof(uint32_t)), &code));
	return code;
}

TEST(EmulatorNativeWaveAdmission, LaunchExecAppendAllocationRunsAsAPartialWave)
{
	NggFixture::Initialize();
	for (uint32_t wave: {32u, 64u})
	{
		EXPECT_EQ(ShaderAnalyzeNativeWave(AppendAllocation(false), wave).proof, ShaderNativeWaveProof::FragmentPartialWave);
		// After a branch EXEC is no longer known to be the launch mask.
		EXPECT_NE(ShaderAnalyzeNativeWave(AppendAllocation(true), wave).proof, ShaderNativeWaveProof::FragmentPartialWave);
		// VCC_LO is not the valid-pixel mask the counter was advanced by.
		EXPECT_NE(ShaderAnalyzeNativeWave(AppendAllocation(false, 106u), wave).proof, ShaderNativeWaveProof::FragmentPartialWave);
	}
}

TEST(EmulatorNativeWaveAdmission, PartialWaveAppendCountsOnlyValidPixels)
{
	ASSERT_EXIT(([] {
		NggFixture::Initialize();
		ScopedShaderValidation validation;
		if (!Config::ShaderValidationEnabled()) { std::_Exit(4); }
		const auto code = AppendAllocation(false);
		for (uint32_t wave: {32u, 64u})
		{
			ShaderPixelInputInfo input {};
			input.native_wave                         = ShaderAnalyzeNativeWave(code, wave);
			input.bind.gds_pointers.pointers_num      = 1;
			input.bind.gds_pointers.start_register[0] = 20;
			ShaderCalcBindingIndices(&input.bind);
			if (input.native_wave.proof != ShaderNativeWaveProof::FragmentPartialWave) { std::_Exit(1); }
			const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
			const auto count  = [&source](const char* text)
			{
				size_t found = 0;
				for (auto at = source.FindIndex(text); at != Core::STRING8_INVALID_INDEX; at = source.FindIndex(text, at + 1)) { found++; }
				return found;
			};
			// One counter update, sized and led by the valid-pixel ballot both MBCNTs count.
			if (count("OpAtomicIAdd") != 1u || count("_valid = OpLogicalAnd %bool") != 3u ||
			    count("OpGroupNonUniformElect") != 0u || count("OpGroupNonUniformBroadcastFirst") != 0u)
			{
				std::_Exit(2);
			}
			Kyty::Vector<uint32_t> binary;
			String8                error;
			if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
			{
				std::fprintf(stderr, "%s\n", error.c_str());
				std::_Exit(3);
			}
		}
		std::_Exit(0);
	})(), ::testing::ExitedWithCode(0), "");
}

UT_END();
