#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_smem)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("smem");

	uint32_t opcode  = (buffer[0] >> 18u) & 0xffu;
	uint32_t glc     = (buffer[0] >> 16u) & 0x1u;
	uint32_t dlc     = (buffer[0] >> 14u) & 0x1u;
	uint32_t sdst    = (buffer[0] >> 6u) & 0x7fu;
	uint32_t sbase   = (buffer[0] >> 0u) & 0x3fu;
	uint32_t soffset = (buffer[1] >> 25u) & 0x7fu;
	auto     offset  = static_cast<int32_t>((buffer[1] >> 0u) & 0x1fffffu);

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(sdst);
	inst.src_num = 2;
	inst.src[0]  = operand_parse(sbase * 2);
	inst.src[1]  = operand_parse(soffset);

	uint32_t size = 2;

	EXIT_NOT_IMPLEMENTED(glc != 0);
	EXIT_NOT_IMPLEMENTED(dlc != 0);
	EXIT_NOT_IMPLEMENTED(inst.src[0].type == ShaderOperandType::LiteralConstant);
	EXIT_NOT_IMPLEMENTED(inst.src[1].type == ShaderOperandType::LiteralConstant);

	if (inst.src[1].type == ShaderOperandType::Null)
	{
		struct
		{
			int x : 21;
		} s {};

		s.x = offset;

		inst.src[1].type       = ShaderOperandType::IntegerInlineConstant;
		inst.src[1].constant.i = s.x;
		inst.src[1].size       = 0;
	} else if (offset != 0)
	{
		// Legal SMEM: SGPR/M0 soffset plus 21-bit signed immediate.
		// Captured: s_buffer_load_dwordx4 with s24 + imm 0x10.
		struct
		{
			int x : 21;
		} s {};
		s.x                  = offset;
		inst.smem_imm_offset = s.x;
	}

	switch (opcode)
	{
		case 0x00:
			inst.type        = ShaderInstructionType::SLoadDword;
			inst.format      = ShaderInstructionFormat::SdstSbaseSoffset;
			inst.src[0].size = 2;
			inst.dst.size    = 1;
			break;
		case 0x01:
			inst.type        = ShaderInstructionType::SLoadDwordx2;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
			inst.src[0].size = 2;
			inst.dst.size    = 2;
			break;
		case 0x02:
			inst.type        = ShaderInstructionType::SLoadDwordx4;
			inst.format      = ShaderInstructionFormat::Sdst4SbaseSoffset;
			inst.src[0].size = 2;
			inst.dst.size    = 4;
			break;
		case 0x03:
			// Captured post-Play Gen5 SMEM: s_load_dwordx8 s[dst:dst+7], s[base:base+1], offset
			// at shader PC 0x18 (hash0 0x210003f0). Recompiler already handles x8 via
			// Sdst8SbaseSoffset / recompile_sload_from_extended.
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
			inst.dst.size    = 1;
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
		case 0x0B:
			// Gen5 SMEM mirrors GCN SMRD encoding: s_buffer_load_dwordx8 s[dst:dst+7], s[base:base+3], offset
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

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
