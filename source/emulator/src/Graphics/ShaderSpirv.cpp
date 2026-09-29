#include "Emulator/Graphics/ShaderSpirv.h"

#include "ShaderSpirvInternal.h"
#include "ShaderSpirvTemplates.h"

#include "Kyty/Core/Hashmap.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Core {

KYTY_HASH_DEFINE_CALC(Kyty::Libs::Graphics::ShaderInstructionTypeFormat)
{
	return hash32(static_cast<uint32_t>(key->type)) ^ hash64(static_cast<uint64_t>(key->format));
}

KYTY_HASH_DEFINE_EQUALS(Kyty::Libs::Graphics::ShaderInstructionTypeFormat)
{
	return key_a->type == key_b->type && key_a->format == key_b->format;
}
} // namespace Kyty::Core

namespace Kyty::Libs::Graphics {

static uint32_t ResolvePixelParameterCount(const ShaderCode& code, uint32_t register_count)
{
	uint32_t count = register_count;
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type != ShaderInstructionType::VInterpP1F32 && inst.type != ShaderInstructionType::VInterpP2F32 &&
		    inst.type != ShaderInstructionType::VInterpMovF32)
		{
			continue;
		}
		if (inst.src[1].type != ShaderOperandType::LiteralConstant && inst.src[1].type != ShaderOperandType::IntegerInlineConstant &&
		    inst.src[1].type != ShaderOperandType::FloatInlineConstant)
		{
			continue;
		}
		const uint32_t input = inst.src[1].constant.u;
		if (input < 32u && input + 1u > count)
		{
			count = input + 1u;
		}
	}
	return count;
}

String8 SpirvGenerateSource(const ShaderCode& code, const ShaderVertexInputInfo* vs_input_info, const ShaderPixelInputInfo* ps_input_info,
                            const ShaderComputeInputInfo* cs_input_info)
{
	ShaderPixelInputInfo resolved_ps_input {};
	if (ps_input_info != nullptr)
	{
		resolved_ps_input           = *ps_input_info;
		resolved_ps_input.input_num = ResolvePixelParameterCount(code, ps_input_info->input_num);
		ps_input_info               = &resolved_ps_input;
	}

	Spirv spirv;
	spirv.SetCode(code);
	spirv.SetVsInputInfo(vs_input_info);
	spirv.SetPsInputInfo(ps_input_info);
	spirv.SetCsInputInfo(cs_input_info);
	spirv.GenerateSource();

	return spirv.GetSource();
}

String8 SpirvGetEmbeddedVs(uint32_t id)
{
	if (id != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: id != 0 condition ignored (continuing)\n"); }

	return EMBEDDED_SHADER_VS_0;
}

static bool FragmentParameterRegisterAvailable(const ShaderBindResources& bind, uint32_t reg)
{
	if (reg == UINT32_MAX) { return true; }
	if (reg > 32u) { return false; }
	auto contains = [reg](int start, uint32_t width)
	{
		return start >= 0 && static_cast<uint32_t>(start) <= reg && reg - static_cast<uint32_t>(start) < width;
	};
	for (int index = 0; index < bind.storage_buffers.buffers_num; ++index)
	{
		if (!bind.storage_buffers.extended[index] && contains(bind.storage_buffers.start_register[index], 4u)) { return false; }
	}
	for (int index = 0; index < bind.textures2D.textures_num; ++index)
	{
		const auto& texture = bind.textures2D.desc[index];
		if (!texture.extended && contains(texture.start_register, 8u)) { return false; }
	}
	for (int index = 0; index < bind.samplers.samplers_num; ++index)
	{
		if (!bind.samplers.extended[index] && contains(bind.samplers.start_register[index], 4u)) { return false; }
	}
	for (int index = 0; index < bind.direct_sgprs.sgprs_num; ++index)
	{
		if (contains(bind.direct_sgprs.start_register[index], 1u)) { return false; }
	}
	for (int index = 0; index < bind.gds_pointers.pointers_num; ++index)
	{
		if (!bind.gds_pointers.extended[index] && contains(bind.gds_pointers.start_register[index], 1u)) { return false; }
	}
	return !bind.extended.used || !contains(bind.extended.start_register, 2u);
}

String8 SpirvGenerateFragmentComputeSource(const ShaderCode& code, const ShaderPixelInputInfo& ps_input_info,
                                            const ShaderFragmentComputeInfo& transport)
{
	if (code.GetType() != ShaderType::Pixel || transport.initial_vgpr_count > 256u ||
	    transport.input_binding == transport.output_binding || transport.descriptor_set == ps_input_info.bind.descriptor_set_slot ||
	    ps_input_info.input_num > 32u || ps_input_info.fragment_tap.enabled || ps_input_info.input0_probe.enabled)
	{
		return "OpKytyFragmentTransportRejected\n";
	}
	if (!FragmentParameterRegisterAvailable(ps_input_info.bind, transport.user_sgpr_count))
	{
		return "OpKytyFragmentParameterRegisterRejected\n";
	}
	for (uint32_t field = 0; field < 16u; ++field)
	{
		if ((ps_input_info.system_input_enable & (1u << field)) == 0u) { continue; }
		const uint32_t width = field == 3u ? 3u : (field < 7u ? 2u : 1u);
		if ((ps_input_info.system_input_address & (1u << field)) == 0u ||
		    ShaderPixelSystemInputRegister(ps_input_info, field) + width > transport.initial_vgpr_count)
		{
			return "OpKytyFragmentSystemInputRejected\n";
		}
	}
	ShaderPixelInputInfo pixel = ps_input_info;
	pixel.input_num = ResolvePixelParameterCount(code, pixel.input_num);
	ShaderComputeInputInfo host {};
	host.threads_num[0] = 64u;
	host.threads_num[1] = host.threads_num[2] = 1u;
	host.wave_layout = {ShaderComputeWaveStrategy::Paired64On32, {64u, 1u, 1u}, {32u, 1u, 1u}, 64u, 32u, 2u, 1u, 0u};
	host.bind = pixel.bind;
	Spirv spirv;
	spirv.SetCode(code);
	spirv.SetPsInputInfo(&pixel);
	spirv.SetCsInputInfo(&host);
	spirv.SetFragmentComputeInfo(&transport);
	spirv.GenerateSource();
	return spirv.GetSource();
}

String8 SpirvGetEmbeddedPs(uint32_t id)
{
	if (id != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: id != 0 condition ignored (continuing)\n"); }

	return EMBEDDED_SHADER_PS_0;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
