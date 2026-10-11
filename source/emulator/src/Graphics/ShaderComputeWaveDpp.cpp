#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

bool Plain(ShaderOperand operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u &&
	       !operand.dpp && operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u &&
	       !operand.dpp_fetch_inactive && !operand.dpp_bound_ctrl;
}

bool Vector(const ShaderOperand& operand)
{
	return Plain(operand) && operand.type == ShaderOperandType::Vgpr && operand.size == 1 &&
	       operand.register_id >= 0 && operand.register_id <= 255;
}

bool Scalar(const ShaderOperand& operand)
{
	if (!Plain(operand)) { return false; }
	switch (operand.type)
	{
		case ShaderOperandType::Sgpr: return operand.size == 1 && operand.register_id >= 0 && operand.register_id <= 105;
		case ShaderOperandType::VccLo:
		case ShaderOperandType::VccHi: return operand.size == 1 && operand.register_id == 0;
		case ShaderOperandType::LiteralConstant:
		case ShaderOperandType::IntegerInlineConstant:
		case ShaderOperandType::FloatInlineConstant: return operand.size == 0;
		default: return false;
	}
}

bool Destination(const ShaderInstruction& instruction)
{
	return Vector(instruction.dst) && instruction.dst2.size == 0 &&
	       (instruction.dst2.type == ShaderOperandType::Unknown || instruction.dst2.type == ShaderOperandType::Null) &&
	       Plain(instruction.dst2) && !instruction.vop_sdwa && instruction.vop3_omod == 0u && instruction.ds_offset == 0u &&
	       instruction.ds_encoding_control == 0u && instruction.ds_encoding_registers == 0u;
}

bool UnusedSources(const ShaderInstruction& instruction, int first)
{
	for (int index = first; index < 4; ++index)
	{
		const auto& operand = instruction.src[index];
		if (operand.type != ShaderOperandType::Unknown || operand.size != 0 || !Plain(operand)) { return false; }
	}
	return true;
}

bool DppControl(uint32_t control)
{
	return control <= 0xffu || (control >= 0x101u && control <= 0x10fu) ||
	       (control >= 0x111u && control <= 0x11fu) || (control >= 0x121u && control <= 0x12fu) ||
	       control == 0x140u || control == 0x141u;
}

} // namespace

bool ShaderComputeWaveDppInstructionSupported(const ShaderInstruction& instruction)
{
	if (!Destination(instruction) || instruction.vop3_op_sel != 0u || !instruction.src[0].dpp ||
	    instruction.src[0].dpp_row_mask > 15u || instruction.src[0].dpp_bank_mask > 15u ||
	    !DppControl(instruction.src[0].dpp_ctrl)) { return false; }
	auto source = instruction.src[0];
	source.dpp = false;
	source.dpp_ctrl = 0;
	source.dpp_row_mask = 0;
	source.dpp_bank_mask = 0;
	source.dpp_fetch_inactive = false;
	source.dpp_bound_ctrl = false;
	if (!Vector(source)) { return false; }
	if (instruction.type == ShaderInstructionType::VMovB32)
	{
		return instruction.src_num == 1 && instruction.format == ShaderInstructionFormat::SVdstSVsrc0 && UnusedSources(instruction, 1);
	}
	const bool binary = instruction.type == ShaderInstructionType::VAndB32 || instruction.type == ShaderInstructionType::VOrB32 ||
	                    instruction.type == ShaderInstructionType::VXorB32;
	return binary && instruction.src_num == 2 && instruction.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1 &&
	       (Vector(instruction.src[1]) || Scalar(instruction.src[1])) && UnusedSources(instruction, 2);
}

bool ShaderComputeWavePermutationSupported(const ShaderInstruction& instruction)
{
	const bool permutation = instruction.type == ShaderInstructionType::VPermlane16B32 ||
	                         instruction.type == ShaderInstructionType::VPermlanex16B32;
	return permutation && Destination(instruction) && instruction.vop3_op_sel <= 3u && instruction.src_num == 3 &&
	       instruction.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2 && Vector(instruction.src[0]) &&
	       Scalar(instruction.src[1]) && Scalar(instruction.src[2]) && UnusedSources(instruction, 3);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
