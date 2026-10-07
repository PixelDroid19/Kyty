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
	// RDNA2 table 96 splits OP across bit 53 and bits 18:16. Legacy GCN
	// has a three-bit opcode and reserves bit 53; it is not a D16 selector.
	if (next_gen) { opcode |= ((buffer[1] >> 21u) & 1u) << 3u; }
	uint32_t dfmt   = (buffer[0] >> 19u) & 0xfu;
	uint32_t nfmt   = (buffer[0] >> 23u) & 0x7u;
	uint32_t bit15  = (buffer[0] >> 15u) & 0x1u;
	uint32_t idxen  = (buffer[0] >> 13u) & 0x1u;
	uint32_t offen  = (buffer[0] >> 12u) & 0x1u;
	uint32_t offset = (buffer[0] >> 0u) & 0xfffu;

	uint32_t soffset = (buffer[1] >> 24u) & 0xffu;
	uint32_t tfe     = (buffer[1] >> 23u) & 0x1u;
	uint32_t slc     = (buffer[1] >> 22u) & 0x1u;
	uint32_t srsrc   = (buffer[1] >> 16u) & 0x1fu;
	uint32_t vdata   = (buffer[1] >> 8u) & 0xffu;
	uint32_t vaddr   = (buffer[1] >> 0u) & 0xffu;

	// No ordinary load or barrier is a substitute for XYZ, stores, or packed
	// D16 results. Reject unsupported opcodes before constructing their tuple.
	if (opcode != 0u && opcode != 1u && opcode != 3u) { KYTY_UNKNOWN_OP(); }

	// GCN and Gen5 encode the scalar 32-bit float typed-buffer view differently:
	// legacy shaders use (4, 7), while Gen5 uses the packed BufferFormat value
	// 0x16, split as dfmt=6/nfmt=1. Both feed the same Float1 IR contract.
	const uint32_t encoded_format = (nfmt << 4u) | dfmt;
	const bool float1_format = (!next_gen && dfmt == 4u && nfmt == 7u) || (next_gen && encoded_format == 22u);
	const bool float2_format = (!next_gen && dfmt == 11u && nfmt == 7u) || (next_gen && encoded_format == 64u);
	const bool float4_format = (!next_gen && dfmt == 14u && nfmt == 7u) || (next_gen && encoded_format == 77u);
	if ((opcode == 0u && !float1_format) || (opcode == 1u && !float2_format) || (opcode == 3u && !float4_format))
	{
		EXIT("unsupported mtbuf format/component tuple: dfmt = %u, nfmt = %u, opcode = 0x%02" PRIx32 ", word0 = 0x%08" PRIx32
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
	const uint32_t dlc = next_gen ? bit15 : 0u;
	const uint32_t unmodeled = next_gen ? 0u : (bit15 | ((buffer[1] >> 21u) & 1u));
	inst.buffer_flags = static_cast<uint8_t>((slc << 1u) | (tfe << 2u) | (dlc << 3u) | (unmodeled << 7u));
	inst.mtbuf_format         = static_cast<uint8_t>(encoded_format);
	inst.mtbuf_components     = static_cast<uint8_t>((opcode & 3u) + 1u);
	inst.mtbuf_format_is_gen5 = next_gen;
	inst.src[0].size = idxen == 1 && offen == 1 ? 2 : 1;

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
			break;
		case 0x01:
			inst.type   = ShaderInstructionType::TBufferLoadFormatXy;
			inst.format = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxenFloat2;
			inst.dst.size = 2;
			break;
		case 0x03:
			inst.type   = ShaderInstructionType::TBufferLoadFormatXyzw;
			inst.format = (idxen == 1 && offen == 1 ? ShaderInstructionFormat::Vdata4Vaddr2SvSoffsOffenIdxenFloat4
			                                        : ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxenFloat4);
			inst.dst.size = 4;
			break;
		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
