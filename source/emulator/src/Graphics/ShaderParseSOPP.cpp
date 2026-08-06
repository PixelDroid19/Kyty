#include "ShaderParseInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_SHADER_PARSER(shader_parse_sopp)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	KYTY_TYPE_STR("sopp");

	uint32_t opcode = (buffer[0] >> 16u) & 0x7fu;
	uint32_t simm   = (buffer[0] >> 0u) & 0xffffu;

	ShaderInstruction inst;
	inst.pc = pc;

	inst.format            = ShaderInstructionFormat::Label;
	inst.src[0].type       = ShaderOperandType::LiteralConstant;
	inst.src[0].constant.i = static_cast<int16_t>(simm) * 4;
	inst.src_num           = 1;

	switch (opcode)
	{
		case 0x01:
			inst.type    = ShaderInstructionType::SEndpgm;
			inst.format  = ShaderInstructionFormat::Empty;
			inst.src_num = 0;
			break;
		case 0x02: inst.type = ShaderInstructionType::SBranch; break;
		case 0x04: inst.type = ShaderInstructionType::SCbranchScc0; break;
		case 0x05: inst.type = ShaderInstructionType::SCbranchScc1; break;
		case 0x06: inst.type = ShaderInstructionType::SCbranchVccz; break;
		case 0x07: inst.type = ShaderInstructionType::SCbranchVccnz; break;
		case 0x08: inst.type = ShaderInstructionType::SCbranchExecz; break;
		case 0x0c:
			inst.type              = ShaderInstructionType::SWaitcnt;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
			break;
		case 0x10:
			inst.type              = ShaderInstructionType::SSendmsg;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
			break;
		case 0x20:
			inst.type              = ShaderInstructionType::SInstPrefetch;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
			break;

		case 0x0:
			// s_nop: hardware no-op — model as a skipped scalar instruction.
			inst.type              = ShaderInstructionType::SInstPrefetch;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
			break;
		case 0x9: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_cbranch_execnz treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0xA:
			if (simm != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: simm != 0 condition ignored (continuing)\n"); }
			inst.type    = ShaderInstructionType::SBarrier;
			inst.format  = ShaderInstructionFormat::Empty;
			inst.src_num = 0;
			break;
		case 0xB: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_setkill treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0xD: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_sethalt treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0xE: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_sleep treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0xF: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_setprio treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x11: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_sendmsghalt treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x12: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_trap treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x13: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_icache_inv treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x14: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_incperflevel treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x15: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_decperflevel treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x16:
			// s_ttracedata only feeds the hardware thread-trace stream. It has no
			// architectural effect on shader registers, memory, or control flow.
			inst.type              = ShaderInstructionType::SInstPrefetch;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
			break;
		case 0x17: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_cbranch_cdbgsys treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x18: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_cbranch_cdbguser treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x19: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_cbranch_cdbgsys_or_user treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;
		case 0x1A: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: s_cbranch_cdbgsys_and_user treated as SBarrier (continuing)\n");
			inst.type = ShaderInstructionType::SBarrier;
			inst.format = ShaderInstructionFormat::Unknown;
			break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	if (inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchScc1 ||
	    inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz ||
	    inst.type == ShaderInstructionType::SCbranchExecz || inst.type == ShaderInstructionType::SBranch)
	{
		dst->GetLabels().Add(ShaderLabel(inst));

		if (inst.type != ShaderInstructionType::SBranch)
		{
			dst->GetIndirectLabels().Add(ShaderLabel(inst.pc + 4, inst.pc));
		}
	}

	return 1;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED