#include "Emulator/Graphics/ShaderComputeWaveResourceAnalysis.h"

#include "ShaderStorageAnalysis.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr int kMaxSgpr = 105; // RDNA2 SGPR0..SGPR105

static bool ShaderPairedEudOperandIsPlain(const ShaderOperand& operand) noexcept
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u &&
	       !operand.dpp && operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u &&
	       !operand.dpp_fetch_inactive && !operand.dpp_bound_ctrl;
}

static bool ShaderPairedEudOperandIsUnused(const ShaderOperand& operand) noexcept
{
	return operand.type == ShaderOperandType::Unknown && operand.constant.u == 0u && operand.register_id == 0 && operand.size == 0 &&
	       ShaderPairedEudOperandIsPlain(operand);
}

static bool ShaderPairedEudInstructionHasOnlySmemControls(const ShaderInstruction& instruction) noexcept
{
	return ShaderPairedEudOperandIsUnused(instruction.dst2) && ShaderPairedEudOperandIsUnused(instruction.src[2]) &&
	       ShaderPairedEudOperandIsUnused(instruction.src[3]) && instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u &&
	       !instruction.vop_sdwa && instruction.ds_offset == 0u && instruction.ds_encoding_control == 0u &&
	       instruction.ds_encoding_registers == 0u;
}

} // namespace

bool ShaderPairedEudStorageLoadSupported(const ShaderInstruction& instruction, const ShaderBindResources& bind) noexcept
{
	if (instruction.type != ShaderInstructionType::SLoadDwordx4 ||
	    instruction.format != ShaderInstructionFormat::Sdst4SbaseSoffset || instruction.src_num != 2 ||
	    instruction.smem_imm_offset != 0 || instruction.smem_flags != 0u || !ShaderPairedEudInstructionHasOnlySmemControls(instruction) ||
	    !bind.extended.used || bind.extended.slot != k_gen5_eud_direct_type || bind.extended.eud_size_dw == 0u ||
	    bind.extended.data.Base() == 0u ||
	    bind.extended.eud_user_sgpr_num <= 1 || bind.extended.eud_user_sgpr_num > HW::UserSgprInfo::SGPRS_MAX ||
	    bind.extended.eud_offset_base < 0x20 || (bind.extended.eud_offset_base & 3) != 0)
	{
		return false;
	}
	const auto& destination = instruction.dst;
	const auto& base        = instruction.src[0];
	const auto& offset      = instruction.src[1];
	if (destination.type != ShaderOperandType::Sgpr || destination.size != 4 || destination.register_id < 0 ||
	    destination.register_id > kMaxSgpr - 3 || (destination.register_id & 3) != 0 || !ShaderPairedEudOperandIsPlain(destination) ||
	    bind.extended.start_register < 0 || bind.extended.start_register > kMaxSgpr - 1 ||
	    bind.extended.start_register + 2 > bind.extended.eud_user_sgpr_num || (bind.extended.start_register & 1) != 0 ||
	    base.type != ShaderOperandType::Sgpr || base.size != 2 || base.register_id != bind.extended.start_register ||
	    !ShaderPairedEudOperandIsPlain(base) || !ShaderPairedEudOperandIsPlain(offset) || offset.size != 0 ||
	    (offset.type != ShaderOperandType::IntegerInlineConstant && offset.type != ShaderOperandType::LiteralConstant) ||
	    offset.constant.i < 0 || (offset.constant.u & 3u) != 0u)
	{
		return false;
	}
	const int offset_dw = offset.constant.i / 4;
	if (!ShaderGen5EudSpanAllowed(16 + offset_dw, 4, bind.extended.eud_size_dw))
	{
		return false;
	}

	const auto& mappings = bind.dynamic_sloads.records;
	bool       found    = false;
	for (uint32_t index = 0; index < mappings.Size(); ++index)
	{
		const auto& mapping = mappings.At(index);
		if (mapping.instruction_pc != instruction.pc || mapping.destination_register != destination.register_id)
		{
			continue;
		}
		if (found || mapping.kind != ShaderDynamicSLoadResourceKind::StorageBuffer || mapping.offset_dw != offset_dw ||
		    mapping.dword_count != 4 || mapping.last_consumer_pc <= instruction.pc || mapping.resource_field_offset < 0 ||
		    mapping.resource_field_offset > 4 - mapping.dword_count || bind.storage_buffers.buffers_num < 0 ||
		    bind.storage_buffers.buffers_num > ShaderStorageResources::BUFFERS_MAX || mapping.resource_index < 0 ||
		    mapping.resource_index >= bind.storage_buffers.buffers_num ||
		    !bind.storage_buffers.dynamic_sload[mapping.resource_index] ||
		    bind.storage_buffers.sources[mapping.resource_index] != ShaderStorageBindingSource::DynamicScalarLoad)
		{
			return false;
		}
		found = true;
	}
	return found;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
