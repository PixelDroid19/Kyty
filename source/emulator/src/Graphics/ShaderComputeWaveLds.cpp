#include "Emulator/Graphics/ShaderComputeWaveLds.h"

#include "Emulator/Graphics/ShaderComputeWaveLdsSafety.h"

#include <array>
#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr int      kMaxVgpr             = 255;
constexpr uint32_t kDsEncodingPrefix    = 0x36u;
constexpr uint32_t kDsWriteB32Opcode    = 0x0du;
constexpr uint32_t kDsAddRtnU32Opcode   = 0x20u;
constexpr uint32_t kDsReadB32Opcode     = 0x36u;
constexpr uint32_t kLdsElementByteWidth = 4u;

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

struct KnownVgprValue
{
	bool     known = false;
	uint32_t value = 0;
};

using KnownVgprValues = std::array<KnownVgprValue, static_cast<size_t>(kMaxVgpr) + 1u>;

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

bool IsComputeWaveLdsInstruction(const ShaderInstruction& instruction)
{
	switch (instruction.type)
	{
		case ShaderInstructionType::DsWriteB32:
		case ShaderInstructionType::DsReadB32:
		case ShaderInstructionType::DsAddRtnU32: return true;
		default: return false;
	}
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

bool IsExecPair(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::ExecLo && operand.register_id == 0 && operand.size == 2 && ComputeWaveOperandIsPlain(operand);
}

bool IsInlineNegativeOnePair(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::IntegerInlineConstant && operand.size == 2 && operand.constant.i == -1 &&
	       ComputeWaveOperandIsPlain(operand);
}

bool TryGetKnownVgprSource(const ShaderOperand& operand, const KnownVgprValues& values, uint32_t* value)
{
	if (value == nullptr || !ComputeWaveOperandIsPlain(operand))
	{
		return false;
	}

	switch (operand.type)
	{
		case ShaderOperandType::IntegerInlineConstant:
		case ShaderOperandType::LiteralConstant:
			if (operand.size != 0)
			{
				return false;
			}
			*value = operand.constant.u;
			return true;
		case ShaderOperandType::Vgpr:
			if (!IsOrdinaryVgpr(operand) || !values.at(VgprIndex(operand)).known)
			{
				return false;
			}
			*value = values.at(VgprIndex(operand)).value;
			return true;
		default: return false;
	}
}

void UpdateKnownVgprMove(const ShaderInstruction& instruction, bool full_exec, KnownVgprValues* values)
{
	if (values == nullptr || !IsOrdinaryVgpr(instruction.dst))
	{
		return;
	}

	uint32_t   source_value = 0;
	const bool source_known = TryGetKnownVgprSource(instruction.src[0], *values, &source_value);
	auto&      destination  = values->at(VgprIndex(instruction.dst));
	if (full_exec && source_known)
	{
		destination = {.known = true, .value = source_value};
		return;
	}
	if (!full_exec && source_known && destination.known && destination.value == source_value)
	{
		return;
	}
	destination.known = false;
}

bool ValidateLdsAddress(const ShaderInstruction& instruction, const ShaderComputeInputInfo& input, const KnownVgprValues& values,
                        String8* reason)
{
	if (reason == nullptr)
	{
		return false;
	}
	if (input.lds_dwords == 0u)
	{
		*reason = "paired compute-wave LDS access requires a nonzero LDS allocation";
		return false;
	}
	if (!IsOrdinaryVgpr(instruction.src[0]) || !values.at(VgprIndex(instruction.src[0])).known)
	{
		*reason = "paired compute-wave LDS address is not a proven constant";
		return false;
	}

	const uint64_t address = values.at(VgprIndex(instruction.src[0])).value;
	const uint64_t offset  = instruction.ds_offset;
	if (address > std::numeric_limits<uint64_t>::max() - offset)
	{
		*reason = "paired compute-wave LDS address arithmetic overflows";
		return false;
	}
	const uint64_t byte_address = address + offset;
	if ((byte_address % kLdsElementByteWidth) != 0u)
	{
		*reason = "paired compute-wave LDS address is not four-byte aligned";
		return false;
	}

	const uint64_t lds_bytes = static_cast<uint64_t>(input.lds_dwords) * kLdsElementByteWidth;
	if (byte_address > std::numeric_limits<uint64_t>::max() - kLdsElementByteWidth || byte_address + kLdsElementByteWidth > lds_bytes)
	{
		*reason = "paired compute-wave LDS access exceeds the declared allocation";
		return false;
	}
	return true;
}

ShaderComputeWaveAnalysisResult UnsupportedLdsInstruction(const ShaderInstruction& instruction, const String8& reason)
{
	ShaderComputeWaveAnalysisResult result {};
	result.unsupported_pc = instruction.pc;
	result.reason         = reason;
	return result;
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

ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveLdsAccesses(const ShaderCode& code, const ShaderComputeInputInfo& input)
{
	KnownVgprValues known_values {};
	bool            full_exec = true;
	for (const auto& instruction: code.GetInstructions())
	{
		if (IsComputeWaveLdsInstruction(instruction))
		{
			if (!ShaderComputeWaveLdsInstructionSupported(instruction))
			{
				return UnsupportedLdsInstruction(instruction, "paired compute-wave LDS tuple is not supported");
			}

			String8 reason;
			if (!ValidateLdsAddress(instruction, input, known_values, &reason))
			{
				return UnsupportedLdsInstruction(instruction, reason);
			}
		}

		const auto kind = ShaderClassifyComputeWaveInstruction(instruction);
		if (kind == ShaderComputeWaveInstructionKind::ScalarMask &&
		    (IsExecPair(instruction.dst) || instruction.type == ShaderInstructionType::SAndSaveexecB64))
		{
			// Do not infer algebraic identities for newly admitted scalar masks.
			// SAndSaveexecB64 also changes EXEC implicitly.
			full_exec = false;
		} else if (instruction.type == ShaderInstructionType::SMovB64 && IsExecPair(instruction.dst))
		{
			full_exec = IsInlineNegativeOnePair(instruction.src[0]) || (full_exec && IsExecPair(instruction.src[0]));
		}

		if (instruction.type == ShaderInstructionType::VMovB32)
		{
			UpdateKnownVgprMove(instruction, full_exec, &known_values);
		} else if (IsOrdinaryVgpr(instruction.dst))
		{
			// LDS reads/atomic returns and every other admitted vector write lose
			// any constant proof after their inputs have been inspected. This also
			// keeps later banked ALU additions from retaining a stale address.
			known_values.at(VgprIndex(instruction.dst)).known = false;
		}
	}

	const auto safety = ShaderAnalyzeComputeWaveLdsSafety(code, input);
	if (!safety.supported)
	{
		return safety;
	}

	ShaderComputeWaveAnalysisResult result {};
	result.supported = true;
	return result;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
