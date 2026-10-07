#include "ShaderSpirvInternal.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kGuestWaveSize      = 64u;
constexpr uint32_t kNativeSubgroupSize = 32u;
constexpr int      kMaxSgpr            = 103;
constexpr int      kMaxVgpr            = 255;

bool ComputeWaveOperandIsPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool ComputeWaveRegisterRangeIsValid(int register_id, int size, int word, int maximum_register)
{
	if (register_id < 0 || size <= 0 || word < 0 || word >= size || register_id > maximum_register)
	{
		return false;
	}

	const int available = maximum_register - register_id + 1;
	return size <= available && word < available;
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

bool ComputeWaveUintSourceIsSupported(const ShaderOperand& operand)
{
	if (!ComputeWaveOperandIsPlain(operand))
	{
		return false;
	}

	switch (operand.type)
	{
		case ShaderOperandType::Vgpr:
			return operand.size == 1 && ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, 0, kMaxVgpr);
		case ShaderOperandType::Sgpr:
			return operand.size == 1 && ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, 0, kMaxSgpr);
		case ShaderOperandType::VccLo:
		case ShaderOperandType::VccHi: return operand.size == 1 && operand.register_id == 0;
		case ShaderOperandType::LiteralConstant:
		case ShaderOperandType::IntegerInlineConstant:
		case ShaderOperandType::FloatInlineConstant: return operand.size == 0;
		default: return false;
	}
}

bool ComputeWaveLayoutIsValid(const ShaderComputeInputInfo* input)
{
	if (input == nullptr)
	{
		return false;
	}

	const auto& layout = input->wave_layout;
	if (layout.strategy != ShaderComputeWaveStrategy::Paired64On32 || layout.guest_wave_size != kGuestWaveSize ||
	    layout.native_subgroup_size != kNativeSubgroupSize || layout.banks != 2u || layout.waves == 0u || layout.guest_local[0] == 0u ||
	    layout.guest_local[1] == 0u || layout.guest_local[2] == 0u || layout.physical_local[1] != 1u || layout.physical_local[2] != 1u ||
	    input->thread_ids_num < 0 || input->thread_ids_num > 3)
	{
		return false;
	}

	uint64_t guest_xy          = 0;
	uint64_t guest_invocations = 0;
	uint64_t expected_guest    = 0;
	uint64_t expected_physical = 0;
	if (!MultiplyU64(layout.guest_local[0], layout.guest_local[1], &guest_xy) ||
	    !MultiplyU64(guest_xy, layout.guest_local[2], &guest_invocations) || !MultiplyU64(layout.waves, kGuestWaveSize, &expected_guest) ||
	    !MultiplyU64(layout.waves, kNativeSubgroupSize, &expected_physical))
	{
		return false;
	}

	return guest_invocations <= expected_guest && guest_invocations + kGuestWaveSize > expected_guest &&
	       expected_physical <= UINT32_MAX && layout.physical_local[0] == expected_physical &&
	       input->threads_num[0] == layout.guest_local[0] && input->threads_num[1] == layout.guest_local[1] &&
	       input->threads_num[2] == layout.guest_local[2] && input->lds_dwords == layout.lds_dwords;
}

bool IsKnownUintConstant(const String8& value)
{
	return !value.IsEmpty() && value != "unknown_uint_constant";
}

bool ComputeWaveBankIsValid(ShaderWaveBank bank)
{
	return bank == ShaderWaveBank::Low || bank == ShaderWaveBank::High;
}

String8 ComputeWaveBankName(ShaderWaveBank bank)
{
	return bank == ShaderWaveBank::Low ? "low" : "high";
}

void AppendComputeWaveCoordinate(String8* output, const char* coordinate_name, const String8& bank_name, const String8& source,
                                 const String8& dimension, uint32_t register_id)
{
	*output +=
	    String8::FromPrintf("%%wave_coord_%s_%s = OpUMod %%uint %s %%%s\n"
	                        "%%wave_coord_%s_float_%s = OpBitcast %%float %%wave_coord_%s_%s\n"
	                        "               OpStore %%v%u_%s %%wave_coord_%s_float_%s\n",
	                        coordinate_name, bank_name.c_str(), source.c_str(), dimension.c_str(), coordinate_name, bank_name.c_str(),
	                        coordinate_name, bank_name.c_str(), register_id, bank_name.c_str(), coordinate_name, bank_name.c_str());
}

} // namespace

bool Spirv::UsesComputeWaveBanks() const
{
	return (m_code.GetType() == ShaderType::Compute || UsesFragmentCompute()) && m_cs_input_info != nullptr &&
	       m_cs_input_info->wave_layout.strategy == ShaderComputeWaveStrategy::Paired64On32;
}

bool Spirv::UsesFragmentWaveTier() const
{
	if (m_code.GetType() != ShaderType::Pixel || UsesFragmentCompute())
	{
		return false;
	}
	if (m_native_wave_tier < 0)
	{
		m_native_wave_tier = m_ps_input_info != nullptr &&
		                     m_ps_input_info->native_wave.proof == ShaderNativeWaveProof::FragmentNeutral32 &&
		                     m_ps_input_info->native_wave.refusal_reason == nullptr ? 1 : 0;
	}
	return m_native_wave_tier != 0;
}

bool Spirv::UsesDsAddtid() const
{
	for (const auto& inst: m_code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::DsWriteAddtidB32 || inst.type == ShaderInstructionType::DsReadAddtidB32)
		{
			return true;
		}
	}
	return false;
}

bool Spirv::UsesDsAddtidLds() const
{
	return UsesDsAddtid() && GetHostShaderType() == ShaderType::Compute && m_cs_input_info != nullptr &&
	       m_cs_input_info->lds_dwords > 0;
}


SpirvValue Spirv::GetComputeWaveRegister(ShaderOperand operand, ShaderWaveBank bank, int word) const
{
	SpirvValue result;
	if (!UsesComputeWaveBanks() || !ComputeWaveBankIsValid(bank) || !ComputeWaveOperandIsPlain(operand) || word < 0)
	{
		return result;
	}

	switch (operand.type)
	{
		case ShaderOperandType::Vgpr:
			if (!ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, word, kMaxVgpr))
			{
				return result;
			}
			result.type  = SpirvType::Float;
			result.value = String8::FromPrintf("v%d_%s", operand.register_id + word, ComputeWaveBankName(bank).c_str());
			return result;
		case ShaderOperandType::Sgpr:
			if (!ComputeWaveRegisterRangeIsValid(operand.register_id, operand.size, word, kMaxSgpr))
			{
				return result;
			}
			result.type  = SpirvType::Uint;
			result.value = String8::FromPrintf("s%d", operand.register_id + word);
			return result;
		case ShaderOperandType::VccLo:
			if (operand.register_id == 0 && operand.size >= 1 && operand.size <= 2 && word < operand.size)
			{
				result.type  = SpirvType::Uint;
				result.value = word == 0 ? "vcc_lo" : "vcc_hi";
			}
			return result;
		case ShaderOperandType::VccHi:
			if (operand.register_id == 0 && word == 0 && operand.size == 1)
			{
				result.type  = SpirvType::Uint;
				result.value = "vcc_hi";
			}
			return result;
		case ShaderOperandType::ExecLo:
			if (operand.register_id == 0 && operand.size == 2 && word < 2)
			{
				result.type  = SpirvType::Uint;
				result.value = word == 0 ? "exec_lo" : "exec_hi";
			}
			return result;
		case ShaderOperandType::ExecHi:
			if (operand.register_id == 0 && word == 0 && operand.size == 1)
			{
				result.type  = SpirvType::Uint;
				result.value = "exec_hi";
			}
			return result;
		default: return result;
	}
}

bool Spirv::EmitComputeWaveOperandUint(const ShaderOperand& operand, ShaderWaveBank bank, const String8& result_id, String8* output) const
{
	if (output == nullptr || result_id.IsEmpty() || !UsesComputeWaveBanks() || !ComputeWaveBankIsValid(bank) ||
	    !ComputeWaveUintSourceIsSupported(operand))
	{
		return false;
	}

	if (operand_is_constant(operand))
	{
		const auto constant = GetConstant(operand);
		if (constant == "unknown_operand_constant")
		{
			return false;
		}
		*output +=
		    String8("%<result> = OpBitcast %uint %<constant>\n").ReplaceStr("<result>", result_id).ReplaceStr("<constant>", constant);
		return true;
	}

	const auto value = GetComputeWaveRegister(operand, bank, 0);
	if (value.value.IsEmpty())
	{
		return false;
	}
	if (value.type == SpirvType::Float)
	{
		*output += String8("%<result>_float = OpLoad %float %<source>\n%<result> = OpBitcast %uint %<result>_float\n")
		               .ReplaceStr("<result>", result_id)
		               .ReplaceStr("<source>", value.value);
		return true;
	}
	if (value.type == SpirvType::Uint)
	{
		*output += String8("%<result> = OpLoad %uint %<source>\n").ReplaceStr("<result>", result_id).ReplaceStr("<source>", value.value);
		return true;
	}

	return false;
}

// Guest lanes exist when they fall inside the guest workgroup and, for
// USE_THREAD_DIMENSIONS, inside the dispatch thread limits. Their ballot is
// the wave's initial EXEC and bounds every later EXEC write.
bool Spirv::EmitComputeWaveValidLanes(String8* output) const
{
	const auto& local       = m_cs_input_info->wave_layout.guest_local;
	const auto  guest_count = GetConstantUint(local[0] * local[1] * local[2]);
	const auto  size_x      = GetConstantUint(local[0]);
	const auto  size_xy     = GetConstantUint(local[0] * local[1]);
	const bool  limits      = m_bind != nullptr && m_bind->thread_limits_used;
	if (limits)
	{
		for (uint32_t axis = 0; axis < 3u; axis++)
		{
			*output += EmitThreadLimitLoad(axis, String8::FromPrintf("wave_limit_%u", axis));
			*output += String8::FromPrintf("%%wave_group_%u_ptr = OpAccessChain %%_ptr_Input_uint %%gl_WorkGroupID %%uint_%u\n"
			                               "%%wave_group_%u = OpLoad %%uint %%wave_group_%u_ptr\n"
			                               "%%wave_group_base_%u = OpIMul %%uint %%wave_group_%u %%%s\n",
			                               axis, axis, axis, axis, axis, axis, GetConstantUint(local[axis]).c_str());
		}
	}
	String8 predicate[2];
	int     slot = 0;
	for (const auto wave_bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto b = ComputeWaveBankName(wave_bank);
		*output += String8::FromPrintf("%%wave_exists_%s = OpULessThan %%bool %%wave_logical_%s %%%s\n", b.c_str(), b.c_str(),
		                               guest_count.c_str());
		String8 valid = String8::FromPrintf("wave_exists_%s", b.c_str());
		if (limits)
		{
			*output += String8::FromPrintf("%%wave_c0_%s = OpUMod %%uint %%wave_logical_%s %%%s\n"
			                               "%%wave_row_%s = OpUDiv %%uint %%wave_logical_%s %%%s\n"
			                               "%%wave_c1_%s = OpUMod %%uint %%wave_row_%s %%%s\n"
			                               "%%wave_c2_%s = OpUDiv %%uint %%wave_logical_%s %%%s\n",
			                               b.c_str(), b.c_str(), size_x.c_str(), b.c_str(), b.c_str(), size_x.c_str(), b.c_str(),
			                               b.c_str(), GetConstantUint(local[1]).c_str(), b.c_str(), b.c_str(), size_xy.c_str());
			for (uint32_t axis = 0; axis < 3u; axis++)
			{
				*output += String8::FromPrintf("%%wave_g%u_%s = OpIAdd %%uint %%wave_group_base_%u %%wave_c%u_%s\n"
				                               "%%wave_in%u_%s = OpULessThan %%bool %%wave_g%u_%s %%wave_limit_%u\n"
				                               "%%wave_ok%u_%s = OpLogicalAnd %%bool %%%s %%wave_in%u_%s\n",
				                               axis, b.c_str(), axis, axis, b.c_str(), axis, b.c_str(), axis, b.c_str(), axis, axis,
				                               b.c_str(), valid.c_str(), axis, b.c_str());
				valid = String8::FromPrintf("wave_ok%u_%s", axis, b.c_str());
			}
		}
		predicate[slot++] = valid;
	}
	return EmitComputeWaveBallot(predicate[0], predicate[1], "wave_valid_lo", "wave_valid_hi", output);
}

bool Spirv::EmitComputeWaveProlog(String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks() || !ComputeWaveLayoutIsValid(m_cs_input_info))
	{
		return false;
	}

	const auto& layout  = m_cs_input_info->wave_layout;
	const auto  bank    = GetConstantUint(kNativeSubgroupSize);
	const auto  wave    = GetConstantUint(kGuestWaveSize);
	const auto  guest_x = GetConstantUint(layout.guest_local[0]);
	const auto  guest_y = GetConstantUint(layout.guest_local[1]);
	const auto  guest_z = GetConstantUint(layout.guest_local[2]);
	if (!IsKnownUintConstant(bank) || !IsKnownUintConstant(wave) || !IsKnownUintConstant(guest_x) || !IsKnownUintConstant(guest_y) ||
	    !IsKnownUintConstant(guest_z))
	{
		return false;
	}

	*output += String8(R"(
%wave_lane_id = OpLoad %uint %gl_SubgroupInvocationID
%wave_subgroup_id = OpLoad %uint %gl_SubgroupID
%wave_logical_base = OpIMul %uint %wave_subgroup_id %<wave>
%wave_logical_low = OpIAdd %uint %wave_logical_base %wave_lane_id
%wave_logical_high = OpIAdd %uint %wave_logical_low %<bank>
)")
	                .ReplaceStr("<wave>", wave)
	                .ReplaceStr("<bank>", bank)
	                ;
	if (!EmitComputeWaveValidLanes(output))
	{
		return false;
	}

	for (const auto wave_bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto bank_name = ComputeWaveBankName(wave_bank);
		const auto logical   = String8::FromPrintf("%%wave_logical_%s", bank_name.c_str());
		if (m_cs_input_info->thread_ids_num >= 1)
		{
			AppendComputeWaveCoordinate(output, "x", bank_name, logical, guest_x, 0u);
		}

		if (m_cs_input_info->thread_ids_num >= 2)
		{
			*output += String8::FromPrintf("%%wave_coord_div_x_%s = OpUDiv %%uint %%wave_logical_%s %%%s\n", bank_name.c_str(),
			                               bank_name.c_str(), guest_x.c_str());
			const auto div_x = String8::FromPrintf("%%wave_coord_div_x_%s", bank_name.c_str());
			AppendComputeWaveCoordinate(output, "y", bank_name, div_x, guest_y, 1u);
		}
		if (m_cs_input_info->thread_ids_num >= 3)
		{
			*output += String8::FromPrintf("%%wave_coord_div_y_%s = OpUDiv %%uint %%wave_coord_div_x_%s %%%s\n", bank_name.c_str(),
			                               bank_name.c_str(), guest_y.c_str());
			const auto div_y = String8::FromPrintf("%%wave_coord_div_y_%s", bank_name.c_str());
			AppendComputeWaveCoordinate(output, "z", bank_name, div_y, guest_z, 2u);
		}
	}

	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
