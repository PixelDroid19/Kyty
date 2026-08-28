#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_sop1)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("sop1");

	uint32_t opcode = (buffer[0] >> 8u) & 0xffu;
	uint32_t ssrc0  = (buffer[0] >> 0u) & 0xffu;
	uint32_t sdst   = (buffer[0] >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(ssrc0);
	inst.src_num = 1;
	inst.dst     = operand_parse(sdst);

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = buffer[size];
		size++;
	}

	switch (opcode)
	{
		case 0x03:
			inst.type   = ShaderInstructionType::SMovB32;
			inst.format = ShaderInstructionFormat::SVdstSVsrc0;
			break;
		case 0x04:
			inst.type        = ShaderInstructionType::SMovB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x05:
			inst.type        = ShaderInstructionType::SCmovB32;
			inst.format      = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src[1]      = inst.dst;
			inst.src_num     = 2;
			break;
		case 0x06:
			inst.type        = ShaderInstructionType::SCmovB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1]      = inst.dst;
			inst.src[1].size = 2;
			inst.src_num     = 2;
			break;
		case 0x07:
			inst.type   = ShaderInstructionType::SNotB32;
			inst.format = ShaderInstructionFormat::SVdstSVsrc0;
			break;
		case 0x08:
			inst.type        = ShaderInstructionType::SNotB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x09: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_wqm_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x0a:
			inst.type        = ShaderInstructionType::SWqmB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x0B:
			inst.type   = ShaderInstructionType::SBrevB32;
			inst.format = ShaderInstructionFormat::SVdstSVsrc0;
			break;
		case 0x0C: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_brev_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x0D: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_bcnt0_i32_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x0E: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_bcnt0_i32_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x0F: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_bcnt1_i32_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x10: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_bcnt1_i32_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x11: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_ff0_i32_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x12: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_ff0_i32_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x13: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_ff1_i32_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x14: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_ff1_i32_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x15: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_flbit_i32_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x16: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_flbit_i32_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x17: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_flbit_i32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x18: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_flbit_i32_i64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x19: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_sext_i32_i8 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x1A: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_sext_i32_i16 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x1B: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_bitset0_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x1C: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_bitset0_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x1D: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_bitset1_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x1E: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_bitset1_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x1F: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_getpc_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x20:
			inst.type        = ShaderInstructionType::SSetpcB64;
			inst.format      = ShaderInstructionFormat::Saddr;
			inst.src[0].size = 2;
			break;
		case 0x21:
			inst.type        = ShaderInstructionType::SSwappcB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.src[0].size = 2;
			inst.dst.size    = 2;
			break;
		case 0x22: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_rfe_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x24:
			inst.type        = ShaderInstructionType::SAndSaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x25:
			inst.type        = ShaderInstructionType::SOrSaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x26:
			inst.type        = ShaderInstructionType::SXorSaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x27:
			inst.type        = ShaderInstructionType::SAndn2SaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x28:
			inst.type        = ShaderInstructionType::SOrn2SaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x29:
			inst.type        = ShaderInstructionType::SNandSaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x2A:
			inst.type        = ShaderInstructionType::SNorSaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x2B:
			inst.type        = ShaderInstructionType::SXnorSaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x2C: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_quadmask_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x2D: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_quadmask_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x2E: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_movrels_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x2F: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_movrels_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x30: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_movreld_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x31: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_movreld_b64 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x32: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_cbranch_join treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x33: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_mov_regrd_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x34: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_abs_i32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x35: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_mov_fed_b32 treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x37:
			inst.type        = ShaderInstructionType::SAndn1SaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
