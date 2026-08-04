#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_sopc)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("sopc");

	uint32_t ssrc1  = (buffer[0] >> 8u) & 0xffu;
	uint32_t ssrc0  = (buffer[0] >> 0u) & 0xffu;
	uint32_t opcode = (buffer[0] >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(ssrc0);
	inst.src[1]  = operand_parse(ssrc1);
	inst.src_num = 2;

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	if (inst.src[1].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[1].constant.u = buffer[size];
		size++;
	}

	inst.format = ShaderInstructionFormat::Ssrc0Ssrc1;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::SCmpEqI32; break;
		case 0x01: inst.type = ShaderInstructionType::SCmpLgI32; break;
		case 0x02: inst.type = ShaderInstructionType::SCmpGtI32; break;
		case 0x03: inst.type = ShaderInstructionType::SCmpGeI32; break;
		case 0x04: inst.type = ShaderInstructionType::SCmpLtI32; break;
		case 0x05: inst.type = ShaderInstructionType::SCmpLeI32; break;
		case 0x06: inst.type = ShaderInstructionType::SCmpEqU32; break;
		case 0x07: inst.type = ShaderInstructionType::SCmpLgU32; break;
		case 0x08: inst.type = ShaderInstructionType::SCmpGtU32; break;
		case 0x09: inst.type = ShaderInstructionType::SCmpGeU32; break;
		case 0x0a: inst.type = ShaderInstructionType::SCmpLtU32; break;
		case 0x0b: inst.type = ShaderInstructionType::SCmpLeU32; break;
		case 0xC: KYTY_NI("s_bitcmp0_b32"); break;
		case 0xD: KYTY_NI("s_bitcmp1_b32"); break;
		case 0xE: KYTY_NI("s_bitcmp0_b64"); break;
		case 0xF: KYTY_NI("s_bitcmp1_b64"); break;
		case 0x10: KYTY_NI("s_setvskip"); break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
