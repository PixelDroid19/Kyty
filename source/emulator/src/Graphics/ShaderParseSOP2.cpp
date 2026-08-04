#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_sop2)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("sop2");

	uint32_t opcode = (buffer[0] >> 23u) & 0x7fu;

	switch (opcode)
	{
		case 0x7d: return shader_parse_sop1(pc, src, buffer, dst, next_gen); break;
		case 0x7e: return shader_parse_sopc(pc, src, buffer, dst, next_gen); break;
		case 0x7f: return shader_parse_sopp(pc, src, buffer, dst, next_gen); break;
		default: break;
	}

	if (opcode >= 0x60)
	{
		return shader_parse_sopk(pc, src, buffer, dst, next_gen);
	}

	uint32_t ssrc1 = (buffer[0] >> 8u) & 0xffu;
	uint32_t ssrc0 = (buffer[0] >> 0u) & 0xffu;
	uint32_t sdst  = (buffer[0] >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(ssrc0);
	inst.src[1]  = operand_parse(ssrc1);
	inst.src_num = 2;
	inst.dst     = operand_parse(sdst);

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

	inst.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::SAddU32; break;
		// s_sub_u32: two's-complement subtract — identical result bits to the signed
		// s_sub_i32; only the SCC borrow-flag semantics differ (unused here).
		case 0x01: inst.type = ShaderInstructionType::SSubI32; break;
		case 0x02: inst.type = ShaderInstructionType::SAddI32; break;
		case 0x03: inst.type = ShaderInstructionType::SSubI32; break;
		case 0x04: inst.type = ShaderInstructionType::SAddcU32; break;
		case 0x05: KYTY_NI("s_subb_u32"); break;
		case 0x06: KYTY_NI("s_min_i32"); break;
		case 0x07: KYTY_NI("s_min_u32"); break;
		case 0x08: KYTY_NI("s_max_i32"); break;
		case 0x09: KYTY_NI("s_max_u32"); break;
		case 0x0a: inst.type = ShaderInstructionType::SCselectB32; break;
		case 0x0b:
			inst.type        = ShaderInstructionType::SCselectB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x0e: inst.type = ShaderInstructionType::SAndB32; break;
		case 0x0f:
			inst.type        = ShaderInstructionType::SAndB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x10: inst.type = ShaderInstructionType::SOrB32; break;
		case 0x11:
			inst.type        = ShaderInstructionType::SOrB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x12: KYTY_NI("s_xor_b32"); break;
		case 0x13:
			inst.type        = ShaderInstructionType::SXorB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x14: KYTY_NI("s_andn2_b32"); break;
		case 0x15:
			inst.type        = ShaderInstructionType::SAndn2B64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x16: KYTY_NI("s_orn2_b32"); break;
		case 0x17:
			inst.type        = ShaderInstructionType::SOrn2B64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x18: KYTY_NI("s_nand_b32"); break;
		case 0x19:
			inst.type        = ShaderInstructionType::SNandB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x1A: KYTY_NI("s_nor_b32"); break;
		case 0x1b:
			inst.type        = ShaderInstructionType::SNorB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x1C: KYTY_NI("s_xnor_b32"); break;
		case 0x1d:
			inst.type        = ShaderInstructionType::SXnorB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x1e: inst.type = ShaderInstructionType::SLshlB32; break;
		case 0x1f:
			inst.type        = ShaderInstructionType::SLshlB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x20: inst.type = ShaderInstructionType::SLshrB32; break;
		case 0x21:
			inst.type        = ShaderInstructionType::SLshrB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x22: KYTY_NI("s_ashr_i32"); break;
		case 0x23: KYTY_NI("s_ashr_i64"); break;
		case 0x24: inst.type = ShaderInstructionType::SBfmB32; break;
		case 0x25: KYTY_NI("s_bfm_b64"); break;
		case 0x26: inst.type = ShaderInstructionType::SMulI32; break;
		case 0x27: inst.type = ShaderInstructionType::SBfeU32; break;
		case 0x28: KYTY_NI("s_bfe_i32"); break;
		case 0x29:
			inst.type        = ShaderInstructionType::SBfeU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x2A: KYTY_NI("s_bfe_i64"); break;
		case 0x2B: KYTY_NI("s_cbranch_g_fork"); break;
		case 0x2C: KYTY_NI("s_absdiff_i32"); break;
		case 0x31:
			EXIT_NOT_IMPLEMENTED(!next_gen);
			inst.type = ShaderInstructionType::SLshl4AddU32;
			break;
		case 0x32: KYTY_NI("s_pack_ll_b32_b16"); break;
		case 0x33: KYTY_NI("s_pack_lh_b32_b16"); break;
		case 0x34: KYTY_NI("s_pack_hh_b32_b16"); break;
		case 0x35: inst.type = ShaderInstructionType::SMulHiU32; break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size,google-readability-function-size,hicpp-function-size)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
