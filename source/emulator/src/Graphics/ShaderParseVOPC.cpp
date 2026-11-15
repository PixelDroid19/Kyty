#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_vopc)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("vopc");

	uint32_t opcode = (buffer[0] >> 17u) & 0xffu;
	uint32_t src0   = (buffer[0] >> 0u) & 0x1ffu;
	uint32_t vsrc1  = (buffer[0] >> 9u) & 0xffu;
	const bool sdwa = (src0 == 249u);
	const bool dpp  = (src0 == 250u);

	uint32_t size = ((sdwa || dpp) ? 2u : 1u);

	src0               = ((sdwa || dpp) ? (buffer[1] >> 0u) & 0xffu : src0);
	uint32_t sdst      = (sdwa ? (buffer[1] >> 8u) & 0x7fu : 0);
	uint32_t sd        = (sdwa ? (buffer[1] >> 15u) & 0x1u : 0);
	uint32_t src0_sel  = (sdwa ? (buffer[1] >> 16u) & 0x7u : 6);
	uint32_t src0_neg  = (sdwa ? (buffer[1] >> 20u) & 0x1u : 0);
	uint32_t src0_abs  = (sdwa ? (buffer[1] >> 21u) & 0x1u : 0);
	uint32_t s0        = (sdwa ? (buffer[1] >> 23u) & 0x1u : 1);
	uint32_t src1_sel  = (sdwa ? (buffer[1] >> 24u) & 0x7u : 6);
	uint32_t src1_neg  = (sdwa ? (buffer[1] >> 28u) & 0x1u : 0);
	uint32_t src1_abs  = (sdwa ? (buffer[1] >> 29u) & 0x1u : 0);
	uint32_t s1        = (sdwa ? (buffer[1] >> 31u) & 0x1u : 0);

	// SDWA SEL 0-6: BYTE_0..3, WORD_0..1, DWORD, zero-extended. The lowering
	// preconditions refuse SEL 7 and SEXT, which stay in vop_sdwa_ctrl.
	ShaderInstruction inst;
	inst.pc            = pc;
	inst.vop_sdwa      = sdwa;
	inst.vop_sdwa_ctrl = sdwa ? buffer[1] : 0u;
	inst.src[0]  = operand_parse(src0 + ((dpp || s0 == 0) ? 256 : 0));
	inst.src[1]  = operand_parse(vsrc1 + (s1 == 0 ? 256 : 0));
	inst.src_num = 2;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	inst.src[0].swizzle  = static_cast<uint8_t>(src0_sel);
	inst.src[1].swizzle  = static_cast<uint8_t>(src1_sel);
	inst.src[0].sdwa_sext = sdwa && ((buffer[1] >> 19u) & 1u) != 0u;
	inst.src[1].sdwa_sext = sdwa && ((buffer[1] >> 27u) & 1u) != 0u;
	inst.src[0].absolute = (src0_abs != 0);
	inst.src[1].absolute = (src1_abs != 0);
	inst.src[0].negate   = (src0_neg != 0);
	inst.src[1].negate   = (src1_neg != 0);
	inst.src[0].dpp = dpp;
	if (dpp)
	{
		inst.src[0].dpp_ctrl           = static_cast<uint16_t>((buffer[1] >> 8u) & 0x1ffu);
		inst.src[0].dpp_fetch_inactive = ((buffer[1] & (1u << 18u)) != 0);
		inst.src[0].dpp_bound_ctrl     = ((buffer[1] & (1u << 19u)) != 0);
		inst.src[0].dpp_bank_mask      = static_cast<uint8_t>((buffer[1] >> 24u) & 0xfu);
		inst.src[0].dpp_row_mask       = static_cast<uint8_t>((buffer[1] >> 28u) & 0xfu);
	}

	inst.format = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
	if (sd == 0)
	{
		inst.dst.type = ShaderOperandType::VccLo;
	} else
	{
		inst.dst = operand_parse(sdst);
	}
	// SDWA can name VCC_HI directly; it is one scalar mask dword.
	inst.dst.size = next_gen && inst.dst.type == ShaderOperandType::VccHi ? 1 : 2;

	switch (opcode)
	{
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
		case 0x20: inst.type = ShaderInstructionType::VCmpFF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x21: inst.type = ShaderInstructionType::VCmpLtF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x22: inst.type = ShaderInstructionType::VCmpEqF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x23: inst.type = ShaderInstructionType::VCmpLeF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x24: inst.type = ShaderInstructionType::VCmpGtF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x25: inst.type = ShaderInstructionType::VCmpLgF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x26: inst.type = ShaderInstructionType::VCmpGeF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x27: inst.type = ShaderInstructionType::VCmpOF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x28: inst.type = ShaderInstructionType::VCmpUF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x29: inst.type = ShaderInstructionType::VCmpNgeF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x2A: inst.type = ShaderInstructionType::VCmpNlgF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x2B: inst.type = ShaderInstructionType::VCmpNgtF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x2C: inst.type = ShaderInstructionType::VCmpNleF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x2D: inst.type = ShaderInstructionType::VCmpNeqF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x2E: inst.type = ShaderInstructionType::VCmpNltF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x2F: inst.type = ShaderInstructionType::VCmpTruF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x30: inst.type = ShaderInstructionType::VCmpxFF32; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x31: inst.type = ShaderInstructionType::VCmpxLtF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x32: inst.type = ShaderInstructionType::VCmpxEqF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x33: inst.type = ShaderInstructionType::VCmpxLeF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x34: inst.type = ShaderInstructionType::VCmpxGtF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x35: inst.type = ShaderInstructionType::VCmpxLgF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x36: inst.type = ShaderInstructionType::VCmpxGeF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x37: inst.type = ShaderInstructionType::VCmpxOF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x38: inst.type = ShaderInstructionType::VCmpxUF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x39: inst.type = ShaderInstructionType::VCmpxNgeF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x3A: inst.type = ShaderInstructionType::VCmpxNlgF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x3B: inst.type = ShaderInstructionType::VCmpxNgtF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x3C: inst.type = ShaderInstructionType::VCmpxNleF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x3D: inst.type = ShaderInstructionType::VCmpxNeqF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x3E: inst.type = ShaderInstructionType::VCmpxNltF64; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0x3F: inst.type = ShaderInstructionType::VCmpxTruF32; inst.src[0].size = 2; inst.src[1].size = 2; break;
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
		case 0x88: inst.type = ShaderInstructionType::VCmpClassF32; break;
		case 0x89: inst.type = ShaderInstructionType::VCmpLtI16; break;
		case 0x8A: inst.type = ShaderInstructionType::VCmpEqI16; break;
		case 0x8B: inst.type = ShaderInstructionType::VCmpLeI16; break;
		case 0x8C: inst.type = ShaderInstructionType::VCmpGtI16; break;
		case 0x8D: inst.type = ShaderInstructionType::VCmpNeI16; break;
		case 0x8E: inst.type = ShaderInstructionType::VCmpGeI16; break;
		case 0x8F: KYTY_NI("v_cmp_class_f16"); break;
		case 0x90: inst.type = ShaderInstructionType::VCmpxFF32; break;
		case 0x91: inst.type = ShaderInstructionType::VCmpxLtI32; break;
		case 0x92: inst.type = ShaderInstructionType::VCmpxEqI32; break;
		case 0x93: inst.type = ShaderInstructionType::VCmpxLeI32; break;
		case 0x94: inst.type = ShaderInstructionType::VCmpxGtI32; break;
		case 0x95: inst.type = ShaderInstructionType::VCmpxNeI32; break;
		case 0x96: inst.type = ShaderInstructionType::VCmpxGeI32; break;
		case 0x97: inst.type = ShaderInstructionType::VCmpxTruF32; break;
		case 0x98: KYTY_NI("v_cmpx_class_f32"); break;
		case 0x99: inst.type = ShaderInstructionType::VCmpxLtI16; break;
		case 0x9A: inst.type = ShaderInstructionType::VCmpxEqI16; break;
		case 0x9B: inst.type = ShaderInstructionType::VCmpxLeI16; break;
		case 0x9C: inst.type = ShaderInstructionType::VCmpxGtI16; break;
		case 0x9D: inst.type = ShaderInstructionType::VCmpxNeI16; break;
		case 0x9E: inst.type = ShaderInstructionType::VCmpxGeI16; break;
		case 0x9F: KYTY_NI("v_cmpx_class_f16"); break;
		case 0xA0:
			inst.type        = ShaderInstructionType::VCmpFU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xA1:
			inst.type        = ShaderInstructionType::VCmpLtI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xA2:
			inst.type        = ShaderInstructionType::VCmpEqI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xA3:
			inst.type        = ShaderInstructionType::VCmpLeI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xA4:
			inst.type        = ShaderInstructionType::VCmpGtI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xA5:
			inst.type        = ShaderInstructionType::VCmpNeI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xA6:
			inst.type        = ShaderInstructionType::VCmpGeI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xA7:
			inst.type        = ShaderInstructionType::VCmpTU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xA8: KYTY_NI("v_cmp_class_f64"); break;
		case 0xA9: inst.type = ShaderInstructionType::VCmpLtU16; break;
		case 0xAA: inst.type = ShaderInstructionType::VCmpEqU16; break;
		case 0xAB: inst.type = ShaderInstructionType::VCmpLeU16; break;
		case 0xAC: inst.type = ShaderInstructionType::VCmpGtU16; break;
		case 0xAD: inst.type = ShaderInstructionType::VCmpNeU16; break;
		case 0xAE: inst.type = ShaderInstructionType::VCmpGeU16; break;
		case 0xB0: inst.type = ShaderInstructionType::VCmpxFF32; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0xB1:
			inst.type        = ShaderInstructionType::VCmpxLtI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xB2:
			inst.type        = ShaderInstructionType::VCmpxEqI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xB3:
			inst.type        = ShaderInstructionType::VCmpxLeI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xB4:
			inst.type        = ShaderInstructionType::VCmpxGtI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xB5:
			inst.type        = ShaderInstructionType::VCmpxNeI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xB6:
			inst.type        = ShaderInstructionType::VCmpxGeI64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xB7: inst.type = ShaderInstructionType::VCmpxTruF32; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0xB8: KYTY_NI("v_cmpx_class_f64"); break;
		case 0xB9: inst.type = ShaderInstructionType::VCmpxLtU16; break;
		case 0xBA: inst.type = ShaderInstructionType::VCmpxEqU16; break;
		case 0xBB: inst.type = ShaderInstructionType::VCmpxLeU16; break;
		case 0xBC: inst.type = ShaderInstructionType::VCmpxGtU16; break;
		case 0xBD: inst.type = ShaderInstructionType::VCmpxNeU16; break;
		case 0xBE: inst.type = ShaderInstructionType::VCmpxGeU16; break;
		case 0xc0: inst.type = ShaderInstructionType::VCmpFU32; break;
		case 0xc1: inst.type = ShaderInstructionType::VCmpLtU32; break;
		case 0xc2: inst.type = ShaderInstructionType::VCmpEqU32; break;
		case 0xc3: inst.type = ShaderInstructionType::VCmpLeU32; break;
		case 0xc4: inst.type = ShaderInstructionType::VCmpGtU32; break;
		case 0xc5: inst.type = ShaderInstructionType::VCmpNeU32; break;
		case 0xc6: inst.type = ShaderInstructionType::VCmpGeU32; break;
		case 0xc7: inst.type = ShaderInstructionType::VCmpTU32; break;
		case 0xC8: inst.type = ShaderInstructionType::VCmpFF16; break;
		case 0xC9: inst.type = ShaderInstructionType::VCmpLtF16; break;
		case 0xCA: inst.type = ShaderInstructionType::VCmpEqF16; break;
		case 0xCB: inst.type = ShaderInstructionType::VCmpLeF16; break;
		case 0xCC: inst.type = ShaderInstructionType::VCmpGtF16; break;
		case 0xCD: inst.type = ShaderInstructionType::VCmpLgF16; break;
		case 0xCE: inst.type = ShaderInstructionType::VCmpGeF16; break;
		case 0xCF: inst.type = ShaderInstructionType::VCmpOF16; break;
		case 0xD0: inst.type = ShaderInstructionType::VCmpxFF32; break;
		case 0xD1: inst.type = ShaderInstructionType::VCmpxLtU32; break;
		case 0xd2: inst.type = ShaderInstructionType::VCmpxEqU32; break;
		case 0xD3: inst.type = ShaderInstructionType::VCmpxLeU32; break;
		case 0xd4: inst.type = ShaderInstructionType::VCmpxGtU32; break;
		case 0xd5: inst.type = ShaderInstructionType::VCmpxNeU32; break;
		case 0xd6: inst.type = ShaderInstructionType::VCmpxGeU32; break;
		case 0xD7: inst.type = ShaderInstructionType::VCmpxTruF32; break;
		case 0xD8: inst.type = ShaderInstructionType::VCmpxFF32; break;
		case 0xD9: inst.type = ShaderInstructionType::VCmpxLtF16; break;
		case 0xDA: inst.type = ShaderInstructionType::VCmpxEqF16; break;
		case 0xDB: inst.type = ShaderInstructionType::VCmpxLeF16; break;
		case 0xDC: inst.type = ShaderInstructionType::VCmpxGtF16; break;
		case 0xDD: inst.type = ShaderInstructionType::VCmpxLgF16; break;
		case 0xDE: inst.type = ShaderInstructionType::VCmpxGeF16; break;
		case 0xDF: inst.type = ShaderInstructionType::VCmpxOF16; break;
		case 0xE0:
			inst.type        = ShaderInstructionType::VCmpFU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xE1:
			inst.type        = ShaderInstructionType::VCmpLtU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xE2:
			inst.type        = ShaderInstructionType::VCmpEqU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xE3:
			inst.type        = ShaderInstructionType::VCmpLeU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xE4:
			inst.type        = ShaderInstructionType::VCmpGtU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xE5:
			inst.type        = ShaderInstructionType::VCmpNeU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xE6:
			inst.type        = ShaderInstructionType::VCmpGeU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xE7:
			inst.type        = ShaderInstructionType::VCmpTU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xE8: inst.type = ShaderInstructionType::VCmpUF16; break;
		case 0xE9: inst.type = ShaderInstructionType::VCmpNgeF16; break;
		case 0xEA: inst.type = ShaderInstructionType::VCmpNlgF16; break;
		case 0xEB: inst.type = ShaderInstructionType::VCmpNgtF16; break;
		case 0xEC: inst.type = ShaderInstructionType::VCmpNleF16; break;
		case 0xED: inst.type = ShaderInstructionType::VCmpNeqF16; break;
		case 0xEE: inst.type = ShaderInstructionType::VCmpNltF16; break;
		case 0xEF: inst.type = ShaderInstructionType::VCmpTruF16; break;
		case 0xF0: inst.type = ShaderInstructionType::VCmpxFF32; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0xF1:
			inst.type        = ShaderInstructionType::VCmpxLtU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xF2:
			inst.type        = ShaderInstructionType::VCmpxEqU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xF3:
			inst.type        = ShaderInstructionType::VCmpxLeU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xF4:
			inst.type        = ShaderInstructionType::VCmpxGtU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xF5:
			inst.type        = ShaderInstructionType::VCmpxNeU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xF6:
			inst.type        = ShaderInstructionType::VCmpxGeU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0xF7: inst.type = ShaderInstructionType::VCmpxTruF32; inst.src[0].size = 2; inst.src[1].size = 2; break;
		case 0xF8: inst.type = ShaderInstructionType::VCmpxUF16; break;
		case 0xF9: inst.type = ShaderInstructionType::VCmpxNgeF16; break;
		case 0xFA: inst.type = ShaderInstructionType::VCmpxNlgF16; break;
		case 0xFB: inst.type = ShaderInstructionType::VCmpxNgtF16; break;
		case 0xFC: inst.type = ShaderInstructionType::VCmpxNleF16; break;
		case 0xFD: inst.type = ShaderInstructionType::VCmpxNeqF16; break;
		case 0xFE: inst.type = ShaderInstructionType::VCmpxNltF16; break;
		case 0xFF: inst.type = ShaderInstructionType::VCmpxTruF32; break;

		default: KYTY_UNKNOWN_OP();
	}
	// SDWA and DPP select 32-bit lanes; the 64-bit compare path has no lowering for them.
	if (inst.src[0].size == 2 && (sdwa || dpp))
	{
		KYTY_NI("vopc 64-bit compare with sdwa or dpp");
	}
	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
