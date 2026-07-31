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

String8 SpirvGenerateSource(const ShaderCode& code, const ShaderVertexInputInfo* vs_input_info, const ShaderPixelInputInfo* ps_input_info,
                            const ShaderComputeInputInfo* cs_input_info)
{
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
	EXIT_NOT_IMPLEMENTED(id != 0);

	return EMBEDDED_SHADER_VS_0;
}

String8 SpirvGetEmbeddedPs(uint32_t id)
{
	EXIT_NOT_IMPLEMENTED(id != 0);

	return EMBEDDED_SHADER_PS_0;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
