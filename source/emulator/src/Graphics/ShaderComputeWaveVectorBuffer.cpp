#include "Emulator/Graphics/ShaderComputeWaveVectorBuffer.h"

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

} // namespace

bool ShaderComputeWaveVectorBufferLoadSupported(const ShaderInstruction& instruction)
{
	return IsPlainTuple(instruction);
}

bool ShaderComputeWaveVectorBufferAtomicUmaxSupported(const ShaderInstruction& instruction)
{
	return IsPlainAtomicUmaxTuple(instruction);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
