#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

// Single-address DS instructions carry a 16-bit byte offset {OFFSET1, OFFSET0}.
uint16_t DsSingleOffset(uint32_t offset0, uint32_t offset1)
{
	return static_cast<uint16_t>(offset0 | (offset1 << 8u));
}

// ds_write_b32/b64/b96/b128: consecutive dwords from DATA0 at ADDR + offset.
void DecodeDsWrite(ShaderInstruction* inst, uint32_t addr, uint32_t data0, uint16_t offset, int dwords)
{
	inst->type        = ShaderInstructionType::DsWriteB32;
	inst->format      = ShaderInstructionFormat::VaddrVdataOffset;
	inst->src[0]      = operand_parse(addr + 256);
	inst->src[1]      = operand_parse(data0 + 256);
	inst->src[1].size = dwords;
	inst->src_num     = 2;
	inst->ds_offset   = offset;
}

// ds_write2[st64]_b32: DATA0 and DATA1 go to two independent dword addresses; the offsets stay packed in ds_offset.
void DecodeDsWrite2(ShaderInstruction* inst, ShaderInstructionType type, uint32_t addr, uint32_t data0, uint32_t data1, uint32_t offset0,
                    uint32_t offset1)
{
	inst->type      = type;
	inst->format    = ShaderInstructionFormat::VaddrVdata2Offset01;
	inst->src[0]    = operand_parse(addr + 256);
	inst->src[1]    = operand_parse(data0 + 256);
	inst->src[2]    = operand_parse(data1 + 256);
	inst->src_num   = 3;
	inst->ds_offset = static_cast<uint16_t>((offset1 << 8u) | offset0);
}

// ds_write2[st64]_b64: DATA0 and DATA1 each hold a 64-bit value in a register
// pair; the offsets stay packed in ds_offset and are scaled by the caller.
void DecodeDsWrite2B64(ShaderInstruction* inst, ShaderInstructionType type, uint32_t addr, uint32_t data0, uint32_t data1, uint32_t offset0,
                       uint32_t offset1)
{
	DecodeDsWrite2(inst, type, addr, data0, data1, offset0, offset1);
	inst->src[1].size = 2;
	inst->src[2].size = 2;
}

// ds_read2[st64]_b64: two qwords from the packed offsets into four destination dwords.
void DecodeDsRead2B64(ShaderInstruction* inst, ShaderInstructionType type, uint32_t vdst, uint32_t addr, uint32_t offset0, uint32_t offset1)
{
	inst->type      = type;
	inst->format    = ShaderInstructionFormat::Vdst2VaddrOffset01;
	inst->dst       = operand_parse(vdst + 256);
	inst->dst.size  = 4;
	inst->src[0]    = operand_parse(addr + 256);
	inst->src_num   = 1;
	inst->ds_offset = static_cast<uint16_t>((offset1 << 8u) | offset0);
}

// ds_read_b32/b64/b96/b128: consecutive dwords at ADDR + offset into VDST.
void DecodeDsRead(ShaderInstruction* inst, uint32_t vdst, uint32_t addr, uint16_t offset, int dwords)
{
	inst->type      = ShaderInstructionType::DsReadB32;
	inst->format    = ShaderInstructionFormat::VdstVaddrOffset;
	inst->dst       = operand_parse(vdst + 256);
	inst->dst.size  = dwords;
	inst->src[0]    = operand_parse(addr + 256);
	inst->src_num   = 1;
	inst->ds_offset = offset;
}

} // namespace

KYTY_SHADER_PARSER(shader_parse_ds)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("ds");

	uint32_t opcode  = (buffer[0] >> 18u) & 0xffu;
	uint32_t gds     = (buffer[0] >> 17u) & 0x1u;
	uint32_t offset0 = (buffer[0] >> 0u) & 0xffu;
	uint32_t offset1 = (buffer[0] >> 8u) & 0xffu;

	uint32_t vdst  = (buffer[1] >> 24u) & 0xffu;
	uint32_t data1 = (buffer[1] >> 16u) & 0xffu;
	uint32_t data0 = (buffer[1] >> 8u) & 0xffu;
	uint32_t addr  = (buffer[1] >> 0u) & 0xffu;

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc = pc;
	inst.ds_encoding_control   = buffer[0];
	inst.ds_encoding_registers = buffer[1];

	switch (opcode) // NOLINT
	{
		case 0x00:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsAddU32;
			inst.format    = ShaderInstructionFormat::VaddrVdataOffset;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x01:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsSubU32;
			inst.format    = ShaderInstructionFormat::VaddrVdataOffset;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x02: KYTY_NI("ds_rsub_u32"); break;
		case 0x03: KYTY_NI("ds_inc_u32"); break;
		case 0x04: KYTY_NI("ds_dec_u32"); break;
		case 0x05:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsMinI32;
			inst.format    = ShaderInstructionFormat::VaddrVdataOffset;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x06:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsMaxI32;
			inst.format    = ShaderInstructionFormat::VaddrVdataOffset;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x07:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsMinU32;
			inst.format    = ShaderInstructionFormat::VaddrVdataOffset;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x08:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsMaxU32;
			inst.format    = ShaderInstructionFormat::VaddrVdataOffset;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x09:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsAndB32;
			inst.format    = ShaderInstructionFormat::VaddrVdataOffset;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x0A:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsOrB32;
			inst.format    = ShaderInstructionFormat::VaddrVdataOffset;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x0B:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsXorB32;
			inst.format    = ShaderInstructionFormat::VaddrVdataOffset;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x0C: KYTY_NI("ds_mskor_b32"); break;
		case 0x0D:
			DecodeDsWrite(&inst, addr, data0, DsSingleOffset(offset0, offset1), 1);
			if (gds != 0)
			{
				inst.format = ShaderInstructionFormat::VaddrVdataOffsetGds;
			}
			break;
		case 0x0E: DecodeDsWrite2(&inst, ShaderInstructionType::DsWrite2B32, addr, data0, data1, offset0, offset1); break;
		case 0x0F: DecodeDsWrite2(&inst, ShaderInstructionType::DsWrite2St64B32, addr, data0, data1, offset0, offset1); break;
		case 0x10: KYTY_NI("ds_cmpst_b32"); break;
		case 0x11: KYTY_NI("ds_cmpst_f32"); break;
		case 0x12: KYTY_NI("ds_min_f32"); break;
		case 0x13: KYTY_NI("ds_max_f32"); break;
		case 0x14: KYTY_NI("ds_nop"); break;
		case 0x18: KYTY_NI("ds_gws_sema_release_all"); break;
		case 0x19: KYTY_NI("ds_gws_init"); break;
		case 0x1A: KYTY_NI("ds_gws_sema_v"); break;
		case 0x1B: KYTY_NI("ds_gws_sema_br"); break;
		case 0x1C: KYTY_NI("ds_gws_sema_p"); break;
		case 0x1D: KYTY_NI("ds_gws_barrier"); break;
		case 0x1E: KYTY_NI("ds_write_b8"); break;
		case 0x1F: KYTY_NI("ds_write_b16"); break;
		case 0x20:
			inst.type = ShaderInstructionType::DsAddRtnU32;
			inst.format = ShaderInstructionFormat::VdstVaddrVdataOffset;
			inst.dst = operand_parse(vdst + 256);
			inst.src[0] = operand_parse(addr + 256);
			inst.src[1] = operand_parse(data0 + 256);
			inst.src_num = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x21: KYTY_NI("ds_sub_rtn_u32"); break;
		case 0x22: KYTY_NI("ds_rsub_rtn_u32"); break;
		case 0x23: KYTY_NI("ds_inc_rtn_u32"); break;
		case 0x24: KYTY_NI("ds_dec_rtn_u32"); break;
		case 0x25: KYTY_NI("ds_min_rtn_i32"); break;
		case 0x26: KYTY_NI("ds_max_rtn_i32"); break;
		case 0x27: KYTY_NI("ds_min_rtn_u32"); break;
		case 0x28: KYTY_NI("ds_max_rtn_u32"); break;
		case 0x29: KYTY_NI("ds_and_rtn_b32"); break;
		case 0x2A: KYTY_NI("ds_or_rtn_b32"); break;
		case 0x2B: KYTY_NI("ds_xor_rtn_b32"); break;
		case 0x2C: KYTY_NI("ds_mskor_rtn_b32"); break;
		case 0x2D:
			inst.type      = ShaderInstructionType::DsWrxchgRtnB32;
			inst.format    = ShaderInstructionFormat::VdstVaddrVdataOffset;
			inst.dst       = operand_parse(vdst + 256);
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x2E: KYTY_NI("ds_wrxchg2_rtn_b32"); break;
		case 0x2F: KYTY_NI("ds_wrxchg2st64_rtn_b32"); break;
		case 0x30: KYTY_NI("ds_cmpst_rtn_b32"); break;
		case 0x31: KYTY_NI("ds_cmpst_rtn_f32"); break;
		case 0x32: KYTY_NI("ds_min_rtn_f32"); break;
		case 0x33: KYTY_NI("ds_max_rtn_f32"); break;
		case 0x34: KYTY_NI("ds_wrap_rtn_b32"); break;
		case 0x35:
			// ds_swizzle_b32: every lane reads ADDR of the lane the 16-bit offset
			// selects (ISA 12.13.1); a disabled source lane reads zero.
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsSwizzleB32;
			inst.format    = ShaderInstructionFormat::SVdstSVsrc0;
			inst.dst       = operand_parse(vdst + 256);
			inst.dst.size  = 1;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src_num   = 1;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x36:
			DecodeDsRead(&inst, vdst, addr, DsSingleOffset(offset0, offset1), 1);
			break;
		case 0x37:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsRead2B32;
			inst.format    = ShaderInstructionFormat::Vdst2VaddrOffset01;
			inst.dst       = operand_parse(vdst + 256);
			inst.dst.size  = 2;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src_num   = 1;
			inst.ds_offset = static_cast<uint16_t>((offset1 << 8u) | offset0);
			break;
		case 0x38:
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsRead2St64B32;
			inst.format    = ShaderInstructionFormat::Vdst2VaddrOffset01;
			inst.dst       = operand_parse(vdst + 256);
			inst.dst.size  = 2;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src_num   = 1;
			inst.ds_offset = static_cast<uint16_t>((offset1 << 8u) | offset0);
			break;
		case 0x39: KYTY_NI("ds_read_i8"); break;
		case 0x3A: KYTY_NI("ds_read_u8"); break;
		case 0x3B: KYTY_NI("ds_read_i16"); break;
		case 0x3C: KYTY_NI("ds_read_u16"); break;
		case 0x3d:
		case 0x3e:
			EXIT_NOT_IMPLEMENTED(gds == 0 || (offset0 & 3u) != 0);
			inst.type      = opcode == 0x3du ? ShaderInstructionType::DsConsume : ShaderInstructionType::DsAppend;
			inst.format    = ShaderInstructionFormat::VdstGds;
			inst.dst       = operand_parse(vdst + 256);
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0x3F:
			// Ordered-count results (VDST and the per-wave counter) are not evidenced. Refuse strictly
			// instead of lowering to a barrier that leaves both unchanged.
			KYTY_NI("ds_ordered_count");
			break;
		case 0x40: KYTY_NI("ds_add_u64"); break;
		case 0x41: KYTY_NI("ds_sub_u64"); break;
		case 0x42: KYTY_NI("ds_rsub_u64"); break;
		case 0x43: KYTY_NI("ds_inc_u64"); break;
		case 0x44: KYTY_NI("ds_dec_u64"); break;
		case 0x45: KYTY_NI("ds_min_i64"); break;
		case 0x46: KYTY_NI("ds_max_i64"); break;
		case 0x47: KYTY_NI("ds_min_u64"); break;
		case 0x48: KYTY_NI("ds_max_u64"); break;
		case 0x49: KYTY_NI("ds_and_b64"); break;
		case 0x4A: KYTY_NI("ds_or_b64"); break;
		case 0x4B: KYTY_NI("ds_xor_b64"); break;
		case 0x4C: KYTY_NI("ds_mskor_b64"); break;
		case 0x4D:
			DecodeDsWrite(&inst, addr, data0, DsSingleOffset(offset0, offset1), 2);
			break;
		case 0x4E:
			DecodeDsWrite2B64(&inst, ShaderInstructionType::DsWrite2B64, addr, data0, data1, offset0, offset1);
			break;
		case 0x4F:
			DecodeDsWrite2B64(&inst, ShaderInstructionType::DsWrite2St64B64, addr, data0, data1, offset0, offset1);
			break;
		case 0x50: KYTY_NI("ds_cmpst_b64"); break;
		case 0x51: KYTY_NI("ds_cmpst_f64"); break;
		case 0x52: KYTY_NI("ds_min_f64"); break;
		case 0x53: KYTY_NI("ds_max_f64"); break;
		case 0x60: KYTY_NI("ds_add_rtn_u64"); break;
		case 0x61: KYTY_NI("ds_sub_rtn_u64"); break;
		case 0x62: KYTY_NI("ds_rsub_rtn_u64"); break;
		case 0x63: KYTY_NI("ds_inc_rtn_u64"); break;
		case 0x64: KYTY_NI("ds_dec_rtn_u64"); break;
		case 0x65: KYTY_NI("ds_min_rtn_i64"); break;
		case 0x66: KYTY_NI("ds_max_rtn_i64"); break;
		case 0x67: KYTY_NI("ds_min_rtn_u64"); break;
		case 0x68: KYTY_NI("ds_max_rtn_u64"); break;
		case 0x69: KYTY_NI("ds_and_rtn_b64"); break;
		case 0x6A: KYTY_NI("ds_or_rtn_b64"); break;
		case 0x6B: KYTY_NI("ds_xor_rtn_b64"); break;
		case 0x6C: KYTY_NI("ds_mskor_rtn_b64"); break;
		case 0x6D: KYTY_NI("ds_wrxchg_rtn_b64"); break;
		case 0x6E: KYTY_NI("ds_wrxchg2_rtn_b64"); break;
		case 0x6F: KYTY_NI("ds_wrxchg2st64_rtn_b64"); break;
		case 0x70: KYTY_NI("ds_cmpst_rtn_b64"); break;
		case 0x71: KYTY_NI("ds_cmpst_rtn_f64"); break;
		case 0x72: KYTY_NI("ds_min_rtn_f64"); break;
		case 0x73: KYTY_NI("ds_max_rtn_f64"); break;
		case 0x76:
			DecodeDsRead(&inst, vdst, addr, DsSingleOffset(offset0, offset1), 2);
			break;
		case 0x77:
			DecodeDsRead2B64(&inst, ShaderInstructionType::DsRead2B64, vdst, addr, offset0, offset1);
			break;
		case 0x78:
			DecodeDsRead2B64(&inst, ShaderInstructionType::DsRead2St64B64, vdst, addr, offset0, offset1);
			break;
		case 0x7E:
			EXIT("unsupported DS conditional exchange: opcode=0x%02x next_gen=%u\n", opcode, next_gen ? 1u : 0u);
			break;
		case 0x80: KYTY_NI("ds_add_src2_u32"); break;
		case 0x81: KYTY_NI("ds_sub_src2_u32"); break;
		case 0x82: KYTY_NI("ds_rsub_src2_u32"); break;
		case 0x83: KYTY_NI("ds_inc_src2_u32"); break;
		case 0x84: KYTY_NI("ds_dec_src2_u32"); break;
		case 0x85: KYTY_NI("ds_min_src2_i32"); break;
		case 0x86: KYTY_NI("ds_max_src2_i32"); break;
		case 0x87: KYTY_NI("ds_min_src2_u32"); break;
		case 0x88: KYTY_NI("ds_max_src2_u32"); break;
		case 0x89: KYTY_NI("ds_and_src2_b32"); break;
		case 0x8A: KYTY_NI("ds_or_src2_b32"); break;
		case 0x8B: KYTY_NI("ds_xor_src2_b32"); break;
		case 0x8D: KYTY_NI("ds_write_src2_b32"); break;
		case 0x92: KYTY_NI("ds_min_src2_f32"); break;
		case 0x93: KYTY_NI("ds_max_src2_f32"); break;
		case 0xB3:
		{
			// ds_bpermute_b32: the backward lane permute. Each lane gathers the
			// value of the lane its index names; the index is a byte address with
			// the offset added to it, and only its bits [6:2] select a lane of
			// the same half wave. A disabled source lane reads zero.
			EXIT_NOT_IMPLEMENTED(gds != 0);
			inst.type      = ShaderInstructionType::DsBpermuteB32;
			inst.format    = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.dst       = operand_parse(vdst + 256);
			inst.dst.size  = 1;
			inst.src[0]    = operand_parse(addr + 256);
			inst.src[1]    = operand_parse(data0 + 256);
			inst.src_num   = 2;
			inst.ds_offset = static_cast<uint16_t>(offset0);
			break;
		}
		case 0xB0:
			// ds_write_addtid_b32: LDS[M0[15:0] + offset + TID*4] = DATA0 (no ADDR field).
			if (gds != 0) { KYTY_NI("ds_write_addtid_b32_gds"); break; }
			inst.type      = ShaderInstructionType::DsWriteAddtidB32;
			inst.format    = ShaderInstructionFormat::VdataOffset;
			inst.src[0]    = operand_parse(data0 + 256);
			inst.src_num   = 1;
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0xB1:
			// ds_read_addtid_b32: VDST = LDS[M0[15:0] + offset + TID*4] (no ADDR field).
			if (gds != 0) { KYTY_NI("ds_read_addtid_b32_gds"); break; }
			inst.type      = ShaderInstructionType::DsReadAddtidB32;
			inst.format    = ShaderInstructionFormat::VdstOffset;
			inst.dst       = operand_parse(vdst + 256);
			inst.ds_offset = DsSingleOffset(offset0, offset1);
			break;
		case 0xC0: KYTY_NI("ds_add_src2_u64"); break;
		case 0xC1: KYTY_NI("ds_sub_src2_u64"); break;
		case 0xC2: KYTY_NI("ds_rsub_src2_u64"); break;
		case 0xC3: KYTY_NI("ds_inc_src2_u64"); break;
		case 0xC4: KYTY_NI("ds_dec_src2_u64"); break;
		case 0xC5: KYTY_NI("ds_min_src2_i64"); break;
		case 0xC6: KYTY_NI("ds_max_src2_i64"); break;
		case 0xC7: KYTY_NI("ds_min_src2_u64"); break;
		case 0xC8: KYTY_NI("ds_max_src2_u64"); break;
		case 0xC9: KYTY_NI("ds_and_src2_b64"); break;
		case 0xCA: KYTY_NI("ds_or_src2_b64"); break;
		case 0xCB: KYTY_NI("ds_xor_src2_b64"); break;
		case 0xCD: KYTY_NI("ds_write_src2_b64"); break;
		case 0xD2: KYTY_NI("ds_min_src2_f64"); break;
		case 0xD3: KYTY_NI("ds_max_src2_f64"); break;
		case 0xDE:
			DecodeDsWrite(&inst, addr, data0, DsSingleOffset(offset0, offset1), 3);
			break;
		case 0xDF:
			DecodeDsWrite(&inst, addr, data0, DsSingleOffset(offset0, offset1), 4);
			break;
		case 0xFD:
			EXIT("unsupported DS conditional exchange: opcode=0x%02x next_gen=%u\n", opcode, next_gen ? 1u : 0u);
			break;
		case 0xFE:
			DecodeDsRead(&inst, vdst, addr, DsSingleOffset(offset0, offset1), 3);
			break;
		case 0xFF:
			DecodeDsRead(&inst, vdst, addr, DsSingleOffset(offset0, offset1), 4);
			break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
