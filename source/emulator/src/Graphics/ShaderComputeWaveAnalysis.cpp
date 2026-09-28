#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveAlu.h"
#include "Emulator/Graphics/ShaderComputeWaveControlFlowAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveLds.h"
#include "Emulator/Graphics/ShaderComputeWaveResourceAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveScalarBuffer.h"
#include "Emulator/Graphics/ShaderComputeWaveSdwa.h"
#include "Emulator/Graphics/ShaderComputeWaveVectorBuffer.h"
#include "Emulator/Graphics/ShaderComputeWaveWaitcnt.h"
#include "Kyty/Core/MagicEnum.h"

#include "ShaderStorageAnalysis.h"

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {
namespace {

constexpr int kMaxSgpr                  = 103;
constexpr int kMaxVgpr                  = 255;
constexpr int kMinIntegerInlineConstant = -16;
constexpr int kMaxIntegerInlineConstant = 64;

bool ComputeWaveOperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool ComputeWaveRegisterRangeIsValid(int register_id, int size, int maximum_register)
{
	if (register_id < 0 || size <= 0 || register_id > maximum_register)
	{
		return false;
	}

	return size <= maximum_register - register_id + 1;
}

bool IsOrdinarySgpr(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Sgpr && operand.size == 1 && ComputeWaveOperandIsPlain(operand) &&
	       ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, kMaxSgpr);
}

bool IsOrdinaryVgpr(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Vgpr && operand.size == 1 && ComputeWaveOperandIsPlain(operand) &&
	       ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, kMaxVgpr);
}

bool IsScalarPairVariable(const ShaderOperand& operand)
{
	if (!ComputeWaveOperandIsPlain(operand) || operand.size != 2)
	{
		return false;
	}

	switch (operand.type)
	{
		case ShaderOperandType::Sgpr:
			return ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, kMaxSgpr) && (operand.register_id % 2 == 0);
		case ShaderOperandType::VccLo:
		case ShaderOperandType::ExecLo: return operand.register_id == 0;
		default: return false;
	}
}

bool IsIntegerInlineConstantPair(const ShaderOperand& operand)
{
	return ComputeWaveOperandIsPlain(operand) && operand.type == ShaderOperandType::IntegerInlineConstant && operand.size == 2 &&
	       operand.constant.i >= kMinIntegerInlineConstant && operand.constant.i <= kMaxIntegerInlineConstant;
}

bool IsIntegerOrLiteralConstant(const ShaderOperand& operand)
{
	return ComputeWaveOperandIsPlain(operand) && operand.size == 0 &&
	       (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant);
}

bool IsTask3MaskSource(const ShaderOperand& operand)
{
	if (IsOrdinarySgpr(operand) || IsOrdinaryVgpr(operand))
	{
		return true;
	}
	return ComputeWaveOperandIsPlain(operand) && operand.size == 0 &&
	       (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant ||
	        operand.type == ShaderOperandType::FloatInlineConstant);
}

bool IsMaskDestination(const ShaderOperand& operand)
{
	if (!ComputeWaveOperandIsPlain(operand) || operand.size != 2)
	{
		return false;
	}

	if (operand.type == ShaderOperandType::VccLo)
	{
		return operand.register_id == 0;
	}
	return operand.type == ShaderOperandType::Sgpr && ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, kMaxSgpr);
}

bool IsUnusedDestination(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Unknown && operand.size == 0 && ComputeWaveOperandIsPlain(operand);
}

bool IsExactTupleBase(const ShaderInstruction& instruction, ShaderInstructionFormat::Format format, int source_count)
{
	return instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u && !instruction.vop_sdwa && instruction.format == format &&
	       instruction.src_num == source_count && IsUnusedDestination(instruction.dst2);
}

bool SgprPairAliasesSource(const ShaderOperand& destination, const ShaderOperand& source)
{
	if (destination.type != ShaderOperandType::Sgpr || source.type != ShaderOperandType::Sgpr || destination.register_id < 0 ||
	    source.register_id < 0)
	{
		return false;
	}

	return source.register_id == destination.register_id || source.register_id == destination.register_id + 1;
}

bool IsPackedMaskInstruction(const ShaderInstruction& instruction)
{
	if (!IsExactTupleBase(instruction, ShaderInstructionFormat::SmaskVsrc0Vsrc1, 2) || !IsMaskDestination(instruction.dst) ||
	    !IsTask3MaskSource(instruction.src[0]) || !IsTask3MaskSource(instruction.src[1]))
	{
		return false;
	}

	return !SgprPairAliasesSource(instruction.dst, instruction.src[0]) && !SgprPairAliasesSource(instruction.dst, instruction.src[1]);
}

// VOPC SDWAB compare with DWORD selects on both sources: identical value
// semantics to the plain packed-mask compare; the control dword may only
// retarget the mask destination (SD/SDST) and mark source register files.
bool IsPackedMaskSdwaInstruction(const ShaderInstruction& instruction)
{
	if (!ShaderComputeWaveSdwaCompareTupleSupported(instruction) || !IsUnusedDestination(instruction.dst2) ||
	    !IsMaskDestination(instruction.dst) || !IsTask3MaskSource(instruction.src[0]) || !IsTask3MaskSource(instruction.src[1]))
	{
		return false;
	}
	return !SgprPairAliasesSource(instruction.dst, instruction.src[0]) && !SgprPairAliasesSource(instruction.dst, instruction.src[1]);
}

bool IsScalarCopyInstruction(const ShaderInstruction& instruction)
{
	return IsExactTupleBase(instruction, ShaderInstructionFormat::SVdstSVsrc0, 1) && IsOrdinarySgpr(instruction.dst) &&
	       (IsOrdinarySgpr(instruction.src[0]) || IsIntegerOrLiteralConstant(instruction.src[0]));
}

bool IsScalarPairCopyInstruction(const ShaderInstruction& instruction)
{
	return IsExactTupleBase(instruction, ShaderInstructionFormat::Sdst2Ssrc02, 1) && IsScalarPairVariable(instruction.dst) &&
	       (IsScalarPairVariable(instruction.src[0]) || IsIntegerInlineConstantPair(instruction.src[0]));
}

bool IsScalarShiftInstruction(const ShaderInstruction& instruction)
{
	if (!IsExactTupleBase(instruction, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, 2) || instruction.dst.type != ShaderOperandType::VccHi ||
	    instruction.dst.register_id != 0 || instruction.dst.size != 1 || !ComputeWaveOperandIsPlain(instruction.dst) ||
	    !IsOrdinarySgpr(instruction.src[0]))
	{
		return false;
	}

	const auto& shift = instruction.src[1];
	if (shift.type != ShaderOperandType::IntegerInlineConstant || shift.size != 0 || !ComputeWaveOperandIsPlain(shift) ||
	    shift.constant.i < 0 || shift.constant.i > 31 || instruction.ds_offset != 0u || instruction.ds_encoding_control != 0u ||
	    instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	for (int source = 2; source < 4; ++source)
	{
		if (!IsUnusedDestination(instruction.src[source]))
		{
			return false;
		}
	}
	return true;
}

bool IsScalarMaskInstruction(const ShaderInstruction& instruction)
{
	const bool save  = instruction.type == ShaderInstructionType::SAndSaveexecB64;
	// SNotB64 is the unary mask complement: one scalar pair source, no save.
	const bool unary = save || instruction.type == ShaderInstructionType::SNotB64;
	const int source_count = unary ? 1 : 2;
	const auto format = unary ? ShaderInstructionFormat::Sdst2Ssrc02 : ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
	if (!IsExactTupleBase(instruction, format, source_count) || !IsScalarPairVariable(instruction.dst) ||
	    (save && instruction.dst.type != ShaderOperandType::Sgpr) || instruction.ds_offset != 0 ||
	    instruction.ds_encoding_control != 0 || instruction.ds_encoding_registers != 0)
	{
		return false;
	}
	for (int source = 0; source < 4; ++source)
	{
		const auto& operand = instruction.src[source];
		if (source < source_count ? !(IsScalarPairVariable(operand) || IsIntegerInlineConstantPair(operand))
		                          : !IsUnusedDestination(operand))
		{
			return false;
		}
	}
	return true;
}

bool IsBankedVectorInstruction(const ShaderInstruction& instruction)
{
	return IsExactTupleBase(instruction, ShaderInstructionFormat::SVdstSVsrc0, 1) && IsOrdinaryVgpr(instruction.dst) &&
	       (IsOrdinaryVgpr(instruction.src[0]) || IsOrdinarySgpr(instruction.src[0]) || IsIntegerOrLiteralConstant(instruction.src[0]));
}

// A wave-uniform lane read may target an SGPR or one VCC word.
bool IsUniformScalarWord(const ShaderOperand& operand)
{
	return IsOrdinarySgpr(operand) || (operand.size == 1 && operand.register_id == 0 && ComputeWaveOperandIsPlain(operand) &&
	                                   (operand.type == ShaderOperandType::VccLo || operand.type == ShaderOperandType::VccHi));
}

bool IsScalarBitfieldMaskInstruction(const ShaderInstruction& instruction)
{
	if (instruction.sopp_opcode != 0xffu ||
	    !IsExactTupleBase(instruction, ShaderInstructionFormat::SmaskVsrc0Vsrc1, 2) ||
	    !IsScalarPairVariable(instruction.dst) || instruction.ds_offset != 0u ||
	    instruction.ds_encoding_control != 0u || instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	const auto scalar_word = [](const ShaderOperand& source)
	{
		if (IsUniformScalarWord(source) || IsIntegerOrLiteralConstant(source))
		{
			return true;
		}
		return source.size == 1 && source.register_id == 0 && ComputeWaveOperandIsPlain(source) &&
		       (source.type == ShaderOperandType::ExecLo || source.type == ShaderOperandType::ExecHi);
	};
	return scalar_word(instruction.src[0]) && scalar_word(instruction.src[1]) &&
	       IsUnusedDestination(instruction.src[2]) && IsUnusedDestination(instruction.src[3]);
}

bool IsReadlaneInstruction(const ShaderInstruction& instruction)
{
	// The emitter reads data and selector before its single store, so the
	// destination may alias the selector.
	const auto& selector = instruction.src[1];
	return IsExactTupleBase(instruction, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, 2) && IsUniformScalarWord(instruction.dst) &&
	       IsOrdinaryVgpr(instruction.src[0]) && (IsUniformScalarWord(selector) || IsIntegerOrLiteralConstant(selector));
}

bool IsWritelaneInstruction(const ShaderInstruction& instruction)
{
	const auto uniform = [](const ShaderOperand& operand) { return IsUniformScalarWord(operand) || IsIntegerOrLiteralConstant(operand); };
	return IsExactTupleBase(instruction, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, 2) && IsOrdinaryVgpr(instruction.dst) &&
	       uniform(instruction.src[0]) && uniform(instruction.src[1]);
}

bool IsReadfirstlaneInstruction(const ShaderInstruction& instruction)
{
	return IsExactTupleBase(instruction, ShaderInstructionFormat::SVdstSVsrc0, 1) && IsUniformScalarWord(instruction.dst) &&
	       IsOrdinaryVgpr(instruction.src[0]);
}

bool IsEndInstruction(const ShaderInstruction& instruction)
{
	return IsExactTupleBase(instruction, ShaderInstructionFormat::Empty, 0) && IsUnusedDestination(instruction.dst);
}

bool IsInstructionPrefetchHint(const ShaderInstruction& instruction)
{
	// RDNA2 defines modes 1..3 as instruction-cache lookahead only. The
	// existing SPIR-V emitter drops this performance hint without state effects.
	if (instruction.sopp_opcode != 0x20u || !IsExactTupleBase(instruction, ShaderInstructionFormat::Imm, 1) ||
	    !IsUnusedDestination(instruction.dst) ||
	    instruction.src[0].type != ShaderOperandType::LiteralConstant || instruction.src[0].size != 0 ||
	    !ComputeWaveOperandIsPlain(instruction.src[0]) || instruction.src[0].constant.u < 1u ||
	    instruction.src[0].constant.u > 3u || instruction.ds_offset != 0u || instruction.ds_encoding_control != 0u ||
	    instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	for (int source = 1; source < 4; ++source)
	{
		if (!IsUnusedDestination(instruction.src[source]))
		{
			return false;
		}
	}
	return true;
}

bool IsBarrierInstruction(const ShaderInstruction& instruction)
{
	// S_BARRIER ignores its SIMM16. A paired wave is one converged subgroup and
	// the guest guarantees every wave reaches it, so any immediate is admitted.
	if (instruction.sopp_opcode != 0x0au || !IsExactTupleBase(instruction, ShaderInstructionFormat::Empty, 0) ||
	    !IsUnusedDestination(instruction.dst) || instruction.ds_offset != 0u || instruction.ds_encoding_control != 0u ||
	    instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	for (const auto& source: instruction.src)
	{
		if (!IsUnusedDestination(source))
		{
			return false;
		}
	}
	return true;
}

bool ComputeWaveOperandMayOverwriteSgprPair(const ShaderOperand& destination, int pair_start)
{
	if (destination.type != ShaderOperandType::Sgpr)
	{
		return false;
	}
	if (!ComputeWaveRegisterRangeIsValid(destination.register_id, destination.size, kMaxSgpr))
	{
		return true;
	}
	return ShaderOperandOverlapsSgprRange(destination, pair_start, 2);
}

bool ComputeWaveInstructionMayOverwriteEudBase(const ShaderInstruction& instruction, int pair_start)
{
	return ComputeWaveOperandMayOverwriteSgprPair(instruction.dst, pair_start) ||
	       ComputeWaveOperandMayOverwriteSgprPair(instruction.dst2, pair_start);
}

bool ComputeWaveOperandMayReadSgprPair(const ShaderOperand& source, int pair_start)
{
	if (source.type != ShaderOperandType::Sgpr)
	{
		return false;
	}
	if (!ComputeWaveRegisterRangeIsValid(source.register_id, source.size, kMaxSgpr))
	{
		return true;
	}
	return ShaderOperandOverlapsSgprRange(source, pair_start, 2);
}

bool ComputeWaveInstructionMayReadEudBase(const ShaderInstruction& instruction, int pair_start,
	                                      const ShaderBindResources& bind)
{
	if (instruction.src_num < 0 || instruction.src_num > 4 || instruction.mimg_address_num < 0 ||
	    instruction.mimg_address_num > 13)
	{
		return true;
	}
	// Scalar loads through the extended pointer resolve through the native
	// per-PC EUD mapping; the emitter fails closed when a PC is unmapped.
	const auto name        = Core::EnumName8(instruction.type);
	const bool mapped_load = ShaderPairedEudStorageLoadSupported(instruction, bind) || name.StartsWith("SLoad");
	for (int source = 0; source < instruction.src_num; ++source)
	{
		if (mapped_load && source == 0)
		{
			continue;
		}
		if (ComputeWaveOperandMayReadSgprPair(instruction.src[source], pair_start))
		{
			return true;
		}
	}
	for (int address = 0; address < instruction.mimg_address_num; ++address)
	{
		if (ComputeWaveOperandMayReadSgprPair(instruction.mimg_address[address], pair_start))
		{
			return true;
		}
	}
	return false;
}

bool IsWaveBranchInstruction(const ShaderInstruction& instruction)
{
	if (!IsExactTupleBase(instruction, ShaderInstructionFormat::Label, 1) || !IsUnusedDestination(instruction.dst) ||
	    instruction.src[0].type != ShaderOperandType::LiteralConstant || instruction.src[0].size != 0 ||
	    !ComputeWaveOperandIsPlain(instruction.src[0]) || instruction.ds_offset != 0u ||
	    instruction.ds_encoding_control != 0u || instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	for (int source = 1; source < 4; ++source)
	{
		if (!IsUnusedDestination(instruction.src[source]))
		{
			return false;
		}
	}
	return instruction.src[0].constant.i >= -131072 && instruction.src[0].constant.i <= 131068 &&
	       (instruction.src[0].constant.i % 4) == 0;
}

bool IsPackedMaskType(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::VCmpEqU32:
		case ShaderInstructionType::VCmpLtU32:
		case ShaderInstructionType::VCmpNeU32:
		case ShaderInstructionType::VCmpGeU32:
		case ShaderInstructionType::VCmpGtU32:
		case ShaderInstructionType::VCmpLeU32: return true;
		default: return false;
	}
}

// v_cmpx_*_u32 writes EXEC, never the parsed mask destination. Only the plain
// VOPC encoding is admitted: it surfaces the implicit VccLo placeholder while
// VOP3 and SDWA forms keep explicitly encoded fields outside the contract.
bool IsPackedExecMaskInstruction(const ShaderInstruction& instruction)
{
	return IsExactTupleBase(instruction, ShaderInstructionFormat::SmaskVsrc0Vsrc1, 2) &&
	       instruction.dst.type == ShaderOperandType::VccLo && instruction.dst.register_id == 0 && instruction.dst.size == 2 &&
	       ComputeWaveOperandIsPlain(instruction.dst) && IsTask3MaskSource(instruction.src[0]) &&
	       IsTask3MaskSource(instruction.src[1]);
}

String8 ComputeWaveUnsupportedReason(const ShaderInstruction& instruction)
{
	if (instruction.vop3_op_sel != 0u)
	{
		return "nonzero VOP3 op_sel is not admitted";
	}
	if (instruction.vop3_omod != 0u)
	{
		return "nonzero VOP3 omod is not admitted";
	}
	if (instruction.vop_sdwa)
	{
		if (instruction.type == ShaderInstructionType::VMovB32)
		{
			return "VMovB32 SDWA requires the zero-extend extract tuple: DWORD dst select with PAD unused, "
			       "BYTE/WORD/DWORD src select, VGPR operands, and no sext/neg/abs/clamp/omod or reserved bits";
		}
		if (IsPackedMaskType(instruction.type))
		{
			return "SDWA compare requires the U32 packed-mask tuple with DWORD selects and no "
			       "sext/neg/abs or reserved bits";
		}
		if (ShaderComputeWaveTypeIsExecCompare(instruction.type))
		{
			return "SDWA exec compare is outside the paired compute-wave set; only the plain VOPC U32 tuple is admitted";
		}
		if (instruction.type == ShaderInstructionType::VCndmaskB32)
		{
			return "VCndmaskB32 SDWA requires the identity control word: DWORD selects, PAD unused, and no "
			       "sext/neg/abs/clamp/omod or reserved bits";
		}
		return "SDWA encoding is not admitted by the paired compute-wave subset";
	}
	if (!IsUnusedDestination(instruction.dst2))
	{
		return "paired instruction has an unsupported second destination";
	}

	switch (instruction.type)
	{
		case ShaderInstructionType::SEndpgm: return "SEndpgm requires the empty terminal tuple";
	case ShaderInstructionType::SMovB32: return "SMovB32 requires ordinary one-word SGPR operands";
		case ShaderInstructionType::SLshlB32:
			return "SLshlB32 in paired compute-wave requires VccHi, one ordinary SGPR, and a plain 0..31 inline shift amount";
		case ShaderInstructionType::SBfmB64:
			return "SBfmB64 requires a two-word scalar destination and two plain scalar-word sources";
		case ShaderInstructionType::SInstPrefetch:
			return "SInstPrefetch requires SOPP opcode 0x20 and an exact defined 1-to-3-line hint immediate";
		case ShaderInstructionType::SMovB64:
			return "SMovB64 requires even in-range SGPR pairs or VCC/EXEC low pairs and an inline integer or matching pair source";
		case ShaderInstructionType::SBranch:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchExecnz: return "paired compute-wave branch requires an exact relative label tuple";
		case ShaderInstructionType::VMovB32: return "VMovB32 requires ordinary one-word VGPR destination and source";
		case ShaderInstructionType::VReadlaneB32:
			return "VReadlaneB32 requires disjoint ordinary SGPR destination/selector and an ordinary VGPR source";
		case ShaderInstructionType::VWritelaneB32:
			return "VWritelaneB32 requires ordinary VGPR destination, scalar data, and SGPR selector";
		case ShaderInstructionType::VReadfirstlaneB32: return "VReadfirstlaneB32 requires ordinary SGPR destination and VGPR source";
		case ShaderInstructionType::VCndmaskB32:
			return "VCndmaskB32 requires a plain mask-select tuple: one-word VGPR destination, two plain data sources, "
			       "and a two-word VCC/EXEC/SGPR mask pair";
		default:
			if (IsPackedMaskType(instruction.type))
			{
				return "packed U32 compare requires Task3's non-aliasing two-word mask tuple";
			}
			if (ShaderComputeWaveTypeIsExecCompare(instruction.type))
			{
				return "packed U32 exec compare requires the plain VOPC two-source tuple with the implicit EXEC destination";
			}
			return String8::FromPrintf("instruction %s is outside the paired compute-wave admission set",
			                           Core::EnumName8(instruction.type).c_str());
	}
}

} // namespace

bool ShaderComputeWaveTypeIsExecCompare(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::VCmpxEqU32:
		case ShaderInstructionType::VCmpxLtU32:
		case ShaderInstructionType::VCmpxNeU32:
		case ShaderInstructionType::VCmpxGeU32:
		case ShaderInstructionType::VCmpxGtU32:
		case ShaderInstructionType::VCmpxLeU32: return true;
		default: return false;
	}
}

namespace {
ShaderComputeWaveInstructionKind ClassifySpecificComputeWaveInstruction(const ShaderInstruction& instruction);
} // namespace

ShaderComputeWaveInstructionKind ShaderClassifyComputeWaveInstruction(const ShaderInstruction& instruction)
{
	// Every LDS access takes the ordered generic path so in-wave DS ordering
	// never depends on the address-proof rules of the older LDS subset.
	if (ShaderComputeWaveGenericLdsSupported(instruction))
	{
		return ShaderComputeWaveInstructionKind::BankedGenericLds;
	}
	const bool vgpr_result = instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.size == 1 &&
	                         (instruction.dst2.type == ShaderOperandType::Unknown || instruction.dst2.type == ShaderOperandType::Null);
	if ((instruction.type == ShaderInstructionType::VMbcntLoU32B32 || instruction.type == ShaderInstructionType::VMbcntHiU32B32) &&
	    vgpr_result && instruction.src_num == 2 && !instruction.vop_sdwa)
	{
		return ShaderComputeWaveInstructionKind::WaveCount;
	}
	if ((instruction.type == ShaderInstructionType::DsAppend || instruction.type == ShaderInstructionType::DsConsume) && vgpr_result)
	{
		return ShaderComputeWaveInstructionKind::WaveAppend;
	}
	const auto mask_pair = [](const ShaderOperand& operand)
	{ return operand.size == 2 && (operand.type == ShaderOperandType::VccLo || operand.type == ShaderOperandType::Sgpr) && !operand.dpp; };
	if ((instruction.type == ShaderInstructionType::VAddCoCiU32 || instruction.type == ShaderInstructionType::VSubrevCoCiU32) &&
	    instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.size == 1 && mask_pair(instruction.dst2) &&
	    instruction.src_num == 3 && mask_pair(instruction.src[2]) && !instruction.vop_sdwa)
	{
		return ShaderComputeWaveInstructionKind::BankedCarry;
	}
	if (ShaderComputeWaveGenericCompareSupported(instruction))
	{
		return ShaderComputeWaveInstructionKind::BankedGenericCompare;
	}
	const auto kind = ClassifySpecificComputeWaveInstruction(instruction);
	if (kind != ShaderComputeWaveInstructionKind::Unsupported)
	{
		return kind;
	}
	if (ShaderComputeWaveVectorBufferAtomicUmaxSupported(instruction))
	{
		return ShaderComputeWaveInstructionKind::BankedGeneric;
	}
	if (ShaderComputeWaveGenericVectorSupported(instruction))
	{
		return ShaderComputeWaveInstructionKind::BankedGeneric;
	}
	if (ShaderComputeWaveGenericScalarSupported(instruction))
	{
		return ShaderComputeWaveInstructionKind::ScalarGeneric;
	}
	return kind;
}

namespace {
ShaderComputeWaveInstructionKind ClassifySpecificComputeWaveInstruction(const ShaderInstruction& instruction)
{
	if (instruction.type == ShaderInstructionType::SEndpgm)
	{
		return IsEndInstruction(instruction) ? ShaderComputeWaveInstructionKind::End : ShaderComputeWaveInstructionKind::Unsupported;
	}
	if (IsPackedMaskType(instruction.type))
	{
		if (instruction.vop_sdwa)
		{
			return IsPackedMaskSdwaInstruction(instruction) ? ShaderComputeWaveInstructionKind::PackedMask
			                                                : ShaderComputeWaveInstructionKind::Unsupported;
		}
		return IsPackedMaskInstruction(instruction) ? ShaderComputeWaveInstructionKind::PackedMask
		                                            : ShaderComputeWaveInstructionKind::Unsupported;
	}
	if (ShaderComputeWaveTypeIsExecCompare(instruction.type))
	{
		return IsPackedExecMaskInstruction(instruction) ? ShaderComputeWaveInstructionKind::PackedExecMask
		                                                : ShaderComputeWaveInstructionKind::Unsupported;
	}

	switch (instruction.type)
	{
		case ShaderInstructionType::SInstPrefetch:
			return IsInstructionPrefetchHint(instruction) ? ShaderComputeWaveInstructionKind::ScalarHint
			                                            : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::SBranch:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchExecnz:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
			return IsWaveBranchInstruction(instruction) ? ShaderComputeWaveInstructionKind::WaveBranch
			                                            : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::SAndB64:
		case ShaderInstructionType::SOrB64:
		case ShaderInstructionType::SOrn2B64:
		case ShaderInstructionType::SNorB64:
		case ShaderInstructionType::SXorB64:
		case ShaderInstructionType::SNotB64:
		case ShaderInstructionType::SAndSaveexecB64:
			return IsScalarMaskInstruction(instruction) ? ShaderComputeWaveInstructionKind::ScalarMask
			                                            : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::SBfmB64:
			return IsScalarBitfieldMaskInstruction(instruction) ? ShaderComputeWaveInstructionKind::ScalarMask
			                                               : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::SMovB32:
			return IsScalarCopyInstruction(instruction) ? ShaderComputeWaveInstructionKind::ScalarCopy
			                                            : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::SLshlB32:
			return IsScalarShiftInstruction(instruction) ? ShaderComputeWaveInstructionKind::ScalarShift
			                                            : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::DsWriteB32:
		case ShaderInstructionType::DsReadB32:
		case ShaderInstructionType::DsAddRtnU32:
			return ShaderComputeWaveLdsInstructionSupported(instruction) ? ShaderComputeWaveInstructionKind::BankedLds
			                                                          : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::SBarrier:
			return IsBarrierInstruction(instruction) ? ShaderComputeWaveInstructionKind::WorkgroupBarrier
			                                    : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::SMovB64:
			return IsScalarPairCopyInstruction(instruction) ? ShaderComputeWaveInstructionKind::ScalarCopy
			                                                : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::VMovB32:
			if (instruction.vop_sdwa)
			{
				return ShaderComputeWaveSdwaExtractSupported(instruction) ? ShaderComputeWaveInstructionKind::BankedSdwaExtract
				                                                        : ShaderComputeWaveInstructionKind::Unsupported;
			}
			return IsBankedVectorInstruction(instruction) ? ShaderComputeWaveInstructionKind::BankedVector
			                                              : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::VAndB32:
		case ShaderInstructionType::VOrB32:
		case ShaderInstructionType::VXorB32:
		case ShaderInstructionType::VAddI32:
		case ShaderInstructionType::VSubI32:
		case ShaderInstructionType::VAdd3U32:
		case ShaderInstructionType::VCndmaskB32:
			return ShaderComputeWaveAluInstructionSupported(instruction) ? ShaderComputeWaveInstructionKind::BankedAlu
			                                                           : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::VReadlaneB32:
			return IsReadlaneInstruction(instruction) ? ShaderComputeWaveInstructionKind::WaveLane
			                                          : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::VWritelaneB32:
			return IsWritelaneInstruction(instruction) ? ShaderComputeWaveInstructionKind::WaveLane
			                                           : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::VReadfirstlaneB32:
			return IsReadfirstlaneInstruction(instruction) ? ShaderComputeWaveInstructionKind::WaveLane
			                                               : ShaderComputeWaveInstructionKind::Unsupported;
		case ShaderInstructionType::BufferLoadDword:
		case ShaderInstructionType::BufferLoadDwordx2:
		case ShaderInstructionType::BufferLoadDwordx3:
		case ShaderInstructionType::BufferLoadDwordx4:
			return ShaderComputeWaveVectorBufferLoadSupported(instruction) ? ShaderComputeWaveInstructionKind::BankedBufferLoad
			                                                             : ShaderComputeWaveInstructionKind::Unsupported;
		default: return ShaderComputeWaveInstructionKind::Unsupported;
	}
}
} // namespace

bool ShaderComputeWaveGenericCompareSupported(const ShaderInstruction& instruction)
{
	const auto name = Core::EnumName8(instruction.type);
	if (!name.StartsWith("VCmp") || instruction.src_num != 2 ||
	    (instruction.vop_sdwa && (instruction.vop_sdwa_ctrl & ((1u << 19u) | (1u << 27u))) != 0u) ||
	    !(instruction.dst2.type == ShaderOperandType::Unknown || instruction.dst2.type == ShaderOperandType::Null))
	{
		return false;
	}
	const bool exec_compare = name.StartsWith("VCmpx");
	const auto& dst         = instruction.dst;
	// v_cmpx's decoded VCC destination is a placeholder; EXEC is the target.
	if (!exec_compare && !((dst.type == ShaderOperandType::VccLo && dst.size == 2) ||
	                       (dst.type == ShaderOperandType::Sgpr && dst.size == 2 && dst.register_id >= 0 && dst.register_id <= 102 &&
	                        (dst.register_id & 1) == 0)))
	{
		return false;
	}
	for (int source = 0; source < 2; ++source)
	{
		switch (instruction.src[source].type)
		{
			case ShaderOperandType::Vgpr:
			case ShaderOperandType::Sgpr:
			case ShaderOperandType::VccLo:
			case ShaderOperandType::VccHi:
			case ShaderOperandType::M0:
			case ShaderOperandType::LiteralConstant:
			case ShaderOperandType::IntegerInlineConstant:
			case ShaderOperandType::FloatInlineConstant:
				if (instruction.src[source].dpp)
				{
					return false;
				}
				break;
			default: return false;
		}
	}
	return true;
}

bool ShaderComputeWaveGenericLdsSupported(const ShaderInstruction& instruction)
{
	switch (instruction.type)
	{
		case ShaderInstructionType::DsWriteB32:
		case ShaderInstructionType::DsReadB32:
		case ShaderInstructionType::DsRead2B32:
		case ShaderInstructionType::DsAddU32:
		case ShaderInstructionType::DsAddRtnU32:
		case ShaderInstructionType::DsWrxchgRtnB32:
		case ShaderInstructionType::DsSubU32:
		case ShaderInstructionType::DsRsubU32:
		case ShaderInstructionType::DsIncU32:
		case ShaderInstructionType::DsDecU32:
		case ShaderInstructionType::DsMinI32:
		case ShaderInstructionType::DsMaxI32:
		case ShaderInstructionType::DsMinU32:
		case ShaderInstructionType::DsMaxU32:
		case ShaderInstructionType::DsAndB32:
		case ShaderInstructionType::DsOrB32:
		case ShaderInstructionType::DsXorB32: break;
		default: return false;
	}
	if (instruction.src_num < 0 || instruction.src_num > 4 ||
	    !(instruction.dst.type == ShaderOperandType::Vgpr || instruction.dst.type == ShaderOperandType::Unknown) || instruction.dst.dpp ||
	    !(instruction.dst2.type == ShaderOperandType::Unknown || instruction.dst2.type == ShaderOperandType::Null))
	{
		return false;
	}
	for (int source = 0; source < instruction.src_num; ++source)
	{
		if (instruction.src[source].type != ShaderOperandType::Vgpr || instruction.src[source].dpp)
		{
			return false;
		}
	}
	return true;
}

bool ShaderComputeWaveGenericScalarSupported(const ShaderInstruction& instruction)
{
	const auto name = Core::EnumName8(instruction.type);
	// Saveexec variants are included: their native lowering updates the two
	// architectural EXEC words, which paired mode keeps uniform per wave.
	if (name.IsEmpty() || name.At(0) != 'S' || name.StartsWith("SCbranch") ||
	    name.ContainsStr("Store") || name.ContainsStr("Atomic") || name.ContainsStr("Setpc") || name.ContainsStr("Swappc"))
	{
		return false;
	}
	switch (instruction.type)
	{
		case ShaderInstructionType::SBranch:
		case ShaderInstructionType::SEndpgm:
		case ShaderInstructionType::SBarrier:
		case ShaderInstructionType::SBfmB64:
		case ShaderInstructionType::SWaitcnt:
		case ShaderInstructionType::SSendmsg:
		case ShaderInstructionType::SWqmB64: return false;
		default: break;
	}
	auto uniform = [](const ShaderOperand& operand, bool destination)
	{
		switch (operand.type)
		{
			case ShaderOperandType::Sgpr:
			case ShaderOperandType::VccLo:
			case ShaderOperandType::VccHi:
			case ShaderOperandType::ExecLo:
			case ShaderOperandType::ExecHi:
			case ShaderOperandType::M0: return !operand.dpp;
			case ShaderOperandType::Unknown:
			case ShaderOperandType::Null: return true;
			case ShaderOperandType::LiteralConstant:
			case ShaderOperandType::IntegerInlineConstant:
			case ShaderOperandType::FloatInlineConstant:
			case ShaderOperandType::Scc: return !destination;
			default: return false;
		}
	};
	if (instruction.src_num < 0 || instruction.src_num > 4 || !uniform(instruction.dst, true) || !uniform(instruction.dst2, true))
	{
		return false;
	}
	for (int source = 0; source < instruction.src_num; ++source)
	{
		if (!uniform(instruction.src[source], false))
		{
			return false;
		}
	}
	return true;
}

bool ShaderComputeWaveGenericVectorSupported(const ShaderInstruction& instruction)
{
	const auto name   = Core::EnumName8(instruction.type);
	const bool memory = name.StartsWith("Buffer") || name.StartsWith("Tbuffer") || name.StartsWith("Image");
	if (memory)
	{
		// Per-lane vector memory: VGPR data/addresses, uniform SGPR descriptors.
		if (name.ContainsStr("Atomic") || instruction.src_num < 0 || instruction.src_num > 4 || instruction.mimg_address_num < 0 ||
		    instruction.mimg_address_num > 13 ||
		    !(instruction.dst.type == ShaderOperandType::Vgpr || instruction.dst.type == ShaderOperandType::Unknown) ||
		    !(instruction.dst2.type == ShaderOperandType::Unknown || instruction.dst2.type == ShaderOperandType::Null))
		{
			return false;
		}
		for (int address = 0; address < instruction.mimg_address_num; ++address)
		{
			if (instruction.mimg_address[address].type != ShaderOperandType::Vgpr)
			{
				return false;
			}
		}
		for (int source = 0; source < instruction.src_num; ++source)
		{
			switch (instruction.src[source].type)
			{
				case ShaderOperandType::Vgpr:
				case ShaderOperandType::Sgpr:
				case ShaderOperandType::LiteralConstant:
				case ShaderOperandType::IntegerInlineConstant:
				case ShaderOperandType::FloatInlineConstant: break;
				default: return false;
			}
		}
		return true;
	}
	// The decoder folds SDWA source selects, neg/abs and omod into operands;
	// a partial destination select and sign extension are not represented.
	const bool sdwa_representable = !instruction.vop_sdwa || (((instruction.vop_sdwa_ctrl >> 8u) & 7u) == 6u &&
	                                                          (instruction.vop_sdwa_ctrl & ((1u << 19u) | (1u << 27u))) == 0u);
	if (name.IsEmpty() || name.At(0) != 'V' || !sdwa_representable || name.ContainsStr("Lane") || name.ContainsStr("lane") ||
	    name.ContainsStr("Mbcnt") || name.ContainsStr("Movrel") || name.ContainsStr("Interp") || name.ContainsStr("Cmp") ||
	    name.ContainsStr("Cndmask") || name.ContainsStr("Permlane"))
	{
		return false;
	}
	if (instruction.src_num < 0 || instruction.src_num > 4 || instruction.dst.type != ShaderOperandType::Vgpr ||
	    instruction.dst.size < 1 || instruction.dst.dpp ||
	    !(instruction.dst2.type == ShaderOperandType::Unknown || instruction.dst2.type == ShaderOperandType::Null))
	{
		return false;
	}
	// DPP16 row operations never leave a 16-lane row, and every row lies inside
	// one bank with the same invocation layout, so the native subgroup shuffle
	// is exact per bank.
	for (int source = 0; source < instruction.src_num; ++source)
	{
		const auto& operand = instruction.src[source];
		switch (operand.type)
		{
			case ShaderOperandType::Vgpr:
			case ShaderOperandType::Sgpr:
			case ShaderOperandType::VccLo:
			case ShaderOperandType::VccHi:
			case ShaderOperandType::M0:
			case ShaderOperandType::Scc:
			case ShaderOperandType::LiteralConstant:
			case ShaderOperandType::IntegerInlineConstant:
			case ShaderOperandType::FloatInlineConstant: break;
			default: return false;
		}
	}
	return true;
}

ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveCode(const ShaderCode& code, const ShaderComputeInputInfo& input)
{
	ShaderComputeWaveAnalysisResult result {};
	const auto&                     instructions = code.GetInstructions();
	if (code.GetType() != ShaderType::Compute)
	{
		if (instructions.Size() != 0)
		{
			result.unsupported_pc = instructions.At(0).pc;
		}
		result.reason = "paired compute-wave analysis requires compute shader code";
		return result;
	}
	if (input.wave_layout.strategy != ShaderComputeWaveStrategy::Paired64On32)
	{
		if (instructions.Size() != 0)
		{
			result.unsupported_pc = instructions.At(0).pc;
		}
		result.reason = "paired compute-wave analysis requires Paired64On32 layout";
		return result;
	}
	if (instructions.Size() == 0)
	{
		result.reason = "paired compute-wave code is empty";
		return result;
	}
	bool saw_end = false;
	bool saw_branch = false;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& instruction = instructions.At(index);
		auto kind = ShaderClassifyComputeWaveInstruction(instruction);
		if (kind == ShaderComputeWaveInstructionKind::Unsupported &&
		    ShaderPairedEudStorageLoadSupported(instruction, input.bind))
		{
			kind           = ShaderComputeWaveInstructionKind::ScalarResourceLoad;
		}
		if (kind == ShaderComputeWaveInstructionKind::Unsupported && instruction.type == ShaderInstructionType::SWaitcntDepctr)
		{
			// Dependency-counter waits order nothing in a synchronous lowering.
			kind = ShaderComputeWaveInstructionKind::SatisfiedWait;
		}
		if (kind == ShaderComputeWaveInstructionKind::Unsupported && instruction.type == ShaderInstructionType::SWaitcnt)
		{
			// Every admitted memory operation completes at its own PC, so any
			// counter threshold is already satisfied.
			if (!ShaderComputeWaveIsExactWait(instruction))
			{
				result.unsupported_pc = instruction.pc;
				result.reason         = "SWaitcnt requires the exact SOPP encoding";
				return result;
			}
			kind = ShaderComputeWaveInstructionKind::SatisfiedWait;
		}
		if (kind == ShaderComputeWaveInstructionKind::Unsupported && instruction.type == ShaderInstructionType::SBufferLoadDword)
		{
			const auto load = ShaderAnalyzeComputeWaveScalarBufferLoad(code, index, input.bind);
			if (!load.supported)
			{
				return load;
			}
			kind = ShaderComputeWaveInstructionKind::ScalarBufferLoad;
		}
		if (instruction.type == ShaderInstructionType::BufferLoadDword || instruction.type == ShaderInstructionType::BufferLoadDwordx2 ||
		    instruction.type == ShaderInstructionType::BufferLoadDwordx3 || instruction.type == ShaderInstructionType::BufferLoadDwordx4)
		{
			const auto load = ShaderAnalyzeComputeWaveVectorBufferLoad(code, index, input.bind);
			if (load.supported)
			{
				kind = ShaderComputeWaveInstructionKind::BankedBufferLoad;
			} else if (ShaderComputeWaveGenericVectorSupported(instruction))
			{
				kind = ShaderComputeWaveInstructionKind::BankedGeneric;
			} else
			{
				return load;
			}
		}
		if (instruction.type == ShaderInstructionType::BufferAtomicUmax)
		{
			const auto atomic = ShaderAnalyzeComputeWaveVectorBufferAtomicUmax(code, index, input.bind);
			if (!atomic.supported)
			{
				return atomic;
			}
		}
		if (kind == ShaderComputeWaveInstructionKind::Unsupported)
		{
			result.unsupported_pc = instruction.pc;
			result.reason         = ComputeWaveUnsupportedReason(instruction);
			return result;
		}
		if (kind == ShaderComputeWaveInstructionKind::End)
		{
			if (index + 1 != instructions.Size())
			{
				result.unsupported_pc = instruction.pc;
				result.reason         = "SEndpgm must be the final paired compute-wave instruction";
				return result;
			}
			saw_end = true;
		}
		if (kind == ShaderComputeWaveInstructionKind::WaveBranch)
		{
			saw_branch = true;
		}
	}

	if (!saw_end)
	{
		result.unsupported_pc = instructions.At(instructions.Size() - 1).pc;
		result.reason         = "paired compute-wave code has no terminal SEndpgm";
		return result;
	}
	// Branch conditions read uniform SCC/VCC/EXEC words, so every branch is
	// subgroup-uniform and uses the shared control-flow structurizer.
	if (!saw_branch && code.GetLabels().Size() != 0)
	{
		result.unsupported_pc = code.GetLabels().At(0).GetSrc();
		result.reason         = "paired compute-wave label metadata is outside this lane slice";
		return result;
	}
	if (!saw_branch && code.GetIndirectLabels().Size() != 0)
	{
		result.unsupported_pc = code.GetIndirectLabels().At(0).GetSrc();
		result.reason         = "paired compute-wave indirect-label metadata is outside this lane slice";
		return result;
	}

	// DS ordering is provided by the per-instruction subgroup barrier of the
	// generic LDS path; the older address proofs no longer gate admission.
	const ShaderComputeWaveAnalysisResult lds_safety {true, 0, {}};
	if (!lds_safety.supported)
	{
		return lds_safety;
	}
	if (input.bind.extended.used)
	{
		const int extended_base = input.bind.extended.start_register;
		if (!ComputeWaveRegisterRangeIsValid(extended_base, 2, kMaxSgpr) || (extended_base & 1) != 0)
		{
			result.unsupported_pc = instructions.At(0).pc;
			result.reason         = "paired extended pointer base SGPR pair is outside the valid architectural range";
			return result;
		}
		for (const auto& instruction: instructions)
		{
			if (ComputeWaveInstructionMayOverwriteEudBase(instruction, extended_base))
			{
				result.unsupported_pc = instruction.pc;
				result.reason         = "paired extended pointer base SGPR pair may be overwritten";
				return result;
			}
			if (ComputeWaveInstructionMayReadEudBase(instruction, extended_base, input.bind))
			{
				result.unsupported_pc = instruction.pc;
				result.reason         = "paired extended pointer base SGPR pair is read outside its exact mapped S_LOAD operand";
				return result;
			}
		}
	}
	return lds_safety;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
