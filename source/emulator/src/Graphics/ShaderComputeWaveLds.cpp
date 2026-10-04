#include "Emulator/Graphics/ShaderComputeWaveLds.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr int      kMaxVgpr             = 255;
constexpr uint32_t kDsEncodingPrefix    = 0x36u;
constexpr uint32_t kDsWriteB32Opcode    = 0x0du;
constexpr uint32_t kDsAddRtnU32Opcode   = 0x20u;
constexpr uint32_t kDsReadB32Opcode     = 0x36u;

struct DsEncoding
{
	uint32_t prefix   = 0;
	uint32_t opcode   = 0;
	uint32_t gds      = 0;
	uint32_t reserved = 0;
	uint32_t offset0  = 0;
	uint32_t offset1  = 0;
	uint32_t vdst     = 0;
	uint32_t data1    = 0;
	uint32_t data0    = 0;
	uint32_t address  = 0;
};

bool ComputeWaveOperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool IsOrdinaryVgpr(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Vgpr && operand.size == 1 && operand.register_id >= 0 && operand.register_id <= kMaxVgpr &&
	       ComputeWaveOperandIsPlain(operand);
}

bool IsUnusedOperand(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Unknown && operand.size == 0 && ComputeWaveOperandIsPlain(operand);
}

bool HasExactSourceTail(const ShaderInstruction& instruction, int source_count)
{
	if (instruction.src_num != source_count)
	{
		return false;
	}

	for (int source = source_count; source < 4; ++source)
	{
		if (!IsUnusedOperand(instruction.src[source]))
		{
			return false;
		}
	}
	return true;
}

bool IsExactDsTupleBase(const ShaderInstruction& instruction, ShaderInstructionFormat::Format format, int source_count)
{
	return instruction.format == format && HasExactSourceTail(instruction, source_count) && IsUnusedOperand(instruction.dst2) &&
	       instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u && !instruction.vop_sdwa;
}

DsEncoding DecodeDsEncoding(const ShaderInstruction& instruction)
{
	const uint32_t control   = instruction.ds_encoding_control;
	const uint32_t registers = instruction.ds_encoding_registers;
	return {.prefix   = control >> 26u,
	        .opcode   = (control >> 18u) & 0xffu,
	        .gds      = (control >> 17u) & 0x1u,
	        .reserved = (control >> 16u) & 0x1u,
	        .offset0  = control & 0xffu,
	        .offset1  = (control >> 8u) & 0xffu,
	        .vdst     = (registers >> 24u) & 0xffu,
	        .data1    = (registers >> 16u) & 0xffu,
	        .data0    = (registers >> 8u) & 0xffu,
	        .address  = registers & 0xffu};
}

bool HasSupportedDsControls(const DsEncoding& encoding)
{
	return encoding.prefix == kDsEncodingPrefix && encoding.gds == 0u && encoding.reserved == 0u;
}

uint32_t VgprIndex(const ShaderOperand& operand)
{
	return static_cast<uint32_t>(operand.register_id);
}

bool IsDsWriteB32InstructionSupported(const ShaderInstruction& instruction)
{
	const auto encoding = DecodeDsEncoding(instruction);
	return IsExactDsTupleBase(instruction, ShaderInstructionFormat::VaddrVdataOffset, 2) && IsUnusedOperand(instruction.dst) &&
	       IsOrdinaryVgpr(instruction.src[0]) && IsOrdinaryVgpr(instruction.src[1]) && HasSupportedDsControls(encoding) &&
	       encoding.opcode == kDsWriteB32Opcode && encoding.offset1 == 0u && encoding.vdst == 0u && encoding.data1 == 0u &&
	       encoding.address == VgprIndex(instruction.src[0]) && encoding.data0 == VgprIndex(instruction.src[1]) &&
	       instruction.ds_offset == encoding.offset0;
}

bool IsDsReadB32InstructionSupported(const ShaderInstruction& instruction)
{
	const auto encoding = DecodeDsEncoding(instruction);
	return IsExactDsTupleBase(instruction, ShaderInstructionFormat::VdstVaddrOffset, 1) && IsOrdinaryVgpr(instruction.dst) &&
	       IsOrdinaryVgpr(instruction.src[0]) && HasSupportedDsControls(encoding) && encoding.opcode == kDsReadB32Opcode &&
	       encoding.offset1 == 0u && encoding.data0 == 0u && encoding.data1 == 0u && encoding.vdst == VgprIndex(instruction.dst) &&
	       encoding.address == VgprIndex(instruction.src[0]) && instruction.ds_offset == encoding.offset0;
}

bool IsDsAddRtnU32InstructionSupported(const ShaderInstruction& instruction)
{
	const auto encoding = DecodeDsEncoding(instruction);
	const auto offset   = static_cast<uint16_t>(encoding.offset0 | (encoding.offset1 << 8u));
	return IsExactDsTupleBase(instruction, ShaderInstructionFormat::VdstVaddrVdataOffset, 2) && IsOrdinaryVgpr(instruction.dst) &&
	       IsOrdinaryVgpr(instruction.src[0]) && IsOrdinaryVgpr(instruction.src[1]) && HasSupportedDsControls(encoding) &&
	       encoding.opcode == kDsAddRtnU32Opcode && encoding.data1 == 0u && encoding.vdst == VgprIndex(instruction.dst) &&
	       encoding.address == VgprIndex(instruction.src[0]) && encoding.data0 == VgprIndex(instruction.src[1]) &&
	       instruction.ds_offset == offset;
}

} // namespace

bool ShaderComputeWaveLdsInstructionSupported(const ShaderInstruction& instruction)
{
	switch (instruction.type)
	{
		case ShaderInstructionType::DsWriteB32: return IsDsWriteB32InstructionSupported(instruction);
		case ShaderInstructionType::DsReadB32: return IsDsReadB32InstructionSupported(instruction);
		case ShaderInstructionType::DsAddRtnU32: return IsDsAddRtnU32InstructionSupported(instruction);
		default: return false;
	}
}

bool ShaderLdsMemoryInstructionSupported(const ShaderInstruction& instruction)
{
	uint32_t opcode = 0;
	int words = 1;
	int sources = 2;
	bool result = false;
	auto format = ShaderInstructionFormat::VaddrVdataOffset;
	switch (instruction.type)
	{
		case ShaderInstructionType::DsWriteB32:
			words = instruction.src[1].size;
			switch (words)
			{
				case 1: opcode = 0x0du; break;
				case 2: opcode = 0x4du; break;
				case 3: opcode = 0xdeu; break;
				case 4: opcode = 0xdfu; break;
				default: return false;
			}
			break;
		case ShaderInstructionType::DsReadB32:
			words = instruction.dst.size;
			sources = 1;
			result = true;
			format = ShaderInstructionFormat::VdstVaddrOffset;
			switch (words)
			{
				case 1: opcode = 0x36u; break;
				case 2: opcode = 0x76u; break;
				case 3: opcode = 0xfeu; break;
				case 4: opcode = 0xffu; break;
				default: return false;
			}
			break;
		case ShaderInstructionType::DsRead2B32:
		case ShaderInstructionType::DsRead2St64B32:
			opcode = instruction.type == ShaderInstructionType::DsRead2B32 ? 0x37u : 0x38u;
			words = 2;
			sources = 1;
			result = true;
			format = ShaderInstructionFormat::Vdst2VaddrOffset01;
			break;
		case ShaderInstructionType::DsWrite2B32:
		case ShaderInstructionType::DsWrite2St64B32:
			opcode = instruction.type == ShaderInstructionType::DsWrite2B32 ? 0x0eu : 0x0fu;
			words = 1;
			sources = 3;
			format = ShaderInstructionFormat::VaddrVdata2Offset01;
			break;
		case ShaderInstructionType::DsAddRtnU32:
		case ShaderInstructionType::DsWrxchgRtnB32:
			opcode = instruction.type == ShaderInstructionType::DsAddRtnU32 ? 0x20u : 0x2du;
			result = true;
			format = ShaderInstructionFormat::VdstVaddrVdataOffset;
			break;
		case ShaderInstructionType::DsAddU32: opcode = 0x00u; break;
		case ShaderInstructionType::DsSubU32: opcode = 0x01u; break;
		case ShaderInstructionType::DsMinI32: opcode = 0x05u; break;
		case ShaderInstructionType::DsMaxI32: opcode = 0x06u; break;
		case ShaderInstructionType::DsMinU32: opcode = 0x07u; break;
		case ShaderInstructionType::DsMaxU32: opcode = 0x08u; break;
		case ShaderInstructionType::DsAndB32: opcode = 0x09u; break;
		case ShaderInstructionType::DsOrB32: opcode = 0x0au; break;
		case ShaderInstructionType::DsXorB32: opcode = 0x0bu; break;
		// INC/DEC require DATA0's wrap limit (ISA 12.9), which the decoder
		// currently drops. Neither unconditional increment nor decrement is exact.
		default: return false;
	}
	const auto vgprs = [](const ShaderOperand& operand, int count)
	{
		return operand.type == ShaderOperandType::Vgpr && operand.size == count && count > 0 &&
		       operand.register_id >= 0 && operand.register_id <= kMaxVgpr - count + 1 && ComputeWaveOperandIsPlain(operand);
	};
	if (!IsExactDsTupleBase(instruction, format, sources) || !vgprs(instruction.src[0], 1) ||
	    (sources >= 2 && !vgprs(instruction.src[1], result ? 1 : words)) || (sources == 3 && !vgprs(instruction.src[2], words)) ||
	    (result ? !vgprs(instruction.dst, words) : !IsUnusedOperand(instruction.dst)))
	{
		return false;
	}
	// Zero is the absent-encoding sentinel for explicitly constructed IR. A
	// parsed DS always retains both raw words and must agree with the tuple.
	if (instruction.ds_encoding_control == 0u)
	{
		return instruction.ds_encoding_registers == 0u;
	}
	const auto encoding = DecodeDsEncoding(instruction);
	return HasSupportedDsControls(encoding) && encoding.opcode == opcode &&
	       encoding.data1 == (sources == 3 ? VgprIndex(instruction.src[2]) : 0u) &&
	       encoding.address == VgprIndex(instruction.src[0]) &&
	       encoding.data0 == (sources >= 2 ? VgprIndex(instruction.src[1]) : 0u) &&
	       encoding.vdst == (result ? VgprIndex(instruction.dst) : 0u) &&
	       instruction.ds_offset == (encoding.offset0 | (encoding.offset1 << 8u));
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
