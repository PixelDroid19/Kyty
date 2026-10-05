#include "Emulator/Graphics/Shader.h"

#include "ShaderStorageAnalysis.h"

#include "Kyty/Core/DbgAssert.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

bool ShaderOperandOverlapsSgprRange(const ShaderOperand& operand, int start_register, int registers_num)
{
	if (operand.type != ShaderOperandType::Sgpr || operand.size <= 0)
	{
		return false;
	}
	const int operand_end = operand.register_id + operand.size;
	const int range_end   = start_register + registers_num;
	return operand.register_id < range_end && start_register < operand_end;
}

static uint8_t ShaderOperandSgprRangeMask(const ShaderOperand& operand, int start_register, int registers_num)
{
	if (!ShaderOperandOverlapsSgprRange(operand, start_register, registers_num))
	{
		return 0;
	}

	uint8_t mask = 0;
	for (int index = 0; index < registers_num; ++index)
	{
		const int reg = start_register + index;
		if (operand.register_id <= reg && reg < operand.register_id + operand.size)
		{
			mask |= static_cast<uint8_t>(1u << index);
		}
	}
	return mask;
}

static void ShaderClearSgprRangeLiveness(uint8_t* live_words, const ShaderOperand& destination, int start_register,
                                         int registers_num)
{
	EXIT_IF(live_words == nullptr);
	const uint8_t written_words = ShaderOperandSgprRangeMask(destination, start_register, registers_num);
	*live_words &= static_cast<uint8_t>(~written_words);
}

static bool ShaderInstructionIsConditionalBranch(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchExecnz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz: return true;
		default: return false;
	}
}

bool ShaderInstructionHasStaticBranchTarget(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SBranch || ShaderInstructionIsConditionalBranch(type);
}

static bool ShaderTryGetStaticBranchTarget(const std::unordered_map<uint32_t, uint32_t>& instruction_index,
                                           const ShaderInstruction& inst, uint32_t* target)
{
	EXIT_IF(target == nullptr);
	if (inst.src_num < 1)
	{
		return false;
	}

	const auto target_it = instruction_index.find(ShaderLabel(inst).GetDst());
	if (target_it == instruction_index.end())
	{
		return false;
	}
	*target = target_it->second;
	return true;
}

static bool ShaderTryGetSortedStaticBranchTarget(const Vector<ShaderInstruction>& instructions,
                                                 const ShaderInstruction& inst, uint32_t* target)
{
	EXIT_IF(target == nullptr);
	if (inst.src_num < 1)
	{
		return false;
	}
	const uint32_t pc = ShaderLabel(inst).GetDst();
	const auto found = std::lower_bound(instructions.begin(), instructions.end(), pc,
	                                    [](const ShaderInstruction& instruction, uint32_t value)
	                                    { return instruction.pc < value; });
	if (found == instructions.end() || found->pc != pc)
	{
		return false;
	}
	*target = static_cast<uint32_t>(found - instructions.begin());
	return true;
}

// Returns the metadata-origin SGPR words that can reach each instruction. A
// word stays live only when no predecessor has overwritten it. This lets
// metadata filtering reject stale descriptors while preserving a strict result
// at joins, indirect jumps, malformed control flow, and unparsed code paths.
static std::vector<uint8_t> ShaderGetMetadataSgprLiveness(const ShaderCode& code, int start_register, int registers_num)
{
	const auto& instructions      = code.GetInstructions();
	const auto  instruction_count = instructions.Size();
	if (instruction_count == 0)
	{
		return {};
	}

	const auto descriptor_live_mask = static_cast<uint8_t>((1u << registers_num) - 1u);
	// Parsed PCs are normally strictly increasing. Search that existing array
	// directly instead of allocating a hash node per instruction and resource.
	const bool sorted_unique = std::adjacent_find(instructions.begin(), instructions.end(),
	                                              [](const ShaderInstruction& previous, const ShaderInstruction& current)
	                                              { return previous.pc >= current.pc; }) == instructions.end();
	std::unordered_map<uint32_t, uint32_t> instruction_index;
	if (!sorted_unique)
	{
		instruction_index.reserve(instruction_count);
		for (uint32_t index = 0; index < instruction_count; ++index)
		{
			if (!instruction_index.emplace(instructions.At(index).pc, index).second)
			{
				return std::vector<uint8_t>(instruction_count, descriptor_live_mask);
			}
		}
	}

	// An indirect PC can enter any block. Keep the metadata entry strict until
	// that control flow is represented in the shader IR.
	if (code.HasAnyOf({ShaderInstructionType::SSetpcB64}))
	{
		return std::vector<uint8_t>(instruction_count, descriptor_live_mask);
	}

	std::vector<uint8_t> live_words(instruction_count, 0);
	std::vector<uint8_t> reachable(instruction_count, 0);
	std::vector<uint32_t> work_list;
	work_list.reserve(instruction_count);
	live_words[0] = descriptor_live_mask;
	reachable[0]  = 1;
	work_list.push_back(0);

	bool unresolved_control_flow = false;
	auto propagate = [&](uint32_t successor, uint8_t words)
	{
		if (successor >= instruction_count)
		{
			unresolved_control_flow = true;
			return;
		}

		bool changed = false;
		if (reachable[successor] == 0)
		{
			reachable[successor] = 1;
			changed              = true;
		}
		const uint8_t merged_words = static_cast<uint8_t>(live_words[successor] | words);
		if (merged_words != live_words[successor])
		{
			live_words[successor] = merged_words;
			changed                = true;
		}
		if (changed)
		{
			work_list.push_back(successor);
		}
	};

	while (!work_list.empty() && !unresolved_control_flow)
	{
		const uint32_t index = work_list.back();
		work_list.pop_back();

		const auto& inst = instructions.At(index);
		uint8_t     out  = live_words[index];
		ShaderClearSgprRangeLiveness(&out, inst.dst, start_register, registers_num);
		ShaderClearSgprRangeLiveness(&out, inst.dst2, start_register, registers_num);

		if (ShaderInstructionHasStaticBranchTarget(inst.type))
		{
			uint32_t target = 0;
			const bool resolved = sorted_unique ? ShaderTryGetSortedStaticBranchTarget(instructions, inst, &target)
			                                    : ShaderTryGetStaticBranchTarget(instruction_index, inst, &target);
			if (!resolved)
			{
				unresolved_control_flow = true;
				continue;
			}
			propagate(target, out);
			if (ShaderInstructionIsConditionalBranch(inst.type))
			{
				propagate(index + 1, out);
			}
			continue;
		}
		if (inst.type == ShaderInstructionType::SEndpgm)
		{
			continue;
		}
		if (inst.type == ShaderInstructionType::SSetpcB64)
		{
			unresolved_control_flow = true;
			continue;
		}
		propagate(index + 1, out);
	}

	if (unresolved_control_flow)
	{
		return std::vector<uint8_t>(instruction_count, descriptor_live_mask);
	}
	for (uint32_t index = 0; index < instruction_count; ++index)
	{
		// An instruction not reached by the parsed CFG might still be entered by
		// unsupported control flow. Do not relax metadata for that case.
		if (reachable[index] == 0)
		{
			live_words[index] = descriptor_live_mask;
		}
	}
	return live_words;
}

bool ShaderInstructionReadsImageResource(ShaderInstructionType type)
{
	return type == ShaderInstructionType::ImageGetResinfo || type == ShaderInstructionType::ImageGather4 || type == ShaderInstructionType::ImageLoad || type == ShaderInstructionType::ImageSample ||
	       type == ShaderInstructionType::ImageSampleL || type == ShaderInstructionType::ImageSampleLz ||
	       type == ShaderInstructionType::ImageSampleLzO || type == ShaderInstructionType::ImageSampleB ||
	       type == ShaderInstructionType::ImageSampleDrefLz;
}

bool ShaderInstructionWritesImageResource(ShaderInstructionType type)
{
	return type == ShaderInstructionType::ImageStore || type == ShaderInstructionType::ImageStoreMip ||
	       type == ShaderInstructionType::ImageAtomicAdd;
}

bool ShaderInstructionUsesImageSampler(ShaderInstructionType type)
{
	return type == ShaderInstructionType::ImageGather4 || type == ShaderInstructionType::ImageSample ||
	       type == ShaderInstructionType::ImageSampleL || type == ShaderInstructionType::ImageSampleLz ||
	       type == ShaderInstructionType::ImageSampleLzO || type == ShaderInstructionType::ImageSampleB ||
	       type == ShaderInstructionType::ImageSampleDrefLz;
}

State::ImageSampleOperation ShaderInstructionSamplerOperation(ShaderInstructionType type)
{
	EXIT_IF(!ShaderInstructionUsesImageSampler(type));
	return type == ShaderInstructionType::ImageSampleDrefLz ? State::ImageSampleOperation::DepthReference
	                                                       : State::ImageSampleOperation::Regular;
}

ShaderSamplerOperationEvidence AnalyzeShaderSamplerOperationEvidence(const ShaderCode& code, int start_register)
{
	constexpr int descriptor_registers = 4;
	const auto    live_descriptor_words = ShaderGetMetadataSgprLiveness(code, start_register, descriptor_registers);

	ShaderSamplerOperationEvidence evidence {};
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		if (!ShaderInstructionUsesImageSampler(inst.type) || inst.src_num < 3 || inst.src[2].type != ShaderOperandType::Sgpr ||
		    inst.src[2].register_id != start_register || inst.src[2].size != descriptor_registers)
		{
			continue;
		}
		if (!live_descriptor_words.empty() && live_descriptor_words[index] != 0x0fu)
		{
			continue;
		}

		const auto operation = ShaderInstructionSamplerOperation(inst.type);
		if (!evidence.found)
		{
			evidence.operation = operation;
			evidence.found     = true;
		} else if (evidence.operation != operation)
		{
			evidence.operation = State::ImageSampleOperation::Mixed;
		}
	}
	return evidence;
}

State::ImageSampleOperation AnalyzeShaderSamplerOperation(const ShaderCode& code, int start_register)
{
	return AnalyzeShaderSamplerOperationEvidence(code, start_register).operation;
}

namespace {

constexpr int      k_meta_fill_descriptor_words = 4;
constexpr uint32_t k_meta_fill_workgroup_shift  = 6;
constexpr uint32_t k_meta_fill_semantic_instruction_count = 9;

bool MetaFillOperandIsPlain(const ShaderOperand& operand)
{
	return operand.swizzle == 6 && !operand.dpp && !operand.absolute && !operand.negate && !operand.clamp &&
	       operand.multiplier == 1.0f;
}

bool MetaFillOperandIsVgpr(const ShaderOperand& operand, int register_id)
{
	return operand.type == ShaderOperandType::Vgpr && operand.register_id == register_id && operand.size == 1 &&
	       MetaFillOperandIsPlain(operand);
}

bool MetaFillOperandIsSgpr(const ShaderOperand& operand, int register_id, int registers_num)
{
	return operand.type == ShaderOperandType::Sgpr && operand.register_id == register_id && operand.size == registers_num &&
	       MetaFillOperandIsPlain(operand);
}

bool MetaFillOperandIsVccLo(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::VccLo && operand.size >= 1 && MetaFillOperandIsPlain(operand);
}

bool MetaFillOperandIsImmediate(const ShaderOperand& operand, uint32_t value)
{
	return (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant) &&
	       operand.size == 0 && operand.constant.u == value && MetaFillOperandIsPlain(operand);
}

bool MetaFillOperandIsZeroSoffset(const ShaderOperand& operand)
{
	return (operand.type == ShaderOperandType::Null && MetaFillOperandIsPlain(operand)) ||
	       MetaFillOperandIsImmediate(operand, 0);
}

bool MetaFillScalarOffsetIs(const ShaderInstruction& inst, uint32_t expected_offset)
{
	if (inst.src_num < 2 ||
	    (inst.src[1].type != ShaderOperandType::IntegerInlineConstant && inst.src[1].type != ShaderOperandType::LiteralConstant) ||
	    inst.src[1].size != 0 || !MetaFillOperandIsPlain(inst.src[1]))
	{
		return false;
	}

	const int64_t offset = static_cast<int64_t>(inst.src[1].constant.i) + static_cast<int64_t>(inst.smem_imm_offset);
	return offset == static_cast<int64_t>(expected_offset);
}

bool MetaFillDescriptorRangesOverlap(int lhs_start, int rhs_start)
{
	const int64_t lhs_end = static_cast<int64_t>(lhs_start) + k_meta_fill_descriptor_words;
	const int64_t rhs_end = static_cast<int64_t>(rhs_start) + k_meta_fill_descriptor_words;
	return static_cast<int64_t>(lhs_start) < rhs_end && static_cast<int64_t>(rhs_start) < lhs_end;
}

bool MetaFillDescriptorStartsAreDistinct(int source_start_register, int destination_start_register, int parameter_start_register)
{
	return source_start_register >= 0 && destination_start_register >= 0 && parameter_start_register >= 0 &&
	       !MetaFillDescriptorRangesOverlap(source_start_register, destination_start_register) &&
	       !MetaFillDescriptorRangesOverlap(source_start_register, parameter_start_register) &&
	       !MetaFillDescriptorRangesOverlap(destination_start_register, parameter_start_register);
}

bool MetaFillOperandOverlapsTrackedDescriptor(const ShaderOperand& operand, int source_start_register,
	                                          int destination_start_register, int parameter_start_register)
{
	return ShaderOperandOverlapsSgprRange(operand, source_start_register, k_meta_fill_descriptor_words) ||
	       ShaderOperandOverlapsSgprRange(operand, destination_start_register, k_meta_fill_descriptor_words) ||
	       ShaderOperandOverlapsSgprRange(operand, parameter_start_register, k_meta_fill_descriptor_words);
}

bool MetaFillInstructionUsesOnlyExpectedDescriptor(const ShaderInstruction& inst, int expected_source_index,
	                                               int expected_start_register, int source_start_register,
	                                               int destination_start_register, int parameter_start_register)
{
	if (inst.src_num < 0 || inst.src_num > 4 || inst.mimg_address_num < 0 || inst.mimg_address_num > 13)
	{
		return false;
	}

	for (int source_index = 0; source_index < inst.src_num; ++source_index)
	{
		const auto& operand = inst.src[source_index];
		if (!MetaFillOperandOverlapsTrackedDescriptor(operand, source_start_register, destination_start_register,
		                                             parameter_start_register))
		{
			continue;
		}
		if (source_index != expected_source_index ||
		    !MetaFillOperandIsSgpr(operand, expected_start_register, k_meta_fill_descriptor_words))
		{
			return false;
		}
	}

	for (int address_index = 0; address_index < inst.mimg_address_num; ++address_index)
	{
		if (MetaFillOperandOverlapsTrackedDescriptor(inst.mimg_address[address_index], source_start_register,
		                                            destination_start_register, parameter_start_register))
		{
			return false;
		}
	}

	return !MetaFillOperandOverlapsTrackedDescriptor(inst.dst, source_start_register, destination_start_register,
	                                                parameter_start_register) &&
	       !MetaFillOperandOverlapsTrackedDescriptor(inst.dst2, source_start_register, destination_start_register,
	                                                parameter_start_register);
}

bool MetaFillInstructionHasBitwiseXor(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::BufferAtomicXor:
		case ShaderInstructionType::DsXorB32:
		case ShaderInstructionType::SXnorSaveexecB64:
		case ShaderInstructionType::SXorSaveexecB64:
		case ShaderInstructionType::SXnorB32:
		case ShaderInstructionType::SXorB32:
		case ShaderInstructionType::SXnorB64:
		case ShaderInstructionType::SXorB64:
		case ShaderInstructionType::VXnorB32:
		case ShaderInstructionType::VXorB32: return true;
		default: return false;
	}
}

bool MetaFillInstructionIsPadding(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SWaitcnt || type == ShaderInstructionType::SInstPrefetch;
}

bool MetaFillHasWaitBetween(const Vector<ShaderInstruction>& instructions, uint32_t before_index, uint32_t after_index)
{
	if (before_index >= after_index || after_index > instructions.Size())
	{
		return false;
	}
	for (uint32_t index = before_index + 1; index < after_index; ++index)
	{
		if (instructions.At(index).type == ShaderInstructionType::SWaitcnt)
		{
			return true;
		}
	}
	return false;
}

bool MetaFillMatchesLinearIndex(const ShaderInstruction& inst, int* index_register, int* workgroup_register)
{
	if (index_register == nullptr || workgroup_register == nullptr || inst.type != ShaderInstructionType::VLshlAddU32 ||
	    inst.src_num != 3 || inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size != 1 ||
	    inst.dst.register_id != 0 || !MetaFillOperandIsPlain(inst.dst) ||
	    !MetaFillOperandIsSgpr(inst.src[0], inst.src[0].register_id, 1) ||
	    !MetaFillOperandIsImmediate(inst.src[1], k_meta_fill_workgroup_shift) ||
	    !MetaFillOperandIsVgpr(inst.src[2], 0))
	{
		return false;
	}

	*index_register     = inst.dst.register_id;
	*workgroup_register = inst.src[0].register_id;
	return true;
}

bool MetaFillMatchesParameterLoad(const ShaderInstruction& inst, int parameter_start_register, uint32_t offset)
{
	return inst.type == ShaderInstructionType::SBufferLoadDword && inst.format == ShaderInstructionFormat::SdstSvSoffset &&
	       inst.src_num == 2 && MetaFillOperandIsVccLo(inst.dst) &&
	       MetaFillOperandIsSgpr(inst.src[0], parameter_start_register, k_meta_fill_descriptor_words) &&
	       MetaFillScalarOffsetIs(inst, offset);
}

bool MetaFillMatchesBoundsCompare(const ShaderInstruction& inst, int index_register)
{
	return inst.type == ShaderInstructionType::VCmpxGtU32 && inst.src_num == 2 && MetaFillOperandIsVccLo(inst.dst) &&
	       MetaFillOperandIsVccLo(inst.src[0]) && MetaFillOperandIsVgpr(inst.src[1], index_register);
}

bool MetaFillFindBranchEnd(const Vector<ShaderInstruction>& instructions, uint32_t branch_index, uint32_t* end_index)
{
	if (end_index == nullptr || branch_index >= instructions.Size())
	{
		return false;
	}

	const auto& branch = instructions.At(branch_index);
	if (branch.type != ShaderInstructionType::SCbranchExecz || branch.format != ShaderInstructionFormat::Label || branch.src_num != 1 ||
	    branch.src[0].type != ShaderOperandType::LiteralConstant || branch.src[0].size != 0 ||
	    !MetaFillOperandIsPlain(branch.src[0]))
	{
		return false;
	}

	const int64_t target_pc = static_cast<int64_t>(branch.pc) + 4 + static_cast<int64_t>(branch.src[0].constant.i);
	if (target_pc <= static_cast<int64_t>(branch.pc) || target_pc > UINT32_MAX)
	{
		return false;
	}

	uint32_t target_count = 0;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		if (instructions.At(index).pc == static_cast<uint32_t>(target_pc))
		{
			*end_index = index;
			target_count++;
		}
	}
	return target_count == 1 && instructions.At(*end_index).type == ShaderInstructionType::SEndpgm;
}

bool MetaFillMatchesMaskAnd(const ShaderInstruction& inst, int index_register, int* value_register)
{
	if (value_register == nullptr || inst.type != ShaderInstructionType::VAndB32 || inst.src_num != 2 ||
	    inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size != 1 || !MetaFillOperandIsPlain(inst.dst) ||
	    !MetaFillOperandIsVccLo(inst.src[0]) || !MetaFillOperandIsVgpr(inst.src[1], index_register))
	{
		return false;
	}

	*value_register = inst.dst.register_id;
	return true;
}

bool MetaFillMatchesSourceLoad(const ShaderInstruction& inst, int source_start_register, int value_register)
{
	return inst.type == ShaderInstructionType::BufferLoadFormatX &&
	       inst.format == ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen && inst.src_num == 3 &&
	       inst.buffer_idxen && !inst.buffer_offen && !inst.buffer_return_old_value && inst.buffer_imm_offset == 0 &&
	       MetaFillOperandIsVgpr(inst.dst, value_register) && MetaFillOperandIsVgpr(inst.src[0], value_register) &&
	       MetaFillOperandIsSgpr(inst.src[1], source_start_register, k_meta_fill_descriptor_words) &&
	       MetaFillOperandIsZeroSoffset(inst.src[2]);
}

bool MetaFillMatchesDestinationStore(const ShaderInstruction& inst, int destination_start_register, int index_register,
	                                 int value_register)
{
	return inst.type == ShaderInstructionType::BufferStoreFormatX &&
	       inst.format == ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen && inst.src_num == 3 &&
	       inst.buffer_idxen && !inst.buffer_offen && !inst.buffer_return_old_value && inst.buffer_imm_offset == 0 &&
	       MetaFillOperandIsVgpr(inst.dst, value_register) && MetaFillOperandIsVgpr(inst.src[0], index_register) &&
	       MetaFillOperandIsSgpr(inst.src[1], destination_start_register, k_meta_fill_descriptor_words) &&
	       MetaFillOperandIsZeroSoffset(inst.src[2]);
}

constexpr int      k_uniform_buffer_fill_descriptor_words  = 4;
constexpr uint32_t k_uniform_buffer_fill_max_shift         = 10u;
constexpr uint32_t k_uniform_buffer_fill_instruction_count = 7u;

bool UniformBufferFillOperandIsPlain(const ShaderOperand& operand)
{
	return operand.swizzle == 6 && !operand.dpp && !operand.absolute && !operand.negate &&
	       !operand.clamp && operand.multiplier == 1.0f;
}

bool UniformBufferFillOperandIsVgpr(const ShaderOperand& operand, int register_id, int registers_num)
{
	return register_id >= 0 && operand.type == ShaderOperandType::Vgpr && operand.register_id == register_id &&
	       operand.size == registers_num && UniformBufferFillOperandIsPlain(operand);
}

bool UniformBufferFillOperandIsSgpr(const ShaderOperand& operand, int register_id, int registers_num)
{
	return register_id >= 0 && operand.type == ShaderOperandType::Sgpr && operand.register_id == register_id &&
	       operand.size == registers_num && UniformBufferFillOperandIsPlain(operand);
}

bool UniformBufferFillOperandIsZeroSoffset(const ShaderOperand& operand)
{
	return ((operand.type == ShaderOperandType::Null && operand.size == 0) ||
	        ((operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant) &&
	         operand.size == 0 && operand.constant.u == 0u)) &&
	       UniformBufferFillOperandIsPlain(operand);
}

bool UniformBufferFillInstructionIsPadding(const ShaderInstruction& inst)
{
	// s_nop is currently represented by SInstPrefetch in the shader IR.
	return (inst.type == ShaderInstructionType::SWaitcnt || inst.type == ShaderInstructionType::SInstPrefetch) &&
	       inst.format == ShaderInstructionFormat::Imm && inst.src_num == 1 &&
	       inst.src[0].type == ShaderOperandType::LiteralConstant && inst.src[0].size == 0 &&
	       UniformBufferFillOperandIsPlain(inst.src[0]);
}

bool UniformBufferFillMatchesLinearIndex(const ShaderInstruction& inst, int* index_register, int* workgroup_register,
	                                     uint32_t* workgroup_shift)
{
	if (index_register == nullptr || workgroup_register == nullptr || workgroup_shift == nullptr ||
	    inst.type != ShaderInstructionType::VLshlAddU32 || inst.format != ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2 ||
	    inst.src_num != 3 || inst.vop3_op_sel != 0 ||
	    !UniformBufferFillOperandIsVgpr(inst.dst, inst.dst.register_id, 1) ||
	    !UniformBufferFillOperandIsSgpr(inst.src[0], inst.src[0].register_id, 1) ||
	    inst.src[1].type != ShaderOperandType::IntegerInlineConstant || inst.src[1].size != 0 ||
	    inst.src[1].constant.u > k_uniform_buffer_fill_max_shift || !UniformBufferFillOperandIsPlain(inst.src[1]) ||
	    !UniformBufferFillOperandIsVgpr(inst.src[2], 0, 1))
	{
		return false;
	}

	*index_register     = inst.dst.register_id;
	*workgroup_register = inst.src[0].register_id;
	*workgroup_shift    = inst.src[1].constant.u;
	return true;
}

bool UniformBufferFillMatchesValueMove(const ShaderInstruction& inst, int destination_register, int* value_register)
{
	if (value_register == nullptr || inst.type != ShaderInstructionType::VMovB32 ||
	    inst.format != ShaderInstructionFormat::SVdstSVsrc0 || inst.src_num != 1 || inst.vop3_op_sel != 0 ||
	    !UniformBufferFillOperandIsVgpr(inst.dst, destination_register, 1) ||
	    !UniformBufferFillOperandIsSgpr(inst.src[0], inst.src[0].register_id, 1))
	{
		return false;
	}

	*value_register = inst.src[0].register_id;
	return true;
}

bool UniformBufferFillMatchesStore(const ShaderInstruction& inst, int index_register, int value_start_register,
	                              int* destination_start_register)
{
	if (destination_start_register == nullptr || inst.type != ShaderInstructionType::BufferStoreFormatXyzw ||
	    inst.format != ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen || inst.src_num != 3 || inst.vop3_op_sel != 0 ||
	    !inst.buffer_idxen || inst.buffer_offen || inst.buffer_return_old_value || inst.buffer_imm_offset != 0u ||
	    !UniformBufferFillOperandIsVgpr(inst.dst, value_start_register, k_uniform_buffer_fill_descriptor_words) ||
	    !UniformBufferFillOperandIsVgpr(inst.src[0], index_register, 1) ||
	    !UniformBufferFillOperandIsSgpr(inst.src[1], inst.src[1].register_id, k_uniform_buffer_fill_descriptor_words) ||
	    !UniformBufferFillOperandIsZeroSoffset(inst.src[2]))
	{
		return false;
	}

	*destination_start_register = inst.src[1].register_id;
	return true;
}

} // namespace

ShaderComputeUniformBufferFillEvidence AnalyzeShaderComputeUniformBufferFill(const ShaderCode& code)
{
	ShaderComputeUniformBufferFillEvidence result {};
	const auto&                            instructions = code.GetInstructions();
	if (code.GetType() != ShaderType::Compute || instructions.Size() == 0 || code.GetLabels().Size() != 0 ||
	    code.GetIndirectLabels().Size() != 0)
	{
		return result;
	}

	uint32_t semantic_indices[k_uniform_buffer_fill_instruction_count] = {};
	uint32_t semantic_count                                               = 0;
	for (uint32_t instruction_index = 0; instruction_index < instructions.Size(); ++instruction_index)
	{
		if (UniformBufferFillInstructionIsPadding(instructions.At(instruction_index)))
		{
			continue;
		}
		if (semantic_count == k_uniform_buffer_fill_instruction_count)
		{
			return result;
		}
		semantic_indices[semantic_count++] = instruction_index;
	}
	if (semantic_count != k_uniform_buffer_fill_instruction_count)
	{
		return result;
	}

	// The four scalar copies are independent of the invocation index. Both
	// consecutive group orderings are accepted, but interleaving remains outside
	// this narrow proof.
	uint32_t index_semantic_index = 0;
	uint32_t moves_semantic_index = 0;
	if (instructions.At(semantic_indices[0]).type == ShaderInstructionType::VLshlAddU32)
	{
		index_semantic_index = 0;
		moves_semantic_index = 1;
	} else if (instructions.At(semantic_indices[4]).type == ShaderInstructionType::VLshlAddU32)
	{
		index_semantic_index = 4;
		moves_semantic_index = 0;
	} else
	{
		return result;
	}

	int      index_register     = -1;
	int      workgroup_register = -1;
	uint32_t workgroup_shift    = 0;
	if (!UniformBufferFillMatchesLinearIndex(instructions.At(semantic_indices[index_semantic_index]), &index_register,
	                                         &workgroup_register, &workgroup_shift))
	{
		return result;
	}

	const int value_start_register = instructions.At(semantic_indices[moves_semantic_index]).dst.register_id;
	const int64_t value_end_register = static_cast<int64_t>(value_start_register) +
	                                   static_cast<int64_t>(k_uniform_buffer_fill_descriptor_words);
	if (static_cast<int64_t>(index_register) >= static_cast<int64_t>(value_start_register) &&
	    static_cast<int64_t>(index_register) < value_end_register)
	{
		return result;
	}
	// A move-first sequence must leave the hardware invocation-X VGPR intact
	// until the linear-index instruction consumes v0. Index-first sequences
	// have already consumed v0 and can reuse it as an output component.
	if (moves_semantic_index < index_semantic_index && static_cast<int64_t>(value_start_register) <= 0 &&
	    0 < value_end_register)
	{
		return result;
	}

	int value_registers[k_uniform_buffer_fill_descriptor_words] = {};
	for (int component = 0; component < k_uniform_buffer_fill_descriptor_words; ++component)
	{
		const int64_t destination_register = static_cast<int64_t>(value_start_register) + component;
		if (destination_register > INT32_MAX ||
		    !UniformBufferFillMatchesValueMove(instructions.At(semantic_indices[moves_semantic_index + static_cast<uint32_t>(component)]),
		                                       static_cast<int>(destination_register), &value_registers[component]) ||
		    value_registers[component] == workgroup_register)
		{
			return result;
		}
	}

	int destination_start_register = -1;
	if (!UniformBufferFillMatchesStore(instructions.At(semantic_indices[5]), index_register, value_start_register,
	                                   &destination_start_register))
	{
		return result;
	}

	const auto& end = instructions.At(semantic_indices[6]);
	if (end.type != ShaderInstructionType::SEndpgm || end.format != ShaderInstructionFormat::Empty || end.src_num != 0 ||
	    semantic_indices[6] + 1u != instructions.Size())
	{
		return result;
	}

	result.destination_start_register = destination_start_register;
	result.workgroup_register         = workgroup_register;
	result.workgroup_shift            = workgroup_shift;
	for (int component = 0; component < k_uniform_buffer_fill_descriptor_words; ++component)
	{
		result.value_registers[component] = value_registers[component];
	}
	result.valid = true;
	return result;
}

ShaderComputeMetaFillEvidence AnalyzeShaderComputeMetaFill(const ShaderCode& code, int source_start_register,
	                                                        int destination_start_register, int parameter_start_register)
{
	ShaderComputeMetaFillEvidence result {};
	if (!MetaFillDescriptorStartsAreDistinct(source_start_register, destination_start_register, parameter_start_register))
	{
		return result;
	}

	result.source_start_register      = source_start_register;
	result.destination_start_register = destination_start_register;
	result.parameter_start_register   = parameter_start_register;
	result.compute_shader             = code.GetType() == ShaderType::Compute;
	result.no_unknown_instructions    = true;
	result.no_bitwise_xor             = true;
	result.descriptor_uses_valid      = true;

	const auto& instructions = code.GetInstructions();
	for (const auto& inst: instructions)
	{
		result.no_unknown_instructions = result.no_unknown_instructions && inst.type != ShaderInstructionType::Unknown;
		result.no_bitwise_xor          = result.no_bitwise_xor && !MetaFillInstructionHasBitwiseXor(inst.type);
	}
	if (!result.compute_shader || !result.no_unknown_instructions || !result.no_bitwise_xor || instructions.Size() == 0)
	{
		return result;
	}

	uint32_t semantic_indices[k_meta_fill_semantic_instruction_count] = {};
	uint32_t semantic_count                                            = 0;
	for (uint32_t instruction_index = 0; instruction_index < instructions.Size(); ++instruction_index)
	{
		const auto& inst = instructions.At(instruction_index);
		if (MetaFillInstructionIsPadding(inst.type))
		{
			continue;
		}
		if (semantic_count == k_meta_fill_semantic_instruction_count)
		{
			return result;
		}
		semantic_indices[semantic_count++] = instruction_index;
	}
	if (semantic_count != k_meta_fill_semantic_instruction_count)
	{
		return result;
	}

	for (uint32_t semantic_index = 0; semantic_index < semantic_count; ++semantic_index)
	{
		int expected_source_index  = -1;
		int expected_start_register = -1;
		switch (semantic_index)
		{
			case 1:
			case 4:
				expected_source_index  = 0;
				expected_start_register = parameter_start_register;
				break;
			case 6:
				expected_source_index  = 1;
				expected_start_register = source_start_register;
				break;
			case 7:
				expected_source_index  = 1;
				expected_start_register = destination_start_register;
				break;
			default: break;
		}
		if (!MetaFillInstructionUsesOnlyExpectedDescriptor(instructions.At(semantic_indices[semantic_index]), expected_source_index,
		                                                expected_start_register, source_start_register, destination_start_register,
		                                                parameter_start_register))
		{
			result.descriptor_uses_valid = false;
			return result;
		}
	}

	int index_register     = -1;
	int value_register     = -1;
	int workgroup_register = -1;
	if (!MetaFillMatchesLinearIndex(instructions.At(semantic_indices[0]), &index_register, &workgroup_register))
	{
		return result;
	}
	result.workgroup_register = workgroup_register;
	result.workgroup_shift    = k_meta_fill_workgroup_shift;
	result.linear_index_valid = true;

	if (!MetaFillMatchesParameterLoad(instructions.At(semantic_indices[1]), parameter_start_register, 0) ||
	    !MetaFillHasWaitBetween(instructions, semantic_indices[1], semantic_indices[2]) ||
	    !MetaFillMatchesBoundsCompare(instructions.At(semantic_indices[2]), index_register))
	{
		return result;
	}

	uint32_t branch_end_index = 0;
	if (!MetaFillFindBranchEnd(instructions, semantic_indices[3], &branch_end_index) || branch_end_index != semantic_indices[8])
	{
		return result;
	}
	result.bounds_guard_valid = true;

	if (!MetaFillMatchesParameterLoad(instructions.At(semantic_indices[4]), parameter_start_register,
	                                 static_cast<uint32_t>(sizeof(uint32_t))) ||
	    !MetaFillHasWaitBetween(instructions, semantic_indices[4], semantic_indices[5]) ||
	    !MetaFillMatchesMaskAnd(instructions.At(semantic_indices[5]), index_register, &value_register))
	{
		return result;
	}
	result.parameter_loads_valid = true;
	result.source_mask_valid     = true;

	if (!MetaFillMatchesSourceLoad(instructions.At(semantic_indices[6]), source_start_register, value_register) ||
	    !MetaFillHasWaitBetween(instructions, semantic_indices[6], semantic_indices[7]))
	{
		return result;
	}
	result.source_load_valid = true;

	if (!MetaFillMatchesDestinationStore(instructions.At(semantic_indices[7]), destination_start_register, index_register,
	                                     value_register))
	{
		return result;
	}
	result.destination_store_valid = true;

	const auto& end = instructions.At(semantic_indices[8]);
	if (end.type != ShaderInstructionType::SEndpgm || end.format != ShaderInstructionFormat::Empty || end.src_num != 0 ||
	    semantic_indices[8] + 1 != instructions.Size())
	{
		return result;
	}
	result.control_flow_valid = true;

	result.valid = result.compute_shader && result.linear_index_valid &&
	               result.parameter_loads_valid && result.bounds_guard_valid && result.source_mask_valid &&
	               result.source_load_valid && result.destination_store_valid && result.descriptor_uses_valid &&
	               result.no_unknown_instructions && result.no_bitwise_xor && result.control_flow_valid;
	return result;
}

void ShaderAccumulateScalarBufferLoadSpan(const ShaderInstruction& inst, uint64_t* required_bytes, bool* dynamic_offset)
{
	EXIT_IF(required_bytes == nullptr || dynamic_offset == nullptr);
	if (!ShaderInstructionIsScalarBufferLoad(inst) || inst.dst.size <= 0 || inst.src_num < 2)
	{
		*dynamic_offset = true;
		return;
	}
	const bool constant_offset =
	    inst.src[1].type == ShaderOperandType::LiteralConstant || inst.src[1].type == ShaderOperandType::IntegerInlineConstant;
	if (!constant_offset)
	{
		*dynamic_offset = true;
		return;
	}
	const int64_t  byte_offset = static_cast<int64_t>(inst.src[1].constant.i) + inst.smem_imm_offset;
	const uint64_t byte_count  = static_cast<uint64_t>(inst.dst.size) * sizeof(uint32_t);
	if (byte_offset < 0 || static_cast<uint64_t>(byte_offset) > UINT64_MAX - byte_count)
	{
		*dynamic_offset = true;
		return;
	}
	*required_bytes = std::max(*required_bytes, static_cast<uint64_t>(byte_offset) + byte_count);
}

ShaderStorageUseEvidence AnalyzeShaderStorageUse(const ShaderCode& code, int start_register)
{
	constexpr int     descriptor_registers = 4;
	constexpr uint8_t descriptor_live_mask = (1u << descriptor_registers) - 1u;
	const auto         live_descriptor_words = ShaderGetMetadataSgprLiveness(code, start_register, descriptor_registers);

	bool     raw                     = false;
	bool     typed                   = false;
	bool     decoded_unknown         = false;
	bool     indirect_descriptor_use = false;
	bool     guarded_raw_vmem        = false;
	bool     raw_smem_use            = false;
	bool     raw_tbuffer_use         = false;
	uint64_t raw_smem_required_bytes = 0;
	bool     raw_smem_dynamic_offset = false;

	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto&   inst       = code.GetInstructions().At(index);
		const uint8_t live_words = live_descriptor_words[index];

		decoded_unknown            = decoded_unknown || inst.type == ShaderInstructionType::Unknown;
		auto reads_live_descriptor = [&](const ShaderOperand& operand)
		{ return (ShaderOperandSgprRangeMask(operand, start_register, descriptor_registers) & live_words) != 0; };

		bool candidate_raw         = false;
		bool candidate_typed       = false;
		bool candidate_raw_vmem    = false;
		bool candidate_raw_smem    = false;
		bool candidate_raw_tbuffer = false;
		switch (inst.type)
		{
			case ShaderInstructionType::BufferLoadUbyte:
			case ShaderInstructionType::BufferLoadDword:
			case ShaderInstructionType::BufferLoadDwordx2:
			case ShaderInstructionType::BufferLoadDwordx3:
			case ShaderInstructionType::BufferLoadDwordx4:
			case ShaderInstructionType::BufferStoreDword:
			case ShaderInstructionType::BufferStoreDwordx2:
			case ShaderInstructionType::BufferStoreDwordx3:
			case ShaderInstructionType::BufferStoreDwordx4:
			case ShaderInstructionType::BufferAtomicAdd:
			case ShaderInstructionType::BufferAtomicUmax:
				candidate_raw      = true;
				candidate_raw_vmem = true;
				break;
			case ShaderInstructionType::SBufferLoadDword:
			case ShaderInstructionType::SBufferLoadDwordx2:
			case ShaderInstructionType::SBufferLoadDwordx4:
			case ShaderInstructionType::SBufferLoadDwordx8:
			case ShaderInstructionType::SBufferLoadDwordx16:
				candidate_raw      = true;
				candidate_raw_smem = true;
				break;
			// MTBUF/TBUFFER encodes its typed data/number format in the
			// instruction. Its resource descriptor contributes address, stride and
			// bounds only, so descriptor validation follows the raw-buffer contract.
			case ShaderInstructionType::TBufferLoadFormatX:
			case ShaderInstructionType::TBufferLoadFormatXy:
			case ShaderInstructionType::TBufferLoadFormatXyzw:
				candidate_raw         = true;
				candidate_raw_tbuffer = true;
				break;
			case ShaderInstructionType::BufferLoadFormatX:
			case ShaderInstructionType::BufferLoadFormatXy:
			case ShaderInstructionType::BufferLoadFormatXyz:
			case ShaderInstructionType::BufferLoadFormatXyzw:
			case ShaderInstructionType::BufferStoreFormatX:
			case ShaderInstructionType::BufferStoreFormatXy:
			case ShaderInstructionType::BufferStoreFormatXyzw: candidate_typed = true; break;
			default: break;
		}
		if (!candidate_raw && !candidate_typed)
		{
			const bool exact_image_resource =
			    (ShaderInstructionReadsImageResource(inst.type) || ShaderInstructionWritesImageResource(inst.type)) && inst.src_num >= 2 &&
			    inst.src[1].type == ShaderOperandType::Sgpr && inst.src[1].register_id == start_register && inst.src[1].size == 8;
			for (int operand = 0; operand < inst.src_num; ++operand)
			{
				if (exact_image_resource && operand == 1)
				{
					continue;
				}
				indirect_descriptor_use = indirect_descriptor_use || reads_live_descriptor(inst.src[operand]);
			}
			for (int operand = 0; operand < inst.mimg_address_num; ++operand)
			{
				indirect_descriptor_use = indirect_descriptor_use || reads_live_descriptor(inst.mimg_address[operand]);
			}
		} else
		{
			bool matches = false;
			for (int operand = 0; operand < inst.src_num; ++operand)
			{
				const auto& src = inst.src[operand];
				if (src.type == ShaderOperandType::Sgpr && src.register_id == start_register && src.size == descriptor_registers &&
				    live_words == descriptor_live_mask)
				{
					matches = true;
				} else
				{
					indirect_descriptor_use = indirect_descriptor_use || reads_live_descriptor(src);
				}
			}
			if (matches)
			{
				raw              = raw || candidate_raw;
				typed            = typed || candidate_typed;
				guarded_raw_vmem = guarded_raw_vmem || candidate_raw_vmem;
				raw_smem_use     = raw_smem_use || candidate_raw_smem;
				raw_tbuffer_use  = raw_tbuffer_use || candidate_raw_tbuffer;
				if (candidate_raw_smem)
				{
					const bool constant_offset = inst.src_num >= 2 && (inst.src[1].type == ShaderOperandType::LiteralConstant ||
					                                                   inst.src[1].type == ShaderOperandType::IntegerInlineConstant);
					if (!constant_offset || inst.dst.size <= 0)
					{
						raw_smem_dynamic_offset = true;
					} else
					{
						const int64_t  byte_offset = static_cast<int64_t>(inst.src[1].constant.i) + inst.smem_imm_offset;
						const uint64_t byte_count  = static_cast<uint64_t>(inst.dst.size) * sizeof(uint32_t);
						if (byte_offset < 0 || static_cast<uint64_t>(byte_offset) > UINT64_MAX - byte_count)
						{
							raw_smem_dynamic_offset = true;
						} else
						{
							raw_smem_required_bytes = std::max(raw_smem_required_bytes, static_cast<uint64_t>(byte_offset) + byte_count);
						}
					}
				}
			}
		}
	}

	if (raw && typed)
	{
		return {ShaderStorageAccess::Mixed, decoded_unknown,        indirect_descriptor_use,
		        guarded_raw_vmem,           raw_smem_use,           raw_tbuffer_use,
		        raw_smem_required_bytes,    raw_smem_dynamic_offset};
	}
	if (typed)
	{
		return {ShaderStorageAccess::Typed, decoded_unknown,        indirect_descriptor_use,
		        guarded_raw_vmem,           raw_smem_use,           raw_tbuffer_use,
		        raw_smem_required_bytes,    raw_smem_dynamic_offset};
	}
	return {raw ? ShaderStorageAccess::Raw : ShaderStorageAccess::Unknown,
	        decoded_unknown,
	        indirect_descriptor_use,
	        guarded_raw_vmem,
	        raw_smem_use,
	        raw_tbuffer_use,
	        raw_smem_required_bytes,
	        raw_smem_dynamic_offset};
}

bool ShaderGen5SampledTextureShapeForMimgDimension(uint8_t dimension, ShaderGen5SampledTextureShape* shape)
{
	if (shape == nullptr)
	{
		return false;
	}

	switch (dimension)
	{
		case 0u:
		case 1u:
		case 6u: *shape = ShaderGen5SampledTextureShape::TwoDimensional; return true;
		case 2u: *shape = ShaderGen5SampledTextureShape::ThreeDimensional; return true;
		case 3u:
		case 4u:
		case 5u:
		case 7u: *shape = ShaderGen5SampledTextureShape::TwoDimensionalArray; return true;
		default: return false;
	}
}

static void RecordMimgSampledShape(const ShaderInstruction& inst, ShaderDirectImageUse* result)
{
	ShaderGen5SampledTextureShape shape {};
	if (result == nullptr || !ShaderGen5SampledTextureShapeForMimgDimension(inst.mimg_dimension, &shape))
	{
		return;
	}
	if (!result->sampled_shape_known)
	{
		result->sampled_shape       = shape;
		result->sampled_shape_known = true;
		return;
	}
	result->sampled_shape_conflict = result->sampled_shape != shape;
}

ShaderDirectImageUse AnalyzeShaderDirectImageUse(const ShaderCode& code, int start_register,
                                                 const std::vector<std::bitset<106>>* entry_values)
{
	ShaderDirectImageUse result;

	uint32_t inst_index = 0;
	for (const auto& inst: code.GetInstructions())
	{
		const uint32_t current = inst_index++;
		if (entry_values != nullptr && current < entry_values->size())
		{
			bool held = true;
			for (int r = start_register; r < start_register + 8; r++)
			{
				held = held && r >= 0 && r < 106 && (*entry_values)[current].test(static_cast<size_t>(r));
			}
			if (!held)
			{
				continue;
			}
		}
		const bool read  = ShaderInstructionReadsImageResource(inst.type);
		const bool write = ShaderInstructionWritesImageResource(inst.type);
		if ((!read && !write) || inst.src_num < 2 || inst.src[1].type != ShaderOperandType::Sgpr ||
		    inst.src[1].register_id != start_register || inst.src[1].size != 8)
		{
			continue;
		}

		if (write)
		{
			result.texture = ShaderTextureUsage::ReadWrite;
			result.writes  = true;
		} else if (result.texture == ShaderTextureUsage::Unknown)
		{
			result.texture = ShaderTextureUsage::ReadOnly;
		}
		// Atomics need the existing texel even when they do not return its value.
		result.reads = result.reads || read || inst.type == ShaderInstructionType::ImageAtomicAdd;
		if (read)
		{
			RecordMimgSampledShape(inst, &result);
		}

		const bool sampled = ShaderInstructionUsesImageSampler(inst.type);
		if (sampled && inst.src_num >= 3 && inst.src[2].type == ShaderOperandType::Sgpr && inst.src[2].size == 4)
		{
			const auto operation = ShaderInstructionSamplerOperation(inst.type);
			if (result.sampler_register >= 0 && result.sampler_register != inst.src[2].register_id)
			{
				KYTY_LOG_DEBUG("WARNING: direct image resource uses multiple sampler ranges (continuing)\n");
			}
			if (result.sampler_register >= 0 && result.sample_operation != operation)
			{
				result.sample_operation = State::ImageSampleOperation::Mixed;
			} else if (result.sampler_register < 0)
			{
				result.sample_operation = operation;
			}
			result.sampler_register = inst.src[2].register_id;
		}
	}

	return result;
}

namespace {

constexpr uint32_t k_tile_lanes = 64;
constexpr uint32_t k_grid_lanes = 256;
constexpr uint32_t k_max_tile_extent = 256;

struct ShaderTileValue
{
	bool valid = false;
	uint32_t group_x = 0;
	uint32_t group_y = 0;
	std::array<uint32_t, k_grid_lanes> lane {};
};

bool ShaderTilePlainOperand(const ShaderOperand& operand)
{
	return operand.swizzle == 6 && !operand.dpp && !operand.absolute && !operand.negate && !operand.clamp &&
	       operand.multiplier == 1.0f;
}

ShaderTileValue ShaderTileReadOperand(const ShaderOperand& operand, const std::array<ShaderTileValue, 256>& registers,
                                     int workgroup_register)
{
	ShaderTileValue result {};
	if (!ShaderTilePlainOperand(operand))
	{
		return result;
	}
	if (operand.type == ShaderOperandType::Vgpr && operand.register_id >= 0 && operand.register_id < 256 &&
	    operand.size == 1)
	{
		return registers[operand.register_id];
	}
	if (operand.type == ShaderOperandType::Sgpr && operand.size == 1 &&
	    (operand.register_id == workgroup_register || operand.register_id == workgroup_register + 1))
	{
		result.valid   = true;
		result.group_x = operand.register_id == workgroup_register ? 1u : 0u;
		result.group_y = operand.register_id == workgroup_register + 1 ? 1u : 0u;
		return result;
	}
	if (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant)
	{
		result.valid = true;
		result.lane.fill(operand.constant.u);
	}
	return result;
}

bool ShaderTileLaneOnly(const ShaderTileValue& value)
{
	return value.valid && value.group_x == 0 && value.group_y == 0;
}

ShaderTileValue ShaderTileEvaluateInstruction(const ShaderInstruction& inst, const std::array<ShaderTileValue, 256>& registers,
                                              int workgroup_register, uint32_t lane_count)
{
	ShaderTileValue result {};
	if (inst.src_num < 2 || inst.src_num > 3)
	{
		return result;
	}
	const auto a = ShaderTileReadOperand(inst.src[0], registers, workgroup_register);
	const auto b = ShaderTileReadOperand(inst.src[1], registers, workgroup_register);
	const auto c = inst.src_num == 3 ? ShaderTileReadOperand(inst.src[2], registers, workgroup_register) : ShaderTileValue {};
	if (!a.valid || !b.valid)
	{
		return result;
	}
	switch (inst.type)
	{
		case ShaderInstructionType::VLshlrevB32:
			if (!ShaderTileLaneOnly(a) || !ShaderTileLaneOnly(b))
			{
				return result;
			}
			break;
		case ShaderInstructionType::VLshrrevB32:
			if (!ShaderTileLaneOnly(a))
			{
				return result;
			}
			if (!ShaderTileLaneOnly(b))
			{
				const uint32_t shift = a.lane[0];
				if (shift > 8u || (b.group_x % (1u << shift)) != 0u || (b.group_y % (1u << shift)) != 0u)
				{
					return result;
				}
				for (uint32_t lane = 1; lane < lane_count; ++lane)
				{
					if (a.lane[lane] != shift)
					{
						return result;
					}
				}
				result.group_x = b.group_x >> shift;
				result.group_y = b.group_y >> shift;
			}
			break;
		case ShaderInstructionType::VBfeU32:
		case ShaderInstructionType::VAndB32:
		case ShaderInstructionType::VAndOrB32:
			if (!ShaderTileLaneOnly(a) || !ShaderTileLaneOnly(b) ||
			    (inst.src_num == 3 && !ShaderTileLaneOnly(c)))
			{
				return result;
			}
			break;
		case ShaderInstructionType::VLshlAddU32:
			if (!c.valid || !ShaderTileLaneOnly(b) || !ShaderTileLaneOnly(c) ||
			    (a.group_x + a.group_y != 1) || b.lane[0] > 8)
			{
				return result;
			}
			for (uint32_t lane = 0; lane < lane_count; ++lane)
			{
				if (a.lane[lane] != 0 || b.lane[lane] != b.lane[0])
				{
					return result;
				}
			}
			result.group_x = a.group_x << b.lane[0];
			result.group_y = a.group_y << b.lane[0];
			break;
		case ShaderInstructionType::VAddI32:
			if (a.group_x + b.group_x > k_max_tile_extent || a.group_y + b.group_y > k_max_tile_extent)
			{
				return result;
			}
			result.group_x = a.group_x + b.group_x;
			result.group_y = a.group_y + b.group_y;
			break;
		case ShaderInstructionType::VAdd3U32:
			if (!c.valid || a.group_x + b.group_x + c.group_x > k_max_tile_extent ||
			    a.group_y + b.group_y + c.group_y > k_max_tile_extent)
			{
				return result;
			}
			result.group_x = a.group_x + b.group_x + c.group_x;
			result.group_y = a.group_y + b.group_y + c.group_y;
			break;
		default: return result;
	}
	for (uint32_t lane = 0; lane < lane_count; ++lane)
	{
		const uint32_t x = a.lane[lane];
		const uint32_t y = b.lane[lane];
		const uint32_t z = c.lane[lane];
		switch (inst.type)
		{
			case ShaderInstructionType::VLshlrevB32: result.lane[lane] = y << (x & 31u); break;
			case ShaderInstructionType::VLshrrevB32: result.lane[lane] = y >> (x & 31u); break;
			case ShaderInstructionType::VBfeU32:
				if ((z & 31u) == 0u) { return {}; }
				result.lane[lane] = (x >> (y & 31u)) & ((1u << (z & 31u)) - 1u);
				break;
			case ShaderInstructionType::VAndB32: result.lane[lane] = x & y; break;
			case ShaderInstructionType::VAndOrB32: result.lane[lane] = (x & y) | z; break;
			case ShaderInstructionType::VLshlAddU32: result.lane[lane] = z; break;
			case ShaderInstructionType::VAddI32:
				if (static_cast<uint64_t>(x) + y > std::numeric_limits<uint32_t>::max()) { return {}; }
				result.lane[lane] = x + y;
				break;
			case ShaderInstructionType::VAdd3U32:
				if (static_cast<uint64_t>(x) + y + z > std::numeric_limits<uint32_t>::max()) { return {}; }
				result.lane[lane] = x + y + z;
				break;
			default: return {};
		}
	}
	result.valid = true;
	return result;
}

int ShaderTileImageResourceIndex(const ShaderInstruction& inst, const ShaderBindResources& bind)
{
	if (inst.src_num < 2 || inst.src[1].type != ShaderOperandType::Sgpr || inst.src[1].size != 8)
	{
		return -1;
	}
	int result = -1;
	for (uint32_t mapping = 0; mapping < bind.dynamic_sloads.records.Size(); ++mapping)
	{
		const auto& record = bind.dynamic_sloads.records.At(mapping);
		if (record.destination_register != inst.src[1].register_id || inst.pc <= record.instruction_pc ||
		    inst.pc > record.last_consumer_pc)
		{
			continue;
		}
		if (record.kind != ShaderDynamicSLoadResourceKind::Texture || record.dword_count != 8 ||
		    record.resource_field_offset != 0 || result >= 0)
		{
			return -2;
		}
		result = record.resource_index;
	}
	return result;
}

bool ShaderTileStoreCoordinates(const ShaderInstruction& inst, const std::array<ShaderTileValue, 256>& registers,
	                            ShaderTileValue* x, ShaderTileValue* y)
{
	EXIT_IF(x == nullptr || y == nullptr);
	ShaderOperand x_operand {};
	ShaderOperand y_operand {};
	if (inst.mimg_address_num >= 2)
	{
		x_operand = inst.mimg_address[0];
		y_operand = inst.mimg_address[1];
	} else if (inst.mimg_address_num == 0 && inst.src[0].type == ShaderOperandType::Vgpr)
	{
		x_operand = inst.src[0];
		x_operand.size = 1;
		y_operand = x_operand;
		y_operand.register_id++;
	} else
	{
		return false;
	}
	*x = ShaderTileReadOperand(x_operand, registers, -100);
	*y = ShaderTileReadOperand(y_operand, registers, -100);
	return x->valid && y->valid && x->group_x > 0 && x->group_y == 0 && y->group_x == 0 && y->group_y > 0 &&
	       x->group_x <= k_max_tile_extent && y->group_y <= k_max_tile_extent;
}

bool ShaderTileMarkStore(const ShaderTileValue& x, const ShaderTileValue& y, uint32_t lane_count,
	                     ShaderStorageImageTileCoverage* coverage, std::vector<uint8_t>* seen,
	                     const std::array<uint8_t, k_grid_lanes>* active = nullptr)
{
	EXIT_IF(coverage == nullptr || seen == nullptr);
	if (coverage->width == 0)
	{
		coverage->width  = x.group_x;
		coverage->height = y.group_y;
		seen->assign(static_cast<size_t>(coverage->width) * coverage->height, 0);
	}
	if (x.group_x != coverage->width || y.group_y != coverage->height)
	{
		return false;
	}
	for (uint32_t lane = 0; lane < lane_count; ++lane)
	{
		if (active != nullptr && (*active)[lane] == 0)
		{
			continue;
		}
		if (x.lane[lane] >= coverage->width || y.lane[lane] >= coverage->height)
		{
			return false;
		}
		(*seen)[static_cast<size_t>(y.lane[lane]) * coverage->width + x.lane[lane]] = 1;
	}
	return true;
}

bool ShaderTileControlFlowIsLinear(const ShaderInstruction& inst, bool final_instruction)
{
	const auto type = inst.type;
	if (type == ShaderInstructionType::Unknown || type == ShaderInstructionType::SSetpcB64 ||
	    type == ShaderInstructionType::SSwappcB64 || type == ShaderInstructionType::STrap ||
	    ShaderInstructionHasStaticBranchTarget(type) || (type == ShaderInstructionType::SEndpgm && !final_instruction))
	{
		return false;
	}
	if ((type >= ShaderInstructionType::SAndSaveexecB32 && type <= ShaderInstructionType::SOrn1SaveexecB32) ||
	    (type >= ShaderInstructionType::SAndSaveexecB64 && type <= ShaderInstructionType::SXorSaveexecB64) ||
	    (type >= ShaderInstructionType::VCmpLtU64 && type <= ShaderInstructionType::VCmpGeU64) ||
	    (type >= ShaderInstructionType::VCmpEqF32 && type <= ShaderInstructionType::VCmpxUF32))
	{
		return false;
	}
	const auto changes_exec = [](const ShaderOperand& operand)
	{
		return operand.type == ShaderOperandType::ExecLo || operand.type == ShaderOperandType::ExecHi ||
		       operand.type == ShaderOperandType::ExecZ;
	};
	return !changes_exec(inst.dst) && !changes_exec(inst.dst2);
}

bool ShaderBoundedGridPlainRegister(const ShaderOperand& operand, ShaderOperandType type, int reg, int size)
{
	return operand.type == type && operand.register_id == reg && operand.size == size && operand.multiplier == 1.0f &&
	       !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6 && !operand.dpp;
}

bool ShaderBoundedGridConstant(const ShaderOperand& operand, uint32_t value)
{
	return (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant) &&
	       operand.constant.u == value && (operand.size == 0 || operand.size == 1) && operand.multiplier == 1.0f && !operand.absolute &&
	       !operand.negate && !operand.clamp && operand.swizzle == 6 && !operand.dpp;
}

uint8_t ShaderBoundedGridFullStoreMask(uint32_t format)
{
	switch (format)
	{
		case 1u:
		case 5u:
		case 11u:
		case 13u:
		case 20u:
		case 22u: return 0x1u;
		case 14u:
		case 29u:
		case 62u: return 0x3u;
		case 36u: return 0x7u;
		case 50u:
		case 56u:
		case 60u:
		case 71u:
		case 75u:
		case 77u: return 0xfu;
		default: return 0u;
	}
}

bool ShaderBoundedGridPostGuardInstructionAllowed(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SLoadDwordx8:
		case ShaderInstructionType::SWaitcnt:
		case ShaderInstructionType::VCvtF32U32:
		case ShaderInstructionType::VAddF32:
		case ShaderInstructionType::VRcpF32:
		case ShaderInstructionType::VMulF32:
		case ShaderInstructionType::VMacF32:
		case ShaderInstructionType::VRsqF32:
		case ShaderInstructionType::VMadF32:
		case ShaderInstructionType::VSqrtF32:
		case ShaderInstructionType::VMaxF32:
		case ShaderInstructionType::ImageSampleLz:
		case ShaderInstructionType::ImageStore: return true;
		default: return false;
	}
}

// Prove a common whole-grid store with an EXEC guard that excludes only pixels
// beyond a runtime width/height pair. The static result is never sufficient by
// itself: descriptor binding must snapshot those two bounds at dispatch time.
ShaderStorageImageTileCoverage AnalyzeShaderStorageImageBoundedGridCoverage(const ShaderCode& code,
                                                                            const ShaderBindResources& bind,
                                                                            int texture_index, int workgroup_register,
                                                                            const uint32_t threads[3], bool group_xy_enabled)
{
	if (!group_xy_enabled || threads == nullptr || threads[0] < 2u || threads[0] > 16u || threads[1] < 2u ||
	    threads[1] > 16u || threads[2] != 1u || (threads[0] & (threads[0] - 1u)) != 0u ||
	    (threads[1] & (threads[1] - 1u)) != 0u || workgroup_register < 0 || texture_index < 0 ||
	    texture_index >= bind.textures2D.textures_num || !bind.textures2D.desc[texture_index].textures2d_without_sampler ||
	    ShaderResolvedSampledTextureShape(bind.textures2D.desc[texture_index]) != ShaderGen5SampledTextureShape::TwoDimensional)
	{
		return {};
	}
	const uint8_t full_mask = ShaderBoundedGridFullStoreMask(bind.textures2D.desc[texture_index].texture.Format());
	if (full_mask == 0u)
	{
		return {};
	}
	const auto& instructions = code.GetInstructions();
	if (instructions.Size() < 13 || instructions.At(instructions.Size() - 1).type != ShaderInstructionType::SEndpgm)
	{
		return {};
	}
	uint32_t cursor = 0;
	if (instructions.At(cursor).type == ShaderInstructionType::SInstPrefetch)
	{
		++cursor;
	}
	uint32_t shift_x = 0;
	uint32_t shift_y = 0;
	while ((1u << shift_x) < threads[0]) { ++shift_x; }
	while ((1u << shift_y) < threads[1]) { ++shift_y; }
	const auto& coord_x = instructions.At(cursor++);
	const auto& coord_y = instructions.At(cursor++);
	const auto coordinate_is_grid = [&](const ShaderInstruction& inst, int group_reg, uint32_t shift, int lane_reg)
	{
		return inst.type == ShaderInstructionType::VLshlAddU32 && inst.src_num == 3 &&
		       inst.dst.type == ShaderOperandType::Vgpr && inst.dst.register_id >= 2 &&
		       ShaderBoundedGridPlainRegister(inst.dst, ShaderOperandType::Vgpr, inst.dst.register_id, 1) &&
		       ShaderBoundedGridPlainRegister(inst.src[0], ShaderOperandType::Sgpr, group_reg, 1) &&
		       ShaderBoundedGridConstant(inst.src[1], shift) &&
		       ShaderBoundedGridPlainRegister(inst.src[2], ShaderOperandType::Vgpr, lane_reg, 1) &&
		       !inst.vop_sdwa && inst.vop3_op_sel == 0u && inst.vop3_omod == 0u;
	};
	if (!coordinate_is_grid(coord_x, workgroup_register, shift_x, 0) ||
	    !coordinate_is_grid(coord_y, workgroup_register + 1, shift_y, 1) ||
	    coord_x.dst.register_id == coord_y.dst.register_id)
	{
		return {};
	}
	const int x_reg = coord_x.dst.register_id;
	const int y_reg = coord_y.dst.register_id;
	const auto& descriptor_load = instructions.At(cursor++);
	if (descriptor_load.type != ShaderInstructionType::SLoadDwordx4 || descriptor_load.src_num != 2 ||
	    descriptor_load.dst.type != ShaderOperandType::Sgpr || descriptor_load.dst.size != 4 ||
	    descriptor_load.src[0].type != ShaderOperandType::Sgpr || descriptor_load.src[0].size != 2 ||
	    (descriptor_load.src[1].type != ShaderOperandType::IntegerInlineConstant &&
	     descriptor_load.src[1].type != ShaderOperandType::LiteralConstant) ||
	    descriptor_load.smem_imm_offset != 0)
	{
		return {};
	}
	const auto wait_after_descriptor = cursor;
	while (cursor < instructions.Size() && instructions.At(cursor).type == ShaderInstructionType::SWaitcnt) { ++cursor; }
	if (cursor == wait_after_descriptor || cursor >= instructions.Size())
	{
		return {};
	}
	const auto& bounds_load = instructions.At(cursor++);
	if (bounds_load.type != ShaderInstructionType::SBufferLoadDwordx2 || bounds_load.src_num != 2 ||
	    !ShaderBoundedGridPlainRegister(bounds_load.dst, ShaderOperandType::Sgpr, workgroup_register, 2) ||
	    !ShaderBoundedGridPlainRegister(bounds_load.src[0], ShaderOperandType::Sgpr, descriptor_load.dst.register_id, 4) ||
	    !ShaderBoundedGridConstant(bounds_load.src[1], 0u) || bounds_load.smem_imm_offset != 0)
	{
		return {};
	}
	const auto wait_after_bounds = cursor;
	while (cursor < instructions.Size() && instructions.At(cursor).type == ShaderInstructionType::SWaitcnt) { ++cursor; }
	if (cursor == wait_after_bounds || cursor + 4 >= instructions.Size())
	{
		return {};
	}
	int bounds_index = -1;
	for (const auto& record: bind.dynamic_sloads.records)
	{
		if (record.destination_register != descriptor_load.dst.register_id || record.instruction_pc != descriptor_load.pc ||
		    bounds_load.pc > record.last_consumer_pc)
		{
			continue;
		}
		if (bounds_index >= 0 || record.kind != ShaderDynamicSLoadResourceKind::StorageBuffer || record.dword_count != 4 ||
		    record.resource_field_offset != 0 || record.resource_index < 0 ||
		    record.resource_index >= bind.storage_buffers.buffers_num)
		{
			return {};
		}
		bounds_index = record.resource_index;
	}
	if (bounds_index < 0)
	{
		return {};
	}
	const auto& buffers = bind.storage_buffers;
	if (!ShaderStorageUsageIsReadOnly(buffers.usages[bounds_index]) ||
	    buffers.accesses[bounds_index] != ShaderStorageAccess::Raw ||
	    buffers.sources[bounds_index] != ShaderStorageBindingSource::DynamicScalarLoad ||
	    !buffers.code_available[bounds_index] || !buffers.exact_matches[bounds_index] ||
	    buffers.unbased_matches[bounds_index] || buffers.decoded_unknown[bounds_index] ||
	    buffers.indirect_descriptor_use[bounds_index])
	{
		return {};
	}
	const auto& compare_x = instructions.At(cursor++);
	const auto& compare_y = instructions.At(cursor++);
	const auto& invert = instructions.At(cursor++);
	const auto& set_exec = instructions.At(cursor++);
	const auto& branch = instructions.At(cursor++);
	if (compare_x.type != ShaderInstructionType::VCmpLeU32 || compare_x.src_num != 2 ||
	    !ShaderBoundedGridPlainRegister(compare_x.dst, ShaderOperandType::Sgpr, descriptor_load.dst.register_id, 2) ||
	    !ShaderBoundedGridPlainRegister(compare_x.src[0], ShaderOperandType::Sgpr, workgroup_register, 1) ||
	    !ShaderBoundedGridPlainRegister(compare_x.src[1], ShaderOperandType::Vgpr, x_reg, 1) ||
	    compare_y.type != ShaderInstructionType::VCmpLeU32 || compare_y.src_num != 2 ||
	    !ShaderBoundedGridPlainRegister(compare_y.dst, ShaderOperandType::VccLo, 0, 2) ||
	    !ShaderBoundedGridPlainRegister(compare_y.src[0], ShaderOperandType::Sgpr, workgroup_register + 1, 1) ||
	    !ShaderBoundedGridPlainRegister(compare_y.src[1], ShaderOperandType::Vgpr, y_reg, 1) ||
	    invert.type != ShaderInstructionType::SNorB64 || invert.src_num != 2 ||
	    !ShaderBoundedGridPlainRegister(invert.dst, ShaderOperandType::VccLo, 0, 2) ||
	    !ShaderBoundedGridPlainRegister(invert.src[0], ShaderOperandType::Sgpr, descriptor_load.dst.register_id, 2) ||
	    !ShaderBoundedGridPlainRegister(invert.src[1], ShaderOperandType::VccLo, 0, 2) ||
	    set_exec.type != ShaderInstructionType::SMovB64 || set_exec.src_num != 1 ||
	    !ShaderBoundedGridPlainRegister(set_exec.dst, ShaderOperandType::ExecLo, 0, 2) ||
	    !ShaderBoundedGridPlainRegister(set_exec.src[0], ShaderOperandType::VccLo, 0, 2) ||
	    branch.type != ShaderInstructionType::SCbranchExecz || branch.src_num != 1 ||
	    ShaderLabel(branch).GetDst() != instructions.At(instructions.Size() - 1).pc)
	{
		return {};
	}
	bool target_written = false;
	for (; cursor + 1 < instructions.Size(); ++cursor)
	{
		const auto& inst = instructions.At(cursor);
		if (!ShaderBoundedGridPostGuardInstructionAllowed(inst.type) || !ShaderTileControlFlowIsLinear(inst, false))
		{
			return {};
		}
		const auto clobbers_coordinate = [&](const ShaderOperand& dst)
		{
			return dst.type == ShaderOperandType::Vgpr && dst.size > 0 &&
			       ((x_reg >= dst.register_id && x_reg < dst.register_id + dst.size) ||
			        (y_reg >= dst.register_id && y_reg < dst.register_id + dst.size));
		};
		if (clobbers_coordinate(inst.dst) || clobbers_coordinate(inst.dst2))
		{
			return {};
		}
		if (ShaderInstructionReadsImageResource(inst.type) || ShaderInstructionWritesImageResource(inst.type))
		{
			const int image_index = ShaderTileImageResourceIndex(inst, bind);
			if (image_index == -2 || (ShaderInstructionReadsImageResource(inst.type) && image_index == texture_index))
			{
				return {};
			}
			if (inst.type == ShaderInstructionType::ImageStore && image_index == texture_index)
			{
				const bool explicit_xy = inst.mimg_address_num >= 2 &&
				    ShaderBoundedGridPlainRegister(inst.mimg_address[0], ShaderOperandType::Vgpr, x_reg, 1) &&
				    ShaderBoundedGridPlainRegister(inst.mimg_address[1], ShaderOperandType::Vgpr, y_reg, 1);
				const bool sequential_xy = inst.mimg_address_num == 0 && y_reg == x_reg + 1 &&
				    inst.src[0].type == ShaderOperandType::Vgpr && inst.src[0].register_id == x_reg && inst.src[0].size >= 2 &&
				    inst.src[0].multiplier == 1.0f && inst.src[0].swizzle == 6 && !inst.src[0].absolute &&
				    !inst.src[0].negate && !inst.src[0].clamp && !inst.src[0].dpp;
				if (inst.mimg_dimension != 1 || inst.mimg_dmask != full_mask || (!explicit_xy && !sequential_xy))
				{
					return {};
				}
				target_written = true;
			}
		}
	}
	if (!target_written)
	{
		return {};
	}
	return {threads[0], threads[1], bounds_index};
}

void ShaderTileInvalidateVgpr(const ShaderOperand& destination, std::array<ShaderTileValue, 256>* registers);

int ShaderPairedSplitImageIndex(const ShaderInstruction& inst, const ShaderCode& code,
                               const ShaderBindResources& bind)
{
	const int mapped = ShaderTileImageResourceIndex(inst, bind);
	if (mapped != -1)
	{
		return mapped;
	}
	if (inst.src_num < 2 || inst.src[1].type != ShaderOperandType::Sgpr || inst.src[1].size != 8)
	{
		return -2;
	}
	const int reg = inst.src[1].register_id;
	for (const auto& earlier: code.GetInstructions())
	{
		if (earlier.pc >= inst.pc) { break; }
		if (ShaderOperandOverlapsSgprRange(earlier.dst, reg, 8) ||
		    ShaderOperandOverlapsSgprRange(earlier.dst2, reg, 8))
		{
			return -2;
		}
	}
	int direct = -1;
	for (int index = 0; index < bind.textures2D.textures_num; ++index)
	{
		const auto& descriptor = bind.textures2D.desc[index];
		if (descriptor.dynamic_sload || descriptor.start_register != reg) { continue; }
		if (direct >= 0) { return -2; }
		direct = index;
	}
	return direct;
}

bool ShaderPairedSplitPlainCompare(const ShaderInstruction& inst)
{
	const bool type_allowed = inst.type == ShaderInstructionType::VCmpLtF32 ||
	                          inst.type == ShaderInstructionType::VCmpEqF32 ||
	                          inst.type == ShaderInstructionType::VCmpEqU32 ||
	                          inst.type == ShaderInstructionType::VCmpGeU32;
	return type_allowed && inst.dst.type != ShaderOperandType::ExecLo && inst.dst.type != ShaderOperandType::ExecHi &&
	       inst.dst2.type != ShaderOperandType::ExecLo && inst.dst2.type != ShaderOperandType::ExecHi;
}

struct ShaderPairedSplitFlow
{
	int early_branch  = -1;
	int split         = -1;
	int first_branch  = -1;
	int first_store   = -1;
	int invert        = -1;
	int second_branch = -1;
	int second_store  = -1;
};

bool ShaderPairedSplitFindFlow(const ShaderCode& code, const ShaderBindResources& bind, int texture_index,
                              uint8_t full_mask, ShaderPairedSplitFlow* flow)
{
	EXIT_IF(flow == nullptr);
	const auto& instructions = code.GetInstructions();
	if (instructions.Size() < 8 || instructions.At(instructions.Size() - 1).type != ShaderInstructionType::SEndpgm)
	{
		return false;
	}
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (index > 0 && inst.pc <= instructions.At(index - 1).pc) { return false; }
		if (inst.type == ShaderInstructionType::SCbranchVccz)
		{
			if (flow->early_branch >= 0) { return false; }
			flow->early_branch = static_cast<int>(index);
		} else if (inst.type == ShaderInstructionType::VCmpxNeqF32)
		{
			if (flow->split >= 0 || inst.src_num != 2) { return false; }
			flow->split = static_cast<int>(index);
		} else if (inst.type == ShaderInstructionType::SCbranchExecz)
		{
			if (flow->first_branch < 0) { flow->first_branch = static_cast<int>(index); }
			else if (flow->second_branch < 0) { flow->second_branch = static_cast<int>(index); }
			else { return false; }
		} else if (inst.type == ShaderInstructionType::SNotB64 && inst.dst.type == ShaderOperandType::ExecLo)
		{
			if (flow->invert >= 0 || inst.src_num != 1 ||
			    !ShaderBoundedGridPlainRegister(inst.dst, ShaderOperandType::ExecLo, 0, 2) ||
			    !ShaderBoundedGridPlainRegister(inst.src[0], ShaderOperandType::ExecLo, 0, 2))
			{
				return false;
			}
			flow->invert = static_cast<int>(index);
		} else if (inst.type == ShaderInstructionType::SEndpgm)
		{
			if (index + 1 != instructions.Size()) { return false; }
		} else if (!ShaderTileControlFlowIsLinear(inst, false) && !ShaderPairedSplitPlainCompare(inst))
		{
			return false;
		}
		if (ShaderInstructionReadsImageResource(inst.type) || ShaderInstructionWritesImageResource(inst.type))
		{
			const int image_index = ShaderPairedSplitImageIndex(inst, code, bind);
			if (image_index < 0 || image_index >= bind.textures2D.textures_num ||
			    (ShaderInstructionReadsImageResource(inst.type) && image_index == texture_index) ||
			    (ShaderInstructionWritesImageResource(inst.type) &&
			     (image_index != texture_index || inst.type != ShaderInstructionType::ImageStore ||
			      inst.mimg_dimension != 1 || (inst.mimg_dmask & full_mask) != full_mask)))
			{
				return false;
			}
			if (inst.type == ShaderInstructionType::ImageStore)
			{
				if (flow->first_store < 0) { flow->first_store = static_cast<int>(index); }
				else if (flow->second_store < 0) { flow->second_store = static_cast<int>(index); }
				else { return false; }
			}
		}
	}
	const int end = static_cast<int>(instructions.Size()) - 1;
	if (!(0 <= flow->early_branch && flow->early_branch < flow->split &&
	      flow->split < flow->first_branch && flow->first_branch < flow->first_store &&
	      flow->first_store < flow->invert && flow->invert < flow->second_branch &&
	      flow->second_branch < flow->second_store && flow->second_store < end))
	{
		return false;
	}
	const auto& early = instructions.At(flow->early_branch);
	const auto& first = instructions.At(flow->first_branch);
	const auto& second = instructions.At(flow->second_branch);
	return early.src_num == 1 && first.src_num == 1 && second.src_num == 1 &&
	       ShaderLabel(early).GetDst() == instructions.At(flow->split).pc &&
	       ShaderLabel(first).GetDst() == instructions.At(flow->invert).pc &&
	       ShaderLabel(second).GetDst() == instructions.At(end).pc;
}

ShaderStorageImageTileCoverage ShaderPairedSplitCoordinates(const ShaderCode& code, const ShaderPairedSplitFlow& flow,
                                                            int workgroup_register, const uint32_t threads[3])
{
	constexpr uint32_t lane_count = 64;
	std::array<ShaderTileValue, 256> registers {};
	registers[0].valid = true;
	registers[1].valid = true;
	for (uint32_t lane = 0; lane < lane_count; ++lane)
	{
		registers[0].lane[lane] = lane % threads[0];
		registers[1].lane[lane] = lane / threads[0];
	}
	bool group_ids_valid = true;
	const auto& instructions = code.GetInstructions();
	for (int index = 0; index < flow.early_branch; ++index)
	{
		const auto& inst = instructions.At(index);
		if (inst.dst.type == ShaderOperandType::Vgpr && inst.type != ShaderInstructionType::ImageStore)
		{
			const auto value = ShaderTileEvaluateInstruction(inst, registers,
			                                                 group_ids_valid ? workgroup_register : -100, lane_count);
			ShaderTileInvalidateVgpr(inst.dst, &registers);
			if (inst.dst.size == 1 && inst.dst.register_id >= 0 && inst.dst.register_id < 256)
			{
				registers[inst.dst.register_id] = value;
			}
		}
		ShaderTileInvalidateVgpr(inst.dst2, &registers);
		if (ShaderOperandOverlapsSgprRange(inst.dst, workgroup_register, 2) ||
		    ShaderOperandOverlapsSgprRange(inst.dst2, workgroup_register, 2))
		{
			group_ids_valid = false;
		}
	}
	const auto& first = instructions.At(flow.first_store);
	const auto& second = instructions.At(flow.second_store);
	if ((first.mimg_address_num == 0 && first.src[0].size < 2) ||
	    (second.mimg_address_num == 0 && second.src[0].size < 2))
	{
		return {};
	}
	ShaderTileValue first_x {};
	ShaderTileValue first_y {};
	ShaderTileValue second_x {};
	ShaderTileValue second_y {};
	if (!ShaderTileStoreCoordinates(first, registers, &first_x, &first_y) ||
	    !ShaderTileStoreCoordinates(second, registers, &second_x, &second_y) ||
	    first_x.group_x != second_x.group_x || first_y.group_y != second_y.group_y)
	{
		return {};
	}
	for (uint32_t lane = 0; lane < lane_count; ++lane)
	{
		if (first_x.lane[lane] != second_x.lane[lane] || first_y.lane[lane] != second_y.lane[lane]) { return {}; }
	}
	const int x_reg = first.mimg_address_num >= 2 ? first.mimg_address[0].register_id : first.src[0].register_id;
	const int y_reg = first.mimg_address_num >= 2 ? first.mimg_address[1].register_id : x_reg + 1;
	const int second_x_reg = second.mimg_address_num >= 2 ? second.mimg_address[0].register_id : second.src[0].register_id;
	const int second_y_reg = second.mimg_address_num >= 2 ? second.mimg_address[1].register_id : second_x_reg + 1;
	for (int index = flow.early_branch; index < static_cast<int>(instructions.Size()); ++index)
	{
		const auto& inst = instructions.At(index);
		if (inst.type == ShaderInstructionType::ImageStore) { continue; }
		const auto clobbers = [x_reg, y_reg, second_x_reg, second_y_reg](const ShaderOperand& dst)
		{
			return dst.type == ShaderOperandType::Vgpr && dst.size > 0 &&
			       ((x_reg >= dst.register_id && x_reg < dst.register_id + dst.size) ||
			        (y_reg >= dst.register_id && y_reg < dst.register_id + dst.size) ||
			        (second_x_reg >= dst.register_id && second_x_reg < dst.register_id + dst.size) ||
			        (second_y_reg >= dst.register_id && second_y_reg < dst.register_id + dst.size));
		};
		if (clobbers(inst.dst) || clobbers(inst.dst2)) { return {}; }
	}
	ShaderStorageImageTileCoverage coverage {};
	std::vector<uint8_t> seen;
	if (!ShaderTileMarkStore(first_x, first_y, lane_count, &coverage, &seen) ||
	    coverage.width != threads[0] || coverage.height != threads[1] ||
	    !std::all_of(seen.begin(), seen.end(), [](uint8_t value) { return value != 0; }))
	{
		return {};
	}
	return coverage;
}

ShaderStorageImageTileCoverage AnalyzeShaderStorageImagePairedLinearCoverage(const ShaderCode& code,
                                                                             const ShaderBindResources& bind,
                                                                             int texture_index, int workgroup_register,
                                                                             const uint32_t threads[3], uint8_t full_mask)
{
	constexpr uint32_t lane_count = 64;
	const auto& instructions = code.GetInstructions();
	if (instructions.IsEmpty() || instructions.At(instructions.Size() - 1).type != ShaderInstructionType::SEndpgm)
	{
		return {};
	}
	std::array<ShaderTileValue, 256> registers {};
	registers[0].valid = true;
	registers[1].valid = true;
	for (uint32_t lane = 0; lane < lane_count; ++lane)
	{
		registers[0].lane[lane] = lane % threads[0];
		registers[1].lane[lane] = lane / threads[0];
	}
	bool group_ids_valid = true;
	int stores = 0;
	ShaderStorageImageTileCoverage coverage {};
	std::vector<uint8_t> seen;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (index > 0 && inst.pc <= instructions.At(index - 1).pc) { return {}; }
		if (!ShaderTileControlFlowIsLinear(inst, index + 1 == instructions.Size()) &&
		    !ShaderPairedSplitPlainCompare(inst))
		{
			return {};
		}
		if (ShaderInstructionReadsImageResource(inst.type) || ShaderInstructionWritesImageResource(inst.type))
		{
			const int image_index = ShaderPairedSplitImageIndex(inst, code, bind);
			if (image_index < 0 || image_index >= bind.textures2D.textures_num ||
			    (ShaderInstructionReadsImageResource(inst.type) && image_index == texture_index) ||
			    (ShaderInstructionWritesImageResource(inst.type) &&
			     (image_index != texture_index || inst.type != ShaderInstructionType::ImageStore ||
			      inst.mimg_dimension != 1 || (inst.mimg_dmask & full_mask) != full_mask)))
			{
				return {};
			}
			if (inst.type == ShaderInstructionType::ImageStore)
			{
				ShaderTileValue x {};
				ShaderTileValue y {};
				if (++stores != 1 || (inst.mimg_address_num == 0 && inst.src[0].size < 2) ||
				    !ShaderTileStoreCoordinates(inst, registers, &x, &y) ||
				    !ShaderTileMarkStore(x, y, lane_count, &coverage, &seen))
				{
					return {};
				}
				continue;
			}
		}
		if (inst.dst.type == ShaderOperandType::Vgpr)
		{
			const auto value = ShaderTileEvaluateInstruction(inst, registers,
			                                                 group_ids_valid ? workgroup_register : -100, lane_count);
			ShaderTileInvalidateVgpr(inst.dst, &registers);
			if (inst.dst.size == 1 && inst.dst.register_id >= 0 && inst.dst.register_id < 256)
			{
				registers[inst.dst.register_id] = value;
			}
		}
		ShaderTileInvalidateVgpr(inst.dst2, &registers);
		if (ShaderOperandOverlapsSgprRange(inst.dst, workgroup_register, 2) ||
		    ShaderOperandOverlapsSgprRange(inst.dst2, workgroup_register, 2))
		{
			group_ids_valid = false;
		}
	}
	if (stores != 1 || coverage.width != threads[0] || coverage.height != threads[1] ||
	    !std::all_of(seen.begin(), seen.end(), [](uint8_t value) { return value != 0; }))
	{
		return {};
	}
	return coverage;
}

ShaderStorageImageTileCoverage AnalyzeShaderStorageImagePairedCoverage(const ShaderCode& code,
                                                                       const ShaderBindResources& bind,
                                                                       int texture_index, int workgroup_register,
                                                                       const uint32_t threads[3], bool group_xy_enabled)
{
	if (!group_xy_enabled || threads == nullptr || threads[0] == 0 || threads[1] == 0 ||
	    static_cast<uint64_t>(threads[0]) * threads[1] != 64u || threads[2] != 1u ||
	    workgroup_register < 0 || texture_index < 0 || texture_index >= bind.textures2D.textures_num ||
	    !bind.textures2D.desc[texture_index].textures2d_without_sampler ||
	    ShaderResolvedSampledTextureShape(bind.textures2D.desc[texture_index]) != ShaderGen5SampledTextureShape::TwoDimensional)
	{
		return {};
	}
	const uint8_t full_mask = ShaderBoundedGridFullStoreMask(bind.textures2D.desc[texture_index].texture.Format());
	if (full_mask == 0u) { return {}; }
	ShaderPairedSplitFlow flow {};
	if (ShaderPairedSplitFindFlow(code, bind, texture_index, full_mask, &flow))
	{
		return ShaderPairedSplitCoordinates(code, flow, workgroup_register, threads);
	}
	return AnalyzeShaderStorageImagePairedLinearCoverage(code, bind, texture_index, workgroup_register,
	                                                     threads, full_mask);
}

void ShaderTileInvalidateVgpr(const ShaderOperand& destination, std::array<ShaderTileValue, 256>* registers)
{
	EXIT_IF(registers == nullptr);
	if (destination.type != ShaderOperandType::Vgpr)
	{
		return;
	}
	for (int index = 0; index < destination.size; ++index)
	{
		const int reg = destination.register_id + index;
		if (reg >= 0 && reg < 256)
		{
			(*registers)[reg] = {};
		}
	}
}

ShaderStorageImageTileCoverage AnalyzeShaderStorageImageGridCoverage(const ShaderCode& code, const ShaderBindResources& bind,
	                                                                 int texture_index, int workgroup_register,
	                                                                 const uint32_t threads[3])
{
	if (threads[0] != 16u || threads[1] != 16u || threads[2] != 1u ||
	    bind.textures2D.desc[texture_index].texture.Format() != 22u)
	{
		return {};
	}
	const auto& instructions = code.GetInstructions();
	if (instructions.IsEmpty() || instructions.At(instructions.Size() - 1).type != ShaderInstructionType::SEndpgm)
	{
		return {};
	}
	uint32_t reset_pc = UINT32_MAX;
	for (const auto& inst: instructions)
	{
		if (inst.type == ShaderInstructionType::SMovB64 && inst.dst.type == ShaderOperandType::ExecLo &&
		    inst.dst.size == 2 && inst.src_num == 1 &&
		    (inst.src[0].type == ShaderOperandType::IntegerInlineConstant ||
		     inst.src[0].type == ShaderOperandType::LiteralConstant) &&
		    inst.src[0].constant.u == UINT32_MAX)
		{
			if (reset_pc != UINT32_MAX)
			{
				return {};
			}
			reset_pc = inst.pc;
		}
	}
	if (reset_pc == UINT32_MAX)
	{
		return {};
	}

	std::array<ShaderTileValue, 256> registers {};
	registers[0].valid = true;
	registers[1].valid = true;
	for (uint32_t lane = 0; lane < k_grid_lanes; ++lane)
	{
		registers[0].lane[lane] = lane % threads[0];
		registers[1].lane[lane] = lane / threads[0];
	}
	std::array<uint8_t, k_grid_lanes> active {};
	bool restored = false;
	bool predicated_before_reset = false;
	bool skipped_to_reset = false;
	ShaderStorageImageTileCoverage coverage {};
	std::vector<uint8_t> seen;
	const uint32_t end_pc = instructions.At(instructions.Size() - 1).pc;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (inst.pc == reset_pc)
		{
			restored = true;
			active.fill(1);
			predicated_before_reset = false;
			skipped_to_reset = false;
			continue;
		}
		if (inst.type == ShaderInstructionType::VCmpxGtU32 && !restored)
		{
			predicated_before_reset = true;
			continue;
		}
		if (inst.type == ShaderInstructionType::VCmpxEqU32 && restored && inst.src_num == 2)
		{
			const auto a = ShaderTileReadOperand(inst.src[0], registers, workgroup_register);
			const auto b = ShaderTileReadOperand(inst.src[1], registers, workgroup_register);
			if (!ShaderTileLaneOnly(a) || !ShaderTileLaneOnly(b))
			{
				return {};
			}
			for (uint32_t lane = 0; lane < k_grid_lanes; ++lane)
			{
				active[lane] &= static_cast<uint8_t>(a.lane[lane] == b.lane[lane]);
			}
			continue;
		}
		if (inst.type == ShaderInstructionType::SCbranchExecz && inst.src_num == 1 &&
		    (inst.src[0].type == ShaderOperandType::LiteralConstant ||
		     inst.src[0].type == ShaderOperandType::IntegerInlineConstant))
		{
			const uint64_t target_pc = static_cast<uint64_t>(inst.pc) + 4u + inst.src[0].constant.u;
			if (target_pc != (restored ? end_pc : reset_pc) ||
			    (!restored && (!predicated_before_reset || skipped_to_reset)))
			{
				return {};
			}
			skipped_to_reset = !restored;
			continue;
		}
		// The pre-reset branch may bypass only a source read. Any skipped
		// scalar or coordinate producer could change a later destination write.
		if (skipped_to_reset && !ShaderInstructionReadsImageResource(inst.type) &&
		    inst.type != ShaderInstructionType::SWaitcnt)
		{
			return {};
		}
		if (!ShaderTileControlFlowIsLinear(inst, index + 1 == instructions.Size()))
		{
			return {};
		}
		if (inst.dst.type == ShaderOperandType::Sgpr && inst.dst.size > 0 &&
		    inst.dst.register_id <= workgroup_register + 1 &&
		    inst.dst.register_id + inst.dst.size > workgroup_register)
		{
			return {};
		}
		if (ShaderInstructionReadsImageResource(inst.type) || ShaderInstructionWritesImageResource(inst.type))
		{
			const int resource_index = ShaderTileImageResourceIndex(inst, bind);
			if (resource_index == -2 || (resource_index == texture_index && inst.type != ShaderInstructionType::ImageStore) ||
			    (ShaderInstructionWritesImageResource(inst.type) && resource_index != texture_index))
			{
				return {};
			}
			if (resource_index == texture_index)
			{
				if (!restored || inst.mimg_dimension != 1 || inst.mimg_dmask != 1)
				{
					return {};
				}
				ShaderTileValue x {};
				ShaderTileValue y {};
				if (!ShaderTileStoreCoordinates(inst, registers, &x, &y) ||
				    !ShaderTileMarkStore(x, y, k_grid_lanes, &coverage, &seen, &active))
				{
					return {};
				}
			}
		}
		if (inst.dst.type == ShaderOperandType::Vgpr && inst.type != ShaderInstructionType::ImageStore &&
		    inst.type != ShaderInstructionType::ImageStoreMip)
		{
			const auto value = predicated_before_reset ? ShaderTileValue {}
			                                          : ShaderTileEvaluateInstruction(inst, registers, workgroup_register, k_grid_lanes);
			ShaderTileInvalidateVgpr(inst.dst, &registers);
			if (inst.dst.register_id >= 0 && inst.dst.register_id < 256 && inst.dst.size == 1)
			{
				registers[inst.dst.register_id] = value;
			}
		}
		ShaderTileInvalidateVgpr(inst.dst2, &registers);
	}
	if (coverage.width == 0 || !std::all_of(seen.begin(), seen.end(), [](uint8_t value) { return value != 0; }))
	{
		return {};
	}
	return coverage;
}

} // namespace

ShaderStorageImageTileCoverage AnalyzeShaderStorageImageTileCoverage(const ShaderCode& code, const ShaderBindResources& bind,
                                                                    int texture_index, int workgroup_register,
                                                                    const uint32_t threads[3], bool native_xy_thread_ids,
                                                                    bool group_xy_enabled, bool paired_xy_thread_ids)
{
	if (paired_xy_thread_ids)
	{
		return AnalyzeShaderStorageImagePairedCoverage(code, bind, texture_index, workgroup_register,
		                                               threads, group_xy_enabled);
	}
	if (threads != nullptr && texture_index >= 0 && texture_index < bind.textures2D.textures_num && workgroup_register >= 0 &&
	    bind.textures2D.desc[texture_index].textures2d_without_sampler && native_xy_thread_ids)
	{
		const auto bounded_coverage = AnalyzeShaderStorageImageBoundedGridCoverage(code, bind, texture_index,
		                                                                            workgroup_register, threads, group_xy_enabled);
		if (bounded_coverage.width != 0)
		{
			return bounded_coverage;
		}
		const auto grid_coverage = AnalyzeShaderStorageImageGridCoverage(code, bind, texture_index, workgroup_register, threads);
		if (grid_coverage.width != 0)
		{
			return grid_coverage;
		}
	}
	if (threads == nullptr || threads[0] == 0 || threads[0] > k_tile_lanes || threads[1] != 1 || threads[2] != 1 ||
	    texture_index < 0 || texture_index >= bind.textures2D.textures_num || workgroup_register < 0 ||
	    !bind.textures2D.desc[texture_index].textures2d_without_sampler)
	{
		return {};
	}
	std::array<ShaderTileValue, 256> registers {};
	registers[0].valid = true;
	for (uint32_t lane = 0; lane < threads[0]; ++lane)
	{
		registers[0].lane[lane] = lane;
	}
	ShaderStorageImageTileCoverage coverage {};
	std::vector<uint8_t> seen;
	const auto& instructions = code.GetInstructions();
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& inst = instructions.At(index);
		if (!ShaderTileControlFlowIsLinear(inst, index + 1 == instructions.Size()))
		{
			return {};
		}
		if (ShaderInstructionReadsImageResource(inst.type) || ShaderInstructionWritesImageResource(inst.type))
		{
			const int resource_index = ShaderTileImageResourceIndex(inst, bind);
			if (resource_index == -2)
			{
				return {};
			}
			if (resource_index == texture_index)
			{
				if (inst.type != ShaderInstructionType::ImageStore || inst.mimg_dimension != 1 || inst.mimg_dmask != 0xf)
				{
					return {};
				}
				ShaderTileValue x {};
				ShaderTileValue y {};
				if (!ShaderTileStoreCoordinates(inst, registers, &x, &y) ||
				    !ShaderTileMarkStore(x, y, threads[0], &coverage, &seen))
				{
					return {};
				}
			}
		}
		if (inst.dst.type == ShaderOperandType::Sgpr && inst.dst.size > 0 &&
		    inst.dst.register_id <= workgroup_register + 1 &&
		    inst.dst.register_id + inst.dst.size > workgroup_register)
		{
			return {};
		}
		if (inst.dst.type == ShaderOperandType::Vgpr && inst.type != ShaderInstructionType::ImageStore &&
		    inst.type != ShaderInstructionType::ImageStoreMip)
		{
			const auto value = ShaderTileEvaluateInstruction(inst, registers, workgroup_register, threads[0]);
			ShaderTileInvalidateVgpr(inst.dst, &registers);
			if (inst.dst.register_id >= 0 && inst.dst.register_id < 256 && inst.dst.size == 1)
			{
				registers[inst.dst.register_id] = value;
			}
		}
		ShaderTileInvalidateVgpr(inst.dst2, &registers);
	}
	if (coverage.width == 0 ||
	    !std::all_of(seen.begin(), seen.end(), [](uint8_t value) { return value != 0; }))
	{
		return {};
	}
	return coverage;
}

namespace {

// Keep this admission closed to known side-effect-free instructions and image
// stores. A newly parsed operation cannot inherit the skip without review.
bool EmptyGateInstructionAllowed(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SInstPrefetch:
		case ShaderInstructionType::VLshlAddU32:
		case ShaderInstructionType::SLoadDwordx4:
		case ShaderInstructionType::SLoadDwordx8:
		case ShaderInstructionType::SWaitcnt:
		case ShaderInstructionType::SBufferLoadDword:
		case ShaderInstructionType::SBufferLoadDwordx2:
		case ShaderInstructionType::VCmpLeU32:
		case ShaderInstructionType::SNorB64:
		case ShaderInstructionType::SMovB64:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::VLshlrevB32:
		case ShaderInstructionType::VCvtF32U32:
		case ShaderInstructionType::SAddI32:
		case ShaderInstructionType::VRcpF32:
		case ShaderInstructionType::VAddF32:
		case ShaderInstructionType::VMulF32:
		case ShaderInstructionType::VMovB32:
		case ShaderInstructionType::VCmpxEqU32:
		case ShaderInstructionType::VCmpxGtU32:
		case ShaderInstructionType::ImageSampleL:
		case ShaderInstructionType::VMaxF32:
		case ShaderInstructionType::VMax3F32:
		case ShaderInstructionType::SCmpLgU32:
		case ShaderInstructionType::VSubrevF32:
		case ShaderInstructionType::ImageStore:
		case ShaderInstructionType::SEndpgm: return true;
		default: return false;
	}
}

bool EmptyGateCanReachWithout(const Vector<ShaderInstruction>& instructions,
                              const std::unordered_map<uint32_t, uint32_t>& instruction_index,
                              uint32_t excluded, uint32_t target)
{
	std::vector<uint8_t> seen(instructions.Size());
	std::vector<uint32_t> pending {0u};
	while (!pending.empty())
	{
		const uint32_t index = pending.back();
		pending.pop_back();
		if (index >= instructions.Size() || index == excluded || seen[index] != 0u)
		{
			continue;
		}
		if (index == target)
		{
			return true;
		}
		seen[index] = 1u;
		const auto& inst = instructions.At(index);
		if (inst.type == ShaderInstructionType::SEndpgm)
		{
			continue;
		}
		if (ShaderInstructionHasStaticBranchTarget(inst.type))
		{
			uint32_t destination = 0;
			if (!ShaderTryGetStaticBranchTarget(instruction_index, inst, &destination))
			{
				return true;
			}
			pending.push_back(destination);
			if (inst.type == ShaderInstructionType::SBranch)
			{
				continue;
			}
		}
		pending.push_back(index + 1u);
	}
	return false;
}

bool EmptyGateWritesExec(const ShaderInstruction& inst)
{
	return inst.dst.type == ShaderOperandType::ExecLo || inst.dst.type == ShaderOperandType::ExecHi ||
	       inst.dst2.type == ShaderOperandType::ExecLo || inst.dst2.type == ShaderOperandType::ExecHi ||
	       inst.type == ShaderInstructionType::VCmpxEqU32 || inst.type == ShaderInstructionType::VCmpxGtU32;
}

} // namespace

ShaderComputeEmptyGate AnalyzeShaderComputeEmptyGate(const ShaderCode& code, const ShaderBindResources& bind)
{
	const auto& instructions = code.GetInstructions();
	if (code.GetType() != ShaderType::Compute || instructions.IsEmpty() ||
	    instructions.At(instructions.Size() - 1u).type != ShaderInstructionType::SEndpgm)
	{
		return {};
	}
	std::unordered_map<uint32_t, uint32_t> instruction_index;
	std::vector<uint32_t> stores;
	for (uint32_t i = 0; i < instructions.Size(); ++i)
	{
		const auto& inst = instructions.At(i);
		if (!EmptyGateInstructionAllowed(inst.type) || !instruction_index.emplace(inst.pc, i).second)
		{
			return {};
		}
		if (inst.type == ShaderInstructionType::ImageStore)
		{
			stores.push_back(i);
		}
	}
	if (stores.empty())
	{
		return {};
	}
	for (const auto& inst: instructions)
	{
		if (ShaderInstructionHasStaticBranchTarget(inst.type))
		{
			uint32_t destination = 0;
			if (!ShaderTryGetStaticBranchTarget(instruction_index, inst, &destination))
			{
				return {};
			}
		}
	}
	for (uint32_t gate = 0; gate + 2u < instructions.Size(); ++gate)
	{
		const auto& compare = instructions.At(gate);
		const auto& branch = instructions.At(gate + 1u);
		if (compare.type != ShaderInstructionType::VCmpxGtU32 || compare.dst.type != ShaderOperandType::VccLo ||
		    compare.format != ShaderInstructionFormat::SmaskVsrc0Vsrc1 || compare.src_num != 2 ||
		    compare.src[0].type != ShaderOperandType::Sgpr || compare.src[0].size != 1 ||
		    !MetaFillOperandIsPlain(compare.src[0]) || !MetaFillOperandIsPlain(compare.dst) ||
		    !MetaFillOperandIsImmediate(compare.src[1], 0u) || branch.type != ShaderInstructionType::SCbranchExecz)
		{
			continue;
		}
		uint32_t restore = 0;
		if (!ShaderTryGetStaticBranchTarget(instruction_index, branch, &restore) || restore <= gate + 1u ||
		    restore >= stores.front())
		{
			continue;
		}
		const auto& restore_inst = instructions.At(restore);
		if (restore_inst.type != ShaderInstructionType::SMovB64 || restore_inst.dst.type != ShaderOperandType::ExecLo ||
		    restore_inst.dst.size != 2 || restore_inst.src_num != 1 || restore_inst.src[0].type != ShaderOperandType::VccLo ||
		    restore_inst.src[0].size != 2 || !MetaFillOperandIsPlain(restore_inst.src[0]))
		{
			continue;
		}
		bool safe_suffix = true;
		for (uint32_t i = restore + 1u; i < instructions.Size(); ++i)
		{
			const auto& inst = instructions.At(i);
			if (EmptyGateWritesExec(inst))
			{
				safe_suffix = false;
				break;
			}
			if (ShaderInstructionHasStaticBranchTarget(inst.type))
			{
				uint32_t destination = 0;
				if (!ShaderTryGetStaticBranchTarget(instruction_index, inst, &destination) || destination < restore)
				{
					safe_suffix = false;
					break;
				}
			}
		}
		if (!safe_suffix)
		{
			continue;
		}
		for (uint32_t store: stores)
		{
			if (store <= restore || EmptyGateCanReachWithout(instructions, instruction_index, gate, store))
			{
				safe_suffix = false;
				break;
			}
		}
		if (!safe_suffix)
		{
			continue;
		}
		for (uint32_t load = 0; load < gate; ++load)
		{
			const auto& inst = instructions.At(load);
			if (inst.type != ShaderInstructionType::SBufferLoadDword || inst.format != ShaderInstructionFormat::SdstSvSoffset ||
			    inst.src_num != 2 || !MetaFillOperandIsSgpr(inst.dst, compare.src[0].register_id, 1) ||
			    inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].size != 4 || inst.smem_flags != 0u ||
			    inst.smem_imm_offset != 0u || inst.src[1].size != 0 ||
			    (inst.src[1].type != ShaderOperandType::IntegerInlineConstant &&
			     inst.src[1].type != ShaderOperandType::LiteralConstant) ||
			    inst.src[1].constant.i < 0 || (inst.src[1].constant.u & 3u) != 0u ||
			    EmptyGateCanReachWithout(instructions, instruction_index, load, gate))
			{
				continue;
			}
			bool unchanged = true;
			for (uint32_t i = load + 1u; i < gate; ++i)
			{
				if (ShaderOperandOverlapsSgprRange(instructions.At(i).dst, compare.src[0].register_id, 1) ||
				    ShaderOperandOverlapsSgprRange(instructions.At(i).dst2, compare.src[0].register_id, 1))
				{
					unchanged = false;
					break;
				}
			}
			if (!unchanged)
			{
				continue;
			}
			for (int b = 0; b < bind.storage_buffers.buffers_num; ++b)
			{
				const auto source = bind.storage_buffers.sources[b];
				if (bind.storage_buffers.start_register[b] == inst.src[0].register_id &&
				    (source == ShaderStorageBindingSource::MetadataSharp ||
				     source == ShaderStorageBindingSource::DynamicScalarLoad) &&
				    ShaderStorageUsageIsReadOnly(bind.storage_buffers.usages[b]) &&
				    bind.storage_buffers.code_available[b] && bind.storage_buffers.exact_matches[b] &&
				    !bind.storage_buffers.unbased_matches[b] &&
				    !bind.storage_buffers.decoded_unknown[b] && !bind.storage_buffers.indirect_descriptor_use[b])
				{
					return {b, inst.src[1].constant.u};
				}
			}
		}
	}
	return {};
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
