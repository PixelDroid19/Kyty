#include "Emulator/Graphics/ShaderComputeWaveVulkan.h"

#include "spirv/unified1/spirv.hpp"

#include <limits>

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint32_t kGuestWaveSize = 64;
constexpr uint32_t kNativeSubgroupSize = 32;

bool NativeSubgroupStageSupported(VkShaderStageFlagBits stage) noexcept
{
	return stage == VK_SHADER_STAGE_VERTEX_BIT || stage == VK_SHADER_STAGE_FRAGMENT_BIT || stage == VK_SHADER_STAGE_COMPUTE_BIT;
}

bool SubgroupSizeRangeValid(const ShaderComputeWaveVulkanState& state) noexcept
{
	return state.min_subgroup_size != 0 && state.max_subgroup_size != 0 &&
	       (state.min_subgroup_size & (state.min_subgroup_size - 1u)) == 0 &&
	       (state.max_subgroup_size & (state.max_subgroup_size - 1u)) == 0 && state.min_subgroup_size <= state.max_subgroup_size;
}

bool MultiplyU64(uint64_t a, uint64_t b, uint64_t* product) noexcept
{
	if (product == nullptr || (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)) { return false; }
	*product = a * b;
	return true;
}

uint32_t SubgroupCapability(uint32_t capability) noexcept
{
	switch (capability)
	{
		case spv::CapabilityGroupNonUniform: return VK_SUBGROUP_FEATURE_BASIC_BIT;
		case spv::CapabilityGroupNonUniformVote: return VK_SUBGROUP_FEATURE_VOTE_BIT;
		case spv::CapabilityGroupNonUniformArithmetic: return VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
		case spv::CapabilityGroupNonUniformBallot: return VK_SUBGROUP_FEATURE_BALLOT_BIT;
		case spv::CapabilityGroupNonUniformShuffle: return VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
		case spv::CapabilityGroupNonUniformShuffleRelative: return VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
		case spv::CapabilityGroupNonUniformClustered: return VK_SUBGROUP_FEATURE_CLUSTERED_BIT;
		case spv::CapabilityGroupNonUniformQuad: return VK_SUBGROUP_FEATURE_QUAD_BIT;
		default: return 0;
	}
}

uint32_t SubgroupOperation(uint32_t opcode) noexcept
{
	if (opcode == spv::OpGroupNonUniformElect) { return VK_SUBGROUP_FEATURE_BASIC_BIT; }
	if (opcode >= spv::OpGroupNonUniformAll && opcode <= spv::OpGroupNonUniformAllEqual) { return VK_SUBGROUP_FEATURE_VOTE_BIT; }
	if (opcode >= spv::OpGroupNonUniformBroadcast && opcode <= spv::OpGroupNonUniformBallotFindMSB)
	{
		return VK_SUBGROUP_FEATURE_BALLOT_BIT;
	}
	if (opcode == spv::OpGroupNonUniformShuffle || opcode == spv::OpGroupNonUniformShuffleXor)
	{
		return VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
	}
	if (opcode == spv::OpGroupNonUniformShuffleUp || opcode == spv::OpGroupNonUniformShuffleDown)
	{
		return VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
	}
	if (opcode >= spv::OpGroupNonUniformIAdd && opcode <= spv::OpGroupNonUniformLogicalXor)
	{
		return VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
	}
	if (opcode == spv::OpGroupNonUniformQuadBroadcast || opcode == spv::OpGroupNonUniformQuadSwap)
	{
		return VK_SUBGROUP_FEATURE_QUAD_BIT;
	}
	return 0;
}

// Called only after the instruction stream has passed the bounded first walk.
bool ConstantId(const uint32_t* words, size_t count, uint32_t id) noexcept
{
	for (size_t word = 5; word < count; word += words[word] >> 16u)
	{
		const auto op = words[word] & 0xffffu;
		if ((op == spv::OpConstant || op == spv::OpSpecConstant) && (words[word] >> 16u) >= 4u && words[word + 2u] == id)
		{
			return true;
		}
	}
	return false;
}

} // namespace

ShaderSubgroupModuleCheck ShaderCheckSubgroupModule(const uint32_t* words, size_t count, VkShaderStageFlagBits stage,
                                                  uint32_t supported_stages, uint32_t supported_operations,
                                                  const ShaderComputeWaveVulkanState& state, bool maximal_reconvergence) noexcept
{
	ShaderSubgroupModuleCheck result;
	const auto reject = [&result](size_t word, const char* reason)
	{
		result.word = word;
		result.reason = reason;
		return result;
	};
	if (words == nullptr || count < 5u || words[0] != spv::MagicNumber)
	{
		return reject(0, "invalid SPIR-V header");
	}
	for (size_t word = 5; word < count;)
	{
		const uint32_t length = words[word] >> 16u;
		if (length == 0 || length > count - word) { return reject(word, "truncated SPIR-V instruction"); }
		word += length;
	}
	for (size_t word = 5; word < count; word += words[word] >> 16u)
	{
		const uint32_t op = words[word] & 0xffffu;
		const uint32_t length = words[word] >> 16u;
		uint32_t operations = 0;
		if (op == spv::OpCapability)
		{
			if (length != 2u) { return reject(word, "malformed capability"); }
			operations = SubgroupCapability(words[word + 1u]);
			if (words[word + 1u] == spv::CapabilitySubgroupBallotKHR || words[word + 1u] == spv::CapabilitySubgroupVoteKHR ||
			    words[word + 1u] == spv::CapabilityGroupNonUniformPartitionedNV)
			{
				return reject(word, "non-core subgroup capability has no admission contract");
			}
		} else
		{
			operations = SubgroupOperation(op);
			if (operations != 0)
			{
				const uint32_t minimum = op == spv::OpGroupNonUniformElect ? 4u :
				                         ((operations == VK_SUBGROUP_FEATURE_ARITHMETIC_BIT ||
				                           operations == VK_SUBGROUP_FEATURE_SHUFFLE_BIT ||
				                           operations == VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT ||
				                           operations == VK_SUBGROUP_FEATURE_QUAD_BIT || op == spv::OpGroupNonUniformBroadcast ||
				                           op == spv::OpGroupNonUniformBallotBitExtract || op == spv::OpGroupNonUniformBallotBitCount) ? 6u : 5u);
				if (length < minimum) { return reject(word, "malformed subgroup operation"); }
				if (operations == VK_SUBGROUP_FEATURE_ARITHMETIC_BIT && words[word + 4u] == spv::GroupOperationClusteredReduce)
				{
					operations |= VK_SUBGROUP_FEATURE_CLUSTERED_BIT;
				}
				if ((op == spv::OpGroupNonUniformBroadcast || op == spv::OpGroupNonUniformQuadBroadcast) &&
				    !ConstantId(words, count, words[word + 5u]) &&
				    (words[1] < 0x00010500u || !state.subgroup_broadcast_dynamic_id_enabled))
				{
					return reject(word, "dynamic subgroup broadcast requires SPIR-V 1.5 and enabled subgroupBroadcastDynamicId");
				}
			}
		}
		// SPV_KHR_maximal_reconvergence (not named in the older bundled headers).
		if (op == spv::OpExecutionMode)
		{
			if (length < 3u) { return reject(word, "malformed execution mode"); }
			if (words[word + 2u] == 6023u && !maximal_reconvergence)
			{
				return reject(word, "shaderMaximalReconvergence is not enabled");
			}
		}
		if (operations == 0) { continue; }
		operations |= VK_SUBGROUP_FEATURE_BASIC_BIT;
		result.required_operations |= operations;
		if ((supported_stages & stage) == 0) { return reject(word, "subgroup operations unavailable in this shader stage"); }
		if ((supported_operations & operations) != operations) { return reject(word, "missing emitted subgroup operation feature"); }
		if ((operations & VK_SUBGROUP_FEATURE_QUAD_BIT) != 0 && stage != VK_SHADER_STAGE_FRAGMENT_BIT &&
		    stage != VK_SHADER_STAGE_COMPUTE_BIT && !state.quad_operations_in_all_stages)
		{
			return reject(word, "subgroupQuadOperationsInAllStages is unavailable");
		}
	}
	result.supported = true;
	return result;
}

ShaderComputeWaveCapabilities ShaderComputeWaveVulkanBuildCapabilities(const ShaderComputeWaveVulkanState& state) noexcept
{
	ShaderComputeWaveCapabilities capabilities {};
	const bool extension_v2_enabled = state.extension_advertised && state.extension_revision >= 2 && state.extension_enabled;
	capabilities.size_control_enabled = extension_v2_enabled && state.size_control_feature_supported && state.size_control_feature_enabled;
	capabilities.full_subgroups_enabled =
	    extension_v2_enabled && state.full_subgroups_feature_supported && state.full_subgroups_feature_enabled;
	capabilities.compute_required_size_supported = extension_v2_enabled && state.compute_required_size_supported;
	capabilities.compute_ballot_shuffle_supported = state.compute_ballot_shuffle_supported;
	capabilities.min_subgroup_size = state.min_subgroup_size;
	capabilities.max_subgroup_size = state.max_subgroup_size;
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		capabilities.max_local_size[axis] = state.max_local_size[axis];
		capabilities.max_group_count[axis] = state.max_group_count[axis];
	}
	capabilities.max_invocations = state.max_invocations;
	capabilities.max_subgroups = state.max_subgroups;
	capabilities.max_shared_bytes = state.max_shared_bytes;
	return capabilities;
}

bool ShaderComputeWaveVulkanProbeOutputWordCountValid(size_t words) noexcept
{
	return words > 0 && words <= 4096;
}

bool ShaderFragmentRequiredSubgroupSizeSupported(const ShaderComputeWaveVulkanState& state,
                                                 uint32_t required_size) noexcept
{
	return ShaderRequiredSubgroupSizeSupported(state, VK_SHADER_STAGE_FRAGMENT_BIT, required_size);
}

bool ShaderRequiredSubgroupSizeSupported(const ShaderComputeWaveVulkanState& state, VkShaderStageFlagBits stage,
                                         uint32_t size) noexcept
{
	const bool stage_supported = (stage == VK_SHADER_STAGE_VERTEX_BIT && state.vertex_required_size_supported) ||
	                             (stage == VK_SHADER_STAGE_FRAGMENT_BIT && state.fragment_required_size_supported) ||
	                             (stage == VK_SHADER_STAGE_COMPUTE_BIT && state.compute_required_size_supported);
	// Query support and enabled feature values are deliberately separate. This
	// also works for core size control; extension advertisement alone is no proof.
	return size != 0 && (size & (size - 1u)) == 0 && state.size_control_feature_supported &&
	       state.size_control_feature_enabled && stage_supported && SubgroupSizeRangeValid(state) &&
	       state.min_subgroup_size <= size && size <= state.max_subgroup_size;
}

ShaderNativeSubgroupSelection ShaderSelectNativeSubgroup(const ShaderComputeWaveVulkanState& state, VkShaderStageFlagBits stage,
                                                        uint32_t default_size, uint32_t guest_size, bool lane_local,
                                                        bool fragment_neutral32, bool varying_subgroups) noexcept
{
	if ((guest_size != 32u && guest_size != 64u) || (lane_local && fragment_neutral32) || !NativeSubgroupStageSupported(stage) ||
	    !SubgroupSizeRangeValid(state) || default_size == 0 || (default_size & (default_size - 1u)) != 0 ||
	    default_size < state.min_subgroup_size || default_size > state.max_subgroup_size)
	{
		return {};
	}
	const auto legal = [=](uint32_t size)
	{
		return size >= 4u && (size & (size - 1u)) == 0 &&
		       (size == guest_size || (lane_local && size <= guest_size) ||
		        (stage == VK_SHADER_STAGE_FRAGMENT_BIT && fragment_neutral32 && guest_size == 64u && size == 32u));
	};
	if (!varying_subgroups)
	{
		if (legal(default_size)) { return {true, default_size, false}; }
	} else
	{
		bool all_legal = true;
		// At most 32 power-of-two widths. Widen before doubling so even the
		// largest uint32_t endpoint cannot overflow the iteration variable.
		for (uint64_t size = state.min_subgroup_size; size <= state.max_subgroup_size; size *= 2u)
		{
			if (!legal(static_cast<uint32_t>(size)))
			{
				all_legal = false;
				break;
			}
		}
		if (all_legal)
		{
			return {true, state.min_subgroup_size == state.max_subgroup_size ? state.min_subgroup_size : 0u, false};
		}
	}
	// Preserve exact guest width preference, including native64 for a fragment
	// neutral proof, before considering its narrower 32-lane alternative.
	if (ShaderRequiredSubgroupSizeSupported(state, stage, guest_size)) { return {true, guest_size, true}; }
	if (legal(32u) && ShaderRequiredSubgroupSizeSupported(state, stage, 32u)) { return {true, 32u, true}; }
	return {};
}

bool ShaderAttachNativeSubgroup(const ShaderComputeWaveVulkanState& state, const ShaderNativeSubgroupSelection& selection,
                                VkPipelineShaderStageCreateInfo* stage,
                                VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT* required) noexcept
{
	if (!selection.supported || stage == nullptr || required == nullptr || !NativeSubgroupStageSupported(stage->stage) ||
	    (stage->flags & VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT) != 0)
	{
		return false;
	}
	for (const auto* node = static_cast<const VkBaseInStructure*>(stage->pNext); node != nullptr; node = node->pNext)
	{
		if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT) { return false; }
	}
	if (!selection.require_size) { return true; }
	// An unspecified covered range needs no node; zero is never a requested size.
	if (!ShaderRequiredSubgroupSizeSupported(state, stage->stage, selection.size)) { return false; }
	required->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
	required->pNext = const_cast<void*>(stage->pNext);
	required->requiredSubgroupSize = selection.size;
	stage->pNext = required;
	return true;
}

bool ShaderWave32FragmentNativeLaneExchangeSupported(uint32_t subgroup_stages, uint32_t subgroup_operations,
                                                     bool maximal_reconvergence,
                                                     uint32_t device_default_subgroup_size,
                                                     const ShaderComputeWaveVulkanState& state, bool varying_subgroups) noexcept
{
	constexpr uint32_t kLaneExchangeOps = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
	                                      VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
	                                      VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
	const auto lane_map = ShaderSelectNativeSubgroup(state, VK_SHADER_STAGE_FRAGMENT_BIT, device_default_subgroup_size,
	                                                32u, false, false, varying_subgroups);
	return (subgroup_stages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0u &&
	       (subgroup_operations & kLaneExchangeOps) == kLaneExchangeOps && maximal_reconvergence && lane_map.supported;
}

bool ShaderComputeWaveVulkanAttachRequiredSubgroupSize(
	const ShaderComputeWaveLayout& layout, const ShaderComputeWaveCapabilities& capabilities,
	VkPipelineShaderStageCreateInfo* stage,
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT* required) noexcept
{
	if (stage == nullptr || required == nullptr || stage->stage != VK_SHADER_STAGE_COMPUTE_BIT ||
	    layout.strategy != ShaderComputeWaveStrategy::Paired64On32 || layout.guest_wave_size != kGuestWaveSize ||
	    layout.native_subgroup_size != kNativeSubgroupSize || layout.banks != 2 || layout.waves == 0 ||
	    !capabilities.size_control_enabled || !capabilities.full_subgroups_enabled ||
	    !capabilities.compute_required_size_supported || !capabilities.compute_ballot_shuffle_supported ||
	    capabilities.min_subgroup_size > kNativeSubgroupSize || capabilities.max_subgroup_size < kNativeSubgroupSize)
	{
		return false;
	}

	uint64_t guest_invocations = 1;
	for (uint32_t axis = 0; axis < 3; ++axis)
	{
		if (layout.guest_local[axis] == 0 ||
		    !MultiplyU64(guest_invocations, layout.guest_local[axis], &guest_invocations))
		{
			return false;
		}
	}
	uint64_t expected_physical_x = 0;
	uint64_t lds_bytes = 0;
	// The trailing guest wave may be partial.
	if (guest_invocations / kGuestWaveSize + (guest_invocations % kGuestWaveSize != 0u ? 1u : 0u) != layout.waves ||
	    !MultiplyU64(layout.waves, kNativeSubgroupSize, &expected_physical_x) ||
	    !MultiplyU64(layout.lds_dwords, sizeof(uint32_t), &lds_bytes))
	{
		return false;
	}

	if (expected_physical_x != layout.physical_local[0] || layout.physical_local[0] == 0 ||
	    (layout.physical_local[0] % kNativeSubgroupSize) != 0 || layout.physical_local[1] != 1 ||
	    layout.physical_local[2] != 1 || layout.physical_local[0] > capabilities.max_local_size[0] ||
	    layout.physical_local[1] > capabilities.max_local_size[1] ||
	    layout.physical_local[2] > capabilities.max_local_size[2] ||
	    expected_physical_x > capabilities.max_invocations || layout.waves > capabilities.max_subgroups ||
	    lds_bytes > capabilities.max_shared_bytes)
	{
		return false;
	}

	for (const auto* chain = static_cast<const VkBaseInStructure*>(stage->pNext); chain != nullptr; chain = chain->pNext)
	{
		if (chain->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT) { return false; }
	}

	// Attach our node at the head rather than replacing an existing stage chain.
	required->sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
	required->pNext               = const_cast<void*>(stage->pNext);
	required->requiredSubgroupSize = kNativeSubgroupSize;
	stage->pNext                  = required;
	stage->flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
	stage->flags &= ~VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT;
	return true;
}

} // namespace Kyty::Libs::Graphics
