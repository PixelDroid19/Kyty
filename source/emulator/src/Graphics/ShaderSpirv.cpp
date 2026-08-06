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

String8 SpirvGetEmbeddedPs(uint32_t id)
{
	if (id != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: id != 0 condition ignored (continuing)\n"); }

	return EMBEDDED_SHADER_PS_0;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED