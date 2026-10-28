#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_vop3p)
{
	EXIT_IF(dst == nullptr || src == nullptr || buffer == nullptr || buffer < src);
	KYTY_TYPE_STR("vop3p");
	const uint32_t opcode = (buffer[0] >> 16u) & 0x7fu;
	// Packed half FMA, ADD, MUL, MIN and MAX (opcodes 14-18) and the mixed FMA (32-34).
	const bool     packed = opcode >= 0xeu && opcode <= 0x12u;
	if (!next_gen || (buffer[0] & 0xff800000u) != 0xcc000000u ||
	    (opcode != 0x20u && opcode != 0x21u && opcode != 0x22u && !packed))
	{
		KYTY_UNKNOWN_OP();
	}

	ShaderInstruction inst;
	inst.pc = pc;
	inst.type = opcode == 0xeu  ? ShaderInstructionType::VPkFmaF16 :
	            opcode == 0xfu  ? ShaderInstructionType::VPkAddF16 :
	            opcode == 0x10u ? ShaderInstructionType::VPkMulF16 :
	            opcode == 0x11u ? ShaderInstructionType::VPkMinF16 :
	            opcode == 0x12u ? ShaderInstructionType::VPkMaxF16 :
	            opcode == 0x20u ? ShaderInstructionType::VFmaMixF32 :
	            opcode == 0x21u ? ShaderInstructionType::VFmaMixLoF16 :
	                              ShaderInstructionType::VFmaMixHiF16;
	const bool binary = opcode >= 0xfu && opcode <= 0x12u;
	inst.format = binary ? ShaderInstructionFormat::SVdstSVsrc0SVsrc1 : ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
	inst.dst = operand_parse(256u + (buffer[0] & 0xffu));
	inst.dst.clamp = ((buffer[0] >> 15u) & 1u) != 0u;
	inst.src_num = binary ? 2 : 3;
	inst.vop3_op_sel = static_cast<uint8_t>((buffer[0] >> 11u) & 7u);
	inst.vop3p_op_sel_hi = static_cast<uint8_t>(((buffer[1] >> 27u) & 3u) | ((buffer[0] >> 12u) & 4u));
	bool has_literal = false;
	for (uint32_t source = 0; source < static_cast<uint32_t>(inst.src_num); ++source)
	{
		const uint32_t encoded = (buffer[1] >> (source * 9u)) & 0x1ffu;
		auto& operand = inst.src[source];
		operand = operand_parse(encoded);
		// NEG_HI: the high result's negation for packed math, an absolute value for the mixed FMA.
		operand.absolute = ((buffer[0] >> (8u + source)) & 1u) != 0u;
		operand.negate = ((buffer[1] >> (29u + source)) & 1u) != 0u;
		if (encoded == 255u)
		{
			operand.constant.u = buffer[2];
			has_literal = true;
		}
		// The ISA does not give the inline-constant contract of packed math; its emitter refuses constants.
		if (packed)
		{
			continue;
		}
		// Inline floats have the selected precision before OPSEL selects a
		// half. They are not the low bits of their FP32 representation.
		if (encoded >= 240u && encoded <= 248u && (inst.vop3p_op_sel_hi & (1u << source)) != 0u)
		{
			// Upper-half selection of inline FP16 constants needs a separate
			// encoding contract; keep that unverified form outside admission.
			if ((inst.vop3_op_sel & (1u << source)) != 0u)
			{
				KYTY_UNKNOWN_OP();
			}
			static constexpr uint32_t half_bits[] = {0x3800u, 0xb800u, 0x3c00u, 0xbc00u, 0x4000u,
			                                          0xc000u, 0x4400u, 0xc400u, 0x3118u};
			operand.type = ShaderOperandType::LiteralConstant;
			operand.constant.u = half_bits[encoded - 240u];
		}
	}
	dst->GetInstructions().Add(inst);
	return has_literal ? 3u : 2u;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
