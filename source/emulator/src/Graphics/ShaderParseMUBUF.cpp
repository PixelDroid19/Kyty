#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_mubuf)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("mubuf");

	uint32_t opcode = (buffer[0] >> 18u) & 0xffu;
	uint32_t lds    = (buffer[0] >> 16u) & 0x1u;
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

	EXIT_NOT_IMPLEMENTED(glc == 1 && opcode != 0x32u);
	EXIT_NOT_IMPLEMENTED(slc == 1);
	EXIT_NOT_IMPLEMENTED(lds == 1);
	EXIT_NOT_IMPLEMENTED(tfe == 1);

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
	inst.buffer_return_old_value = glc == 1;
	inst.src[0].size += static_cast<int>(offen);

	if (inst.src[2].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[2].constant.u = buffer[size];
		size++;
	}

	// Legacy recompiler paths still encode the immediate in the scalar offset.
	// Gen5 takes the split path above so the immediate remains inside the
	// descriptor swizzle and S_OFFSET remains outside it.
	if (!next_gen && offset != 0u)
	{
		if (inst.src[2].type == ShaderOperandType::IntegerInlineConstant)
		{
			inst.src[2].type       = ShaderOperandType::LiteralConstant;
			inst.src[2].constant.u = static_cast<uint32_t>(inst.src[2].constant.i) + offset;
			inst.src[2].size       = 0;
		} else if (inst.src[2].type == ShaderOperandType::LiteralConstant)
		{
			inst.src[2].constant.u += offset;
		} else
		{
			EXIT_NOT_IMPLEMENTED(true);
		}
		inst.buffer_imm_offset = 0;
	}

	switch (opcode)
	{
		case 0x00:
			inst.type        = ShaderInstructionType::BufferLoadFormatX;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x01:
			inst.type        = ShaderInstructionType::BufferLoadFormatXy;
			inst.format      = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x02:
			inst.type        = ShaderInstructionType::BufferLoadFormatXyz;
			inst.format      = ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 3;
			break;
		case 0x03:
			inst.type        = ShaderInstructionType::BufferLoadFormatXyzw;
			inst.format      = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 4;
			break;
		case 0x04:
			inst.type        = ShaderInstructionType::BufferStoreFormatX;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x05:
			inst.type        = ShaderInstructionType::BufferStoreFormatXy;
			inst.format      = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x06: KYTY_NI("buffer_store_format_xyz"); break;
		case 0x07:
			inst.type        = ShaderInstructionType::BufferStoreFormatXyzw;
			inst.format      = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 4;
			break;
		case 0x08:
			inst.type        = ShaderInstructionType::BufferLoadUbyte;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x09: KYTY_NI("buffer_load_sbyte"); break;
		case 0x0A: KYTY_NI("buffer_load_ushort"); break;
		case 0x0B: KYTY_NI("buffer_load_sshort"); break;
		case 0x0c:
			inst.type = ShaderInstructionType::BufferLoadDword;
			inst.format =
			    (offen == 1 ? ShaderInstructionFormat::Vdata1Vaddr2SvSoffsOffenIdxen : ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen);
			inst.src[1].size = 4;
			break;
		case 0x0D:
			inst.type        = ShaderInstructionType::BufferLoadDwordx2;
			inst.format      = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x0E:
			inst.type        = ShaderInstructionType::BufferLoadDwordx4;
			inst.format      = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 4;
			break;
		case 0x0F:
			inst.type        = ShaderInstructionType::BufferLoadDwordx3;
			inst.format      = ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 3;
			break;
		case 0x18: KYTY_NI("buffer_store_byte"); break;
		case 0x1A: KYTY_NI("buffer_store_short"); break;
		case 0x1c:
			inst.type        = ShaderInstructionType::BufferStoreDword;
			inst.format      = (offen == 1 ? ShaderInstructionFormat::Vdata1VaddrSvSoffsOffen
			                              : ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen);
			inst.src[1].size = 4;
			break;
		case 0x1D:
			inst.type        = ShaderInstructionType::BufferStoreDwordx2;
			inst.format      = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x1E:
			inst.type        = ShaderInstructionType::BufferStoreDwordx4;
			inst.format      = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 4;
			break;
		case 0x1F:
			inst.type        = ShaderInstructionType::BufferStoreDwordx3;
			inst.format      = ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 3;
			break;
		case 0x30: KYTY_NI("buffer_atomic_swap"); break;
		case 0x31: KYTY_NI("buffer_atomic_cmpswap"); break;
		case 0x32:
			inst.type        = ShaderInstructionType::BufferAtomicAdd;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x33: KYTY_NI("buffer_atomic_sub"); break;
		case 0x34:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("buffer_atomic_rsub")
			};
			break;
		case 0x35: KYTY_NI("buffer_atomic_smin"); break;
		case 0x36: KYTY_NI("buffer_atomic_umin"); break;
		case 0x37: KYTY_NI("buffer_atomic_smax"); break;
		case 0x38: KYTY_NI("buffer_atomic_umax"); break;
		case 0x39: KYTY_NI("buffer_atomic_and"); break;
		case 0x3A: KYTY_NI("buffer_atomic_or"); break;
		case 0x3B: KYTY_NI("buffer_atomic_xor"); break;
		case 0x3C: KYTY_NI("buffer_atomic_inc"); break;
		case 0x3D: KYTY_NI("buffer_atomic_dec"); break;
		case 0x3E: KYTY_NI("buffer_atomic_fcmpswap"); break;
		case 0x3F: KYTY_NI("buffer_atomic_fmin"); break;
		case 0x40: KYTY_NI("buffer_atomic_fmax"); break;
		case 0x50: KYTY_NI("buffer_atomic_swap_x2"); break;
		case 0x51: KYTY_NI("buffer_atomic_cmpswap_x2"); break;
		case 0x52: KYTY_NI("buffer_atomic_add_x2"); break;
		case 0x53: KYTY_NI("buffer_atomic_sub_x2"); break;
		case 0x54: KYTY_NI("buffer_atomic_rsub_x2"); break;
		case 0x55: KYTY_NI("buffer_atomic_smin_x2"); break;
		case 0x56: KYTY_NI("buffer_atomic_umin_x2"); break;
		case 0x57: KYTY_NI("buffer_atomic_smax_x2"); break;
		case 0x58: KYTY_NI("buffer_atomic_umax_x2"); break;
		case 0x59: KYTY_NI("buffer_atomic_and_x2"); break;
		case 0x5A: KYTY_NI("buffer_atomic_or_x2"); break;
		case 0x5B: KYTY_NI("buffer_atomic_xor_x2"); break;
		case 0x5C: KYTY_NI("buffer_atomic_inc_x2"); break;
		case 0x5D: KYTY_NI("buffer_atomic_dec_x2"); break;
		case 0x5E: KYTY_NI("buffer_atomic_fcmpswap_x2"); break;
		case 0x5F: KYTY_NI("buffer_atomic_fmin_x2"); break;
		case 0x60: KYTY_NI("buffer_atomic_fmax_x2"); break;
		case 0x71:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("buffer_wbinvl1")
			};
			break;
		case 0x80: KYTY_NI("buffer_load_format_d16_x"); break;
		case 0x81: KYTY_NI("buffer_load_format_d16_xy"); break;
		case 0x82: KYTY_NI("buffer_load_format_d16_xyz"); break;
		case 0x83: KYTY_NI("buffer_load_format_d16_xyzw"); break;
		case 0x84: KYTY_NI("buffer_store_format_d16_x"); break;
		case 0x85: KYTY_NI("buffer_store_format_d16_xy"); break;
		case 0x86: KYTY_NI("buffer_store_format_d16_xyz"); break;
		case 0x87: KYTY_NI("buffer_store_format_d16_xyzw"); break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
