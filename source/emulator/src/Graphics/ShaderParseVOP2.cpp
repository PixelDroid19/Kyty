#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_vop2)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("vop2");

	uint32_t opcode = (buffer[0] >> 25u) & 0x3fu;

	switch (opcode)
	{
		case 0x3e: return shader_parse_vopc(pc, src, buffer, dst, next_gen); break;
		case 0x3f: return shader_parse_vop1(pc, src, buffer, dst, next_gen); break;
		default: break;
	}

	uint32_t vdst  = (buffer[0] >> 17u) & 0xffu;
	uint32_t src0  = (buffer[0] >> 0u) & 0x1ffu;
	uint32_t vsrc1 = (buffer[0] >> 9u) & 0xffu;
	const bool sdwa = (src0 == 249u);
	const bool dpp  = (src0 == 250u);

	uint32_t size = ((sdwa || dpp) ? 2u : 1u);

	src0               = ((sdwa || dpp) ? (buffer[1] >> 0u) & 0xffu : src0);
	uint32_t dst_sel   = (sdwa ? (buffer[1] >> 8u) & 0x7u : 6);
	uint32_t dst_u     = (sdwa ? (buffer[1] >> 11u) & 0x3u : 2);
	uint32_t clmp      = (sdwa ? (buffer[1] >> 13u) & 0x1u : 0);
	uint32_t omod      = (sdwa ? (buffer[1] >> 14u) & 0x3u : 0);
	uint32_t src0_sel  = (sdwa ? (buffer[1] >> 16u) & 0x7u : 6);
	uint32_t src0_sext = (sdwa ? (buffer[1] >> 19u) & 0x1u : 0);
	uint32_t src0_neg  = (sdwa ? (buffer[1] >> 20u) & 0x1u : 0);
	uint32_t src0_abs  = (sdwa ? (buffer[1] >> 21u) & 0x1u : 0);
	uint32_t s0        = (sdwa ? (buffer[1] >> 23u) & 0x1u : 1);
	uint32_t src1_sel  = (sdwa ? (buffer[1] >> 24u) & 0x7u : 6);
	uint32_t src1_sext = (sdwa ? (buffer[1] >> 27u) & 0x1u : 0);
	uint32_t src1_neg  = (sdwa ? (buffer[1] >> 28u) & 0x1u : 0);
	uint32_t src1_abs  = (sdwa ? (buffer[1] >> 29u) & 0x1u : 0);
	uint32_t s1        = (sdwa ? (buffer[1] >> 31u) & 0x1u : 0);

	EXIT_NOT_IMPLEMENTED(dst_sel != 6);
	EXIT_NOT_IMPLEMENTED(sdwa && dst_sel == 6 && dst_u != 0);
	EXIT_NOT_IMPLEMENTED(src0_sel > 6);
	EXIT_NOT_IMPLEMENTED(src0_sext != 0);
	// SDWA src0/src1 NEG bits map to operand.negate (same as VOP3).
	// Captured post-Play Gen5 VOP2 SDWA sets src0_neg; SPIR-V already emits OpFNegate.
	// Source SEL 0-6: BYTE_n / WORD_n / DWORD (zero-extend on load).
	EXIT_NOT_IMPLEMENTED(src1_sel > 6);
	EXIT_NOT_IMPLEMENTED(src1_sext != 0);

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0 + ((dpp || s0 == 0) ? 256 : 0));
	inst.src[1]  = operand_parse(vsrc1 + (s1 == 0 ? 256 : 0));
	inst.dst     = operand_parse(vdst + 256);
	inst.src_num = 2;

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
	inst.src[1].swizzle  = static_cast<uint8_t>(src1_sel);
	inst.src[0].absolute = (src0_abs != 0);
	inst.src[1].absolute = (src1_abs != 0);
	inst.src[0].negate   = (src0_neg != 0);
	inst.src[1].negate   = (src1_neg != 0);
	inst.src[0].dpp                = dpp;
	inst.src[0].dpp_ctrl           = static_cast<uint16_t>((buffer[1] >> 8u) & 0x1ffu);
	inst.src[0].dpp_fetch_inactive = dpp && ((buffer[1] & (1u << 18u)) != 0);
	inst.src[0].dpp_bound_ctrl     = dpp && ((buffer[1] & (1u << 19u)) != 0);
	inst.src[0].dpp_bank_mask      = static_cast<uint8_t>((buffer[1] >> 24u) & 0xfu);
	inst.src[0].dpp_row_mask       = static_cast<uint8_t>((buffer[1] >> 28u) & 0xfu);

	inst.dst.clamp = (clmp != 0);

	inst.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;

	switch (opcode)
	{
		case 0x00:
			// Observed post-Play Gen5 shader word 0x00000009:
			// v_cndmask_b32 v0, s9, v0 — VOP2 OP=0 (legacy/VI encoding).
			// Desktop GFX10 moved cndmask to OP=1; Gen5 still emits OP=0
			// (and OP=1 is also accepted below for next_gen).
			inst.type        = ShaderInstructionType::VCndmaskB32;
			inst.format      = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
			inst.src[2].type = ShaderOperandType::VccLo;
			inst.src[2].size = 2;
			inst.src_num     = 3;
			break;
		case 0x01:
			if (next_gen)
			{
				inst.type        = ShaderInstructionType::VCndmaskB32;
				inst.format      = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
				inst.src[2].type = ShaderOperandType::VccLo;
				inst.src[2].size = 2;
				inst.src_num     = 3;
			} else
			{
				KYTY_NI("v_readlane_b32");
			};
			break;
		case 0x02:
			if (next_gen)
			{
				inst.type              = ShaderInstructionType::VDot2cF32F16;
				inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
				inst.src_num           = 3;
				inst.src[2]            = inst.dst;
			} else
			{
				KYTY_NI("v_writelane_b32");
			};
			break;
		case 0x03: inst.type = ShaderInstructionType::VAddF32; break;
		case 0x04: inst.type = ShaderInstructionType::VSubF32; break;
		case 0x05: inst.type = ShaderInstructionType::VSubrevF32; break;
		case 0x06:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_mac_legacy_f32")
			};
			break;
		case 0x07:
			// v_mul_legacy_f32 has the same value operation as v_mul_f32.
			inst.type = ShaderInstructionType::VMulF32;
			break;
		case 0x08: inst.type = ShaderInstructionType::VMulF32; break;
		case 0x09: KYTY_NI("v_mul_i32_i24"); break;
		case 0x0A: KYTY_NI("v_mul_hi_i32_i24"); break;
		case 0x0C: KYTY_NI("v_mul_hi_u32_u24"); break;
		case 0x0D:
			if (next_gen)
			{
				inst.type              = ShaderInstructionType::VDot4cI32I8;
				inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
				inst.src_num           = 3;
				inst.src[2]            = inst.dst;
			} 			else
			{
				// v_min_legacy_f32 has the same value operation as v_min_f32.
				inst.type = ShaderInstructionType::VMinF32;
			};
			break;
		case 0x0E:
			// v_max_legacy_f32 has the same value operation as v_max_f32.
			inst.type = ShaderInstructionType::VMaxF32;
			break;
		case 0x0b: inst.type = ShaderInstructionType::VMulU32U24; break;
		case 0x0f: inst.type = ShaderInstructionType::VMinF32; break;
		case 0x10: inst.type = ShaderInstructionType::VMaxF32; break;
		case 0x11: inst.type = ShaderInstructionType::VMinI32; break;
		case 0x12: inst.type = ShaderInstructionType::VMaxI32; break;
		case 0x13: inst.type = ShaderInstructionType::VMinU32; break;
		case 0x14: inst.type = ShaderInstructionType::VMaxU32; break;
		case 0x15: inst.type = ShaderInstructionType::VLshrB32; break;
		case 0x16: inst.type = ShaderInstructionType::VLshrrevB32; break;
		case 0x17: inst.type = ShaderInstructionType::VAshrI32; break;
		case 0x18: inst.type = ShaderInstructionType::VAshrrevI32; break;
		case 0x19: inst.type = ShaderInstructionType::VLshlB32; break;
		case 0x1a: inst.type = ShaderInstructionType::VLshlrevB32; break;
		case 0x1b: inst.type = ShaderInstructionType::VAndB32; break;
		case 0x1c: inst.type = ShaderInstructionType::VOrB32; break;
		case 0x1d: inst.type = ShaderInstructionType::VXorB32; break;
		case 0x1E:
			if (next_gen)
			{
				inst.type = ShaderInstructionType::VXnorB32;
			} else
			{
				inst.type = ShaderInstructionType::VBfmB32;
			};
			break;
		case 0x1f: inst.type = ShaderInstructionType::VMacF32; break;
		case 0x20:
			inst.type              = ShaderInstructionType::VMadmkF32;
			inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
			inst.src_num           = 3;
			inst.src[2]            = inst.src[1];
			inst.src[1].type       = ShaderOperandType::LiteralConstant;
			inst.src[1].constant.u = buffer[size];
			inst.src[1].size       = 0;
			size++;
			break;
		case 0x21:
			inst.type              = ShaderInstructionType::VMadakF32;
			inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
			inst.src_num           = 3;
			inst.src[2].type       = ShaderOperandType::LiteralConstant;
			inst.src[2].constant.u = buffer[size];
			inst.src[2].size       = 0;
			size++;
			break;
		case 0x22: inst.type = ShaderInstructionType::VBcntU32B32; break;
		case 0x23: inst.type = ShaderInstructionType::VMbcntLoU32B32; break;
		case 0x24: inst.type = ShaderInstructionType::VMbcntHiU32B32; break;
		case 0x25:
			if (next_gen)
			{
				// GFX10/RDNA VOP2 0x25 is v_add_u32/v_add_nc_u32: no VCC dst.
				inst.type = ShaderInstructionType::VAddI32;
			} else
			{
				inst.type      = ShaderInstructionType::VAddI32;
				inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
				inst.dst2.type = ShaderOperandType::VccLo;
				inst.dst2.size = 2;
			};
			break;
		case 0x26:
			// Captured Gen5 E32 opcode 0x26 is v_sub_u32/v_sub_nc_u32:
			// it writes only the VGPR result and has no VCC destination.
			inst.type = ShaderInstructionType::VSubI32;
			if (!next_gen)
			{
				inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
				inst.dst2.type = ShaderOperandType::VccLo;
				inst.dst2.size = 2;
			};
			break;
		case 0x27:
			// Gen5 VOP2 0x27 is v_subrev_u32/v_subrev_nc_u32 (no VCC). Legacy
			// keeps the carry-out VCC destination.
			inst.type = ShaderInstructionType::VSubrevI32;
			if (!next_gen)
			{
				inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
				inst.dst2.type = ShaderOperandType::VccLo;
				inst.dst2.size = 2;
			};
			break;
		case 0x28:
			if (next_gen)
			{
				inst.type        = ShaderInstructionType::VAddCoCiU32;
				inst.format      = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2;
				inst.src[2].type = ShaderOperandType::VccLo;
				inst.src[2].size = 2;
				inst.src_num     = 3;
				inst.dst2.type   = ShaderOperandType::VccLo;
				inst.dst2.size   = 2;
			} else
			{
				KYTY_NI("v_addc_u32")
			};
			break;
		case 0x29:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_subb_u32")
			};
			break;
		case 0x2A:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_subbrev_u32")
			};
			break;
		case 0x2B:
			if (next_gen)
			{
				// RDNA2 v_fmac_f32 accumulates into VDST: dst = fma(src0, src1, dst).
				inst.type = ShaderInstructionType::VMacF32;
			} else
			{
				KYTY_NI("v_ldexp_f32")
			};
			break;
		case 0x2C:
			if (next_gen)
			{
				// RDNA2 v_fmamk_f32 has the same literal layout as legacy v_madmk_f32.
				inst.type              = ShaderInstructionType::VMadmkF32;
				inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
				inst.src_num           = 3;
				inst.src[2]            = inst.src[1];
				inst.src[1].type       = ShaderOperandType::LiteralConstant;
				inst.src[1].constant.u = buffer[size];
				inst.src[1].size       = 0;
				size++;
			} else
			{
				KYTY_NI("v_cvt_pkaccum_u8_f32")
			};
			break;
		case 0x2D:
			if (next_gen)
			{
				inst.type              = ShaderInstructionType::VFmaF32;
				inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
				inst.src_num           = 3;
				inst.src[2].type       = ShaderOperandType::LiteralConstant;
				inst.src[2].constant.u = buffer[size];
				inst.src[2].size       = 0;
				size++;
			} else
			{
				KYTY_NI("v_cvt_pknorm_i16_f32")
			};
			break;
		case 0x2E: KYTY_NI("v_cvt_pknorm_u16_f32"); break;
		case 0x2f: inst.type = ShaderInstructionType::VCvtPkrtzF16F32; break;
		case 0x30: KYTY_NI("v_cvt_pk_u16_u32"); break;
		case 0x31: KYTY_NI("v_cvt_pk_i16_i32"); break;
		case 0x32: KYTY_NI("v_add_f16"); break;
		case 0x33: KYTY_NI("v_sub_f16"); break;
		case 0x34: KYTY_NI("v_subrev_f16"); break;
		case 0x35: KYTY_NI("v_mul_f16"); break;
		case 0x36:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_mac_f16")
			};
			break;
		case 0x37:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_madmk_f16")
			};
			break;
		case 0x38:
			if (next_gen)
			{
				KYTY_UNKNOWN_OP();
			} else
			{
				KYTY_NI("v_madak_f16")
			};
			break;
		case 0x39: KYTY_NI("v_max_f16"); break;
		case 0x3A: KYTY_NI("v_min_f16"); break;
		case 0x3B: KYTY_NI("v_ldexp_f16"); break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size,google-readability-function-size,hicpp-function-size)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
