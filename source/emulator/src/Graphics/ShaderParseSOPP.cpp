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
	inst.pc          = pc;
	inst.sopp_opcode = static_cast<uint8_t>(opcode);

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
		case 0x09: inst.type = ShaderInstructionType::SCbranchExecnz; break;
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
		case 0xA:
			inst.type    = ShaderInstructionType::SBarrier;
			inst.format  = ShaderInstructionFormat::Empty;
			inst.src_num = 0;
			inst.src[0]  = {};
			break;
		case 0xB: KYTY_NI("s_setkill"); break;
		case 0xD: KYTY_NI("s_sethalt"); break;
		case 0x11: KYTY_NI("s_sendmsghalt"); break;
		case 0x12:
			// Traps are not taken without a trap handler (STATUS.TRAP_EN = 0).
			inst.type              = ShaderInstructionType::STrap;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
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
		case 0xE:  // s_sleep
		case 0xF:  // s_setprio
		case 0x13: // s_icache_inv
		case 0x14: // s_incperflevel
		case 0x15: // s_decperflevel
		case 0x17: // s_cbranch_cdbgsys
		case 0x18: // s_cbranch_cdbguser
		case 0x19: // s_cbranch_cdbgsys_or_user
		case 0x1A: // s_cbranch_cdbgsys_and_user
			// Scheduling, cache and performance hints have no architectural effect on a
			// translated shader, and with no attached GPU debugger COND_DBG_SYS/USER stay
			// clear so the debug branches are never taken. Emitters skip these Imm records.
			inst.type              = ShaderInstructionType::SBarrier;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			break;

		default: KYTY_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	if (inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchScc1 ||
	    inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz ||
	    inst.type == ShaderInstructionType::SCbranchExecz || inst.type == ShaderInstructionType::SCbranchExecnz ||
	    inst.type == ShaderInstructionType::SBranch)
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
