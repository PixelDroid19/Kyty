#include "Emulator/Graphics/ShaderComputeWaveWaitcnt.h"

#include "Emulator/Graphics/ShaderComputeWaveResourceAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveScalarBuffer.h"
#include "Kyty/Core/MagicEnum.h"

#include "ShaderStorageAnalysis.h"

#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint8_t  kSoppWaitcntOpcode     = 0x0cu;
constexpr uint32_t kLgkmZeroOnlyImmediate = 0xc07fu;
constexpr int      kMimgAddressSlots      = 13;

ShaderComputeWaveAnalysisResult Failure(uint32_t pc, const String8& reason)
{
	ShaderComputeWaveAnalysisResult result {};
	result.unsupported_pc = pc;
	result.reason         = reason;
	return result;
}

bool OperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool OperandIsUnused(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Unknown && operand.size == 0 && OperandIsPlain(operand);
}

bool IsExactSoppWaitTuple(const ShaderInstruction& instruction)
{
	if (instruction.type != ShaderInstructionType::SWaitcnt || instruction.sopp_opcode != kSoppWaitcntOpcode ||
	    instruction.format != ShaderInstructionFormat::Imm || instruction.src_num != 1 || !OperandIsUnused(instruction.dst) ||
	    !OperandIsUnused(instruction.dst2) || instruction.src[0].type != ShaderOperandType::LiteralConstant ||
	    instruction.src[0].size != 0 || !OperandIsPlain(instruction.src[0]) || instruction.vop3_op_sel != 0u ||
	    instruction.vop3_omod != 0u || instruction.vop_sdwa || instruction.ds_offset != 0u || instruction.ds_encoding_control != 0u ||
	    instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	for (int source = 1; source < 4; ++source)
	{
		if (!OperandIsUnused(instruction.src[source]))
		{
			return false;
		}
	}
	return true;
}

bool IsLgkmZeroOnlyWait(const ShaderInstruction& instruction)
{
	return IsExactSoppWaitTuple(instruction) && instruction.src[0].constant.u == kLgkmZeroOnlyImmediate;
}

// SALU, VALU and instruction-prefetch kinds admitted by the paired subset do
// not issue LDS, GDS, constant-fetch or message operations.
bool KindIssuesNoLgkmOperation(ShaderComputeWaveInstructionKind kind)
{
	switch (kind)
	{
		case ShaderComputeWaveInstructionKind::ScalarHint:
		case ShaderComputeWaveInstructionKind::ScalarCopy:
		case ShaderComputeWaveInstructionKind::ScalarShift:
		case ShaderComputeWaveInstructionKind::ScalarMask:
		case ShaderComputeWaveInstructionKind::BankedVector:
		case ShaderComputeWaveInstructionKind::BankedAlu:
		case ShaderComputeWaveInstructionKind::PackedMask:
		case ShaderComputeWaveInstructionKind::PackedExecMask:
		case ShaderComputeWaveInstructionKind::WaveLane:
		case ShaderComputeWaveInstructionKind::BankedBufferLoad:
		case ShaderComputeWaveInstructionKind::BankedSdwaExtract:
		case ShaderComputeWaveInstructionKind::BankedGeneric:
		case ShaderComputeWaveInstructionKind::BankedGenericLds:
		case ShaderComputeWaveInstructionKind::ScalarGeneric:
		case ShaderComputeWaveInstructionKind::BankedGenericCompare:
		case ShaderComputeWaveInstructionKind::BankedCarry:
		// EXEC branches only gate which PCs execute; they issue no LGKM
		// operation, and real mid-window entries stay rejected by the label
		// scan below.
		case ShaderComputeWaveInstructionKind::WaveBranch: return true;
		default: return false;
	}
}

// Every operand slot is inspected, including unused ones, so a malformed
// source count cannot hide an access.
bool InstructionAccessesSgprRange(const ShaderInstruction& instruction, int start_register, int registers_num)
{
	if (ShaderOperandOverlapsSgprRange(instruction.dst, start_register, registers_num) ||
	    ShaderOperandOverlapsSgprRange(instruction.dst2, start_register, registers_num))
	{
		return true;
	}
	for (const auto& source: instruction.src)
	{
		if (ShaderOperandOverlapsSgprRange(source, start_register, registers_num))
		{
			return true;
		}
	}
	for (int address = 0; address < kMimgAddressSlots; ++address)
	{
		if (ShaderOperandOverlapsSgprRange(instruction.mimg_address[address], start_register, registers_num))
		{
			return true;
		}
	}
	return false;
}

// VCC_LO operands of either width cover the low VCC word.
bool InstructionAccessesVccLo(const ShaderInstruction& instruction)
{
	if (instruction.dst.type == ShaderOperandType::VccLo || instruction.dst2.type == ShaderOperandType::VccLo)
	{
		return true;
	}
	for (const auto& source: instruction.src)
	{
		if (source.type == ShaderOperandType::VccLo)
		{
			return true;
		}
	}
	for (int address = 0; address < kMimgAddressSlots; ++address)
	{
		if (instruction.mimg_address[address].type == ShaderOperandType::VccLo)
		{
			return true;
		}
	}
	return false;
}

// Admitted synchronous producers write an ordinary SGPR range or VCC_LO.
bool InstructionAccessesDestination(const ShaderInstruction& instruction, const ShaderOperand& destination)
{
	if (destination.type == ShaderOperandType::Sgpr)
	{
		return InstructionAccessesSgprRange(instruction, destination.register_id, destination.size);
	}
	return destination.type != ShaderOperandType::VccLo || InstructionAccessesVccLo(instruction);
}

} // namespace

bool ShaderComputeWaveIsExactWait(const ShaderInstruction& instruction)
{
	return IsExactSoppWaitTuple(instruction);
}

bool ShaderComputeWaveIsLgkmZeroOnlyWait(const ShaderInstruction& instruction)
{
	return IsLgkmZeroOnlyWait(instruction);
}

ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveWaitcnt(const ShaderCode& code, uint32_t index, const ShaderBindResources& bind)
{
	const auto& instructions = code.GetInstructions();
	if (index >= instructions.Size())
	{
		return Failure(0, "SWaitcnt admission index is outside the parsed program");
	}
	const auto& wait = instructions.At(index);
	if (!IsExactSoppWaitTuple(wait))
	{
		return Failure(wait.pc, "SWaitcnt requires SOPP opcode 0x0c with one plain literal immediate");
	}
	if (wait.src[0].constant.u != kLgkmZeroOnlyImmediate)
	{
		// Pure VMEM-count waits carry expcnt and lgkmcnt at their field maxima:
		// {vm_hi[15:14]=*, lgkm[13:8]=0x3f, exp[6:4]=7, vm_lo[3:0]=*}. Every VMEM
		// operation the paired subset admits is a banked buffer load lowered
		// synchronously at its own PC, so the pending count is already under any
		// admitted threshold and no window state has to be tracked. Any other
		// field combination (a partial LGKM wait, a pending export count, the
		// all-zero drain) still needs a dedicated contract.
		if ((wait.src[0].constant.u & 0x3ff0u) == 0x3f70u)
		{
			return {true, 0, {}};
		}
		return Failure(wait.pc, String8::FromPrintf("SWaitcnt immediate 0x%04" PRIx32 " is outside the admitted lgkmcnt(0)-only form "
		                                            "0xc07f and the pure vmcnt(N) form 0x3f7x",
		                                            wait.src[0].constant.u));
	}

	uint32_t first = 0;
	for (uint32_t cursor = index; cursor > 0; --cursor)
	{
		if (IsLgkmZeroOnlyWait(instructions.At(cursor - 1)))
		{
			first = cursor;
			break;
		}
	}
	// Only real branch targets can join the window mid-way: indirect labels
	// are the parser's fall-through edges (dst = src + 4), which are the
	// sequential path itself and skip nothing inside the window. A target is
	// hazardous only when it lands strictly after the earliest pending
	// producer — such a path can reach the consumers without ever running the
	// load. Targets at or before that producer still execute it on entry, so
	// the forbidden zone starts at the first synchronous SMEM instruction in
	// the window, not at the last drain.
	auto is_pending_producer = [&code, &bind](uint32_t cursor) {
		const auto& instruction = code.GetInstructions().At(cursor);
		return ShaderPairedEudStorageLoadSupported(instruction, bind) ||
		       (instruction.type == ShaderInstructionType::SBufferLoadDword &&
		        ShaderAnalyzeComputeWaveScalarBufferLoad(code, cursor, bind).supported);
	};
	uint32_t producer_pc = wait.pc;
	for (uint32_t cursor = first; cursor < index; ++cursor)
	{
		if (is_pending_producer(cursor))
		{
			producer_pc = instructions.At(cursor).pc;
			break;
		}
	}
	for (const auto& label: code.GetLabels())
	{
		if (!label.IsDisabled() && label.GetDst() > producer_pc && label.GetDst() <= wait.pc)
		{
			return Failure(wait.pc, String8::FromPrintf("SWaitcnt pending LGKM window has a branch target at pc=0x%08" PRIx32
			                                            " from pc=0x%08" PRIx32,
			                                            label.GetDst(), label.GetSrc()));
		}
	}

	for (uint32_t cursor = first; cursor < index; ++cursor)
	{
		const auto& instruction = instructions.At(cursor);
		const bool  synchronous_smem = is_pending_producer(cursor);
		if (synchronous_smem)
		{
			for (uint32_t later = cursor + 1; later < index; ++later)
			{
				if (InstructionAccessesDestination(instructions.At(later), instruction.dst))
				{
					return Failure(instructions.At(later).pc,
					               String8::FromPrintf("instruction accesses the pending %s destination of pc=0x%08" PRIx32
					                                   " before its covering SWaitcnt at pc=0x%08" PRIx32,
					                                   Core::EnumName8(instruction.type).c_str(), instruction.pc, wait.pc));
				}
			}
			continue;
		}
		// A pure vmcnt wait already admitted inside the window is transparent:
		// it issues no LGKM operation and never drains one. lgkmcnt(0) cannot
		// appear here because it would have closed the window at `first`.
		if (IsExactSoppWaitTuple(instruction) && (instruction.src[0].constant.u & 0x3ff0u) == 0x3f70u)
		{
			continue;
		}
		const auto kind = ShaderClassifyComputeWaveInstruction(instruction);
		if (kind == ShaderComputeWaveInstructionKind::BankedLds)
		{
			return Failure(wait.pc, String8::FromPrintf("SWaitcnt would wait on an LDS operation at pc=0x%08" PRIx32
			                                            " whose completion is not modeled here",
			                                            instruction.pc));
		}
		if (!KindIssuesNoLgkmOperation(kind))
		{
			return Failure(wait.pc, String8::FromPrintf("SWaitcnt pending LGKM window contains unmodeled instruction %s at pc=0x%08" PRIx32,
			                                            Core::EnumName8(instruction.type).c_str(), instruction.pc));
		}
	}
	return {true, 0, {}};
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
