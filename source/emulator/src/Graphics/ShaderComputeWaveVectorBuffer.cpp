#include "Emulator/Graphics/ShaderComputeWaveVectorBuffer.h"

#include "Emulator/Graphics/ShaderComputeWaveResourceAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveWaitcnt.h"
#include "Kyty/Core/MagicEnum.h"

#include "ShaderStorageAnalysis.h"

#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {
namespace {

constexpr int      kMaxSgpr       = 105; // RDNA2 SGPR0..SGPR105
constexpr int      kMaxVgpr       = 255;
constexpr uint32_t kNoInstruction = UINT32_MAX;

ShaderComputeWaveAnalysisResult Failure(uint32_t pc, ShaderInstructionType type, const String8& detail)
{
	ShaderComputeWaveAnalysisResult result {};
	result.unsupported_pc = pc;
	result.reason         = String8::FromPrintf("paired %s %s", Core::EnumName8(type).c_str(), detail.c_str());
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

bool RegisterRangeIsValid(int register_id, int size, int maximum_register)
{
	if (register_id < 0 || size <= 0 || register_id > maximum_register)
	{
		return false;
	}
	return size <= maximum_register - register_id + 1;
}

// The instruction-local half of the contract: everything the banked lowering
// reads must be described by the operand tuple alone.
bool IsPlainTuple(const ShaderInstruction& instruction)
{
	ShaderInstructionFormat::Format format;
	int                             dwords;
	switch (instruction.type)
	{
		case ShaderInstructionType::BufferLoadDword:
			format = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			dwords = 1;
			break;
		case ShaderInstructionType::BufferLoadDwordx2:
			format = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
			dwords = 2;
			break;
		case ShaderInstructionType::BufferLoadDwordx3:
			format = ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen;
			dwords = 3;
			break;
		case ShaderInstructionType::BufferLoadDwordx4:
			format = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
			dwords = 4;
			break;
		default: return false;
	}

	if (instruction.format != format || instruction.src_num != 3 || !instruction.buffer_idxen || instruction.buffer_offen ||
	    instruction.buffer_return_old_value || instruction.buffer_flags != 0u || !OperandIsUnused(instruction.dst2) ||
	    !OperandIsUnused(instruction.src[3]) || instruction.vop3_op_sel != 0u || instruction.vop3_omod != 0u ||
	    instruction.vop_sdwa || instruction.ds_offset != 0u || instruction.ds_encoding_control != 0u ||
	    instruction.ds_encoding_registers != 0u)
	{
		return false;
	}

	const auto& destination = instruction.dst;
	const auto& vaddr       = instruction.src[0];
	const auto& descriptor  = instruction.src[1];
	const auto& soffset     = instruction.src[2];
	return OperandIsPlain(destination) && destination.type == ShaderOperandType::Vgpr && destination.size == dwords &&
	       RegisterRangeIsValid(destination.register_id, destination.size, kMaxVgpr) && OperandIsPlain(vaddr) &&
	       vaddr.type == ShaderOperandType::Vgpr && vaddr.size == 1 &&
	       RegisterRangeIsValid(vaddr.register_id, vaddr.size, kMaxVgpr) && OperandIsPlain(descriptor) &&
	       descriptor.type == ShaderOperandType::Sgpr && descriptor.size == 4 && (descriptor.register_id & 3) == 0 &&
	       RegisterRangeIsValid(descriptor.register_id, descriptor.size, kMaxSgpr) && OperandIsPlain(soffset) &&
	       soffset.size == 0 && soffset.type == ShaderOperandType::IntegerInlineConstant && soffset.constant.i == 0;
}

bool IsPlainAtomicUmaxTuple(const ShaderInstruction& instruction)
{
	if (instruction.type != ShaderInstructionType::BufferAtomicUmax ||
	    instruction.format != ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen || instruction.src_num != 3 ||
	    instruction.buffer_idxen || instruction.buffer_offen || instruction.buffer_return_old_value || instruction.buffer_flags != 0u ||
	    (instruction.buffer_imm_offset & 3u) != 0u || !OperandIsUnused(instruction.dst2) || !OperandIsUnused(instruction.src[3]) ||
	    instruction.vop3_op_sel != 0u || instruction.vop3_omod != 0u || instruction.vop_sdwa || instruction.mimg_address_num != 0 ||
	    instruction.ds_offset != 0u || instruction.ds_encoding_control != 0u || instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	const auto& value      = instruction.dst;
	const auto& vaddr      = instruction.src[0];
	const auto& descriptor = instruction.src[1];
	const auto& soffset    = instruction.src[2];
	return OperandIsPlain(value) && value.type == ShaderOperandType::Vgpr && value.size == 1 &&
	       RegisterRangeIsValid(value.register_id, value.size, kMaxVgpr) && OperandIsPlain(vaddr) &&
	       vaddr.type == ShaderOperandType::Vgpr && vaddr.size == 1 && RegisterRangeIsValid(vaddr.register_id, vaddr.size, kMaxVgpr) &&
	       OperandIsPlain(descriptor) && descriptor.type == ShaderOperandType::Sgpr && descriptor.size == 4 &&
	       (descriptor.register_id & 3) == 0 && RegisterRangeIsValid(descriptor.register_id, descriptor.size, kMaxSgpr) &&
	       OperandIsPlain(soffset) && soffset.type == ShaderOperandType::IntegerInlineConstant && soffset.size == 0 &&
	       soffset.constant.i == 0;
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

// A descriptor quad that no instruction writes still carries the user-SGPR
// value bound at dispatch entry; it must resolve to exactly one bound storage
// buffer whose register base matches.
const ShaderBufferResource* FindDirectDescriptor(const ShaderCode& code, const ShaderBindResources& bind, int descriptor)
{
	for (const auto& instruction: code.GetInstructions())
	{
		if (WritesSgprRange(instruction, descriptor, 4))
		{
			return nullptr;
		}
	}
	const ShaderBufferResource* resource = nullptr;
	for (int i = 0; i < bind.storage_buffers.buffers_num; ++i)
	{
		if (bind.storage_buffers.dynamic_sload[i] || bind.storage_buffers.start_register[i] != descriptor)
		{
			continue;
		}
		if (resource != nullptr)
		{
			return nullptr;
		}
		resource = &bind.storage_buffers.buffers[i];
	}
	return resource;
}

} // namespace

bool ShaderComputeWaveVectorBufferLoadSupported(const ShaderInstruction& instruction)
{
	return IsPlainTuple(instruction);
}

bool ShaderComputeWaveVectorBufferAtomicUmaxSupported(const ShaderInstruction& instruction)
{
	return IsPlainAtomicUmaxTuple(instruction);
}

ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveVectorBufferAtomicUmax(const ShaderCode& code, uint32_t index,
                                                                                const ShaderBindResources& bind)
{
	const auto& instructions = code.GetInstructions();
	if (index >= instructions.Size())
	{
		return Failure(0, ShaderInstructionType::BufferAtomicUmax, "admission index is outside the parsed program");
	}
	const auto& atomic = instructions.At(index);
	if (!IsPlainAtomicUmaxTuple(atomic))
	{
		return Failure(atomic.pc, atomic.type,
		               "requires the aligned no-index, no-offset, no-return dword tuple with zero S_OFFSET and no cache/control bits");
	}
	const int descriptor = atomic.src[1].register_id;
	const auto* resource = FindDirectDescriptor(code, bind, descriptor);
	if (resource == nullptr)
	{
		return Failure(atomic.pc, atomic.type, "V# is not a unique, never-written shader-entry binding");
	}
	int binding = -1;
	for (int i = 0; i < bind.storage_buffers.buffers_num; ++i)
	{
		if (&bind.storage_buffers.buffers[i] == resource)
		{
			binding = i;
			break;
		}
	}
	if (binding < 0)
	{
		return Failure(atomic.pc, atomic.type, "V# has no physical storage binding");
	}
	const auto& buffers = bind.storage_buffers;
	const auto source = buffers.sources[binding];
	if ((source != ShaderStorageBindingSource::DirectResource && source != ShaderStorageBindingSource::MetadataSharp) ||
	    buffers.usages[binding] != ShaderStorageUsage::ReadWrite || buffers.accesses[binding] != ShaderStorageAccess::Raw ||
	    !buffers.raw_vmem_oob_guarded[binding] ||
	    (source == ShaderStorageBindingSource::MetadataSharp && (!buffers.code_available[binding] || !buffers.exact_matches[binding])))
	{
		return Failure(atomic.pc, atomic.type, "V# is not an exactly matched writable raw buffer with guarded VMEM access");
	}
	const uint64_t bytes = ShaderBufferByteSize(resource->Stride(), resource->NumRecords());
	const uint64_t span  = static_cast<uint64_t>(atomic.buffer_imm_offset) + 4u;
	if (ShaderGen5RawDescriptorAlwaysOutOfBounds(*resource) || !ShaderRawStorageDescriptorSupported(*resource) ||
	    resource->SwizzleEnabled() || resource->AddTid() || span > bytes)
	{
		return Failure(atomic.pc, atomic.type, "requires a non-empty, unswizzled raw V# without ADD_TID and with the atomic dword in range");
	}
	return {true, 0, {}};
}

ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveVectorBufferLoad(const ShaderCode& code, uint32_t index,
                                                                         const ShaderBindResources& bind)
{
	const auto& instructions = code.GetInstructions();
	if (index >= instructions.Size())
	{
		return Failure(0, ShaderInstructionType::Unknown, "admission index is outside the parsed program");
	}
	const auto& load = instructions.At(index);
	if (!IsPlainTuple(load))
	{
		return Failure(load.pc, load.type, String8::FromPrintf("%s requires the exact IDXEN dword tuple: no OFFEN/GLC/LDS/SLC/TFE or undefined "
		                                            "bits, zero inline S_OFFSET, aligned SGPR-quad V#, in-range VGPR operands",
		                                            Core::EnumName8(load.type).c_str()));
	}

	const int                  descriptor = load.src[1].register_id;
	const ShaderBufferResource* resource  = nullptr;
	for (uint32_t cursor = index; cursor > 0; --cursor)
	{
		if (WritesSgprRange(instructions.At(cursor - 1), descriptor, 4))
		{
			const auto& sload   = instructions.At(cursor - 1);
			bool        drained = false;
			for (uint32_t scan = cursor; scan < index; ++scan)
			{
				drained = drained || ShaderComputeWaveIsLgkmZeroOnlyWait(instructions.At(scan));
			}
			if (!ShaderPairedEudStorageLoadSupported(sload, bind) || sload.dst.register_id != descriptor || sload.dst.size != 4)
			{
				return Failure(load.pc, load.type,
				               String8::FromPrintf("V# is last written at pc=0x%08" PRIx32
				                                   " by an instruction other than a mapped EUD S_LOAD",
				                                   sload.pc));
			}
			if (!drained)
			{
				return Failure(load.pc, load.type, "V# from its S_LOAD is still in flight without an lgkmcnt(0) wait");
			}
			for (const auto* labels: {&code.GetLabels(), &code.GetIndirectLabels()})
			{
				for (const auto& label: *labels)
				{
					if (!label.IsDisabled() && label.GetDst() > sload.pc && label.GetDst() <= load.pc)
					{
						return Failure(load.pc, load.type, String8::FromPrintf("V# producer and consumer are joined by a branch target at "
						                                          "pc=0x%08" PRIx32,
						                                          label.GetDst()));
					}
				}
			}
			const auto* mapping = FindUniqueMapping(bind, sload.pc, descriptor);
			if (mapping == nullptr || mapping->last_consumer_pc < load.pc || !mapping->raw_vmem_oob_guarded ||
			    mapping->resource_index < 0 || mapping->resource_index >= bind.storage_buffers.buffers_num ||
			    bind.storage_buffers.start_register[mapping->resource_index] < 0 ||
			    !bind.storage_buffers.dynamic_sload[mapping->resource_index])
			{
				return Failure(load.pc, load.type, "collector mapping does not cover this VMEM consumer with a bound storage resource");
			}
			for (int zero = 0; zero < bind.zero_sbuffer_resources.buffers_num &&
			                 zero < ShaderZeroSBufferResources::BUFFERS_MAX;
			     ++zero)
			{
				if (bind.zero_sbuffer_resources.start_register[zero] == descriptor)
				{
					return Failure(load.pc, load.type, "V# is lowered by the out-of-range zero policy, which is not admitted");
				}
			}
			resource = &bind.storage_buffers.buffers[mapping->resource_index];
			break;
		}
	}
	if (resource == nullptr)
	{
		resource = FindDirectDescriptor(code, bind, descriptor);
		if (resource == nullptr)
		{
			return Failure(load.pc, load.type, "V# has neither a mapped EUD S_LOAD producer nor a unique never-written direct binding");
		}
	}

	const uint64_t bytes = ShaderBufferByteSize(resource->Stride(), resource->NumRecords());
	const uint64_t span  = static_cast<uint64_t>(load.buffer_imm_offset) + static_cast<uint64_t>(load.dst.size) * 4u;
	if (ShaderGen5RawDescriptorAlwaysOutOfBounds(*resource) || !ShaderRawStorageDescriptorSupported(*resource) ||
	    resource->SwizzleEnabled() || resource->AddTid() || span > bytes)
	{
		return Failure(load.pc, load.type, "requires a non-empty unswizzled raw V# without ADD_TID whose byte range contains the "
		                        "immediate span");
	}
	return {true, 0, {}};
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
