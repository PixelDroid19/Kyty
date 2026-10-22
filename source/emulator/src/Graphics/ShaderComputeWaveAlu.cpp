#include "Emulator/Graphics/ShaderComputeWaveAlu.h"

#include "Emulator/Graphics/ShaderComputeWaveSdwa.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr int kMaxSgpr = 105; // RDNA2 SGPR0..SGPR105
constexpr int kMaxVgpr = 255;

bool ComputeWaveAluOperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool ComputeWaveAluRegisterRangeIsValid(int register_id, int size, int maximum_register)
{
	if (register_id < 0 || size <= 0 || register_id > maximum_register)
	{
		return false;
	}

	return size <= maximum_register - register_id + 1;
}

bool ComputeWaveAluOrdinaryVgpr(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Vgpr && operand.size == 1 && ComputeWaveAluOperandIsPlain(operand) &&
	       ComputeWaveAluRegisterRangeIsValid(operand.register_id, operand.size, kMaxVgpr);
}

bool ComputeWaveAluOrdinarySgpr(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Sgpr && operand.size == 1 && ComputeWaveAluOperandIsPlain(operand) &&
	       ComputeWaveAluRegisterRangeIsValid(operand.register_id, operand.size, kMaxSgpr);
}

bool ComputeWaveAluIntegerOrLiteralConstant(const ShaderOperand& operand)
{
	return ComputeWaveAluOperandIsPlain(operand) && operand.size == 0 &&
	       (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant);
}

bool ComputeWaveAluSourceSupported(const ShaderOperand& operand)
{
	return ComputeWaveAluOrdinaryVgpr(operand) || ComputeWaveAluOrdinarySgpr(operand) || ComputeWaveAluIntegerOrLiteralConstant(operand);
}

bool ComputeWaveAluUnusedDestination(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Unknown && operand.size == 0 && ComputeWaveAluOperandIsPlain(operand);
}

// One 32-bit VCC word read as wave-uniform scalar data.
bool ComputeWaveAluVccWord(const ShaderOperand& operand)
{
	return ComputeWaveAluOperandIsPlain(operand) && operand.size == 1 && operand.register_id == 0 &&
	       (operand.type == ShaderOperandType::VccLo || operand.type == ShaderOperandType::VccHi);
}

// RDNA2 section 6.2: at most two scalar values (SGPRs, VCC, EXEC data and one
// literal) per instruction; a repeated value counts once and inline constants
// are free.
bool ComputeWaveAluScalarValuesWithinLimit(const ShaderInstruction& instruction)
{
	int  scalar_values = 0;
	bool literal       = false;
	for (int source = 0; source < instruction.src_num; ++source)
	{
		const auto& operand = instruction.src[source];
		if (operand.type == ShaderOperandType::LiteralConstant)
		{
			scalar_values += literal ? 0 : 1;
			literal = true;
			continue;
		}
		if (operand.type != ShaderOperandType::Sgpr && !ComputeWaveAluVccWord(operand))
		{
			continue;
		}
		bool repeated = false;
		for (int earlier = 0; earlier < source; ++earlier)
		{
			repeated =
			    repeated || (instruction.src[earlier].type == operand.type && instruction.src[earlier].register_id == operand.register_id);
		}
		scalar_values += repeated ? 0 : 1;
	}
	return scalar_values <= 2;
}

bool ComputeWaveAluTernarySourceSupported(const ShaderOperand& operand)
{
	return ComputeWaveAluSourceSupported(operand) || ComputeWaveAluVccWord(operand);
}

// V_CNDMASK_B32 data sources may carry float abs/neg input modifiers and may
// be float inline constants; the selection itself moves raw bits.
ShaderOperand CndmaskStripModifiers(const ShaderOperand& operand)
{
	auto stripped     = operand;
	stripped.absolute = false;
	stripped.negate   = false;
	return stripped;
}

bool ComputeWaveAluCndmaskDataSupported(const ShaderOperand& operand)
{
	const auto stripped = CndmaskStripModifiers(operand);
	return ComputeWaveAluTernarySourceSupported(stripped) ||
	       (stripped.type == ShaderOperandType::FloatInlineConstant && stripped.size == 0 && ComputeWaveAluOperandIsPlain(stripped));
}

// src2 of V_CNDMASK_B32 is a packed mask pair read per lane, never data.
bool ComputeWaveAluMaskPairSourceSupported(const ShaderOperand& operand)
{
	if (!ComputeWaveAluOperandIsPlain(operand) || operand.size != 2)
	{
		return false;
	}
	switch (operand.type)
	{
		case ShaderOperandType::Sgpr: return ComputeWaveAluRegisterRangeIsValid(operand.register_id, operand.size, kMaxSgpr);
		case ShaderOperandType::VccLo:
		case ShaderOperandType::ExecLo: return operand.register_id == 0;
		default: return false;
	}
}

bool ComputeWaveAluBinaryOpcode(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::VAndB32:
		case ShaderInstructionType::VOrB32:
		case ShaderInstructionType::VXorB32:
		case ShaderInstructionType::VAddI32:
		case ShaderInstructionType::VSubI32: return true;
		default: return false;
	}
}
} // namespace

bool ShaderComputeWaveAluInstructionSupported(const ShaderInstruction& instruction)
{
	const bool plain = instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u && !instruction.vop_sdwa &&
	                   ComputeWaveAluUnusedDestination(instruction.dst2) && ComputeWaveAluOrdinaryVgpr(instruction.dst);
	if (instruction.type == ShaderInstructionType::VAdd3U32)
	{
		return plain && instruction.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2 && instruction.src_num == 3 &&
		       ComputeWaveAluTernarySourceSupported(instruction.src[0]) && ComputeWaveAluTernarySourceSupported(instruction.src[1]) &&
		       ComputeWaveAluTernarySourceSupported(instruction.src[2]) && ComputeWaveAluScalarValuesWithinLimit(instruction);
	}
	if (instruction.type == ShaderInstructionType::VCndmaskB32)
	{
		// The identity SDWA form selects the same dwords as the plain VOP2.
		const bool identity_sdwa = instruction.vop_sdwa && ShaderComputeWaveSdwaVop2IdentitySupported(instruction) &&
		                           ComputeWaveAluUnusedDestination(instruction.dst2) && ComputeWaveAluOrdinaryVgpr(instruction.dst);
		return (plain || identity_sdwa) && instruction.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2 &&
		       instruction.src_num == 3 && ComputeWaveAluCndmaskDataSupported(instruction.src[0]) &&
		       ComputeWaveAluCndmaskDataSupported(instruction.src[1]) && ComputeWaveAluMaskPairSourceSupported(instruction.src[2]) &&
		       ComputeWaveAluUnusedDestination(instruction.src[3]) && ComputeWaveAluScalarValuesWithinLimit(instruction);
	}
	return plain && ComputeWaveAluBinaryOpcode(instruction.type) && instruction.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1 &&
	       instruction.src_num == 2 && ComputeWaveAluSourceSupported(instruction.src[0]) &&
	       ComputeWaveAluSourceSupported(instruction.src[1]);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
