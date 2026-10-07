#include "Emulator/Graphics/ShaderComputeWaveScalarBuffer.h"

#include "Emulator/Graphics/ShaderComputeWaveResourceAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveWaitcnt.h"

#include "ShaderStorageAnalysis.h"

#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr int      kMaxSgpr       = 103;
constexpr uint32_t kNoInstruction = UINT32_MAX;

ShaderComputeWaveAnalysisResult Failure(uint32_t pc, const String8& detail)
{
	ShaderComputeWaveAnalysisResult result {};
	result.unsupported_pc = pc;
	result.reason         = String8::FromPrintf("paired SBufferLoadDword %s", detail.c_str());
	return result;
}

bool OperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool OperandIsUnused(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Unknown && operand.size == 0 && OperandIsPlain(operand);
}

// The parser folds a null SOFFSET and its immediate into one integer constant;
// a 7-bit SOFFSET cannot encode an inline constant itself.
bool IsPlainTuple(const ShaderInstruction& instruction)
{
	if (instruction.type != ShaderInstructionType::SBufferLoadDword || instruction.format != ShaderInstructionFormat::SdstSvSoffset ||
	    instruction.src_num != 2 || instruction.smem_flags != 0u || instruction.smem_imm_offset != 0 ||
	    !OperandIsUnused(instruction.dst2) || !OperandIsUnused(instruction.src[2]) || !OperandIsUnused(instruction.src[3]) ||
	    instruction.vop3_op_sel != 0u || instruction.vop3_omod != 0u || instruction.vop_sdwa || instruction.ds_offset != 0u ||
	    instruction.ds_encoding_control != 0u || instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	const auto& destination = instruction.dst;
	const auto& descriptor  = instruction.src[0];
	const auto& offset      = instruction.src[1];
	const bool  vcc_lo      = destination.type == ShaderOperandType::VccLo && destination.register_id == 0;
	const bool  sgpr = destination.type == ShaderOperandType::Sgpr && destination.register_id >= 0 && destination.register_id <= kMaxSgpr;
	return OperandIsPlain(destination) && destination.size == 1 && (vcc_lo || sgpr) && OperandIsPlain(descriptor) &&
	       descriptor.type == ShaderOperandType::Sgpr && descriptor.size == 4 && descriptor.register_id >= 0 &&
	       descriptor.register_id <= kMaxSgpr - 3 && (descriptor.register_id & 3) == 0 && OperandIsPlain(offset) && offset.size == 0 &&
	       offset.type == ShaderOperandType::IntegerInlineConstant && offset.constant.i >= 0 && (offset.constant.u & 3u) == 0u;
}

bool WritesSgprRange(const ShaderInstruction& instruction, int start_register, int registers_num)
{
	return ShaderOperandOverlapsSgprRange(instruction.dst, start_register, registers_num) ||
	       ShaderOperandOverlapsSgprRange(instruction.dst2, start_register, registers_num);
}

const ShaderDynamicSLoadMapping* FindUniqueMapping(const ShaderBindResources& bind, uint32_t pc, int destination)
{
	const ShaderDynamicSLoadMapping* found = nullptr;
	for (const auto& mapping: bind.dynamic_sloads.records)
	{
		if (mapping.instruction_pc != pc || mapping.destination_register != destination)
		{
			continue;
		}
		if (found != nullptr)
		{
			return nullptr;
		}
		found = &mapping;
	}
	return found;
}

} // namespace

ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveScalarBufferLoad(const ShaderCode& code, uint32_t index,
                                                                        const ShaderBindResources& bind)
{
	const auto& instructions = code.GetInstructions();
	if (index >= instructions.Size())
	{
		return Failure(0, "admission index is outside the parsed program");
	}
	const auto& load = instructions.At(index);
	if (!IsPlainTuple(load))
	{
		return Failure(load.pc, "requires clear GLC/DLC/undefined bits, a null SOFFSET with a 4-aligned immediate, an aligned SGPR-quad "
		                        "V# and a VCC_LO or ordinary SGPR destination");
	}

	const int descriptor = load.src[0].register_id;
	uint32_t  producer   = kNoInstruction;
	bool      drained    = false;
	for (uint32_t cursor = index; cursor > 0; --cursor)
	{
		const auto& previous = instructions.At(cursor - 1);
		if (WritesSgprRange(previous, descriptor, 4))
		{
			producer = cursor - 1;
			break;
		}
		drained = drained || ShaderComputeWaveIsLgkmZeroOnlyWait(previous);
	}
	if (producer == kNoInstruction)
	{
		return Failure(load.pc, "V# has no mapped EUD S_LOAD producer");
	}
	const auto& sload = instructions.At(producer);
	if (!ShaderPairedEudStorageLoadSupported(sload, bind) || sload.dst.register_id != descriptor || sload.dst.size != 4)
	{
		return Failure(load.pc, String8::FromPrintf("V# is last written at pc=0x%08" PRIx32 " by an instruction other than a mapped EUD S_LOAD",
		                                            sload.pc));
	}
	if (!drained)
	{
		return Failure(load.pc, "V# from its S_LOAD is still in flight without an lgkmcnt(0) wait");
	}
	for (const auto* labels: {&code.GetLabels(), &code.GetIndirectLabels()})
	{
		for (const auto& label: *labels)
		{
			if (!label.IsDisabled() && label.GetDst() > sload.pc && label.GetDst() <= load.pc)
			{
				return Failure(load.pc, String8::FromPrintf("V# producer and consumer are joined by a branch target at pc=0x%08" PRIx32,
				                                            label.GetDst()));
			}
		}
	}

	const auto* mapping = FindUniqueMapping(bind, sload.pc, descriptor);
	if (mapping == nullptr || mapping->last_consumer_pc < load.pc || mapping->resource_index < 0 ||
	    mapping->resource_index >= bind.storage_buffers.buffers_num ||
	    bind.storage_buffers.start_register[mapping->resource_index] < 0)
	{
		return Failure(load.pc, "collector mapping does not cover this consumer with a bound storage resource");
	}
	for (int zero = 0; zero < bind.zero_sbuffer_resources.buffers_num && zero < ShaderZeroSBufferResources::BUFFERS_MAX; ++zero)
	{
		if (bind.zero_sbuffer_resources.start_register[zero] == descriptor)
		{
			return Failure(load.pc, "V# is lowered by the out-of-range zero policy, which is not admitted");
		}
	}
	const auto&    resource = bind.storage_buffers.buffers[mapping->resource_index];
	const uint64_t bytes    = ShaderBufferByteSize(resource.Stride(), resource.NumRecords());
	const uint64_t end      = static_cast<uint64_t>(load.src[1].constant.u) + 4u;
	if (ShaderGen5SBufferDescriptorAlwaysOutOfBounds(resource) || resource.SwizzleEnabled() || end > bytes)
	{
		return Failure(load.pc, "requires an unswizzled V# whose byte range contains the loaded dword");
	}
	return {true, 0, {}};
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
