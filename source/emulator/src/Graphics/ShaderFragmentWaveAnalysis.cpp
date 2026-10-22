#include "Kyty/Core/MagicEnum.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <map>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {
namespace {

bool Plain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool Vector(const ShaderOperand& operand)
{
	return Plain(operand) && operand.type == ShaderOperandType::Vgpr && operand.size == 1 && operand.register_id >= 0 &&
	       operand.register_id <= 255;
}

bool Constant(const ShaderOperand& operand, uint32_t limit)
{
	return Plain(operand) && operand.size == 0 && operand.constant.u < limit &&
	       (operand.type == ShaderOperandType::LiteralConstant || operand.type == ShaderOperandType::IntegerInlineConstant);
}

bool Unused(const ShaderOperand& operand)
{
	return Plain(operand) && operand.size == 0 && (operand.type == ShaderOperandType::Unknown || operand.type == ShaderOperandType::Null);
}

bool Metadata(const ShaderInstruction& instruction)
{
	return !instruction.vop_sdwa && instruction.vop_sdwa_ctrl == 0u && instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u &&
	       instruction.ds_offset == 0u && instruction.ds_encoding_control == 0u && instruction.ds_encoding_registers == 0u &&
	       Unused(instruction.dst2);
}

bool Interpolation(const ShaderInstruction& instruction, const ShaderPixelInputInfo& pixel)
{
	const bool move = instruction.type == ShaderInstructionType::VInterpMovF32;
	if (!move && instruction.type != ShaderInstructionType::VInterpP1F32 && instruction.type != ShaderInstructionType::VInterpP2F32)
	{
		return false;
	}
	return Metadata(instruction) && instruction.format == ShaderInstructionFormat::VdstVsrcAttrChan && instruction.src_num == 3 &&
	       Vector(instruction.dst) && (move ? Constant(instruction.src[0], 3u) : Vector(instruction.src[0])) &&
	       Constant(instruction.src[1], pixel.input_num) && Constant(instruction.src[1], 32u) && Constant(instruction.src[2], 4u) &&
	       Unused(instruction.src[3]);
}

bool Covers(const ShaderOperand& operand, int reg)
{
	return operand.type == ShaderOperandType::Vgpr && operand.register_id <= reg && reg < operand.register_id + operand.size;
}

bool Writes(const ShaderInstruction& instruction, int reg)
{
	return Covers(instruction.dst, reg) || Covers(instruction.dst2, reg);
}

bool Reads(const ShaderInstruction& instruction, int reg)
{
	for (int source = 0; source < instruction.src_num; ++source)
	{
		if (Covers(instruction.src[source], reg))
		{
			return true;
		}
	}
	return false;
}

bool ScalarCovers(const ShaderOperand& operand, uint32_t reg)
{
	return operand.type == ShaderOperandType::Sgpr && operand.register_id >= 0 && operand.size > 0 &&
	       static_cast<uint32_t>(operand.register_id) <= reg &&
	       reg - static_cast<uint32_t>(operand.register_id) < static_cast<uint32_t>(operand.size);
}

bool EntryParameterOverwrite(const ShaderInstruction& instruction, uint32_t reg)
{
	const bool move = instruction.type == ShaderInstructionType::SMovB32 || instruction.type == ShaderInstructionType::SMovB64;
	return move && Plain(instruction.dst) && ScalarCovers(instruction.dst, reg) && instruction.src_num == 1 &&
	       ShaderClassifyComputeWaveInstruction(instruction) == ShaderComputeWaveInstructionKind::ScalarCopy;
}

bool LeavesEntryPrefix(const ShaderInstruction& instruction)
{
	const auto name = Core::EnumName8(instruction.type);
	return name.ContainsStr("branch") || name.ContainsStr("Branch") || instruction.type == ShaderInstructionType::SEndpgm ||
	       instruction.type == ShaderInstructionType::SSetpcB64 || instruction.type == ShaderInstructionType::SSwappcB64;
}

bool ChangesExecution(const ShaderInstruction& instruction)
{
	const auto name = Core::EnumName8(instruction.type);
	return name.ContainsStr("branch") || name.ContainsStr("Branch") || name.StartsWith("VCmpx") || name.ContainsStr("Saveexec") ||
	       instruction.type == ShaderInstructionType::SEndpgm || instruction.dst.type == ShaderOperandType::ExecLo ||
	       instruction.dst.type == ShaderOperandType::ExecHi;
}

} // namespace

bool ShaderFragmentInterpolationPairSupported(const ShaderCode& code, uint32_t index, const ShaderPixelInputInfo& pixel)
{
	const auto& instructions = code.GetInstructions();
	if (index >= instructions.Size())
	{
		return false;
	}
	uint32_t first = index;
	if (instructions.At(index).type == ShaderInstructionType::VInterpP2F32)
	{
		while (first > 0u && !Writes(instructions.At(first - 1u), instructions.At(index).dst.register_id))
		{
			--first;
		}
		if (first == 0u)
		{
			return false;
		}
		--first;
	}
	const auto& p1 = instructions.At(first);
	if (p1.type != ShaderInstructionType::VInterpP1F32 || !Interpolation(p1, pixel))
	{
		return false;
	}
	uint32_t second = first + 1u;
	while (second < instructions.Size() && !Writes(instructions.At(second), p1.dst.register_id))
	{
		if (Reads(instructions.At(second), p1.dst.register_id) || ChangesExecution(instructions.At(second)))
		{
			return false;
		}
		++second;
	}
	if (second >= instructions.Size())
	{
		return false;
	}
	for (const auto& label: code.GetLabels())
	{
		if (label.GetDst() > p1.pc && label.GetDst() <= instructions.At(second).pc)
		{
			return false;
		}
	}
	const auto& p2 = instructions.At(second);
	if (p2.type != ShaderInstructionType::VInterpP2F32 || !Interpolation(p2, pixel) || p2.src[1].constant.u != p1.src[1].constant.u ||
	    p2.src[2].constant.u != p1.src[2].constant.u)
	{
		return false;
	}
	bool native_coordinates = false;
	for (uint32_t field: {1u, 2u, 5u, 6u})
	{
		const int reg = static_cast<int>(ShaderPixelSystemInputRegister(pixel, field));
		native_coordinates |= (pixel.system_input_enable & pixel.system_input_address & (1u << field)) != 0u &&
		                      p1.src[0].register_id == reg && p2.src[0].register_id == reg + 1;
	}
	if (!native_coordinates)
	{
		return false;
	}
	for (uint32_t prior = 0; prior < second; ++prior)
	{
		if ((prior < first && Writes(instructions.At(prior), p1.src[0].register_id)) ||
		    Writes(instructions.At(prior), p2.src[0].register_id))
		{
			return false;
		}
	}
	return index == first || index == second;
}

ShaderComputeWaveAnalysisResult ShaderAnalyzeFragmentParameterBase(const ShaderCode& code, uint32_t parameter_register)
{
	bool     initialized        = false;
	bool     captured           = false;
	bool     saw_control        = false;
	bool     parameter_pristine = true;
	uint32_t base               = 0u;
	for (const auto& instruction: code.GetInstructions())
	{
		auto writes_parameter = [parameter_register](const ShaderOperand& operand)
		{
			return parameter_register != UINT32_MAX && operand.type == ShaderOperandType::Sgpr && operand.register_id >= 0 &&
			       static_cast<uint32_t>(operand.register_id) <= parameter_register &&
			       parameter_register - static_cast<uint32_t>(operand.register_id) < static_cast<uint32_t>(operand.size);
		};
		if (writes_parameter(instruction.dst) || writes_parameter(instruction.dst2))
		{
			parameter_pristine = false;
		}
		const bool interpolation = instruction.type == ShaderInstructionType::VInterpMovF32 ||
		                           instruction.type == ShaderInstructionType::VInterpP1F32 ||
		                           instruction.type == ShaderInstructionType::VInterpP2F32;
		if (interpolation && !initialized)
		{
			return {false, instruction.pc, "fragment interpolation parameter base is not initialized"};
		}
		saw_control |= LeavesEntryPrefix(instruction);
		if (instruction.dst.type != ShaderOperandType::M0 && instruction.dst2.type != ShaderOperandType::M0)
		{
			continue;
		}
		const auto& source   = instruction.src[0];
		const bool  constant = source.type == ShaderOperandType::LiteralConstant || source.type == ShaderOperandType::IntegerInlineConstant;
		const bool  system   = parameter_register <= 32u && source.type == ShaderOperandType::Sgpr && source.size == 1 &&
		                       source.register_id == static_cast<int>(parameter_register);
		if (system && !parameter_pristine)
		{
			return {false, instruction.pc, "fragment M0 reads an overwritten parameter-state SGPR"};
		}
		if (system)
		{
			for (const auto& label: code.GetLabels())
			{
				if (label.GetDst() <= instruction.pc)
				{
					return {false, instruction.pc, "fragment parameter-state copy must stay in the entry prefix"};
				}
			}
		}
		if (instruction.type != ShaderInstructionType::SMovB32 || instruction.format != ShaderInstructionFormat::SVdstSVsrc0 ||
		    instruction.src_num != 1 || (!constant && !system) || !Plain(source) || !Plain(instruction.dst) || instruction.dst.size != 1 ||
		    !Unused(instruction.dst2))
		{
			return {false, instruction.pc, "fragment transport requires a fixed interpolation parameter base"};
		}
		if (!initialized && saw_control)
		{
			return {false, instruction.pc, "fragment parameter base must be initialized before control flow"};
		}
		const uint32_t current = constant ? source.constant.u : 0u;
		if (initialized && (system != captured || (!system && current != base)))
		{
			return {false, instruction.pc, "fragment interpolation parameter base changes during execution"};
		}
		base        = current;
		captured    = system;
		initialized = true;
	}
	return {true, 0u, {}};
}

ShaderComputeWaveAnalysisResult ShaderAnalyzeFragmentVirtualParameterState(const ShaderCode& code, uint32_t parameter_register)
{
	if (parameter_register > 32u)
	{
		return {false, 0u, "virtual fragment parameter register is unavailable"};
	}
	const auto base = ShaderAnalyzeFragmentParameterBase(code, parameter_register);
	if (!base.supported)
	{
		return base;
	}
	bool overwritten  = false;
	bool entry_prefix = true;
	for (const auto& instruction: code.GetInstructions())
	{
		const bool copy_to_m0 = instruction.type == ShaderInstructionType::SMovB32 && instruction.dst.type == ShaderOperandType::M0 &&
		                        ScalarCovers(instruction.src[0], parameter_register) &&
		                        instruction.src[0].register_id == static_cast<int>(parameter_register) && instruction.src[0].size == 1;
		for (int source = 0; source < instruction.src_num; ++source)
		{
			if (instruction.src[source].type == ShaderOperandType::M0 ||
			    (!overwritten && ScalarCovers(instruction.src[source], parameter_register) && !(copy_to_m0 && source == 0)))
			{
				return {false, instruction.pc, "fragment parameter state escapes its virtual interpolation selector"};
			}
		}
		if ((instruction.dst.type == ShaderOperandType::M0 || instruction.dst2.type == ShaderOperandType::M0) && !copy_to_m0)
		{
			return {false, instruction.pc, "virtual fragment selector must copy the initial parameter state"};
		}
		entry_prefix &= !LeavesEntryPrefix(instruction);
		overwritten |= entry_prefix && EntryParameterOverwrite(instruction, parameter_register);
	}
	return {true, 0u, {}};
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
