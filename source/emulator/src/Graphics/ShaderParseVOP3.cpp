#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

static bool is_gen5_vop3b(uint32_t opcode, bool next_gen)
{
	return next_gen && ((opcode >= 0x128u && opcode <= 0x12au) || opcode == 0x176u || opcode == 0x177u);
}

KYTY_SHADER_PARSER(shader_parse_vop3)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("vop3");

	uint32_t   opcode         = (next_gen ? (buffer[0] >> 16u) & 0x3ffu : (buffer[0] >> 17u) & 0x1ffu);
	uint32_t   clamp          = (next_gen ? (buffer[0] >> 15u) & 0x1u : (buffer[0] >> 11u) & 0x1u);
	const bool is_vop3b = is_gen5_vop3b(opcode, next_gen);
	uint32_t   op_sel   = (next_gen && !is_vop3b ? (buffer[0] >> 11u) & 0xfu : 0);
	uint32_t   abs      = (is_vop3b ? 0 : (buffer[0] >> 8u) & 0x7u);
	uint32_t   vdst           = (buffer[0] >> 0u) & 0xffu;
	uint32_t   sdst           = (buffer[0] >> 8u) & 0x7fu;
	uint32_t   neg            = (buffer[1] >> 29u) & 0x7u;
	uint32_t   omod           = (buffer[1] >> 27u) & 0x3u;
	uint32_t   src0           = (buffer[1] >> 0u) & 0x1ffu;
	uint32_t   src1           = (buffer[1] >> 9u) & 0x1ffu;
	uint32_t   src2           = (buffer[1] >> 18u) & 0x1ffu;

	EXIT_NOT_IMPLEMENTED(op_sel != 0);

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0);
	inst.src[1]  = operand_parse(src1);
	inst.src[2]  = operand_parse(src2);
	inst.src_num = 3;
	inst.dst     = operand_parse(vdst + 256);

	switch (omod)
	{
		case 0: inst.dst.multiplier = 1.0f; break;
		case 1: inst.dst.multiplier = 2.0f; break;
		case 2: inst.dst.multiplier = 4.0f; break;
		case 3: inst.dst.multiplier = 0.5f; break;
		default: break;
	}

	if ((neg & 0x1u) != 0)
	{
		inst.src[0].negate = true;
	}
	if ((neg & 0x2u) != 0)
	{
		inst.src[1].negate = true;
	}
	if ((neg & 0x4u) != 0)
	{
		inst.src[2].negate = true;
	}

	uint32_t size = 2;

	const bool has_literal = inst.src[0].type == ShaderOperandType::LiteralConstant ||
	                         inst.src[1].type == ShaderOperandType::LiteralConstant ||
	                         inst.src[2].type == ShaderOperandType::LiteralConstant;
	if (has_literal)
	{
		const uint32_t literal = buffer[size];
		for (auto& operand: inst.src)
		{
			if (operand.type == ShaderOperandType::LiteralConstant)
			{
				operand.constant.u = literal;
			}
		}
		size++;
	}

	inst.format = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;

	if (opcode >= 0 && opcode <= 0xff)
	{
		/* VOPC using VOP3 encoding */
		inst.format   = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
		inst.src_num  = 2;
		inst.dst      = operand_parse(vdst);
		inst.dst.size = 2;
	}

	if (opcode >= 0x100 && opcode <= 0x13d)
	{
		/* VOP2 using VOP3 encoding */
		inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
		inst.src_num = 2;
	}

	if (opcode >= 0x180 && opcode <= 0x1e8)
	{
		/* VOP1 using VOP3 encoding */
		inst.format  = ShaderInstructionFormat::SVdstSVsrc0;
		inst.src_num = 1;
	}

	switch (opcode)
	{
		/* VOPC using VOP3 encoding */
		case 0x00: inst.type = ShaderInstructionType::VCmpFF32; break;
		case 0x01: inst.type = ShaderInstructionType::VCmpLtF32; break;
		case 0x02: inst.type = ShaderInstructionType::VCmpEqF32; break;
		case 0x03: inst.type = ShaderInstructionType::VCmpLeF32; break;
		case 0x04: inst.type = ShaderInstructionType::VCmpGtF32; break;
		case 0x05: inst.type = ShaderInstructionType::VCmpLgF32; break;
		case 0x06: inst.type = ShaderInstructionType::VCmpGeF32; break;
		case 0x07: inst.type = ShaderInstructionType::VCmpOF32; break;
		case 0x08: inst.type = ShaderInstructionType::VCmpUF32; break;
		case 0x09: inst.type = ShaderInstructionType::VCmpNgeF32; break;
		case 0x0a: inst.type = ShaderInstructionType::VCmpNlgF32; break;
		case 0x0b: inst.type = ShaderInstructionType::VCmpNgtF32; break;
		case 0x0c: inst.type = ShaderInstructionType::VCmpNleF32; break;
		case 0x0d: inst.type = ShaderInstructionType::VCmpNeqF32; break;
		case 0x0e: inst.type = ShaderInstructionType::VCmpNltF32; break;
		case 0x0f: inst.type = ShaderInstructionType::VCmpTruF32; break;
		case 0x10: inst.type = ShaderInstructionType::VCmpxFF32; break;
		case 0x11: inst.type = ShaderInstructionType::VCmpxLtF32; break;
		case 0x12: inst.type = ShaderInstructionType::VCmpxEqF32; break;
		case 0x13: inst.type = ShaderInstructionType::VCmpxLeF32; break;
		case 0x14: inst.type = ShaderInstructionType::VCmpxGtF32; break;
		case 0x15: inst.type = ShaderInstructionType::VCmpxLgF32; break;
		case 0x16: inst.type = ShaderInstructionType::VCmpxGeF32; break;
		case 0x17: inst.type = ShaderInstructionType::VCmpxOF32; break;
		case 0x18: inst.type = ShaderInstructionType::VCmpxUF32; break;
		case 0x19: inst.type = ShaderInstructionType::VCmpxNgeF32; break;
		case 0x1A: inst.type = ShaderInstructionType::VCmpxNlgF32; break;
		case 0x1B: inst.type = ShaderInstructionType::VCmpxNgtF32; break;
		case 0x1C: inst.type = ShaderInstructionType::VCmpxNleF32; break;
		case 0x1d: inst.type = ShaderInstructionType::VCmpxNeqF32; break;
		case 0x1E: inst.type = ShaderInstructionType::VCmpxNltF32; break;
		case 0x1F: inst.type = ShaderInstructionType::VCmpxTruF32; break;
		case 0x20: KYTY_NI("v_cmp_f_f64"); break;
		case 0x21: KYTY_NI("v_cmp_lt_f64"); break;
		case 0x22: KYTY_NI("v_cmp_eq_f64"); break;
		case 0x23: KYTY_NI("v_cmp_le_f64"); break;
		case 0x24: KYTY_NI("v_cmp_gt_f64"); break;
		case 0x25: KYTY_NI("v_cmp_lg_f64"); break;
		case 0x26: KYTY_NI("v_cmp_ge_f64"); break;
		case 0x27: KYTY_NI("v_cmp_o_f64"); break;
		case 0x28: KYTY_NI("v_cmp_u_f64"); break;
		case 0x29: KYTY_NI("v_cmp_nge_f64"); break;
		case 0x2A: KYTY_NI("v_cmp_nlg_f64"); break;
		case 0x2B: KYTY_NI("v_cmp_ngt_f64"); break;
		case 0x2C: KYTY_NI("v_cmp_nle_f64"); break;
		case 0x2D: KYTY_NI("v_cmp_neq_f64"); break;
		case 0x2E: KYTY_NI("v_cmp_nlt_f64"); break;
		case 0x2F: KYTY_NI("v_cmp_tru_f64"); break;
		case 0x30: KYTY_NI("v_cmpx_f_f64"); break;
		case 0x31: KYTY_NI("v_cmpx_lt_f64"); break;
		case 0x32: KYTY_NI("v_cmpx_eq_f64"); break;
		case 0x33: KYTY_NI("v_cmpx_le_f64"); break;
		case 0x34: KYTY_NI("v_cmpx_gt_f64"); break;
		case 0x35: KYTY_NI("v_cmpx_lg_f64"); break;
		case 0x36: KYTY_NI("v_cmpx_ge_f64"); break;
		case 0x37: KYTY_NI("v_cmpx_o_f64"); break;
		case 0x38: KYTY_NI("v_cmpx_u_f64"); break;
		case 0x39: KYTY_NI("v_cmpx_nge_f64"); break;
		case 0x3A: KYTY_NI("v_cmpx_nlg_f64"); break;
		case 0x3B: KYTY_NI("v_cmpx_ngt_f64"); break;
		case 0x3C: KYTY_NI("v_cmpx_nle_f64"); break;
		case 0x3D: KYTY_NI("v_cmpx_neq_f64"); break;
		case 0x3E: KYTY_NI("v_cmpx_nlt_f64"); break;
		case 0x3F: KYTY_NI("v_cmpx_tru_f64"); break;
		case 0x40: KYTY_NI("v_cmps_f_f32"); break;
		case 0x41: KYTY_NI("v_cmps_lt_f32"); break;
		case 0x42: KYTY_NI("v_cmps_eq_f32"); break;
		case 0x43: KYTY_NI("v_cmps_le_f32"); break;
		case 0x44: KYTY_NI("v_cmps_gt_f32"); break;
		case 0x45: KYTY_NI("v_cmps_lg_f32"); break;
		case 0x46: KYTY_NI("v_cmps_ge_f32"); break;
		case 0x47: KYTY_NI("v_cmps_o_f32"); break;
		case 0x48: KYTY_NI("v_cmps_u_f32"); break;
		case 0x49: KYTY_NI("v_cmps_nge_f32"); break;
		case 0x4A: KYTY_NI("v_cmps_nlg_f32"); break;
		case 0x4B: KYTY_NI("v_cmps_ngt_f32"); break;
		case 0x4C: KYTY_NI("v_cmps_nle_f32"); break;
		case 0x4D: KYTY_NI("v_cmps_neq_f32"); break;
		case 0x4E: KYTY_NI("v_cmps_nlt_f32"); break;
		case 0x4F: KYTY_NI("v_cmps_tru_f32"); break;
		case 0x50: KYTY_NI("v_cmpsx_f_f32"); break;
		case 0x51: KYTY_NI("v_cmpsx_lt_f32"); break;
		case 0x52: KYTY_NI("v_cmpsx_eq_f32"); break;
		case 0x53: KYTY_NI("v_cmpsx_le_f32"); break;
		case 0x54: KYTY_NI("v_cmpsx_gt_f32"); break;
		case 0x55: KYTY_NI("v_cmpsx_lg_f32"); break;
		case 0x56: KYTY_NI("v_cmpsx_ge_f32"); break;
		case 0x57: KYTY_NI("v_cmpsx_o_f32"); break;
		case 0x58: KYTY_NI("v_cmpsx_u_f32"); break;
		case 0x59: KYTY_NI("v_cmpsx_nge_f32"); break;
		case 0x5A: KYTY_NI("v_cmpsx_nlg_f32"); break;
		case 0x5B: KYTY_NI("v_cmpsx_ngt_f32"); break;
		case 0x5C: KYTY_NI("v_cmpsx_nle_f32"); break;
		case 0x5D: KYTY_NI("v_cmpsx_neq_f32"); break;
		case 0x5E: KYTY_NI("v_cmpsx_nlt_f32"); break;
		case 0x5F: KYTY_NI("v_cmpsx_tru_f32"); break;
		case 0x60: KYTY_NI("v_cmps_f_f64"); break;
		case 0x61: KYTY_NI("v_cmps_lt_f64"); break;
		case 0x62: KYTY_NI("v_cmps_eq_f64"); break;
		case 0x63: KYTY_NI("v_cmps_le_f64"); break;
		case 0x64: KYTY_NI("v_cmps_gt_f64"); break;
		case 0x65: KYTY_NI("v_cmps_lg_f64"); break;
		case 0x66: KYTY_NI("v_cmps_ge_f64"); break;
		case 0x67: KYTY_NI("v_cmps_o_f64"); break;
		case 0x68: KYTY_NI("v_cmps_u_f64"); break;
		case 0x69: KYTY_NI("v_cmps_nge_f64"); break;
		case 0x6A: KYTY_NI("v_cmps_nlg_f64"); break;
		case 0x6B: KYTY_NI("v_cmps_ngt_f64"); break;
		case 0x6C: KYTY_NI("v_cmps_nle_f64"); break;
		case 0x6D: KYTY_NI("v_cmps_neq_f64"); break;
		case 0x6E: KYTY_NI("v_cmps_nlt_f64"); break;
		case 0x6F: KYTY_NI("v_cmps_tru_f64"); break;
		case 0x70: KYTY_NI("v_cmpsx_f_f64"); break;
		case 0x71: KYTY_NI("v_cmpsx_lt_f64"); break;
		case 0x72: KYTY_NI("v_cmpsx_eq_f64"); break;
		case 0x73: KYTY_NI("v_cmpsx_le_f64"); break;
		case 0x74: KYTY_NI("v_cmpsx_gt_f64"); break;
		case 0x75: KYTY_NI("v_cmpsx_lg_f64"); break;
		case 0x76: KYTY_NI("v_cmpsx_ge_f64"); break;
		case 0x77: KYTY_NI("v_cmpsx_o_f64"); break;
		case 0x78: KYTY_NI("v_cmpsx_u_f64"); break;
		case 0x79: KYTY_NI("v_cmpsx_nge_f64"); break;
		case 0x7A: KYTY_NI("v_cmpsx_nlg_f64"); break;
		case 0x7B: KYTY_NI("v_cmpsx_ngt_f64"); break;
		case 0x7C: KYTY_NI("v_cmpsx_nle_f64"); break;
		case 0x7D: KYTY_NI("v_cmpsx_neq_f64"); break;
		case 0x7E: KYTY_NI("v_cmpsx_nlt_f64"); break;
		case 0x7F: KYTY_NI("v_cmpsx_tru_f64"); break;
		case 0x80: inst.type = ShaderInstructionType::VCmpFI32; break;
		case 0x81: inst.type = ShaderInstructionType::VCmpLtI32; break;
		case 0x82: inst.type = ShaderInstructionType::VCmpEqI32; break;
		case 0x83: inst.type = ShaderInstructionType::VCmpLeI32; break;
		case 0x84: inst.type = ShaderInstructionType::VCmpGtI32; break;
		case 0x85: inst.type = ShaderInstructionType::VCmpNeI32; break;
		case 0x86: inst.type = ShaderInstructionType::VCmpGeI32; break;
		case 0x87: inst.type = ShaderInstructionType::VCmpTI32; break;
		case 0x88: KYTY_NI("v_cmp_class_f32"); break;
		case 0x89: KYTY_NI("v_cmp_lt_i16"); break;
		case 0x8A: KYTY_NI("v_cmp_eq_i16"); break;
		case 0x8B: KYTY_NI("v_cmp_le_i16"); break;
		case 0x8C: KYTY_NI("v_cmp_gt_i16"); break;
		case 0x8D: KYTY_NI("v_cmp_ne_i16"); break;
		case 0x8E: KYTY_NI("v_cmp_ge_i16"); break;
		case 0x8F: KYTY_NI("v_cmp_class_f16"); break;
		case 0x90: KYTY_NI("v_cmpx_f_i32"); break;
		case 0x91: inst.type = ShaderInstructionType::VCmpxLtI32; break;
		case 0x92: inst.type = ShaderInstructionType::VCmpxEqI32; break;
		case 0x93: inst.type = ShaderInstructionType::VCmpxLeI32; break;
		case 0x94: inst.type = ShaderInstructionType::VCmpxGtI32; break;
		case 0x95: inst.type = ShaderInstructionType::VCmpxNeI32; break;
		case 0x96: inst.type = ShaderInstructionType::VCmpxGeI32; break;
		case 0x97: KYTY_NI("v_cmpx_t_i32"); break;
		case 0x98: KYTY_NI("v_cmpx_class_f32"); break;
		case 0x99: KYTY_NI("v_cmpx_lt_i16"); break;
		case 0x9A: KYTY_NI("v_cmpx_eq_i16"); break;
		case 0x9B: KYTY_NI("v_cmpx_le_i16"); break;
		case 0x9C: KYTY_NI("v_cmpx_gt_i16"); break;
		case 0x9D: KYTY_NI("v_cmpx_ne_i16"); break;
		case 0x9E: KYTY_NI("v_cmpx_ge_i16"); break;
		case 0x9F: KYTY_NI("v_cmpx_class_f16"); break;
		case 0xA0: KYTY_NI("v_cmp_f_i64"); break;
		case 0xA1: KYTY_NI("v_cmp_lt_i64"); break;
		case 0xA2: KYTY_NI("v_cmp_eq_i64"); break;
		case 0xA3: KYTY_NI("v_cmp_le_i64"); break;
		case 0xA4: KYTY_NI("v_cmp_gt_i64"); break;
		case 0xA5: KYTY_NI("v_cmp_ne_i64"); break;
		case 0xA6: KYTY_NI("v_cmp_ge_i64"); break;
		case 0xA7: KYTY_NI("v_cmp_t_i64"); break;
		case 0xA8: KYTY_NI("v_cmp_class_f64"); break;
		case 0xA9: KYTY_NI("v_cmp_lt_u16"); break;
		case 0xAA: KYTY_NI("v_cmp_eq_u16"); break;
		case 0xAB: KYTY_NI("v_cmp_le_u16"); break;
		case 0xAC: KYTY_NI("v_cmp_gt_u16"); break;
		case 0xAD: KYTY_NI("v_cmp_ne_u16"); break;
		case 0xAE: KYTY_NI("v_cmp_ge_u16"); break;
		case 0xB0: KYTY_NI("v_cmpx_f_i64"); break;
		case 0xB1: KYTY_NI("v_cmpx_lt_i64"); break;
		case 0xB2: KYTY_NI("v_cmpx_eq_i64"); break;
		case 0xB3: KYTY_NI("v_cmpx_le_i64"); break;
		case 0xB4: KYTY_NI("v_cmpx_gt_i64"); break;
		case 0xB5: KYTY_NI("v_cmpx_ne_i64"); break;
		case 0xB6: KYTY_NI("v_cmpx_ge_i64"); break;
		case 0xB7: KYTY_NI("v_cmpx_t_i64"); break;
		case 0xB8: KYTY_NI("v_cmpx_class_f64"); break;
		case 0xB9: KYTY_NI("v_cmpx_lt_u16"); break;
		case 0xBA: KYTY_NI("v_cmpx_eq_u16"); break;
		case 0xBB: KYTY_NI("v_cmpx_le_u16"); break;
		case 0xBC: KYTY_NI("v_cmpx_gt_u16"); break;
		case 0xBD: KYTY_NI("v_cmpx_ne_u16"); break;
		case 0xBE: KYTY_NI("v_cmpx_ge_u16"); break;
		case 0xc0: inst.type = ShaderInstructionType::VCmpFU32; break;
		case 0xc1: inst.type = ShaderInstructionType::VCmpLtU32; break;
		case 0xc2: inst.type = ShaderInstructionType::VCmpEqU32; break;
		case 0xc3: inst.type = ShaderInstructionType::VCmpLeU32; break;
		case 0xc4: inst.type = ShaderInstructionType::VCmpGtU32; break;
		case 0xc5: inst.type = ShaderInstructionType::VCmpNeU32; break;
		case 0xc6: inst.type = ShaderInstructionType::VCmpGeU32; break;
		case 0xc7: inst.type = ShaderInstructionType::VCmpTU32; break;
		case 0xC8: KYTY_NI("v_cmp_f_f16"); break;
		case 0xC9: KYTY_NI("v_cmp_lt_f16"); break;
		case 0xCA: KYTY_NI("v_cmp_eq_f16"); break;
		case 0xCB: KYTY_NI("v_cmp_le_f16"); break;
		case 0xCC: KYTY_NI("v_cmp_gt_f16"); break;
		case 0xCD: KYTY_NI("v_cmp_lg_f16"); break;
		case 0xCE: KYTY_NI("v_cmp_ge_f16"); break;
		case 0xCF: KYTY_NI("v_cmp_o_f16"); break;
		case 0xD0: KYTY_NI("v_cmpx_f_u32"); break;
		case 0xD1: inst.type = ShaderInstructionType::VCmpxLtU32; break;
		case 0xd2: inst.type = ShaderInstructionType::VCmpxEqU32; break;
		case 0xD3: inst.type = ShaderInstructionType::VCmpxLeU32; break;
		case 0xd4: inst.type = ShaderInstructionType::VCmpxGtU32; break;
		case 0xd5: inst.type = ShaderInstructionType::VCmpxNeU32; break;
		case 0xd6: inst.type = ShaderInstructionType::VCmpxGeU32; break;
		case 0xD7: KYTY_NI("v_cmpx_t_u32"); break;
		case 0xD8: KYTY_NI("v_cmpx_f_f16"); break;
		case 0xD9: KYTY_NI("v_cmpx_lt_f16"); break;
		case 0xDA: KYTY_NI("v_cmpx_eq_f16"); break;
		case 0xDB: KYTY_NI("v_cmpx_le_f16"); break;
		case 0xDC: KYTY_NI("v_cmpx_gt_f16"); break;
		case 0xDD: KYTY_NI("v_cmpx_lg_f16"); break;
		case 0xDE: KYTY_NI("v_cmpx_ge_f16"); break;
		case 0xDF: KYTY_NI("v_cmpx_o_f16"); break;
		case 0xE0: KYTY_NI("v_cmp_f_u64"); break;
		case 0xE1: KYTY_NI("v_cmp_lt_u64"); break;
		case 0xE2: KYTY_NI("v_cmp_eq_u64"); break;
		case 0xE3: KYTY_NI("v_cmp_le_u64"); break;
		case 0xE4: KYTY_NI("v_cmp_gt_u64"); break;
		case 0xE5: KYTY_NI("v_cmp_ne_u64"); break;
		case 0xE6: KYTY_NI("v_cmp_ge_u64"); break;
		case 0xE7: KYTY_NI("v_cmp_t_u64"); break;
		case 0xE8: KYTY_NI("v_cmp_u_f16"); break;
		case 0xE9: KYTY_NI("v_cmp_nge_f16"); break;
		case 0xEA: KYTY_NI("v_cmp_nlg_f16"); break;
		case 0xEB: KYTY_NI("v_cmp_ngt_f16"); break;
		case 0xEC: KYTY_NI("v_cmp_nle_f16"); break;
		case 0xED: KYTY_NI("v_cmp_neq_f16"); break;
		case 0xEE: KYTY_NI("v_cmp_nlt_f16"); break;
		case 0xEF: KYTY_NI("v_cmp_tru_f16"); break;
		case 0xF0: KYTY_NI("v_cmpx_f_u64"); break;
		case 0xF1: KYTY_NI("v_cmpx_lt_u64"); break;
		case 0xF2: KYTY_NI("v_cmpx_eq_u64"); break;
		case 0xF3: KYTY_NI("v_cmpx_le_u64"); break;
		case 0xF4: KYTY_NI("v_cmpx_gt_u64"); break;
		case 0xF5: KYTY_NI("v_cmpx_ne_u64"); break;
		case 0xF6: KYTY_NI("v_cmpx_ge_u64"); break;
		case 0xF7: KYTY_NI("v_cmpx_t_u64"); break;
		case 0xF8: KYTY_NI("v_cmpx_u_f16"); break;
		case 0xF9: KYTY_NI("v_cmpx_nge_f16"); break;
		case 0xFA: KYTY_NI("v_cmpx_nlg_f16"); break;
		case 0xFB: KYTY_NI("v_cmpx_ngt_f16"); break;
		case 0xFC: KYTY_NI("v_cmpx_nle_f16"); break;
		case 0xFD: KYTY_NI("v_cmpx_neq_f16"); break;
		case 0xFE: KYTY_NI("v_cmpx_nlt_f16"); break;
		case 0xFF: KYTY_NI("v_cmpx_tru_f16"); break;

		/* VOP2 using VOP3 encoding */
		case 0x100:
			// VOP3 encoding of VOP2 OP=0 cndmask — same dual-encoding as e32.
			inst.type        = ShaderInstructionType::VCndmaskB32;
			inst.format      = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
			inst.src_num     = 3;
			inst.src[2].size = 2;
			break;
		case 0x101:
			if (next_gen)
			{
				inst.type        = ShaderInstructionType::VCndmaskB32;
				inst.format      = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
				inst.src_num     = 3;
				inst.src[2].size = 2;
			} 			else
			{
				// v_readlane_b32 writes the SGPR encoded in VDST, not a VGPR.
				inst.type    = ShaderInstructionType::VReadlaneB32;
				inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
				inst.src_num = 2;
				inst.dst     = operand_parse(vdst);
			};
			break;
		case 0x102:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				// v_writelane_b32 writes one lane of the VGPR encoded in VDST.
				inst.type    = ShaderInstructionType::VWritelaneB32;
				inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
				inst.src_num = 2;
			};
			break;
		case 0x103: inst.type = ShaderInstructionType::VAddF32; break;
		case 0x104: inst.type = ShaderInstructionType::VSubF32; break;
		case 0x105: inst.type = ShaderInstructionType::VSubrevF32; break;
		case 0x106:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_mac_legacy_f32")
			};
			break;
		case 0x107: KYTY_NI("v_mul_legacy_f32"); break;
		case 0x108: inst.type = ShaderInstructionType::VMulF32; break;
		case 0x109: KYTY_NI("v_mul_i32_i24"); break;
		case 0x10A: KYTY_NI("v_mul_hi_i32_i24"); break;
		case 0x10b: inst.type = ShaderInstructionType::VMulU32U24; break;
		case 0x10C: KYTY_NI("v_mul_hi_u32_u24"); break;
		case 0x10D:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_min_legacy_f32")
			};
			break;
		case 0x10E: KYTY_NI("v_max_legacy_f32"); break;
		case 0x10f: inst.type = ShaderInstructionType::VMinF32; break;
		case 0x110: inst.type = ShaderInstructionType::VMaxF32; break;
		case 0x111: inst.type = ShaderInstructionType::VMinI32; break;
		case 0x112: inst.type = ShaderInstructionType::VMaxI32; break;
		case 0x113: inst.type = ShaderInstructionType::VMinU32; break;
		case 0x114: inst.type = ShaderInstructionType::VMaxU32; break;
		case 0x115: inst.type = ShaderInstructionType::VLshrB32; break;
		case 0x116: inst.type = ShaderInstructionType::VLshrrevB32; break;
		case 0x117: inst.type = ShaderInstructionType::VAshrI32; break;
		case 0x118: inst.type = ShaderInstructionType::VAshrrevI32; break;
		case 0x119: inst.type = ShaderInstructionType::VLshlB32; break;
		case 0x11a: inst.type = ShaderInstructionType::VLshlrevB32; break;
		case 0x11b: inst.type = ShaderInstructionType::VAndB32; break;
		case 0x11c: inst.type = ShaderInstructionType::VOrB32; break;
		case 0x11d: inst.type = ShaderInstructionType::VXorB32; break;
		case 0x11E:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				inst.type = ShaderInstructionType::VBfmB32;
			};
			break;
		case 0x11f: inst.type = ShaderInstructionType::VMacF32; break;
		case 0x120: inst.type = ShaderInstructionType::VMadmkF32; break;
		case 0x121: inst.type = ShaderInstructionType::VMadakF32; break;
		case 0x122: inst.type = ShaderInstructionType::VBcntU32B32; break;
		case 0x123: inst.type = ShaderInstructionType::VMbcntLoU32B32; break;
		case 0x124: inst.type = ShaderInstructionType::VMbcntHiU32B32; break;
		case 0x125:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				inst.type      = ShaderInstructionType::VAddI32;
				inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
				inst.dst2      = operand_parse(sdst);
				inst.dst2.size = 2;
			};
			break;
		case 0x126:
			// Gen5 VOP3 0x126 is the direct encoding of the same no-carry
			// subtraction contract as VOP2 0x26. Legacy keeps its VCC result.
			inst.type = ShaderInstructionType::VSubI32;
			if (!next_gen)
			{
				inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
				inst.dst2      = operand_parse(sdst);
				inst.dst2.size = 2;
			};
			break;
		case 0x127:
			// Gen5 VOP3 encoding of v_subrev_u32/v_subrev_nc_u32 (no VCC).
			inst.type = ShaderInstructionType::VSubrevI32;
			if (!next_gen)
			{
				inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
				inst.dst2      = operand_parse(sdst);
				inst.dst2.size = 2;
			};
			break;
		case 0x128:
			if (next_gen)
			{
				inst.type        = ShaderInstructionType::VAddCoCiU32;
				inst.format      = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2;
				inst.src_num     = 3;
				inst.src[2].size = 2;
				inst.dst2        = operand_parse(sdst);
				inst.dst2.size   = 2;
			} else
			{
				KYTY_NI("v_addc_u32")
			};
			break;
		case 0x129:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_subb_u32")
			};
			break;
		case 0x12A:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_subbrev_u32")
			};
			break;
		case 0x12B:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_ldexp_f32")
			};
			break;
		case 0x12C:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_cvt_pkaccum_u8_f32")
			};
			break;
		case 0x12D:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_cvt_pknorm_i16_f32")
			};
			break;
		case 0x12E: KYTY_NI("v_cvt_pknorm_u16_f32"); break;
		case 0x12f: inst.type = ShaderInstructionType::VCvtPkrtzF16F32; break;
		case 0x130: KYTY_NI("v_cvt_pk_u16_u32"); break;
		case 0x131: KYTY_NI("v_cvt_pk_i16_i32"); break;
		case 0x132: KYTY_NI("v_add_f16"); break;
		case 0x133: KYTY_NI("v_sub_f16"); break;
		case 0x134: KYTY_NI("v_subrev_f16"); break;
		case 0x135: KYTY_NI("v_mul_f16"); break;
		case 0x136:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_mac_f16")
			};
			break;
		case 0x137:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_madmk_f16")
			};
			break;
		case 0x138:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_madak_f16")
			};
			break;
		case 0x139: KYTY_NI("v_max_f16"); break;
		case 0x13A: KYTY_NI("v_min_f16"); break;
		case 0x13B: KYTY_NI("v_ldexp_f16"); break;

		/* VOP3 instructions */
		case 0x140:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} 			else
			{
				// v_mad_legacy_f32 has the same value operation as v_mad_f32.
				inst.type = ShaderInstructionType::VMadF32;
			};
			break;
		case 0x141: inst.type = ShaderInstructionType::VMadF32; break;
		case 0x142: KYTY_NI("v_mad_i32_i24"); break;
		case 0x143: inst.type = ShaderInstructionType::VMadU32U24; break;
		case 0x144: inst.type = ShaderInstructionType::VCubeIdF32; break;
		case 0x145: inst.type = ShaderInstructionType::VCubescF32; break;
		case 0x146: inst.type = ShaderInstructionType::VCubetcF32; break;
		case 0x147: inst.type = ShaderInstructionType::VCubeMaF32; break;
		case 0x148: inst.type = ShaderInstructionType::VBfeU32; break;
		case 0x149: inst.type = ShaderInstructionType::VBfeI32; break;
		case 0x14A: inst.type = ShaderInstructionType::VBfiB32; break;
		case 0x14b: inst.type = ShaderInstructionType::VFmaF32; break;
		case 0x14C: KYTY_NI("v_fma_f64"); break;
		case 0x14D: KYTY_NI("v_lerp_u8"); break;
		case 0x14E: KYTY_NI("v_alignbit_b32"); break;
		case 0x14F: KYTY_NI("v_alignbyte_b32"); break;
		case 0x150: KYTY_NI("v_mullit_f32"); break;
		case 0x151: inst.type = ShaderInstructionType::VMin3F32; break;
		case 0x152: inst.type = ShaderInstructionType::VMin3I32; break;
		case 0x153: inst.type = ShaderInstructionType::VMin3U32; break;
		case 0x154: inst.type = ShaderInstructionType::VMax3F32; break;
		case 0x155: inst.type = ShaderInstructionType::VMax3I32; break;
		case 0x156: inst.type = ShaderInstructionType::VMax3U32; break;
		case 0x157: inst.type = ShaderInstructionType::VMed3F32; break;
		case 0x158: inst.type = ShaderInstructionType::VMed3I32; break;
		case 0x159: inst.type = ShaderInstructionType::VMed3U32; break;
		case 0x15A: KYTY_NI("v_sad_u8"); break;
		case 0x15B: KYTY_NI("v_sad_hi_u8"); break;
		case 0x15C: KYTY_NI("v_sad_u16"); break;
		case 0x15d: inst.type = ShaderInstructionType::VSadU32; break;
		case 0x15E: KYTY_NI("v_cvt_pk_u8_f32"); break;
		case 0x15F: KYTY_NI("v_div_fixup_f32"); break;
		case 0x160: KYTY_NI("v_div_fixup_f64"); break;
		case 0x161: KYTY_NI("v_lshl_b64"); break;
		case 0x162: KYTY_NI("v_lshr_b64"); break;
		case 0x163: KYTY_NI("v_ashr_i64"); break;
		case 0x164: KYTY_NI("v_add_f64"); break;
		case 0x165: KYTY_NI("v_mul_f64"); break;
		case 0x166: KYTY_NI("v_min_f64"); break;
		case 0x167: KYTY_NI("v_max_f64"); break;
		case 0x168: KYTY_NI("v_ldexp_f64"); break;
		case 0x169:
			inst.type    = ShaderInstructionType::VMulLoU32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x16a:
			inst.type    = ShaderInstructionType::VMulHiU32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x16b:
			inst.type    = ShaderInstructionType::VMulLoI32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x16c:
			inst.type    = ShaderInstructionType::VMulHiI32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x16D: KYTY_NI("v_div_scale_f32"); break;
		case 0x16E: KYTY_NI("v_div_scale_f64"); break;
		case 0x16F: KYTY_NI("v_div_fmas_f32"); break;
		case 0x170: KYTY_NI("v_div_fmas_f64"); break;
		case 0x171: KYTY_NI("v_msad_u8"); break;
		case 0x174: KYTY_NI("v_trig_preop_f64"); break;
		case 0x175: KYTY_NI("v_mqsad_u32_u8"); break;
		case 0x176:
			inst.type        = ShaderInstructionType::VMadU64U32;
			inst.format      = ShaderInstructionFormat::Vdst2Sdst2Vsrc0Vsrc1Vsrc2Pair;
			inst.dst.size    = 2;
			inst.dst2        = operand_parse(sdst);
			inst.dst2.size   = 2;
			inst.src[2].size = 2;
			break;
		case 0x177: KYTY_NI("v_mad_i64_i32"); break;
		case 0x303:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_add_u16")
			};
			break;
		case 0x304:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_sub_u16")
			};
			break;
		case 0x305: KYTY_NI("v_mul_lo_u16"); break;
		case 0x307: KYTY_NI("v_lshrrev_b16"); break;
		case 0x308: KYTY_NI("v_ashrrev_i16"); break;
		case 0x309: KYTY_NI("v_max_u16"); break;
		case 0x30A: KYTY_NI("v_max_i16"); break;
		case 0x30B: KYTY_NI("v_min_u16"); break;
		case 0x30C: KYTY_NI("v_min_i16"); break;
		case 0x30D:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_add_i16")
			};
			break;
		case 0x30E:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_sub_i16")
			};
			break;
		case 0x30F:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_add_u32")
			};
			break;
		case 0x310:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_sub_u32")
			};
			break;
		case 0x311: KYTY_NI("v_pack_b32_f16"); break;
		case 0x312: KYTY_NI("v_cvt_pknorm_i16_f16"); break;
		case 0x313: inst.type = ShaderInstructionType::VDot2cF32F16; break;
		case 0x314: KYTY_NI("v_lshlrev_b16"); break;
		case 0x316: inst.type = ShaderInstructionType::VDot4cI32I8; break;
		// VOP3P mixed-precision FMA (RDNA2). For now, map all three variants
		// to a full-precision f32 FMA — this is correct for _MIX_F32 and a
		// safe over-precision approximation for the f16 narrowing variants
		// (_MIXLO/_MIXHI), which would write a packed half-float lane. The
		// SPIR-V back-end already handles the Fma GLSL intrinsic correctly.
		case 0x320: inst.type = ShaderInstructionType::VFmaMixF32; break;
		case 0x321: inst.type = ShaderInstructionType::VFmaMixF32; break; // v_fma_mixlo_f16 → Fma f32 (safe)
		case 0x322: inst.type = ShaderInstructionType::VFmaMixF32; break; // v_fma_mixhi_f16 → Fma f32 (safe)
		case 0x340: KYTY_NI("v_mad_u16"); break;
		case 0x341: KYTY_NI("v_mad_f16"); break;
		case 0x342: KYTY_NI("v_interp_p1ll_f16"); break;
		case 0x343: KYTY_NI("v_interp_p1lv_f16"); break;
		case 0x344: KYTY_NI("v_perm_b32"); break;
		case 0x345:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_xad_b32")
			};
			break;
		case 0x346: inst.type = ShaderInstructionType::VLshlAddU32; break;
		case 0x347: inst.type = ShaderInstructionType::VAddLshlU32; break;
		case 0x34B: KYTY_NI("v_fma_f16"); break;
		case 0x351: KYTY_NI("v_min3_f16"); break;
		case 0x352: KYTY_NI("v_min3_i16"); break;
		case 0x353: KYTY_NI("v_min3_u16"); break;
		case 0x354: KYTY_NI("v_max3_f16"); break;
		case 0x355: KYTY_NI("v_max3_i16"); break;
		case 0x356: KYTY_NI("v_max3_u16"); break;
		case 0x357: KYTY_NI("v_med3_f16"); break;
		case 0x358: KYTY_NI("v_med3_i16"); break;
		case 0x359: KYTY_NI("v_med3_u16"); break;
		case 0x35A: KYTY_NI("v_interp_p2_f16"); break;
		case 0x35E: KYTY_NI("v_mad_i16"); break;
		case 0x35F: KYTY_NI("v_div_fixup_f16"); break;
		case 0x360:
			// v_readlane_b32 writes the SGPR encoded in VDST, not a VGPR.
			inst.type    = ShaderInstructionType::VReadlaneB32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			inst.dst     = operand_parse(vdst);
			break;
		case 0x361:
			// v_writelane_b32 writes one lane of the VGPR encoded in VDST.
			inst.type    = ShaderInstructionType::VWritelaneB32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x364:
			inst.type    = ShaderInstructionType::VBcntU32B32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x36D: inst.type = ShaderInstructionType::VAdd3U32; break;
		case 0x36F: inst.type = ShaderInstructionType::VLshlOrB32; break;
		case 0x371: inst.type = ShaderInstructionType::VAndOrB32; break;
		case 0x372:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_or3_u32")
			};
			break;
		case 0x373: KYTY_NI("v_mad_u32_u16"); break;
		case 0x375: KYTY_NI("v_mad_i32_i16"); break;

		/* VOP1 using VOP3 encoding */
		case 0x180:
			inst.type    = ShaderInstructionType::VNop;
			inst.format  = ShaderInstructionFormat::Empty;
			inst.src_num = 0;
			break;
		case 0x181: KYTY_NI("v_mov_b32"); break;
		case 0x182:
			// VOP3 encoding of v_readfirstlane_b32: SGPR destination in VDST.
			inst.type    = ShaderInstructionType::VReadfirstlaneB32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0;
			inst.src_num = 1;
			inst.dst     = operand_parse(vdst);
			break;
		case 0x183: KYTY_NI("v_cvt_i32_f64"); break;
		case 0x184: KYTY_NI("v_cvt_f64_i32"); break;
		case 0x185: inst.type = ShaderInstructionType::VCvtF32I32; break;
		case 0x186: inst.type = ShaderInstructionType::VCvtF32U32; break;
		case 0x187: inst.type = ShaderInstructionType::VCvtU32F32; break;
		case 0x188: inst.type = ShaderInstructionType::VCvtI32F32; break;
		case 0x189: KYTY_NI("v_mov_fed_b32"); break;
		case 0x18A: KYTY_NI("v_cvt_f16_f32"); break;
		case 0x18B: KYTY_NI("v_cvt_f32_f16"); break;
		case 0x18C: KYTY_NI("v_cvt_rpi_i32_f32"); break;
		case 0x18D: inst.type = ShaderInstructionType::VCvtFlrI32F32; break;
		case 0x18E: inst.type = ShaderInstructionType::VCvtOffF32I4; break;
		case 0x18F: KYTY_NI("v_cvt_f32_f64"); break;
		case 0x190: KYTY_NI("v_cvt_f64_f32"); break;
		case 0x191: KYTY_NI("v_cvt_f32_ubyte0"); break;
		case 0x192: KYTY_NI("v_cvt_f32_ubyte1"); break;
		case 0x193: KYTY_NI("v_cvt_f32_ubyte2"); break;
		case 0x194: KYTY_NI("v_cvt_f32_ubyte3"); break;
		case 0x195: KYTY_NI("v_cvt_u32_f64"); break;
		case 0x196: KYTY_NI("v_cvt_f64_u32"); break;
		case 0x197: KYTY_NI("v_trunc_f64"); break;
		case 0x198: KYTY_NI("v_ceil_f64"); break;
		case 0x199: KYTY_NI("v_rndne_f64"); break;
		case 0x19A: KYTY_NI("v_floor_f64"); break;
		case 0x1a0: inst.type = ShaderInstructionType::VFractF32; break;
		case 0x1a1: inst.type = ShaderInstructionType::VTruncF32; break;
		case 0x1a2: inst.type = ShaderInstructionType::VCeilF32; break;
		case 0x1a3: inst.type = ShaderInstructionType::VRndneF32; break;
		case 0x1a4: inst.type = ShaderInstructionType::VFloorF32; break;
		case 0x1a5: inst.type = ShaderInstructionType::VExpF32; break;
		case 0x1A6: KYTY_NI("v_log_clamp_f32"); break;
		case 0x1a7: inst.type = ShaderInstructionType::VLogF32; break;
		case 0x1A8: KYTY_NI("v_rcp_clamp_f32"); break;
		case 0x1A9: KYTY_NI("v_rcp_legacy_f32"); break;
		case 0x1aa: inst.type = ShaderInstructionType::VRcpF32; break;
		case 0x1AB:
			// VOP3 uses the same reciprocal value contract; modifiers remain in
			// the normalized instruction for the shared recompiler helper.
			inst.type = ShaderInstructionType::VRcpF32;
			break;
		case 0x1AC: KYTY_NI("v_rsq_clamp_f32"); break;
		case 0x1AD: KYTY_NI("v_rsq_legacy_f32"); break;
		case 0x1ae: inst.type = ShaderInstructionType::VRsqF32; break;
		case 0x1AF: KYTY_NI("v_rcp_f64"); break;
		case 0x1B0: KYTY_NI("v_rcp_clamp_f64"); break;
		case 0x1B1: KYTY_NI("v_rsq_f64"); break;
		case 0x1B2: KYTY_NI("v_rsq_clamp_f64"); break;
		case 0x1b3: inst.type = ShaderInstructionType::VSqrtF32; break;
		case 0x1B4: KYTY_NI("v_sqrt_f64"); break;
		case 0x1b5: inst.type = ShaderInstructionType::VSinF32; break;
		case 0x1b6: inst.type = ShaderInstructionType::VCosF32; break;
		case 0x1B7: KYTY_NI("v_not_b32"); break;
		case 0x1B8: KYTY_NI("v_bfrev_b32"); break;
		case 0x1B9: KYTY_NI("v_ffbh_u32"); break;
		case 0x1BA: KYTY_NI("v_ffbl_b32"); break;
		case 0x1BB: KYTY_NI("v_ffbh_i32"); break;
		case 0x1BC: KYTY_NI("v_frexp_exp_i32_f64"); break;
		case 0x1BD: KYTY_NI("v_frexp_mant_f64"); break;
		case 0x1BE: KYTY_NI("v_fract_f64"); break;
		case 0x1BF: KYTY_NI("v_frexp_exp_i32_f32"); break;
		case 0x1C0: KYTY_NI("v_frexp_mant_f32"); break;
		case 0x1C1: KYTY_NI("v_clrexcp"); break;
		case 0x1C2: KYTY_NI("v_movreld_b32"); break;
		case 0x1C3: KYTY_NI("v_movrels_b32"); break;
		case 0x1C4: KYTY_NI("v_movrelsd_b32"); break;
		case 0x1C5: KYTY_NI("v_log_legacy_f32"); break;
		case 0x1C6: KYTY_NI("v_exp_legacy_f32"); break;
		case 0x1D0: KYTY_NI("v_cvt_f16_u16"); break;
		case 0x1D1: KYTY_NI("v_cvt_f16_i16"); break;
		case 0x1D2: KYTY_NI("v_cvt_u16_f16"); break;
		case 0x1D3: KYTY_NI("v_cvt_i16_f16"); break;
		case 0x1D4: KYTY_NI("v_rcp_f16"); break;
		case 0x1D5: KYTY_NI("v_sqrt_f16"); break;
		case 0x1D6: KYTY_NI("v_rsq_f16"); break;
		case 0x1D7: KYTY_NI("v_log_f16"); break;
		case 0x1D8: KYTY_NI("v_exp_f16"); break;
		case 0x1D9: KYTY_NI("v_frexp_mant_f16"); break;
		case 0x1DA: KYTY_NI("v_frexp_exp_i16_f16"); break;
		case 0x1DB: KYTY_NI("v_floor_f16"); break;
		case 0x1DC: KYTY_NI("v_ceil_f16"); break;
		case 0x1DD: KYTY_NI("v_trunc_f16"); break;
		case 0x1DE: KYTY_NI("v_rndne_f16"); break;
		case 0x1DF: KYTY_NI("v_fract_f16"); break;
		case 0x1E0: KYTY_NI("v_sin_f16"); break;
		case 0x1E1: KYTY_NI("v_cos_f16"); break;
		case 0x1E2: KYTY_NI("v_sat_pk_u8_i16"); break;
		case 0x1E3: KYTY_NI("v_cvt_norm_i16_f16"); break;
		case 0x1E4: KYTY_NI("v_cvt_norm_u16_f16"); break;
		case 0x1E5: KYTY_NI("v_swap_b32"); break;

		default: KYTY_UNKNOWN_OP();
	}
	if (inst.dst2.type == ShaderOperandType::Unknown)
	{
		if ((abs & 0x1u) != 0)
		{
			inst.src[0].absolute = true;
		}
		if ((abs & 0x2u) != 0)
		{
			inst.src[1].absolute = true;
		}
		if ((abs & 0x4u) != 0)
		{
			inst.src[2].absolute = true;
		}

		if (!next_gen)
		{
			inst.dst.clamp = (clamp != 0);
		}
	}

	if (next_gen)
	{
		inst.dst.clamp = (clamp != 0);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
