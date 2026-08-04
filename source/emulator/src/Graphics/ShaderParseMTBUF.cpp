#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_mtbuf)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("mtbuf");

	uint32_t opcode = (buffer[0] >> 16u) & 0x7u;
	uint32_t dfmt   = (buffer[0] >> 19u) & 0xfu;
	uint32_t nfmt   = (buffer[0] >> 23u) & 0x7u;
	uint32_t glc    = (buffer[0] >> 14u) & 0x1u;
	uint32_t idxen  = (buffer[0] >> 13u) & 0x1u;
	uint32_t offen  = (buffer[0] >> 12u) & 0x1u;
	uint32_t offset = (buffer[0] >> 0u) & 0xfffu;

	uint32_t soffset = (buffer[1] >> 24u) & 0xffu;
	uint32_t tfe     = (buffer[1] >> 23u) & 0x1u;
	uint32_t slc     = (buffer[1] >> 22u) & 0x1u;
	uint32_t srsrc   = (buffer[1] >> 16u) & 0x1fu;
	uint32_t vdata   = (buffer[1] >> 8u) & 0xffu;
	uint32_t vaddr   = (buffer[1] >> 0u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(glc == 1);
	EXIT_NOT_IMPLEMENTED(slc == 1);
	EXIT_NOT_IMPLEMENTED(tfe == 1);
	// EXIT_NOT_IMPLEMENTED(dfmt != 14);
	// EXIT_NOT_IMPLEMENTED(nfmt != 7);

	// GCN and Gen5 encode the scalar 32-bit float typed-buffer view differently:
	// legacy shaders use (4, 7), while Gen5 uses the packed BufferFormat value
	// 0x16, split as dfmt=6/nfmt=1. Both feed the same Float1 IR contract.
	const uint32_t encoded_format = (nfmt << 4u) | dfmt;
	const bool float1_format = (!next_gen && dfmt == 4u && nfmt == 7u) || (next_gen && encoded_format == 22u);
	const bool float2_format = (!next_gen && dfmt == 11u && nfmt == 7u) || (next_gen && encoded_format == 64u);
	const bool float4_format = (!next_gen && dfmt == 14u && nfmt == 7u) || (next_gen && encoded_format == 77u);
	if (!float1_format && !float2_format && !float4_format)
	{
		EXIT("unknown format: dfmt = %d, nfmt = %d, opcode = 0x%02" PRIx32 ", word0 = 0x%08" PRIx32
		     " at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ", crc32 = 0x%08" PRIx32 ")\n",
		     dfmt, nfmt, opcode, buffer[0], pc, dst->GetHash0(), dst->GetCrc32());
	}

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(vdata + 256);
	inst.src_num = 3;
	inst.src[0]  = operand_parse(vaddr + 256);
	inst.src[1]  = operand_parse(srsrc * 4);
	inst.src[2]  = operand_parse(soffset);
	inst.buffer_imm_offset = static_cast<uint16_t>(offset);
	inst.buffer_idxen      = idxen == 1;
	inst.buffer_offen      = offen == 1;
	inst.src[0].size += static_cast<int>(offen);

	if (inst.src[2].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[2].constant.u = buffer[size];
		size++;
	}

	inst.src[1].size = 4;

	switch (opcode)
	{
		case 0x00:
			inst.type   = ShaderInstructionType::TBufferLoadFormatX;
			inst.format = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxenFloat1;
			EXIT_NOT_IMPLEMENTED(!float1_format);
			break;
		case 0x01:
			inst.type   = ShaderInstructionType::TBufferLoadFormatXy;
			inst.format = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxenFloat2;
			inst.dst.size = 2;
			EXIT_NOT_IMPLEMENTED(!float2_format);
			break;
		case 0x02: KYTY_NI("tbuffer_load_format_xyz"); break;
		case 0x03:
			inst.type   = ShaderInstructionType::TBufferLoadFormatXyzw;
			inst.format = (offen == 1 ? ShaderInstructionFormat::Vdata4Vaddr2SvSoffsOffenIdxenFloat4
			                          : ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxenFloat4);
			inst.dst.size = 4;
			EXIT_NOT_IMPLEMENTED(!float4_format);
			break;
		case 0x04: KYTY_NI("tbuffer_store_format_x"); break;
		case 0x05: KYTY_NI("tbuffer_store_format_xy"); break;
		case 0x06: KYTY_NI("tbuffer_store_format_xyz"); break;
		case 0x07: KYTY_NI("tbuffer_store_format_xyzw"); break;
		case 0x08: KYTY_NI("tbuffer_load_format_d16_x"); break;
		case 0x09: KYTY_NI("tbuffer_load_format_d16_xy"); break;
		case 0x0A: KYTY_NI("tbuffer_load_format_d16_xyz"); break;
		case 0x0B: KYTY_NI("tbuffer_load_format_d16_xyzw"); break;
		case 0x0C: KYTY_NI("tbuffer_store_format_d16_x"); break;
		case 0x0D: KYTY_NI("tbuffer_store_format_d16_xy"); break;
		case 0x0E: KYTY_NI("tbuffer_store_format_d16_xyz"); break;
		case 0x0F: KYTY_NI("tbuffer_store_format_d16_xyzw"); break;
		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
