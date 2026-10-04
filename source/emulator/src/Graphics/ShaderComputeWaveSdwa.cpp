#include "Emulator/Graphics/ShaderComputeWaveSdwa.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr int kMaxVgpr = 255;

// SDWA control dword: SRC0 [7:0], DST_SEL [10:8], DST_U [12:11], CLMP [13],
// OMOD [15:14], SRC0_SEL [18:16], SRC0_SEXT [19], SRC0_NEG [20], SRC0_ABS [21],
// reserved [22], S0 [23], reserved [31:24]. The admitted extract keeps every
// field except the source encoding and SRC0_SEL at its fixed value:
// dst_sel=DWORD(6), dst_u=PAD(0), clmp=0, omod=0, sext/neg/abs=0, s0=0.
constexpr uint32_t kSdwaExtractFixedMask = ~0x000700ffu;
constexpr uint32_t kSdwaExtractFixedBits = 6u << 8u;
constexpr uint32_t kSdwaSelectReserved   = 7u;

// VOPC SDWAB dword: SRC0 [7:0], SDST [14:8], SD [15], SRC0_SEL [18:16],
// SRC0_SEXT [19], SRC0_NEG [20], SRC0_ABS [21], reserved [22], S0 [23],
// SRC1_SEL [26:24], SRC1_SEXT [27], SRC1_NEG [28], SRC1_ABS [29], reserved
// [30], S1 [31]. The admitted compare keeps both selects at DWORD and every
// modifier/reserved bit clear; the destination and register-file fields stay
// free because the operand tuple and compare emitter validate them.
constexpr uint32_t kSdwabCompareFixedMask = 0x7f7f0000u;
constexpr uint32_t kSdwabCompareFixedBits = (6u << 16u) | (6u << 24u);

// VOP2 SDWA dword: the extract layout above plus SRC1_SEL [26:24],
// SRC1_SEXT [27], SRC1_NEG [28], SRC1_ABS [29], reserved [30], S1 [31].
// Source neg/abs bits [21:20] and [29:28] are carried by the operands.
constexpr uint32_t kSdwaVop2IdentityFixedMask = 0x4f4fff00u;
constexpr uint32_t kSdwaVop2IdentityFixedBits = (6u << 8u) | (6u << 16u) | (6u << 24u);

bool OperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool RegisterRangeIsValid(int register_id, int size, int maximum_register)
{
	if (register_id < 0 || size <= 0 || register_id > maximum_register)
	{
		return false;
	}
	return size <= maximum_register - register_id + 1;
}

} // namespace

bool ShaderComputeWaveSdwaExtractSupported(const ShaderInstruction& instruction)
{
	// SMEM/MUBUF fields keep their unknown sentinels on non-memory encodings;
	// the VOP1 SDWA tuple is fully described by the control dword and operands.
	if (instruction.type != ShaderInstructionType::VMovB32 || !instruction.vop_sdwa ||
	    instruction.format != ShaderInstructionFormat::SVdstSVsrc0 || instruction.src_num != 1 || instruction.vop3_op_sel != 0u ||
	    instruction.vop3_omod != 0u || instruction.ds_offset != 0u || instruction.ds_encoding_control != 0u ||
	    instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	if ((instruction.vop_sdwa_ctrl & kSdwaExtractFixedMask) != kSdwaExtractFixedBits ||
	    ((instruction.vop_sdwa_ctrl >> 16u) & 0x7u) == kSdwaSelectReserved)
	{
		return false;
	}
	const auto& dst  = instruction.dst;
	const auto& src0 = instruction.src[0];
	return OperandIsPlain(dst) && dst.type == ShaderOperandType::Vgpr && dst.size == 1 &&
	       RegisterRangeIsValid(dst.register_id, dst.size, kMaxVgpr) && src0.type == ShaderOperandType::Vgpr && src0.size == 1 &&
	       src0.swizzle <= 6u && !src0.dpp && !src0.absolute && !src0.negate && !src0.clamp && src0.multiplier == 1.0f &&
	       src0.dpp_ctrl == 0u && src0.dpp_row_mask == 0u && src0.dpp_bank_mask == 0u && !src0.dpp_fetch_inactive &&
	       !src0.dpp_bound_ctrl && RegisterRangeIsValid(src0.register_id, src0.size, kMaxVgpr) &&
	       instruction.dst2.type == ShaderOperandType::Unknown && instruction.dst2.size == 0 && OperandIsPlain(instruction.dst2);
}

bool ShaderComputeWaveSdwaSignedConvertSupported(const ShaderInstruction& instruction)
{
	if (instruction.type != ShaderInstructionType::VCvtF32I32 || !instruction.vop_sdwa ||
	    instruction.format != ShaderInstructionFormat::SVdstSVsrc0 || instruction.src_num != 1 ||
	    instruction.vop3_op_sel != 0u || instruction.vop3_omod != 0u || instruction.ds_offset != 0u ||
	    instruction.ds_encoding_control != 0u || instruction.ds_encoding_registers != 0u)
	{
		return false;
	}
	const uint32_t control = instruction.vop_sdwa_ctrl;
	const uint32_t select = (control >> 16u) & 7u;
	// S0=0, DWORD destination, PAD, source SEXT, no other modifiers or reserved bits.
	if ((control & kSdwaExtractFixedMask) != (kSdwaExtractFixedBits | (1u << 19u)) || select == kSdwaSelectReserved)
	{
		return false;
	}
	const auto& destination = instruction.dst;
	const auto& source = instruction.src[0];
	auto plain_source = source;
	plain_source.swizzle = 6u;
	return OperandIsPlain(destination) && destination.type == ShaderOperandType::Vgpr && destination.size == 1 &&
	       RegisterRangeIsValid(destination.register_id, 1, kMaxVgpr) && OperandIsPlain(plain_source) &&
	       source.type == ShaderOperandType::Vgpr && source.size == 1 && RegisterRangeIsValid(source.register_id, 1, kMaxVgpr) &&
	       source.swizzle == select && static_cast<uint32_t>(source.register_id) == (control & 0xffu) &&
	       instruction.dst2.type == ShaderOperandType::Unknown && instruction.dst2.size == 0 && OperandIsPlain(instruction.dst2);
}

namespace {

// The SDWA compare tuple shared by every element type: whole-dword selects, no modifiers, no op_sel/omod.
bool SdwaCompareTuple(const ShaderInstruction& instruction)
{
	return instruction.vop_sdwa && instruction.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1 && instruction.src_num == 2 &&
	       instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u && instruction.ds_offset == 0u &&
	       instruction.ds_encoding_control == 0u && instruction.ds_encoding_registers == 0u &&
	       (instruction.vop_sdwa_ctrl & kSdwabCompareFixedMask) == kSdwabCompareFixedBits;
}

} // namespace

bool ShaderComputeWaveSdwaCompareTupleSupported(const ShaderInstruction& instruction)
{
	if (!SdwaCompareTuple(instruction)) { return false; }
	switch (instruction.type)
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

bool ShaderComputeWaveSdwaCompareIdentityTuple(const ShaderInstruction& instruction)
{
	return SdwaCompareTuple(instruction);
}

bool ShaderComputeWaveSdwaVop2IdentitySupported(const ShaderInstruction& instruction)
{
	return instruction.vop_sdwa && instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u &&
	       (instruction.vop_sdwa_ctrl & kSdwaVop2IdentityFixedMask) == kSdwaVop2IdentityFixedBits;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
