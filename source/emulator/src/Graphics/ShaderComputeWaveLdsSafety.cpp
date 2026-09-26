#include "Emulator/Graphics/ShaderComputeWaveLdsSafety.h"

#include <array>
#include <cstddef>
#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr int      kMaxSgpr            = 103;
constexpr uint32_t kGuestWaveSize      = 64u;
constexpr uint32_t kNativeSubgroupSize = 32u;

enum class MaskCardinality
{
	Unknown,
	Empty,
	AtMostOne,
};

enum class MaskPairKind
{
	Invalid,
	Sgpr,
	Vcc,
	Exec,
};

enum class LdsEffectPhase
{
	None,
	Write,
	Atomic,
	Read,
};

struct MaskPairLocation
{
	MaskPairKind kind        = MaskPairKind::Invalid;
	int          register_id = 0;
};

struct MaskTags
{
	std::array<MaskCardinality, static_cast<std::size_t>(kMaxSgpr)> sgpr_pairs {};
	MaskCardinality                                                 vcc  = MaskCardinality::Unknown;
	MaskCardinality                                                 exec = MaskCardinality::Unknown;
};

bool ComputeWaveOperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool MultiplyU64(uint64_t left, uint64_t right, uint64_t* result)
{
	if (result == nullptr || (right != 0u && left > std::numeric_limits<uint64_t>::max() / right))
	{
		return false;
	}
	*result = left * right;
	return true;
}

bool HasInitialUniqueX(const ShaderComputeInputInfo& input)
{
	const auto& layout = input.wave_layout;
	if (layout.strategy != ShaderComputeWaveStrategy::Paired64On32 || input.thread_ids_num != 1 ||
	    layout.guest_wave_size != kGuestWaveSize || layout.native_subgroup_size != kNativeSubgroupSize || layout.banks != 2u ||
	    layout.waves == 0u || layout.guest_local[0] == 0u || layout.guest_local[1] != 1u || layout.guest_local[2] != 1u ||
	    layout.physical_local[1] != 1u || layout.physical_local[2] != 1u || input.threads_num[0] == 0u || input.threads_num[1] != 1u ||
	    input.threads_num[2] != 1u || input.lds_dwords != layout.lds_dwords)
	{
		return false;
	}

	uint64_t expected_guest    = 0;
	uint64_t expected_physical = 0;
	if (!MultiplyU64(layout.waves, kGuestWaveSize, &expected_guest) ||
	    !MultiplyU64(layout.waves, kNativeSubgroupSize, &expected_physical) || expected_physical > UINT32_MAX)
	{
		return false;
	}

	return layout.guest_local[0] == expected_guest && layout.physical_local[0] == expected_physical &&
	       input.threads_num[0] == layout.guest_local[0] && input.threads_num[1] == layout.guest_local[1] &&
	       input.threads_num[2] == layout.guest_local[2];
}

bool IsInitialVgprZero(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Vgpr && operand.register_id == 0 && operand.size == 1 && ComputeWaveOperandIsPlain(operand);
}

bool IsGlobalIntegerConstant(const ShaderOperand& operand)
{
	return operand.size == 0 && ComputeWaveOperandIsPlain(operand) &&
	       (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant);
}

bool WritesVgprZero(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Vgpr && operand.register_id == 0 && operand.size > 0;
}

bool IsOrdinarySgprRange(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Sgpr && operand.size > 0 && operand.register_id >= 0 &&
	       operand.register_id <= kMaxSgpr && operand.size <= kMaxSgpr - operand.register_id + 1 &&
	       ComputeWaveOperandIsPlain(operand);
}

MaskPairLocation GetMaskPairLocation(const ShaderOperand& operand)
{
	if (!ComputeWaveOperandIsPlain(operand) || operand.size != 2)
	{
		return {};
	}

	switch (operand.type)
	{
		case ShaderOperandType::Sgpr:
			if (operand.register_id >= 0 && operand.register_id < kMaxSgpr)
			{
				return {.kind = MaskPairKind::Sgpr, .register_id = operand.register_id};
			}
			return {};
		case ShaderOperandType::VccLo: return operand.register_id == 0 ? MaskPairLocation {.kind = MaskPairKind::Vcc} : MaskPairLocation {};
		case ShaderOperandType::ExecLo:
			return operand.register_id == 0 ? MaskPairLocation {.kind = MaskPairKind::Exec} : MaskPairLocation {};
		default: return {};
	}
}

MaskCardinality GetMaskTag(const MaskTags& tags, MaskPairLocation location)
{
	switch (location.kind)
	{
		case MaskPairKind::Sgpr: return tags.sgpr_pairs.at(static_cast<std::size_t>(location.register_id));
		case MaskPairKind::Vcc: return tags.vcc;
		case MaskPairKind::Exec: return tags.exec;
		default: return MaskCardinality::Unknown;
	}
}

void SetMaskTag(MaskTags* tags, MaskPairLocation location, MaskCardinality cardinality)
{
	if (tags == nullptr)
	{
		return;
	}

	switch (location.kind)
	{
		case MaskPairKind::Sgpr: tags->sgpr_pairs.at(static_cast<std::size_t>(location.register_id)) = cardinality; break;
		case MaskPairKind::Vcc: tags->vcc = cardinality; break;
		case MaskPairKind::Exec: tags->exec = cardinality; break;
		default: break;
	}
}

void ClearSgprWordTags(MaskTags* tags, int register_id)
{
	if (tags == nullptr)
	{
		return;
	}

	for (int pair_start = register_id - 1; pair_start <= register_id; ++pair_start)
	{
		if (pair_start >= 0 && pair_start < kMaxSgpr)
		{
			tags->sgpr_pairs.at(static_cast<std::size_t>(pair_start)) = MaskCardinality::Unknown;
		}
	}
}

void ClearMaskPairTags(MaskTags* tags, MaskPairLocation location)
{
	if (tags == nullptr)
	{
		return;
	}

	switch (location.kind)
	{
		case MaskPairKind::Sgpr:
			// A two-word write at lo:lo+1 overlaps tracked pairs beginning at
			// lo-1, lo, and lo+1, including unaligned compare destinations.
			for (int pair_start = location.register_id - 1; pair_start <= location.register_id + 1; ++pair_start)
			{
				if (pair_start >= 0 && pair_start < kMaxSgpr)
				{
					tags->sgpr_pairs.at(static_cast<std::size_t>(pair_start)) = MaskCardinality::Unknown;
				}
			}
			break;
		case MaskPairKind::Vcc: tags->vcc = MaskCardinality::Unknown; break;
		case MaskPairKind::Exec: tags->exec = MaskCardinality::Unknown; break;
		default: break;
	}
}

MaskCardinality ScalarPairSourceTag(const ShaderOperand& operand, const MaskTags& tags)
{
	const auto location = GetMaskPairLocation(operand);
	if (location.kind != MaskPairKind::Invalid)
	{
		return GetMaskTag(tags, location);
	}
	if (operand.type == ShaderOperandType::IntegerInlineConstant && operand.size == 2 && ComputeWaveOperandIsPlain(operand) &&
	    operand.constant.i == 0)
	{
		return MaskCardinality::Empty;
	}
	return MaskCardinality::Unknown;
}

void UpdateScalarPairTags(const ShaderInstruction& instruction, MaskTags* tags)
{
	const auto destination = GetMaskPairLocation(instruction.dst);
	if (destination.kind == MaskPairKind::Invalid)
	{
		return;
	}

	const auto source_tag = ScalarPairSourceTag(instruction.src[0], *tags);
	ClearMaskPairTags(tags, destination);
	SetMaskTag(tags, destination, source_tag);
}

// Any other write to a VCC or EXEC word replaces part of the tracked pair.
void ClearArchitecturalMaskWordTags(const ShaderOperand& destination, MaskTags* tags)
{
	switch (destination.type)
	{
		case ShaderOperandType::VccLo:
		case ShaderOperandType::VccHi: ClearMaskPairTags(tags, {.kind = MaskPairKind::Vcc}); break;
		case ShaderOperandType::ExecLo:
		case ShaderOperandType::ExecHi: ClearMaskPairTags(tags, {.kind = MaskPairKind::Exec}); break;
		default: break;
	}
}

void ClearScalarMaskTags(const ShaderInstruction& instruction, MaskTags* tags)
{
	ClearMaskPairTags(tags, GetMaskPairLocation(instruction.dst));
	if (instruction.type == ShaderInstructionType::SAndSaveexecB64)
	{
		ClearMaskPairTags(tags, {.kind = MaskPairKind::Exec});
	}
}

bool IsInitialUniqueXEquality(const ShaderInstruction& instruction, bool unique_x)
{
	return unique_x &&
	       (instruction.type == ShaderInstructionType::VCmpEqU32 || instruction.type == ShaderInstructionType::VCmpxEqU32) &&
	       ((IsInitialVgprZero(instruction.src[0]) && IsGlobalIntegerConstant(instruction.src[1])) ||
	        (IsInitialVgprZero(instruction.src[1]) && IsGlobalIntegerConstant(instruction.src[0])));
}

void UpdateComparisonTags(const ShaderInstruction& instruction, bool unique_x, MaskPairLocation destination,
                          MaskTags* tags)
{
	if (destination.kind == MaskPairKind::Invalid)
	{
		return;
	}

	const auto      incoming_exec = tags->exec;
	MaskCardinality result        = MaskCardinality::Unknown;
	if (incoming_exec == MaskCardinality::Empty)
	{
		result = MaskCardinality::Empty;
	} else if (incoming_exec == MaskCardinality::AtMostOne || IsInitialUniqueXEquality(instruction, unique_x))
	{
		result = MaskCardinality::AtMostOne;
	}

	ClearMaskPairTags(tags, destination);
	SetMaskTag(tags, destination, result);
}

LdsEffectPhase GetLdsEffectPhase(const ShaderInstruction& instruction)
{
	switch (instruction.type)
	{
		case ShaderInstructionType::DsWriteB32: return LdsEffectPhase::Write;
		case ShaderInstructionType::DsAddRtnU32: return LdsEffectPhase::Atomic;
		case ShaderInstructionType::DsReadB32: return LdsEffectPhase::Read;
		default: return LdsEffectPhase::None;
	}
}

ShaderComputeWaveAnalysisResult UnsupportedAt(const ShaderInstruction& instruction, const char* reason)
{
	ShaderComputeWaveAnalysisResult result {};
	result.unsupported_pc = instruction.pc;
	result.reason         = reason;
	return result;
}

bool RecordLdsEffect(LdsEffectPhase effect, MaskCardinality exec, LdsEffectPhase* phase)
{
	if (phase == nullptr || exec == MaskCardinality::Empty)
	{
		return true;
	}
	if (*phase == LdsEffectPhase::None)
	{
		*phase = effect;
		return true;
	}
	return *phase == LdsEffectPhase::Read && effect == LdsEffectPhase::Read;
}

} // namespace

ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveLdsSafety(const ShaderCode& code, const ShaderComputeInputInfo& input)
{
	MaskTags       tags {};
	LdsEffectPhase phase    = LdsEffectPhase::None;
	bool           unique_x = HasInitialUniqueX(input);

	for (const auto& instruction: code.GetInstructions())
	{
		const auto kind = ShaderClassifyComputeWaveInstruction(instruction);
		if (instruction.type == ShaderInstructionType::SBarrier)
		{
			if (kind != ShaderComputeWaveInstructionKind::WorkgroupBarrier)
			{
				return UnsupportedAt(instruction, "paired compute-wave LDS safety requires a canonical guest SBarrier");
			}
			phase = LdsEffectPhase::None;
			continue;
		}

		if (kind == ShaderComputeWaveInstructionKind::PackedMask)
		{
			UpdateComparisonTags(instruction, unique_x, GetMaskPairLocation(instruction.dst), &tags);
		} else if (kind == ShaderComputeWaveInstructionKind::PackedExecMask)
		{
			// v_cmpx_* writes EXEC; the parsed VccLo destination is a
			// placeholder, so the tracked location is the EXEC pair.
			UpdateComparisonTags(instruction, unique_x, {.kind = MaskPairKind::Exec}, &tags);
		} else if (instruction.type == ShaderInstructionType::SMovB64)
		{
			UpdateScalarPairTags(instruction, &tags);
		} else if (kind == ShaderComputeWaveInstructionKind::ScalarMask)
		{
			ClearScalarMaskTags(instruction, &tags);
		} else if (IsOrdinarySgprRange(instruction.dst))
		{
			for (int word = 0; word < instruction.dst.size; ++word)
			{
				ClearSgprWordTags(&tags, instruction.dst.register_id + word);
			}
		} else
		{
			ClearArchitecturalMaskWordTags(instruction.dst, &tags);
		}
		if (IsOrdinarySgprRange(instruction.dst2))
		{
			for (int word = 0; word < instruction.dst2.size; ++word)
			{
				ClearSgprWordTags(&tags, instruction.dst2.register_id + word);
			}
		}
		ClearArchitecturalMaskWordTags(instruction.dst2, &tags);

		const auto effect = GetLdsEffectPhase(instruction);
		if (effect != LdsEffectPhase::None)
		{
			if (effect == LdsEffectPhase::Write && tags.exec == MaskCardinality::Unknown)
			{
				return UnsupportedAt(instruction, "paired compute-wave LDS write requires an empty or at-most-one EXEC mask");
			}
			if (!RecordLdsEffect(effect, tags.exec, &phase))
			{
				return UnsupportedAt(instruction, "paired compute-wave LDS effects require a canonical SBarrier between phases");
			}
		}

		if (WritesVgprZero(instruction.dst))
		{
			unique_x = false;
		}
	}

	ShaderComputeWaveAnalysisResult result {};
	result.supported = true;
	return result;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
