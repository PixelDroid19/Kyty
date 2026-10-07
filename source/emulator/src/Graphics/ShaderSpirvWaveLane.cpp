#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveSdwa.h"

#include "ShaderSpirvInternal.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

bool IsKnownUintConstant(const String8& value)
{
	return !value.IsEmpty() && value != "unknown_uint_constant";
}

bool EmitComputeWaveVectorMove(const Spirv& spirv, const ShaderInstruction& instruction, uint32_t index, String8* output)
{
	if (output == nullptr)
	{
		return false;
	}

	const auto destination_low  = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::Low, 0);
	const auto destination_high = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::High, 0);
	if (destination_low.type != SpirvType::Float || destination_high.type != SpirvType::Float || destination_low.value.IsEmpty() ||
	    destination_high.value.IsEmpty())
	{
		return false;
	}

	const auto index_string = String8::FromPrintf("%u", index);
	const auto source_low   = String8("wave_vmov_source_low_") + index_string;
	const auto source_high  = String8("wave_vmov_source_high_") + index_string;
	const auto old_low      = String8("wave_vmov_old_low_") + index_string;
	const auto old_high     = String8("wave_vmov_old_high_") + index_string;
	const auto exec_low     = String8("wave_vmov_exec_low_") + index_string;
	const auto exec_high    = String8("wave_vmov_exec_high_") + index_string;
	const auto result_low   = String8("wave_vmov_result_low_") + index_string;
	const auto result_high  = String8("wave_vmov_result_high_") + index_string;

	String8 source;
	if (!spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::Low, source_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::High, source_high, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::Low, old_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::High, old_high, &source))
	{
		return false;
	}

	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	if (!spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::Low, exec_low, &source) ||
	    !spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::High, exec_high, &source))
	{
		return false;
	}

	source += String8(R"(
%<result_low> = OpSelect %uint %<exec_low> %<source_low> %<old_low>
%<result_high> = OpSelect %uint %<exec_high> %<source_high> %<old_high>
%<result_low>_float = OpBitcast %float %<result_low>
%<result_high>_float = OpBitcast %float %<result_high>
               OpStore %<destination_low> %<result_low>_float
               OpStore %<destination_high> %<result_high>_float
)")
	              .ReplaceStr("<result_low>", result_low)
	              .ReplaceStr("<result_high>", result_high)
	              .ReplaceStr("<exec_low>", exec_low)
	              .ReplaceStr("<exec_high>", exec_high)
	              .ReplaceStr("<source_low>", source_low)
	              .ReplaceStr("<source_high>", source_high)
	              .ReplaceStr("<old_low>", old_low)
	              .ReplaceStr("<old_high>", old_high)
	              .ReplaceStr("<destination_low>", destination_low.value)
	              .ReplaceStr("<destination_high>", destination_high.value);
	*output += source;
	return true;
}

// v_mov_b32_sdwa admitted as a zero-extend extract: dst keeps the selected
// BYTE/WORD/DWORD field of the banked source, padded with zeroes.
bool EmitComputeWaveSdwaExtract(const Spirv& spirv, const ShaderInstruction& instruction, uint32_t index, String8* output)
{
	if (output == nullptr || !ShaderComputeWaveSdwaExtractSupported(instruction))
	{
		return false;
	}

	const auto destination_low  = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::Low, 0);
	const auto destination_high = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::High, 0);
	if (destination_low.type != SpirvType::Float || destination_high.type != SpirvType::Float || destination_low.value.IsEmpty() ||
	    destination_high.value.IsEmpty())
	{
		return false;
	}

	const uint32_t select = instruction.src[0].swizzle;
	const uint32_t shift  = select <= 3u ? select * 8u : (select <= 5u ? (select - 4u) * 16u : 0u);
	const uint32_t mask   = select <= 3u ? 0xffu : (select <= 5u ? 0xffffu : 0u);
	const auto     shift_constant = spirv.GetConstantUint(shift);
	const auto     mask_constant  = spirv.GetConstantUint(mask);
	if (select != 6u && (!IsKnownUintConstant(shift_constant) || !IsKnownUintConstant(mask_constant)))
	{
		return false;
	}

	// The operand loader requires a plain tuple; the SDWA select is applied by
	// the extract below, so the register load uses a cleared copy.
	ShaderOperand plain_source = instruction.src[0];
	plain_source.swizzle       = 6u;

	const auto index_string = String8::FromPrintf("%u", index);
	const auto source_low   = String8("wave_sdwa_source_low_") + index_string;
	const auto source_high  = String8("wave_sdwa_source_high_") + index_string;
	const auto old_low      = String8("wave_sdwa_old_low_") + index_string;
	const auto old_high     = String8("wave_sdwa_old_high_") + index_string;
	const auto exec_low     = String8("wave_sdwa_exec_low_") + index_string;
	const auto exec_high    = String8("wave_sdwa_exec_high_") + index_string;
	const auto result_low   = String8("wave_sdwa_result_low_") + index_string;
	const auto result_high  = String8("wave_sdwa_result_high_") + index_string;

	String8 source;
	if (!spirv.EmitComputeWaveOperandUint(plain_source, ShaderWaveBank::Low, source_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(plain_source, ShaderWaveBank::High, source_high, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::Low, old_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::High, old_high, &source))
	{
		return false;
	}

	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	if (!spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::Low, exec_low, &source) ||
	    !spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::High, exec_high, &source))
	{
		return false;
	}

	for (const char* bank: {"low", "high"})
	{
		const auto destination = std::strcmp(bank, "low") == 0 ? destination_low.value : destination_high.value;
		const auto source_id   = String8("wave_sdwa_source_") + bank + "_" + index_string;
		const auto old_id      = String8("wave_sdwa_old_") + bank + "_" + index_string;
		const auto exec_id     = String8("wave_sdwa_exec_") + bank + "_" + index_string;
		const auto result_id   = String8("wave_sdwa_result_") + bank + "_" + index_string;
		const auto value_id    = String8("wave_sdwa_value_") + bank + "_" + index_string;
		const auto shifted_id  = String8("wave_sdwa_shifted_") + bank + "_" + index_string;
		if (select == 6u)
		{
			source += String8(R"(
%<result> = OpSelect %uint %<exec> %<source> %<old>
)")
			              .ReplaceStr("<result>", result_id)
			              .ReplaceStr("<exec>", exec_id)
			              .ReplaceStr("<source>", source_id)
			              .ReplaceStr("<old>", old_id);
		} else if (shift == 0u)
		{
			source += String8(R"(
%<value> = OpBitwiseAnd %uint %<source> %<mask>
%<result> = OpSelect %uint %<exec> %<value> %<old>
)")
			              .ReplaceStr("<value>", value_id)
			              .ReplaceStr("<source>", source_id)
			              .ReplaceStr("<mask>", mask_constant)
			              .ReplaceStr("<result>", result_id)
			              .ReplaceStr("<exec>", exec_id)
			              .ReplaceStr("<old>", old_id);
		} else
		{
			source += String8(R"(
%<shifted> = OpShiftRightLogical %uint %<source> %<shift>
%<value> = OpBitwiseAnd %uint %<shifted> %<mask>
%<result> = OpSelect %uint %<exec> %<value> %<old>
)")
			              .ReplaceStr("<shifted>", shifted_id)
			              .ReplaceStr("<source>", source_id)
			              .ReplaceStr("<shift>", shift_constant)
			              .ReplaceStr("<value>", value_id)
			              .ReplaceStr("<mask>", mask_constant)
			              .ReplaceStr("<result>", result_id)
			              .ReplaceStr("<exec>", exec_id)
			              .ReplaceStr("<old>", old_id);
		}
		source += String8(R"(
%<result>_float = OpBitcast %float %<result>
               OpStore %<destination> %<result>_float
)")
		              .ReplaceStr("<result>", result_id)
		              .ReplaceStr("<destination>", destination);
	}
	*output += source;
	return true;
}

bool EmitComputeWaveReadlane(const Spirv& spirv, const ShaderInstruction& instruction, uint32_t index, String8* output)
{
	if (output == nullptr)
	{
		return false;
	}

	const auto destination = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::Low, 0);
	const auto scope       = spirv.GetConstantUint(3u);
	const auto native_size = spirv.GetConstantUint(32u);
	const auto guest_size  = spirv.GetConstantUint(64u);
	if (destination.type != SpirvType::Uint || destination.value.IsEmpty() || !IsKnownUintConstant(scope) ||
	    !IsKnownUintConstant(native_size) || !IsKnownUintConstant(guest_size))
	{
		return false;
	}

	const auto index_string = String8::FromPrintf("%u", index);
	const auto data_low     = String8("wave_readlane_data_low_") + index_string;
	const auto data_high    = String8("wave_readlane_data_high_") + index_string;
	const auto selector     = String8("wave_readlane_selector_") + index_string;
	const auto selector_mod = String8("wave_readlane_selector_mod_") + index_string;
	const auto select_high  = String8("wave_readlane_select_high_") + index_string;
	const auto native_lane  = String8("wave_readlane_native_lane_") + index_string;
	const auto selected     = String8("wave_readlane_selected_") + index_string;
	const auto result       = String8("wave_readlane_result_") + index_string;

	String8 source;
	if (!spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::Low, data_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::High, data_high, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.src[1], ShaderWaveBank::Low, selector, &source))
	{
		return false;
	}

	source += String8(R"(
%<selector_mod> = OpUMod %uint %<selector> %<guest_size>
%<select_high> = OpUGreaterThanEqual %bool %<selector_mod> %<native_size>
%<native_lane> = OpUMod %uint %<selector_mod> %<native_size>
%<selected> = OpSelect %uint %<select_high> %<data_high> %<data_low>
%<result> = OpGroupNonUniformShuffle %uint %<scope> %<selected> %<native_lane>
               OpStore %<destination> %<result>
)")
	              .ReplaceStr("<selector_mod>", selector_mod)
	              .ReplaceStr("<selector>", selector)
	              .ReplaceStr("<guest_size>", guest_size)
	              .ReplaceStr("<select_high>", select_high)
	              .ReplaceStr("<native_size>", native_size)
	              .ReplaceStr("<native_lane>", native_lane)
	              .ReplaceStr("<selected>", selected)
	              .ReplaceStr("<data_high>", data_high)
	              .ReplaceStr("<data_low>", data_low)
	              .ReplaceStr("<result>", result)
	              .ReplaceStr("<scope>", scope)
	              .ReplaceStr("<destination>", destination.value);
	*output += source;
	return true;
}

bool EmitComputeWaveWritelane(const Spirv& spirv, const ShaderInstruction& instruction, uint32_t index, String8* output)
{
	if (output == nullptr)
	{
		return false;
	}

	const auto destination_low  = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::Low, 0);
	const auto destination_high = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::High, 0);
	const auto native_size      = spirv.GetConstantUint(32u);
	const auto guest_size       = spirv.GetConstantUint(64u);
	if (destination_low.type != SpirvType::Float || destination_high.type != SpirvType::Float || destination_low.value.IsEmpty() ||
	    destination_high.value.IsEmpty() || !IsKnownUintConstant(native_size) || !IsKnownUintConstant(guest_size))
	{
		return false;
	}

	const auto index_string = String8::FromPrintf("%u", index);
	const auto data         = String8("wave_writelane_data_") + index_string;
	const auto selector     = String8("wave_writelane_selector_") + index_string;
	const auto old_low      = String8("wave_writelane_old_low_") + index_string;
	const auto old_high     = String8("wave_writelane_old_high_") + index_string;
	const auto selector_mod = String8("wave_writelane_selector_mod_") + index_string;
	const auto select_high  = String8("wave_writelane_select_high_") + index_string;
	const auto select_low   = String8("wave_writelane_select_low_") + index_string;
	const auto native_lane  = String8("wave_writelane_native_lane_") + index_string;
	const auto matches      = String8("wave_writelane_matches_") + index_string;
	const auto write_low    = String8("wave_writelane_write_low_") + index_string;
	const auto write_high   = String8("wave_writelane_write_high_") + index_string;
	const auto result_low   = String8("wave_writelane_result_low_") + index_string;
	const auto result_high  = String8("wave_writelane_result_high_") + index_string;

	String8 source;
	// Capture uniform scalar inputs and both destination banks before either
	// write. VWritelane ignores EXEC by architecture, so neither result is
	// guarded by the current bank's EXEC bit.
	if (!spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::Low, data, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.src[1], ShaderWaveBank::Low, selector, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::Low, old_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.dst, ShaderWaveBank::High, old_high, &source))
	{
		return false;
	}

	source += String8(R"(
%<selector_mod> = OpUMod %uint %<selector> %<guest_size>
%<select_high> = OpUGreaterThanEqual %bool %<selector_mod> %<native_size>
%<select_low> = OpLogicalNot %bool %<select_high>
%<native_lane> = OpUMod %uint %<selector_mod> %<native_size>
%<matches> = OpIEqual %bool %wave_lane_id %<native_lane>
%<write_low> = OpLogicalAnd %bool %<matches> %<select_low>
%<write_high> = OpLogicalAnd %bool %<matches> %<select_high>
%<result_low> = OpSelect %uint %<write_low> %<data> %<old_low>
%<result_high> = OpSelect %uint %<write_high> %<data> %<old_high>
%<result_low>_float = OpBitcast %float %<result_low>
%<result_high>_float = OpBitcast %float %<result_high>
               OpStore %<destination_low> %<result_low>_float
               OpStore %<destination_high> %<result_high>_float
)")
	              .ReplaceStr("<selector_mod>", selector_mod)
	              .ReplaceStr("<selector>", selector)
	              .ReplaceStr("<guest_size>", guest_size)
	              .ReplaceStr("<select_high>", select_high)
	              .ReplaceStr("<native_size>", native_size)
	              .ReplaceStr("<select_low>", select_low)
	              .ReplaceStr("<native_lane>", native_lane)
	              .ReplaceStr("<matches>", matches)
	              .ReplaceStr("<write_low>", write_low)
	              .ReplaceStr("<write_high>", write_high)
	              .ReplaceStr("<result_low>", result_low)
	              .ReplaceStr("<result_high>", result_high)
	              .ReplaceStr("<data>", data)
	              .ReplaceStr("<old_low>", old_low)
	              .ReplaceStr("<old_high>", old_high)
	              .ReplaceStr("<destination_low>", destination_low.value)
	              .ReplaceStr("<destination_high>", destination_high.value);
	*output += source;
	return true;
}

bool EmitComputeWaveReadfirstlane(const Spirv& spirv, const ShaderInstruction& instruction, uint32_t index, String8* output)
{
	if (output == nullptr)
	{
		return false;
	}

	const auto destination = spirv.GetComputeWaveRegister(instruction.dst, ShaderWaveBank::Low, 0);
	const auto scope       = spirv.GetConstantUint(3u);
	const auto zero        = spirv.GetConstantUint(0u);
	if (destination.type != SpirvType::Uint || destination.value.IsEmpty() || !IsKnownUintConstant(scope) || !IsKnownUintConstant(zero))
	{
		return false;
	}

	const auto index_string = String8::FromPrintf("%u", index);
	const auto source_low   = String8("wave_readfirst_source_low_") + index_string;
	const auto source_high  = String8("wave_readfirst_source_high_") + index_string;
	const auto exec_low     = String8("wave_readfirst_exec_low_") + index_string;
	const auto exec_high    = String8("wave_readfirst_exec_high_") + index_string;
	const auto mask_low     = String8("wave_readfirst_mask_low_") + index_string;
	const auto mask_high    = String8("wave_readfirst_mask_high_") + index_string;
	const auto has_low      = String8("wave_readfirst_has_low_") + index_string;
	const auto has_high     = String8("wave_readfirst_has_high_") + index_string;
	const auto select_high  = String8("wave_readfirst_select_high_") + index_string;
	const auto any_active   = String8("wave_readfirst_any_active_") + index_string;
	const auto first_low    = String8("wave_readfirst_first_low_") + index_string;
	const auto first_high   = String8("wave_readfirst_first_high_") + index_string;
	const auto first        = String8("wave_readfirst_first_") + index_string;
	const auto lane         = String8("wave_readfirst_lane_") + index_string;
	const auto selected     = String8("wave_readfirst_selected_") + index_string;
	const auto result       = String8("wave_readfirst_result_") + index_string;

	String8 source;
	if (!spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::Low, source_low, &source) ||
	    !spirv.EmitComputeWaveOperandUint(instruction.src[0], ShaderWaveBank::High, source_high, &source))
	{
		return false;
	}

	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	if (!spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::Low, exec_low, &source) ||
	    !spirv.EmitComputeWaveMaskBit(exec, ShaderWaveBank::High, exec_high, &source) ||
	    !spirv.EmitComputeWaveBallot(exec_low, exec_high, mask_low, mask_high, &source))
	{
		return false;
	}

	// Both FindLSB operations and the final broadcast are unconditional. The
	// select chooses bank 0/lane 0 when architectural EXEC is empty, matching
	// V_READFIRSTLANE_B32 rather than manufacturing a literal zero.
	source += String8(R"(
%<has_low> = OpINotEqual %bool %<mask_low> %<zero>
%<has_high> = OpINotEqual %bool %<mask_high> %<zero>
%<select_high>_not_low = OpLogicalNot %bool %<has_low>
%<select_high> = OpLogicalAnd %bool %<select_high>_not_low %<has_high>
%<any_active> = OpLogicalOr %bool %<has_low> %<has_high>
%<first_low> = OpGroupNonUniformBallotFindLSB %uint %<scope> %<mask_low>_ballot
%<first_high> = OpGroupNonUniformBallotFindLSB %uint %<scope> %<mask_high>_ballot
%<first> = OpSelect %uint %<select_high> %<first_high> %<first_low>
%<lane> = OpSelect %uint %<any_active> %<first> %<zero>
%<selected> = OpSelect %uint %<select_high> %<source_high> %<source_low>
%<result> = OpGroupNonUniformBroadcast %uint %<scope> %<selected> %<lane>
               OpStore %<destination> %<result>
)")
	              .ReplaceStr("<has_low>", has_low)
	              .ReplaceStr("<has_high>", has_high)
	              .ReplaceStr("<select_high>", select_high)
	              .ReplaceStr("<any_active>", any_active)
	              .ReplaceStr("<first_low>", first_low)
	              .ReplaceStr("<first_high>", first_high)
	              .ReplaceStr("<first>", first)
	              .ReplaceStr("<lane>", lane)
	              .ReplaceStr("<selected>", selected)
	              .ReplaceStr("<result>", result)
	              .ReplaceStr("<mask_low>", mask_low)
	              .ReplaceStr("<mask_high>", mask_high)
	              .ReplaceStr("<zero>", zero)
	              .ReplaceStr("<scope>", scope)
	              .ReplaceStr("<source_low>", source_low)
	              .ReplaceStr("<source_high>", source_high)
	              .ReplaceStr("<destination>", destination.value);
	*output += source;
	return true;
}

} // namespace

bool Spirv::EmitComputeWaveLaneInstruction(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks())
	{
		return false;
	}

	switch (ShaderClassifyComputeWaveInstruction(instruction))
	{
		case ShaderComputeWaveInstructionKind::BankedVector: return EmitComputeWaveVectorMove(*this, instruction, index, output);
		case ShaderComputeWaveInstructionKind::BankedSdwaExtract:
			return EmitComputeWaveSdwaExtract(*this, instruction, index, output);
		case ShaderComputeWaveInstructionKind::WaveLane:
			switch (instruction.type)
			{
				case ShaderInstructionType::VReadlaneB32: return EmitComputeWaveReadlane(*this, instruction, index, output);
				case ShaderInstructionType::VWritelaneB32: return EmitComputeWaveWritelane(*this, instruction, index, output);
				case ShaderInstructionType::VReadfirstlaneB32: return EmitComputeWaveReadfirstlane(*this, instruction, index, output);
				case ShaderInstructionType::VPermlane16B32:
				case ShaderInstructionType::VPermlanex16B32: return EmitComputeWavePermutation(instruction, index, output);
				default: return false;
			}
		default: return false;
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
