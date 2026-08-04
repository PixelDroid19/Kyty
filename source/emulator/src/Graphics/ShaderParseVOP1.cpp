#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_vop1)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("vop1");

	uint32_t vdst   = (buffer[0] >> 17u) & 0xffu;
	uint32_t src0   = (buffer[0] >> 0u) & 0x1ffu;
	uint32_t opcode = (buffer[0] >> 9u) & 0xffu;
	// SDWA uses src0 encoding 249 and a second control dword (same layout as VOP2
	// for src0/dst fields). DPP uses 250 and obtains its VGPR source and lane
	// routing controls from that second dword.
	const bool sdwa = (src0 == 249u);
	const bool dpp  = (src0 == 250u);
	uint32_t   size = ((sdwa || dpp) ? 2u : 1u);

	src0               = ((sdwa || dpp) ? (buffer[1] >> 0u) & 0xffu : src0);
	uint32_t dst_sel   = (sdwa ? (buffer[1] >> 8u) & 0x7u : 6u);
	uint32_t dst_u     = (sdwa ? (buffer[1] >> 11u) & 0x3u : 2u);
	uint32_t clmp      = (sdwa ? (buffer[1] >> 13u) & 0x1u : 0u);
	uint32_t omod      = (sdwa ? (buffer[1] >> 14u) & 0x3u : 0u);
	uint32_t src0_sel  = (sdwa ? (buffer[1] >> 16u) & 0x7u : 6u);
	uint32_t src0_sext = (sdwa ? (buffer[1] >> 19u) & 0x1u : 0u);
	uint32_t src0_neg  = (sdwa ? (buffer[1] >> 20u) & 0x1u : 0u);
	uint32_t src0_abs  = (sdwa ? (buffer[1] >> 21u) & 0x1u : 0u);
	uint32_t s0        = (sdwa ? (buffer[1] >> 23u) & 0x1u : 1u);

	// Destination partial writes (dst_sel != DWORD) need a read-modify-write
	// of the target VGPR; not wired yet. Source SEL 0-6 is supported.
	EXIT_NOT_IMPLEMENTED(dst_sel != 6);
	EXIT_NOT_IMPLEMENTED(sdwa && dst_sel == 6 && dst_u != 0);
	EXIT_NOT_IMPLEMENTED(src0_sel > 6);
	EXIT_NOT_IMPLEMENTED(src0_sext != 0);

	ShaderInstruction inst;
	inst.pc = pc;
	// Non-SDWA: 9-bit src0 is already SGPR/VGPR encoded. SDWA: 8-bit + s0 flag
	// (s0==0 → VGPR, same as VOP2 SDWA).
	inst.src[0]  = operand_parse(dpp ? (src0 + 256u) : (sdwa ? (src0 + (s0 == 0 ? 256u : 0u)) : src0));
	inst.dst     = operand_parse(vdst + 256);
	inst.src_num = 1;

	switch (omod)
	{
		case 0: inst.dst.multiplier = 1.0f; break;
		case 1: inst.dst.multiplier = 2.0f; break;
		case 2: inst.dst.multiplier = 4.0f; break;
		case 3: inst.dst.multiplier = 0.5f; break;
		default: break;
	}

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	inst.src[0].swizzle  = static_cast<uint8_t>(src0_sel);
	inst.src[0].absolute = (src0_abs != 0);
	inst.src[0].negate   = (src0_neg != 0);
	inst.src[0].dpp                = dpp;
	inst.src[0].dpp_ctrl           = static_cast<uint16_t>((buffer[1] >> 8u) & 0x1ffu);
	inst.src[0].dpp_fetch_inactive = dpp && ((buffer[1] & (1u << 18u)) != 0);
	inst.src[0].dpp_bound_ctrl     = dpp && ((buffer[1] & (1u << 19u)) != 0);
	inst.src[0].dpp_bank_mask      = static_cast<uint8_t>((buffer[1] >> 24u) & 0xfu);
	inst.src[0].dpp_row_mask       = static_cast<uint8_t>((buffer[1] >> 28u) & 0xfu);
	inst.dst.clamp       = (clmp != 0);

	inst.format = ShaderInstructionFormat::SVdstSVsrc0;

	switch (opcode)
	{
		case 0x00:
			inst.type    = ShaderInstructionType::VNop;
			inst.format  = ShaderInstructionFormat::Empty;
			inst.src_num = 0;
			break;
		case 0x01: inst.type = ShaderInstructionType::VMovB32; break;
		case 0x02:
			// Destination is an SGPR even though the VOP1 encoding uses the VDST
			// field; the source remains a VGPR/scalar operand.
			inst.type = ShaderInstructionType::VReadfirstlaneB32;
			inst.dst  = operand_parse(vdst);
			break;
		case 0x03: KYTY_NI("v_cvt_i32_f64"); break;
		case 0x04: KYTY_NI("v_cvt_f64_i32"); break;
		case 0x05: inst.type = ShaderInstructionType::VCvtF32I32; break;
		case 0x06: inst.type = ShaderInstructionType::VCvtF32U32; break;
		case 0x07: inst.type = ShaderInstructionType::VCvtU32F32; break;
		case 0x08: inst.type = ShaderInstructionType::VCvtI32F32; break;
		case 0x09: KYTY_NI("v_mov_fed_b32"); break;
		case 0x0A: KYTY_NI("v_cvt_f16_f32"); break;
		case 0x0b: inst.type = ShaderInstructionType::VCvtF32F16; break;
		case 0x0C: KYTY_NI("v_cvt_rpi_i32_f32"); break;
		case 0x0D: inst.type = ShaderInstructionType::VCvtFlrI32F32; break;
		case 0x0E: inst.type = ShaderInstructionType::VCvtOffF32I4; break;
		case 0x0F: KYTY_NI("v_cvt_f32_f64"); break;
		case 0x10: KYTY_NI("v_cvt_f64_f32"); break;
		case 0x11: inst.type = ShaderInstructionType::VCvtF32Ubyte0; break;
		case 0x12: inst.type = ShaderInstructionType::VCvtF32Ubyte1; break;
		case 0x13: inst.type = ShaderInstructionType::VCvtF32Ubyte2; break;
		case 0x14: inst.type = ShaderInstructionType::VCvtF32Ubyte3; break;
		case 0x15: KYTY_NI("v_cvt_u32_f64"); break;
		case 0x16: KYTY_NI("v_cvt_f64_u32"); break;
		case 0x17: KYTY_NI("v_trunc_f64"); break;
		case 0x18: KYTY_NI("v_ceil_f64"); break;
		case 0x19: KYTY_NI("v_rndne_f64"); break;
		case 0x1A: KYTY_NI("v_floor_f64"); break;
		case 0x20: inst.type = ShaderInstructionType::VFractF32; break;
		case 0x21: inst.type = ShaderInstructionType::VTruncF32; break;
		case 0x22: inst.type = ShaderInstructionType::VCeilF32; break;
		case 0x23: inst.type = ShaderInstructionType::VRndneF32; break;
		case 0x24: inst.type = ShaderInstructionType::VFloorF32; break;
		case 0x25: inst.type = ShaderInstructionType::VExpF32; break;
		case 0x26: KYTY_NI("v_log_clamp_f32"); break;
		case 0x27: inst.type = ShaderInstructionType::VLogF32; break;
		case 0x28: KYTY_NI("v_rcp_clamp_f32"); break;
		case 0x29: KYTY_NI("v_rcp_legacy_f32"); break;
		case 0x2a: inst.type = ShaderInstructionType::VRcpF32; break;
		case 0x2B:
			// Gen5 v_rcp_iflag_f32 has the same value operation as v_rcp_f32.
			// The current shader IR has no exception-state side effect to carry.
			inst.type = ShaderInstructionType::VRcpF32;
			break;
		case 0x2C: KYTY_NI("v_rsq_clamp_f32"); break;
		case 0x2D: KYTY_NI("v_rsq_legacy_f32"); break;
		case 0x2e: inst.type = ShaderInstructionType::VRsqF32; break;
		case 0x2F: KYTY_NI("v_rcp_f64"); break;
		case 0x30: KYTY_NI("v_rcp_clamp_f64"); break;
		case 0x31: KYTY_NI("v_rsq_f64"); break;
		case 0x32: KYTY_NI("v_rsq_clamp_f64"); break;
		case 0x33: inst.type = ShaderInstructionType::VSqrtF32; break;
		case 0x34: KYTY_NI("v_sqrt_f64"); break;
		case 0x35: inst.type = ShaderInstructionType::VSinF32; break;
		case 0x36: inst.type = ShaderInstructionType::VCosF32; break;
		case 0x37: inst.type = ShaderInstructionType::VNotB32; break;
		case 0x38: inst.type = ShaderInstructionType::VBfrevB32; break;
		case 0x39: KYTY_NI("v_ffbh_u32"); break;
		case 0x3A: KYTY_NI("v_ffbl_b32"); break;
		case 0x3B: KYTY_NI("v_ffbh_i32"); break;
		case 0x3C: KYTY_NI("v_frexp_exp_i32_f64"); break;
		case 0x3D: KYTY_NI("v_frexp_mant_f64"); break;
		case 0x3E: KYTY_NI("v_fract_f64"); break;
		case 0x3F: KYTY_NI("v_frexp_exp_i32_f32"); break;
		case 0x40: KYTY_NI("v_frexp_mant_f32"); break;
		case 0x41: KYTY_NI("v_clrexcp"); break;
		case 0x42: KYTY_NI("v_movreld_b32"); break;
		case 0x43: KYTY_NI("v_movrels_b32"); break;
		case 0x44: KYTY_NI("v_movrelsd_b32"); break;
		case 0x45: KYTY_NI("v_log_legacy_f32"); break;
		case 0x46: KYTY_NI("v_exp_legacy_f32"); break;
		case 0x50: KYTY_NI("v_cvt_f16_u16"); break;
		case 0x51: KYTY_NI("v_cvt_f16_i16"); break;
		case 0x52: KYTY_NI("v_cvt_u16_f16"); break;
		case 0x53: KYTY_NI("v_cvt_i16_f16"); break;
		case 0x54: KYTY_NI("v_rcp_f16"); break;
		case 0x55: KYTY_NI("v_sqrt_f16"); break;
		case 0x56: KYTY_NI("v_rsq_f16"); break;
		case 0x57: KYTY_NI("v_log_f16"); break;
		case 0x58: KYTY_NI("v_exp_f16"); break;
		case 0x59: KYTY_NI("v_frexp_mant_f16"); break;
		case 0x5A: KYTY_NI("v_frexp_exp_i16_f16"); break;
		case 0x5B: KYTY_NI("v_floor_f16"); break;
		case 0x5C: KYTY_NI("v_ceil_f16"); break;
		case 0x5D: KYTY_NI("v_trunc_f16"); break;
		case 0x5E: KYTY_NI("v_rndne_f16"); break;
		case 0x5F: KYTY_NI("v_fract_f16"); break;
		case 0x60: KYTY_NI("v_sin_f16"); break;
		case 0x61: KYTY_NI("v_cos_f16"); break;
		case 0x62: KYTY_NI("v_sat_pk_u8_i16"); break;
		case 0x63: KYTY_NI("v_cvt_norm_i16_f16"); break;
		case 0x64: KYTY_NI("v_cvt_norm_u16_f16"); break;
		case 0x65: KYTY_NI("v_swap_b32"); break;

		default: KYTY_UNKNOWN_OP();
	}

	// DPP VOP1 is currently represented only for v_mov_b32, whose lane routing
	// can be emitted exactly. Other VOP1 operations need their own modifiers.
	EXIT_NOT_IMPLEMENTED(dpp && inst.type != ShaderInstructionType::VMovB32);

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
