#include "ShaderSpirvInternal.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveLayout.h"
#include "Emulator/Graphics/ShaderNggFrontBody.h"

#include <limits>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

constexpr uint32_t kGuestWaveSize      = 64u;
constexpr uint32_t kNativeSubgroupSize = 32u;

bool NativeComputeWaveInputIsValid(const ShaderComputeInputInfo* input)
{
	if (input == nullptr || !input->native_wave_sensitive || input->required_subgroup_size != kNativeSubgroupSize ||
	    input->thread_ids_num < 0 || input->thread_ids_num > 3 || input->thread_limits_used != input->bind.thread_limits_used)
	{
		return false;
	}

	const auto& layout = input->wave_layout;
	const bool  split  = layout.strategy == ShaderComputeWaveStrategy::Split64On32;
	if (split ? layout.guest_wave_size != kGuestWaveSize || layout.native_subgroup_size != kNativeSubgroupSize || layout.waves == 0u
	          : layout.strategy != ShaderComputeWaveStrategy::Native || layout.guest_wave_size != kNativeSubgroupSize ||
	                (layout.native_subgroup_size != 0u && layout.native_subgroup_size != kNativeSubgroupSize) || layout.waves != 0u)
	{
		return false;
	}
	if (layout.banks != 1u || layout.lds_dwords != input->lds_dwords)
	{
		return false;
	}

	uint32_t physical_local[3] {};
	if (!ShaderNativeWave32PhysicalLocal(layout.guest_local, physical_local))
	{
		return false;
	}
	if (split)
	{
		// Whole guest waves: the trailing one keeps its padding lanes inactive.
		const uint64_t waves = (static_cast<uint64_t>(physical_local[0]) + kGuestWaveSize - 1u) / kGuestWaveSize;
		if (waves != layout.waves || waves * kGuestWaveSize > UINT32_MAX)
		{
			return false;
		}
		physical_local[0] = static_cast<uint32_t>(waves * kGuestWaveSize);
	}

	for (uint32_t axis = 0; axis < 3u; axis++)
	{
		if (layout.guest_local[axis] != input->threads_num[axis] || layout.physical_local[axis] != physical_local[axis])
		{
			return false;
		}
	}

	if (input->thread_limits_used)
	{
		const uint32_t push_constant_dwords = input->bind.push_constant_size / sizeof(uint32_t);
		if ((input->bind.push_constant_size & 15u) != 0u || (input->bind.thread_limits_offset_dw & 3u) != 0u ||
		    push_constant_dwords < 4u ||
		    input->bind.thread_limits_offset_dw > push_constant_dwords - 4u)
		{
			return false;
		}
	}

	return true;
}

bool IsKnownUintConstant(const String8& value)
{
	return !value.IsEmpty() && value != "unknown_uint_constant";
}

void AppendNativeComputeWaveCoordinate(String8* output, const char* coordinate_name, uint32_t register_id)
{
	*output += String8::FromPrintf("%%native_wave_coord_%s_float = OpBitcast %%float %%native_wave_coord_%s\n"
	                               "               OpStore %%v%u %%native_wave_coord_%s_float\n",
	                               coordinate_name, coordinate_name, register_id, coordinate_name);
}

} // namespace

bool Spirv::UsesNativeComputeWave() const
{
	return UsesSplitComputeWave() ||
	       (m_code.GetType() == ShaderType::Compute && m_cs_input_info != nullptr && m_cs_input_info->native_wave_sensitive &&
	        m_cs_input_info->wave_layout.strategy == ShaderComputeWaveStrategy::Native &&
	        m_cs_input_info->wave_layout.guest_wave_size == kNativeSubgroupSize);
}

bool Spirv::NativeComputeWaveLayoutIsValid() const
{
	return UsesNativeComputeWave() && NativeComputeWaveInputIsValid(m_cs_input_info);
}

bool Spirv::EmitNativeComputeWaveProlog(String8* output) const
{
	if (output == nullptr || !NativeComputeWaveLayoutIsValid())
	{
		return false;
	}

	const auto& local   = m_cs_input_info->wave_layout.guest_local;
	const auto  wave    = GetConstantUint(kNativeSubgroupSize);
	const auto  size_x  = GetConstantUint(local[0]);
	const auto  size_y  = GetConstantUint(local[1]);
	const auto  size_xy = GetConstantUint(local[0] * local[1]);
	const auto  lanes   = GetConstantUint(local[0] * local[1] * local[2]);
	if (!IsKnownUintConstant(wave) || !IsKnownUintConstant(size_x) || !IsKnownUintConstant(size_y) || !IsKnownUintConstant(size_xy) ||
	    !IsKnownUintConstant(lanes))
	{
		return false;
	}

	*output += String8(R"(
%native_wave_lane_id = OpLoad %uint %<host_lane>
%native_wave_subgroup_id = OpLoad %uint %gl_SubgroupID
%native_wave_logical_base = OpIMul %uint %native_wave_subgroup_id %<wave>
%native_wave_logical_index = OpIAdd %uint %native_wave_logical_base %native_wave_lane_id
%native_wave_coord_x = OpUMod %uint %native_wave_logical_index %<size_x>
%native_wave_coord_y_div = OpUDiv %uint %native_wave_logical_index %<size_x>
%native_wave_coord_y = OpUMod %uint %native_wave_coord_y_div %<size_y>
%native_wave_coord_z = OpUDiv %uint %native_wave_logical_index %<size_xy>
%native_wave_in_group = OpULessThan %bool %native_wave_logical_index %<lanes>
)")
	                .ReplaceStr("<host_lane>", HostLaneVariable())
	                .ReplaceStr("<wave>", wave)
	                .ReplaceStr("<size_x>", size_x)
	                .ReplaceStr("<size_y>", size_y)
	                .ReplaceStr("<size_xy>", size_xy)
	                .ReplaceStr("<lanes>", lanes);
	if (m_cs_input_info->thread_ids_num >= 1) { AppendNativeComputeWaveCoordinate(output, "x", 0u); }
	if (m_cs_input_info->thread_ids_num >= 2) { AppendNativeComputeWaveCoordinate(output, "y", 1u); }
	if (m_cs_input_info->thread_ids_num >= 3) { AppendNativeComputeWaveCoordinate(output, "z", 2u); }
	if (UsesSplitComputeWave()) { *output += SplitWaveProlog(); }
	return true;
}

bool Spirv::UsesFragmentWaveTier() const
{
	if (m_code.GetType() != ShaderType::Pixel)
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

bool Spirv::UsesFragmentPartialWave() const
{
	return m_code.GetType() == ShaderType::Pixel && m_ps_input_info != nullptr &&
	       m_ps_input_info->native_wave.proof == ShaderNativeWaveProof::FragmentPartialWave &&
	       m_ps_input_info->native_wave.refusal_reason == nullptr;
}

const ShaderLaneReductions& Spirv::LaneReductions() const
{
	if (!m_lane_reductions_ready)
	{
		m_lane_reductions_ready = true;
		const auto* vertex = m_code.GetType() == ShaderType::Vertex ? m_vs_input_info : nullptr;
		if (UsesFragmentPartialWave())
		{
			m_lane_reductions = ShaderFindFragmentLaneReductions(
			    m_code, ShaderAnalyzeFragmentMaskFlow(m_code, m_ps_input_info->native_wave.guest_wave_size));
		} else if (vertex != nullptr && vertex->native_wave.proof == ShaderNativeWaveProof::LaneLocal && vertex->native_wave.refusal_reason == nullptr)
		{
			// The NGG front proof admitted the program with these wave maxima in it.
			m_lane_reductions = ShaderFindNggFrontMaxReductions(m_code, vertex->native_wave.guest_wave_size);
		}
	}
	return m_lane_reductions;
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
	return UsesDsAddtid() && m_code.GetType() == ShaderType::Compute && m_cs_input_info != nullptr && m_cs_input_info->lds_dwords > 0;
}


} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
