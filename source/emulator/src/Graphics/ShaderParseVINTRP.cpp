#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_vintrp)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("vintrp");

	uint32_t opcode = (buffer[0] >> 16u) & 0x3u;
	uint32_t vdst   = (buffer[0] >> 18u) & 0xffu;
	uint32_t attr   = (buffer[0] >> 10u) & 0x3fu;
	uint32_t chan   = (buffer[0] >> 8u) & 0x3u;
	uint32_t vsrc   = (buffer[0] >> 0u) & 0xffu;

	ShaderInstruction inst;
	inst.pc                = pc;
	inst.src[0]            = operand_parse(vsrc + 256);
	inst.dst               = operand_parse(vdst + 256);
	inst.src[1].type       = ShaderOperandType::IntegerInlineConstant;
	inst.src[1].constant.u = attr;
	inst.src[2].type       = ShaderOperandType::IntegerInlineConstant;
	inst.src[2].constant.u = chan;
	inst.src_num           = 3;

	inst.format = ShaderInstructionFormat::VdstVsrcAttrChan;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::VInterpP1F32; break;
		case 0x01: inst.type = ShaderInstructionType::VInterpP2F32; break;
		case 0x02:
			inst.type              = ShaderInstructionType::VInterpMovF32;
			inst.src[0].type       = ShaderOperandType::IntegerInlineConstant;
			inst.src[0].constant.u = vsrc & 0x3u;
			inst.src[0].size       = 0;
			break;
		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return 1;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
