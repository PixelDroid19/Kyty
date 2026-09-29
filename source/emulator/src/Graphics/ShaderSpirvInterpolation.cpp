#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

void Spirv::WriteCustomPixelInterface(Core::StringList8* variables) const
{
	if (!m_ps_input_info->custom_interpolation.Enabled()) { return; }
	for (uint32_t field: {1u, 2u, 5u, 6u})
	{
		if ((m_ps_input_info->system_input_enable & (1u << field)) != 0)
		{
			variables->Add(String8::FromPrintf("%%custom_bary%u", field));
		}
	}
}

void Spirv::WriteCustomPixelAnnotations(Core::StringList8* annotations) const
{
	if (!m_ps_input_info->custom_interpolation.Enabled()) { return; }
	for (uint32_t field: {1u, 2u, 5u, 6u})
	{
		if ((m_ps_input_info->system_input_enable & (1u << field)) == 0) { continue; }
		annotations->Add(String8::FromPrintf("OpDecorate %%custom_bary%u Location %u", field,
		                                   m_ps_input_info->custom_interpolation.barycentric_locations[field]));
		if (field >= 5u) { annotations->Add(String8::FromPrintf("OpDecorate %%custom_bary%u NoPerspective", field)); }
		if (field == 2u || field == 6u) { annotations->Add(String8::FromPrintf("OpDecorate %%custom_bary%u Centroid", field)); }
	}
}

void Spirv::WriteCustomPixelVariables(Core::StringList8* variables) const
{
	if (!m_ps_input_info->custom_interpolation.Enabled()) { return; }
	for (uint32_t field: {1u, 2u, 5u, 6u})
	{
		if ((m_ps_input_info->system_input_enable & (1u << field)) != 0)
		{
			variables->Add(String8::FromPrintf("%%custom_bary%u = OpVariable %%_ptr_Input_v2float Input", field));
		}
	}
}

void Spirv::WriteCustomPixelProlog()
{
	if (m_ps_input_info == nullptr || !m_ps_input_info->custom_interpolation.Enabled()) { return; }
	for (uint32_t field: {1u, 2u, 5u, 6u})
	{
		if ((m_ps_input_info->system_input_enable & (1u << field)) == 0) { continue; }
		const auto first = ShaderPixelSystemInputRegister(*m_ps_input_info, field);
		m_source += String8::FromPrintf("%%custom_bary_value%u = OpLoad %%v2float %%custom_bary%u\n", field, field);
		for (uint32_t component = 0; component < 2u; ++component)
		{
			m_source += String8::FromPrintf("%%custom_bary%u_%u = OpCompositeExtract %%float %%custom_bary_value%u %u\n"
			                                "OpStore %%v%u %%custom_bary%u_%u\n",
			                                field, component, field, component, first + component, field, component);
		}
	}
	for (uint32_t field = 8u; field < 12u; ++field)
	{
		if ((m_ps_input_info->system_input_enable & (1u << field)) == 0) { continue; }
		const auto reg = ShaderPixelSystemInputRegister(*m_ps_input_info, field);
		const auto component = field - 8u;
		m_source += String8::FromPrintf("%%custom_pos_ptr%u = OpAccessChain %%_ptr_Input_float %%gl_FragCoord %%uint_%u\n"
		                                "%%custom_pos%u = OpLoad %%float %%custom_pos_ptr%u\n", field, component, field, field);
		const auto& scale = m_ps_input_info->host_to_guest_scale;
		if (component >= 2u || scale.IsIdentity())
		{
			m_source += String8::FromPrintf("OpStore %%v%u %%custom_pos%u\n", reg, field);
			continue;
		}
		const auto numerator = GetConstantFloat(static_cast<float>(component == 0 ? scale.x_guest_numerator : scale.y_guest_numerator));
		const auto denominator = GetConstantFloat(static_cast<float>(component == 0 ? scale.x_host_denominator : scale.y_host_denominator));
		m_source += String8::FromPrintf("%%custom_pos_scale%u = OpFDiv %%float %%%s %%%s\n"
		                                "%%custom_pos_guest%u = OpFMul %%float %%custom_pos%u %%custom_pos_scale%u\n"
		                                "OpStore %%v%u %%custom_pos_guest%u\n",
		                                field, numerator.c_str(), denominator.c_str(), field, field, field, reg, field);
	}
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
