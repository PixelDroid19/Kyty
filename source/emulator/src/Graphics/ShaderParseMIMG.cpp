#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_mimg)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("mimg");

	uint32_t slc    = (buffer[0] >> 25u) & 0x1u;
	uint32_t opcode = (buffer[0] >> 18u) & 0x7fu;
	uint32_t lwe    = (buffer[0] >> 17u) & 0x1u;
	uint32_t tff    = (buffer[0] >> 16u) & 0x1u;
	uint32_t r128   = (buffer[0] >> 15u) & 0x1u;
	uint32_t da     = (buffer[0] >> 14u) & 0x1u;
	uint32_t glc    = (buffer[0] >> 13u) & 0x1u;
	uint32_t unrm   = (buffer[0] >> 12u) & 0x1u;
	uint32_t dmask  = (buffer[0] >> 8u) & 0xfu;
	uint32_t nsa    = next_gen ? ((buffer[0] >> 1u) & 0x3u) : 0u;
	uint32_t dim    = (buffer[0] >> 3u) & 0x7u;

	uint32_t ssamp = (buffer[1] >> 21u) & 0x1fu; // S#
	uint32_t srsrc = (buffer[1] >> 16u) & 0x1fu; // T#
	uint32_t vdata = (buffer[1] >> 8u) & 0xffu;
	uint32_t vaddr = (buffer[1] >> 0u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(da == 1);
	EXIT_NOT_IMPLEMENTED(r128 == 1);
	EXIT_NOT_IMPLEMENTED(tff == 1);
	EXIT_NOT_IMPLEMENTED(lwe == 1);
	EXIT_NOT_IMPLEMENTED(glc == 1);
	EXIT_NOT_IMPLEMENTED(slc == 1);
	EXIT_NOT_IMPLEMENTED(unrm == 1);
	// EXIT_NOT_IMPLEMENTED(dmask != 0xf && dmask != 0x7);

	uint32_t size = 2 + nsa;

	ShaderInstruction inst;
	inst.pc             = pc;
	inst.dst            = operand_parse(vdata + 256);
	inst.src_num        = 3;
	inst.src[0]         = operand_parse(vaddr + 256);
	inst.src[1]         = operand_parse(srsrc * 4);
	inst.src[2]         = operand_parse(ssamp * 4);
	inst.mimg_dimension = static_cast<uint8_t>(dim);

	if (nsa != 0)
	{
		const uint32_t encoded_address_num = 1 + nsa * 4;
		inst.mimg_address_num              = static_cast<int>(encoded_address_num);
		inst.mimg_address[0]               = operand_parse(vaddr + 256);
		for (uint32_t address = 1; address < encoded_address_num; address++)
		{
			const uint32_t encoded     = address - 1;
			const uint32_t vgpr        = (buffer[2 + encoded / 4] >> ((encoded % 4) * 8u)) & 0xffu;
			inst.mimg_address[address] = operand_parse(vgpr + 256);
		}
	}

	switch (opcode)
	{
		case 0x00:
			EXIT_NOT_IMPLEMENTED(dmask == 0);
			inst.type        = ShaderInstructionType::ImageLoad;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			inst.format      = ShaderInstructionFormat::VdataVaddr3StDmask;
			inst.mimg_dmask  = static_cast<uint8_t>(dmask);
			inst.dst.size    = 0;
			for (uint32_t component = 0; component < 4; component++)
			{
				inst.dst.size += static_cast<int>((dmask >> component) & 1u);
			}
			break;
		case 0x01: KYTY_NI("image_load_mip"); break;
		case 0x02: KYTY_NI("image_load_pck"); break;
		case 0x03: KYTY_NI("image_load_pck_sgn"); break;
		case 0x04: KYTY_NI("image_load_mip_pck"); break;
		case 0x05: KYTY_NI("image_load_mip_pck_sgn"); break;
		case 0x08:
			EXIT_NOT_IMPLEMENTED(dmask == 0);
			inst.type        = ShaderInstructionType::ImageStore;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			inst.format      = ShaderInstructionFormat::VdataVaddr3StDmask;
			inst.mimg_dmask  = static_cast<uint8_t>(dmask);
			inst.dst.size    = 0;
			for (uint32_t component = 0; component < 4; component++)
			{
				inst.dst.size += static_cast<int>((dmask >> component) & 1u);
			}
			break;
		case 0x09:
			inst.type        = ShaderInstructionType::ImageStoreMip;
			inst.src[0].size = 4;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			if (dmask == 0xf)
			{
				inst.format   = ShaderInstructionFormat::Vdata4Vaddr4StDmaskF;
				inst.dst.size = 4;
			}
			break;
		case 0x0A: KYTY_NI("image_store_pck"); break;
		case 0x0B: KYTY_NI("image_store_mip_pck"); break;
		case 0x0E:
		{
			EXIT_NOT_IMPLEMENTED(dmask == 0);
			inst.type        = ShaderInstructionType::ImageGetResinfo;
			inst.src[0].size = 1;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			inst.format      = ShaderInstructionFormat::VdataVaddrStDmask;
			inst.mimg_dmask  = static_cast<uint8_t>(dmask);
			inst.dst.size    = 0;
			for (uint32_t component = 0; component < 4; component++)
			{
				inst.dst.size += static_cast<int>((dmask >> component) & 1u);
			}
			break;
		}
		case 0x0F: KYTY_NI("image_atomic_swap"); break;
		case 0x10: KYTY_NI("image_atomic_cmpswap"); break;
		case 0x11: KYTY_NI("image_atomic_add"); break;
		case 0x12: KYTY_NI("image_atomic_sub"); break;
		case 0x13: KYTY_NI("image_atomic_rsub"); break;
		case 0x14: KYTY_NI("image_atomic_smin"); break;
		case 0x15: KYTY_NI("image_atomic_umin"); break;
		case 0x16: KYTY_NI("image_atomic_smax"); break;
		case 0x17: KYTY_NI("image_atomic_umax"); break;
		case 0x18: KYTY_NI("image_atomic_and"); break;
		case 0x19: KYTY_NI("image_atomic_or"); break;
		case 0x1A: KYTY_NI("image_atomic_xor"); break;
		case 0x1B: KYTY_NI("image_atomic_inc"); break;
		case 0x1C: KYTY_NI("image_atomic_dec"); break;
		case 0x1D: KYTY_NI("image_atomic_fcmpswap"); break;
		case 0x1E: KYTY_NI("image_atomic_fmin"); break;
		case 0x1F: KYTY_NI("image_atomic_fmax"); break;
		case 0x20:
			inst.type        = ShaderInstructionType::ImageSample;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask)
			{
				case 0x1:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1;
					inst.dst.size = 1;
					break;
				}
				case 0x2:
				{
					// Captured post-Play image_sample with dmask 0x2 (G only).
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2;
					inst.dst.size = 1;
					break;
				}
				case 0x3:
				{
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3;
					inst.dst.size = 2;
					break;
				}
				case 0x4:
				{
					// Captured post-Play image_sample with dmask 0x4 (B only).
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4;
					inst.dst.size = 1;
					break;
				}
				case 0x5:
				{
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5;
					inst.dst.size = 2;
					break;
				}
				case 0x7:
				{
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7;
					inst.dst.size = 3;
					break;
				}
				case 0x8:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8;
					inst.dst.size = 1;
					break;
				}
				case 0x9:
				{
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask9;
					inst.dst.size = 2;
					break;
				}
				case 0xa:
				{
					// image_sample dmask 0xa = G+A (two comps).
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmaskA;
					inst.dst.size = 2;
					break;
				}
				case 0xb:
				{
					// Captured post-NGS2 path: image_sample dmask 0xb = R+G+A (3 comps).
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr3StSsDmaskB;
					inst.dst.size = 3;
					break;
				}
				case 0xf:
				{
					inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF;
					inst.dst.size = 4;
					break;
				}
				default:;
			}
			break;
		case 0x21: KYTY_NI("image_sample_cl"); break;
		case 0x22: KYTY_NI("image_sample_d"); break;
		case 0x23: KYTY_NI("image_sample_d_cl"); break;
		case 0x24:
			inst.type        = ShaderInstructionType::ImageSampleL;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask)
			{
				case 0x7:
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7;
					inst.dst.size = 3;
					break;
				case 0xf:
					inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF;
					inst.dst.size = 4;
					break;
				default: break;
			}
			break;
		case 0x25: KYTY_NI("image_sample_b"); break;
		case 0x26: KYTY_NI("image_sample_b_cl"); break;
		case 0x27:
			inst.type        = ShaderInstructionType::ImageSampleLz;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask) // NOLINT
			{
				case 0x1:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1;
					inst.dst.size = 1;
					break;
				}
				case 0x2:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2;
					inst.dst.size = 1;
					break;
				}
				case 0x3:
				{
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3;
					inst.dst.size = 2;
					break;
				}
				case 0x7:
				{
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7;
					inst.dst.size = 3;
					break;
				}
				case 0x8:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8;
					inst.dst.size = 1;
					break;
				}
				case 0xf:
				{
					inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF;
					inst.dst.size = 4;
					break;
				}
				default:;
			}
			break;
		case 0x28: KYTY_NI("image_sample_c"); break;
		case 0x29: KYTY_NI("image_sample_c_cl"); break;
		case 0x2A: KYTY_NI("image_sample_c_d"); break;
		case 0x2B: KYTY_NI("image_sample_c_d_cl"); break;
		case 0x2C: KYTY_NI("image_sample_c_l"); break;
		case 0x2D: KYTY_NI("image_sample_c_b"); break;
		case 0x2E: KYTY_NI("image_sample_c_b_cl"); break;
		case 0x2F: KYTY_NI("image_sample_c_lz"); break;
		case 0x30: KYTY_NI("image_sample_o"); break;
		case 0x31: KYTY_NI("image_sample_cl_o"); break;
		case 0x32: KYTY_NI("image_sample_d_o"); break;
		case 0x33: KYTY_NI("image_sample_d_cl_o"); break;
		case 0x34: KYTY_NI("image_sample_l_o"); break;
		case 0x35: KYTY_NI("image_sample_b_o"); break;
		case 0x36: KYTY_NI("image_sample_b_cl_o"); break;
		case 0x37:
			inst.type        = ShaderInstructionType::ImageSampleLzO;
			inst.src[0].size = 4;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask) // NOLINT
			{
				case 0x7:
				{
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr4StSsDmask7;
					inst.dst.size = 3;
					break;
				}
				default:;
			}
			break;
		case 0x38: KYTY_NI("image_sample_c_o"); break;
		case 0x39: KYTY_NI("image_sample_c_cl_o"); break;
		case 0x3A: KYTY_NI("image_sample_c_d_o"); break;
		case 0x3B: KYTY_NI("image_sample_c_d_cl_o"); break;
		case 0x3C: KYTY_NI("image_sample_c_l_o"); break;
		case 0x3D: KYTY_NI("image_sample_c_b_o"); break;
		case 0x3E: KYTY_NI("image_sample_c_b_cl_o"); break;
		case 0x3F: KYTY_NI("image_sample_c_lz_o"); break;
		case 0x40: KYTY_NI("image_gather4"); break;
		case 0x41: KYTY_NI("image_gather4_cl"); break;
		case 0x44: KYTY_NI("image_gather4_l"); break;
		case 0x45: KYTY_NI("image_gather4_b"); break;
		case 0x46: KYTY_NI("image_gather4_b_cl"); break;
		case 0x47:
			EXIT_NOT_IMPLEMENTED(dmask == 0);
			inst.type        = ShaderInstructionType::ImageGather4;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			inst.src_num     = 3;
			inst.dst.size     = 4;
			inst.format      = ShaderInstructionFormat::Vdata4Vaddr3StSsMimgDmask;
			inst.mimg_dmask  = static_cast<uint8_t>(dmask);
			break;
		case 0x48: KYTY_NI("image_gather4_c"); break;
		case 0x49: KYTY_NI("image_gather4_c_cl"); break;
		case 0x4C: KYTY_NI("image_gather4_c_l"); break;
		case 0x4D: KYTY_NI("image_gather4_c_b"); break;
		case 0x4E: KYTY_NI("image_gather4_c_b_cl"); break;
		case 0x4F: KYTY_NI("image_gather4_c_lz"); break;
		case 0x50: KYTY_NI("image_gather4_o"); break;
		case 0x51: KYTY_NI("image_gather4_cl_o"); break;
		case 0x54: KYTY_NI("image_gather4_l_o"); break;
		case 0x55: KYTY_NI("image_gather4_b_o"); break;
		case 0x56: KYTY_NI("image_gather4_b_cl_o"); break;
		case 0x57: KYTY_NI("image_gather4_lz_o"); break;
		case 0x58: KYTY_NI("image_gather4_c_o"); break;
		case 0x59: KYTY_NI("image_gather4_c_cl_o"); break;
		case 0x5C: KYTY_NI("image_gather4_c_l_o"); break;
		case 0x5D: KYTY_NI("image_gather4_c_b_o"); break;
		case 0x5E: KYTY_NI("image_gather4_c_b_cl_o"); break;
		case 0x5F: KYTY_NI("image_gather4_c_lz_o"); break;
		case 0x60: KYTY_NI("image_get_lod"); break;
		case 0x68: KYTY_NI("image_sample_cd"); break;
		case 0x69: KYTY_NI("image_sample_cd_cl"); break;
		case 0x6A: KYTY_NI("image_sample_c_cd"); break;
		case 0x6B: KYTY_NI("image_sample_c_cd_cl"); break;
		case 0x6C: KYTY_NI("image_sample_cd_o"); break;
		case 0x6D: KYTY_NI("image_sample_cd_cl_o"); break;
		case 0x6E: KYTY_NI("image_sample_c_cd_o"); break;
		case 0x6F: KYTY_NI("image_sample_c_cd_cl_o"); break;
		case 0x7E: KYTY_NI("image_rsrc256"); break;
		case 0x7F: KYTY_NI("image_sampler"); break;
		case 0xA0: KYTY_NI("image_sample_a"); break;
		case 0xA1: KYTY_NI("image_sample_cl_a"); break;
		case 0xA5: KYTY_NI("image_sample_b_a"); break;
		case 0xA6: KYTY_NI("image_sample_b_cl_a"); break;
		case 0xA8: KYTY_NI("image_sample_c_a"); break;
		case 0xA9: KYTY_NI("image_sample_c_cl_a"); break;
		case 0xAD: KYTY_NI("image_sample_c_b_a"); break;
		case 0xAE: KYTY_NI("image_sample_c_b_cl_a"); break;
		case 0xB0: KYTY_NI("image_sample_o_a"); break;
		case 0xB1: KYTY_NI("image_sample_cl_o_a"); break;
		case 0xB5: KYTY_NI("image_sample_b_o_a"); break;
		case 0xB6: KYTY_NI("image_sample_b_cl_o_a"); break;
		case 0xB8: KYTY_NI("image_sample_c_o_a"); break;
		case 0xB9: KYTY_NI("image_sample_c_cl_o_a"); break;
		case 0xBD: KYTY_NI("image_sample_c_b_o_a"); break;
		case 0xBE: KYTY_NI("image_sample_c_b_cl_o_a"); break;
		case 0xC0: KYTY_NI("image_gather4_a"); break;
		case 0xC1: KYTY_NI("image_gather4_cl_a"); break;
		case 0xC5: KYTY_NI("image_gather4_b_a"); break;
		case 0xC6: KYTY_NI("image_gather4_b_cl_a"); break;
		case 0xC8: KYTY_NI("image_gather4_c_a"); break;
		case 0xC9: KYTY_NI("image_gather4_c_cl_a"); break;
		case 0xCD: KYTY_NI("image_gather4_c_b_a"); break;
		case 0xCE: KYTY_NI("image_gather4_c_b_cl_a"); break;
		case 0xD0: KYTY_NI("image_gather4_o_a"); break;
		case 0xD1: KYTY_NI("image_gather4_cl_o_a"); break;
		case 0xD5: KYTY_NI("image_gather4_b_o_a"); break;
		case 0xD6: KYTY_NI("image_gather4_b_cl_o_a"); break;
		case 0xD8: KYTY_NI("image_gather4_c_o_a"); break;
		case 0xD9: KYTY_NI("image_gather4_c_cl_o_a"); break;
		case 0xDD: KYTY_NI("image_gather4_c_b_o_a"); break;
		case 0xDE: KYTY_NI("image_gather4_c_b_cl_o_a"); break;

		default: KYTY_UNKNOWN_OP();
	}

	if (inst.format == ShaderInstructionFormat::Unknown)
	{
		KYTY_LOG_DEBUG("%s", dst->DbgDump().c_str());
		EXIT("unknown mimg format for opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 ", dmask: 0x%" PRIx32 "\n", opcode, pc, dmask);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
