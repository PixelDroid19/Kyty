#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveAlu.h"
#include "Emulator/Graphics/ShaderComputeWaveLds.h"
#include "Emulator/Graphics/ShaderComputeWaveSdwa.h"
#include "Emulator/Graphics/ShaderComputeWaveVectorBuffer.h"
#include "Emulator/Graphics/ShaderComputeWaveWaitcnt.h"
#include "Kyty/Core/MagicEnum.h"

#include "ShaderStorageAnalysis.h"

#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {
namespace {

constexpr int kMaxSgpr                  = 105; // RDNA2 SGPR0..SGPR105
constexpr int kMaxVgpr                  = 255;
constexpr int kMinIntegerInlineConstant = -16;
constexpr int kMaxIntegerInlineConstant = 64;

// Mnemonic tests on the shared name table: the classifiers below run for every
// instruction of every dispatch, so they must not allocate.
bool NameStartsWith(std::string_view name, std::string_view prefix)
{
	return name.substr(0, prefix.size()) == prefix;
}

bool NameContains(std::string_view name, std::string_view part)
{
	return name.find(part) != std::string_view::npos;
}

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

bool IsScalarBfeI64Control(const ShaderOperand& operand)
{
	if (IsOrdinarySgpr(operand) || IsIntegerOrLiteralConstant(operand) ||
	    (operand.type == ShaderOperandType::FloatInlineConstant && operand.size == 0 && ComputeWaveOperandIsPlain(operand)))
	{
		return true;
	}
	if (operand.size != 1 || operand.register_id != 0 || !ComputeWaveOperandIsPlain(operand))
	{
		return false;
	}
	switch (operand.type)
	{
		case ShaderOperandType::VccLo:
		case ShaderOperandType::VccHi:
		case ShaderOperandType::ExecLo:
		case ShaderOperandType::ExecHi:
		case ShaderOperandType::M0:
		case ShaderOperandType::Scc: return true;
		default: return false;
	}
}

bool IsScalarBfeI64Instruction(const ShaderInstruction& instruction)
{
	const auto scalar_pair = [](const ShaderOperand& operand)
	{
		return operand.type == ShaderOperandType::Sgpr && operand.size == 2 && (operand.register_id % 2 == 0) &&
		       ComputeWaveOperandIsPlain(operand) && ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, kMaxSgpr);
	};
	if (instruction.type != ShaderInstructionType::SBfeI64 ||
	    !IsExactTupleBase(instruction, ShaderInstructionFormat::Sdst2Ssrc02Ssrc1, 2) || !scalar_pair(instruction.dst) ||
	    !scalar_pair(instruction.src[0]) || !IsScalarBfeI64Control(instruction.src[1]))
	{
		return false;
	}
	return IsUnusedDestination(instruction.src[2]) && IsUnusedDestination(instruction.src[3]);
}

bool IsWholeQuadMaskInstruction(const ShaderInstruction& instruction)
{
	return IsExactTupleBase(instruction, ShaderInstructionFormat::Sdst2Ssrc02, 1) && IsScalarPairVariable(instruction.dst) &&
	       IsScalarPairVariable(instruction.src[0]) && IsUnusedDestination(instruction.src[1]) &&
	       IsUnusedDestination(instruction.src[2]) && IsUnusedDestination(instruction.src[3]) && instruction.ds_offset == 0u &&
	       instruction.ds_encoding_control == 0u && instruction.ds_encoding_registers == 0u;
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

// One word of the architectural EXEC mask, as a register spill reads it.
bool IsExecMaskWord(const ShaderOperand& operand)
{
	return operand.size == 1 && operand.register_id == 0 && ComputeWaveOperandIsPlain(operand) &&
	       (operand.type == ShaderOperandType::ExecLo || operand.type == ShaderOperandType::ExecHi);
}

bool IsWritelaneInstruction(const ShaderInstruction& instruction)
{
	const auto uniform = [](const ShaderOperand& operand) { return IsUniformScalarWord(operand) || IsIntegerOrLiteralConstant(operand); };
	return IsExactTupleBase(instruction, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, 2) && IsOrdinaryVgpr(instruction.dst) &&
	       (uniform(instruction.src[0]) || IsExecMaskWord(instruction.src[0])) && uniform(instruction.src[1]);
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

bool IsUntakenDebugBranch(const ShaderInstruction& instruction)
{
	// S_CBRANCH_CDBG* test COND_DBG_SYS/USER, which stay clear with no attached
	// debugger; the parser keeps them as no-op SBarrier records.
	if (instruction.sopp_opcode < 0x17u || instruction.sopp_opcode > 0x1au ||
	    !IsExactTupleBase(instruction, ShaderInstructionFormat::Imm, 1) || !IsUnusedDestination(instruction.dst) ||
	    instruction.src[0].type != ShaderOperandType::LiteralConstant || !ComputeWaveOperandIsPlain(instruction.src[0]))
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

} // namespace

bool ShaderInstructionLoweringPreconditions(const ShaderInstruction& instruction)
{
	if (instruction.src_num < 0 || instruction.src_num > 4 || instruction.mimg_address_num < 0 || instruction.mimg_address_num > 13)
	{
		return false;
	}
	const auto operand_valid = [](const ShaderOperand& operand)
	{
		if (operand.swizzle > 6u) { return false; }
		switch (operand.type)
		{
			case ShaderOperandType::Vgpr: return ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, kMaxVgpr);
			case ShaderOperandType::Sgpr:
				return ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, kMaxSgpr);
			case ShaderOperandType::VccLo:
			case ShaderOperandType::ExecLo: return operand.register_id == 0 && (operand.size == 1 || operand.size == 2);
			case ShaderOperandType::VccHi:
			case ShaderOperandType::ExecHi:
			case ShaderOperandType::M0: return operand.register_id == 0 && operand.size == 1;
			// Status bits can feed 64-bit scalar sources; the high dword reads as zero.
			case ShaderOperandType::VccZ:
			case ShaderOperandType::ExecZ:
			case ShaderOperandType::Scc: return operand.register_id == 0 && (operand.size == 1 || operand.size == 2);
			case ShaderOperandType::Unknown: return operand.size == 0 && ComputeWaveOperandIsPlain(operand);
			case ShaderOperandType::Null: return operand.size >= 0 && operand.size <= 2;
			case ShaderOperandType::LiteralConstant:
			case ShaderOperandType::IntegerInlineConstant:
			case ShaderOperandType::FloatInlineConstant: return operand.size >= 0 && operand.size <= 2;
			default: return false;
		}
	};
	if (!operand_valid(instruction.dst) || !operand_valid(instruction.dst2)) { return false; }
	if (instruction.type == ShaderInstructionType::SEndpgm &&
	    (instruction.format != ShaderInstructionFormat::Empty || instruction.src_num != 0 ||
	     !IsUnusedDestination(instruction.dst) || !IsUnusedDestination(instruction.dst2))) { return false; }
	for (int source = 0; source < 4; ++source)
	{
		const auto& operand = instruction.src[source];
		// SOPP's decoder initially materializes SIMM as a literal for branches.
		// Canonical S_ENDPGM leaves that exact zero immediate behind with no
		// source operands. It is padding, not a live register or a general tail
		// exception; synthetic, nonzero and noncanonical tails still fail closed.
		const bool end_padding = source == 0 && instruction.type == ShaderInstructionType::SEndpgm &&
		                         instruction.sopp_opcode == 1u && instruction.raw_word == 0xbf810000u &&
		                         operand.type == ShaderOperandType::LiteralConstant && operand.size == 0 &&
		                         operand.register_id == 0 && operand.constant.u == 0u && ComputeWaveOperandIsPlain(operand);
		if (!operand_valid(instruction.src[source]) ||
		    (source >= instruction.src_num && !IsUnusedDestination(instruction.src[source]) && !end_padding))
		{
			return false;
		}
	}
	for (int address = 0; address < instruction.mimg_address_num; ++address)
	{
		if (!operand_valid(instruction.mimg_address[address])) { return false; }
	}
	if (ShaderInstructionIsVectorBufferAccess(instruction.type) && (instruction.buffer_flags & ~0x0au) != 0u)
	{
		// SLC/DLC are cache hints (ISA tables 96/98). LDS changes the destination, TFE adds a
		// status result, and unknown/reserved bits cannot mean an ordinary access.
		return false;
	}
	if (instruction.vop_sdwa)
	{
		const uint32_t control = instruction.vop_sdwa_ctrl;
		const bool compare = ShaderInstructionTypeStartsWith(instruction.type, "VCmp");
		if (instruction.src_num < 1 || instruction.src_num > 3 || instruction.vop3_op_sel != 0u || instruction.vop3_omod != 0u ||
		    ((control >> 16u) & 7u) == 7u || (control & (1u << 22u)) != 0u ||
		    instruction.src[0].swizzle != ((control >> 16u) & 7u) ||
		    (instruction.src_num >= 2 && instruction.src[1].swizzle != ((control >> 24u) & 7u)) ||
		    (instruction.src_num >= 2 ? (((control >> 24u) & 7u) == 7u || (control & (1u << 30u)) != 0u)
		                              : (control >> 24u) != 0u))
		{
			return false;
		}
		// Partial destination updates are not represented by the ordinary operand
		// path. Source SEXT is applied by the operand extract; it is admitted for
		// the integer consumers of a sign-extended field: the signed convert, the
		// move and the signed compares.
		const auto name           = ShaderInstructionTypeName(instruction.type);
		const bool signed_compare = compare && name.size() >= 3u && name.substr(name.size() - 3u) == "I32";
		if ((!compare && ((control >> 8u) & 7u) != 6u) ||
		    ((control & ((1u << 19u) | (1u << 27u))) != 0u && !ShaderComputeWaveSdwaSignedConvertSupported(instruction) &&
		     !ShaderComputeWaveSdwaExtractSupported(instruction) && !signed_compare))
		{
			return false;
		}
	}
	// These integer emitters have one word per data operand and no saturation,
	// output scaling or half selection. A specialized rejection cannot grant a
	// different meaning to those controls through generic native emission.
	int integer_sources = 0;
	switch (instruction.type)
	{
		case ShaderInstructionType::VAndB32:
		case ShaderInstructionType::VOrB32:
		case ShaderInstructionType::VXorB32:
		case ShaderInstructionType::VAddI32:
		case ShaderInstructionType::VSubI32: integer_sources = 2; break;
		case ShaderInstructionType::VAdd3U32: integer_sources = 3; break;
		default: break;
	}
	if (integer_sources != 0)
	{
		const bool carry = (instruction.type == ShaderInstructionType::VAddI32 || instruction.type == ShaderInstructionType::VSubI32) &&
		                   instruction.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
		// Legacy VOP2/VOP3 add/sub have a real native carry/borrow emitter.
		// This is a VALU mask destination, so odd SGPR pairs remain legal.
		const bool destination_valid = carry ? IsMaskDestination(instruction.dst2) : IsUnusedDestination(instruction.dst2);
		const auto expected_format = carry ? ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1
		                                   : (integer_sources == 2 ? ShaderInstructionFormat::SVdstSVsrc0SVsrc1
		                                                           : ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2);
		if (instruction.src_num != integer_sources || instruction.vop3_op_sel != 0u || instruction.vop3_omod != 0u ||
		    !IsOrdinaryVgpr(instruction.dst) || !destination_valid || instruction.format != expected_format ||
		    (carry && instruction.vop_sdwa))
		{
			return false;
		}
		for (int source = 0; source < integer_sources; ++source)
		{
			const auto& operand = instruction.src[source];
			const bool constant = operand.type == ShaderOperandType::LiteralConstant ||
			                      operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::FloatInlineConstant;
			if (carry)
			{
				// Only plain numeric one-word sources implemented by operand_load_uint.
				const bool variable = operand.type == ShaderOperandType::Vgpr || operand.type == ShaderOperandType::Sgpr ||
				                      operand.type == ShaderOperandType::VccLo || operand.type == ShaderOperandType::VccHi ||
				                      operand.type == ShaderOperandType::ExecLo || operand.type == ShaderOperandType::ExecHi ||
				                      operand.type == ShaderOperandType::VccZ || operand.type == ShaderOperandType::ExecZ ||
				                      operand.type == ShaderOperandType::M0 || operand.type == ShaderOperandType::Scc ||
				                      operand.type == ShaderOperandType::Null;
				if ((!variable && !constant) || !ComputeWaveOperandIsPlain(operand)) { return false; }
			}
			if (operand.absolute || operand.negate || operand.clamp || operand.multiplier != 1.0f ||
			    (operand.dpp && source != 0) || (constant ? operand.size != 0 : operand.size != 1))
			{
				return false;
			}
		}
	}
	if (ShaderInstructionTypeStartsWith(instruction.type, "S"))
	{
		// Scalar 64-bit data needs even SGPR pairs (ISA 3.6.3). Do not apply
		// this to VALU mask destinations, whose SDST can name an odd SGPR.
		const auto aligned_pair = [](const ShaderOperand& operand)
		{
			return operand.type != ShaderOperandType::Sgpr || operand.size != 2 || (operand.register_id & 1) == 0;
		};
		if (instruction.vop_sdwa || instruction.vop3_op_sel != 0u || instruction.vop3_omod != 0u ||
		    !ComputeWaveOperandIsPlain(instruction.dst) || !ComputeWaveOperandIsPlain(instruction.dst2) ||
		    !aligned_pair(instruction.dst) || !aligned_pair(instruction.dst2)) { return false; }
		for (int source = 0; source < instruction.src_num; ++source)
		{
			if (!ComputeWaveOperandIsPlain(instruction.src[source]) || !aligned_pair(instruction.src[source])) { return false; }
		}
	}
	return true;
}

bool ShaderInstructionPerLaneLoweringPreconditions(const ShaderInstruction& instruction)
{
	if (ShaderSdwaPartialDestinationSupported(instruction))
	{
		return ShaderInstructionLoweringPreconditions(ShaderSdwaWholeDestination(instruction));
	}
	return ShaderInstructionLoweringPreconditions(instruction);
}

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
	if (!ShaderInstructionLoweringPreconditions(instruction))
	{
		return ShaderComputeWaveInstructionKind::Unsupported;
	}
	if (ShaderComputeWaveDppInstructionSupported(instruction))
	{
		return ShaderComputeWaveInstructionKind::BankedDpp;
	}
	if (ShaderComputeWavePermutationSupported(instruction))
	{
		return ShaderComputeWaveInstructionKind::WaveLane;
	}
	if (instruction.type == ShaderInstructionType::VMovB32 || instruction.type == ShaderInstructionType::VAndB32 ||
	    instruction.type == ShaderInstructionType::VOrB32 || instruction.type == ShaderInstructionType::VXorB32)
	{
		for (const auto& source: instruction.src)
		{
			// An unadmitted bitwise DPP tuple must not reach a generic emitter
			// that cannot preserve its source and destination mask contract.
			if (source.dpp) { return ShaderComputeWaveInstructionKind::Unsupported; }
		}
	}
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
	if (ShaderComputeWaveGdsWriteSupported(instruction))
	{
		return ShaderComputeWaveInstructionKind::BankedGdsWrite;
	}
	if (instruction.type == ShaderInstructionType::DsWriteAddtidB32 &&
	    instruction.format == ShaderInstructionFormat::VdataOffset && instruction.src_num == 1 &&
	    instruction.src[0].type == ShaderOperandType::Vgpr && instruction.src[0].size == 1 && !instruction.src[0].dpp &&
	    (instruction.dst.type == ShaderOperandType::Unknown || instruction.dst.type == ShaderOperandType::Null) &&
	    (instruction.dst2.type == ShaderOperandType::Unknown || instruction.dst2.type == ShaderOperandType::Null))
	{
		return ShaderComputeWaveInstructionKind::BankedAddtidLds;
	}
	if (instruction.type == ShaderInstructionType::DsReadAddtidB32 &&
	    instruction.format == ShaderInstructionFormat::VdstOffset && instruction.src_num == 0 && vgpr_result)
	{
		return ShaderComputeWaveInstructionKind::BankedAddtidLds;
	}
	const auto mask_pair = [](const ShaderOperand& operand)
	{ return operand.size == 2 && (operand.type == ShaderOperandType::VccLo || operand.type == ShaderOperandType::Sgpr) && !operand.dpp; };
	if ((instruction.type == ShaderInstructionType::VAddCoCiU32 || instruction.type == ShaderInstructionType::VSubrevCoCiU32) &&
	    instruction.dst.type == ShaderOperandType::Vgpr && instruction.dst.size == 1 && mask_pair(instruction.dst2) &&
	    instruction.src_num == 3 && mask_pair(instruction.src[2]) && !instruction.vop_sdwa)
	{
		return ShaderComputeWaveInstructionKind::BankedCarry;
	}
	if (instruction.type == ShaderInstructionType::VMadU64U32 && instruction.dst.type == ShaderOperandType::Vgpr &&
	    instruction.dst.size == 2 && mask_pair(instruction.dst2) && instruction.src_num == 3 && !instruction.vop_sdwa &&
	    !instruction.dst.clamp)
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
		case ShaderInstructionType::SBfeI64:
			return IsScalarBfeI64Instruction(instruction) ? ShaderComputeWaveInstructionKind::ScalarGeneric
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
			if (IsUntakenDebugBranch(instruction))
			{
				return ShaderComputeWaveInstructionKind::ScalarHint;
			}
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

bool ShaderFloatClassComparisonSupported(const ShaderInstruction& instruction)
{
	if (!ShaderInstructionLoweringPreconditions(instruction)) { return false; }
	const auto& dst = instruction.dst;
	// VOPC SDWA has scalar destination fields, not the VOP2 destination-select
	// fields. Admit DWORD sources without sign extension or reserved controls.
	constexpr uint32_t unsupported_sdwa = (1u << 19u) | (1u << 22u) | (1u << 27u) | (1u << 30u);
	// abs and neg on the classified float only move its sign bit.
	ShaderOperand value = instruction.src[0];
	value.absolute      = false;
	value.negate        = false;
	return instruction.type == ShaderInstructionType::VCmpClassF32 &&
	       instruction.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1 && instruction.src_num == 2 &&
	       instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u &&
	       (!instruction.vop_sdwa || (instruction.vop_sdwa_ctrl & unsupported_sdwa) == 0u) &&
	       IsUnusedDestination(instruction.dst2) && IsScalarPairVariable(dst) && dst.type != ShaderOperandType::ExecLo &&
	       IsTask3MaskSource(value) && IsTask3MaskSource(instruction.src[1]);
}

bool ShaderComputeWaveGenericCompareSupported(const ShaderInstruction& instruction)
{
	if (!ShaderInstructionLoweringPreconditions(instruction)) { return false; }
	if (instruction.type == ShaderInstructionType::VCmpClassF32) { return ShaderFloatClassComparisonSupported(instruction); }
	const auto name = ShaderInstructionTypeName(instruction.type);
	// SDWA compares carry the same undefined fields as the vector forms: source
	// selects 7 and reserved bits 22 and 30 are refused, not lowered as defined values.
	const uint32_t sdwa_ctrl           = instruction.vop_sdwa_ctrl;
	const bool     sdwa_fields_defined = !instruction.vop_sdwa || (((sdwa_ctrl >> 16u) & 7u) != 7u && ((sdwa_ctrl >> 24u) & 7u) != 7u &&
	                                                           (sdwa_ctrl & ((1u << 22u) | (1u << 30u))) == 0u);
	if (!NameStartsWith(name, "VCmp") || instruction.src_num != 2 || !sdwa_fields_defined ||
	    (instruction.vop_sdwa && (instruction.vop_sdwa_ctrl & ((1u << 19u) | (1u << 27u))) != 0u) ||
	    !(instruction.dst2.type == ShaderOperandType::Unknown || instruction.dst2.type == ShaderOperandType::Null))
	{
		return false;
	}
	const bool exec_compare = NameStartsWith(name, "VCmpx");
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
	return ShaderLdsMemoryInstructionSupported(instruction);
}

bool ShaderComputeWaveGenericScalarSupported(const ShaderInstruction& instruction)
{
	if (!ShaderInstructionLoweringPreconditions(instruction)) { return false; }
	if (instruction.type == ShaderInstructionType::SBfeI64)
	{
		return IsScalarBfeI64Instruction(instruction);
	}
	if (instruction.type == ShaderInstructionType::SWqmB64)
	{
		return IsWholeQuadMaskInstruction(instruction);
	}
	const auto name = ShaderInstructionTypeName(instruction.type);
	// Saveexec variants are included: their native lowering updates the two
	// architectural EXEC words, which paired mode keeps uniform per wave.
	if (!NameStartsWith(name, "S") || NameStartsWith(name, "SCbranch") || NameContains(name, "Store") ||
	    NameContains(name, "Atomic") || NameContains(name, "Setpc") || NameContains(name, "Swappc"))
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
	if (!ShaderInstructionLoweringPreconditions(instruction)) { return false; }
	// The native legacy carry emitter does not implement paired mask packing.
	if ((instruction.type == ShaderInstructionType::VAddI32 || instruction.type == ShaderInstructionType::VSubI32) &&
	    instruction.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1) { return false; }
	if (ShaderComputeWaveSdwaSignedConvertSupported(instruction)) { return true; }
	if (ShaderIsImageAtomic(instruction.type)) { return ShaderImageAtomicSupported(instruction); }
	const auto name   = ShaderInstructionTypeName(instruction.type);
	const bool memory = NameStartsWith(name, "Buffer") || NameStartsWith(name, "Tbuffer") || NameStartsWith(name, "Image");
	if (memory)
	{
		// Per-lane vector memory: VGPR data/addresses, uniform SGPR descriptors.
		// A MUBUF with LDS set redirects its data to LDS, TFE appends a status VGPR
		// and undefined encoding bits are not a valid instruction; the native buffer
		// lowerings model none of them, so these decoder-preserved flags must be
		// clear. SLC and DLC are cache hints, not reserved encodings.
		constexpr uint8_t kMubufFlagsWithoutLowering = (1u << 0u) | (1u << 2u) | (1u << 7u);
		if (NameStartsWith(name, "Buffer") && (instruction.buffer_flags & kMubufFlagsWithoutLowering) != 0u)
		{
			return false;
		}
		if (NameContains(name, "Atomic") || instruction.src_num < 0 || instruction.src_num > 4 || instruction.mimg_address_num < 0 ||
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
	// RDNA2 SDWA: SRC0_SEL[18:16] and SRC1_SEL[26:24] only define values 0-6, bits 22
	// and 30 are reserved, and a one-source VOP1 leaves bits 31:24 reserved. The
	// decoder preserves the raw control word, so an undefined field is refused here
	// instead of being lowered as if it were a defined one.
	const uint32_t sdwa_ctrl           = instruction.vop_sdwa_ctrl;
	const bool     sdwa_second_source  = instruction.src_num >= 2;
	const bool     sdwa_fields_defined = !instruction.vop_sdwa ||
	                                     (((sdwa_ctrl >> 16u) & 7u) != 7u && (sdwa_ctrl & (1u << 22u)) == 0u &&
	                                      (sdwa_second_source ? (((sdwa_ctrl >> 24u) & 7u) != 7u && (sdwa_ctrl & (1u << 30u)) == 0u)
	                                                          : (sdwa_ctrl >> 24u) == 0u));
	if (!NameStartsWith(name, "V") || !sdwa_representable || !sdwa_fields_defined || NameContains(name, "Lane") ||
	    NameContains(name, "lane") || NameContains(name, "Mbcnt") || NameContains(name, "Movrel") || NameContains(name, "Interp") ||
	    NameContains(name, "Cmp") || NameContains(name, "Cndmask") || NameContains(name, "Permlane"))
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

ShaderComputeWaveInstructionKind ShaderClassifyGeometryWaveInstruction(const ShaderInstruction& instruction)
{
	if (instruction.type == ShaderInstructionType::SSendmsg)
	{
		// RDNA2 message 9 is GS_ALLOC_REQ; no other message has a geometry effect here.
		const bool alloc = instruction.sopp_opcode == 16u && instruction.src_num == 1 &&
		                   instruction.src[0].type == ShaderOperandType::LiteralConstant && instruction.src[0].constant.u == 9u;
		return alloc ? ShaderComputeWaveInstructionKind::GeometryAlloc : ShaderComputeWaveInstructionKind::Unsupported;
	}
	if (instruction.type != ShaderInstructionType::Exp)
	{
		return ShaderComputeWaveInstructionKind::Unsupported;
	}
	const uint32_t target  = (instruction.raw_word >> 4u) & 0x3fu;
	const uint32_t enable  = instruction.raw_word & 0xfu;
	const bool     compr   = ((instruction.raw_word >> 10u) & 1u) != 0u;
	const bool     vertex  = target == 12u || target == 13u || (target >= 32u && target <= 63u);
	const bool     prim    = target == 20u && enable == 1u;
	if (compr || enable == 0u || enable != instruction.exp_enable_mask || (!vertex && !prim))
	{
		return ShaderComputeWaveInstructionKind::Unsupported;
	}
	for (uint32_t component = 0; component < 4u; ++component)
	{
		if ((enable & (1u << component)) != 0u && !IsOrdinaryVgpr(ShaderExportComponentSource(instruction, component)))
		{
			return ShaderComputeWaveInstructionKind::Unsupported;
		}
	}
	return ShaderComputeWaveInstructionKind::GeometryExport;
}

const ShaderOperand& ShaderExportComponentSource(const ShaderInstruction& instruction, uint32_t component)
{
	if (instruction.format == ShaderInstructionFormat::Pos1OffOffVsrc0Off && component == 2u)
	{
		return instruction.src[0];
	}
	return instruction.src[component & 3u];
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
