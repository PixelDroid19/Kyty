#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_smrd)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("smrd");

	uint32_t opcode = (buffer[0] >> 22u) & 0x1fu;
	uint32_t sdst   = (buffer[0] >> 15u) & 0x7fu;
	uint32_t sbase  = (buffer[0] >> 9u) & 0x3fu;
	uint32_t imm    = (buffer[0] >> 8u) & 0x1u;
	uint32_t offset = (buffer[0] >> 0u) & 0xffu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(sdst);
	inst.src_num = 2;
	inst.src[0]  = operand_parse(sbase * 2);

	uint32_t size = 1;

	if (imm == 1)
	{
		inst.src[1].type       = ShaderOperandType::LiteralConstant;
		inst.src[1].constant.u = offset << 2u;
	} else
	{
		inst.src[1] = operand_parse(offset);

		if (inst.src[1].type == ShaderOperandType::LiteralConstant)
		{
			inst.src[1].constant.u = buffer[size];
			size++;
		}
	}

	switch (opcode)
	{
		case 0x00: KYTY_NI("s_load_dword"); break;
		case 0x01: KYTY_NI("s_load_dwordx2"); break;
		case 0x02:
			inst.type        = ShaderInstructionType::SLoadDwordx4;
			inst.format      = ShaderInstructionFormat::Sdst4SbaseSoffset;
			inst.src[0].size = 2;
			inst.dst.size    = 4;
			break;
		case 0x03:
			inst.type        = ShaderInstructionType::SLoadDwordx8;
			inst.format      = ShaderInstructionFormat::Sdst8SbaseSoffset;
			inst.src[0].size = 2;
			inst.dst.size    = 8;
			break;
		case 0x04: KYTY_NI("s_load_dwordx16"); break;
		case 0x08:
			inst.type        = ShaderInstructionType::SBufferLoadDword;
			inst.format      = ShaderInstructionFormat::SdstSvSoffset;
			inst.src[0].size = 4;
			break;
		case 0x09:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx2;
			inst.format      = ShaderInstructionFormat::Sdst2SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x0a:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx4;
			inst.format      = ShaderInstructionFormat::Sdst4SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 4;
			break;
		case 0x0b:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx8;
			inst.format      = ShaderInstructionFormat::Sdst8SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 8;
			break;
		case 0x0c:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx16;
			inst.format      = ShaderInstructionFormat::Sdst16SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 16;
			break;
		case 0x1C: KYTY_NI("s_memrealtime"); break;
		case 0x1D: KYTY_NI("s_dcache_inv_vol"); break;
		case 0x1E: KYTY_NI("s_memtime"); break;
		case 0x1F: KYTY_NI("s_dcache_inv") break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
