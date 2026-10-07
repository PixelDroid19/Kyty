#include "Emulator/Graphics/ShaderComputeWaveWaitcnt.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint8_t  kSoppWaitcntOpcode     = 0x0cu;
constexpr uint32_t kLgkmZeroOnlyImmediate = 0xc07fu;

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

bool IsExactSoppWaitTuple(const ShaderInstruction& instruction)
{
	if (instruction.type != ShaderInstructionType::SWaitcnt || instruction.sopp_opcode != kSoppWaitcntOpcode ||
	    instruction.format != ShaderInstructionFormat::Imm || instruction.src_num != 1 || !OperandIsUnused(instruction.dst) ||
	    !OperandIsUnused(instruction.dst2) || instruction.src[0].type != ShaderOperandType::LiteralConstant ||
	    instruction.src[0].size != 0 || !OperandIsPlain(instruction.src[0]) || instruction.vop3_op_sel != 0u ||
	    instruction.vop3_omod != 0u || instruction.vop_sdwa || instruction.ds_offset != 0u || instruction.ds_encoding_control != 0u ||
	    instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	for (int source = 1; source < 4; ++source)
	{
		if (!OperandIsUnused(instruction.src[source]))
		{
			return false;
		}
	}
	return true;
}

bool IsLgkmZeroOnlyWait(const ShaderInstruction& instruction)
{
	return IsExactSoppWaitTuple(instruction) && instruction.src[0].constant.u == kLgkmZeroOnlyImmediate;
}

} // namespace

bool ShaderComputeWaveIsExactWait(const ShaderInstruction& instruction)
{
	return IsExactSoppWaitTuple(instruction);
}

bool ShaderComputeWaveIsLgkmZeroOnlyWait(const ShaderInstruction& instruction)
{
	return IsLgkmZeroOnlyWait(instruction);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
