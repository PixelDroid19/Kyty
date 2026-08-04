#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_sopk)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("sopk");

	uint32_t opcode = (buffer[0] >> 23u) & 0x1fu;
	auto     imm    = static_cast<int16_t>(buffer[0] >> 0u & 0xffffu);
	uint32_t sdst   = (buffer[0] >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc  = pc;
	inst.dst = operand_parse(sdst);

	inst.format            = ShaderInstructionFormat::SVdstSVsrc0;
	inst.src[0].type       = ShaderOperandType::IntegerInlineConstant;
	inst.src[0].constant.i = imm;
	inst.src_num           = 1;

	auto set_compare = [&inst](ShaderInstructionType type)
	{
		inst.src[1] = inst.src[0];
		inst.src[0] = inst.dst;
		inst.dst    = {};
		inst.type   = type;
		inst.format = ShaderInstructionFormat::Ssrc0Ssrc1;
		inst.src_num = 2;
	};

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::SMovkI32; break;

		case 0x02: KYTY_NI("s_cmovk_i32"); break;
		case 0x03: set_compare(ShaderInstructionType::SCmpEqI32); break;
		case 0x04: set_compare(ShaderInstructionType::SCmpLgI32); break;
		case 0x05: set_compare(ShaderInstructionType::SCmpGtI32); break;
		case 0x06: set_compare(ShaderInstructionType::SCmpGeI32); break;
		case 0x07: set_compare(ShaderInstructionType::SCmpLtI32); break;
		case 0x08: set_compare(ShaderInstructionType::SCmpLeI32); break;
		case 0x09: set_compare(ShaderInstructionType::SCmpEqU32); break;
		case 0x0A: set_compare(ShaderInstructionType::SCmpLgU32); break;
		case 0x0B: set_compare(ShaderInstructionType::SCmpGtU32); break;
		case 0x0C: set_compare(ShaderInstructionType::SCmpGeU32); break;
		case 0x0D: set_compare(ShaderInstructionType::SCmpLtU32); break;
		case 0x0E: set_compare(ShaderInstructionType::SCmpLeU32); break;
		case 0x0F: KYTY_NI("s_addk_i32"); break;
		case 0x10: inst.type = ShaderInstructionType::SMulkI32; break;
		case 0x11: KYTY_NI("s_cbranch_i_fork"); break;
		case 0x12: KYTY_NI("s_getreg_b32"); break;
		case 0x13: KYTY_NI("s_setreg_b32"); break;
		case 0x14: KYTY_NI("s_getreg_regrd_b32"); break;
		case 0x15: KYTY_NI("s_setreg_imm32_b32"); break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return 1;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
