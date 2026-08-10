#include "Emulator/Graphics/Shader.h"

#include "ShaderStorageAnalysis.h"

#include "Kyty/Core/DbgAssert.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
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
	std::unordered_map<uint32_t, uint32_t> instruction_index;
	instruction_index.reserve(instruction_count);
	for (uint32_t index = 0; index < instruction_count; ++index)
	{
		if (!instruction_index.emplace(instructions.At(index).pc, index).second)
		{
			return std::vector<uint8_t>(instruction_count, descriptor_live_mask);
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
			if (!ShaderTryGetStaticBranchTarget(instruction_index, inst, &target))
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
	return type == ShaderInstructionType::ImageStore || type == ShaderInstructionType::ImageStoreMip;
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

ShaderDirectImageUse AnalyzeShaderDirectImageUse(const ShaderCode& code, int start_register)
{
	ShaderDirectImageUse result;

	for (const auto& inst: code.GetInstructions())
	{
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
		result.reads = result.reads || read;
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

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
